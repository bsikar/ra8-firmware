/**
 * @file test_ra8_freestanding.c
 * @brief Unit tests for project-owned freestanding C runtime primitives.
 *
 * @details
 * Validates the core memory, string, and integer math primitives implemented
 * in ra8_freestanding_mem.c, ra8_freestanding_str.c, and ra8_freestanding_math.c.
 * Exercises zero-length requests, boundary conditions, unaligned operations,
 * overlap directions, unsigned character comparisons, and string edge cases.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#ifndef RA8_TEST_FREESTANDING
#define RA8_TEST_FREESTANDING
#endif

#include <stddef.h>
#include <stdint.h>
/* Order-load-bearing: <string.h> must be processed before the
 * RA8_TEST_FREESTANDING redirects in ra8_freestanding.h rename the standard
 * primitives to ra8_* test symbols. A fortified <string.h> seen afterwards
 * would define its intercepting inlines under the renamed symbols and
 * capture the calls under test; seen first, its inlines keep their standard
 * names and every call under test resolves to the project implementation. */
#include <string.h> // ra8-keep-include: `memset` order guard, see above

#include "ra8_freestanding.h"
#include "unity_minimal.h"

/**
 * @enum t_freestanding_limits_t
 * @brief Buffer sizing constants for freestanding runtime unit tests.
 */
typedef enum : size_t {
  k_fs_canary_head = 8U,    /**< Leading canary guard byte count.  */
  k_fs_canary_tail = 8U,    /**< Trailing canary guard byte count. */
  k_fs_buffer_size = 256U,  /**< Work buffer capacity.             */
  k_fs_canary_val  = 0xCCU, /**< Canary fill pattern byte.         */
} t_freestanding_limits_t;

/**
 * @struct guarded_buf_t
 * @brief Buffer bracketed by canary zones to verify strict memory bounds.
 */
typedef struct {
  uint8_t pre[k_fs_canary_head];
  uint8_t data[k_fs_buffer_size];
  uint8_t post[k_fs_canary_tail];
} guarded_buf_t;

static guarded_buf_t s_gb1;
static guarded_buf_t s_gb2;

/**
 * @brief Initialize guarded buffers with canaries.
 * @details Fills leading and trailing canaries and resets data region.
 * @param[out] gb Buffer structure to initialize.
 * @pre gb is in addressable writable memory.
 * @pre Structure size matches geometry.
 * @post Leading and trailing canaries are set to k_fs_canary_val.
 * @post Data region is cleared to zero.
 * @note Used to verify that primitives never write out of bounds.
 * @since 0.1.0
 */
static void internal_buf_init(guarded_buf_t* gb)
{
  for (size_t i = 0U; i < k_fs_canary_head; ++i) {
    gb->pre[i] = (uint8_t)k_fs_canary_val;
  }
  for (size_t i = 0U; i < k_fs_buffer_size; ++i) {
    gb->data[i] = 0U;
  }
  for (size_t i = 0U; i < k_fs_canary_tail; ++i) {
    gb->post[i] = (uint8_t)k_fs_canary_val;
  }
}

/**
 * @brief Assert that buffer canaries are completely intact.
 * @details Validates that no out-of-bounds write corrupts pre/post guards.
 * @param[in] gb Buffer structure to check.
 * @pre gb was initialized by internal_buf_init.
 * @pre gb is readable.
 * @post Verified canaries retain k_fs_canary_val.
 * @post Fails test if any canary byte was modified.
 * @note Ensures bounds safety of memory primitives.
 * @since 0.1.0
 */
static void internal_assert_canaries(const guarded_buf_t* gb)
{
  for (size_t i = 0U; i < k_fs_canary_head; ++i) {
    TEST_ASSERT_EQ((size_t)k_fs_canary_val, (size_t)gb->pre[i]);
  }
  for (size_t i = 0U; i < k_fs_canary_tail; ++i) {
    TEST_ASSERT_EQ((size_t)k_fs_canary_val, (size_t)gb->post[i]);
  }
}

/**
 * @brief Test memset zero-length, odd sizes, and unaligned fills.
 * @details Validates memset behavior across sizes and alignment offsets.
 * @pre s_gb1 is addressable.
 * @pre Canaries are intact before test.
 * @post Return value matches dst pointer.
 * @post Canaries remain intact after fill.
 * @note Verifies core memset contract.
 * @since 0.1.0
 * @par MC/DC:
 * (no compound decisions under test -- loop bounds and byte writes)
 */
