/**
 * @file test_ra8_rtc.c
 * @brief Unit tests for the BCD RTC driver (ra8_rtc.c)
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_rtc.h"
#include "ra8_rtc_regs.h"
#include "ra8_sim_mmap.h"
#include "ra8_system_regs.h"
#include "unity_minimal.h"

/**
 * @enum t_rtc_t
 * @brief Out-of-range calendar fields the alarm validator must reject.
 *
 * @details
 * Each is exactly one past the last legal value for its field, so the guard is
 * tested at its boundary rather than with an obviously absurd number.
 */
typedef enum : uint16_t {
  k_t_hour_over    = 24U,   /**< One past the last valid hour (0..23).      */
  k_t_min_sec_over = 60U,   /**< One past the last valid minute and second. */
  k_t_year_valid   = 2026U, /**< A valid year, so only the field under test is
                                 out of range.                                 */
} t_rtc_t;

typedef enum : uint8_t {
  k_test_rcr2_hr24_start = 0x41U, /**< HR24(bit6) | START(bit0). */
} test_rcr2_t;

typedef enum : uint16_t {
  k_test_rtc_year = 2026U, /**< Test rtc year. */
} test_rtc_year_t;

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_happy_path(void)
{
  TEST_BEGIN("ra8_rtc_init writes RCR2 HR24+START");
  ra8_sim_mmap_reset();

  TEST_ASSERT_EQ(k_ra8_ok, ra8_rtc_init());

  volatile r_rtc_regs_t* rtc = ra8_rtc();
  /* Final write should be HR24=1, START=1. */
  TEST_ASSERT_EQ(k_test_rcr2_hr24_start, rtc->RCR2);
  TEST_ASSERT_EQ(0, rtc->RCR1);

  TEST_END("ra8_rtc_init writes RCR2 HR24+START");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_set_null_rejected(void)
{
  TEST_BEGIN("ra8_rtc_set rejects NULL");
  ra8_sim_mmap_reset();
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_rtc_set(nullptr));
  TEST_END("ra8_rtc_set rejects NULL");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_get_null_rejected(void)
{
  TEST_BEGIN("ra8_rtc_get rejects NULL");
  ra8_sim_mmap_reset();
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_rtc_get(nullptr));
  TEST_END("ra8_rtc_get rejects NULL");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_set_rejects_year_before_2000(void)
{
  TEST_BEGIN("ra8_rtc_set rejects year < 2000");
  ra8_sim_mmap_reset();
  const ra8_rtc_datetime_t dt = {
    .year    = 1999U,
    .month   = 1U,
    .day     = 1U,
    .weekday = 0U,
    .hour    = 0U,
    .minute  = 0U,
    .second  = 0U,
  };
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_rtc_set(&dt));
  TEST_END("ra8_rtc_set rejects year < 2000");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_set_then_get_round_trip(void)
{
  TEST_BEGIN("ra8_rtc_set then ra8_rtc_get round-trips");
  ra8_sim_mmap_reset();

  TEST_ASSERT_EQ(k_ra8_ok, ra8_rtc_init());

  const ra8_rtc_datetime_t in = {
    .year    = (uint16_t)k_test_rtc_year,
    .month   = 4U,
    .day     = 12U,
    .weekday = 0U, /* Sunday. */
    .hour    = 13U,
    .minute  = 37U,
    .second  = 42U,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rtc_set(&in));

  ra8_rtc_datetime_t out = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rtc_get(&out));

  TEST_ASSERT_EQ(in.year, out.year);
  TEST_ASSERT_EQ(in.month, out.month);
  TEST_ASSERT_EQ(in.day, out.day);
  TEST_ASSERT_EQ(in.weekday, out.weekday);
  TEST_ASSERT_EQ(in.hour, out.hour);
  TEST_ASSERT_EQ(in.minute, out.minute);
  TEST_ASSERT_EQ(in.second, out.second);

  TEST_END("ra8_rtc_set then ra8_rtc_get round-trips");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_bcd_boundary_values(void)
{
  TEST_BEGIN("ra8_rtc BCD 59 max");
  ra8_sim_mmap_reset();

  TEST_ASSERT_EQ(k_ra8_ok, ra8_rtc_init());

  const ra8_rtc_datetime_t in = {
    .year    = 2099U,
    .month   = 12U,
    .day     = 31U,
    .weekday = 6U, /* Saturday. */
    .hour    = 23U,
    .minute  = 59U,
    .second  = 59U,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rtc_set(&in));

  ra8_rtc_datetime_t out = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rtc_get(&out));
  TEST_ASSERT_EQ(59, out.second);
  TEST_ASSERT_EQ(59, out.minute);
  TEST_ASSERT_EQ(23, out.hour);
  TEST_ASSERT_EQ(12, out.month);
  TEST_ASSERT_EQ(2099, out.year);

  TEST_END("ra8_rtc BCD 59 max");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_bcd_zero(void)
{
  TEST_BEGIN("ra8_rtc BCD 0 minimum");
  ra8_sim_mmap_reset();

  TEST_ASSERT_EQ(k_ra8_ok, ra8_rtc_init());

  const ra8_rtc_datetime_t in = {
    .year    = 2000U,
    .month   = 1U,
    .day     = 1U,
    .weekday = 0U,
    .hour    = 0U,
    .minute  = 0U,
    .second  = 0U,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rtc_set(&in));

  ra8_rtc_datetime_t out = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rtc_get(&out));
  TEST_ASSERT_EQ(0, out.second);
  TEST_ASSERT_EQ(0, out.minute);
  TEST_ASSERT_EQ(0, out.hour);
  TEST_ASSERT_EQ(2000, out.year);

  TEST_END("ra8_rtc BCD 0 minimum");
}

