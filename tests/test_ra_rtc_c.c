/**
 * @file test_ra_rtc_c.c
 * @brief Unit tests for ra_rtc_c.c (RTC_C calendar-mode sub-driver)
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8d2_rtc_regs.h"
#include "ra_err.h"
#include "ra_rtc_c.h"
#include "ra_sim_mmap.h"
#include "unity_minimal.h"

typedef enum : uint16_t {
  k_test_rtc_c_year     = 2026U,
  k_test_rtc_c_year_bcd = 0x26U, /**< 26 BCD-encoded.        */
} test_rtc_c_const_t;

typedef enum : uint8_t {
  k_test_rtc_c_month    = 4U,
  k_test_rtc_c_day      = 29U,
  k_test_rtc_c_weekday  = 3U,
  k_test_rtc_c_hour     = 12U,
  k_test_rtc_c_minute   = 34U,
  k_test_rtc_c_second   = 56U,
  k_test_rtc_c_aenb_bit = 7U,
} test_rtc_c_byte_t;

static void prep(void)
{
  ra_sim_mmap_reset();
  (void)ra_rtc_c_close();
}

static ra_rtc_c_calendar_t make_sample(void)
{
  return (ra_rtc_c_calendar_t){
    .year    = (uint16_t)k_test_rtc_c_year,
    .month   = k_test_rtc_c_month,
    .day     = k_test_rtc_c_day,
    .weekday = k_test_rtc_c_weekday,
    .hour    = k_test_rtc_c_hour,
    .minute  = k_test_rtc_c_minute,
    .second  = k_test_rtc_c_second,
  };
}

/* ---- Lifecycle ---- */

static void test_open_sets_calendar_mode(void)
{
  TEST_BEGIN("open enables HR24 + START and clears alarm bytes");
  prep();

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_rtc_c_open());

  volatile r_rtc_regs_t* rtc = ra_rtc();
  /* HR24 and START set, CNTMD clear (calendar mode). */
  TEST_ASSERT((rtc->RCR2 & (uint8_t)(1U << k_ra_rcr2_bit_hr24)) != 0U);
  TEST_ASSERT((rtc->RCR2 & (uint8_t)(1U << k_ra_rcr2_bit_start)) != 0U);
  TEST_ASSERT_EQ(0, (int)(rtc->RCR2 & (uint8_t)(1U << k_ra_rcr2_bit_cntmd)));
  /* Alarm bytes scrubbed. */
  TEST_ASSERT_EQ(0, (int)rtc->RSECAR);
  TEST_ASSERT_EQ(0, (int)rtc->RMINAR);
  TEST_ASSERT_EQ(0, (int)rtc->RYRAREN);
  TEST_END("open enables HR24 + START and clears alarm bytes");
}

static void test_close_without_open(void)
{
  TEST_BEGIN("close without open returns invalid_state");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_state, (int32_t)ra_rtc_c_close());
  TEST_END("close without open returns invalid_state");
}

static void test_close_after_open(void)
{
  TEST_BEGIN("close after open halts the counter");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_rtc_c_open());
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_rtc_c_close());

  volatile r_rtc_regs_t* rtc = ra_rtc();
  TEST_ASSERT_EQ(0, (int)(rtc->RCR2 & (uint8_t)(1U << k_ra_rcr2_bit_start)));
  TEST_END("close after open halts the counter");
}

/* ---- Pre-open guards ---- */

static void test_pre_open_guards(void)
{
  TEST_BEGIN("calendar / alarm calls reject pre-open");
  prep();

  ra_rtc_c_calendar_t cal   = make_sample();
  ra_rtc_c_alarm_t    alarm = {.time = cal};
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_state, (int32_t)ra_rtc_c_set_calendar(&cal));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_state, (int32_t)ra_rtc_c_get_calendar(&cal));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_state, (int32_t)ra_rtc_c_set_alarm(&alarm));
  TEST_END("calendar / alarm calls reject pre-open");
}

/* ---- Null-arg rejection ---- */

static void test_null_arg_rejection(void)
{
  TEST_BEGIN("set/get_calendar and set_alarm reject NULL");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_rtc_c_open());
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_rtc_c_set_calendar(nullptr));
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_rtc_c_get_calendar(nullptr));
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_rtc_c_set_alarm(nullptr));
  TEST_END("set/get_calendar and set_alarm reject NULL");
}

