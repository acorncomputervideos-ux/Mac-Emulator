// Basic types for the emulator core.
//
// The core is written as freestanding C++: no libc, no STL, no exceptions.
// That lets the very same sources build inside a Circle bare-metal kernel
// and inside the hosted test harness.

#ifndef MACEMU_TYPES_H
#define MACEMU_TYPES_H

namespace macemu {

typedef unsigned char u8;
typedef signed char s8;
typedef unsigned short u16;
typedef signed short s16;
typedef unsigned int u32;
typedef signed int s32;
typedef unsigned long long u64;
typedef signed long long s64;

inline void memCopy(void *dst, const void *src, u32 n) { __builtin_memcpy(dst, src, n); }
inline void memFill(void *dst, int v, u32 n) { __builtin_memset(dst, v, n); }
inline int memCompare(const void *a, const void *b, u32 n) { return __builtin_memcmp(a, b, n); }

inline u16 be16(const u8 *p) { return (u16)((p[0] << 8) | p[1]); }
inline u32 be32(const u8 *p) { return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3]; }
inline void putBe16(u8 *p, u16 v) { p[0] = (u8)(v >> 8); p[1] = (u8)v; }
inline void putBe32(u8 *p, u32 v) { p[0] = (u8)(v >> 24); p[1] = (u8)(v >> 16); p[2] = (u8)(v >> 8); p[3] = (u8)v; }

// A random-access block of bytes that backs a disk image.  The frontend
// implements this on top of whatever storage it has (FAT file, RAM, ...).
class Storage {
public:
    virtual ~Storage() {}
    virtual u32 size() = 0;                                  // bytes
    virtual bool read(u32 offset, void *buf, u32 len) = 0;
    virtual bool write(u32 offset, const void *buf, u32 len) = 0;
    virtual bool readOnly() = 0;
    virtual void flush() {}
};

} // namespace macemu

#endif