static void internal_test_memset_basic(void)
{
  TEST_BEGIN("memset basic and unaligned");
  internal_buf_init(&s_gb1);

  /* Zero length */
  void* ret = memset(&s_gb1.data[4], 0x55, 0U);
  TEST_ASSERT(ret == &s_gb1.data[4]);
  TEST_ASSERT_EQ(0U, s_gb1.data[4]);
  internal_assert_canaries(&s_gb1);

  /* Odd size unaligned fill */
  ret = memset(&s_gb1.data[3], 0xAA, 17U);
  TEST_ASSERT(ret == &s_gb1.data[3]);
  TEST_ASSERT_EQ(0U, s_gb1.data[2]);
  for (size_t i = 0U; i < 17U; ++i) {
    TEST_ASSERT_EQ(0xAAU, s_gb1.data[3U + i]);
  }
  TEST_ASSERT_EQ(0U, s_gb1.data[20]);
  internal_assert_canaries(&s_gb1);
  TEST_END("memset basic and unaligned");
}

/**
 * @brief Test memset across all byte values.
 * @details Proves that full 8-bit range (0x00 to 0xFF) is correctly written.
 * @pre s_gb1 is addressable.
 * @pre Canaries are intact.
 * @post Each byte value is written faithfully without truncation errors.
 * @post Canaries remain intact.
 * @note Exercises full uint8_t domain.
 * @since 0.1.0
 * @par MC/DC:
 * (no compound decisions under test -- value range coverage)
 */
static void internal_test_memset_patterns(void)
{
  TEST_BEGIN("memset patterns");
  internal_buf_init(&s_gb1);
  const uint8_t patterns[]   = {0x00U, 0x01U, 0x7FU, 0x80U, 0x55U, 0xAAU, 0xFEU, 0xFFU};
  const size_t  num_patterns = sizeof(patterns) / sizeof(patterns[0]);

  for (size_t p = 0U; p < num_patterns; ++p) {
    (void)memset(s_gb1.data, (int)patterns[p], 64U);
    for (size_t i = 0U; i < 64U; ++i) {
      TEST_ASSERT_EQ((size_t)patterns[p], (size_t)s_gb1.data[i]);
    }
  }
  internal_assert_canaries(&s_gb1);
  TEST_END("memset patterns");
}

/**
 * @brief Test memcpy unaligned and odd sizes.
 * @details Validates memcpy with different alignment combinations.
 * @pre Both s_gb1 and s_gb2 are initialized.
 * @pre Source buffer contains deterministic pattern.
 * @post Destination receives exact copied bytes.
 * @post Both sets of canaries remain intact.
 * @note Verifies non-overlapping copy contract.
 * @since 0.1.0
 * @par MC/DC:
 * (no compound decisions under test -- linear copy loop)
 */
static void internal_test_memcpy(void)
{
  TEST_BEGIN("memcpy");
  internal_buf_init(&s_gb1);
  internal_buf_init(&s_gb2);

  for (size_t i = 0U; i < k_fs_buffer_size; ++i) {
    s_gb2.data[i] = (uint8_t)(i & 0xFFU);
  }

  /* Zero length */
  void* ret = memcpy(&s_gb1.data[1], &s_gb2.data[1], 0U);
  TEST_ASSERT(ret == &s_gb1.data[1]);

  /* Unaligned copy (src offset 3, dst offset 5, length 29) */
  ret = memcpy(&s_gb1.data[5], &s_gb2.data[3], 29U);
  TEST_ASSERT(ret == &s_gb1.data[5]);
  for (size_t i = 0U; i < 29U; ++i) {
    TEST_ASSERT_EQ((size_t)s_gb2.data[3U + i], (size_t)s_gb1.data[5U + i]);
  }
  internal_assert_canaries(&s_gb1);
  internal_assert_canaries(&s_gb2);
  TEST_END("memcpy");
}