/* ---- set_calendar rejects year < 2000 ---- */

static void test_set_calendar_year_range(void)
{
  TEST_BEGIN("set_calendar rejects year < 2000");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_rtc_c_open());
  ra_rtc_c_calendar_t bad = make_sample();
  bad.year                = 1999U;
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_rtc_c_set_calendar(&bad));
  TEST_END("set_calendar rejects year < 2000");
}

/* ---- Round-trip set/get encodes BCD correctly ---- */

static void test_set_get_calendar_round_trip(void)
{
  TEST_BEGIN("set then get returns identical fields");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_rtc_c_open());
  ra_rtc_c_calendar_t cal = make_sample();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_rtc_c_set_calendar(&cal));

  ra_rtc_c_calendar_t out = {};
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_rtc_c_get_calendar(&out));
  TEST_ASSERT_EQ((int32_t)cal.year, (int32_t)out.year);
  TEST_ASSERT_EQ((int32_t)cal.month, (int32_t)out.month);
  TEST_ASSERT_EQ((int32_t)cal.day, (int32_t)out.day);
  TEST_ASSERT_EQ((int32_t)cal.weekday, (int32_t)out.weekday);
  TEST_ASSERT_EQ((int32_t)cal.hour, (int32_t)out.hour);
  TEST_ASSERT_EQ((int32_t)cal.minute, (int32_t)out.minute);
  TEST_ASSERT_EQ((int32_t)cal.second, (int32_t)out.second);
  TEST_END("set then get returns identical fields");
}

/* ---- set_calendar BCD-encodes the year register ---- */

static void test_set_calendar_bcd_year(void)
{
  TEST_BEGIN("set_calendar encodes year as BCD into RYRCNT");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_rtc_c_open());
  ra_rtc_c_calendar_t cal = make_sample();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_rtc_c_set_calendar(&cal));

  /* 2026 - 2000 = 26 -> BCD 0x26. */
  TEST_ASSERT_EQ((int32_t)k_test_rtc_c_year_bcd, (int32_t)ra_rtc()->RYRCNT);
  TEST_END("set_calendar encodes year as BCD into RYRCNT");
}

/* ---- set_alarm tags AENB on enabled match flags ---- */

static void test_set_alarm_aenb_bits(void)
{
  TEST_BEGIN("set_alarm tags AENB on the enabled match bytes and arms RCR1.AIE");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_rtc_c_open());

  ra_rtc_c_alarm_t alarm = {
    .time          = make_sample(),
    .second_match  = true,
    .minute_match  = false,
    .hour_match    = true,
    .day_match     = false,
    .month_match   = false,
    .weekday_match = false,
    .year_match    = true,
  };
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_rtc_c_set_alarm(&alarm));

  volatile r_rtc_regs_t* rtc       = ra_rtc();
  const uint8_t          aenb_mask = (uint8_t)(1U << k_test_rtc_c_aenb_bit);
  TEST_ASSERT((rtc->RSECAR & aenb_mask) != 0U);      /* second_match=true.  */
  TEST_ASSERT_EQ(0, (int)(rtc->RMINAR & aenb_mask)); /* minute_match=false. */
  TEST_ASSERT((rtc->RHRAR & aenb_mask) != 0U);
  TEST_ASSERT_EQ(0, (int)(rtc->RDAYAR & aenb_mask));
  TEST_ASSERT_EQ((int32_t)1U, (int32_t)rtc->RYRAREN); /* year_match=true.    */
  /* RCR1.AIE armed. */
  TEST_ASSERT((rtc->RCR1 & 0x01U) != 0U);
  TEST_END("set_alarm tags AENB on the enabled match bytes and arms RCR1.AIE");
}

int32_t main(void)
{
  test_open_sets_calendar_mode();
  test_close_without_open();
  test_close_after_open();
  test_pre_open_guards();
  test_null_arg_rejection();
  test_set_calendar_year_range();
  test_set_get_calendar_round_trip();
  test_set_calendar_bcd_year();
  test_set_alarm_aenb_bits();
  (void)fprintf(stderr, "[OK ] test_ra_rtc_c.c\n");
  return 0;
}
