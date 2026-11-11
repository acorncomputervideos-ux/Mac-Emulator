// Motorola 68000 interpreter.

#ifndef MACEMU_M68K_H
#define MACEMU_M68K_H

#include "types.h"

namespace macemu {

class Bus {
public:
    virtual ~Bus() {}
    virtual u8 read8(u32 addr) = 0;
    virtual u16 read16(u32 addr) = 0;
    virtual void write8(u32 addr, u8 v) = 0;
    virtual void write16(u32 addr, u16 v) = 0;
    // Called when the CPU acknowledges an interrupt of the given level.
    virtual void irqAck(int level) { (void)level; }
    // Called when the CPU executes RESET.
    virtual void resetLine() {}
};

class M68K {
public:
    explicit M68K(Bus *bus);

    void hardReset();           // zero everything, load SSP/PC from vectors 0/1
    int step();                 // execute one instruction, returns CPU clock cycles used
    void setIRQ(int level) { irq = level; }

    u16 getSR() const;
    void setSR(u16 v);
    u32 getUSP() const { return fS ? otherSP : a[7]; }
    u32 getSSP() const { return fS ? a[7] : otherSP; }
    void setUSP(u32 v) { if (fS) otherSP = v; else a[7] = v; }
    void setSSP(u32 v) { if (fS) a[7] = v; else otherSP = v; }

    u32 d[8];
    u32 a[8];
    u32 pc;
    bool stopped;
    u64 totalCycles;

private:
    Bus *bus;
    u32 otherSP;                // the stack pointer that is not currently A7
    u32 fC, fV, fZ, fN, fX;     // condition codes, each 0 or 1
    u32 fS, fT, imask;
    int irq;
    int cyc;
    u32 instPC;
    u16 opcode;
    bool traceNext;

    u16 fetch16() { u16 v = bus->read16(pc); pc += 2; cyc += 4; return v; }
    u32 fetch32() { u32 h = fetch16(); return (h << 16) | fetch16(); }
    u32 rd(u32 addr, int sz);
    void wr(u32 addr, int sz, u32 v);
    void push16(u16 v) { a[7] -= 2; wr(a[7], 1, v); }
    void push32(u32 v) { a[7] -= 4; wr(a[7], 2, v); }
    u16 pop16() { u16 v = (u16)rd(a[7], 1); a[7] += 2; return v; }
    u32 pop32() { u32 v = rd(a[7], 2); a[7] += 4; return v; }

    u32 index(u32 base);
    u32 ea(int mode, int reg, int sz, bool noPenalty = false);
    u32 readEA(int mode, int reg, int sz);
    void setD(int reg, int sz, u32 v);
    bool cond(int c) const;
    void exception(int vec, u32 pushPC, int extraCycles);
    void illegal();
    void privilege() { exception(8, instPC, 10); }

    void setNZ(u32 v, int sz);
    void logicFlags(u32 v, int sz) { setNZ(v, sz); fV = fC = 0; }
    u32 add(u32 s, u32 dd, int sz, u32 x, bool isX);
    u32 sub(u32 s, u32 dd, int sz, u32 x, bool isX, bool setX);
    u32 abcd(u32 s, u32 dd);
    u32 sbcd(u32 s, u32 dd);
    u32 shift(int type, bool left, u32 v, int count, int sz);

    void exec(u16 op);
    void group0(u16 op);
    void group4(u16 op);
    void movem(u16 op);
    void arith(u16 op);         // groups 8, 9, B, C, D
    void groupE(u16 op);
};

} // namespace macemu

#endif
