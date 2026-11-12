// Motorola 68000 / 68020 / 68030 / 68040 interpreter.
//
// Timing model (68000): every bus access (one word, or one byte) costs 4
// clock cycles, and each instruction adds its documented internal cycles on
// top.  Because a 68000 fetches exactly as many words as the instruction is
// long, this reproduces the official timing tables closely without a
// prefetch queue.  The 68020 and later get a simpler estimate: a 32-bit bus
// and caches roughly halve (020/030) or quarter (040) the 68000 figure.
//
// Address errors (odd word access on a 68000) are not raised.  Bus errors
// signalled by the Bus are taken after the faulting instruction finishes.

#include "m68k.h"

namespace macemu {

static const u32 MASK[3] = {0xFF, 0xFFFF, 0xFFFFFFFF};
static const u32 MSB[3] = {0x80, 0x8000, 0x80000000};
static const int BITS[3] = {8, 16, 32};

enum {
    EA_DN = 1 << 0, EA_AN = 1 << 1, EA_IND = 1 << 2, EA_INC = 1 << 3, EA_DEC = 1 << 4,
    EA_D16 = 1 << 5, EA_IDX = 1 << 6, EA_ABSW = 1 << 7, EA_ABSL = 1 << 8,
    EA_PCD = 1 << 9, EA_PCI = 1 << 10, EA_IMM = 1 << 11,

