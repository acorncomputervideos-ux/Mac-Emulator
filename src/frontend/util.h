// Small freestanding string helpers shared by the frontends.

#ifndef MACEMU_UTIL_H
#define MACEMU_UTIL_H

#include "../core/types.h"

namespace macemu {

inline u32 strLen(const char *s) { u32 n = 0; while (s[n]) n++; return n; }

inline void strCopy(char *dst, const char *src, u32 max)
{
    u32 i = 0;
    if (!max) return;
    for (; src[i] && i + 1 < max; i++) dst[i] = src[i];
    dst[i] = 0;
}

inline void strAppend(char *dst, const char *src, u32 max)
{
    u32 n = strLen(dst);
    if (n < max) strCopy(dst + n, src, max - n);
}

inline char lower(char c) { return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c; }

inline bool strEq(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

inline bool strEqNoCase(const char *a, const char *b)
{
    while (*a && lower(*a) == lower(*b)) { a++; b++; }
    return lower(*a) == lower(*b);
}

inline bool endsWithNoCase(const char *s, const char *suffix)
{
    u32 n = strLen(s), m = strLen(suffix);
    return n >= m && strEqNoCase(s + n - m, suffix);
}

inline void appendUint(char *dst, u32 v, u32 max, u32 minDigits = 1)
{
    char tmp[12];
    u32 n = 0;
    do { tmp[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n < minDigits && n < 11) tmp[n++] = '0';
    char out[12];
    for (u32 i = 0; i < n; i++) out[i] = tmp[n - 1 - i];
    out[n] = 0;
    strAppend(dst, out, max);
}

inline void appendHex(char *dst, u32 v, u32 digits, u32 max)
{
    char out[9];
    for (u32 i = 0; i < digits; i++) out[i] = "0123456789ABCDEF"[(v >> (4 * (digits - 1 - i))) & 15];
    out[digits] = 0;
    strAppend(dst, out, max);
}

// Seconds since 1904-01-01 (the Macintosh epoch) for a civil date.
inline u32 macTime(int year, int month, int day, int hour, int minute, int second)
{
    int y = year - (month <= 2 ? 1 : 0);
    int era = y / 400;
    int yoe = y - era * 400;
    int doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    s64 days = (s64)era * 146097 + doe - 719468;        // days since 1970-01-01
    days += 24107;                                      // 1904-01-01 .. 1970-01-01
    return (u32)(days * 86400 + hour * 3600 + minute * 60 + second);
}

} // namespace macemu

#endif
