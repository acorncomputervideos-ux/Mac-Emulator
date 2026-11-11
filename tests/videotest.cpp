// The NuBus video card: its declaration ROM is parsed the way the Slot
// Manager does, and the 68k driver from that ROM runs on the emulated CPU
// against a minimal fake Mac OS (NewPtr, SIntInstall, IODone, VBL tasks).
#include <cstdio>
#include <cstring>
#include <vector>
#include "../src/core/m68k.h"
#include "../src/core/nubusvideo.h"
#include "../src/core/videodrv.h"

using namespace macemu;

static int failures = 0, checks = 0;
#define CHECK(cond) do { checks++; if (!(cond)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

enum {
    kRam = 1 << 20, kSlot = 9, kSlotBase = 0xF9000000,
    kTrapHandler = 0x700, kIODone = 0x710, kVBLTask = 0x720, kReturn = 0x730,
    kDriver = 0x20000, kDCE = 0x30000, kPB = 0x31000, kParam = 0x32000, kTable = 0x33000, kHeap = 0x40000,
};

struct Env;

struct TestMem : GuestMemory {
    std::vector<u8> &ram;
    explicit TestMem(std::vector<u8> &r) : ram(r) {}
    u8 peek(u32 a) override { return a < kRam ? ram[a] : 0; }
    void poke(u32 a, u8 v) override { if (a < kRam) ram[a] = v; }
};

struct Env : Bus {
    std::vector<u8> ram = std::vector<u8>(kRam);
    TestMem mem{ram};
    NubusVideo card{kSlot, 640, 480, mem};
    M68K cpu{this};
    u32 heap = kHeap;
    int sintInstalls = 0, sintRemoves = 0, disposes = 0, vblTasks = 0;
    u32 sqElement = 0, sintSlot = 0, vblSlot = 0;

    u8 read8(u32 a) override {
        if ((a >> 24) == 0xF0 + kSlot) return card.read(a & 0xFFFFFF);
        return a < kRam ? ram[a] : 0xFF;
    }
    u16 read16(u32 a) override { return (u16)((read8(a) << 8) | read8(a + 1)); }
    void write8(u32 a, u8 v) override {
        if ((a >> 24) == 0xF0 + kSlot) { card.write(a & 0xFFFFFF, v); return; }
        if (a < kRam) ram[a] = v;
    }
    void write16(u32 a, u16 v) override { write8(a, (u8)(v >> 8)); write8(a + 1, (u8)v); }

    Env() {
        cpu.setModel(M68K::MC68020);
        cpu.setSR(0x2700);
        cpu.a[7] = 0x10000;
        putBe32(&ram[10 * 4], kTrapHandler);            // line A vector
        putBe32(&ram[0x8FC], kIODone);                  // JIODone
        putBe32(&ram[0xD28], kVBLTask);                 // JVBLTask
        memcpy(&ram[kDriver], kVideoDriver, sizeof kVideoDriver);
        ram[kDCE + 40] = kSlot;                         // dCtlSlot
        putBe32(&ram[kDCE + 42], kSlotBase);            // dCtlDevBase
    }

    // The A-line handler: emulate the few traps the driver uses.
    void trap() {
        u32 sp = cpu.a[7];
        u32 trapPC = (u32)read16(sp + 2) << 16 | read16(sp + 4);
        u16 op = read16(trapPC);
        switch (op) {
        case 0xA71E: {                                  // NewPtr SYS CLEAR
            u32 size = cpu.d[0];
            cpu.a[0] = heap;
            memset(&ram[heap], 0, size);
            heap += (size + 15) & ~15u;
            cpu.d[0] = 0;
            break;
        }
        case 0xA075: sintInstalls++; sqElement = cpu.a[0]; sintSlot = cpu.d[0]; cpu.d[0] = 0; break;
        case 0xA076: sintRemoves++; cpu.d[0] = 0; break;
        case 0xA01F: disposes++; cpu.d[0] = 0; break;
        default: printf("unexpected trap %04X\n", op); break;
        }
        cpu.a[7] = sp + 8;                              // format 0 frame
        cpu.pc = trapPC + 2;
    }

