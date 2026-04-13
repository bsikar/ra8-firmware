/**
 * @file ra_rtc.c
 * @brief BCD-calendar Realtime Clock driver implementation
 *
 * @par Tag
 * [Ring 3 / HAL] {World: S}
 *
 * @details
 * Wave 4 build-out of the RA8D2 RTC. Operates in 24-hour
 * calendar mode, decodes BCD into ``ra_rtc_datetime_t``, and
 * exposes the alarm / carry / periodic IRQ surface via the
 * dispatch path. Every register write below carries a HUM Ch 26
 * citation.
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

  /* HUM Ch 26.2.4 "RCR2 : RTC Control Register 2" p 1230 -- stop
   * the counter before touching anything else. */
  rtc->RCR2 = 0U;

  /* HUM Ch 26.2.4 "RCR2 : RTC Control Register 2" p 1230 */
  /* RCR2.HR24 = 1 selects 24-hour mode; CNTMD = 0 keeps calendar. */
  rtc->RCR2 = (uint8_t)(1U << k_ra_rcr2_bit_hr24);

  /* HUM Ch 26.2.3 "RCR1 : RTC Control Register 1" p 1229 */
  /* Mask every IRQ source until the application opts in. */
  rtc->RCR1 = 0U;

  /* HUM Ch 26.2.4 "RCR2 : RTC Control Register 2" p 1230 */
  /* START = 1 restarts the counter. */
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

  /* HUM Ch 26.2.4 "RCR2 : RTC Control Register 2" p 1230 */
  /* Stop counter while writing the calendar registers (START=0). */
  const uint8_t saved = rtc->RCR2;
  rtc->RCR2           = (uint8_t)(saved & (uint8_t)~(1U << k_ra_rcr2_bit_start));

  /* HUM Ch 26.2.5 "RSECCNT : Second Counter" p 1232 */
  rtc->RSECCNT = internal_bin_to_bcd(dt->second);
  /* HUM Ch 26.2.6 "RMINCNT : Minute Counter" p 1232 */
  rtc->RMINCNT = internal_bin_to_bcd(dt->minute);
  /* HUM Ch 26.2.7 "RHRCNT : Hour Counter" p 1233 */
  rtc->RHRCNT = internal_bin_to_bcd(dt->hour);
  /* HUM Ch 26.2.8 "RWKCNT : Day-of-Week Counter" p 1234 */
  rtc->RWKCNT = dt->weekday;
  /* HUM Ch 26.2.9 "RDAYCNT : Day Counter" p 1234 */
  rtc->RDAYCNT = internal_bin_to_bcd(dt->day);
  /* HUM Ch 26.2.10 "RMONCNT : Month Counter" p 1235 */
  rtc->RMONCNT = internal_bin_to_bcd(dt->month);
  /* HUM Ch 26.2.11 "RYRCNT : Year Counter" p 1235 */
  rtc->RYRCNT = (uint16_t)internal_bin_to_bcd((uint8_t)(dt->year - k_ra_rtc_year_base));

  /* HUM Ch 26.2.4 "RCR2 : RTC Control Register 2" p 1230 */
  /* START = 1 restarts the counter. */
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

/* =============================================================================
 * Wave 4.3 -- full build-out
 * =============================================================================
 */

/**
 * @enum ra_rtc_mask_t
 * @brief Combined IRQ mask.
 */
typedef enum : uint8_t {
  k_ra_rtc_irq_all =
    (uint8_t)k_ra_rtc_irq_alarm | (uint8_t)k_ra_rtc_irq_carry | (uint8_t)k_ra_rtc_irq_periodic,
} ra_rtc_mask_t;

typedef struct {
  ra_rtc_event_fn_t fn;
  void*             ctx;
} ra_rtc_state_t;

static ra_rtc_state_t s_rtc_state;

ra_err_t ra_rtc_deinit(void)
{
  volatile r_rtc_regs_t* rtc = ra_rtc();
  rtc->RCR1                  = 0U;
  rtc->RCR2                  = 0U;
  s_rtc_state.fn             = nullptr;
  s_rtc_state.ctx            = nullptr;
  return k_ra_ok;
}

ra_err_t ra_rtc_set_irq_enable(uint8_t mask)
{
  volatile r_rtc_regs_t* rtc = ra_rtc();
  rtc->RCR1                  = (uint8_t)(rtc->RCR1 | (mask & (uint8_t)k_ra_rtc_irq_all));
  return k_ra_ok;
}

ra_err_t ra_rtc_get_status(uint8_t* out_mask)
{
  RA_CHECK_NULL_PTR(out_mask, s_tag, "out_mask must not be nullptr");
  *out_mask = (uint8_t)(ra_rtc()->RCR1 & (uint8_t)k_ra_rtc_irq_all);
  return k_ra_ok;
}

ra_err_t ra_rtc_clear_status(uint8_t mask)
{
  volatile r_rtc_regs_t* rtc = ra_rtc();
  rtc->RCR1                  = (uint8_t)(rtc->RCR1 & (uint8_t)~(mask & (uint8_t)k_ra_rtc_irq_all));
  return k_ra_ok;
}

ra_err_t ra_rtc_attach_handler(ra_rtc_event_fn_t fn, void* ctx)
{
  s_rtc_state.fn  = fn;
  s_rtc_state.ctx = ctx;
  return k_ra_ok;
}

void ra_rtc_dispatch(void)
{
  const uint8_t           mask = (uint8_t)(ra_rtc()->RCR1 & (uint8_t)k_ra_rtc_irq_all);
  const ra_rtc_event_fn_t fn   = s_rtc_state.fn;
  void* const             ctx  = s_rtc_state.ctx;
  if (fn != nullptr) {
    fn(ctx, mask);
  }
}

ra_err_t ra_rtc_enter_stop(void)
{
  volatile r_rtc_regs_t* rtc = ra_rtc();
  rtc->RCR2                  = (uint8_t)(rtc->RCR2 & (uint8_t)~(1U << k_ra_rcr2_bit_start));
  return k_ra_ok;
}

ra_err_t ra_rtc_exit_stop(void)
{
  volatile r_rtc_regs_t* rtc = ra_rtc();
  rtc->RCR2                  = (uint8_t)(rtc->RCR2 | (1U << k_ra_rcr2_bit_start));
  return k_ra_ok;
}
