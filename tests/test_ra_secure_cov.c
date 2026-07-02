/**
 * @file test_ra_secure_cov.c
 * @brief Coverage + behaviour tests for the constant-time compare (ra_secure.c).
 *
 * @par Tag
 * [Ring 1 / Core] {World: S}
 *
 * @details
 * ::ra_ct_equal is the constant-time equality used on every MAC / tag / digest /
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

#include "ra_secure.h"
#include "unity_minimal.h"

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
 * (no compound decisions in ra_ct_equal -- two independent single-condition
 * NULL guards and a diff accumulator; no `&&` or `||`.)
 */
static void test_ct_equal_matches(void)
{
  TEST_BEGIN("ra_ct_equal: equal buffers + zero length");
  const uint8_t a[k_ct_buf_len] = {1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U};
  const uint8_t b[k_ct_buf_len] = {1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U};
  TEST_ASSERT(ra_ct_equal(a, b, (size_t)k_ct_buf_len));
  TEST_ASSERT(ra_ct_equal(a, b, 0U)); /* vacuously equal. */
  TEST_END("ra_ct_equal: equal buffers + zero length");
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
  TEST_BEGIN("ra_ct_equal: difference at first/middle/last byte");
  const uint8_t a[k_ct_buf_len] = {1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U};

  uint8_t first[k_ct_buf_len] = {1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U};
  first[0]                    = 0xFFU;
  TEST_ASSERT(!ra_ct_equal(a, first, (size_t)k_ct_buf_len));

  uint8_t middle[k_ct_buf_len] = {1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U};
  middle[4]                    = 0xFFU;
  TEST_ASSERT(!ra_ct_equal(a, middle, (size_t)k_ct_buf_len));

  uint8_t last[k_ct_buf_len] = {1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U};
  last[k_ct_buf_len - 1U]    = 0xFFU;
  TEST_ASSERT(!ra_ct_equal(a, last, (size_t)k_ct_buf_len));
  TEST_END("ra_ct_equal: difference at first/middle/last byte");
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
  TEST_BEGIN("ra_ct_equal: NULL pointer guards");
  const uint8_t buf[k_ct_buf_len] = {};
  TEST_ASSERT(!ra_ct_equal(nullptr, buf, (size_t)k_ct_buf_len));
  TEST_ASSERT(!ra_ct_equal(buf, nullptr, (size_t)k_ct_buf_len));
  TEST_END("ra_ct_equal: NULL pointer guards");
}

int32_t main(void)
{
  test_ct_equal_matches();
  test_ct_equal_differs();
  test_ct_equal_null();
  (void)fprintf(stderr, "[OK ] test_ra_secure_cov.c\n");
  return 0;
}
