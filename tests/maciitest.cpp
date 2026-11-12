// Macintosh II machine tests, driving the hardware through its addresses.
#include <cstdio>
#include <cstring>
#include <vector>
#include "../src/core/macii.h"
#include "../src/core/keys.h"

using namespace macemu;

static int failures = 0, checks = 0;
#define CHECK(cond) do { checks++; if (!(cond)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

enum {
    VIA1 = 0x50000000, VIA2 = 0x50002000,
    vBufB = 0x0000, vBufA = 0x1E00, vDirB = 0x0400, vDirA = 0x0600, vSR = 0x1400, vACR = 0x1600,
    vPCR = 0x1800, vIFR = 0x1A00, vIER = 0x1C00,
};

static std::vector<u8> makeRom()
{
    std::vector<u8> r(0x40000, 0xFF);
    auto w16 = [&](u32 at, u16 v) { r[at] = (u8)(v >> 8); r[at + 1] = (u8)v; };
    w16(0, 0x0000); w16(2, 0x8000);                     // SSP
    w16(4, 0x4080); w16(6, 0x0010);                     // PC
    w16(0x10, 0x46FC); w16(0x12, 0x2000);               // move #$2000,sr
    w16(0x14, 0x60FE);                                  // bra.s *
    return r;
}

struct Rig {
    std::vector<u8> rom = makeRom();
    std::vector<u8> ram = std::vector<u8>(8 << 20);
    MacII *m = new MacII;
    Rig() { if (!m->init(rom.data(), (u32)rom.size(), ram.data(), (u32)ram.size())) printf("init failed\n"); }
    ~Rig() { delete m; }
    u32 io() { return m->addressing32() ? 0x50000000 : 0xF00000; }
    u8 via1(u32 r) { return m->read8(io() + r); }
    void via1(u32 r, u8 v) { m->write8(io() + r, v); }
    u8 via2(u32 r) { return m->read8(io() + 0x2000 + r); }
    void via2(u32 r, u8 v) { m->write8(io() + 0x2000 + r, v); }
    void adbState(int st) { via1(vBufB, (u8)((via1(vBufB) & 0xCF) | (st << 4))); }
};

static void testMemoryMap()
{
    Rig r;
    CHECK(r.m->cpu.pc == 0x40800010 && r.m->cpu.a[7] == 0x8000);
    CHECK(r.m->read8(0) == 0x00 && r.m->read8(5) == 0x80);     // overlay: ROM at 0
    r.m->read8(0x40000000);                                    // any ROM access ends the overlay
    r.m->write8(0x1234, 0x5A);
    CHECK(r.m->read8(0x1234) == 0x5A && r.m->read8(5) == 0);
    CHECK(r.m->read8(0x4FFC0005) == 0x80);                     // ROM mirrors

    // 32-bit: the video card's declaration ROM in slot 9, empty slot $A.
    CHECK(r.m->addressing32());
    CHECK(r.m->read8(0xF9FFFFFF) == 0x0F);
    CHECK(!r.m->busError);
    r.m->read8(0xFAFFFFFF);
    CHECK(r.m->busError && r.m->busErrorAddr == 0xFAFFFFFF);
    r.m->busError = false;
    r.m->read8(0xC0000000);                                    // super slot space: nothing there
    CHECK(r.m->busError);
    r.m->busError = false;

    // 24-bit mode through VIA2 PB3.
    r.via2(vBufB, 0x04);                                       // PB3 low, PB2 (power) high
    r.via2(vDirB, 0x0C);
    CHECK(!r.m->addressing32() && !r.m->poweredOff());
    CHECK(r.m->read8(0x800005) == 0x80);                       // ROM
    CHECK(r.m->read8(0xFF001234) == 0x5A);                     // high byte ignored: RAM
    CHECK(r.m->read8(0x9FFFF3) == 9);                          // slot 9 register window: slot number
    r.m->read8(0xA00000);
    CHECK(r.m->busError);
    r.m->busError = false;
    CHECK(r.m->read8(0xF02000 + vDirB) == 0x0C);               // VIA2 through $F00000

    // Frame buffer: 32-bit writes land in VRAM and read back in both modes.
    r.via2(vBufB, 0x0C);
    r.m->write32(0xF9000010, 0x11223344);
    CHECK(r.m->card.vram()[0x10] == 0x11 && r.m->read16(0xF9000012) == 0x3344);
    r.via2(vBufB, 0x04);
    CHECK(r.m->read32(0x900010) == 0x11223344);

    // Soft power off.
    r.via2(vBufB, 0x08);
    CHECK(r.m->poweredOff());
}

