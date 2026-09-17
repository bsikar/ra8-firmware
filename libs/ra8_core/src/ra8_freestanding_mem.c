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
void* memcpy(void* dst, const void* src, size_t n)
{
  uint8_t*       d = (uint8_t*)dst;
  const uint8_t* s = (const uint8_t*)src;
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
  if (d < s) {
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
      const uint8_t* result_source = &p[i];
      void*          result        = nullptr;
      (void)__builtin_memcpy((void*)&result, (const void*)&result_source, sizeof(result));
      return result;
    }
  }
  return nullptr;
}

#if defined(__arm__) || defined(RA8_TEST_FREESTANDING)

/*
 * Arm C Library ABI (AEABI) memory helpers.
 *
 * Compiler-generated aggregate copies and zero-initialisation on this target
 * call the AEABI entry points rather than the ISO C primitives above. A
 * migrated Zig archive cross-compiled for cortex_m85 emits __aeabi_memcpy,
 * __aeabi_memcpy8, __aeabi_memset, __aeabi_memclr, __aeabi_memclr4 and
 * __aeabi_memclr8, and a -nostdlib application link has no newlib behind it to
 * resolve them, which is the second half of issue #948. The whole family is
 * defined here rather than only the six symbols measured so far: which variant
 * a compiler picks for a given aggregate is a codegen decision, and the
 * alignment-suffixed forms differ only in a guarantee the byte-wise bodies
 * below never rely on.
 *
 * The argument orders are NOT the ISO C ones, and that is the trap this block
 * exists to get right once. __aeabi_memset(dst, n, value) takes the length
 * SECOND where memset(dst, value, n) takes it third, and __aeabi_memclr(dst, n)
 * takes no value at all. A transposed shim links clean and corrupts memory, so
 * tests/core/src/test_ra8_freestanding.c asserts the order of every one.
 *
 * Each body delegates to the primitive above it instead of open-coding a
 * second loop, so each operation has exactly one implementation in the
 * firmware. Measured with arm-none-eabi-gcc 13.3.1 for cortex-m85 at -O0, -Og,
 * -Os and -O2: the delegating call is emitted as `bl memcpy` / `bl memset` and
 * never back into the AEABI entry point, so no shim is self-recursive at any
 * optimisation level this project configures.
 */

/**
 * @brief Copy memory between non-overlapping regions (unaligned AEABI helper).
 * @details Copies @p n bytes from @p src to @p dst by delegating to memcpy().
 *          This entry point carries no alignment guarantee; the claim is never relied upon, since the
 *          copy is byte-wise.
 * @param[out] dst Destination buffer pointer.
 * @param[in]  src Source buffer pointer.
 * @param[in]  n   Number of bytes to copy.
 * @pre @p dst and @p src do not overlap.
 * @pre @p dst and @p src point to at least @p n valid bytes if @p n > 0.
 * @post The @p n bytes at @p dst match the @p n bytes at @p src.
 * @post Source memory is unmodified.
 * @note Compiler runtime helper; returns nothing, unlike memcpy(); never allocates; reentrant and thread-safe when memory regions are not concurrently modified.
 * @since 0.1.0
 */
void __aeabi_memcpy(void* dst, const void* src, size_t n)
{
  (void)memcpy(dst, src, n);
}

/**
 * @brief Copy memory between non-overlapping regions (4-byte aligned AEABI helper).
 * @details Copies @p n bytes from @p src to @p dst by delegating to memcpy().
 *          This entry point declares 4-byte aligned operands; the claim is never relied upon, since the
 *          copy is byte-wise.
 * @param[out] dst Destination buffer pointer.
 * @param[in]  src Source buffer pointer.
 * @param[in]  n   Number of bytes to copy.
 * @pre @p dst and @p src do not overlap.
 * @pre @p dst and @p src point to at least @p n valid bytes if @p n > 0.
 * @post The @p n bytes at @p dst match the @p n bytes at @p src.
 * @post Source memory is unmodified.
 * @note Compiler runtime helper; returns nothing, unlike memcpy(); never allocates; reentrant and thread-safe when memory regions are not concurrently modified.
 * @since 0.1.0
 */
void __aeabi_memcpy4(void* dst, const void* src, size_t n)
{
  (void)memcpy(dst, src, n);
}

/**
 * @brief Copy memory between non-overlapping regions (8-byte aligned AEABI helper).
 * @details Copies @p n bytes from @p src to @p dst by delegating to memcpy().
 *          This entry point declares 8-byte aligned operands; the claim is never relied upon, since the
 *          copy is byte-wise.
 * @param[out] dst Destination buffer pointer.
 * @param[in]  src Source buffer pointer.
 * @param[in]  n   Number of bytes to copy.
 * @pre @p dst and @p src do not overlap.
 * @pre @p dst and @p src point to at least @p n valid bytes if @p n > 0.
 * @post The @p n bytes at @p dst match the @p n bytes at @p src.
 * @post Source memory is unmodified.
 * @note Compiler runtime helper; returns nothing, unlike memcpy(); never allocates; reentrant and thread-safe when memory regions are not concurrently modified.
 * @since 0.1.0
 */