/**
 * @brief Test memmove forward and backward overlapping copies.
 * @details Validates identical src/dst, forward overlap, and backward overlap.
 * @pre s_gb1 contains sequential byte pattern.
 * @pre Canaries are intact.
 * @post Overlapping moves preserve all source bytes correctly.
 * @post Canaries remain intact.
 * @note Verifies overlap detection logic.
 * @since 0.1.0
 * @par MC/DC:
 * Decision 1: ((dst == src) || (n == 0U))
 * Decision 2: ((uintptr_t)d < (uintptr_t)s)
 */
static void internal_test_memmove_overlap(void)
{
  TEST_BEGIN("memmove overlap");
  internal_buf_init(&s_gb1);

  for (size_t i = 0U; i < 64U; ++i) {
    s_gb1.data[i] = (uint8_t)(i + 1U);
  }

  /* Forward overlap: dst < src (dst = data, src = data + 4, len = 16) */
  (void)memmove(&s_gb1.data[0], &s_gb1.data[4], 16U);
  for (size_t i = 0U; i < 16U; ++i) {
    TEST_ASSERT_EQ((size_t)(i + 5U), (size_t)s_gb1.data[i]);
  }

  /* Reset pattern */
  for (size_t i = 0U; i < 64U; ++i) {
    s_gb1.data[i] = (uint8_t)(i + 1U);
  }

  /* Backward overlap: dst > src (dst = data + 4, src = data, len = 16) */
  (void)memmove(&s_gb1.data[4], &s_gb1.data[0], 16U);
  for (size_t i = 0U; i < 16U; ++i) {
    TEST_ASSERT_EQ((size_t)(i + 1U), (size_t)s_gb1.data[4U + i]);
  }

  /* Identical pointers */
  void* ret = memmove(&s_gb1.data[10], &s_gb1.data[10], 20U);
  TEST_ASSERT(ret == &s_gb1.data[10]);
  internal_assert_canaries(&s_gb1);
  TEST_END("memmove overlap");
}

/**
 * @brief Test memcmp equality, order, and unsigned comparison semantics.
 * @details Validates that 0x80 compares greater than 0x01 per ISO C unsigned rules.
 * @pre s_gb1 and s_gb2 are initialized.
 * @pre Buffer memory is addressable.
 * @post Unsigned byte semantics are confirmed.
 * @post Difference returns correct sign.
 * @note Vital for ISO C conformance; signed char comparison would be a bug.
 * @since 0.1.0
 * @par MC/DC:
 * Decision: if (p1[i] != p2[i])
 */
static void internal_test_memcmp_semantics(void)
{
  TEST_BEGIN("memcmp semantics");
  internal_buf_init(&s_gb1);
  internal_buf_init(&s_gb2);

  TEST_ASSERT_EQ(0, memcmp(s_gb1.data, s_gb2.data, 0U));
  TEST_ASSERT_EQ(0, memcmp(s_gb1.data, s_gb2.data, 32U));

  /* Difference at first byte */
  s_gb1.data[0] = 0x05U;
  s_gb2.data[0] = 0x02U;
  TEST_ASSERT(memcmp(s_gb1.data, s_gb2.data, 10U) > 0);
  TEST_ASSERT(memcmp(s_gb2.data, s_gb1.data, 10U) < 0);

  /* CRITICAL: Unsigned char comparison semantics. 0x80 (128) vs 0x01 (1) */
  s_gb1.data[0] = 0x80U;
  s_gb2.data[0] = 0x01U;
  TEST_ASSERT(memcmp(s_gb1.data, s_gb2.data, 1U) > 0);
  TEST_ASSERT(memcmp(s_gb2.data, s_gb1.data, 1U) < 0);

  /* Equal prefix, difference past n */
  s_gb1.data[0] = 0x10U;
  s_gb2.data[0] = 0x10U;
  s_gb1.data[5] = 0x20U;
  s_gb2.data[5] = 0x99U;
  TEST_ASSERT_EQ(0, memcmp(s_gb1.data, s_gb2.data, 5U));
  TEST_END("memcmp semantics");
}

/**
 * @brief Test memchr byte location.
 * @details Exercises found, not found, zero-length, and unsigned value searches.
 * @pre s_gb1 is initialized with known byte values.
 * @pre Search target is present at known index.
 * @post Returned pointer matches exact byte address on match.
 * @post Returns nullptr when byte is absent.
 * @note Verifies memchr boundary and search logic.
 * @since 0.1.0
 * @par MC/DC:
 * Decision: if (p[i] == target)
 */
