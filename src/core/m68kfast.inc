// Fast dispatch for the 68000-68040 interpreter (included by m68k.cpp).
//
// Every one of the 65536 opcodes gets a handler in a table, chosen once.
// The instructions Mac OS spends its time on (moves, branches, compares,
// arithmetic, LINK/UNLK/RTS/JSR, ...) get handlers specialised at compile
// time for their size and addressing modes, so nothing is decoded while
// running.  Each handler does exactly what the general code in exec() does,
// in the same order, with the same cycle counts; every other opcode goes to
// exec().  A boot with and without the table must give identical results.

struct M68KFast {
    typedef M68K::Handler Handler;

    // ---------------------------------------------------------------- operands

    static inline u32 incFor(int reg, int sz) { return sz == 0 ? (reg == 7 ? 2u : 1u) : sz == 1 ? 2u : 4u; }

    template <int M>
    static inline u32 eaT(M68K &c, int reg, int sz, bool noPenalty = false)
    {
        if (M == 2) return c.a[reg];
        if (M == 3) { u32 ad = c.a[reg]; c.a[reg] += incFor(reg, sz); return ad; }
        if (M == 4) { c.a[reg] -= incFor(reg, sz); if (!noPenalty) c.cyc += 2; return c.a[reg]; }
        if (M == 5) { u32 dsp = (u32)(s32)(s16)c.fetch16(); return c.a[reg] + dsp; }
        return c.ea(M, reg, sz, noPenalty);
    }

    template <int M>
    static inline u32 readT(M68K &c, int reg, int sz)
    {
        if (M == 0) return c.d[reg] & MASK[sz];
        if (M == 1) return c.a[reg] & MASK[sz];
        if (M == 7) return c.readEA(7, reg, sz);
        return c.rd(eaT<M>(c, reg, sz), sz);
    }

    static void generic(M68K &c, u16 op) { c.exec(op); }

    // ---------------------------------------------------------------- MOVE / MOVEA

    template <int SZ, int SM, int DM>
    static void move(M68K &c, u16 op)
    {
        int reg = op & 7, dreg = (op >> 9) & 7;
        u32 v = readT<SM>(c, reg, SZ);
        if (DM == 1) { c.a[dreg] = (u32)sext(v, SZ); return; }
        c.logicFlags(v, SZ);
        if (DM == 0) c.setD(dreg, SZ, v);
        else c.wr(eaT<DM>(c, dreg, SZ, true), SZ, v);
    }

    // ---------------------------------------------------------------- branches

    template <int CC>
    static void bcc(M68K &c, u16 op)
    {
        u32 base = c.pc;
        s32 disp = (s8)op;
        bool wide = disp == 0;
        if (wide) disp = (s16)c.fetch16();
        else if (disp == -1 && c.model) { disp = (s32)c.fetch32(); wide = true; }
        if (CC == 1) {                                  // BSR
            c.push32(c.pc);
            c.pc = base + (u32)disp;
            c.cyc += wide ? 2 : 6;
            return;
        }
        if (c.cond(CC)) { c.pc = base + (u32)disp; c.cyc += wide ? 2 : 6; }
        else c.cyc += 4;
    }

    template <int CC>
    static void dbcc(M68K &c, u16 op)
    {
        int reg = op & 7;
        u32 base = c.pc;
        s32 disp = (s16)c.fetch16();
        if (c.cond(CC)) { c.cyc += 4; return; }
        u16 cnt = (u16)(c.d[reg] - 1);
        c.setD(reg, 1, cnt);
        if (cnt != 0xFFFF) { c.pc = base + (u32)disp; c.cyc += 2; }
        else c.cyc += 6;
    }

    template <int M>
    static void jmpjsr(M68K &c, u16 op)
    {
        int reg = op & 7;
        u32 addr = eaT<M>(c, reg, 2);
        if (M == 2 || M == 6 || (M == 7 && reg == 3)) c.cyc += 4;
        else if (M == 5 || (M == 7 && (reg == 0 || reg == 2))) c.cyc += 2;
        if (!(op & 0x40)) c.push32(c.pc);               // JSR (JMP has bit 6 set)
        c.pc = addr;
    }

