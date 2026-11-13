// Motorola 68000 / 68020 / 68030 / 68040 interpreter.

#ifndef MACEMU_M68K_H
#define MACEMU_M68K_H

#include "types.h"

namespace macemu {

class StateIO;

class Bus {
public:
    virtual ~Bus() {}
    virtual u8 read8(u32 addr) = 0;
    virtual u16 read16(u32 addr) = 0;
    virtual void write8(u32 addr, u8 v) = 0;
    virtual void write16(u32 addr, u16 v) = 0;
    // 32-bit accesses; the default splits them into two word accesses.
    virtual u32 read32(u32 addr) { u32 h = read16(addr); return (h << 16) | read16(addr + 2); }
    virtual void write32(u32 addr, u32 v) { write16(addr, (u16)(v >> 16)); write16(addr + 2, (u16)v); }
    // Called when the CPU acknowledges an interrupt of the given level.
    virtual void irqAck(int level) { (void)level; }
    // Called when the CPU executes RESET.
    virtual void resetLine() {}

    // A device calls this during an access that should end in a bus error
    // (an empty NuBus slot, say).  The CPU finishes the instruction and then
    // takes the bus error exception.
    // Fast path for the CPU: the host address of the 4 KB physical page
    // containing `addr` if it is plain memory (RAM, ROM, video RAM), or
    // null.  `writable` is false for ROM.  Machines bump mapGeneration
    // whenever this answer could change (overlays, address modes).
    virtual u8 *hostPage(u32 addr, bool &writable) { (void)addr; writable = false; return 0; }
    u32 mapGeneration = 0;

    bool busError = false;
    u32 busErrorAddr = 0;
    bool busErrorWrite = false;
    bool busErrorAtc = false;           // an MMU translation fault (not a device's)
    bool busErrorCode = false;          // on an instruction fetch
    int busErrorSize = 2;               // 0 byte, 1 word, 2 long
    void signalBusError(u32 addr, bool write)
    {
        if (!busError) {
            busError = true; busErrorAddr = addr; busErrorWrite = write;
            busErrorAtc = busErrorCode = false; busErrorSize = 2;
        }
    }
};

// Coprocessor hooks (floating point unit, MMU).  The CPU hands F-line
// instructions to them; they use the CPU's public helpers for operands.
class M68K;
class Coprocessor {
public:
    virtual ~Coprocessor() {}
    // Returns false if the instruction is not recognised (F-line exception).
    virtual bool execute(M68K &cpu, u16 op) = 0;
    virtual void reset() {}
    // Save and restore the registers an instruction may change, so that a
    // 68040 access fault can restart it.
    virtual void checkpoint() {}
    virtual void rollback() {}
    // 68040 MOVEC access to MMU registers; returns false if unknown.
    virtual bool control(int reg, u32 &value, bool write) { (void)reg; (void)value; (void)write; return false; }
};

// Address translation for 68030/68040 MMUs.  translate() returns the
// physical address, or sets `fault` for a bus error.
class Translator {
public:
    virtual ~Translator() {}
    u32 generation = 0;                 // bumped whenever translations may change
    virtual bool enabled() const = 0;
    virtual u32 translate(u32 logical, bool write, bool supervisor, bool code, bool &fault) = 0;
};

struct M68KFast;

class M68K {
public:
    void state(StateIO &s);             // save or load (state.cpp)
    enum Model { MC68000 = 0, MC68020 = 2, MC68030 = 3, MC68040 = 4 };
    typedef void (*Handler)(M68K &, u16);

    explicit M68K(Bus *bus);

    void setModel(int m) { model = m; table = m == MC68000 ? s_table000 : s_table020; }
    // 68040: make access faults restartable (the registers are saved before
    // each instruction, which costs some speed).  The machine turns this on
    // while virtual memory is in use.
    bool restartable = false;
    u16 lastTrap = 0;                   // the last A-line trap (Mac OS call), for logs
    u32 pageFaults = 0;                 // translation faults below $40000000 since reset
    bool fastDispatch = true;           // false: decode every instruction in exec() (for comparisons)
    int getModel() const { return model; }
    void attachFpu(Coprocessor *f) { fpu = f; }
    void attachMmu(Coprocessor *unit, Translator *t) { mmuUnit = unit; mmu = t; }

