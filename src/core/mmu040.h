// MC68040 memory management unit: three-level table walks for 4K or 8K
// pages from URP/SRP, data and instruction transparent translation
// registers, used/modified bit maintenance, write-protect and supervisor
// faults, an address translation cache, PFLUSH and PTEST.

#ifndef MACEMU_MMU040_H
#define MACEMU_MMU040_H

#include "m68k.h"

namespace macemu {

class StateIO;

class Mmu040 : public Coprocessor, public Translator {
public:
    void state(StateIO &s);             // save or load (state.cpp)
    explicit Mmu040(Bus &physicalBus);

    // Translator
    bool enabled() const override { return (tc & 0x8000) != 0; }
    u32 translate(u32 logical, bool write, bool supervisor, bool code, bool &fault) override;

    // Coprocessor: PFLUSH / PTEST (opcodes $F500-$F5FF) and MOVEC access.
    bool execute(M68K &cpu, u16 op) override;
    bool control(int reg, u32 &value, bool write) override;
    void reset() override;

    void flush();                       // empty the translation cache

    u32 tc, urp, srp, mmusr;
    u32 itt[2], dtt[2];

private:
    enum { kCacheSize = 256 };
    struct Entry {
        u32 tag;                        // logical page | 1 (valid) | 2 (supervisor) | 4 (code)
        u32 frame;                      // physical page
        bool writeProtect, supervisorOnly, modified;
    };

    Bus &bus;
    Entry cache[kCacheSize];

    u32 pageShift() const { return (tc & 0x4000) ? 13 : 12; }
    bool transparent(u32 addr, bool supervisor, bool code, bool write, bool &wp) const;
    // Walk the tables.  Returns false on an invalid descriptor; sets the
    // descriptor flags.  `update` writes U (and M when `write`) back.
    bool walk(u32 logical, bool supervisor, bool write, bool update,
              u32 &frame, bool &wp, bool &supervisorOnly, bool &modified, u32 &status);
    u32 rd32(u32 a) { return bus.read32(a); }
    void wr32(u32 a, u32 v) { bus.write32(a, v); }
};

} // namespace macemu

#endif
