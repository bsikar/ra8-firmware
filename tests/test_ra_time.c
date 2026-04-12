/**
 * @file test_ra_time.c
 * @brief Unit tests for ra_time.h SysTick tick counter
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_err.h"
#include "ra_time.h"
#include "unity_minimal.h"

/* On the host there is no real SysTick; we use the exposed
 * ra_time_on_tick() to manually advance the counter. */

static void test_init_rejects_zero_hz(void)
{
  TEST_BEGIN("ra_time_init rejects 0 Hz");
  TEST_ASSERT_EQ((int)k_ra_err_invalid_arg, (int)ra_time_init(0U));
  TEST_END("ra_time_init rejects 0 Hz");
}

static void test_init_accepts_reasonable_hz(void)
{
  TEST_BEGIN("ra_time_init accepts sane cpu_hz");
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_time_init(1000000UL));
  TEST_END("ra_time_init accepts sane cpu_hz");
}

static void test_tick_counter_advances(void)
{
  TEST_BEGIN("tick counter advances on on_tick");
  (void)ra_time_init(1000000UL);
  const uint32_t start = ra_time_ms();
  ra_time_on_tick();
  ra_time_on_tick();
  ra_time_on_tick();
  TEST_ASSERT_EQ((int)(start + 3U), (int)ra_time_ms());
  TEST_END("tick counter advances on on_tick");
}

static void test_now_and_sleep_aliases(void)
{
  TEST_BEGIN("short aliases forward to systick");
  (void)ra_time_init(1000000UL);
  const uint32_t via_long  = ra_time_ms();
  const uint32_t via_short = ra_now_ms();
  TEST_ASSERT_EQ((int)via_long, (int)via_short);
  TEST_END("short aliases forward to systick");
}

int main(void)
{
  test_init_rejects_zero_hz();
  test_init_accepts_reasonable_hz();
  test_tick_counter_advances();
  test_now_and_sleep_aliases();
  (void)fprintf(stderr, "[OK  ] test_ra_time.c\n");
  return 0;
}
