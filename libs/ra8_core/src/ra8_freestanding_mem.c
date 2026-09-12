/**
 * @file ra8_freestanding_mem.c
 * @brief Project-owned freestanding memory ABI primitives (memset, memcpy, memmove, memcmp, memchr).
 *
 * @par Tag
 * [Ring 0 / Foundation] {World: Dual}
 *
 * @details
 * Standard ISO C memory primitives implemented directly for the bare-metal
 * RA8 target firmware. Compiled with loop-distribution optimization disabled
 * to guarantee that the compiler never lowers primitive bodies into self-recursive
 * runtime calls. Fully deterministic, bounds-safe, and heap-free.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize("no-tree-loop-distribute-patterns")
#endif

#include <stddef.h>
#include <stdint.h>

#include "ra8_freestanding.h"

#if !defined(RA8_OFF_TARGET) || defined(RA8_TEST_FREESTANDING)

/**
 * @brief Fill memory with a constant byte value.
 * @details Sets the first @p n bytes of @p dst to @p value converted to uint8_t.
 * @param[out] dst   Destination buffer to fill.
 * @param[in]  value Byte value to write.
 * @param[in]  n     Number of bytes to write.
 * @return Original destination pointer @p dst.
 * @retval dst The destination pointer passed in @p dst is always returned.
 * @pre @p dst points to at least @p n writable bytes if @p n > 0.
 * @pre The destination memory region is addressable and valid.
 * @post The first @p n bytes of @p dst contain (uint8_t)@p value.
 * @post Bytes beyond @p n are unmodified.
 * @note Freestanding runtime primitive; never allocates; reentrant and thread-safe when destination memory is not concurrently modified.
 * @since 0.1.0
 */
void* memset(void* dst, int value, size_t n)
{
  uint8_t*      d = (uint8_t*)dst;
  const uint8_t v = (uint8_t)value;
  for (size_t i = 0U; i < n; ++i) {
    d[i] = v;
  }
  return dst;
}

/**
 * @brief Copy memory area between non-overlapping regions.
 * @details Copies @p n bytes from @p src to @p dst sequentially.
 * @param[out] dst Destination buffer pointer.
 * @param[in]  src Source buffer pointer.
 * @param[in]  n   Number of bytes to copy.
 * @return Original destination pointer @p dst.
 * @retval dst The destination pointer passed in @p dst is always returned.
 * @pre @p dst and @p src do not overlap.
 * @pre @p dst and @p src point to at least @p n valid bytes if @p n > 0.
 * @post The @p n bytes at @p dst match the @p n bytes at @p src.
 * @post Source memory is unmodified.
 * @note Freestanding runtime primitive; never allocates; reentrant and thread-safe when memory regions are not concurrently modified.
 * @since 0.1.0
 */
void* memcpy(void* restrict dst, const void* restrict src, size_t n)
{
  uint8_t* restrict d       = (uint8_t* restrict)dst;
  const uint8_t* restrict s = (const uint8_t* restrict)src;
  for (size_t i = 0U; i < n; ++i) {
    d[i] = s[i];
  }
  return dst;
}

/**
 * @brief Copy memory area between potentially overlapping regions.
 * @details Copies @p n bytes from @p src to @p dst handling overlaps safely.
 * @param[out] dst Destination buffer pointer.
 * @param[in]  src Source buffer pointer.
 * @param[in]  n   Number of bytes to copy.
 * @return Original destination pointer @p dst.
 * @retval dst The destination pointer passed in @p dst is always returned.
 * @pre @p dst points to at least @p n writable bytes if @p n > 0.
 * @pre @p src points to at least @p n readable bytes if @p n > 0.
 * @post The @p n bytes at @p dst match the initial @p n bytes at @p src.
 * @post Handles any overlap between @p dst and @p src correctly.
 * @note Freestanding runtime primitive; never allocates; reentrant and thread-safe when memory regions are not concurrently modified.
 * @since 0.1.0
 */
void* memmove(void* dst, const void* src, size_t n)
{
  if (dst == src) {
    return dst;
  }
  if (n == 0U) {
    return dst;
  }
  uint8_t*       d = (uint8_t*)dst;
  const uint8_t* s = (const uint8_t*)src;
  if ((uintptr_t)d < (uintptr_t)s) {
    for (size_t i = 0U; i < n; ++i) {
      d[i] = s[i];
    }
  } else {
    size_t i = n;
    while (i > 0U) {
      --i;
      d[i] = s[i];
    }
  }
  return dst;
}

/**
 * @brief Compare bytes in two memory areas.
 * @details Compares the first @p n bytes of @p a and @p b as unsigned char.
 * @param[in] a First memory area pointer.
 * @param[in] b Second memory area pointer.
 * @param[in] n Maximum number of bytes to compare.
 * @return Negative if a < b, positive if a > b, 0 if equal.
 * @retval 0  All @p n bytes match or @p n is zero.
 * @retval <0 First differing byte in @p a is less than in @p b.
 * @retval >0 First differing byte in @p a is greater than in @p b.
 * @pre @p a points to at least @p n readable bytes if @p n > 0.
 * @pre @p b points to at least @p n readable bytes if @p n > 0.
 * @post Input buffers are unmodified.
 * @post Comparison semantics follow unsigned byte order.
 * @note Freestanding runtime primitive; never allocates; reentrant and thread-safe when memory regions are not concurrently modified.
 * @since 0.1.0
 */
int memcmp(const void* a, const void* b, size_t n)
{
  const uint8_t* p1 = (const uint8_t*)a;
  const uint8_t* p2 = (const uint8_t*)b;
  for (size_t i = 0U; i < n; ++i) {
    if (p1[i] != p2[i]) {
      return (p1[i] < p2[i]) ? -1 : 1;
    }
  }
  return 0;
}

/**
 * @brief Locate a byte in a memory area.
 * @details Scans the initial @p n bytes of @p s for the first instance of @p c.
 * @param[in] s Pointer to memory buffer.
 * @param[in] c Byte value to search for.
 * @param[in] n Maximum bytes to examine.
 * @return Pointer to matching byte, or nullptr if not found.
 * @retval non-null Pointer to matching byte within @p s.
 * @retval nullptr  Byte not found within first @p n bytes, or @p n is zero.
 * @pre @p s points to at least @p n readable bytes if @p n > 0.
 * @pre @p s is addressable and valid.
 * @post Memory buffer is unmodified.
 * @post Returned pointer is within [s, s + n) on success.
 * @note Freestanding runtime primitive; never allocates; reentrant and thread-safe when memory is not concurrently modified.
 * @since 0.1.0
 */
void* memchr(const void* s, int c, size_t n)
{
  const uint8_t* p      = (const uint8_t*)s;
  const uint8_t  target = (uint8_t)c;
  for (size_t i = 0U; i < n; ++i) {
    if (p[i] == target) {
      return (void*)(uintptr_t)(p + i);
    }
  }
  return nullptr;
}
#endif /* !RA8_OFF_TARGET || RA8_TEST_FREESTANDING */
