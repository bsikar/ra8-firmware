/**
 * @file ra8_freestanding.h
 * @brief Project-owned freestanding C runtime ABI primitives.
 *
 * @par Tag
 * [Ring 0 / Foundation] {World: Dual}
 *
 * @details
 * Declares standard compiler-required memory, string, and integer primitives
 * implemented directly in target firmware without libc or newlib dependencies.
 * Satisfies standard ABI contracts required by GCC and clang codegen while
 * ensuring all implementations are bounded, deterministic, and heap-free.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(RA8_TEST_FREESTANDING)
#undef memset
#undef memcpy
#undef memmove
#undef memcmp
#undef memchr
#undef strlen
#undef strnlen
#undef strcmp
#undef strncmp
#undef strchr
#undef strrchr
#undef strstr
#undef strcpy
#undef strncpy
#undef abs

#define memset  ra8_memset
#define memcpy  ra8_memcpy
#define memmove ra8_memmove
#define memcmp  ra8_memcmp
#define memchr  ra8_memchr
#define strlen  ra8_strlen
#define strnlen ra8_strnlen
#define strcmp  ra8_strcmp
#define strncmp ra8_strncmp
#define strchr  ra8_strchr
#define strrchr ra8_strrchr
#define strstr  ra8_strstr
#define strcpy  ra8_strcpy
#define strncpy ra8_strncpy
#define abs     ra8_abs
#endif /* RA8_TEST_FREESTANDING */

/**
 * @brief Fill memory with a constant byte value.
 * @details Sets the first @p n bytes of the memory area pointed to by @p dst
 *          to the specified value (converted to an unsigned char).
 * @param[out] dst   Pointer to the destination memory block to fill.
 * @param[in]  value Byte value to write (passed as int, converted to uint8_t).
 * @param[in]  n     Number of bytes to set.
 * @return Original destination pointer @p dst.
 * @retval dst The destination pointer passed in @p dst is always returned.
 * @pre @p dst points to at least @p n contiguous writable bytes if @p n > 0.
 * @pre The destination memory region is addressable and valid.
 * @post The first @p n bytes of @p dst contain (uint8_t)@p value.
 * @post Bytes beyond @p n are unmodified.
 * @note Freestanding runtime primitive; never allocates; reentrant and thread-safe when destination memory is not concurrently modified.
 * @since 0.1.0
 */
void* memset(void* dst, int value, size_t n);

/**
 * @brief Copy memory area between non-overlapping regions.
 * @details Copies @p n bytes from memory area @p src to memory area @p dst.
 *          The memory areas must not overlap.
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
void* memcpy(void* restrict dst, const void* restrict src, size_t n);

/**
 * @brief Copy memory area between potentially overlapping regions.
 * @details Copies @p n bytes from @p src to @p dst with correct overlap
 *          handling by determining forward vs backward copy order.
 * @param[out] dst Destination buffer pointer.
 * @param[in]  src Source buffer pointer.
 * @param[in]  n   Number of bytes to copy.
 * @return Original destination pointer @p dst.
 * @retval dst The destination pointer passed in @p dst is always returned.
 * @pre @p dst points to at least @p n writable bytes if @p n > 0.
 * @pre @p src points to at least @p n readable bytes if @p n > 0.
 * @post The @p n bytes at @p dst match the initial @p n bytes at @p src.
 * @post Safe against any degree of overlap between @p dst and @p src.
 * @note Freestanding runtime primitive; never allocates; reentrant and thread-safe when memory regions are not concurrently modified.
 * @since 0.1.0
 */
void* memmove(void* dst, const void* src, size_t n);

/**
 * @brief Compare bytes in two memory areas.
 * @details Compares the first @p n bytes of memory areas @p a and @p b as
 *          unsigned char values.
 * @param[in] a First memory area pointer.
 * @param[in] b Second memory area pointer.
 * @param[in] n Maximum number of bytes to compare.
 * @return Difference between first differing bytes, or 0 if all equal.
 * @retval 0  All @p n bytes match or @p n is zero.
 * @retval <0 First differing byte in @p a is less than in @p b.
 * @retval >0 First differing byte in @p a is greater than in @p b.
 * @pre @p a points to at least @p n readable bytes if @p n > 0.
 * @pre @p b points to at least @p n readable bytes if @p n > 0.
 * @post Input buffers are unmodified.
 * @post Comparison is based on unsigned byte values per ISO C standard.
 * @note Freestanding runtime primitive; never allocates; reentrant and thread-safe when memory regions are not concurrently modified.
 * @since 0.1.0
 */
