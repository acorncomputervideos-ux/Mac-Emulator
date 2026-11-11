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
#include "../core/mac.h"
#include "../core/macii.h"
#include "../core/quadra.h"
#include "../core/keys.h"
#include "../frontend/config.h"
#include "../frontend/osd.h"
#include "../frontend/util.h"
#include "../frontend/video.h"

using namespace macemu;

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
    int listDisks(char (*names)[kNameLen], int max) override {
        static const char *demo[] = {"System 6.0.8.dsk", "MacPaint.dsk", "Utilities.img"};
        int n = 0;
        for (const char *d : demo) if (n < max) strCopy(names[n++], d, kNameLen);
        return n;
    }
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
    bool screenshot() override { return true; }
    const char *infoLine(int n) override { return n == 0 ? mac.modelName() : "F12 closes this menu"; }
};

static u32 gWatchPc = 0;
static M68K *gWatchCpu = nullptr;
static int gWatchHits = 0;
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
    std::string floppy[2], hd[7];
    for (int i = 2; i < argc; i++) {
        std::string a = argv[i];
        const char *v = i + 1 < argc ? argv[i + 1] : "";
        if (a == "--ram") { cfg.ramKB = (u32)atoi(v); i++; }
        else if (a == "--machine") { strCopy(cfg.machine, v, sizeof cfg.machine); i++; }
        else if (a == "--floppy1") { floppy[0] = v; i++; }
        else if (a == "--floppy2") { floppy[1] = v; i++; }
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
    std::vector<u8> ram(cfg.ramKB * 1024);
    bool quadra = strEqNoCase(cfg.machine, "quadra") || (strEqNoCase(cfg.machine, "auto") && romFile.data.size() == 0x100000);
    bool macII = !quadra && (strEqNoCase(cfg.machine, "ii") || (!strEqNoCase(cfg.machine, "plus") && romFile.data.size() >= 0x40000));
    if (!macII && !quadra && ram.size() > 0x400000) ram.resize(0x400000);
    static Machine plus;
    static MacII *two = nullptr;
    Emulator *emu;
    bool ok;
    static Quadra *quad = nullptr;
    if (quadra) {
        if (ram.size() < 0x800000) ram.resize(0x800000);
        quad = new Quadra;
        ok = quad->init(romFile.data.data(), (u32)romFile.data.size(), ram.data(), (u32)ram.size());
        if (traceIo) { quad->ioLog = logIo; }
        if (watchPc) { gWatchPc = watchPc; gWatchCpu = &quad->cpu; quad->stepHook = watchCheck; }
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
    printf("%s, %uK RAM\n", mac.modelName(), (unsigned)(ram.size() / 1024));
    u32 sum = 0;
    for (size_t i = 4; i + 1 < romFile.data.size(); i += 2) sum += (u32)((romFile.data[i] << 8) | romFile.data[i + 1]);
    printf("ROM %zuK, checksum %08X (computed %08X)\n", romFile.data.size() / 1024, be32(romFile.data.data()), sum);

    time_t now = time(nullptr);
    struct tm *tm = localtime(&now);
    mac.clockChip().seconds = (macTime(tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday, tm->tm_hour, tm->tm_min, tm->tm_sec));

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

    Surface surf;
    surf.width = 1280; surf.height = 720; surf.pitch = 1280;
    std::vector<u32> pixels((size_t)surf.pitch * surf.height, 0);
    surf.pixels = pixels.data();
    surf.swapRB = false;
    setPalette(surf, cfg.palette);
    Layout layout = layoutFor(surf, 512, 342, cfg.scale);
    std::vector<u8> prev(0x100000);
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
    for (int f = 0; f < frames; f++) {
        mac.runFrame();
        if (tracePc && f % tracePc == 0 && quad) {
            printf("frame %d: pc=%08X sr=%04X tc=%04X a7=%08X disk activity %u\n", f, quad->cpu.pc, quad->cpu.getSR(), quad->mmu.tc, quad->cpu.a[7], mac.diskActivity());
        }
        if (!wavPath.empty()) { u32 n, rate; const s16 *snd = mac.sound(n, rate); wav.insert(wav.end(), snd, snd + n); }

        // Scripted input, starting once the machine has had time to boot.
        if (f >= frames / 2 && f % 4 == 0 && typed < typeText.size()) {
            bool shift = false;
            int key = asciiToKey(typeText[typed], shift);
            if (key < 0) typed++;
            else if (typeState == 0) {
                if (shift != shiftHeld) { mac.keyEvent(MK_Shift, shift); shiftHeld = shift; }
                mac.keyEvent(key, true); typeState = 1;
            } else { mac.keyEvent(key, false); typeState = 0; typed++; }
        }
        if (clickX >= 0 && f == frames / 2) mac.mouseMove(clickX - 256, clickY - 171);
        if (clickX >= 0 && f == frames / 2 + 10) mac.mouseButton(true);
        if (clickX >= 0 && f == frames / 2 + 16) mac.mouseButton(false);

        for (int d = 0; d < 2; d++)
            if (mac.floppy().ejectedByGuest[d]) { mac.floppy().ejectedByGuest[d] = false; host.ejectDisk(d); printf("frame %d: guest ejected floppy %d\n", f, d + 1); }

        draw(false);
        if (shotEvery && f % shotEvery == shotEvery - 1) {
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
        draw(true);
        osd.draw(surf, font);
        writePpm(out + "-picker.ppm", surf);
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
    printf("ran %d frames, %llu cycles, PRAM %s%s\n", frames, (unsigned long long)mac.cycles(),
           mac.clockChip().pramDirty ? "modified" : "untouched", mac.poweredOff() ? ", powered off" : "");
    return 0;
}
