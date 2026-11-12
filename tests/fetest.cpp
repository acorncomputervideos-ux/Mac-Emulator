// Tests for the frontend-independent pieces: config file, key map, video
// scaling, BMP export and the on-screen menu.
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "../src/core/keys.h"
#include "../src/core/mac.h"
#include "../src/frontend/config.h"
#include "../src/frontend/romid.h"
#include "../src/frontend/keymap.h"
#include "../src/frontend/osd.h"
#include "../src/frontend/util.h"
#include "../src/frontend/video.h"

using namespace macemu;

static int failures = 0, checks = 0;
#define CHECK(cond) do { checks++; if (!(cond)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

static void testConfig()
{
    Config c;
    c.setDefaults();
    CHECK(c.ramKB == 8192 && c.speed == 1 && c.sound == 1 && c.mouseSpeed == 4 && c.videoWidth == 640);
    const char *text =
        "# comment\r\n"
        "rom = MyPlus.rom\r\n"
        "ram=1024\n"
        "machine=II\n"
        "video=832x624\n"
        "floppy1=disks/System Tools.dsk\n"
        "hd0=hd/boot.img\n"
        "hd6 = other.img  \n"
        "speed=max\n"
        "scale=2\n"
        "palette=3\n"
        "volume=99\n"
        "mouse_speed=0\n"
        "sound_device=PWM\n"
        "date=1987-03-02 09:30\n"
        "nonsense line\n"
        "unknown=1\n";
    c.parse(text, (u32)strlen(text));
    CHECK(!strcmp(c.rom, "MyPlus.rom"));
    CHECK(c.ramKB == 1024 && strEqNoCase(c.machine, "ii") && c.videoWidth == 832 && c.videoHeight == 624);
    CHECK(!strcmp(c.floppy[0], "disks/System Tools.dsk"));
    CHECK(!strcmp(c.hd[0], "hd/boot.img") && !strcmp(c.hd[6], "other.img") && !c.hd[1][0]);
    CHECK(c.speed == 0 && c.scale == 2 && c.palette == 3);
    CHECK(c.volume == 10 && c.mouseSpeed == 1);
    CHECK(strEqNoCase(c.soundDevice, "pwm"));

    char buf[2048];
    u32 n = c.serialize(buf, sizeof buf);
    Config d;
    d.setDefaults();
    d.parse(buf, n);
    CHECK(!memcmp(&c, &d, sizeof c));

    Config bad;
    bad.setDefaults();
    const char *junk = "ram=300\nspeed=3\npalette=-4\nscale=100\nmachine=lisa\nvideo=4000x10\n";
    bad.parse(junk, (u32)strlen(junk));
    CHECK(bad.ramKB == 8192 && bad.speed == 1 && bad.palette == 0 && bad.scale == 8 && strEq(bad.machine, "auto") && bad.videoWidth == 640);

    // Clock: 1987-03-02 09:30 is 30376 days after 1904-01-01.
    CHECK(c.clockSeconds("Jan  1 2000", "00:00:00") == 30376u * 86400 + 9 * 3600 + 30 * 60);
    Config e;
    e.setDefaults();
    CHECK(e.clockSeconds("Nov 11 2026", "01:02:03") == macTime(2026, 11, 11, 1, 2, 3));
    CHECK(macTime(1904, 1, 1, 0, 0, 0) == 0);
    CHECK(macTime(1970, 1, 1, 0, 0, 0) == 2082844800u);
    CHECK(macTime(2000, 3, 1, 0, 0, 0) == 2082844800u + 951868800u);
}

