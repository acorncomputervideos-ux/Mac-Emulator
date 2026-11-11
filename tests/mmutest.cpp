// MC68040 MMU: table walks, faults, transparent translation, the ATC, and
// the CPU instructions that drive it.
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <vector>
#include "../src/core/m68k.h"
#include "../src/core/mmu040.h"

using namespace macemu;

static int failures = 0, checks = 0;
#define CHECK(cond) do { checks++; if (!(cond)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

struct Phys : Bus {
    std::vector<u8> mem = std::vector<u8>(16 << 20);
    u8 read8(u32 a) override { return mem[a & 0xFFFFFF]; }
    u16 read16(u32 a) override { return (u16)(read8(a) << 8 | read8(a + 1)); }
    void write8(u32 a, u8 v) override { mem[a & 0xFFFFFF] = v; }
    void write16(u32 a, u16 v) override { write8(a, (u8)(v >> 8)); write8(a + 1, (u8)v); }
    u32 l(u32 a) { return be32(&mem[a]); }
    void l(u32 a, u32 v) { putBe32(&mem[a], v); }
};

enum { ROOT = 0x10000, PTR = 0x10200, PAGES = 0x10400, PAGES8K = 0x10600 };

static void tables(Phys &p)
{
    p.l(ROOT + 0, PTR | 3);                             // logical 0x00000000-0x01FFFFFF
    p.l(PTR + 0, PAGES | 3);                            // 0x00000000-0x0003FFFF
    p.l(PAGES + 5 * 4, 0x200000 | 1);                   // page 5 -> 0x200000
    p.l(PAGES + 6 * 4, 0x201000 | 1 | 4);               // page 6 write protected
    p.l(PAGES + 7 * 4, 0x202000 | 1 | 0x80);            // page 7 supervisor only
    p.l(PAGES + 8 * 4, 0x30000 | 2);                    // page 8 indirect
    p.l(0x30000, 0x203000 | 1);
    p.l(PAGES + 9 * 4, 0);                              // page 9 invalid
}

static void testWalks()
{
    Phys p;
    Mmu040 m(p);
    bool f = false;
    CHECK(!m.enabled());
    tables(p);
    u32 v = 0x8000;
    m.control(0x807, (v = ROOT), true);
    m.control(0x806, (v = ROOT), true);
    m.control(0x003, (v = 0x8000), true);
    CHECK(m.enabled());

    CHECK(m.translate(0x5123, false, true, false, f) == 0x200123 && !f);
    CHECK((p.l(ROOT) & 8) && (p.l(PTR) & 8) && (p.l(PAGES + 20) & 8));   // used bits
    CHECK(!(p.l(PAGES + 20) & 0x10));                   // not modified yet
    CHECK(m.translate(0x5124, true, true, false, f) == 0x200124 && !f);
    CHECK(p.l(PAGES + 20) & 0x10);                      // modified after a write

    f = false; m.translate(0x6000, false, true, false, f); CHECK(!f);
    f = false; m.translate(0x6000, true, true, false, f); CHECK(f);   // write protected
    CHECK(!(p.l(PAGES + 24) & 0x10));
    f = false; m.translate(0x7000, false, true, false, f); CHECK(!f);
    f = false; m.translate(0x7000, false, false, false, f); CHECK(f); // supervisor only
    f = false; CHECK(m.translate(0x8010, false, true, false, f) == 0x203010 && !f);   // indirect
    f = false; m.translate(0x9000, false, true, false, f); CHECK(f);  // invalid
    f = false; m.translate(0x02000000, false, true, false, f); CHECK(f);   // root entry 1 invalid
    f = false; CHECK(m.translate(0x5000, false, false, false, f) == 0x200000 && !f);   // user mode via URP

    // The ATC keeps old translations until PFLUSH.
    p.l(PAGES + 20, 0x210000 | 1);
    f = false;
    CHECK(m.translate(0x5000, false, true, false, f) == 0x200000);
    m.flush();
    CHECK(m.translate(0x5000, false, true, false, f) == 0x210000);

    // Transparent translation: DTT0 maps $50xxxxxx-$5Fxxxxxx straight through.
    v = 0x500FE040;                                     // base $50, mask $0F, E, S=ignore, cache mode
    m.control(0x006, v, true);
    f = false;
    CHECK(m.translate(0x5ABCDEF0, true, false, false, f) == 0x5ABCDEF0 && !f);
    CHECK(m.translate(0x5ABCDEF0, false, true, true, f) != 0x5ABCDEF0 || f);   // instruction side not covered
    v = 0x500FA044;                                     // supervisor only, write protected
    m.control(0x007, v, true);
    v = 0;
    m.control(0x006, v, true);
    f = false; m.translate(0x51000000, true, true, false, f); CHECK(f);

    // 8K pages: page index is bits 17..13.
    tables(p);
    p.l(PTR + 4, PAGES8K | 3);                          // 0x00040000-0x0007FFFF
    p.l(PAGES8K + 3 * 4, 0x400000 | 1);
    m.control(0x003, (v = 0xC000), true);
    f = false;
    CHECK(m.translate(0x40000 + 3 * 0x2000 + 0x1ABC, false, true, false, f) == 0x401ABC && !f);
    u32 tcBack = 0;
    m.control(0x003, tcBack, false);
    CHECK(tcBack == 0xC000);
}

