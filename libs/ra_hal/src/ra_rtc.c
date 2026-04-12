/**
 * @file ra_rtc.c
 * @brief BCD-calendar RTC driver implementation
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_rtc.h"

#include <stdint.h>

#include "ra8d2_rtc_regs.h"
#include "ra_check.h"
#include "ra_err.h"
#include "ra_log.h"

static const char* s_tag = "RTC";

typedef enum : uint8_t {
  k_ra_bcd_digit_mask  = 0x0FU,
  k_ra_bcd_digit_shift = 4U,
  k_ra_bcd_digit_base  = 10U,
} ra_rtc_bcd_t;

typedef enum : uint16_t {
  k_ra_rtc_year_base = 2000U, /**< Base year for ra_rtc_datetime_t. */
} ra_rtc_year_t;

static uint8_t internal_bcd_to_bin(uint8_t bcd)
{
  const uint8_t high = (uint8_t)((bcd >> k_ra_bcd_digit_shift) & k_ra_bcd_digit_mask);
  const uint8_t low  = (uint8_t)(bcd & k_ra_bcd_digit_mask);
  return (uint8_t)((high * k_ra_bcd_digit_base) + low);
}

static uint8_t internal_bin_to_bcd(uint8_t bin)
{
  const uint8_t high = (uint8_t)(bin / k_ra_bcd_digit_base);
  const uint8_t low  = (uint8_t)(bin % k_ra_bcd_digit_base);
  return (uint8_t)((high << k_ra_bcd_digit_shift) | low);
}

ra_err_t ra_rtc_init(void)
{
  volatile r_rtc_regs_t* rtc = ra_rtc();

  /* Stop the counter before touching it (RCR2.START = 0). */
  rtc->RCR2 = 0U;

  /* 24-hour mode (RCR2.HR24 = 1), calendar mode (RCR2.CNTMD = 0). */
  rtc->RCR2 = (uint8_t)(1U << k_ra_rcr2_bit_hr24);

  /* Disable all IRQs for now. */
  rtc->RCR1 = 0U;

  /* Restart counter. */
  rtc->RCR2 = (uint8_t)((1U << k_ra_rcr2_bit_hr24) | (1U << k_ra_rcr2_bit_start));

  ra_log_info(s_tag, "rtc_init (24h calendar)");
  return k_ra_ok;
}

ra_err_t ra_rtc_set(const ra_rtc_datetime_t* dt)
{
  RA_CHECK_NULL_PTR(dt, s_tag, "dt must not be nullptr");
  if (dt->year < k_ra_rtc_year_base) {
    return k_ra_err_invalid_arg;
  }

  volatile r_rtc_regs_t* rtc = ra_rtc();

  /* Stop counter while writing registers. */
  const uint8_t saved = rtc->RCR2;
  rtc->RCR2           = (uint8_t)(saved & (uint8_t)~(1U << k_ra_rcr2_bit_start));

  rtc->RSECCNT = internal_bin_to_bcd(dt->second);
  rtc->RMINCNT = internal_bin_to_bcd(dt->minute);
  rtc->RHRCNT  = internal_bin_to_bcd(dt->hour);
  rtc->RWKCNT  = dt->weekday;
  rtc->RDAYCNT = internal_bin_to_bcd(dt->day);
  rtc->RMONCNT = internal_bin_to_bcd(dt->month);
  rtc->RYRCNT  = (uint16_t)internal_bin_to_bcd((uint8_t)(dt->year - k_ra_rtc_year_base));

  /* Restart counter. */
  rtc->RCR2 = (uint8_t)(saved | (1U << k_ra_rcr2_bit_start));

  ra_log_info_val(s_tag, "rtc_set year", (uint32_t)dt->year);
  return k_ra_ok;
}

ra_err_t ra_rtc_get(ra_rtc_datetime_t* out)
{
  RA_CHECK_NULL_PTR(out, s_tag, "out must not be nullptr");

  volatile r_rtc_regs_t* rtc = ra_rtc();

  out->second  = internal_bcd_to_bin(rtc->RSECCNT);
  out->minute  = internal_bcd_to_bin(rtc->RMINCNT);
  out->hour    = internal_bcd_to_bin(rtc->RHRCNT);
  out->weekday = rtc->RWKCNT;
  out->day     = internal_bcd_to_bin(rtc->RDAYCNT);
  out->month   = internal_bcd_to_bin(rtc->RMONCNT);
  out->year    = (uint16_t)(k_ra_rtc_year_base + internal_bcd_to_bin((uint8_t)rtc->RYRCNT));

  return k_ra_ok;
}
