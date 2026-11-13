// The JIT against the interpreter: random instructions from random states,
// run once by each; every register, flag, cycle count and memory byte must
// agree.  Only meaningful on an AArch64 host (run it under qemu-aarch64).
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "../src/core/m68k.h"
#include "../src/core/jit.h"

using namespace macemu;

static int failures = 0, checks = 0;

enum { kRam = 256 * 1024, kIo = 0xF00000 };

// RAM at 0 (host pages: the TLB), and an I/O window at $F00000 that the
// CPU reaches only through the bus (slow accesses).
struct TestBus : Bus {
    std::vector<u8> ram;
    u32 ioWrites = 0, ioSum = 0;
    TestBus() : ram(kRam) {}
    u8 r(u32 a) { return a < kRam ? ram[a] : a >= kIo && a < kIo + 0x1000 ? (u8)(a * 7) : 0xFF; }
    void w(u32 a, u8 v) { if (a < kRam) ram[a] = v; else if (a >= kIo && a < kIo + 0x1000) { ioWrites++; ioSum = ioSum * 31 + a + v; } }
    u8 read8(u32 a) override { return r(a); }
    u16 read16(u32 a) override { return (u16)(r(a) << 8 | r(a + 1)); }
    u32 read32(u32 a) override { return (u32)read16(a) << 16 | read16(a + 2); }
    void write8(u32 a, u8 v) override { w(a, v); }
    void write16(u32 a, u16 v) override { w(a, (u8)(v >> 8)); w(a + 1, (u8)v); }
    void write32(u32 a, u32 v) override { write16(a, (u16)(v >> 16)); write16(a + 2, (u16)v); }
    u8 *hostPage(u32 a, bool &writable) override { writable = true; return a < kRam ? &ram[a & ~0xFFFu] : 0; }
};

static u32 rnd() { return (u32)rand() << 16 ^ (u32)rand(); }

struct State {
    u32 d[8], a[8], usp, ssp;
    u16 sr;
    std::vector<u8> ext;            // the instruction's extension words
};

static const u32 kCode = 0x10000;

// Addresses: mostly RAM (some at page ends), some I/O.
static u32 addr()
{
    switch (rand() % 8) {
    case 0: return kIo + (rnd() & 0xFFF);
    case 1: return 0x20000 + (rand() % 16) * 0x1000 + 0xFFC + (rand() % 4);   // page crossings
    default: return 0x20000 + (rnd() % 0x10000);
    }
}

static void setup(M68K &c, TestBus &b, const State &s, u16 op, const std::vector<u8> &mem)
{
    b.ram = mem;
    b.ioWrites = b.ioSum = 0;
    u8 *code = &b.ram[kCode];
    code[0] = (u8)(op >> 8); code[1] = (u8)op;
    for (size_t i = 0; i < s.ext.size(); i++) code[2 + i] = s.ext[i];
    for (u32 i = 2 + (u32)s.ext.size(); i < 0x200; i += 2) { code[i] = 0x4E; code[i + 1] = 0x71; }   // NOPs
    memcpy(c.d, s.d, sizeof c.d);
    memcpy(c.a, s.a, sizeof c.a);
    c.setSR(0x2700);
    c.setUSP(s.usp);
    c.setSSP(s.ssp);
    c.setSR(s.sr);
    c.pc = kCode;
    c.stopped = false;
    c.setIRQ(0);
}

static bool same(M68K &x, TestBus &bx, M68K &y, TestBus &by, u16 op, const State &s)
{
    checks++;
    bool ok = !memcmp(x.d, y.d, sizeof x.d) && !memcmp(x.a, y.a, sizeof x.a) && x.pc == y.pc
              && x.getSR() == y.getSR() && x.totalCycles == y.totalCycles && x.cyc == y.cyc
              && x.getUSP() == y.getUSP() && x.getSSP() == y.getSSP()
              && bx.ram == by.ram && bx.ioWrites == by.ioWrites && bx.ioSum == by.ioSum;
    if (!ok && failures++ < 20) {
        printf("FAIL op %04X ext", op);
        for (u8 v : s.ext) printf(" %02X", v);
        printf(" sr %04X\n  interp: pc %08X sr %04X cyc %llu/%d", s.sr, x.pc, x.getSR(), (unsigned long long)x.totalCycles, x.cyc);
        for (int i = 0; i < 8; i++) printf(" d%d=%08X", i, x.d[i]);
        for (int i = 0; i < 8; i++) printf(" a%d=%08X", i, x.a[i]);
        printf("\n  jit:    pc %08X sr %04X cyc %llu/%d", y.pc, y.getSR(), (unsigned long long)y.totalCycles, y.cyc);
        for (int i = 0; i < 8; i++) printf(" d%d=%08X", i, y.d[i]);
        for (int i = 0; i < 8; i++) printf(" a%d=%08X", i, y.a[i]);
        printf("\n  memory %s, io %u/%u\n", bx.ram == by.ram ? "same" : "differs", bx.ioWrites, by.ioWrites);
    }
    return ok;
}

