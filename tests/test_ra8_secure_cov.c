/**
 * @file test_ra8_secure_cov.c
 * @brief Coverage + behaviour tests for the constant-time compare (ra8_secure.c).
 *
 * @par Tag
 * [Ring 1 / Core] {World: S}
 *
 * @details
 * ::ra8_ct_equal is the constant-time equality used on every MAC / tag / digest /
 * key verdict (e.g. the OTA image-digest check). These cases exercise both
 * outcomes at each difference position (first, middle, last byte -- to confirm
 * the accumulator scans the whole buffer with no early-out), the zero-length
 * vacuous-equal case, and both NULL-pointer guards.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>

#include "ra8_secure.h"
#include "unity_minimal.h"

/**
 * @enum t_cmp_t
 * @brief Payload bytes and the mismatch marker for the constant-time compare.
 *
 * @details
 * The reference buffer is the ascending run 1..8; each arm copies it and flips
 * one byte to 0xFF at a different position -- first, middle, last -- so the
 * comparator cannot pass by short-circuiting at any one end.
 */
typedef enum : uint8_t {
  k_t_ref_b4       = 5U,    /**< Byte 4 of the ascending reference buffer. */
  k_t_ref_b6       = 7U,    /**< Byte 6.                                   */
  k_t_mismatch_val = 0xFFU, /**< Value written to create the single-byte
                                 difference under test.                        */
} t_cmp_t;

/** @brief Fixed comparison-buffer length used across the cases. */
typedef enum : uint8_t {
  k_ct_buf_len = 8U, /**< bytes in each test buffer. */
} ct_test_len_t;

/**
 * @test test_ct_equal_matches
 * @details Equal buffers compare equal; a zero-length compare is vacuously
 * equal (the loop body never runs).
 *
 * @par MC/DC:
 * (no compound decisions in ra8_ct_equal -- two independent single-condition
 * NULL guards and a diff accumulator; no `&&` or `||`.)
 */
static void test_ct_equal_matches(void)
{
  TEST_BEGIN("ra8_ct_equal: equal buffers + zero length");
  const uint8_t a[k_ct_buf_len] = {1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U};
  const uint8_t b[k_ct_buf_len] = {1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U};
  TEST_ASSERT(ra8_ct_equal(a, b, (size_t)k_ct_buf_len));
  TEST_ASSERT(ra8_ct_equal(a, b, 0U)); /* vacuously equal. */
  TEST_END("ra8_ct_equal: equal buffers + zero length");
}

/**
 * @test test_ct_equal_differs
 * @details A difference at the first, a middle, or the last byte all report
 * inequality -- confirming the accumulator does not early-out.
 *
 * @par MC/DC:
 * (single-condition diff test; the three positions exercise the accumulator
 * across the whole span.)
 */
static void test_ct_equal_differs(void)
{
  TEST_BEGIN("ra8_ct_equal: difference at first/middle/last byte");
  const uint8_t a[k_ct_buf_len] = {1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U};

  uint8_t first[k_ct_buf_len] = {1U, 2U, 3U, 4U, k_t_ref_b4, 6U, k_t_ref_b6, 8U};
  first[0]                    = k_t_mismatch_val;
  TEST_ASSERT(!ra8_ct_equal(a, first, (size_t)k_ct_buf_len));

  uint8_t middle[k_ct_buf_len] = {1U, 2U, 3U, 4U, k_t_ref_b4, 6U, k_t_ref_b6, 8U};
  middle[4]                    = k_t_mismatch_val;
  TEST_ASSERT(!ra8_ct_equal(a, middle, (size_t)k_ct_buf_len));

  uint8_t last[k_ct_buf_len] = {1U, 2U, 3U, 4U, k_t_ref_b4, 6U, k_t_ref_b6, 8U};
  last[k_ct_buf_len - 1U]    = k_t_mismatch_val;
  TEST_ASSERT(!ra8_ct_equal(a, last, (size_t)k_ct_buf_len));
  TEST_END("ra8_ct_equal: difference at first/middle/last byte");
}

/**
 * @test test_ct_equal_null
 * @details Either NULL pointer yields false (both single-condition guards).
 *
 * @par MC/DC:
 * (two independent NULL guards, each exercised on its own vector.)
 */
static void test_ct_equal_null(void)
{
  TEST_BEGIN("ra8_ct_equal: NULL pointer guards");
  const uint8_t buf[k_ct_buf_len] = {};
  TEST_ASSERT(!ra8_ct_equal(nullptr, buf, (size_t)k_ct_buf_len));
  TEST_ASSERT(!ra8_ct_equal(buf, nullptr, (size_t)k_ct_buf_len));
  TEST_END("ra8_ct_equal: NULL pointer guards");
}

/**
 * @test test_memzero_clears_and_guards
 * @details ra8_secure_memzero zeros exactly ``len`` bytes; a NULL pointer and a
 * zero length are both no-ops; bytes past ``len`` are left intact.
 *
 * @par MC/DC:
 * (no compound decisions in ra8_secure_memzero -- two independent single-condition
 * guards (NULL, zero-length) and a bounded write loop; asserts are split to keep
 * the test itself free of `&&`.)
 */
static void test_memzero_clears_and_guards(void)
{
  TEST_BEGIN("ra8_secure_memzero: clears buffer + NULL/zero-length guards");

  uint8_t       buf[k_ct_buf_len]  = {1U, 2U, 3U, 4U, k_t_ref_b4, 6U, k_t_ref_b6, 8U};
  const uint8_t zero[k_ct_buf_len] = {};
  ra8_secure_memzero(buf, (size_t)k_ct_buf_len);
  TEST_ASSERT(ra8_ct_equal(buf, zero, (size_t)k_ct_buf_len));

  /* Zero only a sub-range: the tail must be untouched. */
  uint8_t partial[k_ct_buf_len] = {1U, 2U, 3U, 4U, k_t_ref_b4, 6U, k_t_ref_b6, 8U};
  ra8_secure_memzero(partial, 4U);
  TEST_ASSERT(partial[0] == 0U);
  TEST_ASSERT(partial[3] == 0U);
  TEST_ASSERT(partial[4] == 5U);
  TEST_ASSERT(partial[k_ct_buf_len - 1U] == 8U);

  /* NULL pointer and zero length are both no-ops (no crash, no change). */
  ra8_secure_memzero(nullptr, (size_t)k_ct_buf_len);
  const uint8_t orig[k_ct_buf_len]      = {1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U};
  uint8_t       unchanged[k_ct_buf_len] = {1U, 2U, 3U, 4U, k_t_ref_b4, 6U, k_t_ref_b6, 8U};
  ra8_secure_memzero(unchanged, 0U);
  TEST_ASSERT(ra8_ct_equal(unchanged, orig, (size_t)k_ct_buf_len));

  TEST_END("ra8_secure_memzero: clears buffer + NULL/zero-length guards");
}

int32_t main(void)
{
  test_ct_equal_matches();
  test_ct_equal_differs();
  test_ct_equal_null();
  test_memzero_clears_and_guards();
  return 0;
}
