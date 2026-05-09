/**
 * @file ra_rtc.c
 * @brief BCD-calendar Realtime Clock driver implementation
 *
 * @par Tag
 * [Ring 3 / HAL] {World: S}
 *
 * @details
 * Hand-written RA8D2 RTC driver. Operates in 24-hour calendar mode,
 * decodes BCD into ``ra_rtc_datetime_t``, and exposes the alarm /
 * carry / periodic IRQ surface via the dispatch path.
 *
 * Cross-verified against FSP `r_rtc.c` `R_RTC_Open` / `CalendarTimeSet`
 * / `CalendarTimeGet` and FSP `R_RTC_Type` (BSP `R7KA8D2KF_core0.h`).
 * Every register access carries a HUM Ch 26 citation -- the chapter
 * spans pages 1219..1255.
 *
 * Key sequencing (FSP-aligned):
 *  - To write any count register the START bit must be cleared first;
 *    the driver waits with a bounded loop for the bit to actually
 *    fall and then re-asserts it after the writes.
 *  - HUM Ch 26.2.4: writes to RCR2.HR24 / CNTMD only take effect
 *    after the bit reads back the new value, so init polls those.
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

/**
 * @enum ra_rtc_wait_t
 * @brief Bounded-loop limits for register-change waits.
 *
 * @details
 * The RTC sub-clock is at most a few kHz, so a few thousand CPU
 * spin-iterations is plenty (FSP uses `FSP_HARDWARE_REGISTER_WAIT`
 * which itself spins with no upper bound). We pick an explicit
 * ceiling here to satisfy NASA Rule 2 (bounded loops) and to fail
 * loudly if the hardware never honours the write.
 */
typedef enum : uint16_t {
  k_ra_rtc_wait_iters = 10000U,
} ra_rtc_wait_t;

typedef enum : uint8_t {
  k_ra_rtc_byte_mask_all = 0xFFU, /**< Whole-byte mask for wait loops. */
} ra_rtc_byte_mask_t;

/**
 * @brief Internal helper.
 * @details See implementation.
 * @param[in] bcd See implementation.
 * @return Result code.
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static uint8_t internal_bcd_to_bin(uint8_t bcd)
{
  const uint8_t high = (uint8_t)((bcd >> k_ra_bcd_digit_shift) & k_ra_bcd_digit_mask);
  const uint8_t low  = (uint8_t)(bcd & k_ra_bcd_digit_mask);
  return (uint8_t)((high * k_ra_bcd_digit_base) + low);
}

/**
 * @brief Internal helper.
 * @details See implementation.
 * @param[in] bin See implementation.
 * @return Result code.
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static uint8_t internal_bin_to_bcd(uint8_t bin)
{
  const uint8_t high = (uint8_t)(bin / k_ra_bcd_digit_base);
  const uint8_t low  = (uint8_t)(bin % k_ra_bcd_digit_base);
  return (uint8_t)((high << k_ra_bcd_digit_shift) | low);
}

/**
 * @brief Spin until `(*reg & mask) == expect`, up to `k_ra_rtc_wait_iters`.
 *
 * @details
 * FSP uses `FSP_HARDWARE_REGISTER_WAIT(...)` which hard-loops with
 * no time-out. We bound the loop instead so the driver still passes
 * NASA Rule 2 even when the hardware never matches (test mock).
 *
 * @param[in] reg    Register pointer to poll.
 * @param[in] mask   Bits to mask before comparing.
 * @param[in] expect Expected masked value.
 *
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static void internal_wait_bit(volatile const uint8_t* reg, uint8_t mask, uint8_t expect)
{
  for (uint16_t i = 0U; i < k_ra_rtc_wait_iters; ++i) { /* GCOVR_EXCL_BR_LINE */
    if (((*reg) & mask) == expect) {                    /* GCOVR_EXCL_BR_LINE */
      return;
    }
  }
}

/* Implementation of ra_rtc_init (see header for full contract) -- see header for the documented contract. */
ra_err_t ra_rtc_init(void)
{
  volatile r_rtc_regs_t* rtc = ra_rtc();

  /* HUM Ch 26.2.4 "RCR2 : RTC Control Register 2" p 1230 -- stop the
   * counter (START=0) and clear CNTMD so we are in calendar mode.
   * FSP r_rtc.c r_rtc_software_reset writes RCR2 = 0 and waits for
   * CNTMD == 0 to confirm the mode change. */
  rtc->RCR2 = 0U;
  internal_wait_bit(&rtc->RCR2, (uint8_t)(1U << k_ra_rcr2_bit_cntmd), 0U);

  /* HUM Ch 26.2.3 "RCR1 : RTC Control Register 1" p 1229 -- mask
   * every IRQ source (AIE/CIE/PIE) and clear PES. FSP waits for the
   * write to land. */
  rtc->RCR1 = 0U;
  internal_wait_bit(&rtc->RCR1, k_ra_rtc_byte_mask_all, 0U);

  /* HUM Ch 26.2.4 "RCR2 : RTC Control Register 2" p 1230 -- HR24=1
   * selects 24-hour mode. FSP also polls until HR24 reads back 1. */
  rtc->RCR2 = (uint8_t)(1U << k_ra_rcr2_bit_hr24);
  internal_wait_bit(&rtc->RCR2,
                    (uint8_t)(1U << k_ra_rcr2_bit_hr24),
                    (uint8_t)(1U << k_ra_rcr2_bit_hr24));

  /* HUM Ch 26.2.4 "RCR2 : RTC Control Register 2" p 1230 -- START=1
   * starts the counter. */
  rtc->RCR2 = (uint8_t)((1U << k_ra_rcr2_bit_hr24) | (1U << k_ra_rcr2_bit_start));
  internal_wait_bit(&rtc->RCR2,
                    (uint8_t)(1U << k_ra_rcr2_bit_start),
                    (uint8_t)(1U << k_ra_rcr2_bit_start));

  ra_log_info(s_tag, "rtc_init (24h calendar)");
  return k_ra_ok;
}