    static void rts(M68K &c, u16) { c.pc = c.pop32(); c.cyc += 4; }
    static void nop(M68K &, u16) {}

    static void link(M68K &c, u16 op)
    {
        int reg = op & 7;
        u32 disp = (u32)(s32)(s16)c.fetch16();
        c.a[7] -= 4;
        c.wr(c.a[7], 2, c.a[reg]);
        c.a[reg] = c.a[7];
        c.a[7] += disp;
    }

    static void unlk(M68K &c, u16 op)
    {
        int reg = op & 7;
        c.a[7] = c.a[reg];
        c.a[reg] = c.pop32();
    }

    // ---------------------------------------------------------------- quick forms

    static void moveq(M68K &c, u16 op)
    {
        c.d[(op >> 9) & 7] = (u32)(s32)(s8)op;
        c.logicFlags(c.d[(op >> 9) & 7], 2);
    }

    template <int SZ, int M, bool SUB>
    static void addsubq(M68K &c, u16 op)
    {
        int reg = op & 7;
        u32 q = (op >> 9) & 7;
        if (!q) q = 8;
        if (M == 1) { c.a[reg] = SUB ? c.a[reg] - q : c.a[reg] + q; c.cyc += 4; return; }
        if (M == 0) {
            u32 v = c.d[reg] & MASK[SZ];
            c.setD(reg, SZ, SUB ? c.sub(q, v, SZ, 0, false, true) : c.add(q, v, SZ, 0, false));
            if (SZ == 2) c.cyc += 4;
            return;
        }
        u32 addr = eaT<M>(c, reg, SZ);
        u32 v = c.rd(addr, SZ);
        c.wr(addr, SZ, SUB ? c.sub(q, v, SZ, 0, false, true) : c.add(q, v, SZ, 0, false));
    }

    // ---------------------------------------------------------------- group 4

    template <int M>
    static void lea(M68K &c, u16 op)
    {
        int reg = op & 7;
        c.a[(op >> 9) & 7] = eaT<M>(c, reg, 2);
        if (M == 6 || (M == 7 && reg == 3)) c.cyc += 2;
    }

    template <int M>
    static void pea(M68K &c, u16 op)
    {
        int reg = op & 7;
        u32 addr = eaT<M>(c, reg, 2);
        if (M == 6 || (M == 7 && reg == 3)) c.cyc += 2;
        c.push32(addr);
    }

    template <int SZ, int M>
    static void tst(M68K &c, u16 op)
    {
        c.logicFlags(readT<M>(c, op & 7, SZ), SZ);
    }

    template <int SZ, int M>
    static void clr(M68K &c, u16 op)
    {
        int reg = op & 7;
        c.fN = c.fV = c.fC = 0; c.fZ = 1;
        if (M == 0) { c.setD(reg, SZ, 0); if (SZ == 2) c.cyc += 2; }
        else { c.wr(eaT<M>(c, reg, SZ), SZ, 0); c.cyc += SZ == 2 ? 8 : 4; }
    }

    static void movem(M68K &c, u16 op) { c.movem(op); }

    // ---------------------------------------------------------------- <ea>,Dn and Dn,<ea>

    // GRP: 0x8 OR, 0x9 SUB, 0xB CMP, 0xC AND, 0xD ADD.
    template <int GRP, int SZ, int M>
    static void eaToD(M68K &c, u16 op)
    {
        int reg = op & 7, dn = (op >> 9) & 7;
        u32 s = readT<M>(c, reg, SZ);
        u32 v = c.d[dn] & MASK[SZ];
        bool fromMem = M >= 2 && !(M == 7 && reg == 4);
        u32 r;
        switch (GRP) {
        case 0x8: r = v | s; c.logicFlags(r, SZ); break;
        case 0xC: r = v & s; c.logicFlags(r, SZ); break;
        case 0x9: r = c.sub(s, v, SZ, 0, false, true); break;
        case 0xD: r = c.add(s, v, SZ, 0, false); break;
        default:
            c.sub(s, v, SZ, 0, false, false);
            if (SZ == 2) c.cyc += 2;
            return;
        }
        c.setD(dn, SZ, r);
        if (SZ == 2) c.cyc += fromMem ? 2 : 4;
    }