    void hardReset();           // zero everything, load SSP/PC from vectors 0/1
    int step();                 // execute one instruction, returns CPU clock cycles used
    // Up to `count` instructions, stopping once totalCycles reaches `end`;
    // returns the cycles used.  Exactly like calling step() that often.
    u32 run(int count, u64 end);
    void setIRQ(int level) { irq = level; }

    u16 getSR() const;
    void setSR(u16 v);
    u32 getUSP() const { return fS ? usp : a[7]; }
    u32 getSSP() const { return fS ? a[7] : (fM ? msp : isp); }
    void setUSP(u32 v) { if (fS) usp = v; else a[7] = v; }
    void setSSP(u32 v) { if (fS) a[7] = v; else if (fM) msp = v; else isp = v; }
    u32 getISP() const { return fS && !fM ? a[7] : isp; }
    u32 getMSP() const { return fS && fM ? a[7] : msp; }

    u32 d[8];
    u32 a[8];
    u32 pc;
    bool stopped;
    u64 totalCycles;

    // 68010+ control registers
    u32 vbr, sfc, dfc, cacr, caar;
    u32 instPC;
    int cyc;

    // Helpers, public so that coprocessors can use them.
    // sz: 0 byte, 1 word, 2 long.  The common case (a 68020+ access within
    // a page that the host TLB maps) is inline; the rest is rdSlow/wrSlow.
    // The 68000 never fills the TLB, so its accesses always take the slow way.
    u32 rd(u32 addr, int sz)
    {
        if ((addr & 0xFFF) <= 0x1000 - (1u << sz)) {
            const TlbEntry &e = tlb[(addr >> 12) & (kTlbSize - 1)];
            if (e.tag == tlbTag(addr)) {
                cyc += 4;
                const u8 *p = e.ptr + (addr & 0xFFF);
                return sz == 0 ? p[0] : sz == 1 ? (u32)be16(p) : be32(p);
            }
        }
        return rdSlow(addr, sz);
    }
    void wr(u32 addr, int sz, u32 v)
    {
        if ((addr & 0xFFF) <= 0x1000 - (1u << sz)) {
            const TlbEntry &e = tlb[(addr >> 12) & (kTlbSize - 1)];
            if (e.tag == tlbTag(addr) && e.writable) {
                cyc += 4;
                u8 *p = e.ptr + (addr & 0xFFF);
                if (sz == 0) p[0] = (u8)v; else if (sz == 1) putBe16(p, (u16)v); else putBe32(p, v);
                return;
            }
        }
        wrSlow(addr, sz, v);
    }
    __attribute__((always_inline)) u16 fetch16()
    {
        u32 addr = pc;
        if ((addr & 0xFFF) != 0xFFF) {
            const TlbEntry &e = codeTlb[(addr >> 12) & (kCodeTlbSize - 1)];
            if (e.tag == tlbTag(addr)) {
                pc = addr + 2;
                cyc += 4;
                return be16(e.ptr + (addr & 0xFFF));
            }
        }
        return fetch16Slow();
    }
    u32 fetch32() { u32 h = fetch16(); return (h << 16) | fetch16(); }
    u32 ea(int mode, int reg, int sz, bool noPenalty = false);
    u32 readEA(int mode, int reg, int sz);
    void setD(int reg, int sz, u32 v);
    bool cond(int c) const;
    void exception(int vec, u32 pushPC, int extraCycles, int format = 0);
    void illegal();
    void privilege() { exception(8, instPC, 10); }
    bool supervisor() const { return fS != 0; }
    u16 currentOpcode() const { return opcode; }
    u32 physical(u32 addr, bool write, bool code);
    u32 *addressRegs() { return a; }
    void flushTlb() { tlbGeneration = ~0u; }

