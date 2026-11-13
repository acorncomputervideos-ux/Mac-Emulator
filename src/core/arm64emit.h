// A small AArch64 assembler for the JIT: just the instructions it emits.
// Registers are numbers 0-30; 31 is the zero register or SP, as the
// instruction takes it.

#ifndef MACEMU_ARM64EMIT_H
#define MACEMU_ARM64EMIT_H

#include "types.h"

namespace macemu {

namespace a64 {

enum Cond { EQ = 0, NE, HS, LO, MI, PL, VS, VC, HI, LS, GE, LT, GT, LE, AL };
enum { ZR = 31, SP = 31 };

class Emitter {
public:
    Emitter(u32 *buf, u32 words) : start(buf), p(buf), end(buf + words) {}

    bool full() const { return p + 64 >= end; }        // room for one more instruction's code
    u32 *here() const { return p; }
    u32 size() const { return (u32)(p - start); }
    void put(u32 w) { if (p < end) *p++ = w; }

    // Moves
    void movw(int rd, u32 v)                            // 32-bit constant
    {
        put(0x52800000 | ((v & 0xFFFF) << 5) | (u32)rd);
        if (v >> 16) put(0x72A00000 | ((v >> 16) << 5) | (u32)rd);
    }
    void movx(int rd, u64 v)                            // 64-bit constant
    {
        put(0xD2800000 | ((u32)(v & 0xFFFF) << 5) | (u32)rd);
        for (int hw = 1; hw < 4; hw++) {
            u32 part = (u32)(v >> (16 * hw)) & 0xFFFF;
            if (part) put(0xF2800000 | ((u32)hw << 21) | (part << 5) | (u32)rd);
        }
    }
    void movrx(int rd, int rm) { put(0xAA0003E0 | ((u32)rm << 16) | (u32)rd); }
    void movrw(int rd, int rm) { put(0x2A0003E0 | ((u32)rm << 16) | (u32)rd); }

    // Loads and stores at [rn + off] (off a multiple of the size).
    void ldrw(int rt, int rn, u32 off)  { put(0xB9400000 | ((off / 4) << 10) | ((u32)rn << 5) | (u32)rt); }
    void strw(int rt, int rn, u32 off)  { put(0xB9000000 | ((off / 4) << 10) | ((u32)rn << 5) | (u32)rt); }
    void ldrx(int rt, int rn, u32 off)  { put(0xF9400000 | ((off / 8) << 10) | ((u32)rn << 5) | (u32)rt); }
    void strx(int rt, int rn, u32 off)  { put(0xF9000000 | ((off / 8) << 10) | ((u32)rn << 5) | (u32)rt); }
    void ldrb(int rt, int rn, u32 off)  { put(0x39400000 | (off << 10) | ((u32)rn << 5) | (u32)rt); }
    void strb(int rt, int rn, u32 off)  { put(0x39000000 | (off << 10) | ((u32)rn << 5) | (u32)rt); }
    void ldrh(int rt, int rn, u32 off)  { put(0x79400000 | ((off / 2) << 10) | ((u32)rn << 5) | (u32)rt); }
    void strh(int rt, int rn, u32 off)  { put(0x79000000 | ((off / 2) << 10) | ((u32)rn << 5) | (u32)rt); }
    // Pairs of X registers on the stack.
    void stpPre(int t1, int t2, int rn, int off)  { put(0xA9800000 | (((u32)(off / 8) & 0x7F) << 15) | ((u32)t2 << 10) | ((u32)rn << 5) | (u32)t1); }
    void stp(int t1, int t2, int rn, int off)     { put(0xA9000000 | (((u32)(off / 8) & 0x7F) << 15) | ((u32)t2 << 10) | ((u32)rn << 5) | (u32)t1); }
    void ldp(int t1, int t2, int rn, int off)     { put(0xA9400000 | (((u32)(off / 8) & 0x7F) << 15) | ((u32)t2 << 10) | ((u32)rn << 5) | (u32)t1); }
    void ldpPost(int t1, int t2, int rn, int off) { put(0xA8C00000 | (((u32)(off / 8) & 0x7F) << 15) | ((u32)t2 << 10) | ((u32)rn << 5) | (u32)t1); }

    // Arithmetic
    void addwi(int rd, int rn, u32 imm) { put(0x11000000 | (imm << 10) | ((u32)rn << 5) | (u32)rd); }   // imm < 4096
    void subwi(int rd, int rn, u32 imm) { put(0x51000000 | (imm << 10) | ((u32)rn << 5) | (u32)rd); }
    void addw(int rd, int rn, int rm)   { put(0x0B000000 | ((u32)rm << 16) | ((u32)rn << 5) | (u32)rd); }
    void addxUxtw(int rd, int rn, int rm) { put(0x8B204000 | ((u32)rm << 16) | ((u32)rn << 5) | (u32)rd); }
    void asrwi(int rd, int rn, u32 sh)  { put(0x13007C00 | (sh << 16) | ((u32)rn << 5) | (u32)rd); }
    void cmpw(int rn, int rm)           { put(0x6B00001F | ((u32)rm << 16) | ((u32)rn << 5)); }
    void cmpx(int rn, int rm)           { put(0xEB00001F | ((u32)rm << 16) | ((u32)rn << 5)); }
    void cmpwi(int rn, u32 imm)         { put(0x7100001F | (imm << 10) | ((u32)rn << 5)); }

    // Branches.  Forward ones are emitted with no target and patched.
    u32 *bcond(Cond c)        { u32 *at = p; put(0x54000000 | (u32)c); return at; }
    u32 *cbz(int rt)          { u32 *at = p; put(0x34000000 | (u32)rt); return at; }
    u32 *cbnz(int rt)         { u32 *at = p; put(0x35000000 | (u32)rt); return at; }
    u32 *b()                  { u32 *at = p; put(0x14000000); return at; }
    static void patch(u32 *at, const u32 *target)
    {
        s32 off = (s32)(target - at);
        if ((*at & 0xFC000000) == 0x14000000) *at |= (u32)off & 0x03FFFFFF;
        else *at |= ((u32)off & 0x7FFFF) << 5;
    }
    void blr(int rn) { put(0xD63F0000 | ((u32)rn << 5)); }
    void ret()       { put(0xD65F03C0); }

private:
    u32 *start, *p, *end;
};

} // namespace a64
} // namespace macemu

#endif