    // GRP: 0x8 OR, 0x9 SUB, 0xB EOR, 0xC AND, 0xD ADD; M is a memory mode,
    // or 0 for EOR Dn,Dn.
    template <int GRP, int SZ, int M>
    static void dToEa(M68K &c, u16 op)
    {
        int reg = op & 7, dn = (op >> 9) & 7;
        if (M == 0) {                                   // EOR Dn,Dn
            u32 r = (c.d[reg] ^ c.d[dn]) & MASK[SZ];
            c.logicFlags(r, SZ);
            c.setD(reg, SZ, r);
            if (SZ == 2) c.cyc += 4;
            return;
        }
        u32 addr = eaT<M>(c, reg, SZ);
        u32 v = c.rd(addr, SZ);
        u32 s = c.d[dn] & MASK[SZ];
        u32 r;
        switch (GRP) {
        case 0x8: r = v | s; c.logicFlags(r, SZ); break;
        case 0xC: r = v & s; c.logicFlags(r, SZ); break;
        case 0x9: r = c.sub(s, v, SZ, 0, false, true); break;
        case 0xD: r = c.add(s, v, SZ, 0, false); break;
        default: r = v ^ s; c.logicFlags(r, SZ); break;
        }
        c.wr(addr, SZ, r);
    }

    // ADDA (GRP 0xD), SUBA (0x9), CMPA (0xB); OSZ 1 = word, 2 = long.
    template <int GRP, int OSZ, int M>
    static void addrArith(M68K &c, u16 op)
    {
        int reg = op & 7, dn = (op >> 9) & 7;
        u32 s = (u32)sext(readT<M>(c, reg, OSZ), OSZ);
        bool fromMem = M >= 2 && !(M == 7 && reg == 4);
        if (GRP == 0xB) { c.sub(s, c.a[dn], 2, 0, false, false); c.cyc += 2; return; }
        c.a[dn] = GRP == 0xD ? c.a[dn] + s : c.a[dn] - s;
        c.cyc += (OSZ == 1 || !fromMem) ? 4 : 2;
    }

    // ---------------------------------------------------------------- immediates

    // KIND: 0 ORI, 1 ANDI, 2 SUBI, 3 ADDI, 5 EORI, 6 CMPI (not to CCR/SR).
    template <int KIND, int SZ, int M>
    static void immediate(M68K &c, u16 op)
    {
        int reg = op & 7;
        u32 imm = SZ == 2 ? c.fetch32() : (c.fetch16() & MASK[SZ]);
        u32 addr = 0, v;
        if (M == 0) v = c.d[reg] & MASK[SZ];
        else { addr = eaT<M>(c, reg, SZ); v = c.rd(addr, SZ); }
        u32 r;
        switch (KIND) {
        case 0: r = v | imm; c.logicFlags(r, SZ); break;
        case 1: r = v & imm; c.logicFlags(r, SZ); break;
        case 2: r = c.sub(imm, v, SZ, 0, false, true); break;
        case 3: r = c.add(imm, v, SZ, 0, false); break;
        case 5: r = v ^ imm; c.logicFlags(r, SZ); break;
        default:
            c.sub(imm, v, SZ, 0, false, false);
            if (M == 0 && SZ == 2) c.cyc += 2;
            return;
        }
        if (M == 0) { c.setD(reg, SZ, r); if (SZ == 2) c.cyc += 4; }
        else c.wr(addr, SZ, r);
    }

    // ---------------------------------------------------------------- building the tables