static void internal_test_memchr(void)
{
  TEST_BEGIN("memchr");
  internal_buf_init(&s_gb1);
  s_gb1.data[10] = 0x42U;
  s_gb1.data[20] = 0x80U;

  TEST_ASSERT_NULL(memchr(s_gb1.data, 0x42, 0U));
  TEST_ASSERT(memchr(s_gb1.data, 0x42, 15U) == &s_gb1.data[10]);
  TEST_ASSERT_NULL(memchr(s_gb1.data, 0x42, 10U));
  TEST_ASSERT(memchr(s_gb1.data, 0x80, 25U) == &s_gb1.data[20]);
  TEST_ASSERT_NULL(memchr(s_gb1.data, 0x99, 30U));
  TEST_END("memchr");
}

/**
 * @brief Test strlen and strnlen edge cases.
 * @details Verifies empty strings, maxlen bounds, and standard strings.
 * @pre Strings are null-terminated.
 * @pre Buffers are readable.
 * @post Returned lengths match expected character counts.
 * @post strnlen never exceeds maxlen.
 * @note Verifies string length measurement primitives.
 * @since 0.1.0
 * @par MC/DC:
 * Decision: while ((len < maxlen) && (s[len] != '\0'))
 */
static void internal_test_strlen_strnlen(void)
{
  TEST_BEGIN("strlen and strnlen");
  TEST_ASSERT_EQ(0U, strlen(""));
  TEST_ASSERT_EQ(5U, strlen("hello"));
  TEST_ASSERT_EQ(1U, strlen("A"));

  TEST_ASSERT_EQ(0U, strnlen("", 10U));
  TEST_ASSERT_EQ(0U, strnlen("hello", 0U));
  TEST_ASSERT_EQ(3U, strnlen("hello", 3U));
  TEST_ASSERT_EQ(5U, strnlen("hello", 5U));
  TEST_ASSERT_EQ(5U, strnlen("hello", 10U));
  TEST_END("strlen and strnlen");
}

/**
 * @brief Test strcmp and strncmp lexicographical ordering.
 * @details Validates equality, prefix matching, and unsigned byte comparison.
 * @pre Test strings are valid null-terminated strings.
 * @pre String memory is readable.
 * @post Comparison returns 0 for equal strings and correct sign for differences.
 * @post strncmp respects bound n.
 * @note Conforms to ISO C lexicographical comparison.
 * @since 0.1.0
 * @par MC/DC:
 * Decision: while ((*p1 != 0U) && (*p1 == *p2))
 */
static void internal_test_strcmp_strncmp(void)
{
  TEST_BEGIN("strcmp and strncmp");
  TEST_ASSERT_EQ(0, strcmp("", ""));
  TEST_ASSERT_EQ(0, strcmp("abc", "abc"));
  TEST_ASSERT(strcmp("abc", "abd") < 0);
  TEST_ASSERT(strcmp("abd", "abc") > 0);
  TEST_ASSERT(strcmp("a", "") > 0);
  TEST_ASSERT(strcmp("", "a") < 0);

  /* strncmp bounds */
  TEST_ASSERT_EQ(0, strncmp("abcX", "abcY", 3U));
  TEST_ASSERT(strncmp("abcX", "abcY", 4U) < 0);
  TEST_ASSERT_EQ(0, strncmp("anything", "different", 0U));
  TEST_ASSERT_EQ(0, strncmp("short", "short", 10U));
  TEST_END("strcmp and strncmp");
}

/**
 * @brief Test strchr and strrchr searching.
 * @details Validates locating first occurrence, last occurrence, and null byte.
 * @pre Target string is valid.
 * @pre Character occurrences exist at known indices.
 * @post strchr returns first match, strrchr returns last match.
 * @post Searching for null character returns pointer to null terminator.
 * @note Covers character scanning primitives.
 * @since 0.1.0
 * @par MC/DC:
 * (no compound decisions under test -- linear character search)
 */
