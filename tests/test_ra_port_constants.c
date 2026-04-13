/**
 * @file test_ra_port_constants.c
 * @brief Unit tests for ra_port_constants.h packed pin encoding
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_port_constants.h"
#include "unity_minimal.h"

static void test_pin_encoding_matches_led_constants(void)
{
  TEST_BEGIN("LED constants decode correctly");
  TEST_ASSERT_EQ((int)k_ra_port_6, (int)RA_PIN_PORT(k_ra_pin_led1));
  TEST_ASSERT_EQ(0, (int)RA_PIN_PIN(k_ra_pin_led1));

  TEST_ASSERT_EQ((int)k_ra_port_3, (int)RA_PIN_PORT(k_ra_pin_led2));
  TEST_ASSERT_EQ(3, (int)RA_PIN_PIN(k_ra_pin_led2));

  TEST_ASSERT_EQ((int)k_ra_port_10, (int)RA_PIN_PORT(k_ra_pin_led3));
  TEST_ASSERT_EQ(7, (int)RA_PIN_PIN(k_ra_pin_led3));
  TEST_END("LED constants decode correctly");
}

static void test_ra_pin_macro_roundtrip(void)
{
  TEST_BEGIN("RA_PIN roundtrip");
  const ra_port_pin_t p = RA_PIN(k_ra_port_2, k_ra_pin_5);
  TEST_ASSERT_EQ(2, (int)RA_PIN_PORT(p));
  TEST_ASSERT_EQ(5, (int)RA_PIN_PIN(p));
  TEST_END("RA_PIN roundtrip");
}

static void test_port_count_invariants(void)
{
  TEST_BEGIN("port count invariants");
  TEST_ASSERT_EQ(15, (int)k_ra_port_count);
  TEST_ASSERT_EQ(14, (int)k_ra_port_max);
  TEST_ASSERT_EQ(16, (int)k_ra_pin_count);
  TEST_ASSERT_EQ(15, (int)k_ra_pin_max);
  TEST_END("port count invariants");
}

int32_t main(void)
{
  test_pin_encoding_matches_led_constants();
  test_ra_pin_macro_roundtrip();
  test_port_count_invariants();
  (void)fprintf(stderr, "[OK  ] test_ra_port_constants.c\n");
  return 0;
}
