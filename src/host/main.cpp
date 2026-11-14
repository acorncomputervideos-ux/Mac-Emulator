// Headless host build of the emulator.  It runs the same core and the same
// menu code as the Raspberry Pi kernel, but takes its ROM and disks from
// ordinary files and writes screenshots (PPM) and sound (WAV) instead of
// driving real hardware.  Useful for testing a ROM/disk combination on a PC
// and for developing the core.
//
//   macemu-host ROM [--machine plus|ii] [--ram KB] [--floppy1 IMG] [--floppy2 IMG] [--hdN IMG]
//               [--frames N] [--shot-every K] [--out PREFIX] [--wav FILE]
//               [--menu] [--palette N] [--scale N] [--type TEXT] [--click X,Y]

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include "../core/mac.h"
#include "../core/macii.h"
#include "../core/quadra.h"
#include "../core/jit.h"
#include "../core/state.h"
#include "hostfs_posix.h"
#include "sharetest.h"
#include "enettest.h"
#include "../core/keys.h"
#include "../frontend/config.h"
#include "../frontend/osd.h"
#include "../frontend/romid.h"
#include "natposix.h"
#include "../frontend/httpsproxy.h"
#include "../frontend/util.h"
#include "../frontend/video.h"

using namespace macemu;

static Jit *gJit = 0;

// A save state in a file.
struct FileState : StateIO {
    FILE *f;
    FileState(FILE *file, bool save) : StateIO(save), f(file) {}
    void raw(void *p, u32 n) override
    {
        if (!ok) return;
        if (saving() ? fwrite(p, 1, n, f) != n : fread(p, 1, n, f) != n) ok = false;
    }
};

#ifdef HAVE_CIRCLE_FONT
struct TFont { unsigned width, height, extra_height, first_char, last_char; const void *data; };
extern const TFont Font8x16;
struct HostFont : Font {
    int width() const override { return 8; }
    int height() const override { return (int)(Font8x16.height + Font8x16.extra_height); }
    bool pixel(char c, int x, int y) const override {
        unsigned a = (unsigned char)c;
        if (a < Font8x16.first_char || a > Font8x16.last_char || y >= (int)Font8x16.height) return false;
        return (((const unsigned char *)Font8x16.data)[(a - Font8x16.first_char) * Font8x16.height + y] >> (7 - x)) & 1;
    }
};
#else
struct HostFont : Font {                                // placeholder: solid blocks
    int width() const override { return 8; }
    int height() const override { return 16; }
    bool pixel(char c, int x, int y) const override { return c != ' ' && x > 0 && x < 7 && y > 3 && y < 13; }
};
#endif

struct FileStorage : Storage {
    std::string path;
    std::vector<u8> data;
    bool dirty = false, ro = false;
    bool load(const std::string &p) {
        path = p;
        FILE *f = fopen(p.c_str(), "rb");
        if (!f) return false;
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        data.resize((size_t)n);
        bool ok = fread(data.data(), 1, data.size(), f) == data.size();
        fclose(f);
        FILE *w = fopen(p.c_str(), "r+b");
        if (w) fclose(w); else ro = true;
        return ok;
    }
    u32 size() override { return (u32)data.size(); }
    bool read(u32 off, void *buf, u32 len) override { if ((size_t)off + len > data.size()) return false; memcpy(buf, &data[off], len); return true; }
    bool write(u32 off, const void *buf, u32 len) override { if (ro || (size_t)off + len > data.size()) return false; memcpy(&data[off], buf, len); dirty = true; return true; }
    bool readOnly() override { return ro; }
    void flush() override {
        if (!dirty) return;
        FILE *f = fopen(path.c_str(), "r+b");
        if (f) { fwrite(data.data(), 1, data.size(), f); fclose(f); dirty = false; }
    }
};

