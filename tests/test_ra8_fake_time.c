/**
 * @file test_ra8_fake_time.c
 * @brief Unit tests for the host deterministic time source (tests/mocks/ra8_fake_time.c).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_fake_time.h"
#include "ra8_time.h"
#include "unity_minimal.h"

/**
 * @enum t_advance_ms_t
 * @brief Fake-clock advances, in milliseconds.
 *
 * @details
 * Chosen so no advance is a multiple of another: a tick counter that scales or
 * truncates would still agree on round numbers, but cannot agree on all four.
 */
typedef enum : uint16_t {
  k_t_advance_short  = 10U,   /**< A sub-tick advance.                        */
  k_t_advance_odd    = 42U,   /**< A non-round advance.                       */
  k_t_advance_half_s = 500U,  /**< Half a second.                             */
  k_t_advance_one_s  = 1000U, /**< A full second, crossing the seconds field. */
} t_advance_ms_t;

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_reset_zeros_counter(void)
{
  TEST_BEGIN("ra8_fake_time_reset: tick counter back to zero");

  /* Pre-state: advance the clock so we can prove reset takes us
   * back down. */
  ra8_fake_time_reset();
  ra8_fake_time_advance_ms(k_t_advance_half_s);
  TEST_ASSERT_EQ(500, ra8_time_ms());

  ra8_fake_time_reset();
  TEST_ASSERT_EQ(0, ra8_time_ms());
  TEST_END("ra8_fake_time_reset: tick counter back to zero");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_advance_ms_matches_count(void)
{
  TEST_BEGIN("ra8_fake_time_advance_ms: tick counter tracks argument");
  ra8_fake_time_reset();

  ra8_fake_time_advance_ms(1U);
  TEST_ASSERT_EQ(1, ra8_time_ms());

  ra8_fake_time_advance_ms(k_t_advance_short);
  TEST_ASSERT_EQ(11, ra8_time_ms());

  ra8_fake_time_advance_ms(k_t_advance_one_s);
  TEST_ASSERT_EQ(1011, ra8_time_ms());
  TEST_END("ra8_fake_time_advance_ms: tick counter tracks argument");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_advance_zero_is_noop(void)
{
  TEST_BEGIN("ra8_fake_time_advance_ms(0) is a no-op");
  ra8_fake_time_reset();
  ra8_fake_time_advance_ms(k_t_advance_odd);
  ra8_fake_time_advance_ms(0U);
  TEST_ASSERT_EQ(42, ra8_time_ms());
  TEST_END("ra8_fake_time_advance_ms(0) is a no-op");
}

int32_t main(void)
{
  test_reset_zeros_counter();
  test_advance_ms_matches_count();
  test_advance_zero_is_noop();
  (void)fprintf(stderr, "[OK  ] test_ra8_fake_time.c\n");
  return 0;
}
