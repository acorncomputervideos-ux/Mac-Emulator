/* Minimal <string.h> for building BearSSL inside the Circle kernel: the
   functions come from Circle's libcircle (circle/util.h). */
#ifndef MACEMU_SHIM_STRING_H
#define MACEMU_SHIM_STRING_H
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
void *memcpy(void *dest, const void *src, size_t n);
void *memmove(void *dest, const void *src, size_t n);
void *memset(void *s, int c, size_t n);
int memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
#ifdef __cplusplus
}
#endif
#endif
