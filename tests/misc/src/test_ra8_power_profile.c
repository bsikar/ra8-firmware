/**
 * @file test_ra8_power_profile.c
 * @brief Unit tests for libs/ra8_power_profile/ -- mocked GPIO + RTC.
 *
 * @details
 * Exercises the power-profiling helper using injected function-pointer
 * hooks. The mocks record every GPIO edge and feed back a controllable
 * monotonic clock so tests can assert on both the side-effects (pulse
 * count, edge order) and the time accumulators.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <string.h>

#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_power_profile.h"
#include "unity_minimal.h"

/**
 * @enum t_pp_clock_t
 * @brief RTC timestamps, in microseconds, driving the mock clock.
 *
 * @details
 * The profiler accumulates `exit - enter` per region, so each name states
 * which edge it marks and the arms read as a timeline. The nesting arm
 * interleaves two regions: sleep spans 100..500 (400 us) while standby spans
 * 200..350 (150 us) inside it, proving the two accumulate independently.
 */
typedef enum : uint16_t {
  k_t_us_nest_sleep_in    = 100U,  /**< Nesting arm: outer sleep region entered. */
  k_t_us_nest_standby_in  = 200U,  /**< Inner standby region entered.            */
  k_t_us_nest_standby_out = 350U,  /**< Inner region exited: 150 us accumulated. */
  k_t_us_nest_sleep_out   = 500U,  /**< Outer region exited: 400 us accumulated. */
  k_t_us_reset_out        = 999U,  /**< Exit edge of the span that reset_stats
                                        must discard.                            */
  k_t_us_span_a_in        = 1000U, /**< First timed span entered; also the
                                        enter edge of the GPIO-pulse arm.        */
  k_t_us_span_a_out       = 1100U, /**< First span exited: 100 us.       */
  k_t_us_pulse_out        = 1500U, /**< Exit edge of the GPIO-pulse arm. */
  k_t_us_span_b_in        = 5000U, /**< Second timed span entered.       */
  k_t_us_span_b_out       = 5250U, /**< Second span exited: 250 us.      */
} t_pp_clock_t;

/* ---------------------------------------------------------------------------
 * Mock GPIO + RTC state
 * ------------------------------------------------------------------------- */

enum : uint8_t {
  k_mock_pulse_log_capacity = 32, /**< Mock pulse log capacity. */
};

typedef struct {
  ra8_power_profile_region_id_t region_id; /**< Region ID. */
  bool                          entering;  /**< Entering.  */
} mock_pulse_record_t;

typedef struct {
  mock_pulse_record_t log[k_mock_pulse_log_capacity]; /**< Log.   */
  uint8_t             count;                          /**< Count. */
} mock_gpio_t;

typedef struct {
  uint64_t now_us; /**< Now us. */
} mock_rtc_t;

#include "ra8_power_profile_test_contracts.h"

/** @copydoc internal_mock_gpio_pulse */
RA8_INTERNAL static void
internal_mock_gpio_pulse(void* ctx, ra8_power_profile_region_id_t region_id, bool entering)
{
  mock_gpio_t* m = (mock_gpio_t*)ctx;
  if (m->count < k_mock_pulse_log_capacity) {
    m->log[m->count].region_id = region_id;
    m->log[m->count].entering  = entering;
    m->count += 1U;
  }
}

/** @copydoc internal_mock_rtc_now_us */
RA8_INTERNAL static uint64_t internal_mock_rtc_now_us(void* ctx)
{
  const mock_rtc_t* m = (const mock_rtc_t*)ctx;
  return m->now_us;
}

/* ---------------------------------------------------------------------------
 * Helpers
 * ------------------------------------------------------------------------- */

static mock_gpio_t s_gpio = {};
static mock_rtc_t  s_rtc  = {};

/** @copydoc internal_reset_mocks */
RA8_INTERNAL static void internal_reset_mocks(void)
{
  s_gpio = (mock_gpio_t){};
  s_rtc  = (mock_rtc_t){};
}

/** @copydoc internal_init_with_mocks */
RA8_INTERNAL static ra8_err_t internal_init_with_mocks(void)
{
  internal_reset_mocks();
  ra8_power_profile_config_t cfg = {
    .pulse         = internal_mock_gpio_pulse,
    .now_us        = internal_mock_rtc_now_us,
    .user_ctx_gpio = &s_gpio,
    .user_ctx_time = &s_rtc,
  };
  return ra8_power_profile_init(&cfg);
}

/* ---------------------------------------------------------------------------
 * Tests
  *
  * @par MC/DC:
  * (no compound decisions in this test -- exercises the public-API
  * happy path / error-rejection contract; no `&&` or `||` in the
  * code under test that this case touches)
 * ------------------------------------------------------------------------- */

