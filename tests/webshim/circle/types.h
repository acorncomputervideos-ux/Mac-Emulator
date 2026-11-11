// Host stand-ins for the parts of Circle and FatFs the web server uses,
// so that it can be tested with real sockets and files (tests/webtest.cpp).
#ifndef SHIM_TYPES_H
#define SHIM_TYPES_H
#include <stdint.h>
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64;
typedef bool boolean;
#define TRUE true
#define FALSE false
#endif