    EA_ALL = 0xFFF,
    EA_DATA = EA_ALL & ~EA_AN,
    EA_ALT = 0x1FF,
    EA_DATAALT = 0x1FD,
    EA_MEMALT = 0x1FC,
    EA_CTRL = EA_IND | EA_D16 | EA_IDX | EA_ABSW | EA_ABSL | EA_PCD | EA_PCI,
    EA_CTRLALT = EA_IND | EA_D16 | EA_IDX | EA_ABSW | EA_ABSL,
};

static inline bool validEA(int mode, int reg, u32 mask)
{
    int i = mode < 7 ? mode : 7 + reg;
    return i < 12 && ((mask >> i) & 1);
}

#define NEED(mask) do { if (!validEA(mode, reg, (mask))) { illegal(); return; } } while (0)

static inline s32 sext(u32 v, int sz)
{
    return sz == 0 ? (s32)(s8)v : sz == 1 ? (s32)(s16)v : (s32)v;
}

M68K::M68K(Bus *bus) : bus(bus)
{
    fpu = mmuUnit = 0;
    mmu = 0;
    model = MC68000;
    for (int i = 0; i < 8; i++) d[i] = a[i] = 0;
    pc = 0;
    fC = fV = fZ = fN = fX = 0;
    fS = 1; fM = 0; fT = 0; imask = 7;
    irq = 0; nmiSeen = false; stopped = false; usp = isp = msp = 0; totalCycles = 0; cyc = 0;
    vbr = sfc = dfc = cacr = caar = 0;
    instPC = 0; opcode = 0; traceNext = false;
    tlbGeneration = ~0u;
    for (int i = 0; i < kTlbSize; i++) tlb[i].tag = 0;
    for (int i = 0; i < kCodeTlbSize; i++) codeTlb[i].tag = 0;
    buildTables();
    table = s_table000;
}

void M68K::hardReset()
{
    for (int i = 0; i < 8; i++) d[i] = a[i] = 0;
    fC = fV = fZ = fN = fX = 0;
    fS = 1; fM = 0; fT = 0; imask = 7;
    stopped = false; usp = isp = msp = 0;
    restartable = false; pageFaults = 0;
    vbr = sfc = dfc = cacr = caar = 0;
    if (fpu) fpu->reset();
    if (mmuUnit) mmuUnit->reset();
    a[7] = rd(0, 2);
    pc = rd(4, 2);
}

u16 M68K::getSR() const
{
    return (u16)((fT << 15) | (fS << 13) | (fM << 12) | (imask << 8) | (fX << 4) | (fN << 3) | (fZ << 2) | (fV << 1) | fC);
}

// Store A7 in the slot of the current stack, then load the new one.
void M68K::switchStack(u32 newS, u32 newM)
{
    if (newS == fS && newM == fM) return;
    if (!fS) usp = a[7]; else if (fM) msp = a[7]; else isp = a[7];
    fS = newS; fM = newM;
    a[7] = !fS ? usp : fM ? msp : isp;
}

void M68K::setSR(u16 v)
{
    switchStack((v >> 13) & 1, model >= MC68020 ? (v >> 12) & 1 : 0);
    fT = (v >> 15) & 1;
    imask = (v >> 8) & 7;
    fX = (v >> 4) & 1; fN = (v >> 3) & 1; fZ = (v >> 2) & 1; fV = (v >> 1) & 1; fC = v & 1;
}

u32 M68K::physical(u32 addr, bool write, bool code)
{
    bool fault = false;
    u32 p = mmu->translate(addr, write, fS != 0, code, fault);
    if (fault && !bus->busError) {
        bus->signalBusError(addr, write);
        bus->busErrorAtc = true;
        bus->busErrorCode = code;
        if (addr < 0x40000000u) pageFaults++;
    }
    return p;
}

// d[]/a[] and usp, isp, msp, fC, fV, fZ, fN, fX, fS, fM, fT, imask are each
// contiguous (checked below), so a checkpoint is two block copies.
static_assert(sizeof(u32) == 4, "u32");

__attribute__((noinline)) void M68K::checkpoint()
{
    memCopy(saved.da, d, sizeof saved.da);
    memCopy(saved.rest, &usp, sizeof saved.rest);
}

__attribute__((noinline)) void M68K::rollback()
{
    memCopy(d, saved.da, sizeof saved.da);
    memCopy(&usp, saved.rest, sizeof saved.rest);
    pc = instPC;
}

void M68K::flushTlb(u32 gen)
{
    tlbGeneration = gen;
    for (int i = 0; i < kTlbSize; i++) tlb[i].tag = 0;
    for (int i = 0; i < kCodeTlbSize; i++) codeTlb[i].tag = 0;
}

// Find (or build) the host pointer for the page of `addr`.  Translation
// faults and I/O pages return null: the caller takes the slow path, which
// raises the bus error or performs the device access.
u8 *M68K::tlbLookup(u32 addr, bool write, bool code)
{
    checkTlb();
    u32 tag = tlbTag(addr);
    TlbEntry &e = code ? codeTlb[(addr >> 12) & (kCodeTlbSize - 1)] : tlb[(addr >> 12) & (kTlbSize - 1)];
    if (e.tag == tag && (!write || e.writable)) return e.ptr;
    u32 phys = addr;
    if (mmu && mmu->enabled()) {
        bool fault = false;
        phys = mmu->translate(addr, write, fS != 0, code, fault);
        if (fault) return 0;
    }
    bool writable = false;
    u8 *p = bus->hostPage(phys & 0xFFFFF000u, writable);
    if (!p) return 0;
    if (write && !writable) return 0;
    e.tag = tag;
    e.ptr = p;
    e.writable = writable && write;                     // later writes revalidate the M bit once
    return p;
}

u16 M68K::fetch16Slow()
{
    u32 addr = pc;
    pc += 2;
    cyc += 4;
    if (model != MC68000 && (addr & 0xFFF) != 0xFFF) {
        const TlbEntry &e = codeTlb[(addr >> 12) & (kCodeTlbSize - 1)];
        if (e.tag == tlbTag(addr)) return be16(e.ptr + (addr & 0xFFF));
        if (u8 *p = tlbLookup(addr, false, true)) return be16(p + (addr & 0xFFF));
    }
    if (mmu && mmu->enabled()) addr = physical(addr, false, true);
    return bus->read16(addr);
}

static const u32 kSize[3] = {1, 2, 4};

u32 M68K::rdSlow(u32 addr, int sz)
{
    if (model == MC68000) {
        if (sz == 0) { cyc += 4; return bus->read8(addr); }
        if (sz == 1) { cyc += 4; return bus->read16(addr); }
        cyc += 8;
        u32 h = bus->read16(addr);
        return (h << 16) | bus->read16(addr + 2);
    }
    cyc += 4;
    if ((addr & 0xFFF) <= 0x1000 - kSize[sz]) {
        const TlbEntry &e = tlb[(addr >> 12) & (kTlbSize - 1)];
        u8 *p = e.tag == tlbTag(addr) ? e.ptr : tlbLookup(addr, false, false);
        if (p) {
            p += addr & 0xFFF;
            return sz == 0 ? p[0] : sz == 1 ? (u32)be16(p) : be32(p);
        }
    }
    bool mapped = mmu && mmu->enabled();
    // Misaligned, or crossing a page with the MMU on: assemble from bytes.
    if ((sz && (addr & 1)) || (mapped && sz && (addr & 0xFFF) > 0x1000 - kSize[sz])) {
        u32 v = 0;
        for (u32 i = 0; i < kSize[sz]; i++) {
            u32 p = mapped ? physical(addr + i, false, false) : addr + i;
            v = (v << 8) | bus->read8(p);
        }
        return v;
    }
    if (mapped) addr = physical(addr, false, false);
    bool before = bus->busError;
    if (before && model >= MC68040) {                   // translation fault: no access
        faultSeen();
        if (bus->busErrorAtc && !bus->busErrorWrite) bus->busErrorSize = sz;
        return sz == 0 ? 0xFF : sz == 1 ? 0xFFFF : 0xFFFFFFFFu;
    }
    u32 v;
    if (sz == 0) v = bus->read8(addr);
    else if (sz == 1) v = bus->read16(addr);
    else if (addr & 2) { u32 h = bus->read16(addr); v = (h << 16) | bus->read16(addr + 2); }
    else v = bus->read32(addr);
    if (!before && bus->busError && model >= MC68040) { bus->busErrorSize = sz; faultSeen(); }   // a device's
    return v;
}

void M68K::wrSlow(u32 addr, int sz, u32 v)
{
    if (bus->busError && model >= MC68040) return;      // the instruction will run again: no writes now
    if (model == MC68000) {
        if (sz == 0) { cyc += 4; bus->write8(addr, (u8)v); }
        else if (sz == 1) { cyc += 4; bus->write16(addr, (u16)v); }
        else { cyc += 8; bus->write16(addr, (u16)(v >> 16)); bus->write16(addr + 2, (u16)v); }
        return;
    }
    cyc += 4;
    if ((addr & 0xFFF) <= 0x1000 - kSize[sz]) {
        const TlbEntry &e = tlb[(addr >> 12) & (kTlbSize - 1)];
        u8 *p = e.tag == tlbTag(addr) && e.writable ? e.ptr : tlbLookup(addr, true, false);
        if (p) {
            p += addr & 0xFFF;
            if (sz == 0) p[0] = (u8)v; else if (sz == 1) putBe16(p, (u16)v); else putBe32(p, v);
            return;
        }
    }
    bool mapped = mmu && mmu->enabled();
    if ((sz && (addr & 1)) || (mapped && sz && (addr & 0xFFF) > 0x1000 - kSize[sz])) {
        for (u32 i = 0; i < kSize[sz]; i++) {
            u32 p = mapped ? physical(addr + i, true, false) : addr + i;
            if (bus->busError && model >= MC68040) { faultSeen(); return; }
            bus->write8(p, (u8)(v >> (8 * (kSize[sz] - 1 - i))));
        }
        return;
    }
    if (mapped) addr = physical(addr, true, false);
    if (bus->busError && model >= MC68040) { faultSeen(); bus->busErrorSize = sz; return; }
    if (sz == 0) bus->write8(addr, (u8)v);
    else if (sz == 1) bus->write16(addr, (u16)v);
    else if (addr & 2) { bus->write16(addr, (u16)(v >> 16)); bus->write16(addr + 2, (u16)v); }
    else bus->write32(addr, v);
    if (bus->busError && model >= MC68040) { bus->busErrorSize = sz; faultSeen(); }   // a device's
}

u32 M68K::index(u32 base)
{
    u16 ext = fetch16();
    cyc += 2;
    u32 xn = (ext & 0x8000) ? a[(ext >> 12) & 7] : d[(ext >> 12) & 7];
    if (!(ext & 0x800)) xn = (u32)(s32)(s16)xn;
    if (model == MC68000) return base + (u32)(s32)(s8)ext + xn;

    xn <<= (ext >> 9) & 3;                              // scale
    if (!(ext & 0x100)) return base + (u32)(s32)(s8)ext + xn;

    // Full extension word: base and outer displacements, memory indirection.
    if (ext & 0x80) base = 0;
    if (ext & 0x40) xn = 0;
    u32 bd = 0;
    switch ((ext >> 4) & 3) {
    case 2: bd = (u32)(s32)(s16)fetch16(); break;
    case 3: bd = fetch32(); break;
    default: break;
    }
    int iis = ext & 7;
    if (iis == 0) return base + bd + xn;
    u32 od = 0;
    switch (iis & 3) {
    case 2: od = (u32)(s32)(s16)fetch16(); break;
    case 3: od = fetch32(); break;
    default: break;
    }
    if (iis & 4) return rd(base + bd, 2) + xn + od;     // post-indexed
    return rd(base + bd + xn, 2) + od;                  // pre-indexed
}

// Compute the address of a memory operand.  Callers validate the mode first.
u32 M68K::ea(int mode, int reg, int sz, bool noPenalty)
{
    u32 inc = sz == 0 ? (reg == 7 ? 2 : 1) : sz == 1 ? 2 : 4;
    switch (mode) {
    case 2: return a[reg];
    case 3: { u32 ad = a[reg]; a[reg] += inc; return ad; }
    case 4: a[reg] -= inc; if (!noPenalty) cyc += 2; return a[reg];
    case 5: { u32 dsp = (u32)(s32)(s16)fetch16(); return a[reg] + dsp; }
    case 6: return index(a[reg]);
    default:
        switch (reg) {
        case 0: return (u32)(s32)(s16)fetch16();
        case 1: return fetch32();
        case 2: { u32 base = pc; return base + (u32)(s32)(s16)fetch16(); }
        default: { u32 base = pc; return index(base); }
        }
    }
}

u32 M68K::readEA(int mode, int reg, int sz)
{
    if (mode == 0) return d[reg] & MASK[sz];
    if (mode == 1) return a[reg] & MASK[sz];
    if (mode == 7 && reg == 4) return sz == 2 ? fetch32() : (fetch16() & MASK[sz]);
    return rd(ea(mode, reg, sz), sz);
}

void M68K::setD(int reg, int sz, u32 v)
{
    d[reg] = (d[reg] & ~MASK[sz]) | (v & MASK[sz]);
}

bool M68K::cond(int c) const
{
    switch (c) {
    case 0: return true;
    case 1: return false;
    case 2: return !fC && !fZ;
    case 3: return fC || fZ;
    case 4: return !fC;
    case 5: return fC;
    case 6: return !fZ;
    case 7: return fZ;
    case 8: return !fV;
    case 9: return fV;
    case 10: return !fN;
    case 11: return fN;
    case 12: return fN == fV;
    case 13: return fN != fV;
    case 14: return !fZ && fN == fV;
    default: return fZ || fN != fV;
    }
}

// Stack frame formats (68010+): 0 = four words, 2 = six words with the
// address of the instruction that caused the exception.
void M68K::exception(int vec, u32 pushPC, int extraCycles, int format)
{
    u16 old = getSR();
    switchStack(1, vec >= 24 && vec < 32 ? 0 : fM);    // interrupts use the ISP
    fT = 0;
    if (model != MC68000) {
        if (format == 2) push32(instPC);
        push16((u16)((format << 12) | (vec * 4)));
    }
    push32(pushPC);
    push16(old);
    pc = rd(vbr + (u32)vec * 4, 2);
    cyc += extraCycles;
    traceNext = false;
}

// Bus error: format A (68020/68030 short bus cycle fault) or format 7
// (68040 access error).  On the 68040 the registers have been put back as
// the instruction found them and later writes were dropped, so returning
// with RTE runs the instruction again: virtual memory can page in and go on.
void M68K::busErrorException()
{
    u32 addr = bus->busErrorAddr;
    bool write = bus->busErrorWrite;
    bus->busError = false;
    u16 old = getSR();
    switchStack(1, fM);
    fT = 0;
    if (model >= MC68040) {
        a[7] -= 60;
        u32 f = a[7];
        for (u32 i = 0; i < 60; i += 4) wr(f + i, 2, 0);
        wr(f, 1, old);
        wr(f + 2, 2, instPC);
        wr(f + 6, 1, 0x7000 | 2 * 4);
        wr(f + 8, 2, addr);                             // effective address
        // SSW: ATC for a translation fault, RW, SIZE, TM (user/supervisor, data/code).
        static const u16 kSizeBits[3] = {0x20, 0x40, 0x00};   // byte, word, long
        u16 tm = (u16)((fS ? 4 : 0) | (bus->busErrorCode ? 2 : 1));
        wr(f + 12, 1, (u16)((bus->busErrorAtc ? 0x400 : 0) | (write ? 0 : 0x100) |
                            kSizeBits[bus->busErrorSize < 0 || bus->busErrorSize > 2 ? 2 : bus->busErrorSize] | tm));
        wr(f + 20, 2, addr);                            // fault address
    } else {
        a[7] -= 32;
        u32 f = a[7];
        for (u32 i = 0; i < 32; i += 4) wr(f + i, 2, 0);
        wr(f, 1, old);
        wr(f + 2, 2, instPC);
        wr(f + 6, 1, 0xA000 | 2 * 4);
        wr(f + 10, 1, (u16)(0x0100 | (write ? 0 : 0x40) | (fS ? 5 : 1)));  // SSW: DF, RW, FC
        wr(f + 16, 2, addr);                            // data cycle fault address
    }
    pc = rd(vbr + 2 * 4, 2);
    cyc += 50;
    traceNext = false;
    bus->busError = false;                              // a fault while stacking is ignored
}

void M68K::illegal()
{
    int grp = opcode >> 12;
    exception(grp == 0xA ? 10 : grp == 0xF ? 11 : 4, instPC, 10);
}

static inline u16 srMask(int model) { return model == 0 ? 0xA71F : 0xB71F; }

void M68K::setNZ(u32 v, int sz)
{
    fN = (v & MSB[sz]) ? 1 : 0;
    fZ = (v & MASK[sz]) ? 0 : 1;
}

u32 M68K::add(u32 s, u32 dd, int sz, u32 x, bool isX)
{
    u64 r = (u64)s + dd + x;
    u32 res = (u32)r & MASK[sz];
    fC = fX = (u32)(r >> BITS[sz]) & 1;
    fV = ((s ^ res) & (dd ^ res) & MSB[sz]) ? 1 : 0;
    fN = (res & MSB[sz]) ? 1 : 0;
    if (isX) { if (res) fZ = 0; } else fZ = res == 0;
    return res;
}

// dd - s - x
u32 M68K::sub(u32 s, u32 dd, int sz, u32 x, bool isX, bool setX)
{
    u64 r = (u64)dd - s - x;
    u32 res = (u32)r & MASK[sz];
    fC = (u32)(r >> BITS[sz]) & 1;
    if (setX) fX = fC;
    fV = ((s ^ dd) & (res ^ dd) & MSB[sz]) ? 1 : 0;
    fN = (res & MSB[sz]) ? 1 : 0;
    if (isX) { if (res) fZ = 0; } else fZ = res == 0;
    return res;
}

// BCD arithmetic including the undocumented N and V results of real silicon.
u32 M68K::abcd(u32 s, u32 dd)
{
    u32 ss = s + dd + fX;
    u32 bc = ((s & dd) | (~ss & s) | (~ss & dd)) & 0x88;
    u32 dc = (((ss + 0x66) ^ ss) & 0x110) >> 1;
    u32 corf = (bc | dc) - ((bc | dc) >> 2);
    u32 rr = (ss + corf) & 0xFF;
    fX = fC = ((bc | (ss & ~rr)) >> 7) & 1;
    fV = ((~ss & rr) >> 7) & 1;
    if (rr) fZ = 0;
    fN = rr >> 7;
    return rr;
}

u32 M68K::sbcd(u32 s, u32 dd)
{
    u32 df = (dd - s - fX) & 0xFF;
    u32 bc = ((~dd & s) | (df & ~dd) | (df & s)) & 0x88;
    u32 corf = bc - (bc >> 2);
    u32 rr = (df - corf) & 0xFF;
    fX = fC = ((bc | (~df & rr)) >> 7) & 1;
    fV = ((df & ~rr) >> 7) & 1;
    if (rr) fZ = 0;
    fN = rr >> 7;
    return rr;
}

u32 M68K::shift(int type, bool left, u32 v, int count, int sz)
{
    u32 m = MASK[sz], msb = MSB[sz];
    v &= m;
    fV = 0;
    if (count == 0) fC = type == 2 ? fX : 0;
    for (int i = 0; i < count; i++) {
        if (left) {
            u32 out = (v & msb) ? 1 : 0;
            switch (type) {
            case 0: v = (v << 1) & m; fC = fX = out; if (((v & msb) ? 1u : 0u) != out) fV = 1; break;
            case 1: v = (v << 1) & m; fC = fX = out; break;
            case 2: v = ((v << 1) | fX) & m; fC = fX = out; break;
            default: v = ((v << 1) | out) & m; fC = out; break;
            }
        } else {
            u32 out = v & 1;
            switch (type) {
            case 0: v = (v >> 1) | (v & msb); fC = fX = out; break;
            case 1: v >>= 1; fC = fX = out; break;
            case 2: v = (v >> 1) | (fX ? msb : 0); fC = fX = out; break;
            default: v = (v >> 1) | (out ? msb : 0); fC = out; break;
            }
        }
    }
    setNZ(v, sz);
    return v;
}

int M68K::step()
{
    return stepOne();
}

u32 M68K::run(int count, u64 end)
{
    u32 total = 0;
    for (int i = 0; i < count && totalCycles < end; i++) total += (u32)stepOne();
    return total;
}

inline int M68K::stepOne()
{
    cyc = 0;
    if (irq != 7) nmiSeen = false;
    if ((irq < 7 && irq > (int)imask) || (irq == 7 && !nmiSeen)) {
        int level = irq;
        if (level == 7) nmiSeen = true;
        stopped = false;
        instPC = pc;
        exception(24 + level, pc, 24);
        imask = (u32)level;
        bus->irqAck(level);
    } else if (stopped) {
        cyc = 4;
    } else {
        traceNext = fT != 0;
        if (model != MC68000) checkTlb();               // mappings may have changed in the last instruction
        instPC = pc;
        if (__builtin_expect(restartable, 0)) checkpoint();
        opcode = fetch16();
#ifdef OPHIST
        extern unsigned long long gOpHist[65536];
        gOpHist[opcode]++;
#endif
        bool fline = __builtin_expect(restartable, 0) && (opcode >> 12) == 0xF && fpu;
        if (fline) fpu->checkpoint();
        if (fastDispatch) table[opcode](*this, opcode);
        else exec(opcode);
        if (bus->busError) {
            if (restartable) { rollback(); if (fline) fpu->rollback(); }
            busErrorException();
        }
        else if (traceNext) exception(9, pc, 10, model == MC68000 ? 0 : 2);
    }
    if (model >= MC68040) cyc = (cyc + 3) >> 2;
    else if (model >= MC68020) cyc = (cyc + 1) >> 1;
    totalCycles += (u32)cyc;
    return cyc;
}

void M68K::exec(u16 op)
{
    int mode = (op >> 3) & 7, reg = op & 7;
    switch (op >> 12) {
    case 0x0: group0(op); return;

    case 0x1: case 0x2: case 0x3: {                     // MOVE / MOVEA
        int sz = (op >> 12) == 1 ? 0 : (op >> 12) == 3 ? 1 : 2;
        int dmode = (op >> 6) & 7, dreg = (op >> 9) & 7;
        NEED(sz == 0 ? EA_DATA : EA_ALL);
        if (!validEA(dmode, dreg, sz == 0 ? EA_DATAALT : EA_ALT)) { illegal(); return; }
        u32 v = readEA(mode, reg, sz);
        if (dmode == 1) { a[dreg] = (u32)sext(v, sz); return; }
        logicFlags(v, sz);
        if (dmode == 0) setD(dreg, sz, v);
        else wr(ea(dmode, dreg, sz, true), sz, v);
        return;
    }

    case 0x4: group4(op); return;

    case 0x5: {
        int sz = (op >> 6) & 3;
        if (sz == 3) {
            int cc = (op >> 8) & 15;
            if (mode == 1) {                            // DBcc
                u32 base = pc;
                s32 disp = (s16)fetch16();
                if (cond(cc)) { cyc += 4; return; }
                u16 cnt = (u16)(d[reg] - 1);
                setD(reg, 1, cnt);
                if (cnt != 0xFFFF) { pc = base + (u32)disp; cyc += 2; }
                else cyc += 6;
                return;
            }
            if (model && mode == 7 && reg >= 2 && reg <= 4) { trapcc(op); return; }
            NEED(EA_DATAALT);                           // Scc
            bool t = cond(cc);
            if (mode == 0) { setD(reg, 0, t ? 0xFF : 0); if (t) cyc += 2; }
            else { wr(ea(mode, reg, 0), 0, t ? 0xFF : 0); cyc += 4; }
            return;
        }
        u32 q = (op >> 9) & 7;                          // ADDQ / SUBQ
        if (!q) q = 8;
        bool isSub = (op & 0x100) != 0;
        NEED(sz == 0 ? EA_DATAALT : EA_ALT);
        if (mode == 1) { a[reg] = isSub ? a[reg] - q : a[reg] + q; cyc += 4; return; }
        if (mode == 0) {
            u32 v = d[reg] & MASK[sz];
            setD(reg, sz, isSub ? sub(q, v, sz, 0, false, true) : add(q, v, sz, 0, false));
            if (sz == 2) cyc += 4;
            return;
        }
        u32 addr = ea(mode, reg, sz);
        u32 v = rd(addr, sz);
        wr(addr, sz, isSub ? sub(q, v, sz, 0, false, true) : add(q, v, sz, 0, false));
        return;
    }

    case 0x6: {                                         // Bcc / BRA / BSR
        int cc = (op >> 8) & 15;
        u32 base = pc;
        s32 disp = (s8)op;
        bool wide = disp == 0;
        if (wide) disp = (s16)fetch16();
        else if (disp == -1 && model) { disp = (s32)fetch32(); wide = true; }
        if (cc == 1) {                                  // BSR
            push32(pc);
            pc = base + (u32)disp;
            cyc += wide ? 2 : 6;
            return;
        }
        if (cond(cc)) { pc = base + (u32)disp; cyc += wide ? 2 : 6; }
        else cyc += 4;
        return;
    }

    case 0x7:                                           // MOVEQ
        if (op & 0x100) { illegal(); return; }
        d[(op >> 9) & 7] = (u32)(s32)(s8)op;
        logicFlags(d[(op >> 9) & 7], 2);
        return;

    case 0x8: case 0x9: case 0xB: case 0xC: case 0xD:
        arith(op);
        return;

    case 0xE: groupE(op); return;

    case 0xF: lineF(op); return;

    default:                                            // A-line
        illegal();
        return;
    }
}

void M68K::group0(u16 op)
{
    int mode = (op >> 3) & 7, reg = op & 7;

    if (model && !(op & 0x100)) {
        if ((op & 0xF9C0) == 0x00C0 && (op & 0x0600) != 0x0600) { chk2(op); return; }
        if ((op & 0xF9FF) == 0x08FC && (op & 0x0600) >= 0x0400) { cas2(op); return; }
        if ((op & 0xF9C0) == 0x08C0 && (op & 0x0600)) { cas(op); return; }
        if ((op & 0xFF00) == 0x0E00 && (op & 0xC0) != 0xC0) { moves(op); return; }
    }

    if (op & 0x100) {
        if (mode == 1) {                                // MOVEP
            u32 addr = a[reg] + (u32)(s32)(s16)fetch16();
            int dn = (op >> 9) & 7;
            switch ((op >> 6) & 3) {
            case 0: {
                u32 v = rd(addr, 0) << 8; v |= rd(addr + 2, 0);
                setD(dn, 1, v); break;
            }
            case 1: {
                u32 v = rd(addr, 0) << 24; v |= rd(addr + 2, 0) << 16;
                v |= rd(addr + 4, 0) << 8; v |= rd(addr + 6, 0);
                d[dn] = v; break;
            }
            case 2: wr(addr, 0, d[dn] >> 8); wr(addr + 2, 0, d[dn]); break;
            default:
                wr(addr, 0, d[dn] >> 24); wr(addr + 2, 0, d[dn] >> 16);
                wr(addr + 4, 0, d[dn] >> 8); wr(addr + 6, 0, d[dn]); break;
            }
            return;
        }
    }

    if ((op & 0x100) || ((op >> 9) & 7) == 4) {         // BTST / BCHG / BCLR / BSET
        int type = (op >> 6) & 3;
        u32 bit;
        if (op & 0x100) {
            NEED(type == 0 ? EA_DATA : EA_DATAALT);
            bit = d[(op >> 9) & 7];
        } else {
            NEED(type == 0 ? (EA_DATA & ~EA_IMM) : EA_DATAALT);
            bit = fetch16();
        }
        if (mode == 0) {
            bit &= 31;
            u32 m = 1u << bit;
            fZ = (d[reg] & m) ? 0 : 1;
            switch (type) {
            case 0: cyc += 2; break;
            case 1: d[reg] ^= m; cyc += bit > 15 ? 4 : 2; break;
            case 2: d[reg] &= ~m; cyc += bit > 15 ? 6 : 4; break;
            default: d[reg] |= m; cyc += bit > 15 ? 4 : 2; break;
            }
            return;
        }
        u32 m = 1u << (bit & 7);
        if (mode == 7 && reg == 4) {                    // BTST Dn,#imm
            fZ = (fetch16() & m) ? 0 : 1;
            cyc += 2;
            return;
        }
        u32 addr = ea(mode, reg, 0);
        u32 v = rd(addr, 0);
        fZ = (v & m) ? 0 : 1;
        switch (type) {
        case 0: break;
        case 1: wr(addr, 0, v ^ m); break;
        case 2: wr(addr, 0, v & ~m); break;
        default: wr(addr, 0, v | m); break;
        }
        return;
    }

    int kind = (op >> 9) & 7;                           // ORI ANDI SUBI ADDI - EORI CMPI
    int sz = (op >> 6) & 3;
    if (kind == 7 || sz == 3) { illegal(); return; }

    if (mode == 7 && reg == 4) {                        // to CCR / to SR
        if (sz == 2 || !(kind == 0 || kind == 1 || kind == 5)) { illegal(); return; }
        if (sz == 1 && !fS) { privilege(); return; }
        u16 imm = fetch16();
        u16 sr = getSR();
        if (sz == 0) imm &= 0xFF;
        if (kind == 0) sr |= imm;
        else if (kind == 1) sr &= (u16)(imm | (sz == 0 ? 0xFF00 : 0));
        else sr ^= imm;
        setSR(sr & srMask(model));
        cyc += 12;
        return;
    }

    if (kind == 6 && model) NEED(EA_DATA & ~EA_IMM);   // CMPI allows PC-relative on 68020+
    else NEED(EA_DATAALT);
    u32 imm = sz == 2 ? fetch32() : (fetch16() & MASK[sz]);
    u32 addr = 0, v;
    if (mode == 0) v = d[reg] & MASK[sz];
    else { addr = ea(mode, reg, sz); v = rd(addr, sz); }
    u32 r;
    switch (kind) {
    case 0: r = v | imm; logicFlags(r, sz); break;
    case 1: r = v & imm; logicFlags(r, sz); break;
    case 2: r = sub(imm, v, sz, 0, false, true); break;
    case 3: r = add(imm, v, sz, 0, false); break;
    case 5: r = v ^ imm; logicFlags(r, sz); break;
    default:
        sub(imm, v, sz, 0, false, false);
        if (mode == 0 && sz == 2) cyc += 2;
        return;
    }
    if (mode == 0) { setD(reg, sz, r); if (sz == 2) cyc += 4; }
    else wr(addr, sz, r);
}

void M68K::movem(u16 op)
{
    int mode = (op >> 3) & 7, reg = op & 7;
    int sz = (op & 0x40) ? 2 : 1;
    u32 step = sz == 2 ? 4 : 2;
    bool toReg = (op & 0x400) != 0;
    NEED(toReg ? (EA_CTRL | EA_INC) : (EA_CTRLALT | EA_DEC));
    u16 mask = fetch16();

    if (!toReg) {
        if (mode == 4) {
            u32 addr = a[reg];
            for (int i = 0; i < 16; i++) {
                if (!(mask & (1 << i))) continue;
                int r = 15 - i;
                addr -= step;
                wr(addr, sz, r < 8 ? d[r] : a[r - 8]);
            }
            a[reg] = addr;
            return;
        }
        u32 addr = ea(mode, reg, sz);
        for (int i = 0; i < 16; i++) {
            if (!(mask & (1 << i))) continue;
            wr(addr, sz, i < 8 ? d[i] : a[i - 8]);
            addr += step;
        }
        return;
    }

    u32 addr = mode == 3 ? a[reg] : ea(mode, reg, sz);
    for (int i = 0; i < 16; i++) {
        if (!(mask & (1 << i))) continue;
        u32 v = rd(addr, sz);
        if (sz == 1) v = (u32)(s32)(s16)v;
        if (i < 8) d[i] = v; else a[i - 8] = v;
        addr += step;
    }
    if (mode == 3) a[reg] = addr;
    cyc += 4;
}

void M68K::group4(u16 op)
{
    int mode = (op >> 3) & 7, reg = op & 7;
    int sz = (op >> 6) & 3;

    if (op == 0x4AFC) { illegal(); return; }

    if (model) {
        if ((op & 0xFFF8) == 0x49C0) {                  // EXTB.L
            d[reg] = (u32)(s32)(s8)d[reg];
            logicFlags(d[reg], 2);
            return;
        }
        if ((op & 0xFFF8) == 0x4808) {                  // LINK.L
            u32 disp = fetch32();
            a[7] -= 4;
            wr(a[7], 2, a[reg]);
            a[reg] = a[7];
            a[7] += disp;
            return;
        }
        if ((op & 0xFFC0) == 0x4C00) { mulLong(op); return; }
        if ((op & 0xFFC0) == 0x4C40) { divLong(op); return; }
        if ((op & 0xFFC0) == 0x42C0) {                  // MOVE from CCR
            NEED(EA_DATAALT);
            u32 ccr = getSR() & 0x1F;
            if (mode == 0) setD(reg, 1, ccr); else wr(ea(mode, reg, 1), 1, ccr);
            return;
        }
        if (op == 0x4E74) {                             // RTD
            u32 disp = (u32)(s32)(s16)fetch16();
            pc = pop32();
            a[7] += disp;
            return;
        }
        if ((op & 0xFFFE) == 0x4E7A) { movec(op); return; }
        if ((op & 0xFFF8) == 0x4848) { illegal(); return; }   // BKPT: no debugger attached
    }

    if (op & 0x100) {
        int dn = (op >> 9) & 7;
        if (sz == 3) {                                  // LEA
            NEED(EA_CTRL);
            a[dn] = ea(mode, reg, 2);
            if (mode == 6 || (mode == 7 && reg == 3)) cyc += 2;
            return;
        }
        if (sz == 2) {                                  // CHK
            NEED(EA_DATA);
            s16 bound = (s16)readEA(mode, reg, 1);
            s16 v = (s16)d[dn];
            fZ = fV = fC = 0;                           // N is left alone when in range
            if (v < 0) { fN = 1; exception(6, pc, 16, model ? 2 : 0); return; }
            if (v > bound) { fN = 0; exception(6, pc, 14, model ? 2 : 0); return; }
            cyc += 6;
            return;
        }
        if (sz == 0 && model) {                         // CHK.L
            NEED(EA_DATA);
            s32 bound = (s32)readEA(mode, reg, 2);
            s32 v = (s32)d[dn];
            fZ = v == 0; fV = fC = 0;
            if (v < 0) { fN = 1; exception(6, pc, 16, 2); return; }
            if (v > bound) { fN = 0; exception(6, pc, 14, 2); return; }
            return;
        }
        illegal();
        return;
    }

    switch ((op >> 8) & 0xF) {
    case 0x0:
        NEED(EA_DATAALT);
        if (sz == 3) {                                  // MOVE from SR
            if (model && !fS) { privilege(); return; }
            if (mode == 0) { setD(reg, 1, getSR()); cyc += 2; }
            else { wr(ea(mode, reg, 1), 1, getSR()); cyc += 4; }
            return;
        }
        if (mode == 0) {                                // NEGX
            setD(reg, sz, sub(d[reg] & MASK[sz], 0, sz, fX, true, true));
            if (sz == 2) cyc += 2;
        } else {
            u32 addr = ea(mode, reg, sz);
            wr(addr, sz, sub(rd(addr, sz), 0, sz, fX, true, true));
        }
        return;

    case 0x2:                                           // CLR
        if (sz == 3) { illegal(); return; }
        NEED(EA_DATAALT);
        fN = fV = fC = 0; fZ = 1;
        if (mode == 0) { setD(reg, sz, 0); if (sz == 2) cyc += 2; }
        else { wr(ea(mode, reg, sz), sz, 0); cyc += sz == 2 ? 8 : 4; }
        return;

    case 0x4:
        if (sz == 3) {                                  // MOVE to CCR
            NEED(EA_DATA);
            u16 v = (u16)readEA(mode, reg, 1);
            setSR((u16)((getSR() & 0xFF00) | (v & 0x1F)));
            cyc += 8;
            return;
        }
        NEED(EA_DATAALT);                               // NEG
        if (mode == 0) {
            setD(reg, sz, sub(d[reg] & MASK[sz], 0, sz, 0, false, true));
            if (sz == 2) cyc += 2;
        } else {
            u32 addr = ea(mode, reg, sz);
            wr(addr, sz, sub(rd(addr, sz), 0, sz, 0, false, true));
        }
        return;

    case 0x6:
        if (sz == 3) {                                  // MOVE to SR
            NEED(EA_DATA);
            if (!fS) { privilege(); return; }
            u16 v = (u16)readEA(mode, reg, 1);
            setSR(v & srMask(model));
            cyc += 8;
            return;
        }
        NEED(EA_DATAALT);                               // NOT
        if (mode == 0) {
            u32 r = ~d[reg] & MASK[sz];
            logicFlags(r, sz);
            setD(reg, sz, r);
            if (sz == 2) cyc += 2;
        } else {
            u32 addr = ea(mode, reg, sz);
            u32 r = ~rd(addr, sz) & MASK[sz];
            logicFlags(r, sz);
            wr(addr, sz, r);
        }
        return;

    case 0x8:
        if (sz == 0) {                                  // NBCD
            NEED(EA_DATAALT);
            if (mode == 0) { setD(reg, 0, sbcd(d[reg] & 0xFF, 0)); cyc += 2; }
            else { u32 addr = ea(mode, reg, 0); wr(addr, 0, sbcd(rd(addr, 0), 0)); }
            return;
        }
        if (sz == 1) {
            if (mode == 0) {                            // SWAP
                d[reg] = (d[reg] >> 16) | (d[reg] << 16);
                logicFlags(d[reg], 2);
                return;
            }
            NEED(EA_CTRL);                              // PEA
            u32 addr = ea(mode, reg, 2);
            if (mode == 6 || (mode == 7 && reg == 3)) cyc += 2;
            push32(addr);
            return;
        }
        if (mode == 0) {                                // EXT
            if (sz == 2) { setD(reg, 1, (u32)(s32)(s8)d[reg]); logicFlags(d[reg], 1); }
            else { d[reg] = (u32)(s32)(s16)d[reg]; logicFlags(d[reg], 2); }
            return;
        }
        movem(op);
        return;

    case 0xA:
        if (sz == 3) {                                  // TAS
            NEED(EA_DATAALT);
            if (mode == 0) { logicFlags(d[reg], 0); d[reg] |= 0x80; }
            else {
                u32 addr = ea(mode, reg, 0);
                u32 v = rd(addr, 0);
                logicFlags(v, 0);
                wr(addr, 0, v | 0x80);
                cyc += 2;
            }
            return;
        }
        if (model) NEED(sz == 0 ? EA_DATA : EA_ALL);    // TST
        else NEED(sz == 0 ? EA_DATAALT : EA_ALT);
        logicFlags(readEA(mode, reg, sz), sz);
        return;

    case 0xC:
        if (sz < 2) { illegal(); return; }
        movem(op);
        return;

    case 0xE:
        break;

    default:
        illegal();
        return;
    }

    // 0x4Exx
    if (sz == 2 || sz == 3) {                           // JSR / JMP
        NEED(EA_CTRL);
        u32 addr = ea(mode, reg, 2);
        if (mode == 2 || mode == 6 || (mode == 7 && reg == 3)) cyc += 4;
        else if (mode == 5 || (mode == 7 && (reg == 0 || reg == 2))) cyc += 2;
        if (sz == 2) push32(pc);
        pc = addr;
        return;
    }
    if (sz == 0) { illegal(); return; }

    switch ((op >> 4) & 3) {
    case 0:                                             // TRAP
        exception(32 + (op & 15), pc, 10);
        return;
    case 1:
        if (op & 8) {                                   // UNLK
            a[7] = a[reg];
            a[reg] = pop32();
        } else {                                        // LINK
            u32 disp = (u32)(s32)(s16)fetch16();
            a[7] -= 4;
            wr(a[7], 2, a[reg]);
            a[reg] = a[7];
            a[7] += disp;
        }
        return;
    case 2:                                             // MOVE USP
        if (!fS) { privilege(); return; }
        if (op & 8) a[reg] = usp; else usp = a[reg];
        return;
    default:
        switch (op & 15) {
        case 0:                                         // RESET
            if (!fS) { privilege(); return; }
            bus->resetLine();
            cyc += 128;
            return;
        case 1: return;                                 // NOP
        case 2: {                                       // STOP
            if (!fS) { privilege(); return; }
            setSR(fetch16() & srMask(model));
            stopped = true;
            cyc -= 4;
            return;
        }
        case 3:                                         // RTE
            if (!fS) { privilege(); return; }
            rte();
            return;
        case 5: pc = pop32(); cyc += 4; return;         // RTS
        case 6:                                         // TRAPV
            if (fV) exception(7, pc, 10, model ? 2 : 0);
            return;
        case 7: {                                       // RTR
            u16 ccr = pop16();
            pc = pop32();
            setSR((u16)((getSR() & 0xFF00) | (ccr & 0x1F)));
            cyc += 4;
            return;
        }
        default: illegal(); return;
        }
    }
}

static int divuCycles(u32 dividend, u16 divisor)
{
    if ((dividend >> 16) >= divisor) return 10;
    int m = 38;
    u32 hd = (u32)divisor << 16;
    for (int i = 0; i < 15; i++) {
        u32 t = dividend;
        dividend <<= 1;
        if ((s32)t < 0) dividend -= hd;
        else {
            m += 2;
            if (dividend >= hd) { dividend -= hd; m--; }
        }
    }
    return m * 2;
}

static int divsCycles(s32 dividend, s16 divisor)
{
    int m = 6;
    if (dividend < 0) m++;
    u32 ad = dividend < 0 ? 0u - (u32)dividend : (u32)dividend;
    u32 av = divisor < 0 ? (u32)(-(s32)divisor) : (u32)divisor;
    if ((ad >> 16) >= av) return (m + 2) * 2;
    u32 aq = ad / av;
    m += 55;
    if (divisor >= 0) { if (dividend >= 0) m--; else m++; }
    for (int i = 0; i < 15; i++) {
        if ((s16)aq >= 0) m++;
        aq <<= 1;
    }
    return m * 2;
}

void M68K::arith(u16 op)
{
    int grp = op >> 12, dn = (op >> 9) & 7, opmode = (op >> 6) & 7;
    int mode = (op >> 3) & 7, reg = op & 7;
    int sz = opmode & 3;

    if (grp == 0x8 && model && mode < 2 && (opmode == 5 || opmode == 6)) { packUnpk(op); return; }

    if (grp == 0x8 || grp == 0xC) {
        if (sz == 3) {                                  // MULU MULS DIVU DIVS
            NEED(EA_DATA);
            u32 s = readEA(mode, reg, 1);
            bool isSigned = opmode == 7;
            if (grp == 0xC) {
                u32 r;
                int n = 0;
                if (isSigned) {
                    r = (u32)((s32)(s16)s * (s32)(s16)d[dn]);
                    u32 p = (s << 1) & 0x1FFFF;
                    for (int i = 0; i < 16; i++) if (((p >> i) & 3) == 1 || ((p >> i) & 3) == 2) n++;
                } else {
                    r = s * (d[dn] & 0xFFFF);
                    for (int i = 0; i < 16; i++) if (s & (1u << i)) n++;
                }
                d[dn] = r;
                logicFlags(r, 2);
                cyc += 34 + 2 * n;
                return;
            }
            if (s == 0) {
                fN = fZ = fV = fC = 0;
                exception(5, pc, 14, model ? 2 : 0);
                return;
            }
            if (!isSigned) {
                u32 dividend = d[dn];
                cyc += divuCycles(dividend, (u16)s) - 4;
                u32 q = dividend / s, r = dividend % s;
                fC = 0;
                if (q > 0xFFFF) { fV = 1; return; }          // N and Z keep their values
                fV = 0;
                setNZ(q, 1);
                d[dn] = (r << 16) | q;
                return;
            }
            s32 dividend = (s32)d[dn];
            s32 divisor = (s16)s;
            cyc += divsCycles(dividend, (s16)s) - 4;
            fC = 0;
            s64 q = (s64)dividend / divisor;
            s64 r = (s64)dividend % divisor;
            if (q > 32767 || q < -32768) { fV = 1; return; }
            fV = 0;
            setNZ((u32)q, 1);
            d[dn] = (((u32)r & 0xFFFF) << 16) | ((u32)q & 0xFFFF);
            return;
        }
        if (opmode == 4 && mode < 2) {                  // ABCD / SBCD
            u32 r;
            if (mode == 0) {
                r = grp == 0xC ? abcd(d[reg] & 0xFF, d[dn] & 0xFF) : sbcd(d[reg] & 0xFF, d[dn] & 0xFF);
                setD(dn, 0, r);
                cyc += 2;
            } else {
                u32 s = rd(ea(4, reg, 0, true), 0);
                u32 addr = ea(4, dn, 0, true);
                u32 v = rd(addr, 0);
                wr(addr, 0, grp == 0xC ? abcd(s, v) : sbcd(s, v));
                cyc += 2;
            }
            return;
        }
        if (grp == 0xC && opmode == 5 && mode == 0) { u32 t = d[dn]; d[dn] = d[reg]; d[reg] = t; cyc += 2; return; }
        if (grp == 0xC && opmode == 5 && mode == 1) { u32 t = a[dn]; a[dn] = a[reg]; a[reg] = t; cyc += 2; return; }
        if (grp == 0xC && opmode == 6 && mode == 1) { u32 t = d[dn]; d[dn] = a[reg]; a[reg] = t; cyc += 2; return; }
    } else {
        if (sz == 3) {                                  // ADDA SUBA CMPA
            int osz = opmode == 3 ? 1 : 2;
            NEED(EA_ALL);
            u32 s = (u32)sext(readEA(mode, reg, osz), osz);
            bool fromMem = mode >= 2 && !(mode == 7 && reg == 4);
            if (grp == 0xB) { sub(s, a[dn], 2, 0, false, false); cyc += 2; return; }
            a[dn] = grp == 0xD ? a[dn] + s : a[dn] - s;
            cyc += (osz == 1 || !fromMem) ? 4 : 2;
            return;
        }
        if (grp != 0xB && opmode >= 4 && mode < 2) {    // ADDX / SUBX
            u32 r;
            if (mode == 0) {
                u32 s = d[reg] & MASK[sz], v = d[dn] & MASK[sz];
                r = grp == 0xD ? add(s, v, sz, fX, true) : sub(s, v, sz, fX, true, true);
                setD(dn, sz, r);
                if (sz == 2) cyc += 4;
            } else {
                u32 s = rd(ea(4, reg, sz, true), sz);
                u32 addr = ea(4, dn, sz, true);
                u32 v = rd(addr, sz);
                wr(addr, sz, grp == 0xD ? add(s, v, sz, fX, true) : sub(s, v, sz, fX, true, true));
                cyc += 2;
            }
            return;
        }
        if (grp == 0xB && opmode >= 4 && mode == 1) {   // CMPM
            u32 s = rd(ea(3, reg, sz), sz);
            u32 v = rd(ea(3, dn, sz), sz);
            sub(s, v, sz, 0, false, false);
            return;
        }
    }

    if (opmode < 4) {                                   // <ea>,Dn
        NEED((grp == 0x8 || grp == 0xC || sz == 0) ? EA_DATA : EA_ALL);
        u32 s = readEA(mode, reg, sz);
        u32 v = d[dn] & MASK[sz];
        bool fromMem = mode >= 2 && !(mode == 7 && reg == 4);
        u32 r;
        switch (grp) {
        case 0x8: r = v | s; logicFlags(r, sz); break;
        case 0xC: r = v & s; logicFlags(r, sz); break;
        case 0x9: r = sub(s, v, sz, 0, false, true); break;
        case 0xD: r = add(s, v, sz, 0, false); break;
        default:
            sub(s, v, sz, 0, false, false);
            if (sz == 2) cyc += 2;
            return;
        }
        setD(dn, sz, r);
        if (sz == 2) cyc += fromMem ? 2 : 4;
        return;
    }

    // Dn,<ea>
    if (grp == 0xB && mode == 0) {                      // EOR Dn,Dn
        u32 r = (d[reg] ^ d[dn]) & MASK[sz];
        logicFlags(r, sz);
        setD(reg, sz, r);
        if (sz == 2) cyc += 4;
        return;
    }
    NEED(EA_MEMALT);
    u32 addr = ea(mode, reg, sz);
    u32 v = rd(addr, sz);
    u32 s = d[dn] & MASK[sz];
    u32 r;
    switch (grp) {
    case 0x8: r = v | s; logicFlags(r, sz); break;
    case 0xC: r = v & s; logicFlags(r, sz); break;
    case 0x9: r = sub(s, v, sz, 0, false, true); break;
    case 0xD: r = add(s, v, sz, 0, false); break;
    default: r = v ^ s; logicFlags(r, sz); break;
    }
    wr(addr, sz, r);
}

void M68K::groupE(u16 op)
{
    int mode = (op >> 3) & 7, reg = op & 7;
    int sz = (op >> 6) & 3;
    bool left = (op & 0x100) != 0;

    if (sz == 3) {                                      // memory, one bit
        if (op & 0x800) {
            if (model) bitfield(op); else illegal();
            return;
        }
        NEED(EA_MEMALT);
        u32 addr = ea(mode, reg, 1);
        wr(addr, 1, shift((op >> 9) & 3, left, rd(addr, 1), 1, 1));
        return;
    }
    int count = (op >> 9) & 7;
    if (op & 0x20) count = (int)(d[count] & 63);
    else if (!count) count = 8;
    setD(reg, sz, shift((op >> 3) & 3, left, d[reg], count, sz));
    cyc += (sz == 2 ? 4 : 2) + 2 * count;
}

// ---------------------------------------------------------------- 68020+

void M68K::rte()
{
    u32 sp = a[7];
    u16 sr = (u16)rd(sp, 1);
    u32 newPC = rd(sp + 2, 2);
    if (model == MC68000) {
        a[7] = sp + 6;
    } else {
        u16 fv = (u16)rd(sp + 6, 1);
        static const u8 kFrameBytes[16] = {8, 8, 12, 12, 16, 0, 0, 60, 0, 20, 32, 92, 0, 0, 0, 0};
        u32 n = kFrameBytes[fv >> 12];
        if (!n) { exception(14, instPC, 10); return; }   // format error
        a[7] = sp + n;
        if ((fv >> 12) == 1) {                          // throwaway frame: RTE again from the new stack
            setSR(sr & srMask(model));
            rte();
            return;
        }
    }
    pc = newPC;
    setSR(sr & srMask(model));
    cyc += 4;
    traceNext = false;
}

void M68K::trapcc(u16 op)
{
    switch (op & 7) {
    case 2: fetch16(); break;
    case 3: fetch32(); break;
    default: break;
    }
    if (cond((op >> 8) & 15)) exception(7, pc, 10, 2);
}

void M68K::movec(u16 op)
{
    if (!fS) { privilege(); return; }
    u16 ext = fetch16();
    int r = (ext >> 12) & 15;
    u32 &reg = r < 8 ? d[r] : a[r - 8];
    bool toControl = op & 1;
    int cr = ext & 0xFFF;
    u32 v = toControl ? reg : 0;

    switch (cr) {
    case 0x000: if (toControl) sfc = v & 7; else v = sfc; break;
    case 0x001: if (toControl) dfc = v & 7; else v = dfc; break;
    case 0x002: {
        u32 m = model == MC68020 ? 0x3 : model == MC68030 ? 0x3313 : 0x80008000;
        if (toControl) cacr = v & m; else v = cacr;
        break;
    }
    case 0x800:
        if (toControl) usp = v; else v = usp;           // supervisor mode: USP is never in A7
        break;
    case 0x801: if (toControl) vbr = v; else v = vbr; break;
    case 0x802:
        if (model >= MC68040) { illegal(); return; }
        if (toControl) caar = v; else v = caar;
        break;
    case 0x803:
        if (toControl) { if (fM) a[7] = v; else msp = v; } else v = fM ? a[7] : msp;
        break;
    case 0x804:
        if (toControl) { if (!fM) a[7] = v; else isp = v; } else v = !fM ? a[7] : isp;
        break;
    default:
        // 68040 MMU registers (TC, ITTx, DTTx, MMUSR, URP, SRP)
        if (model >= MC68040 && ((cr >= 0x003 && cr <= 0x007) || (cr >= 0x805 && cr <= 0x807)) && mmuUnit) {
            if (!mmuControl(cr, v, toControl)) { illegal(); return; }
            break;
        }
        illegal();
        return;
    }
    if (!toControl) reg = v;
}

void M68K::moves(u16 op)
{
    if (!fS) { privilege(); return; }
    int mode = (op >> 3) & 7, reg = op & 7, sz = (op >> 6) & 3;
    u16 ext = fetch16();
    NEED(EA_MEMALT);
    int r = (ext >> 12) & 15;
    if (ext & 0x800) {                                  // register to memory
        u32 v = r < 8 ? d[r] : a[r - 8];
        u32 addr = ea(mode, reg, sz);
        wr(addr, sz, v);
    } else {
        u32 v = rd(ea(mode, reg, sz), sz);
        if (r < 8) setD(r, sz, v); else a[r - 8] = (u32)sext(v, sz);
    }
}

void M68K::mulLong(u16 op)
{
    int mode = (op >> 3) & 7, reg = op & 7;
    u16 ext = fetch16();
    NEED(EA_DATA);
    u32 s = readEA(mode, reg, 2);
    int dl = (ext >> 12) & 7, dh = ext & 7;
    bool isSigned = (ext & 0x800) != 0, quad = (ext & 0x400) != 0;
    u64 r = isSigned ? (u64)((s64)(s32)s * (s64)(s32)d[dl]) : (u64)s * d[dl];
    u32 lo = (u32)r, hi = (u32)(r >> 32);
    fC = 0;
    if (quad) {
        d[dh] = hi;
        d[dl] = lo;
        fN = hi >> 31;
        fZ = r == 0;
        fV = 0;
    } else {
        d[dl] = lo;
        fN = lo >> 31;
        fZ = lo == 0;
        fV = isSigned ? ((s64)r != (s64)(s32)lo) : hi != 0;
    }
    cyc += 40;
}

void M68K::divLong(u16 op)
{
    int mode = (op >> 3) & 7, reg = op & 7;
    u16 ext = fetch16();
    NEED(EA_DATA);
    u32 divisor = readEA(mode, reg, 2);
    int dq = (ext >> 12) & 7, dr = ext & 7;
    bool isSigned = (ext & 0x800) != 0, quad = (ext & 0x400) != 0;
    cyc += 70;
    if (divisor == 0) {
        fC = 0;
        exception(5, pc, 14, 2);
        return;
    }
    fC = 0;
    if (!isSigned) {
        u64 dividend = quad ? ((u64)d[dr] << 32) | d[dq] : d[dq];
        u64 q = dividend / divisor, rem = dividend % divisor;
        if (q > 0xFFFFFFFFull) { fV = 1; fN = 1; fZ = 0; return; }
        fV = 0;
        d[dr] = (u32)rem;
        d[dq] = (u32)q;
        fN = (u32)q >> 31;
        fZ = q == 0;
        return;
    }
    s64 dividend = quad ? (s64)(((u64)d[dr] << 32) | d[dq]) : (s64)(s32)d[dq];
    s64 dv = (s32)divisor;
    if (quad && dividend == (s64)0x8000000000000000ull && dv == -1) { fV = 1; fN = 1; fZ = 0; return; }
    s64 q = dividend / dv, rem = dividend % dv;
    if (q > 0x7FFFFFFFll || q < -0x80000000ll) { fV = 1; fN = 1; fZ = 0; return; }
    fV = 0;
    d[dr] = (u32)rem;
    d[dq] = (u32)q;
    fN = ((u32)q) >> 31;
    fZ = q == 0;
}

// CMP2 / CHK2: Rn - lower <= upper - lower, compared without sign, covers
// both signed and unsigned bounds the way the hardware does.
void M68K::chk2(u16 op)
{
    int mode = (op >> 3) & 7, reg = op & 7, sz = (op >> 9) & 3;
    u16 ext = fetch16();
    NEED(EA_CTRL);
    u32 addr = ea(mode, reg, sz);
    u32 lower = rd(addr, sz), upper = rd(addr + kSize[sz], sz);
    int r = (ext >> 12) & 15;
    u32 v, m;
    if (r >= 8) {
        v = a[r - 8];
        lower = (u32)sext(lower, sz);
        upper = (u32)sext(upper, sz);
        m = 0xFFFFFFFF;
    } else {
        m = MASK[sz];
        v = d[r] & m;
    }
    fZ = v == lower || v == upper;
    fC = ((v - lower) & m) > ((upper - lower) & m);
    cyc += 10;
    if (fC && (ext & 0x800)) exception(6, pc, 30, 2);
}

void M68K::cas(u16 op)
{
    int mode = (op >> 3) & 7, reg = op & 7, sz = ((op >> 9) & 3) - 1;
    u16 ext = fetch16();
    NEED(EA_MEMALT);
    int du = (ext >> 6) & 7, dc = ext & 7;
    u32 addr = ea(mode, reg, sz);
    u32 m = rd(addr, sz);
    sub(d[dc] & MASK[sz], m, sz, 0, false, false);
    if (fZ) wr(addr, sz, d[du]);
    else setD(dc, sz, m);
}

void M68K::cas2(u16 op)
{
    int sz = ((op >> 9) & 3) - 1;
    u16 e1 = fetch16(), e2 = fetch16();
    int r1 = (e1 >> 12) & 15, r2 = (e2 >> 12) & 15;
    u32 addr1 = r1 < 8 ? d[r1] : a[r1 - 8];
    u32 addr2 = r2 < 8 ? d[r2] : a[r2 - 8];
    int dc1 = e1 & 7, dc2 = e2 & 7, du1 = (e1 >> 6) & 7, du2 = (e2 >> 6) & 7;
    u32 m1 = rd(addr1, sz), m2 = rd(addr2, sz);
    sub(d[dc1] & MASK[sz], m1, sz, 0, false, false);
    if (fZ) {
        sub(d[dc2] & MASK[sz], m2, sz, 0, false, false);
        if (fZ) {
            wr(addr1, sz, d[du1]);
            wr(addr2, sz, d[du2]);
            return;
        }
    }
    setD(dc1, sz, m1);
    setD(dc2, sz, m2);
}

void M68K::packUnpk(u16 op)
{
    int rx = op & 7, ry = (op >> 9) & 7;
    bool memory = (op & 8) != 0;
    bool pack = ((op >> 6) & 7) == 5;
    u16 adj = fetch16();
    if (pack) {
        u32 src;
        if (memory) {
            u32 lo = rd(ea(4, rx, 0, true), 0);
            u32 hi = rd(ea(4, rx, 0, true), 0);
            src = (hi << 8) | lo;
        } else {
            src = d[rx];
        }
        u32 v = (src + adj) & 0xFFFF;
        u32 b = ((v >> 4) & 0xF0) | (v & 0x0F);
        if (memory) wr(ea(4, ry, 0, true), 0, b); else setD(ry, 0, b);
    } else {
        u32 src = memory ? rd(ea(4, rx, 0, true), 0) : d[rx] & 0xFF;
        u32 v = ((((src << 4) & 0x0F00) | (src & 0x0F)) + adj) & 0xFFFF;
        if (memory) {
            wr(ea(4, ry, 0, true), 0, v & 0xFF);
            wr(ea(4, ry, 0, true), 0, v >> 8);
        } else {
            setD(ry, 1, v);
        }
    }
    cyc += 6;
}

void M68K::bitfield(u16 op)
{
    int mode = (op >> 3) & 7, reg = op & 7, kind = (op >> 8) & 7;
    u16 ext = fetch16();
    // BFTST, BFEXTU, BFEXTS and BFFFO only read; Dn and control modes allowed.
    bool readOnly = kind == 0 || kind == 1 || kind == 3 || kind == 5;
    if (mode != 0) NEED(readOnly ? EA_CTRL : EA_CTRLALT);
    s32 offset = (ext & 0x800) ? (s32)d[(ext >> 6) & 7] : (s32)((ext >> 6) & 31);
    u32 width = (ext & 0x20) ? d[ext & 7] : ext;
    width = ((width - 1) & 31) + 1;
    u32 wmask = width == 32 ? 0xFFFFFFFF : (1u << width) - 1;
    int dn = (ext >> 12) & 7;

    u32 field;
    u32 addr = 0;
    u64 data64 = 0;
    int bitOff = 0;
    if (mode == 0) {
        u32 off = (u32)offset & 31;
        u32 rot = off ? (d[reg] << off) | (d[reg] >> (32 - off)) : d[reg];
        field = rot >> (32 - width);
        if (width == 32) field = rot;
    } else {
        addr = ea(mode, reg, 0) + (u32)(offset >> 3);
        bitOff = offset & 7;
        data64 = (u64)rd(addr, 2) << 8;
        if (bitOff + width > 32) data64 |= rd(addr + 4, 0);
        field = (u32)(data64 >> (40 - bitOff - width)) & wmask;
    }

    u32 newField = field;
    switch (kind) {
    case 2: newField = ~field & wmask; break;           // BFCHG
    case 4: newField = 0; break;                        // BFCLR
    case 6: newField = wmask; break;                    // BFSET
    case 7: newField = d[dn] & wmask; break;            // BFINS
    default: break;
    }
    u32 flagsFrom = kind == 7 ? newField : field;
    fN = (flagsFrom >> (width - 1)) & 1;
    fZ = flagsFrom == 0;
    fV = fC = 0;

    switch (kind) {
    case 1: d[dn] = field; break;                       // BFEXTU
    case 3: d[dn] = width == 32 ? field : (field ^ (1u << (width - 1))) - (1u << (width - 1)); break;
    case 5: {                                           // BFFFO
        u32 n = mode == 0 ? ((u32)offset & 31) : (u32)offset;
        for (u32 bit = 1u << (width - 1); bit && !(field & bit); bit >>= 1) n++;
        d[dn] = n;
        break;
    }
    default: break;
    }
    cyc += 12;

    if (readOnly) return;
    if (mode == 0) {
        u32 off = (u32)offset & 31;
        u32 m = width == 32 ? 0xFFFFFFFF : wmask << (32 - width);
        u32 v = width == 32 ? newField : newField << (32 - width);
        m = off ? (m >> off) | (m << (32 - off)) : m;
        v = off ? (v >> off) | (v << (32 - off)) : v;
        d[reg] = (d[reg] & ~m) | (v & m);
    } else {
        int shiftBy = 40 - bitOff - (int)width;
        u64 m = (u64)wmask << shiftBy;
        data64 = (data64 & ~m) | ((u64)newField << shiftBy);
        wr(addr, 2, (u32)(data64 >> 8));
        if (bitOff + width > 32) wr(addr + 4, 0, (u32)data64 & 0xFF);
    }
}

void M68K::move16(u16 op)
{
    u32 src, dst;
    int ry = op & 7;
    if ((op & 0xFFF8) == 0xF620) {                      // MOVE16 (Ax)+,(Ay)+
        u16 ext = fetch16();
        int rx = (ext >> 12) & 7;
        src = a[ry] & ~15u;
        dst = a[rx] & ~15u;
        a[ry] += 16;
        if (rx != ry) a[rx] += 16;
    } else {
        u32 abs = fetch32() & ~15u;
        u32 an = a[ry] & ~15u;
        switch ((op >> 3) & 3) {
        case 0: src = an; dst = abs; a[ry] += 16; break;   // (Ay)+,abs
        case 1: src = abs; dst = an; a[ry] += 16; break;   // abs,(Ay)+
        case 2: src = an; dst = abs; break;                // (Ay),abs
        default: src = abs; dst = an; break;               // abs,(Ay)
        }
    }
    for (u32 i = 0; i < 16; i += 4) wr(dst + i, 2, rd(src + i, 2));
}

void M68K::lineF(u16 op)
{
    int cpid = (op >> 9) & 7;
    if (model >= MC68040) {
        if ((op & 0xFFE0) == 0xF620 || (op & 0xFFE0) == 0xF600) { move16(op); return; }
        if ((op & 0xFF00) == 0xF400) {                  // CINV / CPUSH: caches are not modelled
            if (!fS) { privilege(); return; }
            return;
        }
        if ((op & 0xFF00) == 0xF500) {                  // PFLUSH / PTEST
            if (!fS) { privilege(); return; }
            if (mmuUnit && mmuUnit->execute(*this, op)) return;
            return;
        }
    }
    if (cpid == 1 && fpu) {
        if (fpu->execute(*this, op)) return;
    } else if (cpid == 0 && model == MC68030 && mmuUnit) {
        if (!fS) { privilege(); return; }
        if (mmuUnit->execute(*this, op)) return;
    }
    illegal();
}

bool M68K::mmuControl(int cr, u32 &v, bool write)
{
    return mmuUnit && mmuUnit->control(cr, v, write);
}

#include "m68kfast.inc"

} // namespace macemu

namespace macemu {
// The checkpoint's block copies rely on this member layout.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winvalid-offsetof"
void M68K::checkLayout()
{
    static_assert(__builtin_offsetof(M68K, a) == __builtin_offsetof(M68K, d) + 32, "d[] then a[]");
    static_assert(__builtin_offsetof(M68K, imask) == __builtin_offsetof(M68K, usp) + 44, "usp ... imask in order");
}
#pragma GCC diagnostic pop
} // namespace macemu
