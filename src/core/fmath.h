// Freestanding long double maths for the FPU emulation.
//
// The bare-metal build has no libm, so the functions the 68881 implements
// in microcode are written here: range reduction plus series that converge
// to full extended (64-bit mantissa) precision.  `long double` is IEEE quad
// on AArch64 (exact superset of 68881 extended), x87 extended on x86 hosts
// and plain double on 32-bit ARM.

#ifndef MACEMU_FMATH_H
#define MACEMU_FMATH_H

#include "types.h"

namespace macemu {
namespace fm {

typedef long double F;

const F kPi = 3.14159265358979323846264338327950288L;
const F kLn2 = 0.693147180559945309417232121458176568L;
const F kLn10 = 2.30258509299404568401799145468436421L;
const F kLog2e = 1.44269504088896340735992468100189214L;
const F kLog10e = 0.434294481903251827651128918916605082L;
const F kLog10of2 = 0.301029995663981195213738894724493027L;
const F kE = 2.71828182845904523536028747135266250L;

inline bool isNan(F x) { return x != x; }
inline bool isInf(F x) { return !isNan(x) && isNan(x - x); }
inline bool signBit(F x) { return x < 0 || (x == 0 && 1 / x < 0); }
inline F fabs(F x) { return signBit(x) ? -x : x; }
F inf();
F nan();

F scalb(F x, int n);                 // x * 2^n
F frexp(F x, int &e);                // x = m * 2^e, 0.5 <= |m| < 1
F trunc(F x);
F floor(F x);
F roundEven(F x);
F sqrt(F x);
F exp(F x);
F expm1(F x);
F log(F x);
F log1p(F x);
F sin(F x);
F cos(F x);
F tan(F x);
F atan(F x);
F asin(F x);
F acos(F x);
F sinh(F x);
F cosh(F x);
F tanh(F x);
F atanh(F x);
F pow2(F x);
F pow10(F x);

// IEEE remainder style helpers for FMOD (truncating) and FREM (nearest);
// the low seven bits of the quotient and its sign come back too.
F fmodQ(F x, F y, bool nearest, int &quotient, bool &qneg);

} // namespace fm
} // namespace macemu

#endif