    // The JIT (AArch64 hosts): runs the usual instructions as translated
    // blocks, with exactly the same results.  Null: the interpreter only.
    class Jit *jit = 0;
    // Accesses that took the slow way (devices, TLB misses): the JIT's sign
    // that interrupts or the address map may have changed.
    u32 slowAccesses = 0;

private:
    friend struct M68KFast;
    friend struct JitAccess;
    friend struct NativeGen;
    friend class Jit;
    static Handler s_generic;           // the handler of the instructions exec() decodes
    // One handler per opcode (m68kfast.inc), for the 68000 and for 68020+.
    static Handler *s_table000, *s_table020;   // 65536 entries each, from the heap
    static bool s_tablesBuilt;
    static void buildTables();
    Handler *table;

    Bus *bus;
    Coprocessor *fpu, *mmuUnit;
    Translator *mmu;
    int model;
    u32 usp, isp, msp;          // stack pointers not currently in A7
    u32 fC, fV, fZ, fN, fX;     // condition codes, each 0 or 1
    u32 fS, fM, fT, imask;
    int irq;
    bool nmiSeen;               // level 7 is edge triggered
    u16 opcode;
    bool traceNext;

    // Host TLB: logical 4 KB page -> host memory, for model >= 68020.
    enum { kTlbSize = 512 };
    struct TlbEntry { u32 tag; u8 *ptr; bool writable; };
    TlbEntry tlb[kTlbSize];
    enum { kCodeTlbSize = 256 };
    TlbEntry codeTlb[kCodeTlbSize];     // instruction fetches (the 68040 translates them with ITT)
    u32 tlbGeneration;
    void checkTlb()
    {
        u32 gen = bus->mapGeneration + (mmu ? mmu->generation * 7919u : 0);   // TC writes flush, bumping it
        if (gen != tlbGeneration) flushTlb(gen);
    }
    void flushTlb(u32 gen);
    u32 rdSlow(u32 addr, int sz);
    void wrSlow(u32 addr, int sz, u32 v);
    u16 fetch16Slow();
    inline int stepOne();
    u8 *tlbLookup(u32 addr, bool write, bool code);
    u32 tlbTag(u32 addr) const { return (addr & 0xFFFFF000u) | 1u | (fS << 1); }   // fS is 0 or 1

    void push16(u16 v) { a[7] -= 2; wr(a[7], 1, v); }
    void push32(u32 v) { a[7] -= 4; wr(a[7], 2, v); }
    u16 pop16() { u16 v = (u16)rd(a[7], 1); a[7] += 2; return v; }
    u32 pop32() { u32 v = rd(a[7], 2); a[7] += 4; return v; }
    void switchStack(u32 newS, u32 newM);
    void busErrorException();

    // The 68040 restarts a faulting instruction: its state as it began.
    // Two block copies: d[] and a[] are adjacent, and so are usp ... imask.
    struct Checkpoint { u32 da[16]; u32 rest[12]; };
    Checkpoint saved;
    void checkpoint();
    void rollback();
    static void checkLayout();
    void faultSeen() { if (tlbGeneration != 0xFFFFFFFFu) flushTlb(0xFFFFFFFFu); }   // later accesses take the slow path

    u32 index(u32 base);

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
    void lineF(u16 op);

    // 68020+ instructions
    void movec(u16 op);
    void moves(u16 op);
    void mulLong(u16 op);
    void divLong(u16 op);
    void chk2(u16 op);
    void cas(u16 op);
    void cas2(u16 op);
    void bitfield(u16 op);
    void packUnpk(u16 op);
    void trapcc(u16 op);
    void move16(u16 op);
    void rte();
    bool mmuControl(int cr, u32 &v, bool write);
};

} // namespace macemu

#endif