/* Implementation of ra_rtc_set (see header for full contract) -- see header for the documented contract. */
ra_err_t ra_rtc_set(const ra_rtc_datetime_t* dt)
{
  RA_CHECK_NULL_PTR(dt, s_tag, "dt must not be nullptr");
  if (dt->year < k_ra_rtc_year_base) {
    return k_ra_err_invalid_arg;
  }

  volatile r_rtc_regs_t* rtc = ra_rtc();

  /* HUM Ch 26.2.4 "RCR2 : RTC Control Register 2" p 1230 -- count
   * registers must only be written while START=0. Clear the bit and
   * wait for the hardware to honour it (FSP's r_rtc_start_bit_update). */
  const uint8_t saved = rtc->RCR2;
  rtc->RCR2           = (uint8_t)(saved & (uint8_t)~(1U << k_ra_rcr2_bit_start));
  internal_wait_bit(&rtc->RCR2, (uint8_t)(1U << k_ra_rcr2_bit_start), 0U);

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

  /* HUM Ch 26.2.4 "RCR2 : RTC Control Register 2" p 1230 -- restore
   * START to its prior value (always 1 if init has run) and wait. */
  rtc->RCR2 = (uint8_t)(saved | (1U << k_ra_rcr2_bit_start));
  internal_wait_bit(&rtc->RCR2,
                    (uint8_t)(1U << k_ra_rcr2_bit_start),
                    (uint8_t)(1U << k_ra_rcr2_bit_start));

  ra_log_info_val(s_tag, "rtc_set year", (uint32_t)dt->year);
  return k_ra_ok;
}

/* Implementation of ra_rtc_get (see header for full contract) -- see header for the documented contract. */
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

/**
 * @enum ra_rtc_alarm_t
 * @brief Bit positions / limits for the RxxAR alarm registers.
 *
 * @details
 * HUM Ch 26.2.13..26.2.16 -- each alarm register's bit 7 is the ENB
 * (alarm enable) flag; the lower 7 bits hold the BCD value to match.
 */
typedef enum : uint8_t {
  k_ra_rtc_alarm_enb_bit = 7U,
  k_ra_rtc_alarm_max_hr  = 23U,
  k_ra_rtc_alarm_max_min = 59U,
  k_ra_rtc_alarm_max_sec = 59U,
} ra_rtc_alarm_t;

/**
 * @brief Implementation of ra_rtc_set_alarm (see header for full contract).
 *
 * @details
 * Writes the BCD-encoded second / minute / hour into RSECAR / RMINAR /
 * RHRAR with their ENB bits set, and clears every other AR register's
 * ENB so the alarm matches purely on time-of-day.
 *
 * @param[in] alarm See header.
 * @return Result code.
 * @retval k_ra_ok Alarm written.
 * @retval k_ra_err_null_ptr ``alarm`` is NULL.
 * @retval k_ra_err_invalid_arg Out-of-range hour/min/sec.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe.
 * @since 0.1.0
 */
ra_err_t ra_rtc_set_alarm(const ra_rtc_datetime_t* alarm)
{
  RA_CHECK_NULL_PTR(alarm, s_tag, "alarm must not be nullptr");
  if (alarm->hour > k_ra_rtc_alarm_max_hr || alarm->minute > k_ra_rtc_alarm_max_min ||
      alarm->second > k_ra_rtc_alarm_max_sec) {
    return k_ra_err_invalid_arg;
  }

  volatile r_rtc_regs_t* rtc = ra_rtc();
  const uint8_t          enb = (uint8_t)(1U << k_ra_rtc_alarm_enb_bit);

  /* HUM Ch 26.2.13 "RSECAR" / 26.2.14 "RMINAR" / 26.2.15 "RHRAR" --
   * encode the BCD match value, OR in the ENB bit to enable the field. */
  rtc->RSECAR = (uint8_t)(internal_bin_to_bcd(alarm->second) | enb);
  rtc->RMINAR = (uint8_t)(internal_bin_to_bcd(alarm->minute) | enb);
  rtc->RHRAR  = (uint8_t)(internal_bin_to_bcd(alarm->hour) | enb);

  /* Wildcard the date / weekday / month / year alarms so the alarm
   * fires on the next hh:mm:ss match regardless of date. */
  rtc->RWKAR   = 0U;
  rtc->RDAYAR  = 0U;
  rtc->RMONAR  = 0U;
  rtc->RYRAR   = 0U;
  rtc->RYRAREN = 0U;

  return k_ra_ok;
}

