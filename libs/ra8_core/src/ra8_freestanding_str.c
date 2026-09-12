/**
 * @file ra8_freestanding_str.c
 * @brief Project-owned freestanding string ABI primitives (strlen, strcmp, strchr, strstr, etc.).
 *
 * @par Tag
 * [Ring 0 / Foundation] {World: Dual}
 *
 * @details
 * Standard ISO C string primitives implemented directly for the bare-metal
 * RA8 target firmware without libc or newlib dependencies.
 * Deterministic, bounds-conscious, and heap-free.
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
 * @brief Calculate string length.
 * @details Computes number of characters before terminating null byte.
 * @param[in] s Null-terminated character string.
 * @return String length in characters.
 * @retval length Number of characters before null terminator.
 * @pre @p s points to a valid null-terminated string.
 * @pre @p s is readable.
 * @post String content is unmodified.
 * @post Never negative.
 * @note Freestanding runtime primitive; never allocates; reentrant and thread-safe when input string is not concurrently modified.
 * @since 0.1.0
 */
size_t strlen(const char* s)
{
  size_t len = 0U;
  while (s[len] != '\0') {
    ++len;
  }
  return len;
}

/**
 * @brief Calculate bounded string length.
 * @details Computes length of string @p s up to a maximum of @p maxlen.
 * @param[in] s      Character string to measure.
 * @param[in] maxlen Maximum characters to examine.
 * @return Length of string up to @p maxlen.
 * @retval length Min of characters before null terminator and @p maxlen.
 * @pre @p s points to readable memory of at least @p maxlen bytes or null-terminated.
 * @pre @p s is addressable.
 * @post String content is unmodified.
 * @post Return value is at most @p maxlen.
 * @note Freestanding runtime primitive; never allocates; reentrant and thread-safe when input string is not concurrently modified.
 * @since 0.1.0
 */
size_t strnlen(const char* s, size_t maxlen)
{
  size_t len = 0U;
  while (len < maxlen) {
    if (s[len] == '\0') {
      break;
    }
    ++len;
  }
  return len;
}

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
int strcmp(const char* s1, const char* s2)
{
  const uint8_t* p1 = (const uint8_t*)s1;
  const uint8_t* p2 = (const uint8_t*)s2;
  while (*p1 != 0U) {
    if (*p1 != *p2) {
      break;
    }
    ++p1;
    ++p2;
  }
  return (int)*p1 - (int)*p2;
}

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
int strncmp(const char* s1, const char* s2, size_t n)
{
  if (n == 0U) {
    return 0;
  }
  const uint8_t* p1 = (const uint8_t*)s1;
  const uint8_t* p2 = (const uint8_t*)s2;
  for (size_t i = 0U; i < n; ++i) {
    if (p1[i] != p2[i]) {
      return (int)p1[i] - (int)p2[i];
    }
    if (p1[i] == 0U) {
      return 0;
    }
  }
  return 0;
}

/**
 * @brief Locate first occurrence of character in string.
 * @details Scans @p s for the first instance of character @p c (including null).
 * @param[in] s Null-terminated string to search.
 * @param[in] c Character value to locate.
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
char* strchr(const char* s, int c)
{
  const char target = (char)c;
  while (*s != '\0') {
    if (*s == target) {
      return (char*)(uintptr_t)s;
    }
    ++s;
  }
  return (target == '\0') ? (char*)(uintptr_t)s : nullptr;
}

/**
 * @brief Locate last occurrence of character in string.
 * @details Scans @p s for the last instance of character @p c (including null).
 * @param[in] s Null-terminated string to search.
 * @param[in] c Character value to locate.
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
char* strrchr(const char* s, int c)
{
  const char  target = (char)c;
  const char* last   = nullptr;
  while (*s != '\0') {
    if (*s == target) {
      last = s;
    }
    ++s;
  }
  return (target == '\0') ? (char*)(uintptr_t)s : (char*)(uintptr_t)last;
}

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
char* strstr(const char* haystack, const char* needle)
{
  if (needle[0] == '\0') {
    return (char*)(uintptr_t)haystack;
  }
  for (size_t i = 0U; haystack[i] != '\0'; ++i) {
    size_t j = 0U;
    while (needle[j] != '\0') {
      if (haystack[i + j] != needle[j]) {
        break;
      }
      ++j;
    }
    if (needle[j] == '\0') {
      return (char*)(uintptr_t)&haystack[i];
    }
  }
  return nullptr;
}

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
char* strcpy(char* restrict dst, const char* restrict src)
{
  size_t i = 0U;
  while (src[i] != '\0') {
    dst[i] = src[i];
    ++i;
  }
  dst[i] = '\0';
  return dst;
}

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
char* strncpy(char* restrict dst, const char* restrict src, size_t n)
{
  size_t i = 0U;
  while (i < n) {
    if (src[i] == '\0') {
      break;
    }
    dst[i] = src[i];
    ++i;
  }
  while (i < n) {
    dst[i] = '\0';
    ++i;
  }
  return dst;
}
#endif /* !RA8_OFF_TARGET || RA8_TEST_FREESTANDING */