static void testKeymap()
{
    CHECK(usbToMacKey(0x04) == MK_A && usbToMacKey(0x1D) == MK_Z && usbToMacKey(0x16) == MK_S);
    CHECK(usbToMacKey(0x1E) == MK_1 && usbToMacKey(0x27) == MK_0);
    CHECK(usbToMacKey(0x28) == MK_Return && usbToMacKey(0x2C) == MK_Space);
    CHECK(usbToMacKey(0x50) == MK_Left && usbToMacKey(0x52) == MK_Up);
    CHECK(usbToMacKey(0x62) == MK_Kp0 && usbToMacKey(0x59) == MK_Kp1);
    CHECK(usbToMacKey(HID_F12) == MK_None && usbToMacKey(0xE0) == MK_None && usbToMacKey(0x3A) == MK_F1);
    CHECK(usbToMacKey(0x29) == MK_Escape && usbToMacKey(0x4C) == MK_ForwardDelete && usbToMacKey(0x55) == MK_KpMultiply);
    CHECK(usbModifierToMacKey(HIDMOD_LALT, false) == MK_Command && usbModifierToMacKey(HIDMOD_LALT, true) == MK_Option);
    CHECK(usbModifierToMacKey(HIDMOD_LGUI, false) == MK_Option && usbModifierToMacKey(HIDMOD_RSHIFT, false) == MK_Shift);
    CHECK(usbModifierToMacKey(HIDMOD_RCTRL, false) == MK_Control);
    // Every mapped key must be a valid 7-bit Mac key code, and no two letters collide.
    bool seen[128] = {false};
    for (int u = 0x04; u <= 0x67; u++) { int k = usbToMacKey((u8)u); if (k == MK_None) continue; CHECK(k >= 0 && k < 0x80 && (!seen[k] || k == MK_Backslash || k == MK_Grave)); seen[k] = true; }
}

static Surface makeSurface(std::vector<u32> &px, int w, int h, u32 fill)
{
    px.assign((size_t)w * h + 64, fill);
    Surface s{px.data(), w, h, w, {}, false, 0, 0};
    setPalette(s, 0);
    return s;
}