struct Host : OsdHost {
    Emulator &mac;
    Config &cfg;
    FileStorage *floppy[2] = {nullptr, nullptr};
    std::string floppyName[2];
    Host(Emulator &m, Config &c) : mac(m), cfg(c) {}
    int listFiles(FileKind kind, char (*names)[kNameLen], int max) override {
        static const char *floppies[] = {"System 6.0.8.dsk", "MacPaint.dsk", "Utilities.img"};
        static const char *hds[] = {"hd0.img", "disks/Games.hda"};
        static const char *roms[] = {"Quadra-650.ROM", "MacPlus.rom"};
        const char **list = kind == Floppies ? floppies : kind == HardDisks ? hds : roms;
        int count = kind == Floppies ? 3 : 2, n = 0;
        for (int i = 0; i < count && n < max; i++) strCopy(names[n++], list[i], kNameLen);
        return n;
    }
    bool insertCd(const char *) override { return false; }
    const char *cdName() override { return ""; }
    bool insertDisk(int drive, const char *name) override {
        FileStorage *s = new FileStorage;
        if (!s->load(name) || !mac.floppy().insert(drive, s)) { delete s; return false; }
        floppy[drive] = s;
        floppyName[drive] = name;
        return true;
    }
    void ejectDisk(int drive) override {
        if (mac.floppy().eject(drive)) { delete floppy[drive]; floppy[drive] = nullptr; floppyName[drive].clear(); }
    }
    const char *diskName(int drive) override { return floppyName[drive].c_str(); }
    void resetMachine() override { mac.reset(); }
    void interruptButton() override { mac.interruptButton(); }
    void settingsChanged() override {}
    bool saveSettings() override { return true; }
    void powerOff() override {}
    bool hibernate() override { return false; }
    void saveAndReboot() override {}
    const char *networkLine(int n) override { return n == 0 ? "Web file manager: http://192.168.1.20/" : n == 1 ? "Open it from a browser on your network" : ""; }
    bool screenshot() override { return true; }
    bool newFloppy(bool, char *, unsigned) override { return false; }
    const char *slotInfo(int n) override { return n == 1 ? "13 Nov 2026 16:20  hd0.img" : ""; }
    bool saveSlot(int) override { return false; }
    bool loadSlot(int, char *why, unsigned max) override { strCopy(why, "Not in this runner", max); return false; }
    const char *infoLine(int n) override {
        return n == 0 ? mac.modelName() : n == 1 ? "Shared disk: on   Web: http://192.168.1.20/" : "Built today";
    }
};

static EnetTest *gEnetTest = nullptr;
static u32 gWatchPc = 0;
static M68K *gWatchCpu = nullptr;
static int gWatchHits = 0;
static const char *(*gWatchEsp)() = nullptr;
static int ioLogCount = 0;
static void logIo(void *, const char *what, u32 addr, u32 value)
{
    if (ioLogCount++ < 200) printf("  %s %08X (%08X)\n", what, addr, value);
}

// Called from the Quadra's bus: report CPU state when it reaches gWatchPc.
static void watchCheck()
{
    if (!gWatchCpu || gWatchCpu->instPC != gWatchPc || gWatchHits >= 100000) return;
    M68K &c = *gWatchCpu;
    gWatchHits++;
    printf("watch %08X: d0=%08X d1=%08X d2=%08X d5=%08X a0=%08X a1=%08X a2=%08X a3=%08X a4=%08X sr=%04X\n", c.instPC,
           c.d[0], c.d[1], c.d[2], c.d[5], c.a[0], c.a[1], c.a[2], c.a[3], c.a[4], c.getSR());
    printf("      return %08X %08X %08X\n", c.rd(c.a[7], 2), c.rd(c.a[7] + 4, 2), c.rd(c.a[7] + 8, 2));
    if (gWatchEsp) printf("      %s\n", gWatchEsp());
    if (const char *dump = getenv("WATCH_DUMP")) {      // code around the watchpoint, for objdump
        if (FILE *f = fopen(dump, "wb")) {
            u32 from = c.instPC - 128, len = 4096;
            if (const char *r = getenv("WATCH_DUMP_RANGE")) sscanf(r, "%x:%x", &from, &len);   // hex start:length
            for (u32 i = 0; i < len; i++) fputc((int)c.rd(from + i, 0), f);
            fclose(f);
        }
    }
}

static void writePpm(const std::string &path, const Surface &s)
{
    FILE *f = fopen(path.c_str(), "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", s.width, s.height);
    for (int y = 0; y < s.height; y++)
        for (int x = 0; x < s.width; x++) {
            u32 c = s.pixels[y * s.pitch + x];
            fputc((int)(c >> 16), f); fputc((int)(c >> 8) & 255, f); fputc((int)c & 255, f);
        }
    fclose(f);
}

static int asciiToKey(char c, bool &shift)
{
    static const char plain[] = "asdfhgzxcv\0bqweryt123465=97-80]ou[ip\rlj'k;\\,/nm.\t `";
    static const char shifted[] = "ASDFHGZXCV\0BQWERYT!@#$^%+(&_*)}OU{IP\rLJ\"K:|<?NM>\t ~";
    for (int i = 0; i < 0x33; i++) {
        if (plain[i] && plain[i] == c) { shift = false; return i; }
        if (shifted[i] && shifted[i] == c && shifted[i] != plain[i]) { shift = true; return i; }
    }
    return -1;
}