    // Call a driver routine; returns d0.  `viaIODone` tells which exit was taken.
    u32 call(u32 entry, u32 a1, u32 a0, bool *viaIODone = nullptr) {
        cpu.a[0] = a0;
        cpu.a[1] = a1;
        cpu.a[7] -= 4;
        write16(cpu.a[7], kReturn >> 16); write16(cpu.a[7] + 2, kReturn & 0xFFFF);
        cpu.pc = entry;
        for (int n = 0; n < 10000; n++) {
            if (cpu.pc == kTrapHandler) { trap(); continue; }
            if (cpu.pc == kReturn) { if (viaIODone) *viaIODone = false; return cpu.d[0]; }
            if (cpu.pc == kIODone) { cpu.a[7] += 4; if (viaIODone) *viaIODone = true; return cpu.d[0]; }
            if (cpu.pc == kVBLTask) {                   // DoVBLTask(slot)
                vblTasks++;
                vblSlot = cpu.d[0];
                u32 ret = (u32)read16(cpu.a[7]) << 16 | read16(cpu.a[7] + 2);
                cpu.a[7] += 4;
                cpu.pc = ret;
                continue;
            }
            cpu.step();
        }
        printf("driver call did not return (pc=%08X)\n", cpu.pc);
        return 0xDEAD;
    }
    u32 entry(int which) { return kDriver + be16(&ram[kDriver + 8 + 2 * which]); }   // 0 open .. 4 close
    u32 control(u16 code, u32 param, bool immediate = false, bool *io = nullptr) {
        memset(&ram[kPB], 0, 64);
        if (immediate) putBe16(&ram[kPB + 6], 0x0200);
        putBe16(&ram[kPB + 26], code);
        putBe32(&ram[kPB + 28], param);
        return call(entry(2), kDCE, kPB, io);
    }
    u32 status(u16 code, u32 param, bool *io = nullptr) {
        memset(&ram[kPB], 0, 64);
        putBe16(&ram[kPB + 26], code);
        putBe32(&ram[kPB + 28], param);
        return call(entry(3), kDCE, kPB, io);
    }
};

// ------------------------------------------------------------ declaration ROM

static u32 romByte(NubusVideo &c, u32 addr) { return c.read(addr & 0xFFFFFF); }
static u32 romLong(NubusVideo &c, u32 addr) { return romByte(c, addr) << 24 | romByte(c, addr + 1) << 16 | romByte(c, addr + 2) << 8 | romByte(c, addr + 3); }
static u32 target(NubusVideo &c, u32 entry) { s32 off = (s32)(romLong(c, entry) << 8) >> 8; return entry + (u32)off; }
static u32 find(NubusVideo &c, u32 list, u8 id) {
    for (u32 e = list; ; e += 4) {
        u8 i = (u8)romByte(c, e);
        if (i == 0xFF) return 0;
        if (i == id) return e;
    }
}

