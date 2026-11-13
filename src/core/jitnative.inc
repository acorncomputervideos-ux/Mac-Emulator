// The JIT's own ARM code for the commonest instructions (included by
// jit.cpp).  Each does exactly what its handler in m68kfast.inc does, in
// the same order: the same register and memory changes, flags and cycle
// counts.  Memory goes through the CPU's TLB as M68K::rd/wr do, inline;
// anything else (a TLB miss, a device, a page crossing) calls rdSlow /
// wrSlow, which count a slow access, so the block stops after the
// instruction just as for a handler.
//
// Supported: MOVEQ; MOVE/MOVEA; ADDQ/SUBQ; LEA; TST; CLR; OR/AND/SUB/ADD/
// CMP <ea>,Dn; ADDA/SUBA/CMPA; OR/SUB/EOR/AND/ADD Dn,<ea>; ORI/ANDI/SUBI/
// ADDI/EORI/CMPI.  Addressing modes: Dn, An, (An), (An)+, -(An), d16(An),
// abs.W, abs.L, d16(PC) and #imm, where the handler takes them.

namespace {

using namespace a64;

struct NativeGen {
    Emitter &e;
    const u8 *h;                        // host address of the opcode
    u32 pc;                             // its address
    u32 words = 0;                      // extension words taken so far
    u32 extra = 0;                      // fixed cycles (fetches, the handler's own)

    NativeGen(Emitter &em, const u8 *host, u32 at) : e(em), h(host), pc(at) {}

    u16 ext() { u16 w = (u16)((h[2 + 2 * words] << 8) | h[3 + 2 * words]); words++; extra += 4; return w; }
    u32 extAddr() const { return pc + 2 + 2 * words; }  // where the next extension word is

    // Registers: w9 the instruction's cycles so far, w0 values, w1
    // addresses, w10-w12 values kept across a memory access.
    enum { kCyc = 9 };

    static u32 oD(int r) { return JitAccess::oD + 4 * (u32)r; }
    static u32 oA(int r) { return JitAccess::oA + 4 * (u32)r; }
    static u32 bits(int sz) { return sz == 0 ? 8 : sz == 1 ? 16 : 32; }

    void mask(int rd, int sz) { if (sz < 2) e.ubfxw(rd, rd, 0, bits(sz)); }

    // A call to rdSlow/wrSlow: w9 to cyc, w10-w12 kept on the stack.
    void slowCall(void *fn, int sz)
    {
        e.strw(kCyc, 19, JitAccess::oCyc);
        e.stp(10, 11, SP, 96);
        e.strx(12, SP, 112);
        e.movrx(0, 19);                                 // (w1: address; w2: size; w3: value)
        e.movw(2, (u32)sz);
        e.movx(16, (u64)(unsigned long)fn);
        e.blr(16);
        e.ldp(10, 11, SP, 96);
        e.ldrx(12, SP, 112);
        e.ldrw(kCyc, 19, JitAccess::oCyc);
    }

    // The TLB entry for w1 into x3, and w2 = w1's offset in the page;
    // branches to `slow` (patched by the caller) if not usable.
    void tlb(int sz, bool write, u32 **slow, int &nSlow)
    {
        e.ubfxw(2, 1, 0, 12);
        e.cmpwi(2, 0x1000 - (1u << sz));
        slow[nSlow++] = e.bcond(HI);
        e.ubfxw(3, 1, 12, 9);                           // kTlbSize 512
        e.addxLsl(3, 3, 3, 1);
        e.addxLsl(3, 24, 3, 3);                         // x24: &tlb[0]; entries of 24 bytes
        e.ldrw(4, 3, JitAccess::oTlbTag);
        e.lsrwi(5, 1, 12);
        e.lslwi(5, 5, 12);
        e.orrw(5, 5, 28);                               // w28: 1 | fS << 1
        e.cmpw(4, 5);
        slow[nSlow++] = e.bcond(NE);
        if (write) {
            e.ldrb(4, 3, JitAccess::oTlbWritable);
            slow[nSlow++] = e.cbz(4);
        }
        e.ldrx(7, 3, JitAccess::oTlbPtr);
    }

