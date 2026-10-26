/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 *
 * Minimal freestanding string.h shim for the portability gate
 * (tools/check_portability.py). A real port supplies its own libc; this shim
 * exists only so the gate can prove the portable rows compile for
 * riscv32 (ESP32-C) and thumbv7em (TI / Cortex-M4) without pulling one in.
 * Declarations only, no definitions: the gate compiles with -c.
 */
#ifndef RA8_GATE_STRING_H
#define RA8_GATE_STRING_H

#include <stddef.h>

void *memcpy(void *restrict dst, const void *restrict src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
void *memset(void *dst, int c, size_t n);
int memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
size_t strnlen(const char *s, size_t n);
char *strcpy(char *restrict dst, const char *restrict src);
char *strncpy(char *restrict dst, const char *restrict src, size_t n);
char *strcat(char *restrict dst, const char *restrict src);
char *strncat(char *restrict dst, const char *restrict src, size_t n);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t n);
char *strchr(const char *s, int c);
char *strrchr(const char *s, int c);
char *strstr(const char *hay, const char *needle);

#endif /* RA8_GATE_STRING_H */