int memcmp(const void* a, const void* b, size_t n);

/**
 * @brief Locate a byte in a memory area.
 * @details Scans the initial @p n bytes of @p s for the first instance of @p c.
 * @param[in] s Pointer to memory buffer.
 * @param[in] c Byte value to search for (interpreted as unsigned char).
 * @param[in] n Maximum bytes to examine.
 * @return Pointer to matching byte, or nullptr if not found.
 * @retval non-null Pointer to matching byte within @p s.
 * @retval nullptr  Byte not found within first @p n bytes, or @p n is zero.
 * @pre @p s points to at least @p n readable bytes if @p n > 0.
 * @pre @p s is valid and addressable.
 * @post Memory buffer is unmodified.
 * @post Returned pointer is within [s, s + n) on success.
 * @note Freestanding runtime primitive; never allocates; reentrant and thread-safe when memory is not concurrently modified.
 * @since 0.1.0
 */
void* memchr(const void* s, int c, size_t n);

/**
 * @brief Calculate string length.
 * @details Computes the length of null-terminated string @p s.
 * @param[in] s Null-terminated character string.
 * @return Number of characters preceding the terminating null character.
 * @retval length Number of characters before null terminator.
 * @pre @p s points to a valid null-terminated string.
 * @pre String is readable up to the null terminator.
 * @post String content is unmodified.
 * @post Result is bounded by string length.
 * @note Freestanding runtime primitive; never allocates; reentrant and thread-safe when input string is not concurrently modified.
 * @since 0.1.0
 */
size_t strlen(const char* s);

/**
 * @brief Calculate bounded string length.
 * @details Computes length of string @p s, up to a maximum of @p maxlen.
 * @param[in] s      Character string to measure.
 * @param[in] maxlen Maximum characters to examine.
 * @return Length of string up to @p maxlen.
 * @retval length Min of characters before null terminator and @p maxlen.
 * @pre @p s points to readable memory of at least @p maxlen bytes or null-terminated.
 * @pre @p s is addressable.
 * @post String content is unmodified.
 * @post Return value is never greater than @p maxlen.
 * @note Freestanding runtime primitive; never allocates; reentrant and thread-safe when input string is not concurrently modified.
 * @since 0.1.0
 */
size_t strnlen(const char* s, size_t maxlen);

/**
 * @brief Compare two null-terminated strings.
 * @details Compares @p s1 and @p s2 lexicographically as unsigned char values.
 * @param[in] s1 First null-terminated string.
 * @param[in] s2 Second null-terminated string.
 * @return Difference between first differing characters, or 0 if identical.
 * @retval 0  Strings are identical up to null terminator.
 * @retval <0 First non-matching character in @p s1 is less than in @p s2.
 * @retval >0 First non-matching character in @p s1 is greater than in @p s2.
 * @pre Both @p s1 and @p s2 are valid null-terminated strings.
 * @pre String buffers are readable.
 * @post Both input strings are unmodified.
 * @post Comparison semantics follow unsigned byte values.
 * @note Freestanding runtime primitive; never allocates; reentrant and thread-safe when input strings are not concurrently modified.
 * @since 0.1.0
 */
int strcmp(const char* s1, const char* s2);

/**
 * @brief Compare two strings up to a specified length.
 * @details Compares at most @p n characters of @p s1 and @p s2.
 * @param[in] s1 First string.
 * @param[in] s2 Second string.
 * @param[in] n  Maximum characters to compare.
 * @return Difference between first differing characters, or 0 if identical.
 * @retval 0  Strings match for first @p n characters or up to null terminator.
 * @retval <0 First non-matching character in @p s1 is less than in @p s2.
 * @retval >0 First non-matching character in @p s1 is greater than in @p s2.
 * @pre @p s1 and @p s2 are readable up to @p n bytes or null-terminated.
 * @pre Both pointers are addressable.
 * @post Both input strings are unmodified.
 * @post Bounded by @p n characters.
 * @note Freestanding runtime primitive; never allocates; reentrant and thread-safe when input strings are not concurrently modified.
 * @since 0.1.0
 */
int strncmp(const char* s1, const char* s2, size_t n);