static void testCpu()
{
    Phys p;
    Mmu040 m(p);
    M68K cpu(&p);
    cpu.setModel(M68K::MC68040);
    cpu.attachMmu(&m, &m);
    tables(p);
    // Identity-map the first 64K (code, stack and vectors) and $1000000 as data.
    for (u32 i = 0; i < 16; i++) p.l(PAGES + i * 4 + 0x100, 0);
    for (u32 i = 0; i < 4; i++) if (i != 5) {}
    p.l(PAGES + 0 * 4, 0x000000 | 1);
    p.l(PAGES + 1 * 4, 0x001000 | 1);
    p.l(PAGES + 2 * 4, 0x002000 | 1);
    p.l(PAGES + 3 * 4, 0x003000 | 1);
    p.l(PAGES + 15 * 4, 0x00F000 | 1);                  // stack page
    putBe32(&p.mem[0x200000 + 0x10], 0xCAFEF00D);       // behind logical page 5
    putBe32(&p.mem[2 * 4], 0x3000);                     // access error vector
    const u8 code[] = {
        0x20, 0x3C, 0x00, 0x01, 0x00, 0x00,             // move.l #ROOT,d0
        0x4E, 0x7B, 0x08, 0x07,                         // movec d0,srp
        0x4E, 0x7B, 0x08, 0x06,                         // movec d0,urp
        0x20, 0x3C, 0x00, 0x00, 0x80, 0x00,             // move.l #$8000,d0
        0x4E, 0x7B, 0x00, 0x03,                         // movec d0,tc
        0x41, 0xF8, 0x50, 0x10,                         // lea $5010.w,a0
        0x22, 0x10,                                     // move.l (a0),d1
        0xF5, 0x68,                                     // ptestr (a0)
        0x4E, 0x7A, 0x28, 0x05,                         // movec mmusr,d2
        0xF5, 0x18,                                     // pflusha
        0x41, 0xF8, 0x40, 0x00,                         // lea $4000.w,a0  (invalid page)
        0x26, 0x10,                                     // move.l (a0),d3  -> access error
        0x4E, 0x71,
    };
    memcpy(&p.mem[0x1000], code, sizeof code);
    cpu.setSR(0x2700);
    cpu.a[7] = 0xF800;
    cpu.dfc = 5;
    cpu.pc = 0x1000;
    for (int i = 0; i < 40 && cpu.pc != 0x3000; i++) cpu.step();
    CHECK(cpu.d[1] == 0xCAFEF00D);
    CHECK((cpu.d[2] & 0xFFFFF000) == 0x200000 && (cpu.d[2] & 1));
    CHECK(cpu.pc == 0x3000);                            // access error taken
    u32 sp = cpu.a[7];
    CHECK(be16(&p.mem[sp + 6]) == 0x7008);              // format 7, vector 2
    CHECK(be32(&p.mem[sp + 20]) == 0x4000);             // fault address
}

int main()
{
    testWalks();
    testCpu();
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