/** @copydoc internal_test_init_rejects_null_cfg */
RA8_INTERNAL static void internal_test_init_rejects_null_cfg(void)
{
  TEST_BEGIN("init rejects null cfg");
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_power_profile_init(nullptr));
  TEST_END("init rejects null cfg");
}

/**
 * @copydoc internal_test_init_zeroes_stats
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
RA8_INTERNAL static void internal_test_init_zeroes_stats(void)
{
  TEST_BEGIN("init zeroes stats");
  TEST_ASSERT_EQ(k_ra8_ok, internal_init_with_mocks());

  ra8_power_profile_stats_t stats = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_power_profile_get_stats(&stats));
  for (uint8_t i = 0; i < (uint8_t)k_ra8_power_profile_max_regions; i += 1U) {
    TEST_ASSERT_EQ(0, stats.regions[i].entries);
    TEST_ASSERT_EQ(0, stats.regions[i].exits);
    TEST_ASSERT_EQ(0, stats.regions[i].total_time_us);
    TEST_ASSERT_EQ(0, stats.regions[i].is_open);
  }
  TEST_END("init zeroes stats");
}

/**
 * @copydoc internal_test_enter_exit_pulses_gpio
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
RA8_INTERNAL static void internal_test_enter_exit_pulses_gpio(void)
{
  TEST_BEGIN("enter/exit pulses gpio");
  TEST_ASSERT_EQ(k_ra8_ok, internal_init_with_mocks());

  s_rtc.now_us = k_t_us_span_a_in;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_power_profile_mark_enter(k_ra8_power_profile_region_active));
  s_rtc.now_us = k_t_us_pulse_out;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_power_profile_mark_exit(k_ra8_power_profile_region_active));

  TEST_ASSERT_EQ(2, s_gpio.count);
  TEST_ASSERT_EQ(k_ra8_power_profile_region_active, s_gpio.log[0].region_id);
  TEST_ASSERT_EQ(1, s_gpio.log[0].entering);
  TEST_ASSERT_EQ(k_ra8_power_profile_region_active, s_gpio.log[1].region_id);
  TEST_ASSERT_EQ(0, s_gpio.log[1].entering);
  TEST_END("enter/exit pulses gpio");
}

/**
 * @copydoc internal_test_time_accumulation
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
RA8_INTERNAL static void internal_test_time_accumulation(void)
{
  TEST_BEGIN("time accumulation");
  TEST_ASSERT_EQ(k_ra8_ok, internal_init_with_mocks());

  /* First pair: 100us. */
  s_rtc.now_us = k_t_us_span_a_in;
  (void)ra8_power_profile_mark_enter(k_ra8_power_profile_region_sleep);
  s_rtc.now_us = k_t_us_span_a_out;
  (void)ra8_power_profile_mark_exit(k_ra8_power_profile_region_sleep);

  /* Second pair: 250us. */
  s_rtc.now_us = k_t_us_span_b_in;
  (void)ra8_power_profile_mark_enter(k_ra8_power_profile_region_sleep);
  s_rtc.now_us = k_t_us_span_b_out;
  (void)ra8_power_profile_mark_exit(k_ra8_power_profile_region_sleep);

  ra8_power_profile_stats_t stats = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_power_profile_get_stats(&stats));

  const ra8_power_profile_region_stats_t* slot =
    &stats.regions[(uint8_t)k_ra8_power_profile_region_sleep];
  TEST_ASSERT_EQ(2, slot->entries);
  TEST_ASSERT_EQ(2, slot->exits);
  TEST_ASSERT_EQ(350, slot->total_time_us);
  TEST_ASSERT_EQ(0, slot->is_open);
  TEST_END("time accumulation");
}