/**
 * @brief Locate first occurrence of character in string.
 * @details Scans @p s for the first instance of character @p c (including null).
 * @param[in] s Null-terminated string to search.
 * @param[in] c Character value to locate (converted to char).
 * @return Pointer to first occurrence in @p s, or nullptr if not found.
 * @retval non-null Pointer to first matching character in @p s.
 * @retval nullptr  Character not found in @p s.
 * @pre @p s points to a valid null-terminated string.
 * @pre @p s is readable.
 * @post String content is unmodified.
 * @post Returned pointer (if non-null) is within @p s.
 * @note Freestanding runtime primitive; never allocates; reentrant and thread-safe when input string is not concurrently modified.
 * @since 0.1.0
 */
char* strchr(const char* s, int c);

/**
 * @brief Locate last occurrence of character in string.
 * @details Scans @p s for the last instance of character @p c (including null).
 * @param[in] s Null-terminated string to search.
 * @param[in] c Character value to locate (converted to char).
 * @return Pointer to last occurrence in @p s, or nullptr if not found.
 * @retval non-null Pointer to last matching character in @p s.
 * @retval nullptr  Character not found in @p s.
 * @pre @p s points to a valid null-terminated string.
 * @pre @p s is readable.
 * @post String content is unmodified.
 * @post Returned pointer (if non-null) is within @p s.
 * @note Freestanding runtime primitive; never allocates; reentrant and thread-safe when input string is not concurrently modified.
 * @since 0.1.0
 */
char* strrchr(const char* s, int c);

/**
 * @brief Locate substring in string.
 * @details Finds the first occurrence of @p needle in @p haystack.
 * @param[in] haystack Null-terminated string to search.
 * @param[in] needle   Null-terminated substring to locate.
 * @return Pointer to beginning of substring in @p haystack, or nullptr if not found.
 * @retval non-null Pointer to substring occurrence in @p haystack.
 * @retval nullptr  Substring not found in @p haystack.
 * @pre Both @p haystack and @p needle are valid null-terminated strings.
 * @pre Both buffers are readable.
 * @post Both input strings are unmodified.
 * @post Returns @p haystack if @p needle is empty.
 * @note Freestanding runtime primitive; never allocates; reentrant and thread-safe when input strings are not concurrently modified.
 * @since 0.1.0
 */
char* strstr(const char* haystack, const char* needle);

/**
 * @brief Copy string to destination buffer.
 * @details Copies string @p src (including terminating null) to @p dst.
 * @param[out] dst Destination buffer pointer.
 * @param[in]  src Source null-terminated string pointer.
 * @return Original destination pointer @p dst.
 * @retval dst The destination pointer passed in @p dst is always returned.
 * @pre @p dst has sufficient capacity to hold @p src and terminating null.
 * @pre @p dst and @p src do not overlap.
 * @post The copied string at @p dst matches @p src.
 * @post Source string is unmodified.
 * @note Freestanding runtime primitive; never allocates; reentrant and thread-safe when strings are not concurrently modified.
 * @since 0.1.0
 */
char* strcpy(char* restrict dst, const char* restrict src);

/**
 * @brief Copy bounded string to destination buffer.
 * @details Copies at most @p n characters from @p src to @p dst, padding with
 *          null bytes if @p src is shorter than @p n.
 * @param[out] dst Destination buffer pointer.
 * @param[in]  src Source string pointer.
 * @param[in]  n   Maximum number of bytes to write.
 * @return Original destination pointer @p dst.
 * @retval dst The destination pointer passed in @p dst is always returned.
 * @pre @p dst points to at least @p n writable bytes.
 * @pre @p dst and @p src do not overlap.
 * @post Exactly @p n bytes are written to @p dst.
 * @post Source string is unmodified.
 * @note Freestanding runtime primitive; never allocates; reentrant and thread-safe when strings are not concurrently modified.
 * @since 0.1.0
 */
char* strncpy(char* restrict dst, const char* restrict src, size_t n);

/**
 * @brief Compute absolute value of integer.
 * @details Returns the absolute value of @p j.
 * @param[in] j Signed integer value.
 * @return Absolute value of @p j.
 * @retval value Non-negative absolute value.
 * @pre @p j is not INT_MIN (which cannot be represented as positive int).
 * @pre Standard integer evaluation context.
 * @post Returned value is non-negative.
 * @post Input @p j is unmodified.
 * @note Pure arithmetic function; never allocates; reentrant and thread-safe.
 * @since 0.1.0
 */
int abs(int j);

#ifdef __cplusplus
}
#endif
