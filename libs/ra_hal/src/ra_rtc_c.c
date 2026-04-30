/**
 * @file ra_rtc_c.c
 * @brief RTC_C calendar-mode sub-driver implementation
 *
 * @par Tag
 * [Ring 3 / HAL] {World: S}
 *
 * @details
 * Calendar-mode wrapper around the RA8D2 RTC block (HUM Ch 26 pages
 * 1219..1255). Mirrors FSP `r_rtc_c` lifecycle: explicit
 * `_open`/`_close`, BCD calendar set / get, and alarm match register
 * programming.
 *
 * The sibling `ra_rtc.c` driver also drives this block; the two
 * drivers share the same MMIO window. RTC_C carries an explicit
 * `s_opened` guard so calendar / alarm calls can be rejected before
 * the first `_open`. On open the driver re-initialises RCR1, RCR2,
 * and the alarm-enable bytes to a known state.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_rtc_c.h"

#include <stdint.h>

#include "ra8d2_rtc_regs.h"
#include "ra_check.h"
#include "ra_err.h"
#include "ra_log.h"

static const char* s_tag = "RTC_C";

/* =============================================================================
 * Internal constants
 * =============================================================================
 */

typedef enum : uint8_t {
  k_ra_rtc_c_bcd_digit_mask  = 0x0FU, /**< Low BCD digit mask.    */
  k_ra_rtc_c_bcd_digit_shift = 4U,    /**< High BCD digit shift.  */
  k_ra_rtc_c_bcd_digit_base  = 10U,   /**< Decimal radix per digit.*/
} ra_rtc_c_bcd_t;

typedef enum : uint16_t {
  k_ra_rtc_c_year_base  = 2000U,  /**< Year base for ::ra_rtc_c_calendar_t. */
  k_ra_rtc_c_wait_iters = 10000U, /**< Bounded loop iterations.             */
} ra_rtc_c_const_t;

typedef enum : uint8_t {
  k_ra_rtc_c_byte_all = 0xFFU, /**< Mask for whole-byte waits. */
  k_ra_rtc_c_aenb_bit = 7U,    /**< AENB bit in alarm bytes.   */
} ra_rtc_c_byte_t;

typedef enum : uint8_t {
  k_ra_rtc_c_rcr1_aie = 0x01U, /**< RCR1.AIE bit (HUM 26.2.3 p1229). */
} ra_rtc_c_rcr1_t;

/* =============================================================================
 * Internal state
 * =============================================================================
 */

/**
 * @struct ra_rtc_c_state_t
 * @brief Singleton RTC_C driver shadow state.
 */
typedef struct {
  bool opened; /**< True after `ra_rtc_c_open` succeeded. */
} ra_rtc_c_state_t;

static ra_rtc_c_state_t s_state = {};

/* =============================================================================
 * Internal helpers
 * =============================================================================
 */

static uint8_t internal_bcd_to_bin(uint8_t bcd)
{
  const uint8_t high = (uint8_t)((bcd >> k_ra_rtc_c_bcd_digit_shift) & k_ra_rtc_c_bcd_digit_mask);
  const uint8_t low  = (uint8_t)(bcd & k_ra_rtc_c_bcd_digit_mask);
  return (uint8_t)((high * k_ra_rtc_c_bcd_digit_base) + low);
}

static uint8_t internal_bin_to_bcd(uint8_t bin)
{
  const uint8_t high = (uint8_t)(bin / k_ra_rtc_c_bcd_digit_base);
  const uint8_t low  = (uint8_t)(bin % k_ra_rtc_c_bcd_digit_base);
  return (uint8_t)((high << k_ra_rtc_c_bcd_digit_shift) | low);
}

/**
 * @brief Bounded spin until `(*reg & mask) == expect`.
 */
static void internal_wait_bit(volatile const uint8_t* reg, uint8_t mask, uint8_t expect)
{
  for (uint16_t i = 0U; i < k_ra_rtc_c_wait_iters; ++i) {
    if (((*reg) & mask) == expect) {
      return;
    }
  }
}