    template <int SZ, int SM>
    static Handler moveDst(int dm)
    {
        switch (dm) {
        case 0: return &move<SZ, SM, 0>;
        case 1: return &move<SZ, SM, 1>;
        case 2: return &move<SZ, SM, 2>;
        case 3: return &move<SZ, SM, 3>;
        case 4: return &move<SZ, SM, 4>;
        case 5: return &move<SZ, SM, 5>;
        case 6: return &move<SZ, SM, 6>;
        default: return &move<SZ, SM, 7>;
        }
    }

    template <int SZ>
    static Handler moveFor(int sm, int dm)
    {
        switch (sm) {
        case 0: return moveDst<SZ, 0>(dm);
        case 1: return moveDst<SZ, 1>(dm);
        case 2: return moveDst<SZ, 2>(dm);
        case 3: return moveDst<SZ, 3>(dm);
        case 4: return moveDst<SZ, 4>(dm);
        case 5: return moveDst<SZ, 5>(dm);
        case 6: return moveDst<SZ, 6>(dm);
        default: return moveDst<SZ, 7>(dm);
        }
    }

    // A handler template indexed by an addressing mode 0..7.
#define MODE_SWITCH(m, EXPR) \
    switch (m) { \
    case 0: { const int M = 0; return EXPR; } case 1: { const int M = 1; return EXPR; } \
    case 2: { const int M = 2; return EXPR; } case 3: { const int M = 3; return EXPR; } \
    case 4: { const int M = 4; return EXPR; } case 5: { const int M = 5; return EXPR; } \
    case 6: { const int M = 6; return EXPR; } default: { const int M = 7; return EXPR; } }

    template <int GRP, int SZ> static Handler eaToDFor(int m) { MODE_SWITCH(m, (&eaToD<GRP, SZ, M>)) }
    template <int GRP, int SZ> static Handler dToEaFor(int m) { MODE_SWITCH(m, (&dToEa<GRP, SZ, M>)) }
    template <int GRP, int OSZ> static Handler addrArithFor(int m) { MODE_SWITCH(m, (&addrArith<GRP, OSZ, M>)) }
    template <int KIND, int SZ> static Handler immediateFor(int m) { MODE_SWITCH(m, (&immediate<KIND, SZ, M>)) }
    template <int SZ, bool SUB> static Handler addsubqFor(int m) { MODE_SWITCH(m, (&addsubq<SZ, M, SUB>)) }
    template <int SZ> static Handler tstFor(int m) { MODE_SWITCH(m, (&tst<SZ, M>)) }
    template <int SZ> static Handler clrFor(int m) { MODE_SWITCH(m, (&clr<SZ, M>)) }
    static Handler leaFor(int m) { MODE_SWITCH(m, (&lea<M>)) }
    static Handler peaFor(int m) { MODE_SWITCH(m, (&pea<M>)) }
    static Handler jmpFor(int m) { MODE_SWITCH(m, (&jmpjsr<M>)) }
#undef MODE_SWITCH

    template <int GRP>
    static Handler bySize(int sz, int m, bool toEa)
    {
        if (toEa) return sz == 0 ? dToEaFor<GRP, 0>(m) : sz == 1 ? dToEaFor<GRP, 1>(m) : dToEaFor<GRP, 2>(m);
        return sz == 0 ? eaToDFor<GRP, 0>(m) : sz == 1 ? eaToDFor<GRP, 1>(m) : eaToDFor<GRP, 2>(m);
    }

    template <int KIND>
    static Handler immBySize(int sz, int m)
    {
        return sz == 0 ? immediateFor<KIND, 0>(m) : sz == 1 ? immediateFor<KIND, 1>(m) : immediateFor<KIND, 2>(m);
    }

