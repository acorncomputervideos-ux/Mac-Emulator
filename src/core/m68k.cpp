// Motorola 68000 interpreter.
//
// Timing model: every bus access (one word, or one byte) costs 4 clock
// cycles, and each instruction adds its documented internal cycles on top.
// Because a 68000 fetches exactly as many words as the instruction is long,
// this reproduces the official timing tables closely without a prefetch
// queue.  Address errors (odd word access) are not raised.

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
    for (int i = 0; i < 8; i++) d[i] = a[i] = 0;
    pc = 0;
    fC = fV = fZ = fN = fX = 0;
    fS = 1; fT = 0; imask = 7;
    irq = 0; stopped = false; otherSP = 0; totalCycles = 0; cyc = 0;
    instPC = 0; opcode = 0; traceNext = false;
}

void M68K::hardReset()
{
    for (int i = 0; i < 8; i++) d[i] = a[i] = 0;
    fC = fV = fZ = fN = fX = 0;
    fS = 1; fT = 0; imask = 7;
    stopped = false; otherSP = 0;
    a[7] = rd(0, 2);
    pc = rd(4, 2);
}

u16 M68K::getSR() const
{
    return (u16)((fT << 15) | (fS << 13) | (imask << 8) | (fX << 4) | (fN << 3) | (fZ << 2) | (fV << 1) | fC);
}

void M68K::setSR(u16 v)
{
    u32 newS = (v >> 13) & 1;
    if (newS != fS) {
        u32 t = a[7]; a[7] = otherSP; otherSP = t;
        fS = newS;
    }
    fT = (v >> 15) & 1;
    imask = (v >> 8) & 7;
    fX = (v >> 4) & 1; fN = (v >> 3) & 1; fZ = (v >> 2) & 1; fV = (v >> 1) & 1; fC = v & 1;
}

u32 M68K::rd(u32 addr, int sz)
{
    if (sz == 0) { cyc += 4; return bus->read8(addr); }
    if (sz == 1) { cyc += 4; return bus->read16(addr); }
    cyc += 8;
    u32 h = bus->read16(addr);
    return (h << 16) | bus->read16(addr + 2);
}

void M68K::wr(u32 addr, int sz, u32 v)
{
    if (sz == 0) { cyc += 4; bus->write8(addr, (u8)v); }
    else if (sz == 1) { cyc += 4; bus->write16(addr, (u16)v); }
    else { cyc += 8; bus->write16(addr, (u16)(v >> 16)); bus->write16(addr + 2, (u16)v); }
}

u32 M68K::index(u32 base)
{
    u16 ext = fetch16();
    cyc += 2;
    u32 xn = (ext & 0x8000) ? a[(ext >> 12) & 7] : d[(ext >> 12) & 7];
    if (!(ext & 0x800)) xn = (u32)(s32)(s16)xn;
    return base + (u32)(s32)(s8)ext + xn;
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

void M68K::exception(int vec, u32 pushPC, int extraCycles)
{
    u16 old = getSR();
    if (!fS) { u32 t = a[7]; a[7] = otherSP; otherSP = t; fS = 1; }
    fT = 0;
    push32(pushPC);
    push16(old);
    pc = rd((u32)vec * 4, 2);
    cyc += extraCycles;
    traceNext = false;
}

void M68K::illegal()
{
    int grp = opcode >> 12;
    exception(grp == 0xA ? 10 : grp == 0xF ? 11 : 4, instPC, 10);
}

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
    cyc = 0;
    if (irq > (int)imask || irq == 7) {
        int level = irq;
        stopped = false;
        exception(24 + level, pc, 24);
        imask = (u32)level;
        bus->irqAck(level);
        totalCycles += (u32)cyc;
        return cyc;
    }
    if (stopped) {
        totalCycles += 4;
        return 4;
    }
    traceNext = fT != 0;
    instPC = pc;
    opcode = fetch16();
    exec(opcode);
    if (traceNext) exception(9, pc, 10);
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

    default:                                            // A-line, F-line
        illegal();
        return;
    }
}

void M68K::group0(u16 op)
{
    int mode = (op >> 3) & 7, reg = op & 7;

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
        setSR(sr & 0xA71F);
        cyc += 12;
        return;
    }

    NEED(EA_DATAALT);
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
            if (v < 0) { fN = 1; exception(6, pc, 16); return; }
            if (v > bound) { fN = 0; exception(6, pc, 14); return; }
            cyc += 6;
            return;
        }
        illegal();
        return;
    }

    switch ((op >> 8) & 0xF) {
    case 0x0:
        NEED(EA_DATAALT);
        if (sz == 3) {                                  // MOVE from SR
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
            setSR(v & 0xA71F);
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
        NEED(sz == 0 ? EA_DATAALT : EA_ALT);            // TST
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
        if (op & 8) a[reg] = otherSP; else otherSP = a[reg];
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
            setSR(fetch16() & 0xA71F);
            stopped = true;
            cyc -= 4;
            return;
        }
        case 3: {                                       // RTE
            if (!fS) { privilege(); return; }
            u16 sr = pop16();
            pc = pop32();
            setSR(sr & 0xA71F);
            cyc += 4;
            traceNext = false;
            return;
        }
        case 5: pc = pop32(); cyc += 4; return;         // RTS
        case 6:                                         // TRAPV
            if (fV) exception(7, pc, 10);
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
                exception(5, pc, 14);
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
        if (op & 0x800) { illegal(); return; }
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

} // namespace macemu
