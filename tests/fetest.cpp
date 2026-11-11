// Tests for the frontend-independent pieces: config file, key map, video
// scaling, BMP export and the on-screen menu.
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "../src/core/keys.h"
#include "../src/core/mac.h"
#include "../src/frontend/config.h"
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
    int resets = 0, nmis = 0, changes = 0, saves = 0, shots = 0;
    bool acceptInsert = true;
    int listDisks(char (*names)[kNameLen], int max) override {
        const char *n[] = {"zebra.dsk", "Apple.dsk", "mango.img"};
        int c = 0;
        for (const char *s : n) if (c < max) strCopy(names[c++], s, kNameLen);
        return c;
    }
    bool insertDisk(int d, const char *name) override { if (!acceptInsert) return false; disk[d] = name; return true; }
    void ejectDisk(int d) override { disk[d].clear(); }
    const char *diskName(int d) override { return disk[d].c_str(); }
    void resetMachine() override { resets++; }
    void interruptButton() override { nmis++; }
    void settingsChanged() override { changes++; }
    bool saveSettings() override { saves++; return true; }
    bool screenshot() override { shots++; return true; }
    const char *infoLine(int) override { return "info"; }
};

struct BoxFont : Font {
    int width() const override { return 8; }
    int height() const override { return 16; }
    bool pixel(char c, int x, int y) const override { return c != ' ' && ((x + y) & 1); }
};

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

    // Floppy 1 -> picker; list is sorted; pick the second image.
    osd.key(Osd::NavSelect);
    osd.key(Osd::NavDown); osd.key(Osd::NavDown);
    osd.key(Osd::NavSelect);
    CHECK(host.disk[0] == "mango.img");
    // Picker again, entry 0 ejects.
    osd.key(Osd::NavSelect);
    osd.key(Osd::NavSelect);
    CHECK(host.disk[0].empty());
    // Esc leaves the picker without changing anything.
    host.disk[0] = "keep.dsk";
    osd.key(Osd::NavSelect);
    osd.key(Osd::NavDown);
    osd.key(Osd::NavBack);
    CHECK(host.disk[0] == "keep.dsk" && osd.active());
    // A rejected image leaves the drive empty and the menu open.
    host.acceptInsert = false;
    osd.key(Osd::NavSelect); osd.key(Osd::NavDown); osd.key(Osd::NavSelect);
    CHECK(host.disk[0].empty() && osd.active());

    osd.key(Osd::NavDown); osd.key(Osd::NavDown);       // speed
    osd.key(Osd::NavRight);
    CHECK(cfg.speed == 2 && host.changes == 1);
    osd.key(Osd::NavRight); osd.key(Osd::NavRight); osd.key(Osd::NavRight);
    CHECK(cfg.speed == 0);
    osd.key(Osd::NavRight);
    CHECK(cfg.speed == 1);
    osd.key(Osd::NavLeft);
    CHECK(cfg.speed == 0);
    osd.key(Osd::NavDown); osd.key(Osd::NavDown);       // palette
    osd.key(Osd::NavLeft);
    CHECK(cfg.palette == kPaletteCount - 1);
    osd.key(Osd::NavDown); osd.key(Osd::NavSelect);     // sound toggles
    CHECK(cfg.sound == 0);
    osd.key(Osd::NavDown);
    for (int i = 0; i < 20; i++) osd.key(Osd::NavRight);
    CHECK(cfg.volume == 10);
    for (int i = 0; i < 20; i++) osd.key(Osd::NavLeft);
    CHECK(cfg.volume == 0);

    // Draw both pages on surfaces of several sizes without overrunning.
    BoxFont font;
    for (int w : {320, 640, 1920}) {
        int h = w * 3 / 4;
        std::vector<u32> px((size_t)w * h + 256, 0x99);
        Surface s{px.data(), w, h, w, {}, false, 0, 0};
        setPalette(s, 0);
        osd.draw(s, font);
        osd.key(Osd::NavPageUp); osd.key(Osd::NavSelect);
        osd.draw(s, font);
        osd.key(Osd::NavBack);
        bool intact = true;
        for (int i = 0; i < 256; i++) if (px[(size_t)w * h + i] != 0x99) intact = false;
        CHECK(intact);
    }

    osd.key(Osd::NavPageDown); osd.key(Osd::NavUp);     // save
    osd.key(Osd::NavSelect);
    CHECK(host.saves == 1 && osd.active());
    osd.key(Osd::NavUp); osd.key(Osd::NavSelect);       // screenshot
    CHECK(host.shots == 1);
    osd.key(Osd::NavUp); osd.key(Osd::NavSelect);       // interrupt closes the menu
    CHECK(host.nmis == 1 && !osd.active());
    osd.open();
    osd.key(Osd::NavUp); osd.key(Osd::NavSelect);       // reset
    CHECK(host.resets == 1 && !osd.active());
    osd.open();
    osd.key(Osd::NavBack);
    CHECK(!osd.active());
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