void __aeabi_memcpy8(void* dst, const void* src, size_t n)
{
  (void)memcpy(dst, src, n);
}

/**
 * @brief Copy memory between potentially overlapping regions (unaligned AEABI helper).
 * @details Copies @p n bytes from @p src to @p dst by delegating to memmove(),
 *          which selects the copy direction. This entry point carries no alignment guarantee; the claim
 *          is never relied upon.
 * @param[out] dst Destination buffer pointer.
 * @param[in]  src Source buffer pointer.
 * @param[in]  n   Number of bytes to copy.
 * @pre @p dst points to at least @p n writable bytes if @p n > 0.
 * @pre @p src points to at least @p n readable bytes if @p n > 0.
 * @post The @p n bytes at @p dst match the initial @p n bytes at @p src.
 * @post Any overlap between @p dst and @p src is handled correctly.
 * @note Compiler runtime helper; returns nothing, unlike memmove(); never allocates; reentrant and thread-safe when memory regions are not concurrently modified.
 * @since 0.1.0
 */
void __aeabi_memmove(void* dst, const void* src, size_t n)
{
  (void)memmove(dst, src, n);
}

/**
 * @brief Copy memory between potentially overlapping regions (4-byte aligned AEABI helper).
 * @details Copies @p n bytes from @p src to @p dst by delegating to memmove(),
 *          which selects the copy direction. This entry point declares 4-byte aligned operands; the claim
 *          is never relied upon.
 * @param[out] dst Destination buffer pointer.
 * @param[in]  src Source buffer pointer.
 * @param[in]  n   Number of bytes to copy.
 * @pre @p dst points to at least @p n writable bytes if @p n > 0.
 * @pre @p src points to at least @p n readable bytes if @p n > 0.
 * @post The @p n bytes at @p dst match the initial @p n bytes at @p src.
 * @post Any overlap between @p dst and @p src is handled correctly.
 * @note Compiler runtime helper; returns nothing, unlike memmove(); never allocates; reentrant and thread-safe when memory regions are not concurrently modified.
 * @since 0.1.0
 */
void __aeabi_memmove4(void* dst, const void* src, size_t n)
{
  (void)memmove(dst, src, n);
}

/**
 * @brief Copy memory between potentially overlapping regions (8-byte aligned AEABI helper).
 * @details Copies @p n bytes from @p src to @p dst by delegating to memmove(),
 *          which selects the copy direction. This entry point declares 8-byte aligned operands; the claim
 *          is never relied upon.
 * @param[out] dst Destination buffer pointer.
 * @param[in]  src Source buffer pointer.
 * @param[in]  n   Number of bytes to copy.
 * @pre @p dst points to at least @p n writable bytes if @p n > 0.
 * @pre @p src points to at least @p n readable bytes if @p n > 0.
 * @post The @p n bytes at @p dst match the initial @p n bytes at @p src.
 * @post Any overlap between @p dst and @p src is handled correctly.
 * @note Compiler runtime helper; returns nothing, unlike memmove(); never allocates; reentrant and thread-safe when memory regions are not concurrently modified.
 * @since 0.1.0
 */
void __aeabi_memmove8(void* dst, const void* src, size_t n)
{
  (void)memmove(dst, src, n);
}

/**
 * @brief Fill memory with a constant byte value (unaligned AEABI helper).
 * @details Writes (uint8_t)@p value to the first @p n bytes of @p dst by
 *          delegating to memset(). The length is the SECOND parameter here and
 *          the third in memset(). This entry point carries no alignment guarantee.
 * @param[out] dst   Destination buffer to fill.
 * @param[in]  n     Number of bytes to write.
 * @param[in]  value Byte value to write.
 * @pre @p dst points to at least @p n writable bytes if @p n > 0.
 * @pre The destination memory region is addressable and valid.
 * @post The first @p n bytes of @p dst contain (uint8_t)@p value.
 * @post Bytes beyond @p n are unmodified.
 * @note Compiler runtime helper; takes (dst, n, value), never memset()'s (dst, value, n); never allocates; reentrant and thread-safe when destination memory is not concurrently modified.
 * @since 0.1.0
 */
void __aeabi_memset(void* dst, size_t n, int value)
{
  (void)memset(dst, value, n);
}

