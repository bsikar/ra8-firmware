/**
 * @file test_ra_timer.c
 * @brief Unit tests for timer.c (thin ra_now_ms / ra_sleep_ms wrappers)
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_err.h"
#include "ra_sim_mmap.h"
#include "ra_time.h"
#include "unity_minimal.h"

/**
 * @enum ra_timer_test_const_t
 * @brief Constants used by the timer tests.
 */
typedef enum : uint32_t {
  k_ra_timer_test_cpu_hz   = 1000000UL, /**< 1 MHz pretend CPU clock.     */
  k_ra_timer_test_tick_add = 3U,        /**< Number of manual ticks.      */
  k_ra_timer_test_sleep_ms = 0U,        /**< Zero-length sleep (no wait). */
} ra_timer_test_const_t;

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_now_ms_matches_time_ms(void)
{
  TEST_BEGIN("timer now_ms matches time_ms");
  ra_sim_mmap_reset();

  TEST_ASSERT_EQ(k_ra_ok, ra_time_init((uint32_t)k_ra_timer_test_cpu_hz));
  const uint32_t a = ra_time_ms();
  const uint32_t b = ra_now_ms();
  TEST_ASSERT_EQ(a, b);
  TEST_END("timer now_ms matches time_ms");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_now_ms_advances_with_ticks(void)
{
  TEST_BEGIN("timer now_ms advances with ticks");
  ra_sim_mmap_reset();

  TEST_ASSERT_EQ(k_ra_ok, ra_time_init((uint32_t)k_ra_timer_test_cpu_hz));
  const uint32_t start = ra_now_ms();
  for (uint32_t i = 0U; i < (uint32_t)k_ra_timer_test_tick_add; i++) {
    ra_time_on_tick();
  }
  TEST_ASSERT_EQ((start + (uint32_t)k_ra_timer_test_tick_add), ra_now_ms());
  TEST_END("timer now_ms advances with ticks");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_sleep_ms_zero_returns_immediately(void)
{
  TEST_BEGIN("timer sleep_ms zero returns immediately");
  ra_sim_mmap_reset();

  TEST_ASSERT_EQ(k_ra_ok, ra_time_init((uint32_t)k_ra_timer_test_cpu_hz));
  const uint32_t before = ra_now_ms();
  ra_sleep_ms((uint32_t)k_ra_timer_test_sleep_ms);
  const uint32_t after = ra_now_ms();
  TEST_ASSERT_EQ(before, after);
  TEST_END("timer sleep_ms zero returns immediately");
}

int32_t main(void)
{
  test_now_ms_matches_time_ms();
  test_now_ms_advances_with_ticks();
  test_sleep_ms_zero_returns_immediately();
  (void)fprintf(stderr, "[OK  ] test_ra_timer.c\n");
  return 0;
}