/**
 * @brief BCD-encode a calendar field, optionally tagging it with the
 *        AENB bit (alarm-enable per byte).
 */
static uint8_t internal_alarm_byte(uint8_t bin, bool match)
{
  uint8_t bcd = internal_bin_to_bcd(bin);
  if (match) {
    bcd = (uint8_t)(bcd | (uint8_t)(1U << k_ra_rtc_c_aenb_bit));
  }
  return bcd;
}

/* =============================================================================
 * Lifecycle
 * =============================================================================
 */

ra_err_t ra_rtc_c_open(void)
{
  volatile r_rtc_regs_t* rtc = ra_rtc();

  /* HUM Ch 26.2.4 "RCR2 : RTC Control Register 2" p 1230 -- stop the
   * counter and clear CNTMD so the block sits in calendar mode. */
  rtc->RCR2 = 0U;
  internal_wait_bit(&rtc->RCR2, (uint8_t)(1U << k_ra_rcr2_bit_cntmd), 0U);

  /* HUM Ch 26.2.3 "RCR1" p 1229 -- mask AIE/CIE/PIE. */
  rtc->RCR1 = 0U;
  internal_wait_bit(&rtc->RCR1, k_ra_rtc_c_byte_all, 0U);

  /* HUM Ch 26.2.13..26.2.20 -- clear every alarm-enable byte. */
  rtc->RSECAR  = 0U;
  rtc->RMINAR  = 0U;
  rtc->RHRAR   = 0U;
  rtc->RWKAR   = 0U;
  rtc->RDAYAR  = 0U;
  rtc->RMONAR  = 0U;
  rtc->RYRAR   = 0U;
  rtc->RYRAREN = 0U;

  /* HUM Ch 26.2.4 "RCR2" p 1230 -- HR24=1 (24-hour calendar). */
  rtc->RCR2 = (uint8_t)(1U << k_ra_rcr2_bit_hr24);
  internal_wait_bit(&rtc->RCR2,
                    (uint8_t)(1U << k_ra_rcr2_bit_hr24),
                    (uint8_t)(1U << k_ra_rcr2_bit_hr24));

  /* HUM Ch 26.2.4 "RCR2" p 1230 -- START=1. */
  rtc->RCR2 = (uint8_t)((1U << k_ra_rcr2_bit_hr24) | (1U << k_ra_rcr2_bit_start));
  internal_wait_bit(&rtc->RCR2,
                    (uint8_t)(1U << k_ra_rcr2_bit_start),
                    (uint8_t)(1U << k_ra_rcr2_bit_start));

  s_state.opened = true;
  ra_log_info(s_tag, "rtc_c open (24h calendar)");
  return k_ra_ok;
}

ra_err_t ra_rtc_c_close(void)
{
  if (!s_state.opened) {
    return k_ra_err_invalid_state;
  }
  volatile r_rtc_regs_t* rtc = ra_rtc();
  /* HUM Ch 26.2.3 "RCR1" p 1229 -- mask all IRQs. */
  rtc->RCR1 = 0U;
  /* HUM Ch 26.2.4 "RCR2" p 1230 -- stop the counter. */
  rtc->RCR2      = 0U;
  s_state.opened = false;
  return k_ra_ok;
}

/* =============================================================================
 * Calendar
 * =============================================================================
 */