static void testDeclRom()
{
    std::vector<u8> ram(64);
    struct Null : GuestMemory { u8 peek(u32) override { return 0; } void poke(u32, u8) override {} } mem;
    NubusVideo card(kSlot, 640, 480, mem);
    u32 top = 0xFFFFFF;
    CHECK(romByte(card, top) == 0x0F);                  // byte lanes
    u32 fb = top - 19;
    CHECK(romLong(card, fb + 14) == 0x5A932BC7);
    CHECK(romByte(card, fb + 12) == 1 && romByte(card, fb + 13) == 1);
    u32 length = romLong(card, fb + 4);
    CHECK(length == NubusVideo::kRomSize);
    u32 crc = 0;
    for (u32 a = top - length + 1; a <= top; a++) {
        u32 b = (a >= fb + 8 && a < fb + 12) ? 0 : romByte(card, a);
        crc = ((crc << 1) | (crc >> 31)) + b;
    }
    CHECK(crc == romLong(card, fb + 8));

    u32 dir = target(card, fb);
    u32 board = find(card, dir, 1), video = find(card, dir, 0x80);
    CHECK(board && video);
    u32 boardList = target(card, board);
    u32 typeEntry = find(card, boardList, 1);
    CHECK(typeEntry && romLong(card, target(card, typeEntry)) == 0x00010000);
    CHECK(find(card, boardList, 32) && (romLong(card, find(card, boardList, 32)) & 0xFFFF) == 0x0E3E);
    u32 videoList = target(card, video);
    u32 vt = target(card, find(card, videoList, 1));
    CHECK(romLong(card, vt) == 0x00030001 && romLong(card, vt + 4) == 0x0001BEEF);
    u32 name = target(card, find(card, videoList, 2));
    char buf[32] = {0};
    for (int i = 0; i < 31 && romByte(card, name + i); i++) buf[i] = (char)romByte(card, name + i);
    CHECK(!strcmp(buf, "Display_Video_Emu"));
    CHECK((romLong(card, find(card, videoList, 7)) & 0xFFFFFF) == 2);
    u32 drvrDir = target(card, find(card, videoList, 4));
    u32 block = target(card, find(card, drvrDir, 2));
    CHECK(romLong(card, block) == 4 + sizeof kVideoDriver);
    bool same = true;
    for (u32 i = 0; i < sizeof kVideoDriver; i++) if (romByte(card, block + 4 + i) != kVideoDriver[i]) same = false;
    CHECK(same);
    for (int m = 0; m < 4; m++) {
        u32 modeEntry = find(card, videoList, (u8)(0x80 + m));
        CHECK(modeEntry != 0);
        u32 modeList = target(card, modeEntry);
        u32 vp = target(card, find(card, modeList, 1)) + 4;
        int bits = 1 << m;
        CHECK((romLong(card, vp + 4) >> 16) == (u32)(640 * bits / 8));          // rowBytes
        CHECK(romLong(card, vp + 6) == 0 && romLong(card, vp + 10) == (480u << 16 | 640));   // bounds
        CHECK((romLong(card, vp + 32) >> 16) == (u32)bits);                     // pixel size
        CHECK((romLong(card, find(card, modeList, 3)) & 0xFFFF) == 1);           // page count
    }
}

// ------------------------------------------------------------ driver

