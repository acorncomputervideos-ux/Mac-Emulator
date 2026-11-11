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
    CHECK(c.ramKB == 4096 && c.speed == 1 && c.sound == 1 && c.mouseSpeed == 4);
    const char *text =
        "# comment\r\n"
        "rom = MyPlus.rom\r\n"
        "ram=1024\n"
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
    CHECK(c.ramKB == 1024);
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
    const char *junk = "ram=300\nspeed=3\npalette=-4\nscale=100\n";
    bad.parse(junk, (u32)strlen(junk));
    CHECK(bad.ramKB == 4096 && bad.speed == 1 && bad.palette == 0 && bad.scale == 8);

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
    CHECK(usbToMacKey(HID_F12) == MK_None && usbToMacKey(0xE0) == MK_None);
    CHECK(usbModifierToMacKey(HIDMOD_LALT, false) == MK_Command && usbModifierToMacKey(HIDMOD_LALT, true) == MK_Option);
    CHECK(usbModifierToMacKey(HIDMOD_LGUI, false) == MK_Option && usbModifierToMacKey(HIDMOD_RSHIFT, false) == MK_Shift);
    // Every mapped key must be a valid 7-bit Mac key code, and no two letters collide.
    bool seen[128] = {false};
    for (int u = 0x04; u <= 0x27; u++) { int k = usbToMacKey((u8)u); CHECK(k >= 0 && k < 0x40 && !seen[k]); seen[k] = true; }
}

static void testVideo()
{
    std::vector<u8> px(800 * 600, 0xEE);
    Surface s{px.data(), 800, 600, 800};
    Layout l = layoutFor(s, 0);
    CHECK(l.scale == 1 && l.x0 == 144 && l.y0 == 129);
    std::vector<u8> big(1920 * 1080, 0xEE);
    Surface b{big.data(), 1920, 1080, 1920};
    Layout lb = layoutFor(b, 0);
    CHECK(lb.scale == 3 && lb.x0 == 192 && lb.y0 == 27);
    CHECK(layoutFor(b, 2).scale == 2 && layoutFor(b, 7).scale == 3);

    std::vector<u8> mac(Machine::kScreenBytes, 0), prev(Machine::kScreenBytes, 0xFF);
    mac[0] = 0x80;                                      // top-left pixel black
    mac[64 * 341 + 63] = 0x01;                          // bottom-right pixel black
    CHECK(blitMacScreen(b, lb, mac.data(), prev.data(), false) == 342);
    CHECK(big[27 * 1920 + 192] == ColInk && big[29 * 1920 + 194] == ColInk && big[27 * 1920 + 195] == ColPaper);
    CHECK(big[(27 + 1025) * 1920 + 192 + 1535] == ColInk && big[(27 + 1022) * 1920 + 192 + 1532] == ColPaper);
    CHECK(big[26 * 1920 + 192] == 0xEE && big[27 * 1920 + 191] == 0xEE && big[(27 + 1026) * 1920 + 192] == 0xEE);
    CHECK(blitMacScreen(b, lb, mac.data(), prev.data(), false) == 0);     // nothing changed
    mac[64 * 10] = 0xFF;
    CHECK(blitMacScreen(b, lb, mac.data(), prev.data(), false) == 1);
    CHECK(blitMacScreen(b, lb, mac.data(), prev.data(), true) == 342);

    // A surface smaller than the Mac screen must clip, not overrun.
    std::vector<u8> tiny(320 * 200 + 64, 0x55);
    Surface t{tiny.data(), 320, 200, 320};
    Layout lt = layoutFor(t, 0);
    blitMacScreen(t, lt, mac.data(), nullptr, true);
    bool intact = true;
    for (int i = 0; i < 64; i++) if (tiny[320 * 200 + i] != 0x55) intact = false;
    CHECK(intact);
    fillRect(t, -10, -10, 1000, 1000, 7);
    for (int i = 0; i < 64; i++) if (tiny[320 * 200 + i] != 0x55) intact = false;
    CHECK(intact && tiny[0] == 7 && tiny[320 * 200 - 1] == 7);

    std::vector<u8> bmp(kBmpSize);
    makeBmp(mac.data(), bmp.data());
    CHECK(bmp[0] == 'B' && bmp[1] == 'M' && bmp[18] == 0 && bmp[19] == 2 && bmp[22] == 0x56 && bmp[23] == 1);
    CHECK(bmp[62 + 64 * 341] == 0x80 && bmp[62 + 63] == 0x01);

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
        std::vector<u8> px((size_t)w * h + 256, 0x99);
        Surface s{px.data(), w, h, w};
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
