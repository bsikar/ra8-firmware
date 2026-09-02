/**
 * @file test_app_rtc_alarm.c
 * @brief Integration test: RTC init + set + new ra8_rtc_set_alarm API
 *
 * @details
 * Mirrors examples/ek_ra8d2/hil_needs_revalidation/rtc_alarm/src/main.c bring-up flow:
 * ra8_rtc_init -> ra8_rtc_set seed -> ra8_rtc_set_alarm -> AIE enable
 * -> status flag round-trip. Exercises the new ``ra8_rtc_set_alarm``
 * helper introduced for this app, including its NULL-pointer and
 * out-of-range guards.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_rtc.h"
#include "ra8_rtc_regs.h"
#include "unity_minimal.h"

/**
 * @enum rtc_alarm_date_t
 * @brief Alarm year, past the RTC epoch so a dropped field is visible.
 */
typedef enum : uint16_t {
  k_rtc_alarm_year =
    2026U, /**< Alarm year, past the RTC epoch; non-zero, so a dropped field is visible. */
} rtc_alarm_date_t;

typedef enum : uint8_t {
  k_test_rtc_app_seed_year_lo = 26U,   /**< Test rtc app seed year lo. */
  k_test_rtc_app_seed_month   = 1U,    /**< Test rtc app seed month.   */
  k_test_rtc_app_seed_day     = 1U,    /**< Test rtc app seed day.     */
  k_test_rtc_app_alarm_offset = 5U,    /**< Test rtc app alarm offset. */
  k_test_rtc_app_alarm_enb    = 0x80U, /**< Test rtc app alarm enb.    */
  k_test_rtc_app_bad_hour     = 25U,   /**< Test rtc app bad hour.     */
  k_test_rtc_app_bad_min      = 60U,   /**< Test rtc app bad minimum.  */
  k_test_rtc_app_bad_sec      = 60U,   /**< Test rtc app bad sec.      */
} test_rtc_app_const_t;

static void reset_world(void)
{
  ra8_fake_mmap_reset();
}

/**
 * @brief Golden bring-up: init + set seed + set alarm + enable IRQ.
 *
 * @par MC/DC:
 * Compound decision in app: ``set_alarm != ok || set_irq_enable != ok``.
 * Two atomic conditions x N+1 = 3 vectors -- both ok (this test),
 * NULL-alarm rejects (test_rtc_app_alarm_null), bad-hour rejects
 * (test_rtc_app_alarm_bad_range).
 */
static void test_rtc_app_arm_alarm_ok(void)
{
  reset_world();
  TEST_BEGIN("rtc_alarm: arm +5 s alarm");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rtc_init());
  const ra8_rtc_datetime_t seed = {
    .year    = (uint16_t)(2000U + k_test_rtc_app_seed_year_lo),
    .month   = (uint8_t)k_test_rtc_app_seed_month,
    .day     = (uint8_t)k_test_rtc_app_seed_day,
    .weekday = 0U,
    .hour    = 0U,
    .minute  = 0U,
    .second  = 0U,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rtc_set(&seed));
  const ra8_rtc_datetime_t alarm = {
    .year    = seed.year,
    .month   = seed.month,
    .day     = seed.day,
    .weekday = 0U,
    .hour    = 0U,
    .minute  = 0U,
    .second  = (uint8_t)k_test_rtc_app_alarm_offset,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rtc_set_alarm(&alarm));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rtc_set_irq_enable((uint8_t)k_ra8_rtc_irq_alarm));
  /* RSECAR low nibble holds 5 in BCD; bit 7 is the ENB flag. */
  const uint8_t rsecar = ra8_rtc()->RSECAR;
  TEST_ASSERT((rsecar & (uint8_t)k_test_rtc_app_alarm_enb) != 0U);
  TEST_ASSERT_EQ(k_test_rtc_app_alarm_offset, (rsecar & 0x0FU));
  TEST_END("rtc_alarm: arm +5 s alarm");
}

/**
 * @brief NULL alarm pointer is rejected.
 *
 * @par MC/DC:
 * Decision: ``alarm == nullptr``. One atomic condition x 2 vectors --
 * NULL (this test) + non-NULL (golden test above).
 */
static void test_rtc_app_alarm_null(void)
{
  reset_world();
  TEST_BEGIN("rtc_alarm: NULL alarm rejected");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rtc_init());
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_rtc_set_alarm(nullptr));
  TEST_END("rtc_alarm: NULL alarm rejected");
}

/**
 * @brief Out-of-range hour / minute / second are rejected.
 *
 * @par MC/DC:
 * Compound decision: ``hour > 23 || minute > 59 || second > 59``.
 * Three atomic conditions x N+1 = 4 vectors. Vector A: all ok
 * (golden test). Vector B: bad hour only. Vector C: bad minute
 * only. Vector D: bad second only.
 */
static void test_rtc_app_alarm_bad_range(void)
{
  reset_world();
  TEST_BEGIN("rtc_alarm: out-of-range fields rejected");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rtc_init());
  ra8_rtc_datetime_t a = {
    .year    = k_rtc_alarm_year,
    .month   = 1U,
    .day     = 1U,
    .weekday = 0U,
    .hour    = (uint8_t)k_test_rtc_app_bad_hour,
    .minute  = 0U,
    .second  = 0U,
  };
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_rtc_set_alarm(&a));
  a.hour   = 0U;
  a.minute = (uint8_t)k_test_rtc_app_bad_min;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_rtc_set_alarm(&a));
  a.minute = 0U;
  a.second = (uint8_t)k_test_rtc_app_bad_sec;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_rtc_set_alarm(&a));
  TEST_END("rtc_alarm: out-of-range fields rejected");
}

/**
 * @brief Status flag round-trip: enable -> set bit -> clear -> reads 0.
 *
 * @par MC/DC:
 * Decision: ``(status & k_ra8_rtc_irq_alarm) == 0``. One atomic
 * condition x 2 vectors -- bit set (this test, fall-through) and
 * bit clear (after clear_status, end of this test).
 */
static void test_rtc_app_status_roundtrip(void)
{
  reset_world();
  TEST_BEGIN("rtc_alarm: status set + clear");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rtc_init());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rtc_set_irq_enable((uint8_t)k_ra8_rtc_irq_alarm));
  uint8_t status = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rtc_get_status(&status));
  TEST_ASSERT((status & (uint8_t)k_ra8_rtc_irq_alarm) != 0U);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rtc_clear_status((uint8_t)k_ra8_rtc_irq_alarm));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rtc_get_status(&status));
  TEST_ASSERT((status & (uint8_t)k_ra8_rtc_irq_alarm) == 0U);
  TEST_END("rtc_alarm: status set + clear");
}

int main(void)
{
  test_rtc_app_arm_alarm_ok();
  test_rtc_app_alarm_null();
  test_rtc_app_alarm_bad_range();
  test_rtc_app_status_roundtrip();
  return 0;
}