static void testVideo()
{
    std::vector<u32> px;
    Surface s = makeSurface(px, 800, 600, 0xEEEEEE);
    Layout l = layoutFor(s, 512, 342, 0);
    CHECK(l.scale == 1 && l.x0 == 144 && l.y0 == 129);
    std::vector<u32> bigPx;
    Surface b = makeSurface(bigPx, 1920, 1080, 0xEEEEEE);
    Layout lb = layoutFor(b, 512, 342, 0);
    CHECK(lb.scale == 3 && lb.x0 == 192 && lb.y0 == 27);
    CHECK(layoutFor(b, 512, 342, 2).scale == 2 && layoutFor(b, 512, 342, 7).scale == 3);
    CHECK(layoutFor(b, 640, 480, 0).scale == 2);

    std::vector<u8> mac(Machine::kScreenBytes, 0), prev(Machine::kScreenBytes, 0xFF);
    mac[0] = 0x80;                                      // top-left pixel black
    mac[64 * 341 + 63] = 0x01;                          // bottom-right pixel black
    VideoFrame f{512, 342, 1, 64, mac.data(), nullptr, 0};
    CHECK(blitScreen(b, lb, f, prev.data(), false) == 342);
    CHECK(bigPx[27 * 1920 + 192] == 0x000000 && bigPx[29 * 1920 + 194] == 0x000000 && bigPx[27 * 1920 + 195] == 0xFFFFFF);
    CHECK(bigPx[(27 + 1025) * 1920 + 192 + 1535] == 0 && bigPx[(27 + 1022) * 1920 + 192 + 1532] == 0xFFFFFF);
    CHECK(bigPx[26 * 1920 + 192] == 0xEEEEEE && bigPx[27 * 1920 + 191] == 0xEEEEEE && bigPx[(27 + 1026) * 1920 + 192] == 0xEEEEEE);
    CHECK(blitScreen(b, lb, f, prev.data(), false) == 0);     // nothing changed
    mac[64 * 10] = 0xFF;
    CHECK(blitScreen(b, lb, f, prev.data(), false) == 1);
    CHECK(blitScreen(b, lb, f, prev.data(), true) == 342);

    // Colour schemes recolour one-bit screens.
    setPalette(b, 2);
    blitScreen(b, lb, f, prev.data(), true);
    CHECK(bigPx[27 * 1920 + 192] == 0x04180A && bigPx[27 * 1920 + 195] == 0x4AFF7A);

    // 8-bit indexed through a colour table, with a row pitch wider than the image.
    u32 clut[256];
    for (int i = 0; i < 256; i++) clut[i] = (u32)i * 0x010101;
    std::vector<u8> fb8(1024 * 480, 0);
    fb8[0] = 7; fb8[1024 * 479 + 639] = 200;
    VideoFrame f8{640, 480, 8, 1024, fb8.data(), clut, 1};
    Layout l8 = layoutFor(b, 640, 480, 0);
    std::vector<u8> prev8(640 * 480);
    CHECK(blitScreen(b, l8, f8, prev8.data(), true) == 480);
    CHECK(bigPx[l8.y0 * 1920 + l8.x0] == 0x070707);
    CHECK(bigPx[(l8.y0 + 959) * 1920 + l8.x0 + 1279] == 0xC8C8C8);
    // 2- and 4-bit packing, 16-bit x555 and 32-bit direct colour.
    u8 p2[1] = {0x1B}, p4[1] = {0xA5}, p16[2] = {0x7C, 0x00}, p32[4] = {0, 0x12, 0x34, 0x56};
    std::vector<u32> tinyPx;
    Surface t = makeSurface(tinyPx, 8, 8, 0);
    Layout one = layoutFor(t, 4, 1, 1);
    VideoFrame f2{4, 1, 2, 1, p2, clut, 0};
    blitScreen(t, one, f2, nullptr, true);
    CHECK(tinyPx[one.y0 * 8 + one.x0] == 0 && tinyPx[one.y0 * 8 + one.x0 + 3] == 0x030303);
    VideoFrame f4{2, 1, 4, 1, p4, clut, 0};
    Layout two = layoutFor(t, 2, 1, 1);
    blitScreen(t, two, f4, nullptr, true);
    CHECK(tinyPx[two.y0 * 8 + two.x0] == 0x0A0A0A && tinyPx[two.y0 * 8 + two.x0 + 1] == 0x050505);
    VideoFrame f16{1, 1, 16, 2, p16, nullptr, 0};
    Layout px1 = layoutFor(t, 1, 1, 1);
    blitScreen(t, px1, f16, nullptr, true);
    CHECK(tinyPx[px1.y0 * 8 + px1.x0] == 0xFF0000);
    VideoFrame f32{1, 1, 32, 4, p32, nullptr, 0};
    blitScreen(t, px1, f32, nullptr, true);
    CHECK(tinyPx[px1.y0 * 8 + px1.x0] == 0x123456);
    // A BGR device gets swapped colours, but one-bit screens keep the scheme.
    t.swapRB = true;
    setPalette(t, 0);
    blitScreen(t, px1, f32, nullptr, true);
    CHECK(tinyPx[px1.y0 * 8 + px1.x0] == 0x563412);
    u8 oneBit[1] = {0x80};
    VideoFrame f1{1, 1, 1, 1, oneBit, nullptr, 0};
    blitScreen(t, px1, f1, nullptr, true);
    CHECK(tinyPx[px1.y0 * 8 + px1.x0] == 0x000000 && t.palette[ColSelBg] == 0xA84F2A);

    // A surface smaller than the Mac screen must clip, not overrun.
    std::vector<u32> smallPx;
    Surface sm = makeSurface(smallPx, 320, 200, 0x555555);
    Layout ls = layoutFor(sm, 512, 342, 0);
    blitScreen(sm, ls, f, nullptr, true);
    bool intact = true;
    for (int i = 0; i < 64; i++) if (smallPx[320 * 200 + i] != 0x555555) intact = false;
    CHECK(intact);
    fillRect(sm, -10, -10, 1000, 1000, ColDim);
    for (int i = 0; i < 64; i++) if (smallPx[320 * 200 + i] != 0x555555) intact = false;
    CHECK(intact && smallPx[0] == 0x707070 && smallPx[320 * 200 - 1] == 0x707070);

    u32 mono[2] = {0xFFFFFF, 0x000000};
    std::vector<u8> bmp(bmpSize(f));
    CHECK(bmp.size() == 54 + 1536 * 342);
    makeBmp(f, mono, bmp.data());
    CHECK(bmp[0] == 'B' && bmp[1] == 'M' && bmp[18] == 0 && bmp[19] == 2 && bmp[22] == 0x56 && bmp[23] == 1 && bmp[28] == 24);
    u32 top = 54 + 1536 * 341;                          // first Mac row is the last BMP row
    CHECK(bmp[top] == 0 && bmp[top + 3] == 0xFF);
    std::vector<u8> bmp8(bmpSize(f8));
    makeBmp(f8, mono, bmp8.data());
    CHECK(bmp8[54 + 1920 * 479] == 7);

    u32 rgb[kColorCount];
    for (int p = 0; p < kPaletteCount; p++) { paletteColors(p, rgb); CHECK(rgb[ColPaper] != rgb[ColInk] && paletteName(p)[0]); }
}