int main(int argc, char **argv)
{
    if (!jitAvailable()) { puts("no JIT on this host: skipped"); return 0; }
    int perOp = argc > 1 ? atoi(argv[1]) : 1;
    int stride = argc > 2 ? atoi(argv[2]) : 1;          // every stride-th opcode
    srand(12345);
    TestBus bi, bj;
    M68K ci(&bi), cj(&bj);
    ci.setModel(M68K::MC68040); cj.setModel(M68K::MC68040);
    ci.hardReset(); cj.hardReset();
    Jit jit;
    if (!jit.init()) { puts("JIT init failed"); return 1; }
    cj.jit = &jit;

    std::vector<u8> mem(kRam);
    int ops = 0, inBlocks = 0;
    u32 opFrom = argc > 3 ? (u32)strtoul(argv[3], 0, 16) : 0, opTo = argc > 4 ? (u32)strtoul(argv[4], 0, 16) : 0xFFFF;
    for (u32 op = opFrom; op <= opTo; op++) {
        if (!Jit::hasOwnCode((u16)op) || (op / 7) % stride) continue;
        ops++;
        for (int k = 0; k < perOp; k++) {
            srand(op * 977 + (u32)k);                   // each case on its own (to rerun one)
            if (ops % 64 == 1 || opFrom == opTo) { srand(op / 64); for (auto &v : mem) v = (u8)rand(); srand(op * 977 + (u32)k); }
            State s;
            for (int i = 0; i < 8; i++) { s.d[i] = rnd(); s.a[i] = addr(); }
            if (rand() % 4 == 0) s.d[rand() % 8] = 0;
            s.usp = addr() & ~1u; s.ssp = addr() & ~1u;
            s.sr = (u16)((rand() % 2 ? 0x2700 : 0x0000) | (rand() & 0x1F));
            // Extension words: displacements that keep addresses near RAM.
            for (int w = 0; w < 4; w++) {
                u16 v = (u16)(rand() % 3 == 0 ? rnd() : (rnd() & 0x3FF) - 0x200);
                if (rand() % 6 == 0) v = (u16)(0x2000 + (rnd() & 0xFFF));            // abs.W into RAM
                s.ext.push_back((u8)(v >> 8)); s.ext.push_back((u8)v);
            }
            // The JIT: first runs fill the TLB, record and translate ...
            for (int w = 0; w < 2; w++) { setup(cj, bj, s, (u16)op, mem); cj.run(64, ~0ull); }
            // ... the second runs it; the interpreter runs the same.
            setup(cj, bj, s, (u16)op, mem);
            setup(ci, bi, s, (u16)op, mem);
            cj.totalCycles = ci.totalCycles = 0;
            u64 before = jit.jitInstructions;
            u32 tj = cj.run(1, ~0ull), ti = ci.run(1, ~0ull);
            if (jit.jitInstructions != before) inBlocks++;
            same(ci, bi, cj, bj, (u16)op, s);
            if (tj != ti) { failures++; printf("FAIL op %04X: run() returned %u, interpreter %u\n", op, tj, ti); }
        }
    }
    // Code that changes itself within a block: MOVE.W D1,$00010008 puts
    // D1 where the fourth instruction is.  Recorded with D1 = NOP, run
    // with D1 = MOVEQ #5,D0: the interpreter then runs the MOVEQ.
    {
        static const u8 prog[] = {0x33, 0xC1, 0x00, 0x01, 0x00, 0x08, 0x4E, 0x71, 0x4E, 0x71};
        State s = {};
        s.sr = 0x2700; s.ssp = 0x30000; s.usp = 0x31000;
        s.ext.assign(prog + 2, prog + sizeof prog);
        for (auto &v : mem) v = 0;
        s.d[1] = 0x4E71;
        setup(cj, bj, s, 0x33C1, mem); cj.run(1, ~0ull);     // (fills the TLB: the block then starts here)
        for (int w = 0; w < 2; w++) { setup(cj, bj, s, 0x33C1, mem); cj.run(64, ~0ull); }
        u64 before = jit.jitInstructions;
        s.d[1] = 0x7005;
        setup(cj, bj, s, 0x33C1, mem);
        setup(ci, bi, s, 0x33C1, mem);
        cj.totalCycles = ci.totalCycles = 0;
        cj.run(8, ~0ull);
        ci.run(8, ~0ull);
        if (jit.jitInstructions == before) { failures++; puts("FAIL self-modifying code: no block ran"); }
        same(ci, bi, cj, bj, 0x33C1, s);
        if (ci.d[0] != 5 || cj.d[0] != 5) { failures++; printf("FAIL self-modifying code: d0 %08X / %08X\n", ci.d[0], cj.d[0]); }
    }

    printf("%d opcodes with code of their own; %u blocks, %u instructions translated\n", ops, jit.blocks, jit.nativeInstructions);
    printf("%d of the runs were in a block\n", inBlocks);
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