static void internal_test_strchr_strrchr(void)
{
  TEST_BEGIN("strchr and strrchr");
  const char* str = "ab.cd.ef";

  TEST_ASSERT(strchr(str, '.') == &str[2]);
  TEST_ASSERT(strrchr(str, '.') == &str[5]);

  /* Search for null terminator */
  TEST_ASSERT(strchr(str, '\0') == &str[8]);
  TEST_ASSERT(strrchr(str, '\0') == &str[8]);

  /* Not found */
  TEST_ASSERT_NULL(strchr(str, 'z'));
  TEST_ASSERT_NULL(strrchr(str, 'z'));
  TEST_END("strchr and strrchr");
}

/**
 * @brief Test strstr substring search.
 * @details Validates empty needle, prefix, middle, suffix, and absent needles.
 * @pre Haystack and needle strings are valid null-terminated strings.
 * @pre String memory is addressable.
 * @post Returns pointer to first occurrence on match.
 * @post Returns nullptr when substring is not found.
 * @note Verifies substring location primitive.
 * @since 0.1.0
 * @par MC/DC:
 * Decision: while ((haystack[i+j] != '\0') && (needle[j] != '\0') && (haystack[i+j] == needle[j]))
 */
static void internal_test_strstr(void)
{
  TEST_BEGIN("strstr");
  const char* h = "abracadabra";

  /* Empty needle */
  TEST_ASSERT(strstr(h, "") == h);

  /* Match at start */
  TEST_ASSERT(strstr(h, "abra") == h);

  /* Match in middle */
  TEST_ASSERT(strstr(h, "cada") == &h[4]);

  /* Match at end */
  TEST_ASSERT(strstr(h, "dabra") == &h[6]);

  /* Not found */
  TEST_ASSERT_NULL(strstr(h, "xyz"));
  TEST_ASSERT_NULL(strstr("short", "longer_than_haystack"));
  TEST_END("strstr");
}

/**
 * @brief Test strcpy and strncpy buffer copies.
 * @details Validates full string copy and strncpy null-padding behavior.
 * @pre Destination buffer has sufficient space.
 * @pre Source string is valid null-terminated string.
 * @post String is copied correctly including null terminator.
 * @post strncpy pads remaining space with null bytes.
 * @note Verifies string copy primitives.
 * @since 0.1.0
 * @par MC/DC:
 * (no compound decisions under test -- sequential character copy)
 */
static void internal_test_strcpy_strncpy(void)
{
  TEST_BEGIN("strcpy and strncpy");
  char dst[32];
  (void)memset(dst, 0x55, sizeof(dst));

  char* ret = strcpy(dst, "hello");
  TEST_ASSERT(ret == dst);
  TEST_ASSERT_EQ(0, strcmp(dst, "hello"));

  (void)memset(dst, 0x55, sizeof(dst));
  ret = strncpy(dst, "world", 8U);
  TEST_ASSERT(ret == dst);
  TEST_ASSERT_EQ(0, strcmp(dst, "world"));
  TEST_ASSERT_EQ('\0', dst[5]);
  TEST_ASSERT_EQ('\0', dst[6]);
  TEST_ASSERT_EQ('\0', dst[7]);
  TEST_ASSERT_EQ((char)0x55, dst[8]);
  TEST_END("strcpy and strncpy");
}

/**
 * @brief Test abs magnitude calculation.
 * @details Validates positive, negative, and zero values for int.
 * @pre Inputs are within valid non-overflowing range.
 * @pre Arithmetic environment is standard two's complement.
 * @post Returns positive magnitude of input value.
 * @post Zero maps to zero.
 * @note Integer absolute value primitives.
 * @since 0.1.0
 * @par MC/DC:
 * (no compound decisions under test -- sign conditional)
 */
static void internal_test_abs(void)
{
  TEST_BEGIN("abs");
  TEST_ASSERT_EQ(0, abs(0));
  TEST_ASSERT_EQ(42, abs(42));
  TEST_ASSERT_EQ(42, abs(-42));
  TEST_END("abs");
}

int main(void)
{
  internal_test_memset_basic();
  internal_test_memset_patterns();
  internal_test_memcpy();
  internal_test_memmove_overlap();
  internal_test_memcmp_semantics();
  internal_test_memchr();
  internal_test_strlen_strnlen();
  internal_test_strcmp_strncmp();
  internal_test_strchr_strrchr();
  internal_test_strstr();
  internal_test_strcpy_strncpy();
  internal_test_abs();
  return 0;
}