struct FakeHost : OsdHost {
    std::string disk[2];
    int resets = 0, nmis = 0, changes = 0, saves = 0, shots = 0, reboots = 0;
    bool acceptInsert = true;
    int listFiles(FileKind kind, char (*names)[kNameLen], int max) override {
        static const char *floppies[] = {"zebra.dsk", "Apple.dsk", "mango.img"};
        static const char *hds[] = {"hd0.img", "disks/System 7.5.hda"};
        static const char *roms[] = {"Quadra-650.ROM", "MacPlus.rom"};
        static const char *nets[] = {"Strong Net", "Another"};
        static const char *cds[] = {"Install.iso"};
        if (kind == CdImages) { strCopy(names[0], cds[0], kNameLen); return 1; }
        const char **list = kind == Floppies ? floppies : kind == HardDisks ? hds : kind == WifiNetworks ? nets : roms;
        int n = kind == Floppies ? 3 : 2, c = 0;
        for (int i = 0; i < n && c < max; i++) strCopy(names[c++], list[i], kNameLen);
        return c;
    }
    bool insertDisk(int d, const char *name) override { if (!acceptInsert) return false; disk[d] = name; return true; }
    void ejectDisk(int d) override { disk[d].clear(); }
    const char *diskName(int d) override { return disk[d].c_str(); }
    std::string cd;
    bool insertCd(const char *n) override { cd = n ? n : ""; return true; }
    const char *cdName() override { return cd.c_str(); }
    void resetMachine() override { resets++; }
    void interruptButton() override { nmis++; }
    void settingsChanged() override { changes++; }
    bool saveSettings() override { saves++; return true; }
    void saveAndReboot() override { reboots++; }
    int offs = 0;
    void powerOff() override { offs++; }
    bool screenshot() override { shots++; return true; }
    const char *infoLine(int) override { return "info"; }
    const char *networkLine(int n) override { return n == 0 ? "http://192.168.1.20/" : "net"; }
};

struct BoxFont : Font {
    int width() const override { return 8; }
    int height() const override { return 16; }
    bool pixel(char c, int x, int y) const override { return c != ' ' && ((x + y) & 1); }
};

// Main page order: floppy 1, floppy 2, hard disks, machine, prefs, network,
// restart, interrupt, screenshot, save, turn off, resume.
static void go(Osd &o, int n) { o.key(Osd::NavPageUp); for (int i = 0; i < n; i++) o.key(Osd::NavDown); }

