/* translate-c seam shim: memset/memcmp appear in translated inline bodies. */
#ifndef SEAM_SHIM_STRING_H
#define SEAM_SHIM_STRING_H
#include <stddef.h>
extern void *memset(void *, int, size_t);
extern void *memcpy(void *, const void *, size_t);
extern int memcmp(const void *, const void *, size_t);
#endif
