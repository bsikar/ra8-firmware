/**
 * @file test_ra8_pin_validator.c
 * @brief Unit tests for the pin ownership validator
 * @details Verifies pin ownership reset, claim, duplicate detection, release, and board-constant decoding behavior.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_err.h"
#include "ra8_pin_validator.h"
#include "ra8_port_constants.h"
#include "unity_minimal.h"

/**
 * @enum pin_validator_fixture_t
 * @brief Packed identifiers whose port half is valid but whose pin half is not,
 *        so the pin-range guard is the only rejection that can fire.
 */
typedef enum : uint16_t {
  k_pin_bad_index_only =
    (uint16_t)k_ra8_pin_count, /**< Port 0, pin index one past the last legal pin. */
} pin_validator_fixture_t;

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_reset_clears_everything(void)
{
  TEST_BEGIN("reset clears everything");
  ra8_pin_validator_reset();
  TEST_ASSERT(!ra8_pin_validator_is_claimed(k_ra8_pin_led1));
  TEST_ASSERT(!ra8_pin_validator_is_claimed(k_ra8_pin_led2));
  TEST_ASSERT(!ra8_pin_validator_is_claimed(k_ra8_pin_led3));
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
  ra8_pin_validator_reset();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_pin_validator_claim(k_ra8_pin_led1, "TEST"));
  TEST_ASSERT(ra8_pin_validator_is_claimed(k_ra8_pin_led1));
  TEST_ASSERT(!ra8_pin_validator_is_claimed(k_ra8_pin_led2));
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
  ra8_pin_validator_reset();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_pin_validator_claim(k_ra8_pin_led1, "FIRST"));
  const ra8_err_t second = ra8_pin_validator_claim(k_ra8_pin_led1, "SECOND");
  TEST_ASSERT_EQ(k_ra8_err_gpio_conflict, second);
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
  ra8_pin_validator_reset();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_pin_validator_claim(k_ra8_pin_led1, "FIRST"));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_pin_validator_release(k_ra8_pin_led1));
  TEST_ASSERT(!ra8_pin_validator_is_claimed(k_ra8_pin_led1));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_pin_validator_claim(k_ra8_pin_led1, "SECOND"));
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
  ra8_pin_validator_reset();
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_pin_validator_claim(k_ra8_pin_led1, nullptr));
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
  ra8_pin_validator_reset();
  const ra8_port_pin_t bad_pin = (ra8_port_pin_t)0xFFFE;
  const ra8_err_t      err     = ra8_pin_validator_claim(bad_pin, "TEST");
  TEST_ASSERT(err != k_ra8_ok);
  TEST_END("invalid pin rejected");
}

/**
 * @par MC/DC:
 * Decision: `if (idx >= k_ra8_pin_count)` in internal_flat_index (1 condition)
 * - Vector 1: pin index 0                  -> false (release succeeds, k_ra8_ok)
 * - Vector 2: pin index k_ra8_pin_count    -> true  (k_ra8_err_gpio_invalid_pin)
 * N+1 = 2 vectors for N=1: minimal MC/DC. Vector 2 is the only input that
 * reaches ra8_pin_validator_release's error return and the false-on-invalid
 * arm of ra8_pin_validator_is_claimed; vector 1 is the control.
 */
static void test_pin_index_out_of_range_surfaces_error(void)
{
  TEST_BEGIN("out-of-range pin index rejected by release and is_claimed");
  ra8_pin_validator_reset();
  const ra8_port_pin_t bad_pin = (ra8_port_pin_t)k_pin_bad_index_only;

  /* Vector 2: the pin half is out of range, so the flat index is never formed. */
  TEST_ASSERT_EQ(k_ra8_err_gpio_invalid_pin, ra8_pin_validator_release(bad_pin));
  TEST_ASSERT(!ra8_pin_validator_is_claimed(bad_pin));

  /* Vector 1 (control): a legal pin still releases cleanly. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_pin_validator_claim(k_ra8_pin_led1, "TEST"));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_pin_validator_release(k_ra8_pin_led1));
  TEST_ASSERT(!ra8_pin_validator_is_claimed(k_ra8_pin_led1));
  TEST_END("out-of-range pin index rejected by release and is_claimed");
}

int main(void)
{
  test_reset_clears_everything();
  test_claim_then_query();
  test_double_claim_rejected();
  test_release_allows_reclaim();
  test_null_owner_rejected();
  test_invalid_pin_rejected();
  test_pin_index_out_of_range_surfaces_error();
  return 0;
}