/* ---- full build-out ---- */

static uint32_t s_rtc_cb_count;
static uint8_t  s_rtc_cb_last_mask;

static void stub_rtc_cb(void* ctx, uint8_t mask)
{
  (void)ctx;
  ++s_rtc_cb_count;
  s_rtc_cb_last_mask = mask;
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_deinit(void)
{
  TEST_BEGIN("rtc deinit");
  ra8_sim_mmap_reset();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rtc_init());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rtc_deinit());
  TEST_ASSERT_EQ(0, ra8_rtc()->RCR1);
  TEST_ASSERT_EQ(0, ra8_rtc()->RCR2);
  TEST_END("rtc deinit");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_irq_enable_and_status(void)
{
  TEST_BEGIN("rtc irq enable + status");
  ra8_sim_mmap_reset();
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_rtc_set_irq_enable((uint8_t)k_ra8_rtc_irq_alarm | (uint8_t)k_ra8_rtc_irq_periodic));

  uint8_t mask = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rtc_get_status(&mask));
  TEST_ASSERT_EQ(((uint8_t)k_ra8_rtc_irq_alarm | (uint8_t)k_ra8_rtc_irq_periodic), mask);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_rtc_clear_status((uint8_t)k_ra8_rtc_irq_alarm));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rtc_get_status(&mask));
  TEST_ASSERT_EQ(k_ra8_rtc_irq_periodic, mask);
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_rtc_get_status(nullptr));
  TEST_END("rtc irq enable + status");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_attach_and_dispatch(void)
{
  TEST_BEGIN("rtc attach + dispatch");
  ra8_sim_mmap_reset();
  s_rtc_cb_count     = 0U;
  s_rtc_cb_last_mask = 0U;

  TEST_ASSERT_EQ(k_ra8_ok, ra8_rtc_attach_handler(stub_rtc_cb, (void*)(uintptr_t)0x42U));
  ra8_rtc()->RCR1 = (uint8_t)k_ra8_rtc_irq_alarm;
  ra8_rtc_dispatch();
  TEST_ASSERT_EQ(1, s_rtc_cb_count);
  TEST_ASSERT_EQ(k_ra8_rtc_irq_alarm, s_rtc_cb_last_mask);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_rtc_attach_handler(nullptr, nullptr));
  ra8_rtc_dispatch();
  TEST_ASSERT_EQ(1, s_rtc_cb_count);
  TEST_END("rtc attach + dispatch");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_power_transition(void)
{
  TEST_BEGIN("rtc power transition");
  ra8_sim_mmap_reset();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rtc_init());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rtc_enter_stop());
  TEST_ASSERT((ra8_rtc()->RCR2 & (uint8_t)0x01U) == 0U);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rtc_exit_stop());
  TEST_ASSERT((ra8_rtc()->RCR2 & (uint8_t)0x01U) != 0U);
  TEST_END("rtc power transition");
}

/**
 * @brief MC/DC for ra8_rtc_set_alarm range guard.
 *
 * @par MC/DC:
 * Decision under test (libs/ra8_hal/src/ra8_rtc.c@ra8_rtc_set_alarm):
 * ``alarm->hour > k_ra8_rtc_alarm_max_hr || alarm->minute >
 * k_ra8_rtc_alarm_max_min || alarm->second > k_ra8_rtc_alarm_max_sec``.
 * Three atomic conditions x N+1 = 4 vectors:
 *  - Vector A: all in range -> false  (returns ok).
 *  - Vector B: hour out of range only -> true.
 *  - Vector C: minute out of range only -> true.
 *  - Vector D: second out of range only -> true.
 * Vectors A+B prove `hour` independently flips the outcome; A+C the
 * same for `minute`; A+D the same for `second`. Minimal MC/DC.
 *
 * @since 0.1.0
 */
