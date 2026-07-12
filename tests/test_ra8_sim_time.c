/**
 * @file test_ra8_sim_time.c
 * @brief Unit tests for the host deterministic time source (tests/mocks/ra8_sim_time.c).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_sim_time.h"
#include "ra8_time.h"
#include "unity_minimal.h"

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_reset_zeros_counter(void)
{
  TEST_BEGIN("ra8_sim_time_reset: tick counter back to zero");

  /* Pre-state: advance the clock so we can prove reset takes us
   * back down. */
  ra8_sim_time_reset();
  ra8_sim_time_advance_ms(500U);
  TEST_ASSERT_EQ(500, ra8_time_ms());

  ra8_sim_time_reset();
  TEST_ASSERT_EQ(0, ra8_time_ms());
  TEST_END("ra8_sim_time_reset: tick counter back to zero");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_advance_ms_matches_count(void)
{
  TEST_BEGIN("ra8_sim_time_advance_ms: tick counter tracks argument");
  ra8_sim_time_reset();

  ra8_sim_time_advance_ms(1U);
  TEST_ASSERT_EQ(1, ra8_time_ms());

  ra8_sim_time_advance_ms(10U);
  TEST_ASSERT_EQ(11, ra8_time_ms());

  ra8_sim_time_advance_ms(1000U);
  TEST_ASSERT_EQ(1011, ra8_time_ms());
  TEST_END("ra8_sim_time_advance_ms: tick counter tracks argument");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_advance_zero_is_noop(void)
{
  TEST_BEGIN("ra8_sim_time_advance_ms(0) is a no-op");
  ra8_sim_time_reset();
  ra8_sim_time_advance_ms(42U);
  ra8_sim_time_advance_ms(0U);
  TEST_ASSERT_EQ(42, ra8_time_ms());
  TEST_END("ra8_sim_time_advance_ms(0) is a no-op");
}

int32_t main(void)
{
  test_reset_zeros_counter();
  test_advance_ms_matches_count();
  test_advance_zero_is_noop();
  (void)fprintf(stderr, "[OK  ] test_ra8_sim_time.c\n");
  return 0;
}
