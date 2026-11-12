// MC68881 / MC68882 floating point coprocessor and the MC68040's FPU.
//
// Values are held as `long double` (quad precision on the Pi), which covers
// the 68881's 80-bit extended format.  Every instruction is implemented,
// including the transcendental ones the 68040 leaves to software, so no
// FPSP emulation package is needed.  Floating point exceptions are only
// reported in FPSR; they never trap.

#ifndef MACEMU_FPU_H
#define MACEMU_FPU_H

#include "m68k.h"
#include "fmath.h"

namespace macemu {

class Fpu : public Coprocessor {
public:
    enum Type { MC68881 = 1, MC68882 = 2, MC68040FPU = 4 };

    explicit Fpu(int type);
    bool execute(M68K &cpu, u16 op) override;
    void reset() override;
    void checkpoint() override { for (int i = 0; i < 8; i++) savedFp[i] = fp[i]; savedCr = fpcr; savedSr = fpsr; savedIar = fpiar; }
    void rollback() override { for (int i = 0; i < 8; i++) fp[i] = savedFp[i]; fpcr = savedCr; fpsr = savedSr; fpiar = savedIar; }

    fm::F fp[8];
    u32 fpcr, fpsr, fpiar;
    fm::F savedFp[8];                   // as the current instruction found them
    u32 savedCr, savedSr, savedIar;

    // Memory formats (big-endian byte arrays), public for tests.
    static fm::F fromExtended(const u8 *p);
    static void toExtended(fm::F x, u8 *p);
    static fm::F fromPacked(const u8 *p);
    static void toPacked(fm::F x, int k, u8 *p);
    static fm::F fromSingle(u32 bits);
    static u32 toSingle(fm::F x);
    static fm::F fromDouble(u64 bits);
    static u64 toDouble(fm::F x);

    bool condition(int c);

private:
    int type;
    bool used;                  // anything executed since reset (FSAVE frame type)

    enum Format { L = 0, S = 1, X = 2, P = 3, W = 4, D = 5, B = 6, Pdyn = 7 };

    u32 operandAddress(M68K &cpu, int mode, int reg, u32 size);
    bool readOperand(M68K &cpu, int mode, int reg, int format, fm::F &out);
    bool writeOperand(M68K &cpu, int mode, int reg, int format, int k, fm::F x);
    bool general(M68K &cpu);
    bool arithmetic(M68K &cpu, int opmode, int dst, fm::F src);
    bool moveControl(M68K &cpu, u16 ext, bool toMemory);
    bool moveMultiple(M68K &cpu, u16 ext, bool toMemory);
    void fsave(M68K &cpu);
    void frestore(M68K &cpu);

    fm::F roundPrecision(fm::F x, int precision);
    fm::F roundInteger(fm::F x, int mode);
    void setCC(fm::F x);
    void raise(u32 status);
    s64 toInteger(fm::F x, s64 lo, s64 hi);
};

} // namespace macemu

#endif