static void testDriver()
{
    static Env e;
    VideoFrame f;
    CHECK(!e.card.video(f));                            // dark until the driver opens

    CHECK(e.call(e.entry(0), kDCE, kPB) == 0);          // Open
    CHECK(e.sintInstalls == 1 && e.sintSlot == kSlot);
    u32 sq = e.sqElement;
    CHECK(be16(&e.ram[sq + 4]) == 6);
    CHECK(be32(&e.ram[sq + 8]) == kDriver + kVideoDriverVBLHandler);
    CHECK(be32(&e.ram[sq + 12]) == kSlotBase + 0xFFFF0);
    CHECK(be32(&e.ram[kDCE + 20]) == sq);               // kept in dCtlStorage
    CHECK(e.card.video(f) && f.depth == 1 && f.width == 640 && f.rowBytes == 80);
    CHECK(f.base[0] == 0xAA && f.base[80] == 0x55);     // gray pattern
    CHECK(f.clut[0] == 0xFFFFFF && f.clut[1] == 0x000000);

    // SetMode to 8 bits; the call completes through IODone.
    putBe16(&e.ram[kParam], 0x83);
    putBe16(&e.ram[kParam + 6], 0);
    bool io = false;
    CHECK(e.control(2, kParam, false, &io) == 0 && io);
    CHECK(be32(&e.ram[kParam + 8]) == kSlotBase);
    CHECK(e.card.video(f) && f.depth == 8 && f.rowBytes == 640);
    CHECK(f.clut[0] == 0xFFFFFF && f.clut[255] == 0);
    u32 version = f.clutVersion;

    // SetEntries: red, green at 0 and 1.
    putBe32(&e.ram[kParam], kTable);
    putBe16(&e.ram[kParam + 4], 0);
    putBe16(&e.ram[kParam + 6], 1);
    u8 spec[16] = {0, 0, 0xFF, 0xFF, 0, 0, 0, 0, 0, 1, 0, 0, 0xFF, 0xFF, 0, 0};
    memcpy(&e.ram[kTable], spec, 16);
    CHECK(e.control(3, kParam) == 0);
    CHECK(e.card.video(f) && f.clut[0] == 0xFF0000 && f.clut[1] == 0x00FF00 && f.clutVersion != version);
    // Index mode (csStart = -1): entry value names the index.
    putBe16(&e.ram[kParam + 4], 0xFFFF);
    putBe16(&e.ram[kParam + 6], 0);
    u8 spec2[8] = {0, 7, 0, 0, 0, 0, 0xFF, 0xFF};
    memcpy(&e.ram[kTable], spec2, 8);
    CHECK(e.control(3, kParam) == 0);
    CHECK(e.card.video(f) && f.clut[7] == 0x0000FF);
    // GetEntries reads them back.
    memset(&e.ram[kTable], 0, 16);
    putBe16(&e.ram[kParam + 4], 0);
    putBe16(&e.ram[kParam + 6], 1);
    CHECK(e.status(3, kParam) == 0);
    CHECK(be16(&e.ram[kTable + 2]) == 0xFFFF && be16(&e.ram[kTable + 4]) == 0 && be16(&e.ram[kTable + 12]) == 0xFFFF);
    // Gray mode turns colours into luminance.
    e.ram[kParam] = 1;
    CHECK(e.control(6, kParam) == 0);
    putBe32(&e.ram[kParam], kTable);
    putBe16(&e.ram[kParam + 4], 0);
    putBe16(&e.ram[kParam + 6], 0);
    memcpy(&e.ram[kTable], spec, 8);
    CHECK(e.control(3, kParam) == 0);
    CHECK(e.card.video(f) && f.clut[0] == 0x4C4C4C);
    CHECK(e.status(6, kParam) == 0 && e.ram[kParam] == 1);

    // Status calls.
    CHECK(e.status(2, kParam) == 0 && be16(&e.ram[kParam]) == 0x83 && be32(&e.ram[kParam + 8]) == kSlotBase);
    CHECK(e.status(4, kParam) == 0 && be16(&e.ram[kParam + 6]) == 1);
    CHECK(e.status(8, kParam) == 0);                    // GetGamma, answered by the driver
    u32 gamma = be32(&e.ram[kParam]);
    CHECK(gamma > kDriver && gamma < kDriver + sizeof kVideoDriver);
    CHECK(be16(&e.ram[gamma + 6]) == 1 && be16(&e.ram[gamma + 8]) == 256 && e.ram[gamma + 12 + 200] == 200);
    CHECK((s32)e.status(12, kParam) == -18);            // unsupported: statusErr
    CHECK((s32)e.control(99, kParam) == -17);           // unsupported: controlErr
    putBe16(&e.ram[kParam], 0x90);                      // bad mode
    CHECK((s32)e.control(2, kParam) == -17);

    // Immediate calls return with RTS, not through IODone.
    io = true;
    CHECK(e.status(2, kParam, &io) == 0 && io);         // queued: via IODone
    CHECK(e.control(1, kParam, true, &io) == 0 && !io);

    // VBL: the card interrupts, the handler acknowledges and runs the VBL tasks.
    e.card.vbl();
    CHECK(e.card.irq());
    u32 handler = be32(&e.ram[sq + 8]), parm = be32(&e.ram[sq + 12]);
    CHECK(e.call(handler, parm, 0) == 1);
    CHECK(!e.card.irq() && e.vblTasks == 1 && e.vblSlot == kSlot);
    // SetInterrupt off: no more VBL interrupts.
    e.ram[kParam] = 1;
    CHECK(e.control(7, kParam) == 0);
    e.card.vbl();
    CHECK(!e.card.irq());
    CHECK(e.status(7, kParam) == 0 && e.ram[kParam] == 1);

    // GrayPage at 8 bits.
    putBe16(&e.ram[kParam + 6], 0);
    CHECK(e.control(5, kParam) == 0);
    CHECK(e.card.video(f) && f.base[0] == 0x00 && f.base[1] == 0xFF && f.base[640] == 0xFF);

    // Close removes the interrupt handler.
    CHECK(e.call(e.entry(4), kDCE, kPB) == 0);
    CHECK(e.sintRemoves == 1 && e.disposes == 1 && be32(&e.ram[kDCE + 20]) == 0);
}

int main()
{
    testDeclRom();
    testDriver();
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
