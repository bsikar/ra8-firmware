/**
 * @file test_ra_err.c
 * @brief Unit tests for ra_err.h / ra_err_to_str()
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <string.h>

#include "ra_err.h"
#include "unity_minimal.h"

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_k_ra_ok_is_zero(void)
{
  TEST_BEGIN("k_ra_ok is zero");
  TEST_ASSERT_EQ(0, (int)k_ra_ok);
  TEST_END("k_ra_ok is zero");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_ra_err_is_error(void)
{
  TEST_BEGIN("ra_err_is_error");
  TEST_ASSERT(!ra_err_is_error(k_ra_ok));
  TEST_ASSERT(ra_err_is_error(k_ra_fail));
  TEST_ASSERT(ra_err_is_error(k_ra_err_null_ptr));
  TEST_ASSERT(ra_err_is_error(k_ra_err_timeout));
  TEST_END("ra_err_is_error");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_ra_err_to_str_known(void)
{
  TEST_BEGIN("ra_err_to_str known codes");
  TEST_ASSERT(strcmp(ra_err_to_str(k_ra_ok), "ok") == 0);
  TEST_ASSERT(strcmp(ra_err_to_str(k_ra_fail), "fail") == 0);
  TEST_ASSERT(strcmp(ra_err_to_str(k_ra_err_null_ptr), "null_ptr") == 0);
  TEST_ASSERT(strcmp(ra_err_to_str(k_ra_err_gpio_conflict), "gpio_conflict") == 0);
  TEST_END("ra_err_to_str known codes");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_ra_err_to_str_unknown(void)
{
  TEST_BEGIN("ra_err_to_str unknown code");
  const ra_err_t sentinel = (ra_err_t)0xBEEF;
  TEST_ASSERT(strcmp(ra_err_to_str(sentinel), "unknown") == 0);
  TEST_END("ra_err_to_str unknown code");
}

int32_t main(void)
{
  test_k_ra_ok_is_zero();
  test_ra_err_is_error();
  test_ra_err_to_str_known();
  test_ra_err_to_str_unknown();
  (void)fprintf(stderr, "[OK  ] test_ra_err.c\n");
  return 0;
}