// ETHER_UDP=port:peer:peerport carries the Mac's Ethernet frames in UDP
// datagrams to one peer (another host runner, say); ETHER_LOG=1 lists them.
struct UdpEther : EtherLink {
    int fd = -1, port = 6066;
    PosixNatSockets natSockets;
    HttpsProxy *proxy = nullptr;        // the HTTPS helper, between the router and the sockets
    NatRouter *router = nullptr;        // ETHER_NAT=1: internet for the Mac
    sockaddr_in peer = {};
    bool log = false;
    bool open(const char *spec) {
        char host[64] = {};
        int peerPort = 0;
        if (sscanf(spec, "%d:%63[^:]:%d", &port, host, &peerPort) != 3) return false;
        fd = socket(AF_INET, SOCK_DGRAM, 0);
        int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        sockaddr_in me = {};
        me.sin_family = AF_INET; me.sin_port = htons((u16)port); me.sin_addr.s_addr = htonl(INADDR_ANY);
        if (bind(fd, (sockaddr *)&me, sizeof me) < 0) return false;
        fcntl(fd, F_SETFL, O_NONBLOCK);
        peer.sin_family = AF_INET; peer.sin_port = htons((u16)peerPort);
        inet_pton(AF_INET, host, &peer.sin_addr);
        log = getenv("ETHER_LOG") != nullptr;
        if (log) setvbuf(stdout, nullptr, _IOLBF, 0);
        return true;
    }
    static void show(const char *dir, const u8 *f, u32 n) {
        printf("ether %s %4u  %02X:%02X:%02X:%02X:%02X:%02X <- %02X:%02X:%02X:%02X:%02X:%02X  type %04X",
               dir, n, f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7], f[8], f[9], f[10], f[11], be16(f + 12));
        if (be16(f + 12) <= 1500 && n >= 22)   // 802.2 SNAP (AppleTalk Phase 2)
            printf("  snap %02X%02X%02X %04X", f[17], f[18], f[19], be16(f + 20));
        printf("\n");
    }
    void etherAddress(u8 mac[6]) override {
        u8 a[6] = {0x08, 0x00, 0x07, 0x5A, (u8)(port >> 8), (u8)port};
        if (const char *m = getenv("ETHER_MAC")) {     // ETHER_MAC=42:32:C0:A8:00:0E (as the Pi's "B2" + its IP)
            unsigned v[6];
            if (sscanf(m, "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) == 6)
                for (int i = 0; i < 6; i++) a[i] = (u8)v[i];
        }
        memcpy(mac, a, 6);
    }
    void etherSend(const u8 *f, u32 n) override {
        if (log) show("out", f, n);
        if (router && router->fromMac(f, n)) return;
        if (fd >= 0) sendto(fd, f, n, 0, (sockaddr *)&peer, sizeof peer);
    }
    u32 etherReceive(u8 *f) override {
        if (router) {
            router->poll();
            if (u32 n = router->toMac(f)) { if (log) show("in ", f, n); return n; }
        }
        if (fd < 0) return 0;
        ssize_t n = recv(fd, f, 1514, 0);
        if (n < 14) return 0;
        if (log) show("in ", f, (u32)n);
        return (u32)n;
    }
};

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s ROM [--ram KB] [--floppy1 IMG] [--floppy2 IMG] [--hdN IMG] [--frames N]\n"
                        "       [--shot-every K] [--out PREFIX] [--wav FILE] [--menu] [--palette N] [--scale N]\n"
                        "       [--type TEXT] [--click X,Y]\n", argv[0]);
        return 2;
    }
    Config cfg;
    cfg.setDefaults();
    std::string out = "macemu", wavPath, typeText;
    int frames = 600, shotEvery = 0, clickX = -1, clickY = -1;
    bool menu = false, traceIo = false;
    int tracePc = 0;
    u32 watchPc = 0;
    std::string floppy[2], hd[7], cd, saveState, loadState, shareDir;
    for (int i = 2; i < argc; i++) {
        std::string a = argv[i];
        const char *v = i + 1 < argc ? argv[i + 1] : "";
        if (a == "--ram") { cfg.ramKB = (u32)atoi(v); i++; }
        else if (a == "--machine") { strCopy(cfg.machine, v, sizeof cfg.machine); i++; }
        else if (a == "--floppy1") { floppy[0] = v; i++; }
        else if (a == "--floppy2") { floppy[1] = v; i++; }
        else if (a == "--cd") { cd = v; i++; }
        else if (a == "--save-state") { saveState = v; i++; }   // the Quadra, after the frames
        else if (a == "--share") { shareDir = v; i++; }         // a host directory as the Mac's shared folder
        else if (a == "--load-state") { loadState = v; i++; }   // ... and before them
        else if (a.size() == 5 && a.compare(0, 4, "--hd") == 0 && a[4] >= '0' && a[4] <= '6') { hd[a[4] - '0'] = v; i++; }
        else if (a == "--frames") { frames = atoi(v); i++; }
        else if (a == "--shot-every") { shotEvery = atoi(v); i++; }
        else if (a == "--out") { out = v; i++; }
        else if (a == "--wav") { wavPath = v; i++; }
        else if (a == "--palette") { cfg.palette = atoi(v); i++; }
        else if (a == "--scale") { cfg.scale = atoi(v); i++; }
        else if (a == "--type") { typeText = v; i++; }
        else if (a == "--click") { sscanf(v, "%d,%d", &clickX, &clickY); i++; }
        else if (a == "--menu") menu = true;
        else if (a == "--trace-io") traceIo = true;
        else if (a == "--trace-pc") { tracePc = atoi(v); i++; }
        else if (a == "--watch-pc") { watchPc = (u32)strtoul(v, nullptr, 16); i++; }
        else { fprintf(stderr, "unknown option %s\n", a.c_str()); return 2; }
    }
    cfg.sanitize();

    FileStorage romFile;
    if (!romFile.load(argv[1])) { fprintf(stderr, "cannot read ROM %s\n", argv[1]); return 1; }
    if (u32 n = romImageSize(romFile.data.data(), (u32)romFile.data.size())) romFile.data.resize(n);
    std::vector<u8> ram(cfg.ramKB * 1024);
    const RomInfo &romInfo = romIdentify(romFile.data.data(), (u32)romFile.data.size());
    bool autoPick = strEqNoCase(cfg.machine, "auto");
    bool compact = autoPick && (romInfo.family == RomCompact || romInfo.family == RomSE);
    bool quadra = strEqNoCase(cfg.machine, "quadra") || (autoPick && !compact && romFile.data.size() == 0x100000);
    bool macII = !quadra && !compact && (strEqNoCase(cfg.machine, "ii") || (!strEqNoCase(cfg.machine, "plus") && romFile.data.size() >= 0x40000));
    printf("ROM: %s\n", romInfo.name);
    if (!macII && !quadra && ram.size() > 0x400000) ram.resize(0x400000);
    static Machine plus;
    static MacII *two = nullptr;
    Emulator *emu;
    bool ok;
    static Quadra *quad = nullptr;
    if (quadra) {
        if (ram.size() < 0x800000) ram.resize(0x800000);
        quad = new Quadra(getenv("QUADRA_21") ? Dafb::Apple21 : Dafb::VGA);
        ok = quad->init(romFile.data.data(), (u32)romFile.data.size(), ram.data(), (u32)ram.size());
        if (getenv("MACEMU_NOFAST")) quad->cpu.fastDispatch = false;
        // JIT=1: the JIT (on an AArch64 host), JIT_STATS=1: what it did.
        if (getenv("JIT") && atoi(getenv("JIT"))) {
            gJit = new Jit;
            if (getenv("JIT_NATIVE") && !atoi(getenv("JIT_NATIVE"))) gJit->native = false;
            if (gJit->init()) quad->cpu.jit = gJit;
            else { fprintf(stderr, "no JIT on this host\n"); delete gJit; gJit = 0; }
        }
        if (getenv("SWIM_TRACE")) {
            static Quadra *tq = quad;
            quad->floppy().trace = [](void *, int line, bool write, u8 v) {
                static unsigned n = 0;
                if (n++ < 400000) printf("swim %s %2d %02X pc=%08X\n", write ? "W" : "R", line, v, tq->cpu.instPC);
            };
        }
        if (traceIo) { quad->ioLog = logIo; }
        if (watchPc) {
            gWatchPc = watchPc; gWatchCpu = &quad->cpu; quad->stepHook = watchCheck;
            static Quadra *q = quad;
            gWatchEsp = [] { static char b[200]; q->esp.describe(b); return (const char *)b; };
        }
        emu = quad;
    } else if (macII) {
        two = new MacII(cfg.videoWidth, cfg.videoHeight);
        ok = two->init(romFile.data.data(), (u32)romFile.data.size(), ram.data(), (u32)ram.size());
        emu = two;
    } else {
        ok = plus.init(romFile.data.data(), (u32)romFile.data.size(), ram.data(), (u32)ram.size());
        emu = &plus;
    }
    if (!ok) {
        fprintf(stderr, "unsupported ROM size (%zu bytes) or RAM size\n", romFile.data.size());
        return 1;
    }
    Emulator &mac = *emu;
    static UdpEther ether;
    if (getenv("ETHER_NAT")) {
        // HTTPS_ROOTS: root certificates for the HTTPS helper (sdcard/certs/cacert.pem).
        ether.proxy = new HttpsProxy(ether.natSockets);
        const char *roots = getenv("HTTPS_ROOTS") ? getenv("HTTPS_ROOTS") : "sdcard/certs/cacert.pem";
        for (const char *path = roots; *path; ) {       // several files, separated by ':'
            std::string one(path, strcspn(path, ":"));
            path += one.size() + (path[one.size()] == ':');
            FileStorage f;
            if (f.load(one)) printf("HTTPS helper: %u root certificates from %s\n",
                                    ether.proxy->loadRoots((const char *)f.data.data(), (u32)f.data.size()), one.c_str());
        }
        ether.router = new NatRouter(*ether.proxy);
    }
    if (getenv("ETHER_LOG")) { ether.log = true; setvbuf(stdout, nullptr, _IOLBF, 0); }
    if (const char *spec = getenv("ETHER_UDP")) {
        if (!ether.open(spec)) fprintf(stderr, "no Ethernet tunnel (%s)\n", spec);
    }
    if ((getenv("ETHER_UDP") || ether.router) && !mac.attachEthernet(&ether)) fprintf(stderr, "this Mac has no Ethernet\n");
    printf("%s, %uK RAM\n", mac.modelName(), (unsigned)(ram.size() / 1024));
    u32 sum = 0;
    for (size_t i = 4; i + 1 < romFile.data.size(); i += 2) sum += (u32)((romFile.data[i] << 8) | romFile.data[i + 1]);
    printf("ROM %zuK, checksum %08X (computed %08X)\n", romFile.data.size() / 1024, be32(romFile.data.data()), sum);

    time_t now = getenv("MACEMU_TIME") ? (time_t)atoll(getenv("MACEMU_TIME")) : time(nullptr);   // fixed clock for repeatable runs
    struct tm *tm = gmtime(&now);
    mac.clockChip().seconds = (macTime(tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday, tm->tm_hour, tm->tm_min, tm->tm_sec));

    // PRAM_FILE=path: parameter RAM loaded at start (if the file exists) and saved at the end.
    const char *pramFile = getenv("PRAM_FILE");
    if (pramFile) {
        if (FILE *pf = fopen(pramFile, "rb")) {
            if (fread(mac.clockChip().pram, 1, 256, pf) != 256) fprintf(stderr, "short PRAM file\n");
            fclose(pf);
        }
    }
    Host host(mac, cfg);
    for (int d = 0; d < 2; d++)
        if (!floppy[d].empty() && !host.insertDisk(d, floppy[d].c_str()))
            fprintf(stderr, "cannot use floppy image %s (need a 400K/800K raw or DiskCopy 4.2 image)\n", floppy[d].c_str());
    for (int id = 0; id < 7; id++) {
        if (hd[id].empty()) continue;
        FileStorage *s = new FileStorage;
        if (s->load(hd[id])) mac.attachDisk(id, s);
        else fprintf(stderr, "cannot read hard disk image %s\n", hd[id].c_str());
    }
    // CD_LATE=frame: put the CD in at that frame, as the web page's transfer
    // CDs are, rather than at power-on (the drive is empty until then).
    int cdLate = getenv("CD_LATE") ? atoi(getenv("CD_LATE")) : -1;
    FileStorage *cdDisc = 0;
    if (!cd.empty()) {                                  // CD-ROM drive at SCSI ID 3
        cdDisc = new FileStorage;
        if (cdDisc->load(cd)) { cdDisc->ro = true; mac.insertCdrom(3, cdLate >= 0 ? 0 : cdDisc); }
        else fprintf(stderr, "cannot read CD image %s\n", cd.c_str());
    }

    Surface surf;
    surf.width = 1280; surf.height = 720; surf.pitch = 1280;
    std::vector<u32> pixels((size_t)surf.pitch * surf.height, 0);
    surf.pixels = pixels.data();
    surf.swapRB = false;
    setPalette(surf, cfg.palette);
    Layout layout = layoutFor(surf, 512, 342, cfg.scale);
    std::vector<u8> prev(kPreviousFrameBytes);
    int shownW = 0, shownH = 0;
    u32 shownClut = 0;
    auto draw = [&](bool force) {
        VideoFrame vf;
        if (!mac.video(vf)) return;
        if (vf.width != shownW || vf.height != shownH) {
            shownW = vf.width; shownH = vf.height;
            layout = layoutFor(surf, vf.width, vf.height, cfg.scale);
            std::fill(pixels.begin(), pixels.end(), 0);
            force = true;
        }
        if (vf.clutVersion != shownClut) { shownClut = vf.clutVersion; force = true; }
        blitScreen(surf, layout, vf, prev.data(), force);
    };
    HostFont font;
    Osd osd(cfg, host);

    std::vector<s16> wav;
    size_t typed = 0;
    int typeState = 0;
    bool shiftHeld = false;
    // FLOPPY_LATE=frame:image inserts a floppy into drive 1 while running
    // (a non-startup disk present at power-on is ejected by the ROM).
    int lateFrame = -1;
    std::string lateImage;
    if (const char *late = getenv("FLOPPY_LATE")) {
        lateFrame = atoi(late);
        if (const char *colon = strchr(late, ':')) lateImage = colon + 1;
    }
    // HOLD_KEYS=k,k,...:frames holds ADB keys (hex) down from power-on, for
    // start-up key combinations (the Classic's ROM disk: 37,3A,07,1F:600).
    std::vector<int> heldKeys;
    int holdFrames = 0;
    if (const char *hk = getenv("HOLD_KEYS")) {
        const char *p = hk;
        while (*p && *p != ':') { heldKeys.push_back((int)strtol(p, (char **)&p, 16)); if (*p == ',') p++; }
        if (*p == ':') holdFrames = atoi(p + 1);
    }
    PosixHostFS *shareFs = 0;
    SharedFolder *share = 0;
    if (!shareDir.empty() && quad) {
        shareFs = new PosixHostFS(shareDir);
        share = new SharedFolder(quad->cpu, *shareFs);
        quad->attachSharedFolder(share);
        if (getenv("ENET_TEST")) {
            EnetTest *t = new EnetTest(*share, quad->cpu);
            gEnetTest = t;
            if (getenv("ENET_AT")) t->armed = false;
            share->onIdle = &EnetTest::idle;
            share->idleCtx = t;
        }
        if (getenv("SHARE_TEST")) {
            ShareTest *t = new ShareTest(*share, quad->cpu, shareDir);
            share->onIdle = &ShareTest::idle;
            share->idleCtx = t;
        }
    }
    if (!loadState.empty()) {
        FILE *sf = fopen(loadState.c_str(), "rb");
        FileState st(sf, false);
        if (sf && quad) quad->state(st);
        if (!sf || !quad || !st.ok) { fprintf(stderr, "cannot load the state %s\n", loadState.c_str()); return 1; }
        fclose(sf);
    }
    for (int f = 0; f < frames; f++) {
        if (f == (getenv("HOLD_FROM") ? atoi(getenv("HOLD_FROM")) : 1)) for (int k : heldKeys) mac.keyEvent(k, true);
        if (f == holdFrames) for (int k : heldKeys) mac.keyEvent(k, false);
        if (gEnetTest && getenv("ENET_AT") && f == atoi(getenv("ENET_AT"))) gEnetTest->armed = true;
        if (f == cdLate && cdDisc) mac.insertCdrom(3, cdDisc);
        if (f == lateFrame && !host.insertDisk(0, lateImage.c_str()))
            fprintf(stderr, "cannot insert %s\n", lateImage.c_str());
        mac.runFrame();
        if (getenv("DISK_LOG")) {                       // frames with disk activity
            static u32 lastAct = 0;
            u32 act = mac.diskActivity();
            if (act != lastAct && f >= atoi(getenv("DISK_LOG"))) printf("frame %d: %u disk operations\n", f, act - lastAct);
            lastAct = act;
        }
        if (quad && getenv("RESTARTABLE")) quad->cpu.restartable = atoi(getenv("RESTARTABLE")) != 0;   // 0/1: override the VM detection
        if (getenv("REALTIME")) {                       // keep to 60.15 frames a second
            static timespec start;
            if (f == 0) clock_gettime(CLOCK_MONOTONIC, &start);
            timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            double ahead = (f + 1) / 60.15 - ((now.tv_sec - start.tv_sec) + (now.tv_nsec - start.tv_nsec) * 1e-9);
            if (ahead > 0) usleep((useconds_t)(ahead * 1e6));
        }
        if (quad && getenv("IO_FROM") && f == atoi(getenv("IO_FROM"))) { ioLogCount = 0; quad->ioLog = logIo; quad->logAllIo = true; }
        if (quad && getenv("IO_FROM") && f == atoi(getenv("IO_FROM")) + 1) { quad->logAllIo = false; quad->ioLog = nullptr; }
        if (tracePc && f % tracePc == 0 && quad) {
            printf("frame %d: pc=%08X sr=%04X tc=%04X a7=%08X disk activity %u\n", f, quad->cpu.pc, quad->cpu.getSR(), quad->mmu.tc, quad->cpu.a[7], mac.diskActivity());
        }
        if (!wavPath.empty()) { u32 n, rate; const s16 *snd = mac.sound(n, rate); wav.insert(wav.end(), snd, snd + n); }

        // Scripted input, starting once the machine has had time to boot.
        static int typeAt = getenv("TYPE_AT") ? atoi(getenv("TYPE_AT")) : -1;
        // CMD_KEY=frame:k presses Command-k at that frame (e.g. 4800:s to save).
        for (const char *ck = getenv("CMD_KEY"); ck && *ck; ) {   // "frame:k,frame:k,..."
            int at = atoi(ck);
            const char *colon = strchr(ck, ':');
            if (!colon) break;
            bool sh = false;
            int key = asciiToKey(colon[1], sh);
            if (key >= 0 && f == at) { mac.keyEvent(MK_Command, true); mac.keyEvent(key, true); }
            if (key >= 0 && f == at + 3) { mac.keyEvent(key, false); mac.keyEvent(MK_Command, false); }
            const char *comma = strchr(ck, ',');
            if (!comma) break;
            ck = comma + 1;
        }
        if (f >= (typeAt >= 0 ? typeAt : frames / 2) && f % 4 == 0 && typed < typeText.size()) {
            bool shift = false;
            int key = asciiToKey(typeText[typed], shift);
            if (key < 0) typed++;
            else if (typeState == 0) {
                if (shift != shiftHeld) { mac.keyEvent(MK_Shift, shift); shiftHeld = shift; }
                mac.keyEvent(key, true); typeState = 1;
            } else { mac.keyEvent(key, false); typeState = 0; typed++; }
        }
        // SHARE_ADD=frame:name puts a file in the shared folder then (as the web page would).
        if (const char *sa = getenv("SHARE_ADD")) {
            if (f == atoi(sa) && strchr(sa, ':') && !shareDir.empty()) {
                FILE *nf = fopen((shareDir + "/" + (strchr(sa, ':') + 1)).c_str(), "wb");
                if (nf) { fputs("Added while the Mac was running\n", nf); fclose(nf); }
            }
        }
        // TYPE2=text from frame TYPE2_AT: a second bit of typing (say, once a window is open).
        if (const char *t2 = getenv("TYPE2")) {
            static size_t typed2 = 0;
            static int state2 = 0;
            int at2 = getenv("TYPE2_AT") ? atoi(getenv("TYPE2_AT")) : frames - 200;
            if (f >= at2 && f % 4 == 0 && typed2 < strlen(t2)) {
                bool sh = false;
                int key = asciiToKey(t2[typed2], sh);
                if (key < 0) typed2++;
                else if (state2 == 0) { if (sh) mac.keyEvent(MK_Shift, true); mac.keyEvent(key, true); state2 = 1; }
                else { mac.keyEvent(key, false); if (sh) mac.keyEvent(MK_Shift, false); state2 = 0; typed2++; }
            }
        }
        // MOUSE_SCRIPT="F:park;F:to X,Y;F:down;F:up;..." - from frame F park the
        // cursor top-left, walk to (X, Y) (1 pixel a frame: no acceleration), press, release.
        if (const char *script = getenv("MOUSE_SCRIPT")) {
            static int curX = 0, curY = 0, toX = -1, toY = -1;
            for (const char *p = script; *p; ) {
                int at = atoi(p);
                const char *c = strchr(p, ':');
                if (!c) break;
                c++;
                if (at == f) {
                    if (!strncmp(c, "park", 4)) { if (getenv("MOUSE_DIRECT")) mac.mouseMoveDirect(-2000, -2000); else mac.mouseMove(-2000, -2000); curX = curY = 0; toX = -1; }
                    else if (!strncmp(c, "to ", 3)) sscanf(c + 3, "%d,%d", &toX, &toY);
                    else if (!strncmp(c, "warp ", 5)) {    // set the cursor (low-memory globals)
                        int x = 0, y = 0;
                        sscanf(c + 5, "%d,%d", &x, &y);
                        for (u32 g : {0x828u, 0x82Cu}) { putBe16(&ram[g], (u16)y); putBe16(&ram[g + 2], (u16)x); }
                        ram[0x8CE] = ram[0x8CF];        // CrsrNew = CrsrCouple
                        curX = x; curY = y; toX = -1;
                    }
                    else if (!strncmp(c, "down", 4)) mac.mouseButton(true);
                    else if (!strncmp(c, "up", 2)) mac.mouseButton(false);
                }
                const char *semi = strchr(p, ';');
                if (!semi) break;
                p = semi + 1;
            }
            if (toX >= 0 && (curX != toX || curY != toY)) {
                int dx = toX - curX, dy = toY - curY;
                dx = dx > 1 ? 1 : dx < -1 ? -1 : dx;
                dy = dy > 1 ? 1 : dy < -1 ? -1 : dy;
                if (getenv("MOUSE_DIRECT")) mac.mouseMoveDirect(dx, dy);   // as the Pi's direct mouse
                else mac.mouseMove(dx, dy);
                curX += dx; curY += dy;
            }
        }
        // Scripted click: park the cursor in the top-left corner, walk it to
        // (clickX, clickY) one pixel per frame (no mouse acceleration), press,
        // hold for CLICK_HOLD frames (default 6) and release.
        if (clickX >= 0) {
            int f0 = frames / 2, walk = f0 + 40;
            int steps = clickX > clickY ? clickX : clickY;
            int press = walk + steps + 5;
            int hold = getenv("CLICK_HOLD") ? atoi(getenv("CLICK_HOLD")) : 6;
            if (f >= f0 && f < walk) mac.mouseMove(-50, -50);
            if (f >= walk && f < walk + steps) mac.mouseMove(f - walk < clickX ? 1 : 0, f - walk < clickY ? 1 : 0);
            if (f == press) mac.mouseButton(true);
            if (f == press + hold) mac.mouseButton(false);
            if (getenv("DOUBLE_CLICK")) {                // and again, to open what was clicked
                if (f == press + hold + 4) mac.mouseButton(true);
                if (f == press + 2 * hold + 4) mac.mouseButton(false);
            }
        }

        for (int d = 0; d < 2; d++)
            if (mac.floppy().ejectedByGuest[d]) { mac.floppy().ejectedByGuest[d] = false; host.ejectDisk(d); printf("frame %d: guest ejected floppy %d\n", f, d + 1); }

        draw(false);
        if (shotEvery && f % shotEvery == shotEvery - 1 && f >= (getenv("SHOT_FROM") ? atoi(getenv("SHOT_FROM")) : 0)) {
            char name[32];
            snprintf(name, sizeof name, "-%05d.ppm", f + 1);
            writePpm(out + name, surf);
        }
    }
    mac.flushDisks();
    writePpm(out + "-final.ppm", surf);
    VideoFrame vf;
    std::vector<u8> bmp;
    if (mac.video(vf)) { bmp.resize(bmpSize(vf)); makeBmp(vf, surf.palette, bmp.data()); }
    if (FILE *bf = fopen((out + "-final.bmp").c_str(), "wb")) { fwrite(bmp.data(), 1, bmp.size(), bf); fclose(bf); }

    if (menu) {
        osd.open();
        osd.draw(surf, font);
        writePpm(out + "-menu.ppm", surf);
        osd.key(Osd::NavSelect);                        // disk picker
        std::fill(pixels.begin(), pixels.end(), 0);     // as the Pi does: pages differ in size
        draw(true);
        osd.draw(surf, font);
        writePpm(out + "-picker.ppm", surf);
        osd.key(Osd::NavBack);
        const char *pages[] = {"-disks.ppm", "-machine.ppm", "-prefs.ppm", "-network.ppm"};
        for (int pg = 0; pg < 4; pg++) {                // the four sub-pages, items 3..6
            osd.close();
            osd.open();
            osd.key(Osd::NavPageUp);
            for (int k = 0; k < (pg ? 3 + pg : 2); k++) osd.key(Osd::NavDown);
            osd.key(Osd::NavSelect);
            std::fill(pixels.begin(), pixels.end(), 0);
            draw(true);
            osd.draw(surf, font);
            writePpm(out + pages[pg], surf);
            osd.key(Osd::NavBack);
        }
    }

    if (!wavPath.empty()) {
        FILE *w = fopen(wavPath.c_str(), "wb");
        if (w) {
            u32 bytes = (u32)wav.size() * 2, rate = 22255;
            u8 h[44] = {'R','I','F','F',0,0,0,0,'W','A','V','E','f','m','t',' ',16,0,0,0,1,0,1,0,0,0,0,0,0,0,0,0,2,0,16,0,'d','a','t','a',0,0,0,0};
            u32 v = bytes + 36; memcpy(h + 4, &v, 4);
            memcpy(h + 24, &rate, 4); v = rate * 2; memcpy(h + 28, &v, 4); memcpy(h + 40, &bytes, 4);
            fwrite(h, 1, 44, w); fwrite(wav.data(), 2, wav.size(), w); fclose(w);
        }
    }
    if (quad && (getenv("ETHER_UDP") || getenv("ETHER_NAT")))
        printf("ethernet: %u frames sent, %u received\n", quad->sonic.framesSent, quad->sonic.framesReceived);
    if (pramFile) {
        if (FILE *pf = fopen(pramFile, "wb")) { fwrite(mac.clockChip().pram, 1, 256, pf); fclose(pf); }
    }
    if (quad && getenv("SHOW_VBR")) printf("vbr %08X bus error handler %08X restartable %d page faults %u\n", quad->cpu.vbr, be32(&ram[quad->cpu.vbr + 8]), quad->cpu.restartable, quad->cpu.pageFaults);
    printf("last trap %04X, pc %08X\n", mac.debugTrap(), mac.debugPC());
    if (gJit && getenv("JIT_STATS"))
        printf("JIT: %u blocks (%u instructions of their own), %llu runs, %llu instructions, %u invalidated, %u flushes\n", gJit->blocks, gJit->nativeInstructions,
               (unsigned long long)gJit->blockRuns, (unsigned long long)gJit->jitInstructions, gJit->invalidated, gJit->flushes);
    if (!saveState.empty()) {
        FILE *sf = fopen(saveState.c_str(), "wb");
        FileState st(sf, true);
        if (sf && quad) { mac.flushDisks(); quad->state(st); }
        if (sf) fclose(sf);
        if (!sf || !quad || !st.ok) fprintf(stderr, "cannot save the state %s\n", saveState.c_str());
    }
    if (share) printf("shared folder: %s, %u calls\n", share->status, share->callsServed);
    if (quad && getenv("MMU_PROBE")) {
        u32 probe = (u32)strtoul(getenv("MMU_PROBE"), 0, 16);
        bool fault = false;
        u32 phys = quad->mmu.translate(probe, false, true, true, fault);
        printf("mmu tc %08X itt %08X %08X dtt %08X %08X srp %08X: %08X -> %08X%s\n", quad->mmu.tc, quad->mmu.itt[0], quad->mmu.itt[1],
               quad->mmu.dtt[0], quad->mmu.dtt[1], quad->mmu.srp, probe, phys, fault ? " FAULT" : "");
    }
    printf("ran %d frames, %llu cycles, PRAM %s%s\n", frames, (unsigned long long)mac.cycles(),
           mac.clockChip().pramDirty ? "modified" : "untouched", mac.poweredOff() ? ", powered off" : "");
    return 0;
}