/**
 * @copydoc internal_test_exit_without_enter_reports_invalid_state
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
RA8_INTERNAL static void internal_test_exit_without_enter_reports_invalid_state(void)
{
  TEST_BEGIN("exit without enter -> invalid state");
  TEST_ASSERT_EQ(k_ra8_ok, internal_init_with_mocks());

  TEST_ASSERT_EQ(k_ra8_err_invalid_state,
                 ra8_power_profile_mark_exit(k_ra8_power_profile_region_snooze));

  ra8_power_profile_stats_t stats = {};
  (void)ra8_power_profile_get_stats(&stats);
  TEST_ASSERT_EQ(0, stats.regions[(uint8_t)k_ra8_power_profile_region_snooze].entries);
  TEST_ASSERT_EQ(1, stats.regions[(uint8_t)k_ra8_power_profile_region_snooze].exits);
  TEST_END("exit without enter -> invalid state");
}

/**
 * @copydoc internal_test_region_id_range_check
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
RA8_INTERNAL static void internal_test_region_id_range_check(void)
{
  TEST_BEGIN("region id range check");
  TEST_ASSERT_EQ(k_ra8_ok, internal_init_with_mocks());

  TEST_ASSERT_EQ(
    k_ra8_err_range_check_failed,
    ra8_power_profile_mark_enter((ra8_power_profile_region_id_t)k_ra8_power_profile_max_regions));
  TEST_ASSERT_EQ(k_ra8_err_range_check_failed,
                 ra8_power_profile_mark_exit(
                   (ra8_power_profile_region_id_t)(k_ra8_power_profile_max_regions + 1U)));
  TEST_END("region id range check");
}

/**
 * @copydoc internal_test_get_stats_null
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
RA8_INTERNAL static void internal_test_get_stats_null(void)
{
  TEST_BEGIN("get_stats null");
  TEST_ASSERT_EQ(k_ra8_ok, internal_init_with_mocks());
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_power_profile_get_stats(nullptr));
  TEST_END("get_stats null");
}

/**
 * @copydoc internal_test_reset_stats_clears_accumulators
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
RA8_INTERNAL static void internal_test_reset_stats_clears_accumulators(void)
{
  TEST_BEGIN("reset_stats clears accumulators");
  TEST_ASSERT_EQ(k_ra8_ok, internal_init_with_mocks());

  s_rtc.now_us = 0U;
  (void)ra8_power_profile_mark_enter(k_ra8_power_profile_region_active);
  s_rtc.now_us = k_t_us_reset_out;
  (void)ra8_power_profile_mark_exit(k_ra8_power_profile_region_active);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_power_profile_reset_stats());

  ra8_power_profile_stats_t stats = {};
  (void)ra8_power_profile_get_stats(&stats);
  for (uint8_t i = 0; i < (uint8_t)k_ra8_power_profile_max_regions; i += 1U) {
    TEST_ASSERT_EQ(0, stats.regions[i].entries);
    TEST_ASSERT_EQ(0, stats.regions[i].exits);
    TEST_ASSERT_EQ(0, stats.regions[i].total_time_us);
  }
  TEST_END("reset_stats clears accumulators");
}

/**
 * @copydoc internal_test_null_hooks_safe
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
RA8_INTERNAL static void internal_test_null_hooks_safe(void)
{
  TEST_BEGIN("null hooks are safe");
  ra8_power_profile_config_t cfg = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_power_profile_init(&cfg));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_power_profile_mark_enter(k_ra8_power_profile_region_user_0));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_power_profile_mark_exit(k_ra8_power_profile_region_user_0));

  ra8_power_profile_stats_t stats = {};
  (void)ra8_power_profile_get_stats(&stats);
  TEST_ASSERT_EQ(1, stats.regions[(uint8_t)k_ra8_power_profile_region_user_0].entries);
  TEST_ASSERT_EQ(1, stats.regions[(uint8_t)k_ra8_power_profile_region_user_0].exits);
  TEST_ASSERT_EQ(0, stats.regions[(uint8_t)k_ra8_power_profile_region_user_0].total_time_us);
  TEST_END("null hooks are safe");
}

/**
 * @copydoc internal_test_multiple_regions_independent
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
RA8_INTERNAL static void internal_test_multiple_regions_independent(void)
{
  TEST_BEGIN("multiple regions independent");
  TEST_ASSERT_EQ(k_ra8_ok, internal_init_with_mocks());

  s_rtc.now_us = k_t_us_nest_sleep_in;
  (void)ra8_power_profile_mark_enter(k_ra8_power_profile_region_sleep);
  s_rtc.now_us = k_t_us_nest_standby_in;
  (void)ra8_power_profile_mark_enter(k_ra8_power_profile_region_software_standby);
  s_rtc.now_us = k_t_us_nest_standby_out;
  (void)ra8_power_profile_mark_exit(k_ra8_power_profile_region_software_standby);
  s_rtc.now_us = k_t_us_nest_sleep_out;
  (void)ra8_power_profile_mark_exit(k_ra8_power_profile_region_sleep);

  ra8_power_profile_stats_t stats = {};
  (void)ra8_power_profile_get_stats(&stats);
  TEST_ASSERT_EQ(400, stats.regions[(uint8_t)k_ra8_power_profile_region_sleep].total_time_us);
  TEST_ASSERT_EQ(150,
                 stats.regions[(uint8_t)k_ra8_power_profile_region_software_standby].total_time_us);
  TEST_END("multiple regions independent");
}

int main(void)
{
  internal_test_init_rejects_null_cfg();
  internal_test_init_zeroes_stats();
  internal_test_enter_exit_pulses_gpio();
  internal_test_time_accumulation();
  internal_test_exit_without_enter_reports_invalid_state();
  internal_test_region_id_range_check();
  internal_test_get_stats_null();
  internal_test_reset_stats_clears_accumulators();
  internal_test_null_hooks_safe();
  internal_test_multiple_regions_independent();
  return 0;
}