static void testOsd()
{
    Config cfg;
    cfg.setDefaults();
    FakeHost host;
    Osd osd(cfg, host);
    CHECK(!osd.active());
    osd.key(Osd::NavSelect);                            // ignored while closed
    CHECK(host.changes == 0);
    osd.open();
    CHECK(osd.active() && osd.needsRedraw() && !osd.needsRedraw());

    // Floppies: picker sorted, entry 0 ejects, Esc leaves things alone.
    osd.key(Osd::NavSelect);
    osd.key(Osd::NavDown); osd.key(Osd::NavDown);
    osd.key(Osd::NavSelect);
    CHECK(host.disk[0] == "mango.img");
    go(osd, 1); osd.key(Osd::NavSelect); osd.key(Osd::NavDown); osd.key(Osd::NavSelect);
    CHECK(host.disk[1] == "Apple.dsk");
    go(osd, 0); osd.key(Osd::NavSelect); osd.key(Osd::NavSelect);
    CHECK(host.disk[0].empty());
    host.disk[0] = "keep.dsk";
    osd.key(Osd::NavSelect); osd.key(Osd::NavDown); osd.key(Osd::NavBack);
    CHECK(host.disk[0] == "keep.dsk" && osd.active());
    host.acceptInsert = false;
    osd.key(Osd::NavSelect); osd.key(Osd::NavDown); osd.key(Osd::NavSelect);
    CHECK(host.disk[0].empty() && osd.active());
    host.acceptInsert = true;

    // Hard disks page: pick an image for SCSI ID 2, then clear ID 0.
    go(osd, 2); osd.key(Osd::NavSelect);
    osd.key(Osd::NavDown); osd.key(Osd::NavDown); osd.key(Osd::NavSelect);   // ID 2 -> picker
    osd.key(Osd::NavDown); osd.key(Osd::NavDown); osd.key(Osd::NavSelect);   // second file (sorted)
    CHECK(!strcmp(cfg.hd[2], "hd0.img") || !strcmp(cfg.hd[2], "disks/System 7.5.hda"));
    strCopy(cfg.hd[0], "old.img", Config::kPath);
    osd.key(Osd::NavPageUp); osd.key(Osd::NavSelect); osd.key(Osd::NavSelect);   // ID 0 -> none
    CHECK(strEq(cfg.hd[0], "none"));
    osd.key(Osd::NavPageDown); osd.key(Osd::NavUp); osd.key(Osd::NavSelect); // CD-ROM picker
    osd.key(Osd::NavDown); osd.key(Osd::NavSelect);
    CHECK(host.cd == "Install.iso");
    osd.key(Osd::NavBack);                                                   // back to main
    go(osd, 6); osd.key(Osd::NavSelect);                                     // restart attaches them
    CHECK(host.resets == 1 && !osd.active());

    // Machine page: model, ROM, memory and video need a reboot.
    osd.open();
    go(osd, 3); osd.key(Osd::NavSelect);
    CHECK(!osd.rebootPending());
    osd.key(Osd::NavRight);                                                  // model auto -> plus
    CHECK(strEq(cfg.machine, "plus") && osd.rebootPending());
    osd.key(Osd::NavDown); osd.key(Osd::NavSelect); osd.key(Osd::NavDown); osd.key(Osd::NavSelect);
    CHECK(strEq(cfg.rom, "MacPlus.rom"));
    osd.key(Osd::NavDown);                                                   // memory
    u32 ram = cfg.ramKB;
    osd.key(Osd::NavRight);
    CHECK(cfg.ramKB == ram * 2);
    for (int i = 0; i < 20; i++) osd.key(Osd::NavLeft);
    CHECK(cfg.ramKB >= 512 && cfg.ramKB <= 131072);
    osd.key(Osd::NavDown); osd.key(Osd::NavRight);                           // Mac II video
    CHECK(cfg.videoWidth == 832 && cfg.videoHeight == 624);
    osd.key(Osd::NavDown); osd.key(Osd::NavRight);                           // Quadra monitor
    CHECK(strEq(cfg.monitor, "21"));
    osd.key(Osd::NavDown); osd.key(Osd::NavRight);                           // scaling
    CHECK(cfg.sharp == 1);
    osd.key(Osd::NavDown); osd.key(Osd::NavSelect);                          // apply
    CHECK(host.reboots == 1);
    osd.key(Osd::NavBack);

    // Display, sound and input.
    go(osd, 4); osd.key(Osd::NavSelect);
    osd.key(Osd::NavRight);
    CHECK(cfg.speed == 2 && host.changes == 1);
    osd.key(Osd::NavRight); osd.key(Osd::NavRight); osd.key(Osd::NavRight);
    CHECK(cfg.speed == 0);
    osd.key(Osd::NavRight);
    CHECK(cfg.speed == 1);
    osd.key(Osd::NavDown); osd.key(Osd::NavLeft);
    CHECK(cfg.palette == kPaletteCount - 1);
    osd.key(Osd::NavDown); osd.key(Osd::NavSelect);
    CHECK(cfg.sound == 0);
    osd.key(Osd::NavDown);
    for (int i = 0; i < 20; i++) osd.key(Osd::NavRight);
    CHECK(cfg.volume == 10);
    for (int i = 0; i < 20; i++) osd.key(Osd::NavLeft);
    CHECK(cfg.volume == 0);
    osd.key(Osd::NavPageDown); osd.key(Osd::NavSelect);                      // back
    osd.key(Osd::NavBack);
    CHECK(!osd.active());

    // Every page draws at small, medium and large sizes without overruns.
    BoxFont font;
    for (int w : {512, 640, 1920}) {
        int h = w == 512 ? 384 : w * 3 / 4;
        std::vector<u32> px((size_t)w * h + 256, 0x99);
        Surface s{px.data(), w, h, w, {}, false, 0, 0};
        setPalette(s, 0);
        for (int pageIdx = 0; pageIdx < 5; pageIdx++) {
            osd.open();
            if (pageIdx) { go(osd, 1 + pageIdx); osd.key(Osd::NavSelect); }
            osd.draw(s, font);
        }
        osd.open(); osd.key(Osd::NavSelect); osd.draw(s, font);              // picker
        osd.close();
        bool intact = true;
        for (int i = 0; i < 256; i++) if (px[(size_t)w * h + i] != 0x99) intact = false;
        CHECK(intact);
    }

    // Main page actions.
    osd.open();
    go(osd, 9); osd.key(Osd::NavSelect);
    CHECK(host.saves == 1 && osd.active());
    go(osd, 8); osd.key(Osd::NavSelect);
    CHECK(host.shots == 1);
    go(osd, 7); osd.key(Osd::NavSelect);
    CHECK(host.nmis == 1 && !osd.active());
    osd.open();
    go(osd, 10); osd.key(Osd::NavSelect);
    CHECK(host.offs == 1);
    osd.open();
    go(osd, 11); osd.key(Osd::NavSelect);
    CHECK(!osd.active());

    // Network page: connection type, Wi-Fi name and password typed in.
    Osd net(cfg, host);
    net.open();
    go(net, 5); net.key(Osd::NavSelect);
    CHECK(strEq(cfg.network, "auto"));
    net.key(Osd::NavRight);
    CHECK(strEq(cfg.network, "ethernet") && net.rebootPending());
    net.key(Osd::NavRight); net.key(Osd::NavRight);
    CHECK(strEq(cfg.network, "off"));
    net.key(Osd::NavDown); net.key(Osd::NavSelect);                         // scan: picker keeps signal order
    net.key(Osd::NavDown); net.key(Osd::NavSelect);                          // first = strongest
    CHECK(strEq(cfg.wifiSsid, "Strong Net") && net.editing());                // then straight to the password
    net.key(Osd::NavBack);
    CHECK(!net.editing() && strEq(cfg.network, "auto"));
    strCopy(cfg.network, "off", sizeof cfg.network);
    net.key(Osd::NavPageUp); net.key(Osd::NavDown);
    net.key(Osd::NavDown); net.key(Osd::NavSelect);                         // edit the name
    CHECK(net.editing());
    for (int i = 0; i < 40; i++) net.typeChar('\b');                        // clear the old name
    for (const char *t = "My Wifi!x"; *t; t++) net.typeChar(*t);
    net.typeChar('\b');
    net.typeChar('\n');                                                    // ignored
    net.key(Osd::NavSelect);
    CHECK(!net.editing() && strEq(cfg.wifiSsid, "My Wifi!"));
    CHECK(strEq(cfg.network, "auto"));                                       // a name turns Wi-Fi on
    net.key(Osd::NavDown); net.key(Osd::NavSelect);                          // password
    for (const char *t = "p@ss \"#1"; *t; t++) net.typeChar(*t);
    net.key(Osd::NavBack);                                                   // cancel
    CHECK(cfg.wifiPassword[0] == 0 && net.active());
    net.key(Osd::NavSelect);
    for (const char *t = "p@ss \"#1"; *t; t++) net.typeChar(*t);
    net.key(Osd::NavSelect);
    CHECK(strEq(cfg.wifiPassword, "p@ss \"#1"));
    for (int i = 0; i < 70; i++) { net.key(Osd::NavSelect); net.typeChar('a'); net.key(Osd::NavSelect); }
    CHECK(strLen(cfg.wifiPassword) == 63);                                   // never overflows
    net.key(Osd::NavDown); net.key(Osd::NavRight);
    CHECK(strEq(cfg.wifiCountry, "US"));
    net.key(Osd::NavDown); net.key(Osd::NavSelect);
    CHECK(strEq(cfg.keyboard, "us"));
    {
        BoxFont f;
        std::vector<u32> px(640 * 480 + 256, 0x99);
        Surface s{px.data(), 640, 480, 640, {}, false, 0, 0};
        setPalette(s, 0);
        net.key(Osd::NavUp); net.key(Osd::NavUp); net.key(Osd::NavUp); net.key(Osd::NavSelect);  // editing, long text
        CHECK(net.editing());
        net.draw(s, f);
        net.key(Osd::NavBack);
        bool intact = true;
        for (int i = 0; i < 256; i++) if (px[640 * 480 + i] != 0x99) intact = false;
        CHECK(intact);
    }
    net.key(Osd::NavPageDown); net.key(Osd::NavUp); net.key(Osd::NavSelect);  // save and restart
    CHECK(host.reboots == 2);

    // The settings survive a save and load.
    char text[4096];
    u32 len = cfg.serialize(text, sizeof text);
    Config back;
    back.setDefaults();
    back.parse(text, len);
    CHECK(strEq(back.wifiSsid, "My Wifi!") && strEq(back.wifiPassword, cfg.wifiPassword));
    CHECK(strEq(back.network, "auto") && strEq(back.wifiCountry, "US") && strEq(back.keyboard, "us"));
    back.parse("network=0\nwifi_country=de\n", 26);
    CHECK(strEq(back.network, "off") && strEq(back.wifiCountry, "DE"));
    back.parse("network=1\n", 10);
    CHECK(strEq(back.network, "auto"));

    // The Mac's Ethernet and a fixed address.
    CHECK(back.macEthernet == 6066 && !back.macEthernetPeer[0] && !back.ipAddress[0]);
    const char *eth = "mac_ethernet=off\nip_address=192.168.0.9\nip_gateway=192.168.0.1\nmac_ethernet_peer=10.1.2.3\n";
    back.parse(eth, (u32)strlen(eth));
    CHECK(back.macEthernet == 0 && strEq(back.ipAddress, "192.168.0.9") && strEq(back.macEthernetPeer, "10.1.2.3"));
    len = back.serialize(text, sizeof text);
    Config again;
    again.setDefaults();
    again.parse(text, len);
    CHECK(again.macEthernet == 0 && strEq(again.ipGateway, "192.168.0.1") && strEq(again.macEthernetPeer, "10.1.2.3"));
    CHECK(again.macInternet == 1);
    again.parse("mac_internet=off\n", 17);
    CHECK(again.macInternet == 0);
    CHECK(again.httpsHelper == 1);
    again.parse("https_helper=off\n", 17);
    CHECK(again.httpsHelper == 0);
    again.parse("mac_ethernet=on\n", 16);
    CHECK(again.macEthernet == 6066);
    again.parse("mac_ethernet=6100\n", 18);
    CHECK(again.macEthernet == 6100);
    u8 ip[4] = {1, 1, 1, 1};
    CHECK(parseIPv4("192.168.0.9", ip) && ip[0] == 192 && ip[1] == 168 && ip[2] == 0 && ip[3] == 9);
    CHECK(!parseIPv4("192.168.0", ip) && !parseIPv4("256.1.1.1", ip) && !parseIPv4("1.2.3.4x", ip) && !parseIPv4("", ip));
    CHECK(ip[3] == 9);                                  // untouched by the failures

    // ROM files: extra bytes after a ROM are dropped when its checksum says so.
    {
        std::vector<u8> rom(0x20000 + 7504, 0x5A);
        u32 sum = 0;
        for (u32 a = 4; a < 0x20000; a += 2) sum += (u32)((rom[a] << 8) | rom[a + 1]);
        putBe32(rom.data(), sum);
        CHECK(romImageSize(rom.data(), (u32)rom.size()) == 0x20000);
        rom[100] ^= 1;                                  // wrong checksum: not a ROM
        CHECK(romImageSize(rom.data(), (u32)rom.size()) == 0);
        CHECK(romImageSize(rom.data(), 0x40000) == 0x40000);   // standard sizes as they are
        putBe32(rom.data(), 0xF1A6F343);
        CHECK(romIdentify(rom.data(), 0x100000).family == RomQuadra);
        putBe32(rom.data(), 0xA49F9914);
        CHECK(romIdentify(rom.data(), 0x80000).family == RomSE);
        putBe32(rom.data(), 0x97221136);
        CHECK(romIdentify(rom.data(), 0x40000).family == RomNotYet);
    }

    // Typing.
    CHECK(usbToAscii(0x04, false, true) == 'a' && usbToAscii(0x04, true, true) == 'A');
    CHECK(usbToAscii(0x1F, true, false) == '@' && usbToAscii(0x1F, true, true) == '"');
    CHECK(usbToAscii(0x34, true, true) == '@' && usbToAscii(0x34, true, false) == '"');
    CHECK(usbToAscii(0x27, false, true) == '0' && usbToAscii(0x2A, false, true) == '\b');
    CHECK(usbToAscii(0x2C, false, true) == ' ' && usbToAscii(0x3A, false, true) == 0);
}

int main()
{
    testConfig();
    testKeymap();
    testVideo();
    testOsd();
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