    // w0 = the value at w1 (as M68K::rd).
    void read(int sz)
    {
        u32 *slow[4];
        int n = 0;
        tlb(sz, false, slow, n);
        if (sz == 0) e.ldrbR(0, 7, 2);
        else if (sz == 1) { e.ldrhR(0, 7, 2); e.rev16w(0, 0); }
        else { e.ldrwR(0, 7, 2); e.revw(0, 0); }
        e.addwi(kCyc, kCyc, 4);
        u32 *done = e.b();
        for (int i = 0; i < n; i++) Emitter::patch(slow[i], e.here());
        slowCall((void *)&JitAccess::rdSlow, sz);
        Emitter::patch(done, e.here());
    }

    // The value w0 to w1 (as M68K::wr).
    void write(int sz)
    {
        u32 *slow[4];
        int n = 0;
        tlb(sz, true, slow, n);
        if (sz == 0) e.strbR(0, 7, 2);
        else if (sz == 1) { e.rev16w(4, 0); e.strhR(4, 7, 2); }
        else { e.revw(4, 0); e.strwR(4, 7, 2); }
        e.addwi(kCyc, kCyc, 4);
        u32 *done = e.b();
        for (int i = 0; i < n; i++) Emitter::patch(slow[i], e.here());
        e.movrw(3, 0);
        slowCall((void *)&JitAccess::wrSlow, sz);
        Emitter::patch(done, e.here());
    }

    // Which modes this generator takes (mode 7: reg 0 abs.W, 1 abs.L,
    // 2 d16(PC), 4 #imm).
    static bool modeOk(int mode, int reg, bool data)
    {
        if (mode <= 5) return true;
        if (mode != 7) return false;
        return reg == 0 || reg == 1 || (reg == 2 && data) || (reg == 4 && data);
    }

    // The effective address of a memory mode into w1 (as M68K::ea and
    // eaT, with their register updates and cycles).
    void address(int mode, int reg, int sz, bool noPenalty = false)
    {
        u32 inc = sz == 0 ? (reg == 7 ? 2u : 1u) : sz == 1 ? 2u : 4u;
        switch (mode) {
        case 2: e.ldrw(1, 19, oA(reg)); break;
        case 3: e.ldrw(1, 19, oA(reg)); e.addwi(2, 1, inc); e.strw(2, 19, oA(reg)); break;
        case 4: e.ldrw(1, 19, oA(reg)); e.subwi(1, 1, inc); e.strw(1, 19, oA(reg)); if (!noPenalty) extra += 2; break;
        case 5: { u32 d = (u32)(s32)(s16)ext(); e.ldrw(1, 19, oA(reg)); e.movw(2, d); e.addw(1, 1, 2); break; }
        default:
            if (reg == 0) e.movw(1, (u32)(s32)(s16)ext());
            else if (reg == 1) { u32 hi = ext(); e.movw(1, (hi << 16) | ext()); }
            else { u32 base = extAddr(); e.movw(1, base + (u32)(s32)(s16)ext()); }
            break;
        }
    }

    // An operand's value into w0, masked to its size (as readT / readEA).
    void operand(int mode, int reg, int sz)
    {
        if (mode == 0) { e.ldrw(0, 19, oD(reg)); mask(0, sz); return; }
        if (mode == 1) { e.ldrw(0, 19, oA(reg)); mask(0, sz); return; }
        if (mode == 7 && reg == 4) {
            u32 v = sz == 2 ? (u32)ext() << 16 : 0;
            v |= ext();
            if (sz == 0) v &= 0xFF;
            e.movw(0, v);
            return;
        }
        address(mode, reg, sz);
        read(sz);
    }