/**
 * @brief Fill memory with a constant byte value (4-byte aligned AEABI helper).
 * @details Writes (uint8_t)@p value to the first @p n bytes of @p dst by
 *          delegating to memset(). The length is the SECOND parameter here and
 *          the third in memset(). This entry point declares a 4-byte aligned destination.
 * @param[out] dst   Destination buffer to fill.
 * @param[in]  n     Number of bytes to write.
 * @param[in]  value Byte value to write.
 * @pre @p dst points to at least @p n writable bytes if @p n > 0.
 * @pre The destination memory region is addressable and valid.
 * @post The first @p n bytes of @p dst contain (uint8_t)@p value.
 * @post Bytes beyond @p n are unmodified.
 * @note Compiler runtime helper; takes (dst, n, value), never memset()'s (dst, value, n); never allocates; reentrant and thread-safe when destination memory is not concurrently modified.
 * @since 0.1.0
 */
void __aeabi_memset4(void* dst, size_t n, int value)
{
  (void)memset(dst, value, n);
}

/**
 * @brief Fill memory with a constant byte value (8-byte aligned AEABI helper).
 * @details Writes (uint8_t)@p value to the first @p n bytes of @p dst by
 *          delegating to memset(). The length is the SECOND parameter here and
 *          the third in memset(). This entry point declares an 8-byte aligned destination.
 * @param[out] dst   Destination buffer to fill.
 * @param[in]  n     Number of bytes to write.
 * @param[in]  value Byte value to write.
 * @pre @p dst points to at least @p n writable bytes if @p n > 0.
 * @pre The destination memory region is addressable and valid.
 * @post The first @p n bytes of @p dst contain (uint8_t)@p value.
 * @post Bytes beyond @p n are unmodified.
 * @note Compiler runtime helper; takes (dst, n, value), never memset()'s (dst, value, n); never allocates; reentrant and thread-safe when destination memory is not concurrently modified.
 * @since 0.1.0
 */
void __aeabi_memset8(void* dst, size_t n, int value)
{
  (void)memset(dst, value, n);
}

/**
 * @brief Clear memory to zero (unaligned AEABI helper).
 * @details Writes zero to the first @p n bytes of @p dst by delegating to
 *          memset(). Takes no value parameter at all, since zero is implied by
 *          the entry point. This entry point carries no alignment guarantee.
 * @param[out] dst Destination buffer to clear.
 * @param[in]  n   Number of bytes to clear.
 * @pre @p dst points to at least @p n writable bytes if @p n > 0.
 * @pre The destination memory region is addressable and valid.
 * @post The first @p n bytes of @p dst are zero.
 * @post Bytes beyond @p n are unmodified.
 * @note Compiler runtime helper; zero is implied by the entry point and never passed; never allocates; reentrant and thread-safe when destination memory is not concurrently modified.
 * @since 0.1.0
 */
void __aeabi_memclr(void* dst, size_t n)
{
  (void)memset(dst, 0, n);
}

/**
 * @brief Clear memory to zero (4-byte aligned AEABI helper).
 * @details Writes zero to the first @p n bytes of @p dst by delegating to
 *          memset(). Takes no value parameter at all, since zero is implied by
 *          the entry point. This entry point declares a 4-byte aligned destination.
 * @param[out] dst Destination buffer to clear.
 * @param[in]  n   Number of bytes to clear.
 * @pre @p dst points to at least @p n writable bytes if @p n > 0.
 * @pre The destination memory region is addressable and valid.
 * @post The first @p n bytes of @p dst are zero.
 * @post Bytes beyond @p n are unmodified.
 * @note Compiler runtime helper; zero is implied by the entry point and never passed; never allocates; reentrant and thread-safe when destination memory is not concurrently modified.
 * @since 0.1.0
 */
void __aeabi_memclr4(void* dst, size_t n)
{
  (void)memset(dst, 0, n);
}

/**
 * @brief Clear memory to zero (8-byte aligned AEABI helper).
 * @details Writes zero to the first @p n bytes of @p dst by delegating to
 *          memset(). Takes no value parameter at all, since zero is implied by
 *          the entry point. This entry point declares an 8-byte aligned destination.
 * @param[out] dst Destination buffer to clear.
 * @param[in]  n   Number of bytes to clear.
 * @pre @p dst points to at least @p n writable bytes if @p n > 0.
 * @pre The destination memory region is addressable and valid.
 * @post The first @p n bytes of @p dst are zero.
 * @post Bytes beyond @p n are unmodified.
 * @note Compiler runtime helper; zero is implied by the entry point and never passed; never allocates; reentrant and thread-safe when destination memory is not concurrently modified.
 * @since 0.1.0
 */
void __aeabi_memclr8(void* dst, size_t n)
{
  (void)memset(dst, 0, n);
}

#endif /* __arm__ || RA8_TEST_FREESTANDING         */
#endif /* !RA8_OFF_TARGET || RA8_TEST_FREESTANDING */