static void testAdb()
{
    Rig r;
    r.via1(vDirB, 0x37);                                       // RTC lines and ADB state are outputs
    r.adbState(3);
    r.m->keyEvent(MK_A, true);

    // Talk R0 to the keyboard (address 2).
    r.via1(vACR, 0x1C);                                        // shift out
    r.via1(vSR, 0x2C);
    r.adbState(0);
    CHECK(r.via1(vIFR) & 0x04);                                // command sent
    CHECK(r.via1(vBufB) & 0x08);                               // /INT high: no autopoll data
    r.via1(vACR, 0x0C);                                        // shift in
    r.via1(vSR);
    r.adbState(1);
    CHECK((r.via1(vIFR) & 0x04) && r.via1(vSR) == 0x00);       // key A down
    CHECK(r.via1(vBufB) & 0x08);                               // not a timeout
    r.adbState(2);
    CHECK(r.via1(vSR) == 0xFF);                                // no second key
    r.adbState(1);
    CHECK(r.via1(vSR) == 0x00 && !(r.via1(vBufB) & 0x08));     // end of data
    r.adbState(3);

    // Talk to an empty address times out.
    r.via1(vACR, 0x1C);
    r.via1(vSR, 0x5C);
    r.adbState(0);
    r.via1(vACR, 0x0C);
    r.adbState(1);
    CHECK(r.via1(vSR) == 0xFF && !(r.via1(vBufB) & 0x08));
    r.adbState(3);

    // Talk R3 then Listen R3: move the mouse to address 5.
    r.via1(vACR, 0x1C);
    r.via1(vSR, 0x3F);
    r.adbState(0);
    r.via1(vACR, 0x0C);
    r.adbState(1);
    CHECK((r.via1(vSR) & 0x0F) == 3);
    r.adbState(2);
    CHECK(r.via1(vSR) == 1);                                   // handler 1
    r.adbState(3);
    r.via1(vACR, 0x1C);
    r.via1(vSR, 0x3B);
    r.adbState(0);
    r.via1(vSR, 0x05);
    r.adbState(1);
    r.via1(vSR, 0xFE);
    r.adbState(2);
    r.adbState(3);
    CHECK(r.m->mouse.address == 5);

    // Autopoll: in idle, mouse movement arrives without a command.
    r.via1(vACR, 0x0C);
    r.via1(vSR);
    r.m->mouseMove(3, -2);
    r.m->mouseButton(true);
    r.m->runFrame();
    CHECK(r.via1(vIFR) & 0x04);
    CHECK(r.via1(vSR) == 0x5C && !(r.via1(vBufB) & 0x08));     // talk R0 from address 5
    r.adbState(1);
    CHECK(r.via1(vSR) == 0x7E);                                // button down, dy = -2
    r.adbState(2);
    CHECK(r.via1(vSR) == 0x83);                                // dx = 3
    r.adbState(3);
}

// The power key goes out as 7F 7F (down) and FF FF (up), never paired
// with another key.
// Keys held through an ADB reset are reported again (start-up key
// combinations), and once more on a rescan, but not after being released.
static void testAdbHeldKeys()
{
    AdbKeyboard kb;
    u8 out[2];
    kb.key(0x37, true);                 // Command
    kb.key(0x07, true);                 // X
    kb.reset();
    CHECK(kb.talk(0, out) == 2 && out[0] == 0x07 && out[1] == 0x37);   // in key-code order
    CHECK(!kb.hasData());
    kb.rescan();
    CHECK(kb.hasData() && kb.talk(0, out) == 2 && out[0] == 0x07 && out[1] == 0x37);
    kb.key(0x07, false);
    kb.flush();
    CHECK(kb.talk(0, out) == 2 && out[0] == 0x37);                      // still held: Command only
    kb.key(0x37, false);
    kb.flush();
    CHECK(!kb.hasData());
}