    void storeFlag(int reg, u32 off) { e.strw(reg, 19, off); }

    // N and Z of w0 (masked), V = C = 0 (logicFlags).
    void logicFlags(int sz)
    {
        e.lslwi(2, 0, 32 - bits(sz));
        e.cmpwi(2, 0);
        e.csetw(3, MI); storeFlag(3, JitAccess::oFN);
        e.csetw(3, EQ); storeFlag(3, JitAccess::oFZ);
        storeFlag(ZR, JitAccess::oFV);
        storeFlag(ZR, JitAccess::oFC);
    }

    // w0 = dd (wD) + s (wS) with M68K::add's flags (not X-form).
    void add(int wS, int wD, int sz)
    {
        u32 sh = 32 - bits(sz);
        e.lslwi(4, wS, sh); e.lslwi(5, wD, sh);
        e.addsw(6, 4, 5);
        e.csetw(3, HS); storeFlag(3, JitAccess::oFC); storeFlag(3, JitAccess::oFX);
        e.csetw(3, VS); storeFlag(3, JitAccess::oFV);
        e.csetw(3, MI); storeFlag(3, JitAccess::oFN);
        e.csetw(3, EQ); storeFlag(3, JitAccess::oFZ);
        e.lsrwi(0, 6, sh);
    }

    // w0 = dd (wD) - s (wS) with M68K::sub's flags (not X-form).
    void sub(int wS, int wD, int sz, bool setX)
    {
        u32 sh = 32 - bits(sz);
        e.lslwi(4, wS, sh); e.lslwi(5, wD, sh);
        e.subsw(6, 5, 4);
        e.csetw(3, LO); storeFlag(3, JitAccess::oFC);
        if (setX) storeFlag(3, JitAccess::oFX);
        e.csetw(3, VS); storeFlag(3, JitAccess::oFV);
        e.csetw(3, MI); storeFlag(3, JitAccess::oFN);
        e.csetw(3, EQ); storeFlag(3, JitAccess::oFZ);
        e.lsrwi(0, 6, sh);
    }

    // Dn's low bits from w0 (setD).
    void setD(int reg, int sz)
    {
        if (sz == 2) { e.strw(0, 19, oD(reg)); return; }
        e.ldrw(2, 19, oD(reg));
        e.movw(3, sz == 0 ? 0xFFFFFF00u : 0xFFFF0000u);
        e.andw(2, 2, 3);
        e.orrw(2, 2, 0);
        e.strw(2, 19, oD(reg));
    }