static void test_mcdc_set_alarm_range_guard(void)
{
  TEST_BEGIN("mcdc rtc_set_alarm range guard");
  ra8_sim_mmap_reset();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rtc_init());
  ra8_rtc_datetime_t a = {.year    = (uint16_t)k_t_year_valid,
                          .month   = (uint8_t)1,
                          .day     = (uint8_t)1,
                          .weekday = 0U,
                          .hour    = 0U,
                          .minute  = 0U,
                          .second  = 0U};
  /* Vector A: all in range. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rtc_set_alarm(&a));
  /* Vector B: bad hour only. */
  a.hour = (uint8_t)k_t_hour_over;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_rtc_set_alarm(&a));
  /* Vector C: bad minute only. */
  a.hour   = 0U;
  a.minute = (uint8_t)k_t_min_sec_over;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_rtc_set_alarm(&a));
  /* Vector D: bad second only. */
  a.minute = 0U;
  a.second = (uint8_t)k_t_min_sec_over;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_rtc_set_alarm(&a));
  TEST_END("mcdc rtc_set_alarm range guard");
}

/**
 * @brief ra8_rtc_clock_init brings up and selects the sub-clock source.
 *
 * @par MC/DC:
 * (ra8_rtc_clock_init contains only single-condition guards -- no `&&`
 * or `||`. This case drives the sub-clock branch of `if (src ==
 * k_ra8_rtc_clk_loco)` to false and asserts SOSCCR.SOSTP cleared and
 * RCR4.RCKSEL = 0.)
 */
static void test_clock_init_subclock(void)
{
  TEST_BEGIN("ra8_rtc_clock_init sub-clock");
  ra8_sim_mmap_reset();

  TEST_ASSERT_EQ(k_ra8_ok, ra8_rtc_clock_init(k_ra8_rtc_clk_subclock));

  /* Sub-clock oscillator started: SOSCCR.SOSTP cleared. */
  TEST_ASSERT_EQ(0, (*ra8_sys_sosccr() & k_ra8_sosccr_sostp_mask));
  /* Count source selected: RCR4.RCKSEL = 0 (sub-clock). */
  TEST_ASSERT_EQ(0, ra8_rtc()->RCR4);

  TEST_END("ra8_rtc_clock_init sub-clock");
}

/**
 * @brief ra8_rtc_clock_init brings up and selects LOCO + programs RFRL.
 *
 * @par MC/DC:
 * (ra8_rtc_clock_init contains only single-condition guards -- no `&&`
 * or `||`. This case drives `if (src == k_ra8_rtc_clk_loco)` to true and
 * asserts LOCOCR.LCSTP cleared, RCR4.RCKSEL = 1, and the 32.768 kHz
 * prescaler value in RFRL.)
 */
static void test_clock_init_loco(void)
{
  TEST_BEGIN("ra8_rtc_clock_init loco");
  ra8_sim_mmap_reset();

  TEST_ASSERT_EQ(k_ra8_ok, ra8_rtc_clock_init(k_ra8_rtc_clk_loco));

  /* LOCO running: LOCOCR.LCSTP cleared. */
  TEST_ASSERT_EQ(0, (*ra8_sys_lococr() & k_ra8_lococr_lcstp_mask));
  /* Count source selected: RCR4.RCKSEL = 1 (LOCO). */
  TEST_ASSERT_EQ(1, ra8_rtc()->RCR4);
  /* Prescaler programmed for the 32.768 kHz LOCO: (32768/128)-1 = 0x00FF. */
  TEST_ASSERT_EQ(0x00FF, ra8_rtc()->RFRL);

  TEST_END("ra8_rtc_clock_init loco");
}

/**
 * @brief ra8_rtc_clock_init rejects an out-of-range count source.
 *
 * @par MC/DC:
 * Decision: `if (src > k_ra8_rtc_clk_loco)` (1 condition). The false
 * outcome is covered by the sub-clock / loco cases above; this case is
 * the true vector (src = 2), proving the out-of-range guard rejects.
 */
static void test_clock_init_invalid_src(void)
{
  TEST_BEGIN("ra8_rtc_clock_init rejects bad src");
  ra8_sim_mmap_reset();

  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_rtc_clock_init((ra8_rtc_clk_src_t)2U));

  TEST_END("ra8_rtc_clock_init rejects bad src");
}

int32_t main(void)
{
  test_init_happy_path();
  test_set_null_rejected();
  test_get_null_rejected();
  test_set_rejects_year_before_2000();
  test_set_then_get_round_trip();
  test_bcd_boundary_values();
  test_bcd_zero();
  test_deinit();
  test_irq_enable_and_status();
  test_attach_and_dispatch();
  test_power_transition();
  test_mcdc_set_alarm_range_guard();
  test_clock_init_subclock();
  test_clock_init_loco();
  test_clock_init_invalid_src();
  (void)fprintf(stderr, "[OK ] test_ra8_rtc.c\n");
  return 0;
}