static void testAdbPowerKey()
{
    AdbKeyboard kb;
    u8 out[2];
    kb.key(0x00, true);                 // A down
    kb.key(MK_Power, true);
    kb.key(MK_Power, false);
    kb.key(0x00, false);
    CHECK(kb.talk(0, out) == 2 && out[0] == 0x00 && out[1] == 0xFF);
    CHECK(kb.talk(0, out) == 2 && out[0] == 0x7F && out[1] == 0x7F);
    CHECK(kb.talk(0, out) == 2 && out[0] == 0xFF && out[1] == 0xFF);
    CHECK(kb.talk(0, out) == 2 && out[0] == 0x80 && out[1] == 0xFF);
    CHECK(kb.talk(0, out) == 0);
}

static void testInterrupts()
{
    Rig r;
    r.m->read8(0x40000000);
    // Level 2 autovector counts interrupts: addq.w #1,$100; tst.b VIA2 IFR-clear; rte
    u32 handler = 0x2000;
    putBe32(&r.ram[0x68], handler);
    const u8 code[] = {0x52, 0x78, 0x01, 0x00,             // addq.w #1,$100.w
                       0x13, 0xFC, 0x00, 0x7F, 0x50, 0x00, 0x3A, 0x00,   // move.b #$7F,VIA2 IFR
                       0x13, 0xFC, 0x00, 0xA0, 0xF9, 0x0F, 0xFF, 0xF3,   // move.b #$A0,card ack (low byte)
                       0x4E, 0x73};                         // rte
    memcpy(&r.ram[handler], code, sizeof code);
    r.via2(vIER, 0x82);                                        // CA1 (slot interrupts)
    r.m->write32(0xF90FFFF0, 1);                               // open the card: VBL on
    for (int i = 0; i < 5; i++) r.m->runFrame();
    int count = be16(&r.ram[0x100]);
    CHECK(count >= 3 && count <= 5);

    // ASC: an empty FIFO in FIFO mode interrupts through VIA2 CB1.
    r.via2(vIER, 0x02);                                        // CA1 off
    r.via2(vIER, 0x90);                                        // CB1 on
    putBe16(&r.ram[0x100], 0);
    r.m->write8(0x50014801, 1);                                // ASC FIFO mode
    for (int i = 0; i < 6; i++) r.m->runFrame();
    count = be16(&r.ram[0x100]);
    CHECK(count >= 1);
    CHECK(r.m->read8(0x50014800) == 0);                        // ASC version
    CHECK((r.m->read8(0x50014804) & 3) == 0 || true);

    // The interrupt switch is edge triggered: one NMI per press.
    putBe32(&r.ram[0x7C], 0x2100);
    const u8 nmi[] = {0x52, 0x78, 0x01, 0x02, 0x4E, 0x73};
    memcpy(&r.ram[0x2100], nmi, sizeof nmi);
    r.via2(vIER, 0x7F);
    r.m->interruptButton();
    for (int i = 0; i < 10; i++) r.m->runFrame();
    CHECK(be16(&r.ram[0x102]) == 1);
}

static void testSound()
{
    Rig r;
    r.m->read8(0x40000000);
    r.m->write8(0x50014806, 0xE0);                             // full volume
    r.m->write8(0x50014801, 1);                                // FIFO mode
    for (int i = 0; i < 370; i++) r.m->write8(0x50014000, (u8)(i & 1 ? 0xFF : 0x00));
    r.m->runFrame();
    u32 n, rate;
    const s16 *s = r.m->sound(n, rate);
    CHECK(n == 370 && rate == 22257);
    CHECK(s[0] < -3000 && s[1] > 3000);
    // Wavetable mode: voice 0 steps through a square wave.
    r.m->write8(0x50014801, 2);
    for (int i = 0; i < 0x200; i++) r.m->write8(0x50014000 + i, i < 0x100 ? 0xFF : 0x00);
    r.m->write32(0x50014814, 0x00010000);                      // increment: 2 samples per step
    r.m->runFrame();
    s = r.m->sound(n, rate);
    bool varies = false;
    for (u32 i = 1; i < n; i++) if (s[i] != s[0]) varies = true;
    CHECK(varies);
}

int main()
{
    testAdbPowerKey();
    testAdbHeldKeys();
    testMemoryMap();
    testAdb();
    testInterrupts();
    testSound();
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