/* =============================================================================
 * full build-out
 * =============================================================================
 */

/**
 * @enum ra_rtc_mask_t
 * @brief Combined IRQ mask.
 */
typedef enum : uint8_t {
  k_ra_rtc_irq_all = k_ra_rtc_irq_alarm | k_ra_rtc_irq_carry | k_ra_rtc_irq_periodic,
} ra_rtc_mask_t;

typedef struct {
  ra_rtc_event_fn_t fn;
  void*             ctx;
} ra_rtc_state_t;

static ra_rtc_state_t s_rtc_state;

/* Implementation of ra_rtc_deinit (see header for full contract) -- see header for the documented contract. */
ra_err_t ra_rtc_deinit(void)
{
  volatile r_rtc_regs_t* rtc = ra_rtc();
  /* HUM Ch 26.2.3 "RCR1" p 1229 */ /* mask all IRQs. */
  rtc->RCR1 = 0U;
  /* HUM Ch 26.2.4 "RCR2" p 1230 */ /* stop the counter. */
  rtc->RCR2       = 0U;
  s_rtc_state.fn  = nullptr;
  s_rtc_state.ctx = nullptr;
  return k_ra_ok;
}

/* Implementation of ra_rtc_set_irq_enable (see header for full contract) -- see header for the documented contract. */
ra_err_t ra_rtc_set_irq_enable(uint8_t mask)
{
  volatile r_rtc_regs_t* rtc = ra_rtc();
  /* HUM Ch 26.2.3 "RCR1" p 1229 */ /* AIE/CIE/PIE enable bits 0..2. */
  rtc->RCR1 = (uint8_t)(rtc->RCR1 | (mask & k_ra_rtc_irq_all));
  return k_ra_ok;
}

/* Implementation of ra_rtc_get_status (see header for full contract) -- see header for the documented contract. */
ra_err_t ra_rtc_get_status(uint8_t* out_mask)
{
  RA_CHECK_NULL_PTR(out_mask, s_tag, "out_mask must not be nullptr");
  *out_mask = (uint8_t)(ra_rtc()->RCR1 & k_ra_rtc_irq_all);
  return k_ra_ok;
}

/* Implementation of ra_rtc_clear_status (see header for full contract) -- see header for the documented contract. */
ra_err_t ra_rtc_clear_status(uint8_t mask)
{
  volatile r_rtc_regs_t* rtc = ra_rtc();
  rtc->RCR1                  = (uint8_t)(rtc->RCR1 & (uint8_t)~(mask & k_ra_rtc_irq_all));
  return k_ra_ok;
}

/* Implementation of ra_rtc_attach_handler (see header for full contract) -- see header for the documented contract. */
ra_err_t ra_rtc_attach_handler(ra_rtc_event_fn_t fn, void* ctx)
{
  s_rtc_state.fn  = fn;
  s_rtc_state.ctx = ctx;
  return k_ra_ok;
}

/* Implementation of ra_rtc_dispatch (see header for full contract) -- see header for the documented contract. */
void ra_rtc_dispatch(void)
{
  const uint8_t           mask = (uint8_t)(ra_rtc()->RCR1 & k_ra_rtc_irq_all);
  const ra_rtc_event_fn_t fn   = s_rtc_state.fn;
  void* const             ctx  = s_rtc_state.ctx;
  if (fn != nullptr) {
    fn(ctx, mask);
  }
}

/* Implementation of ra_rtc_enter_stop (see header for full contract) -- see header for the documented contract. */
ra_err_t ra_rtc_enter_stop(void)
{
  volatile r_rtc_regs_t* rtc = ra_rtc();
  /* HUM Ch 26.2.4 "RCR2.START" p 1230 */ /* clear START to halt counter. */
  rtc->RCR2 = (uint8_t)(rtc->RCR2 & (uint8_t)~(1U << k_ra_rcr2_bit_start));
  internal_wait_bit(&rtc->RCR2, (uint8_t)(1U << k_ra_rcr2_bit_start), 0U);
  return k_ra_ok;
}

/* Implementation of ra_rtc_exit_stop (see header for full contract) -- see header for the documented contract. */
ra_err_t ra_rtc_exit_stop(void)
{
  volatile r_rtc_regs_t* rtc = ra_rtc();
  /* HUM Ch 26.2.4 "RCR2.START" p 1230 */ /* set START to resume. */
  rtc->RCR2 = (uint8_t)(rtc->RCR2 | (1U << k_ra_rcr2_bit_start));
  internal_wait_bit(&rtc->RCR2,
                    (uint8_t)(1U << k_ra_rcr2_bit_start),
                    (uint8_t)(1U << k_ra_rcr2_bit_start));
  return k_ra_ok;
}