    static Handler ccHandler(bool db, int cc)
    {
        static const Handler b[16] = {&bcc<0>, &bcc<1>, &bcc<2>, &bcc<3>, &bcc<4>, &bcc<5>, &bcc<6>, &bcc<7>,
                                      &bcc<8>, &bcc<9>, &bcc<10>, &bcc<11>, &bcc<12>, &bcc<13>, &bcc<14>, &bcc<15>};
        static const Handler d[16] = {&dbcc<0>, &dbcc<1>, &dbcc<2>, &dbcc<3>, &dbcc<4>, &dbcc<5>, &dbcc<6>, &dbcc<7>,
                                      &dbcc<8>, &dbcc<9>, &dbcc<10>, &dbcc<11>, &dbcc<12>, &dbcc<13>, &dbcc<14>, &dbcc<15>};
        return db ? d[cc] : b[cc];
    }

    // The specialised handler for `op` on this CPU model, or generic.
    static Handler choose(u16 op, bool m020)
    {
        int mode = (op >> 3) & 7, reg = op & 7;
        int top = op >> 12;

        if (top >= 1 && top <= 3) {                     // MOVE / MOVEA
            int sz = top == 1 ? 0 : top == 3 ? 1 : 2;
            int dmode = (op >> 6) & 7, dreg = (op >> 9) & 7;
            if (!validEA(mode, reg, sz == 0 ? EA_DATA : EA_ALL)) return &generic;
            if (!validEA(dmode, dreg, sz == 0 ? EA_DATAALT : EA_ALT)) return &generic;
            return sz == 0 ? moveFor<0>(mode, dmode) : sz == 1 ? moveFor<1>(mode, dmode) : moveFor<2>(mode, dmode);
        }

        if (top == 7) return (op & 0x100) ? &generic : &moveq;

        if (top == 6) return ccHandler(false, (op >> 8) & 15);

        if (top == 5) {
            int sz = (op >> 6) & 3;
            if (sz == 3) return mode == 1 ? ccHandler(true, (op >> 8) & 15) : &generic;   // DBcc; Scc/TRAPcc generic
            if (!validEA(mode, reg, sz == 0 ? EA_DATAALT : EA_ALT)) return &generic;
            bool sub = (op & 0x100) != 0;
            if (sz == 0) return sub ? addsubqFor<0, true>(mode) : addsubqFor<0, false>(mode);
            if (sz == 1) return sub ? addsubqFor<1, true>(mode) : addsubqFor<1, false>(mode);
            return sub ? addsubqFor<2, true>(mode) : addsubqFor<2, false>(mode);
        }

        if (top == 4) {
            if (op == 0x4E75) return &rts;
            if (op == 0x4E71) return &nop;
            if ((op & 0xFFF8) == 0x4E50) return &link;
            if ((op & 0xFFF8) == 0x4E58) return &unlk;
            if ((op & 0xFF80) == 0x4E80 && validEA(mode, reg, EA_CTRL)) return jmpFor(mode);   // JSR / JMP
            if ((op & 0xF1C0) == 0x41C0 && validEA(mode, reg, EA_CTRL)) return leaFor(mode);
            if ((op & 0xFFC0) == 0x4840 && mode != 0 && validEA(mode, reg, EA_CTRL)) return peaFor(mode);
            if ((op & 0xFF00) == 0x4A00 && ((op >> 6) & 3) != 3) {                       // TST
                int sz = (op >> 6) & 3;
                u32 mask = m020 ? (sz == 0 ? EA_DATA : EA_ALL) : (sz == 0 ? EA_DATAALT : EA_ALT);
                if (!validEA(mode, reg, mask)) return &generic;
                return sz == 0 ? tstFor<0>(mode) : sz == 1 ? tstFor<1>(mode) : tstFor<2>(mode);
            }
            if ((op & 0xFF00) == 0x4200 && ((op >> 6) & 3) != 3 && validEA(mode, reg, EA_DATAALT)) {   // CLR
                int sz = (op >> 6) & 3;
                return sz == 0 ? clrFor<0>(mode) : sz == 1 ? clrFor<1>(mode) : clrFor<2>(mode);
            }
            if ((op & 0xFB80) == 0x4880 && mode != 0) return &movem;   // (mode 0 is EXT)
            return &generic;
        }

        if (top == 0x8 || top == 0x9 || top == 0xB || top == 0xC || top == 0xD) {
            int opmode = (op >> 6) & 7, sz = opmode & 3;
            if (sz == 3) {                              // ADDA / SUBA / CMPA (MUL/DIV generic)
                if (top == 0x8 || top == 0xC) return &generic;
                if (!validEA(mode, reg, EA_ALL)) return &generic;
                bool l = opmode == 7;
                switch (top) {
                case 0x9: return l ? addrArithFor<0x9, 2>(mode) : addrArithFor<0x9, 1>(mode);
                case 0xB: return l ? addrArithFor<0xB, 2>(mode) : addrArithFor<0xB, 1>(mode);
                default: return l ? addrArithFor<0xD, 2>(mode) : addrArithFor<0xD, 1>(mode);
                }
            }
            if (opmode < 4) {                           // <ea>,Dn
                if (!validEA(mode, reg, (top == 0x8 || top == 0xC || sz == 0) ? EA_DATA : EA_ALL)) return &generic;
                switch (top) {
                case 0x8: return bySize<0x8>(sz, mode, false);
                case 0x9: return bySize<0x9>(sz, mode, false);
                case 0xB: return bySize<0xB>(sz, mode, false);
                case 0xC: return bySize<0xC>(sz, mode, false);
                default: return bySize<0xD>(sz, mode, false);
                }
            }
            // Dn,<ea>: memory destinations, and EOR Dn,Dn.  Register forms of
            // the others are ADDX/SUBX/ABCD/SBCD/EXG/PACK/UNPK/CMPM.
            if (top == 0xB && mode == 0) return bySize<0xB>(sz, 0, true);
            if (mode < 2 || !validEA(mode, reg, EA_MEMALT)) return &generic;
            switch (top) {
            case 0x8: return bySize<0x8>(sz, mode, true);
            case 0x9: return bySize<0x9>(sz, mode, true);
            case 0xB: return bySize<0xB>(sz, mode, true);
            case 0xC: return bySize<0xC>(sz, mode, true);
            default: return bySize<0xD>(sz, mode, true);
            }
        }

        if (top == 0 && !(op & 0x100)) {                // ORI ANDI SUBI ADDI EORI CMPI
            int kind = (op >> 9) & 7, sz = (op >> 6) & 3;
            if (kind == 4 || kind == 7 || sz == 3) return &generic;   // bit ops, MOVES, CHK2/CAS
            if (mode == 7 && reg == 4) return &generic;               // to CCR / SR
            if (m020 && kind == 6) { if (!validEA(mode, reg, EA_DATA & ~EA_IMM)) return &generic; }
            else if (!validEA(mode, reg, EA_DATAALT)) return &generic;
            if (m020 && (op & 0xF9C0) == 0x00C0) return &generic;     // CHK2/CMP2 share these bits
            switch (kind) {
            case 0: return immBySize<0>(sz, mode);
            case 1: return immBySize<1>(sz, mode);
            case 2: return immBySize<2>(sz, mode);
            case 3: return immBySize<3>(sz, mode);
            case 5: return immBySize<5>(sz, mode);
            default: return immBySize<6>(sz, mode);
            }
        }

        return &generic;
    }
};

// On the heap rather than static: a Circle kernel image (code, data and
// zeroed data) may be at most 2 MB, and these are 512 KB each.
M68K::Handler *M68K::s_table000 = 0;
M68K::Handler *M68K::s_table020 = 0;
bool M68K::s_tablesBuilt = false;

void M68K::buildTables()
{
    if (s_tablesBuilt) return;
    s_table000 = new Handler[65536];
    s_table020 = new Handler[65536];
    for (u32 op = 0; op < 65536; op++) {
        s_table000[op] = M68KFast::choose((u16)op, false);
        s_table020[op] = M68KFast::choose((u16)op, true);
    }
    s_tablesBuilt = true;
}