    // The instruction, if supported; false (and nothing emitted) if not.
    // `len` gets its length in bytes.
    bool translate(u16 op, u32 &len);
};

// Plans first (decode only), so that nothing is emitted for an
// instruction it then turns down.
static bool nativeSupported(u16 op)
{
    int top = op >> 12, mode = (op >> 3) & 7, reg = op & 7;
    if (top == 7) return !(op & 0x100);                                         // MOVEQ
    if (top >= 1 && top <= 3) {                                                 // MOVE
        int dmode = (op >> 6) & 7, dreg = (op >> 9) & 7;
        return NativeGen::modeOk(mode, reg, true) && (dmode <= 5 || (dmode == 7 && dreg <= 1));
    }
    if (top == 5) {                                                             // ADDQ / SUBQ
        int sz = (op >> 6) & 3;
        return sz != 3 && (mode <= 5 || (mode == 7 && reg <= 1));
    }
    if ((op & 0xF1C0) == 0x41C0) return (mode == 2 || mode == 5 || (mode == 7 && reg <= 2));   // LEA
    if ((op & 0xFF00) == 0x4A00 && ((op >> 6) & 3) != 3) return NativeGen::modeOk(mode, reg, true);   // TST
    if ((op & 0xFF00) == 0x4200 && ((op >> 6) & 3) != 3) return mode != 1 && (mode <= 5 || (mode == 7 && reg <= 1));   // CLR
    if (top == 0x8 || top == 0x9 || top == 0xB || top == 0xC || top == 0xD) {
        int opmode = (op >> 6) & 7, sz = opmode & 3;
        if (sz == 3) return top != 0x8 && top != 0xC && NativeGen::modeOk(mode, reg, true);   // ADDA SUBA CMPA
        if (opmode < 4) return NativeGen::modeOk(mode, reg, true);              // <ea>,Dn
        if (top == 0xB && mode == 0) return true;                               // EOR Dn,Dn
        return mode >= 2 && (mode <= 5 || (mode == 7 && reg <= 1));             // Dn,<ea>
    }
    if (top == 0 && !(op & 0x100)) {                                            // the immediates
        int kind = (op >> 9) & 7;
        if (kind == 4 || kind == 7) return false;
        return mode == 0 || (mode >= 2 && mode <= 5) || (mode == 7 && reg <= 1);
    }
    return false;
}

bool NativeGen::translate(u16 op, u32 &len)
{
    if (!nativeSupported(op)) return false;
    int top = op >> 12, mode = (op >> 3) & 7, reg = op & 7, rn = (op >> 9) & 7;

    if (top == 7) {                                                             // MOVEQ
        e.movw(0, (u32)(s32)(s8)op);
        e.strw(0, 19, oD(rn));
        logicFlags(2);
    } else if (top >= 1 && top <= 3) {                                          // MOVE / MOVEA
        int sz = top == 1 ? 0 : top == 3 ? 1 : 2, dmode = (op >> 6) & 7;
        operand(mode, reg, sz);
        if (dmode == 1) {
            if (sz == 1) e.sxthw(0, 0);
            e.strw(0, 19, oA(rn));
        } else {
            logicFlags(sz);
            if (dmode == 0) setD(rn, sz);
            else {
                e.movrw(10, 0);
                address(dmode, rn, sz, true);
                e.movrw(0, 10);
                write(sz);
            }
        }
    } else if (top == 5) {                                                      // ADDQ / SUBQ
        int sz = (op >> 6) & 3;
        bool subq = (op & 0x100) != 0;
        u32 q = (u32)rn ? (u32)rn : 8;
        if (mode == 1) {
            e.ldrw(0, 19, oA(reg));
            if (subq) e.subwi(0, 0, q); else e.addwi(0, 0, q);
            e.strw(0, 19, oA(reg));
            extra += 4;
        } else if (mode == 0) {
            e.ldrw(10, 19, oD(reg)); mask(10, sz);
            e.movw(11, q);
            if (subq) sub(11, 10, sz, true); else add(11, 10, sz);
            setD(reg, sz);
            if (sz == 2) extra += 4;
        } else {
            address(mode, reg, sz);
            e.movrw(11, 1);
            read(sz);
            e.movw(12, q);
            if (subq) sub(12, 0, sz, true); else add(12, 0, sz);
            e.movrw(1, 11);
            write(sz);
        }
    } else if ((op & 0xF1C0) == 0x41C0) {                                       // LEA
        address(mode, reg, 2);
        e.strw(1, 19, oA(rn));
    } else if ((op & 0xFF00) == 0x4A00) {                                       // TST
        int sz = (op >> 6) & 3;
        operand(mode, reg, sz);
        logicFlags(sz);
    } else if ((op & 0xFF00) == 0x4200) {                                       // CLR
        int sz = (op >> 6) & 3;
        e.movw(3, 1);
        storeFlag(ZR, JitAccess::oFN); storeFlag(ZR, JitAccess::oFV); storeFlag(ZR, JitAccess::oFC);
        storeFlag(3, JitAccess::oFZ);
        e.movw(0, 0);
        if (mode == 0) { setD(reg, sz); if (sz == 2) extra += 2; }
        else { address(mode, reg, sz); e.movw(0, 0); write(sz); extra += sz == 2 ? 8 : 4; }
    } else if (top == 0x8 || top == 0x9 || top == 0xB || top == 0xC || top == 0xD) {
        int opmode = (op >> 6) & 7, sz = opmode & 3;
        bool fromMem = mode >= 2 && !(mode == 7 && reg == 4);
        if (sz == 3) {                                                          // ADDA / SUBA / CMPA
            int osz = opmode == 7 ? 2 : 1;
            operand(mode, reg, osz);
            if (osz == 1) e.sxthw(0, 0);
            e.movrw(10, 0);
            e.ldrw(11, 19, oA(rn));
            if (top == 0xB) { sub(10, 11, 2, false); extra += 2; }
            else {
                if (top == 0xD) e.addw(0, 11, 10); else e.subw(0, 11, 10);
                e.strw(0, 19, oA(rn));
                extra += (osz == 1 || !fromMem) ? 4 : 2;
            }
        } else if (opmode < 4) {                                                // <ea>,Dn
            operand(mode, reg, sz);
            e.movrw(10, 0);
            e.ldrw(11, 19, oD(rn)); mask(11, sz);
            switch (top) {
            case 0x8: e.orrw(0, 11, 10); logicFlags(sz); break;
            case 0xC: e.andw(0, 11, 10); logicFlags(sz); break;
            case 0x9: sub(10, 11, sz, true); break;
            case 0xD: add(10, 11, sz); break;
            default: sub(10, 11, sz, false); if (sz == 2) extra += 2; break;
            }
            if (top != 0xB) { setD(rn, sz); if (sz == 2) extra += fromMem ? 2 : 4; }
        } else if (mode == 0) {                                                 // EOR Dn,Dn
            e.ldrw(0, 19, oD(reg)); e.ldrw(2, 19, oD(rn)); e.eorw(0, 0, 2); mask(0, sz);
            logicFlags(sz);
            setD(reg, sz);
            if (sz == 2) extra += 4;
        } else {                                                                // Dn,<ea>
            address(mode, reg, sz);
            e.movrw(11, 1);
            read(sz);
            e.ldrw(10, 19, oD(rn)); mask(10, sz);
            switch (top) {
            case 0x8: e.orrw(0, 0, 10); logicFlags(sz); break;
            case 0xC: e.andw(0, 0, 10); logicFlags(sz); break;
            case 0x9: e.movrw(12, 0); sub(10, 12, sz, true); break;
            case 0xD: e.movrw(12, 0); add(10, 12, sz); break;
            default: e.eorw(0, 0, 10); logicFlags(sz); break;
            }
            e.movrw(1, 11);
            write(sz);
        }
    } else {                                                                    // the immediates
        int kind = (op >> 9) & 7, sz = (op >> 6) & 3;
        u32 imm = sz == 2 ? (u32)ext() << 16 : 0;
        imm |= ext();
        if (sz == 0) imm &= 0xFF;
        if (mode == 0) { e.ldrw(0, 19, oD(reg)); mask(0, sz); }
        else { address(mode, reg, sz); e.movrw(11, 1); read(sz); }
        e.movrw(12, 0);
        e.movw(10, imm);
        switch (kind) {
        case 0: e.orrw(0, 12, 10); logicFlags(sz); break;
        case 1: e.andw(0, 12, 10); logicFlags(sz); break;
        case 2: sub(10, 12, sz, true); break;
        case 3: add(10, 12, sz); break;
        case 5: e.eorw(0, 12, 10); logicFlags(sz); break;
        default: sub(10, 12, sz, false); if (mode == 0 && sz == 2) extra += 2; break;
        }
        if (kind != 6) {
            if (mode == 0) { setD(reg, sz); if (sz == 2) extra += 4; }
            else { e.movrw(1, 11); write(sz); }
        }
    }
    len = 2 + 2 * words;
    return true;
}

} // namespace
