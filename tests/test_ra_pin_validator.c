/**
 * @file test_ra_pin_validator.c
 * @brief Unit tests for the pin ownership validator
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_err.h"
#include "ra_pin_validator.h"
#include "ra_port_constants.h"
#include "unity_minimal.h"

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_reset_clears_everything(void)
{
  TEST_BEGIN("reset clears everything");
  ra_pin_validator_reset();
  TEST_ASSERT(!ra_pin_validator_is_claimed(k_ra_pin_led1));
  TEST_ASSERT(!ra_pin_validator_is_claimed(k_ra_pin_led2));
  TEST_ASSERT(!ra_pin_validator_is_claimed(k_ra_pin_led3));
  TEST_END("reset clears everything");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_claim_then_query(void)
{
  TEST_BEGIN("claim then query");
  ra_pin_validator_reset();
  TEST_ASSERT_EQ(k_ra_ok, ra_pin_validator_claim(k_ra_pin_led1, "TEST"));
  TEST_ASSERT(ra_pin_validator_is_claimed(k_ra_pin_led1));
  TEST_ASSERT(!ra_pin_validator_is_claimed(k_ra_pin_led2));
  TEST_END("claim then query");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_double_claim_rejected(void)
{
  TEST_BEGIN("double claim rejected");
  ra_pin_validator_reset();
  TEST_ASSERT_EQ(k_ra_ok, ra_pin_validator_claim(k_ra_pin_led1, "FIRST"));
  const ra_err_t second = ra_pin_validator_claim(k_ra_pin_led1, "SECOND");
  TEST_ASSERT_EQ((int)k_ra_err_gpio_conflict, (int)second);
  TEST_END("double claim rejected");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_release_allows_reclaim(void)
{
  TEST_BEGIN("release allows reclaim");
  ra_pin_validator_reset();
  TEST_ASSERT_EQ(k_ra_ok, ra_pin_validator_claim(k_ra_pin_led1, "FIRST"));
  TEST_ASSERT_EQ(k_ra_ok, ra_pin_validator_release(k_ra_pin_led1));
  TEST_ASSERT(!ra_pin_validator_is_claimed(k_ra_pin_led1));
  TEST_ASSERT_EQ(k_ra_ok, ra_pin_validator_claim(k_ra_pin_led1, "SECOND"));
  TEST_END("release allows reclaim");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_null_owner_rejected(void)
{
  TEST_BEGIN("null owner rejected");
  ra_pin_validator_reset();
  TEST_ASSERT_EQ((int)k_ra_err_null_ptr, (int)ra_pin_validator_claim(k_ra_pin_led1, NULL));
  TEST_END("null owner rejected");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_invalid_pin_rejected(void)
{
  TEST_BEGIN("invalid pin rejected");
  ra_pin_validator_reset();
  const ra_port_pin_t bad_pin = (ra_port_pin_t)0xFFFE;
  const ra_err_t      err     = ra_pin_validator_claim(bad_pin, "TEST");
  TEST_ASSERT(err != k_ra_ok);
  TEST_END("invalid pin rejected");
}

int32_t main(void)
{
  test_reset_clears_everything();
  test_claim_then_query();
  test_double_claim_rejected();
  test_release_allows_reclaim();
  test_null_owner_rejected();
  test_invalid_pin_rejected();
  (void)fprintf(stderr, "[OK  ] test_ra_pin_validator.c\n");
  return 0;
}
