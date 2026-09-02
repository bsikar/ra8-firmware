/**
 * @file test_ra8_port_constants.c
 * @brief Unit tests for ra8_port_constants.h packed pin encoding
 * @details Pins packed port/pin encoding and board LED identities so public hardware constants remain ABI-stable.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_port_constants.h"
#include "unity_minimal.h"

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_pin_encoding_matches_led_constants(void)
{
  TEST_BEGIN("LED constants decode correctly");
  TEST_ASSERT_EQ(k_ra8_port_6, RA8_PIN_PORT(k_ra8_pin_led1));
  TEST_ASSERT_EQ(0, RA8_PIN_PIN(k_ra8_pin_led1));

  TEST_ASSERT_EQ(k_ra8_port_3, RA8_PIN_PORT(k_ra8_pin_led2));
  TEST_ASSERT_EQ(3, RA8_PIN_PIN(k_ra8_pin_led2));

  TEST_ASSERT_EQ(k_ra8_port_10, RA8_PIN_PORT(k_ra8_pin_led3));
  TEST_ASSERT_EQ(7, RA8_PIN_PIN(k_ra8_pin_led3));
  TEST_END("LED constants decode correctly");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_ra8_pin_macro_roundtrip(void)
{
  TEST_BEGIN("RA8_PIN roundtrip");
  const ra8_port_pin_t p = RA8_PIN(k_ra8_port_2, k_ra8_pin_5);
  TEST_ASSERT_EQ(2, RA8_PIN_PORT(p));
  TEST_ASSERT_EQ(5, RA8_PIN_PIN(p));
  TEST_END("RA8_PIN roundtrip");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_port_count_invariants(void)
{
  TEST_BEGIN("port count invariants");
  TEST_ASSERT_EQ(15, k_ra8_port_count);
  TEST_ASSERT_EQ(14, k_ra8_port_max);
  TEST_ASSERT_EQ(16, k_ra8_pin_count);
  TEST_ASSERT_EQ(15, k_ra8_pin_max);
  TEST_END("port count invariants");
}

int main(void)
{
  test_pin_encoding_matches_led_constants();
  test_ra8_pin_macro_roundtrip();
  test_port_count_invariants();
  return 0;
}
