/* Minimal <limits.h> for building BearSSL inside the Circle kernel (-nostdinc). */
#ifndef MACEMU_SHIM_LIMITS_H
#define MACEMU_SHIM_LIMITS_H
#define CHAR_BIT __CHAR_BIT__
#define INT_MAX __INT_MAX__
#define INT_MIN (-__INT_MAX__ - 1)
#define UINT_MAX (__INT_MAX__ * 2U + 1U)
#define LONG_MAX __LONG_MAX__
#define ULONG_MAX (__LONG_MAX__ * 2UL + 1UL)
#define SIZE_MAX __SIZE_MAX__
#endif