ra_err_t ra_rtc_c_set_calendar(const ra_rtc_c_calendar_t* dt)
{
  RA_CHECK_NULL_PTR(dt, s_tag, "dt must not be nullptr");
  if (!s_state.opened) {
    return k_ra_err_invalid_state;
  }
  if (dt->year < k_ra_rtc_c_year_base) {
    return k_ra_err_invalid_arg;
  }

  volatile r_rtc_regs_t* rtc = ra_rtc();

  /* HUM Ch 26.2.4 "RCR2" p 1230 -- count registers must only be
   * written while START=0. Save and restore around the burst. */
  const uint8_t saved = rtc->RCR2;
  rtc->RCR2           = (uint8_t)(saved & (uint8_t)~(1U << k_ra_rcr2_bit_start));
  internal_wait_bit(&rtc->RCR2, (uint8_t)(1U << k_ra_rcr2_bit_start), 0U);

  /* HUM Ch 26.2.5..26.2.11 -- count register burst. */
  rtc->RSECCNT = internal_bin_to_bcd(dt->second);
  rtc->RMINCNT = internal_bin_to_bcd(dt->minute);
  rtc->RHRCNT  = internal_bin_to_bcd(dt->hour);
  rtc->RWKCNT  = dt->weekday;
  rtc->RDAYCNT = internal_bin_to_bcd(dt->day);
  rtc->RMONCNT = internal_bin_to_bcd(dt->month);
  rtc->RYRCNT  = (uint16_t)internal_bin_to_bcd((uint8_t)(dt->year - k_ra_rtc_c_year_base));

  rtc->RCR2 = (uint8_t)(saved | (1U << k_ra_rcr2_bit_start));
  internal_wait_bit(&rtc->RCR2,
                    (uint8_t)(1U << k_ra_rcr2_bit_start),
                    (uint8_t)(1U << k_ra_rcr2_bit_start));

  ra_log_info_val(s_tag, "rtc_c set_calendar year", (uint32_t)dt->year);
  return k_ra_ok;
}

ra_err_t ra_rtc_c_get_calendar(ra_rtc_c_calendar_t* out)
{
  RA_CHECK_NULL_PTR(out, s_tag, "out must not be nullptr");
  if (!s_state.opened) {
    return k_ra_err_invalid_state;
  }

  volatile r_rtc_regs_t* rtc = ra_rtc();
  out->second                = internal_bcd_to_bin(rtc->RSECCNT);
  out->minute                = internal_bcd_to_bin(rtc->RMINCNT);
  out->hour                  = internal_bcd_to_bin(rtc->RHRCNT);
  out->weekday               = rtc->RWKCNT;
  out->day                   = internal_bcd_to_bin(rtc->RDAYCNT);
  out->month                 = internal_bcd_to_bin(rtc->RMONCNT);
  out->year = (uint16_t)(k_ra_rtc_c_year_base + internal_bcd_to_bin((uint8_t)rtc->RYRCNT));
  return k_ra_ok;
}

/* =============================================================================
 * Alarm
 * =============================================================================
 */

ra_err_t ra_rtc_c_set_alarm(const ra_rtc_c_alarm_t* alarm)
{
  RA_CHECK_NULL_PTR(alarm, s_tag, "alarm must not be nullptr");
  if (!s_state.opened) {
    return k_ra_err_invalid_state;
  }

  volatile r_rtc_regs_t* rtc = ra_rtc();

  /* HUM Ch 26.2.13..26.2.20 -- alarm registers RSECAR..RYRAR. Each
   * BCD byte's bit 7 (AENB) gates the match. The year alarm uses a
   * separate enable bit RYRAREN. */
  rtc->RSECAR  = internal_alarm_byte(alarm->time.second, alarm->second_match);
  rtc->RMINAR  = internal_alarm_byte(alarm->time.minute, alarm->minute_match);
  rtc->RHRAR   = internal_alarm_byte(alarm->time.hour, alarm->hour_match);
  rtc->RWKAR   = (uint8_t)(alarm->time.weekday |
                           (alarm->weekday_match ? (uint8_t)(1U << k_ra_rtc_c_aenb_bit) : 0U));
  rtc->RDAYAR  = internal_alarm_byte(alarm->time.day, alarm->day_match);
  rtc->RMONAR  = internal_alarm_byte(alarm->time.month, alarm->month_match);
  rtc->RYRAR   = (uint16_t)internal_bin_to_bcd((uint8_t)(alarm->time.year - k_ra_rtc_c_year_base));
  rtc->RYRAREN = alarm->year_match ? (uint8_t)1U : (uint8_t)0U;

  /* HUM Ch 26.2.3 "RCR1.AIE" p 1229 -- arm the alarm IRQ. */
  rtc->RCR1 = (uint8_t)(rtc->RCR1 | k_ra_rtc_c_rcr1_aie);

  ra_log_info(s_tag, "rtc_c set_alarm armed");
  return k_ra_ok;
}
