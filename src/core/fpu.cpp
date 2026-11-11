#include "fpu.h"

namespace macemu {

using fm::F;

enum {
    CC_N = 1u << 27, CC_Z = 1u << 26, CC_I = 1u << 25, CC_NAN = 1u << 24,
    EX_BSUN = 1u << 15, EX_SNAN = 1u << 14, EX_OPERR = 1u << 13, EX_OVFL = 1u << 12,
    EX_UNFL = 1u << 11, EX_DZ = 1u << 10, EX_INEX2 = 1u << 9, EX_INEX1 = 1u << 8,
    AE_IOP = 1u << 7, AE_OVFL = 1u << 6, AE_UNFL = 1u << 5, AE_DZ = 1u << 4, AE_INEX = 1u << 3,
};

Fpu::Fpu(int t) : type(t)
{
    reset();
}

void Fpu::reset()
{
    for (int i = 0; i < 8; i++) fp[i] = fm::nan();
    fpcr = fpsr = fpiar = 0;
    used = false;
}

// ---------------------------------------------------------------- formats

F Fpu::fromSingle(u32 b)
{
    float f;
    __builtin_memcpy(&f, &b, 4);
    return (F)f;
}

u32 Fpu::toSingle(F x)
{
    float f = (float)x;
    u32 b;
    __builtin_memcpy(&b, &f, 4);
    return b;
}

F Fpu::fromDouble(u64 b)
{
    double d;
    __builtin_memcpy(&d, &b, 8);
    return (F)d;
}

u64 Fpu::toDouble(F x)
{
    double d = (double)x;
    u64 b;
    __builtin_memcpy(&b, &d, 8);
    return b;
}

F Fpu::fromExtended(const u8 *p)
{
    u32 se = be16(p);
    u64 mant = ((u64)be32(p + 4) << 32) | be32(p + 8);
    bool neg = (se & 0x8000) != 0;
    int e = (int)(se & 0x7FFF);
    F v;
    if (e == 0x7FFF) {
        v = (mant << 1) ? fm::nan() : fm::inf();
        if (fm::isNan(v)) return v;
    } else if (mant == 0) {
        v = 0;
    } else {
        v = fm::scalb((F)mant, (e ? e : 1) - 16383 - 63);
    }
    return neg ? -v : v;
}

void Fpu::toExtended(F x, u8 *p)
{
    memFill(p, 0, 12);
    u32 se = fm::signBit(x) ? 0x8000 : 0;
    u64 mant = 0;
    if (fm::isNan(x)) {
        se = 0x7FFF;
        mant = 0xFFFFFFFFFFFFFFFFull;
    } else if (fm::isInf(x)) {
        se |= 0x7FFF;
    } else if (x != 0) {
        int e;
        F m = fm::frexp(fm::fabs(x), e);                // [0.5, 1)
        int biased = e - 1 + 16383;
        if (biased <= 0) {                              // denormal
            mant = (u64)fm::roundEven(fm::scalb(fm::fabs(x), 16382 + 63));
            biased = 0;
        } else {
            F scaled = fm::roundEven(fm::scalb(m, 64));
            if (scaled >= fm::scalb(1, 64)) { scaled /= 2; biased++; }
            mant = (u64)scaled;
        }
        if (biased >= 0x7FFF) { biased = 0x7FFF; mant = 0; }
        se |= (u32)biased;
    }
    putBe16(p, (u16)se);
    putBe32(p + 4, (u32)(mant >> 32));
    putBe32(p + 8, (u32)mant);
}

// Packed decimal: SM SE YY | 3-digit exponent | integer digit | 16 fraction digits.
F Fpu::fromPacked(const u8 *p)
{
    bool negM = (p[0] & 0x80) != 0, negE = (p[0] & 0x40) != 0;
    if ((p[0] & 0x7F) == 0x7F && p[1] == 0xFF) {
        bool zeroFrac = true;
        for (int i = 4; i < 12; i++) if (p[i]) zeroFrac = false;
        F v = zeroFrac ? fm::inf() : fm::nan();
        return negM ? -v : v;
    }
    int e10 = ((p[0] & 0x0F) * 100) + ((p[1] >> 4) * 10) + (p[1] & 0x0F);
    u64 digits = p[3] & 0x0F;
    for (int i = 4; i < 12; i++) digits = digits * 100 + (p[i] >> 4) * 10 + (p[i] & 0x0F);
    F v = (F)digits;
    int exp10 = (negE ? -e10 : e10) - 16;
    F scale = fm::pow10((F)(exp10 < 0 ? -exp10 : exp10));
    v = exp10 < 0 ? v / scale : v * scale;
    return negM ? -v : v;
}

void Fpu::toPacked(F x, int k, u8 *p)
{
    memFill(p, 0, 12);
    if (fm::signBit(x)) p[0] |= 0x80;
    if (fm::isNan(x) || fm::isInf(x)) {
        p[0] |= 0x7F;
        p[1] = 0xFF;
        if (fm::isNan(x)) for (int i = 4; i < 12; i++) p[i] = 0xFF;
        return;
    }
    F a = fm::fabs(x);
    if (a == 0) return;
    int e10 = (int)fm::floor(fm::log(a) * fm::kLog10e);
    // Correct an off-by-one from the logarithm.
    if (fm::pow10((F)e10) > a) e10--;
    else if (fm::pow10((F)(e10 + 1)) <= a) e10++;
    int n = k > 0 ? k : e10 + 1 - k;
    if (n > 17) n = 17;
    if (n < 1) n = 1;
    int sh = e10 - n + 1;
    F scaled = sh >= 0 ? a / fm::pow10((F)sh) : a * fm::pow10((F)-sh);
    F mant = fm::roundEven(scaled);
    if (mant >= fm::pow10((F)n)) { mant = fm::roundEven(mant / 10); e10++; }
    u64 m = (u64)mant;
    for (int i = n; i < 17; i++) m *= 10;               // left-align to 17 digits
    u8 dig[17];
    for (int i = 16; i >= 0; i--) { dig[i] = (u8)(m % 10); m /= 10; }
    p[3] = dig[0];
    for (int i = 0; i < 8; i++) p[4 + i] = (u8)((dig[1 + 2 * i] << 4) | dig[2 + 2 * i]);
    int ae = e10 < 0 ? -e10 : e10;
    if (e10 < 0) p[0] |= 0x40;
    p[0] |= (u8)((ae / 100) % 10);
    p[1] = (u8)((((ae / 10) % 10) << 4) | (ae % 10));
}

// ---------------------------------------------------------------- helpers

void Fpu::raise(u32 status)
{
    fpsr |= status;
    if (status & (EX_SNAN | EX_OPERR)) fpsr |= AE_IOP;
    if (status & EX_OVFL) fpsr |= AE_OVFL;
    if (status & EX_UNFL) fpsr |= AE_UNFL;
    if (status & EX_DZ) fpsr |= AE_DZ;
    if (status & (EX_INEX1 | EX_INEX2 | EX_OVFL)) fpsr |= AE_INEX;
}

void Fpu::setCC(F x)
{
    fpsr &= 0x00FFFFFF;
    if (fm::isNan(x)) fpsr |= CC_NAN;
    else {
        if (fm::isInf(x)) fpsr |= CC_I;
        if (x == 0) fpsr |= CC_Z;
    }
    if (fm::signBit(x)) fpsr |= CC_N;
}

F Fpu::roundPrecision(F x, int precision)
{
    if (precision == 1) return (F)(float)x;
    if (precision == 2) return (F)(double)x;
    return x;
}

F Fpu::roundInteger(F x, int mode)
{
    switch (mode) {
    case 1: return fm::trunc(x);                        // toward zero
    case 2: return fm::floor(x);                        // toward minus infinity
    case 3: return -fm::floor(-x);                      // toward plus infinity
    default: return fm::roundEven(x);
    }
}

s64 Fpu::toInteger(F x, s64 lo, s64 hi)
{
    if (fm::isNan(x)) { raise(EX_OPERR); return hi; }
    F r = roundInteger(x, (fpcr >> 4) & 3);
    if (r != x) raise(EX_INEX2);
    if (r < (F)lo) { raise(EX_OPERR); return lo; }
    if (r > (F)hi) { raise(EX_OPERR); return hi; }
    return (s64)r;
}

static const u32 kFormatSize[8] = {4, 4, 12, 12, 2, 8, 1, 12};

u32 Fpu::operandAddress(M68K &cpu, int mode, int reg, u32 size)
{
    if (mode == 3) {
        u32 addr = cpu.a[reg];
        cpu.a[reg] += (size == 1 && reg == 7) ? 2 : size;
        return addr;
    }
    if (mode == 4) {
        cpu.a[reg] -= (size == 1 && reg == 7) ? 2 : size;
        return cpu.a[reg];
    }
    return cpu.ea(mode, reg, 2);
}

bool Fpu::readOperand(M68K &cpu, int mode, int reg, int format, F &out)
{
    u32 size = kFormatSize[format];
    u8 buf[12];
    if (mode == 0) {
        if (size > 4) return false;
        u32 v = cpu.d[reg];
        switch (format) {
        case L: out = (F)(s32)v; return true;
        case S: out = fromSingle(v); return true;
        case W: out = (F)(s16)v; return true;
        default: out = (F)(s8)v; return true;
        }
    }
    if (mode == 1) return false;
    if (mode == 7 && reg == 4) {                        // immediate
        if (size <= 2) { u16 w = cpu.fetch16(); buf[0] = (u8)(w >> 8); buf[1] = (u8)w; if (size == 1) buf[0] = buf[1]; }
        else for (u32 i = 0; i < size; i += 4) putBe32(buf + i, cpu.fetch32());
    } else {
        if (mode == 7 && reg > 4) return false;
        u32 addr = operandAddress(cpu, mode, reg, size);
        if (size == 1) buf[0] = (u8)cpu.rd(addr, 0);
        else if (size == 2) putBe16(buf, (u16)cpu.rd(addr, 1));
        else for (u32 i = 0; i < size; i += 4) putBe32(buf + i, cpu.rd(addr + i, 2));
    }
    switch (format) {
    case L: out = (F)(s32)be32(buf); break;
    case S: out = fromSingle(be32(buf)); break;
    case X: out = fromExtended(buf); break;
    case P: out = fromPacked(buf); break;
    case W: out = (F)(s16)be16(buf); break;
    case D: out = fromDouble(((u64)be32(buf) << 32) | be32(buf + 4)); break;
    case B: out = (F)(s8)buf[0]; break;
    default: return false;
    }
    return true;
}

bool Fpu::writeOperand(M68K &cpu, int mode, int reg, int format, int k, F x)
{
    u32 size = kFormatSize[format];
    u8 buf[12];
    switch (format) {
    case L: putBe32(buf, (u32)toInteger(x, -0x80000000ll, 0x7FFFFFFF)); break;
    case S: putBe32(buf, toSingle(x)); if (fm::isInf((F)(float)x) && !fm::isInf(x)) raise(EX_OVFL); break;
    case X: toExtended(x, buf); break;
    case P: case Pdyn: toPacked(x, k, buf); break;
    case W: putBe16(buf, (u16)toInteger(x, -0x8000, 0x7FFF)); break;
    case D: { u64 b = toDouble(x); putBe32(buf, (u32)(b >> 32)); putBe32(buf + 4, (u32)b); break; }
    default: buf[0] = (u8)toInteger(x, -0x80, 0x7F); break;
    }
    if (mode == 0) {
        if (size > 4) return false;
        if (size == 4) cpu.d[reg] = be32(buf);
        else if (size == 2) cpu.setD(reg, 1, be16(buf));
        else cpu.setD(reg, 0, buf[0]);
        return true;
    }
    if (mode == 1 || (mode == 7 && reg > 1)) return false;
    u32 addr = operandAddress(cpu, mode, reg, size);
    if (size == 1) cpu.wr(addr, 0, buf[0]);
    else if (size == 2) cpu.wr(addr, 1, be16(buf));
    else for (u32 i = 0; i < size; i += 4) cpu.wr(addr + i, 2, be32(buf + i));
    return true;
}

// ---------------------------------------------------------------- conditions

bool Fpu::condition(int c)
{
    bool n = (fpsr & CC_N) != 0, z = (fpsr & CC_Z) != 0, nan = (fpsr & CC_NAN) != 0;
    if ((c & 0x10) && nan) raise(EX_BSUN);
    switch (c & 0x0F) {
    case 0x0: return false;
    case 0x1: return z;
    case 0x2: return !(nan || z || n);
    case 0x3: return z || !(nan || n);
    case 0x4: return n && !(nan || z);
    case 0x5: return z || (n && !nan);
    case 0x6: return !(nan || z);
    case 0x7: return !nan;
    case 0x8: return nan;
    case 0x9: return nan || z;
    case 0xA: return nan || !(n || z);
    case 0xB: return nan || z || !n;
    case 0xC: return nan || (n && !z);
    case 0xD: return nan || z || n;
    case 0xE: return !z;
    default: return true;
    }
}

// ---------------------------------------------------------------- execution

bool Fpu::execute(M68K &cpu, u16 op)
{
    int mode = (op >> 3) & 7, reg = op & 7;
    switch ((op >> 6) & 7) {
    case 0:
        return general(cpu);

    case 1: {                                           // FScc, FDBcc, FTRAPcc
        u16 ext = cpu.fetch16();
        bool t = condition(ext & 0x3F);
        if (mode == 1) {                                // FDBcc
            u32 base = cpu.pc;
            s32 disp = (s16)cpu.fetch16();
            if (!t) {
                u16 cnt = (u16)(cpu.d[reg] - 1);
                cpu.setD(reg, 1, cnt);
                if (cnt != 0xFFFF) cpu.pc = base + (u32)disp;
            }
            return true;
        }
        if (mode == 7 && reg >= 2 && reg <= 4) {        // FTRAPcc
            if (reg == 2) cpu.fetch16(); else if (reg == 3) cpu.fetch32();
            if (t) cpu.exception(7, cpu.pc, 20, 2);
            return true;
        }
        if (mode == 1 || (mode == 7 && reg > 1)) return false;
        u8 v = t ? 0xFF : 0;
        if (mode == 0) cpu.setD(reg, 0, v);
        else cpu.wr(cpu.ea(mode, reg, 0), 0, v);
        return true;
    }

    case 2: case 3: {                                   // FBcc.W / FBcc.L
        u32 base = cpu.pc;
        s32 disp = (op & 0x40) ? (s32)cpu.fetch32() : (s16)cpu.fetch16();
        if (condition(op & 0x3F)) cpu.pc = base + (u32)disp;
        return true;
    }

    case 4:                                             // FSAVE
        if (!cpu.supervisor()) { cpu.privilege(); return true; }
        if (mode < 2 || mode == 3 || (mode == 7 && reg > 1)) return false;
        fsave(cpu);
        return true;

    case 5:                                             // FRESTORE
        if (!cpu.supervisor()) { cpu.privilege(); return true; }
        if (mode < 2 || mode == 4 || (mode == 7 && reg == 4)) return false;
        frestore(cpu);
        return true;

    default:
        return false;
    }
}

void Fpu::fsave(M68K &cpu)
{
    int mode = (cpu.currentOpcode() >> 3) & 7, reg = cpu.currentOpcode() & 7;
    u32 header, size;
    if (!used) { header = 0; size = 0; }
    else if (type == MC68040FPU) { header = 0x41000000; size = 0; }
    else if (type == MC68881) { header = 0x1F180000; size = 0x18; }
    else { header = 0x1F380000; size = 0x38; }
    u32 total = 4 + size;
    u32 addr;
    if (mode == 4) { cpu.a[reg] -= total; addr = cpu.a[reg]; }
    else addr = cpu.ea(mode, reg, 2);
    cpu.wr(addr, 2, header);
    for (u32 i = 4; i < total; i += 4) cpu.wr(addr + i, 2, (i == total - 4 && size) ? 0x70000000 : 0);
}

void Fpu::frestore(M68K &cpu)
{
    int mode = (cpu.currentOpcode() >> 3) & 7, reg = cpu.currentOpcode() & 7;
    u32 addr = mode == 3 ? cpu.a[reg] : cpu.ea(mode, reg, 2);
    u32 header = cpu.rd(addr, 2);
    u32 size = (header >> 16) & 0xFF;
    if (mode == 3) cpu.a[reg] += 4 + size;
    if ((header >> 24) == 0) {                          // null frame: reset the FPU
        reset();
    } else {
        used = true;
    }
}

bool Fpu::moveControl(M68K &cpu, u16 ext, bool toMemory)
{
    u16 op = cpu.currentOpcode();
    int mode = (op >> 3) & 7, reg = op & 7;
    int list = (ext >> 10) & 7;
    int count = (list & 1) + ((list >> 1) & 1) + ((list >> 2) & 1);
    if (count == 0) return false;
    u32 *regs[3] = {&fpcr, &fpsr, &fpiar};
    const u32 masks[3] = {0x0000FFF0, 0x0FFFFFF8, 0xFFFFFFFF};

    if (mode == 0 || mode == 1) {
        if (count != 1 || (mode == 1 && list != 1)) return false;
        int i = list == 4 ? 0 : list == 2 ? 1 : 2;
        u32 &r = mode == 0 ? cpu.d[reg] : cpu.a[reg];
        if (toMemory) r = *regs[i]; else *regs[i] = r & masks[i];
        return true;
    }
    if (mode == 7 && reg == 4) {
        if (toMemory) return false;
        for (int i = 0; i < 3; i++) if (list & (4 >> i)) *regs[i] = cpu.fetch32() & masks[i];
        return true;
    }
    u32 bytes = (u32)count * 4;
    u32 addr;
    if (mode == 4) { cpu.a[reg] -= bytes; addr = cpu.a[reg]; }
    else if (mode == 3) { addr = cpu.a[reg]; cpu.a[reg] += bytes; }
    else addr = cpu.ea(mode, reg, 2);
    for (int i = 0; i < 3; i++) {
        if (!(list & (4 >> i))) continue;
        if (toMemory) cpu.wr(addr, 2, *regs[i]); else *regs[i] = cpu.rd(addr, 2) & masks[i];
        addr += 4;
    }
    return true;
}

bool Fpu::moveMultiple(M68K &cpu, u16 ext, bool toMemory)
{
    u16 op = cpu.currentOpcode();
    int mode = (op >> 3) & 7, reg = op & 7;
    int kind = (ext >> 11) & 3;
    u32 list = (kind & 1) ? cpu.d[(ext >> 4) & 7] & 0xFF : ext & 0xFF;
    bool predec = (kind & 2) == 0;
    if (predec != (mode == 4)) return false;            // the list layout must match the mode
    if (mode < 2 || (toMemory && mode == 3) || (!toMemory && mode == 4) || (mode == 7 && reg > (toMemory ? 1 : 3)))
        return false;
    // Predecrement lists name FP7..FP0 in bits 7..0, others FP0..FP7.
    u32 mask = 0;
    for (int i = 0; i < 8; i++) if (list & (1u << i)) mask |= 1u << (predec ? i : 7 - i);
    int count = 0;
    for (int i = 0; i < 8; i++) if (mask & (1u << i)) count++;
    u32 addr;
    if (mode == 4) { cpu.a[reg] -= (u32)count * 12; addr = cpu.a[reg]; }
    else if (mode == 3) { addr = cpu.a[reg]; cpu.a[reg] += (u32)count * 12; }
    else addr = cpu.ea(mode, reg, 2);
    for (int i = 0; i < 8; i++) {
        if (!(mask & (1u << i))) continue;
        u8 buf[12];
        if (toMemory) {
            toExtended(fp[i], buf);
            for (int j = 0; j < 12; j += 4) cpu.wr(addr + (u32)j, 2, be32(buf + j));
        } else {
            for (int j = 0; j < 12; j += 4) putBe32(buf + j, cpu.rd(addr + (u32)j, 2));
            fp[i] = fromExtended(buf);
        }
        addr += 12;
    }
    return true;
}

static F constantROM(int offset)
{
    switch (offset) {
    case 0x00: return fm::kPi;
    case 0x0B: return fm::kLog10of2;
    case 0x0C: return fm::kE;
    case 0x0D: return fm::kLog2e;
    case 0x0E: return fm::kLog10e;
    case 0x0F: return 0;
    case 0x30: return fm::kLn2;
    case 0x31: return fm::kLn10;
    case 0x32: return 1;
    default:
        if (offset >= 0x33 && offset <= 0x3F) {         // 10^(2^(n-1)): 10, 100, 1e4 ... 1e4096
            F v = 10;
            for (int i = 0x33; i < offset; i++) v *= v;
            return v;
        }
        return 0;
    }
}

bool Fpu::general(M68K &cpu)
{
    u16 op = cpu.currentOpcode();
    int mode = (op >> 3) & 7, reg = op & 7;
    u16 ext = cpu.fetch16();
    int opclass = ext >> 13;
    used = true;
    cpu.cyc += 20;

    switch (opclass) {
    case 0: case 2: {
        int dst = (ext >> 7) & 7;
        F src;
        if (opclass == 0) {
            if (mode || reg) return false;
            src = fp[(ext >> 10) & 7];
        } else {
            int fmt = (ext >> 10) & 7;
            if (fmt == 7) {                             // FMOVECR
                if (mode || reg) return false;
                fp[dst] = roundPrecision(constantROM(ext & 0x7F), (fpcr >> 6) & 3);
                setCC(fp[dst]);
                return true;
            }
            if (!readOperand(cpu, mode, reg, fmt, src)) return false;
        }
        fpiar = cpu.instPC;
        return arithmetic(cpu, ext & 0x7F, dst, src);
    }
    case 3: {                                           // FMOVE to memory
        int fmt = (ext >> 10) & 7;
        int k = fmt == Pdyn ? (s8)(cpu.d[(ext >> 4) & 7] << 1) >> 1 : (s8)((ext & 0x7F) << 1) >> 1;
        F x = fp[(ext >> 7) & 7];
        fpiar = cpu.instPC;
        fpsr &= 0xFFFF00FF;
        return writeOperand(cpu, mode, reg, fmt, k, x);
    }
    case 4: return moveControl(cpu, ext, false);
    case 5: return moveControl(cpu, ext, true);
    case 6: return moveMultiple(cpu, ext, false);
    default: return moveMultiple(cpu, ext, true);
    }
}

bool Fpu::arithmetic(M68K &cpu, int opmode, int dst, F src)
{
    (void)cpu;
    int precision = (fpcr >> 6) & 3;
    // 68040 single/double-rounding variants (opmode 0x40..0x7F).
    if (opmode & 0x40) {
        if (type != MC68040FPU) return false;
        precision = (opmode & 4) ? 2 : 1;
        opmode &= ~0x44;
        if (opmode > 0x2F && opmode != 0x38 && opmode != 0x3A) return false;
    }
    fpsr &= 0xFFFF00FF;                                 // clear exception status byte

    F d = fp[dst];
    F r;
    switch (opmode) {
    case 0x00: r = src; break;                          // FMOVE
    case 0x01: r = roundInteger(src, (fpcr >> 4) & 3); break;   // FINT
    case 0x02: r = fm::sinh(src); break;
    case 0x03: r = fm::trunc(src); break;               // FINTRZ
    case 0x04: if (src < 0) raise(EX_OPERR); r = fm::sqrt(src); break;
    case 0x06: if (src < -1) raise(EX_OPERR); else if (src == -1) raise(EX_DZ); r = fm::log1p(src); break;
    case 0x08: r = fm::expm1(src); break;
    case 0x09: r = fm::tanh(src); break;
    case 0x0A: r = fm::atan(src); break;
    case 0x0C: if (fm::fabs(src) > 1) raise(EX_OPERR); r = fm::asin(src); break;
    case 0x0D: if (fm::fabs(src) > 1) raise(EX_OPERR); else if (fm::fabs(src) == 1) raise(EX_DZ); r = fm::atanh(src); break;
    case 0x0E: if (fm::isInf(src)) raise(EX_OPERR); r = fm::sin(src); break;
    case 0x0F: if (fm::isInf(src)) raise(EX_OPERR); r = fm::tan(src); break;
    case 0x10: r = fm::exp(src); break;
    case 0x11: r = fm::pow2(src); break;
    case 0x12: r = fm::pow10(src); break;
    case 0x14: if (src < 0) raise(EX_OPERR); else if (src == 0) raise(EX_DZ); r = fm::log(src); break;
    case 0x15: if (src < 0) raise(EX_OPERR); else if (src == 0) raise(EX_DZ); r = fm::log(src) * fm::kLog10e; break;
    case 0x16: if (src < 0) raise(EX_OPERR); else if (src == 0) raise(EX_DZ); r = fm::log(src) * fm::kLog2e; break;
    case 0x18: r = fm::fabs(src); break;                // FABS
    case 0x19: r = fm::cosh(src); break;
    case 0x1A: r = -src; break;                         // FNEG
    case 0x1C: if (fm::fabs(src) > 1) raise(EX_OPERR); r = fm::acos(src); break;
    case 0x1D: if (fm::isInf(src)) raise(EX_OPERR); r = fm::cos(src); break;
    case 0x1E: {                                        // FGETEXP
        if (fm::isInf(src)) { raise(EX_OPERR); r = fm::nan(); break; }
        if (src == 0 || fm::isNan(src)) { r = src; break; }
        int e; fm::frexp(src, e); r = (F)(e - 1);
        break;
    }
    case 0x1F: {                                        // FGETMAN
        if (fm::isInf(src)) { raise(EX_OPERR); r = fm::nan(); break; }
        if (src == 0 || fm::isNan(src)) { r = src; break; }
        int e; r = fm::frexp(src, e) * 2;
        break;
    }
    case 0x20: if (src == 0 && d != 0 && !fm::isNan(d)) raise(EX_DZ); r = d / src; break;   // FDIV
    case 0x21: case 0x25: {                             // FMOD, FREM
        int q; bool qneg;
        r = fm::fmodQ(d, src, opmode == 0x25, q, qneg);
        if (src == 0 || fm::isInf(d)) raise(EX_OPERR);
        fpsr = (fpsr & 0xFF00FFFF) | ((u32)(qneg ? 0x80 : 0) | (u32)q) << 16;
        break;
    }
    case 0x22: r = d + src; break;                      // FADD
    case 0x23: r = d * src; break;                      // FMUL
    case 0x24: r = (F)(float)((F)(float)d / (F)(float)src); break;   // FSGLDIV
    case 0x26: {                                        // FSCALE
        F n = fm::trunc(src);
        if (n > 20000) n = 20000;
        if (n < -20000) n = -20000;
        r = fm::scalb(d, (int)n);
        break;
    }
    case 0x27: r = (F)(float)((F)(float)d * (F)(float)src); break;   // FSGLMUL
    case 0x28: r = d - src; break;                      // FSUB
    case 0x30: case 0x31: case 0x32: case 0x33: case 0x34: case 0x35: case 0x36: case 0x37:
        if (fm::isInf(src)) raise(EX_OPERR);
        fp[opmode & 7] = roundPrecision(fm::cos(src), precision);
        r = fm::sin(src);
        break;
    case 0x38: {                                        // FCMP
        fpsr &= 0x00FFFFFF;
        if (fm::isNan(d) || fm::isNan(src)) { fpsr |= CC_NAN; return true; }
        if (d == src) { fpsr |= CC_Z; if (fm::signBit(d)) fpsr |= CC_N; return true; }
        if (d < src) fpsr |= CC_N;
        return true;
    }
    case 0x3A: setCC(src); return true;                 // FTST
    default: return false;
    }

    if (fm::isNan(r) && !fm::isNan(src) && !fm::isNan(d)) raise(EX_OPERR);
    if (fm::isInf(r) && !fm::isInf(src) && !fm::isInf(d) && !(fpsr & EX_DZ)) raise(EX_OVFL);
    r = roundPrecision(r, precision);
    fp[dst] = r;
    setCC(r);
    return true;
}

} // namespace macemu
