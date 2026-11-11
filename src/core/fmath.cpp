#include "fmath.h"

namespace macemu {
namespace fm {

// Volatile so the compiler does not fold these at build time with host rules.
F inf() { volatile F z = 0; return 1 / z; }
F nan() { volatile F z = 0; return z / z; }

static const int kMantBits = __LDBL_MANT_DIG__;
static F kTwo32 = 4294967296.0L;

F scalb(F x, int n)
{
    if (x == 0 || isNan(x) || isInf(x)) return x;
    while (n >= 32) { x *= kTwo32; n -= 32; if (isInf(x)) return x; }
    while (n <= -32) { x /= kTwo32; n += 32; if (x == 0) return x; }
    if (n > 0) x *= (F)(1u << n);
    else if (n < 0) x /= (F)(1u << -n);
    return x;
}

F frexp(F x, int &e)
{
    e = 0;
    if (x == 0 || isNan(x) || isInf(x)) return x;
    F m = fabs(x);
    // Coarse steps of 2^32, then single bits.
    while (m >= kTwo32) { m /= kTwo32; e += 32; }
    while (m < 1 / kTwo32) { m *= kTwo32; e -= 32; }
    while (m >= 1) { m /= 2; e++; }
    while (m < 0.5L) { m *= 2; e--; }
    return signBit(x) ? -m : m;
}

F trunc(F x)
{
    if (isNan(x) || isInf(x)) return x;
    F a = fabs(x);
    if (a < 1) return signBit(x) ? -0.0L : 0.0L;
    // Adding 2^(p-1) pushes the fraction bits out (round to nearest); step
    // back if that rounded up.  Values that large are already integers.
    static const F big = scalb(1, kMantBits - 1);
    if (a >= big) return x;
    volatile F t = a + big;
    F w = t - big;
    if (w > a) w -= 1;
    return signBit(x) ? -w : w;
}

F floor(F x)
{
    F t = trunc(x);
    return (t > x) ? t - 1 : t;
}

F roundEven(F x)
{
    if (isNan(x) || isInf(x)) return x;
    F f = floor(x);
    F diff = x - f;
    if (diff > 0.5L) return f + 1;
    if (diff < 0.5L) return f == 0 && signBit(x) ? -0.0L : f;
    // tie: pick the even neighbour
    F half = f / 2;
    return floor(half) == half ? f : f + 1;
}

F sqrt(F x)
{
    if (isNan(x) || x < 0) return nan();
    if (x == 0 || isInf(x)) return x;
    int e;
    F m = frexp(x, e);
    if (e & 1) { m *= 2; e--; }                         // m in [0.5, 2)
    F y = (F)__builtin_sqrt((double)m);
    for (int i = 0; i < 3; i++) y = (y + m / y) / 2;
    return scalb(y, e / 2);
}

// exp(r) for |r| <= ln2/2 by Taylor series.
static F expSmall(F r)
{
    F sum = 1, term = 1;
    for (int n = 1; n < 40; n++) {
        term *= r / n;
        F next = sum + term;
        if (next == sum) break;
        sum = next;
    }
    return sum;
}

// ln 2 and pi/2 split into a 40-bit head (k * head is exact) and a tail.
static const F kLn2Hi = 762123384785.0L / 1099511627776.0L;
static const F kLn2Lo = 7.37100256516779890183404013000134360e-13L;
static const F kPio2Hi = 1727108826178.0L / 1099511627776.0L;
static const F kPio2Lo = 7.44354748048662312358863973584699688e-13L;

F exp(F x)
{
    if (isNan(x)) return x;
    if (x > 11400) return inf();
    if (x < -11400) return 0;
    F k = roundEven(x * kLog2e);
    F r = (x - k * kLn2Hi) - k * kLn2Lo;
    return scalb(expSmall(r), (int)k);
}

F expm1(F x)
{
    if (fabs(x) < 0.5L) {
        F sum = 0, term = 1;
        for (int n = 1; n < 60; n++) {
            term *= x / n;
            F next = sum + term;
            if (next == sum) break;
            sum = next;
        }
        return sum;
    }
    return exp(x) - 1;
}

// log(m) for m in [sqrt(1/2), sqrt(2)): 2 atanh(s), s = (m-1)/(m+1).
static F logCore(F m)
{
    F s = (m - 1) / (m + 1);
    F s2 = s * s;
    F sum = 0, p = s;
    for (int n = 1; n < 200; n += 2) {
        F next = sum + p / n;
        if (next == sum) break;
        sum = next;
        p *= s2;
    }
    return 2 * sum;
}

F log(F x)
{
    if (isNan(x) || x < 0) return nan();
    if (x == 0) return -inf();
    if (isInf(x)) return x;
    int e;
    F m = frexp(x, e);                                  // [0.5, 1)
    if (m < 0.70710678118654752440L) { m *= 2; e--; }
    return logCore(m) + e * kLn2;
}

F log1p(F x)
{
    if (isNan(x) || x < -1) return nan();
    if (x == -1) return -inf();
    F u = 1 + x;
    if (u == 1) return x;
    // Corrects the rounding error of 1 + x.
    return log(u) * (x / (u - 1));
}

// Reduce x to r in [-pi/4, pi/4] with x = r + q * pi/2.
static F reduceHalfPi(F x, int &q)
{
    F k = roundEven(x * (2 / kPi));
    F r = (x - k * kPio2Hi) - k * kPio2Lo;
    F km = k - 4 * floor(k / 4);
    q = (int)km;
    return r;
}

static F sinSeries(F r)
{
    F r2 = r * r, term = r, sum = r;
    for (int n = 3; n < 80; n += 2) {
        term *= -r2 / ((n - 1) * n);
        F next = sum + term;
        if (next == sum) break;
        sum = next;
    }
    return sum;
}

static F cosSeries(F r)
{
    F r2 = r * r, term = 1, sum = 1;
    for (int n = 2; n < 80; n += 2) {
        term *= -r2 / ((n - 1) * n);
        F next = sum + term;
        if (next == sum) break;
        sum = next;
    }
    return sum;
}

F sin(F x)
{
    if (isNan(x) || isInf(x)) return nan();
    int q;
    F r = reduceHalfPi(x, q);
    switch (q) {
    case 0: return sinSeries(r);
    case 1: return cosSeries(r);
    case 2: return -sinSeries(r);
    default: return -cosSeries(r);
    }
}

F cos(F x)
{
    if (isNan(x) || isInf(x)) return nan();
    int q;
    F r = reduceHalfPi(x, q);
    switch (q) {
    case 0: return cosSeries(r);
    case 1: return -sinSeries(r);
    case 2: return -cosSeries(r);
    default: return sinSeries(r);
    }
}

F tan(F x)
{
    if (isNan(x) || isInf(x)) return nan();
    int q;
    F r = reduceHalfPi(x, q);
    F s = sinSeries(r), c = cosSeries(r);
    return (q & 1) ? -c / s : s / c;
}

F atan(F x)
{
    if (isNan(x)) return x;
    if (isInf(x)) return signBit(x) ? -kPi / 2 : kPi / 2;
    bool neg = signBit(x);
    F a = fabs(x);
    bool inv = a > 1;
    if (inv) a = 1 / a;
    // Halve the argument twice: atan(a) = 2 atan(a / (1 + sqrt(1 + a^2))).
    int doublings = 0;
    while (a > 0.125L) { a = a / (1 + sqrt(1 + a * a)); doublings++; }
    F a2 = a * a, p = a, sum = 0;
    for (int n = 1; n < 400; n += 2) {
        F next = sum + ((n & 2) ? -p / n : p / n);
        if (next == sum) break;
        sum = next;
        p *= a2;
    }
    sum = scalb(sum, doublings);
    if (inv) sum = kPi / 2 - sum;
    return neg ? -sum : sum;
}

F asin(F x)
{
    if (isNan(x) || x > 1 || x < -1) return nan();
    if (x == 1) return kPi / 2;
    if (x == -1) return -kPi / 2;
    return atan(x / sqrt((1 - x) * (1 + x)));
}

F acos(F x)
{
    if (isNan(x) || x > 1 || x < -1) return nan();
    if (x == -1) return kPi;
    return 2 * atan(sqrt((1 - x) / (1 + x)));
}

F sinh(F x)
{
    if (fabs(x) < 0.5L) {
        F e = expm1(x);
        return (e + e / (e + 1)) / 2;
    }
    F e = exp(x);
    return (e - 1 / e) / 2;
}

F cosh(F x)
{
    F e = exp(fabs(x));
    return (e + 1 / e) / 2;
}

F tanh(F x)
{
    if (isNan(x)) return x;
    if (x > 50) return 1;
    if (x < -50) return -1;
    F e = expm1(2 * x);
    return e / (e + 2);
}

F atanh(F x)
{
    if (isNan(x) || x > 1 || x < -1) return nan();
    if (x == 1) return inf();
    if (x == -1) return -inf();
    return log1p(2 * x / (1 - x)) / 2;
}

F pow2(F x)
{
    if (isNan(x)) return x;
    if (x > 16500) return inf();
    if (x < -16500) return 0;
    F k = roundEven(x);
    return scalb(expSmall((x - k) * kLn2), (int)k);
}

F pow10(F x)
{
    if (isNan(x)) return x;
    // Exact powers of ten for integer arguments where possible.
    if (x == trunc(x) && fabs(x) <= 27) {
        F r = 1;
        for (int i = 0; i < (int)fabs(x); i++) r *= 10;
        return x < 0 ? 1 / r : r;
    }
    return pow2(x * (kLn10 / kLn2));
}

F fmodQ(F x, F y, bool nearest, int &quotient, bool &qneg)
{
    quotient = 0;
    qneg = signBit(x) != signBit(y);
    if (isNan(x) || isNan(y) || isInf(x) || y == 0) return nan();
    if (isInf(y)) return x;
    F ax = fabs(x), ay = fabs(y);
    // Long division on the exponents: exact.
    u32 q = 0;
    int ex, ey;
    frexp(ax, ex);
    frexp(ay, ey);
    F r = ax;
    for (int sh = ex - ey; sh >= 0; sh--) {
        F t = scalb(ay, sh);
        q <<= 1;
        if (r >= t) { r -= t; q |= 1; }
    }
    if (nearest) {
        F twice = 2 * r;
        if (twice > ay || (twice == ay && (q & 1))) { r -= ay; q++; }
    }
    quotient = (int)(q & 0x7F);
    return signBit(x) ? -r : r;
}

} // namespace fm
} // namespace macemu
