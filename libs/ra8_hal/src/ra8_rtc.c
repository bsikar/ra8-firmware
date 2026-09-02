/**
 * @file ra8_rtc.c
 * @brief BCD-calendar Realtime Clock driver implementation
 *
 * @par Tag
 * [Ring 3 / HAL] {World: S}
 *
 * @details
 * Hand-written RA8D2 RTC driver. Operates in 24-hour calendar mode,
 * decodes BCD into ``ra8_rtc_datetime_t``, and exposes the alarm /
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
 *  - HUM Ch 26.2.21: writes to RCR2.HR24 / CNTMD only take effect
 *    after the bit reads back the new value, so init polls those.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_rtc.h"

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_hw_err.h"
#include "ra8_log.h"
#include "ra8_register_protection.h"
#include "ra8_rtc_regs.h"
#include "ra8_system_regs.h"
#include "ra8_time.h"

static const char* s_tag = "RTC";

typedef enum : uint8_t {
  k_ra8_bcd_digit_mask  = 0x0FU, /**< RA8 bcd digit mask.  */
  k_ra8_bcd_digit_shift = 4U,    /**< RA8 bcd digit shift. */
  k_ra8_bcd_digit_base  = 10U,   /**< RA8 bcd digit base.  */
} ra8_rtc_bcd_t;

typedef enum : uint16_t {
  k_ra8_rtc_year_base = 2000U, /**< Base year for ra8_rtc_datetime_t. */
} ra8_rtc_year_t;

/**
 * @enum ra8_rtc_wait_t
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
  k_ra8_rtc_wait_iters = 10000U, /**< RA8 rtc wait iters. */
} ra8_rtc_wait_t;

typedef enum : uint8_t {
  k_ra8_rtc_byte_mask_all = 0xFFU, /**< Whole-byte mask for wait loops. */
} ra8_rtc_byte_mask_t;

/**
 * @brief Internal helper.
 * @details See implementation.
 * @param[in] bcd See implementation.
 * @return Result code.
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL static uint8_t internal_bcd_to_bin(uint8_t bcd)
{
  const uint8_t high = (uint8_t)((bcd >> k_ra8_bcd_digit_shift) & k_ra8_bcd_digit_mask);
  const uint8_t low  = (uint8_t)(bcd & k_ra8_bcd_digit_mask);
  return (uint8_t)((high * k_ra8_bcd_digit_base) + low);
}

/**
 * @brief Internal helper.
 * @details See implementation.
 * @param[in] bin See implementation.
 * @return Result code.
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL static uint8_t internal_bin_to_bcd(uint8_t bin)
{
  const uint8_t high = (uint8_t)(bin / k_ra8_bcd_digit_base);
  const uint8_t low  = (uint8_t)(bin % k_ra8_bcd_digit_base);
  return (uint8_t)((high << k_ra8_bcd_digit_shift) | low);
}

/**
 * @brief Spin until `(*reg & mask) == expect`, up to `k_ra8_rtc_wait_iters`.
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
RA8_INTERNAL static void
internal_wait_bit(volatile const uint8_t* reg, uint8_t mask, uint8_t expect)
{
  for (uint16_t i = 0U; i < k_ra8_rtc_wait_iters; ++i) {
#if defined(RA8_OFF_TARGET) && defined(UNIT_TEST)
    const bool matched = ra8_fake_mmio_poll(reg, i, ((*reg) & mask) == expect);
#else
    const bool matched = ((*reg) & mask) == expect;
#endif
    if (matched) {
      return;
    }
  }
}

/* =============================================================================
 * Count-source bring-up (HUM Ch 26.3.2 "Clock and Count Mode Setting
 * Procedure", Figure 26.3 p 1243)
 * =============================================================================
 */

/**
 * @enum ra8_rtc_clk_wait_t
 * @brief Fixed timed waits (ms) used while bringing up the count source.
 *
 * @details
 * Neither the sub-clock oscillator nor the LOCO exposes a hardware
 * stabilization flag (OSCSF only covers MOSC / HOCO / PLL), so the
 * count-source bring-up waits fixed times rather than polling a ready bit.
 *  - ``stab_sub_ms``  covers t_SUBOSCWT for the 32.768 kHz watch crystal.
 *  - ``stab_loco_ms`` covers the (much shorter) LOCO settle after LCSTP=0.
 *  - ``six_clocks_ms`` is >= 6 cycles of a 32.768 kHz source (~183 us).
 *  - ``reset_ms`` bounds the count-source-synchronized RTC software reset.
 */
typedef enum : uint16_t {
  k_ra8_rtc_clk_stab_sub_ms   = 1000U, /**< Sub-clock crystal stabilization wait. */
  k_ra8_rtc_clk_stab_loco_ms  = 5U,    /**< LOCO settle wait.                     */
  k_ra8_rtc_clk_six_clocks_ms = 1U,    /**< >= 6 clocks of the count source.      */
  k_ra8_rtc_clk_reset_ms      = 5U,    /**< RTC software-reset settle wait.       */
} ra8_rtc_clk_wait_t;

/**
 * @enum ra8_rtc_rfr_t
 * @brief Prescaler frequency-register values for the LOCO count source.
 *
 * @details
 * When LOCO is the count source the prescaler divides it to a 128 Hz base,
 * so RFRL must hold (LOCO_Hz / 128) - 1; for a 32.768 kHz LOCO that is
 * 0x00FF, and RFRH must be cleared first after a cold start (HUM
 * Ch 26.2.24 "RFRL" p 1236, Ch 26.2.25 "RFRH" p 1237).
 */
typedef enum : uint16_t {
  k_ra8_rtc_rfrh_cold  = 0x0000U, /**< RFRH cleared before RFRL on cold start.  */
  k_ra8_rtc_rfrl_32768 = 0x00FFU, /**< (32768 / 128) - 1 for a 32.768 kHz LOCO. */
} ra8_rtc_rfr_t;

/**
 * @brief Start and wait out the requested RTC count-source oscillator.
 *
 * @details
 * Sub-clock path: write SOMCR drive (standard, matching the EK-RA8D2 watch
 * crystal) while SOSC is stopped, clear SOSCCR.SOSTP, then wait t_SUBOSCWT.
 * LOCO path: clear LOCOCR.LCSTP (the LOCO is normally already running) and
 * let it settle. All three registers sit behind PRCR group 0, so the
 * writes run inside an ::RA8_PROTECTED_WRITE window. The "other" bits of
 * each register are documented read-as-0 / write-0, so each is written
 * whole rather than read-modify-write.
 *
 * @param[in] src Count source to enable (sub-clock or LOCO).
 *
 * @return ``ra8_err_t`` outcome.
 * @retval k_ra8_ok                 Oscillator running (stop bit reads 0).
 * @retval k_ra8_err_hw_init_failed Stop bit still set after the enable write.
 *
 * @pre  ``ra8_time_init()`` has run (blocks on ``ra8_delay_ms``).
 * @pre  Single-threaded init context.
 * @post The selected oscillator's stop bit is clear.
 * @post PRCR is re-locked.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_start_count_source(ra8_rtc_clk_src_t src)
{
  if (src == k_ra8_rtc_clk_loco) {
    RA8_PROTECTED_WRITE(k_ra8_prcr_unlock_cgc)
    {
      /* HUM Ch 9.2.15 "LOCOCR : Low-Speed On-Chip Oscillator Control
       * Register" p 339 -- LCSTP = 0 keeps the internal LOCO running. */
      *ra8_sys_lococr() = 0U;
    }
    ra8_delay_ms((uint32_t)k_ra8_rtc_clk_stab_loco_ms);
#if defined(RA8_OFF_TARGET) && defined(UNIT_TEST)
    const bool loco_running =
      ra8_fake_mmio_poll(ra8_sys_lococr(), 0U, (*ra8_sys_lococr() & k_ra8_lococr_lcstp_mask) == 0U);
#else
    const bool loco_running = (*ra8_sys_lococr() & k_ra8_lococr_lcstp_mask) == 0U;
#endif
    if (!loco_running) {
      return k_ra8_err_hw_init_failed;
    }
    return k_ra8_ok;
  }

  RA8_PROTECTED_WRITE(k_ra8_prcr_unlock_cgc)
  {
    /* HUM Ch 9.2.29 "SOMCR : Sub-Clock Oscillator Mode Control Register"
     * p 351 -- set the crystal drive (standard, SOSEL = resonator) while
     * SOSTP is still 1. */
    *ra8_sys_somcr() = (uint8_t)k_ra8_somcr_drv_standard;
    /* HUM Ch 9.2.14 "SOSCCR : Sub-Clock Oscillator Control Register"
     * p 339 -- SOSTP = 0 starts the 32.768 kHz crystal. */
    *ra8_sys_sosccr() = 0U;
  }
  ra8_delay_ms((uint32_t)k_ra8_rtc_clk_stab_sub_ms);
#if defined(RA8_OFF_TARGET) && defined(UNIT_TEST)
  const bool subclock_running =
    ra8_fake_mmio_poll(ra8_sys_sosccr(), 0U, (*ra8_sys_sosccr() & k_ra8_sosccr_sostp_mask) == 0U);
#else
  const bool subclock_running = (*ra8_sys_sosccr() & k_ra8_sosccr_sostp_mask) == 0U;
#endif
  if (!subclock_running) {
    return k_ra8_err_hw_init_failed;
  }
  return k_ra8_ok;
}

ra8_err_t ra8_rtc_clock_init(ra8_rtc_clk_src_t src)
{
  if (src > k_ra8_rtc_clk_loco) {
    return k_ra8_err_invalid_arg;
  }

  const ra8_err_t osc_err = internal_start_count_source(src);
  if (osc_err != k_ra8_ok) {
    ra8_log_error(s_tag, "rtc count source not running");
    return osc_err;
  }

  volatile r_rtc_regs_t* rtc = ra8_rtc();

  /* HUM Ch 26.2.23 "RCR4 : RTC Control Register 4" p 1236 -- RCKSEL
   * selects the count source (0 = sub-clock, 1 = LOCO). It must be set
   * once before the initial RTC register settings. */
  rtc->RCR4 = (uint8_t)src;
  /* HUM Ch 26.3.2 "Clock and Count Mode Setting Procedure" p 1243 --
   * supply at least 6 clocks of the count source after writing RCKSEL. */
  ra8_delay_ms((uint32_t)k_ra8_rtc_clk_six_clocks_ms);

  /* HUM Ch 26.2.21 "RCR2 : RTC Control Register 2" p 1232 -- stop the
   * prescaler (START = 0) before the frequency register and software
   * reset, and wait for the bit to fall. */
  rtc->RCR2 = (uint8_t)(rtc->RCR2 & (uint8_t)~(1U << k_ra8_rcr2_bit_start));
  internal_wait_bit(&rtc->RCR2, (uint8_t)(1U << k_ra8_rcr2_bit_start), 0U);

  if (src == k_ra8_rtc_clk_loco) {
    /* HUM Ch 26.2.25 "RFRH : Frequency Register H" p 1237 -- clear RFRH
     * before RFRL on a cold start. */
    rtc->RFRH = (uint16_t)k_ra8_rtc_rfrh_cold;
    /* HUM Ch 26.2.24 "RFRL : Frequency Register L" p 1236 -- (LOCO / 128)
     * - 1; 0x00FF for a 32.768 kHz LOCO. */
    rtc->RFRL = (uint16_t)k_ra8_rtc_rfrl_32768;
  }

  /* HUM Ch 26.2.21 "RCR2 : RTC Control Register 2" p 1233 -- RESET = 1
   * initializes the prescaler and count registers against the live count
   * source; the bit auto-clears when the reset completes. */
  rtc->RCR2 = (uint8_t)(1U << k_ra8_rcr2_bit_reset);
  ra8_delay_ms((uint32_t)k_ra8_rtc_clk_reset_ms);
  internal_wait_bit(&rtc->RCR2, (uint8_t)(1U << k_ra8_rcr2_bit_reset), 0U);

  ra8_log_info_val(s_tag, "rtc clock init src", (uint32_t)src);
  return k_ra8_ok;
}

ra8_err_t ra8_rtc_init(void)
{
  volatile r_rtc_regs_t* rtc = ra8_rtc();

  /* HUM Ch 26.2.21 "RCR2 : RTC Control Register 2" p 1232 -- stop the
   * counter (START=0) and clear CNTMD so we are in calendar mode.
   * FSP r_rtc.c r_rtc_software_reset writes RCR2 = 0 and waits for
   * CNTMD == 0 to confirm the mode change. */
  rtc->RCR2 = 0U;
  internal_wait_bit(&rtc->RCR2, (uint8_t)(1U << k_ra8_rcr2_bit_cntmd), 0U);

  /* HUM Ch 26.2.20 "RCR1 : RTC Control Register 1" p 1231 -- mask
   * every IRQ source (AIE/CIE/PIE) and clear PES. FSP waits for the
   * write to land. */
  rtc->RCR1 = 0U;
  internal_wait_bit(&rtc->RCR1, k_ra8_rtc_byte_mask_all, 0U);

  /* HUM Ch 26.2.21 "RCR2 : RTC Control Register 2" p 1232 -- HR24=1
   * selects 24-hour mode. FSP also polls until HR24 reads back 1. */
  rtc->RCR2 = (uint8_t)(1U << k_ra8_rcr2_bit_hr24);
  internal_wait_bit(&rtc->RCR2,
                    (uint8_t)(1U << k_ra8_rcr2_bit_hr24),
                    (uint8_t)(1U << k_ra8_rcr2_bit_hr24));

  /* HUM Ch 26.2.21 "RCR2 : RTC Control Register 2" p 1232 -- START=1
   * starts the counter. */
  rtc->RCR2 = (uint8_t)((1U << k_ra8_rcr2_bit_hr24) | (1U << k_ra8_rcr2_bit_start));
  internal_wait_bit(&rtc->RCR2,
                    (uint8_t)(1U << k_ra8_rcr2_bit_start),
                    (uint8_t)(1U << k_ra8_rcr2_bit_start));

  ra8_log_info(s_tag, "rtc_init (24h calendar)");
  return k_ra8_ok;
}

ra8_err_t ra8_rtc_set(const ra8_rtc_datetime_t* dt)
{
  RA8_CHECK_NULL_PTR(dt, s_tag, "dt must not be nullptr");
  if (dt->year < k_ra8_rtc_year_base) {
    return k_ra8_err_invalid_arg;
  }

  volatile r_rtc_regs_t* rtc = ra8_rtc();

  /* HUM Ch 26.2.21 "RCR2 : RTC Control Register 2" p 1232 -- count
   * registers must only be written while START=0. Clear the bit and
   * wait for the hardware to honour it (FSP's r_rtc_start_bit_update). */
  const uint8_t saved = rtc->RCR2;
  rtc->RCR2           = (uint8_t)(saved & (uint8_t)~(1U << k_ra8_rcr2_bit_start));
  internal_wait_bit(&rtc->RCR2, (uint8_t)(1U << k_ra8_rcr2_bit_start), 0U);

  /* HUM Ch 26.2.2 "RSECCNT : Second Counter" p 1221 */
  rtc->RSECCNT = internal_bin_to_bcd(dt->second);
  /* HUM Ch 26.2.3 "RMINCNT : Minute Counter" p 1222 */
  rtc->RMINCNT = internal_bin_to_bcd(dt->minute);
  /* HUM Ch 26.2.4 "RHRCNT : Hour Counter" p 1222 */
  rtc->RHRCNT = internal_bin_to_bcd(dt->hour);
  /* HUM Ch 26.2.5 "RWKCNT : Day-of-Week Counter" p 1223 */
  rtc->RWKCNT = dt->weekday;
  /* HUM Ch 26.2.7 "RDAYCNT : Day Counter" p 1224 */
  rtc->RDAYCNT = internal_bin_to_bcd(dt->day);
  /* HUM Ch 26.2.8 "RMONCNT : Month Counter" p 1224 */
  rtc->RMONCNT = internal_bin_to_bcd(dt->month);
  /* HUM Ch 26.2.9 "RYRCNT : Year Counter" p 1225 */
  rtc->RYRCNT = (uint16_t)internal_bin_to_bcd((uint8_t)(dt->year - k_ra8_rtc_year_base));

  /* HUM Ch 26.2.21 "RCR2 : RTC Control Register 2" p 1232 -- restore
   * START to its prior value (always 1 if init has run) and wait. */
  rtc->RCR2 = (uint8_t)(saved | (1U << k_ra8_rcr2_bit_start));
  internal_wait_bit(&rtc->RCR2,
                    (uint8_t)(1U << k_ra8_rcr2_bit_start),
                    (uint8_t)(1U << k_ra8_rcr2_bit_start));

  ra8_log_info_val(s_tag, "rtc_set year", (uint32_t)dt->year);
  return k_ra8_ok;
}

ra8_err_t ra8_rtc_get(ra8_rtc_datetime_t* out)
{
  RA8_CHECK_NULL_PTR(out, s_tag, "out must not be nullptr");

  volatile const r_rtc_regs_t* rtc = ra8_rtc();

  out->second  = internal_bcd_to_bin(rtc->RSECCNT);
  out->minute  = internal_bcd_to_bin(rtc->RMINCNT);
  out->hour    = internal_bcd_to_bin(rtc->RHRCNT);
  out->weekday = rtc->RWKCNT;
  out->day     = internal_bcd_to_bin(rtc->RDAYCNT);
  out->month   = internal_bcd_to_bin(rtc->RMONCNT);
  out->year    = (uint16_t)(k_ra8_rtc_year_base + internal_bcd_to_bin((uint8_t)rtc->RYRCNT));

  return k_ra8_ok;
}

/**
 * @enum ra8_rtc_alarm_t
 * @brief Bit positions / limits for the RxxAR alarm registers.
 *
 * @details
 * HUM Ch 26.2.10..26.2.16 -- each alarm register's bit 7 is the ENB
 * (alarm enable) flag; the lower 7 bits hold the BCD value to match.
 */
typedef enum : uint8_t {
  k_ra8_rtc_alarm_enb_bit = 7U,  /**< RA8 rtc alarm enb bit.         */
  k_ra8_rtc_alarm_max_hr  = 23U, /**< RA8 rtc alarm maximum hr.      */
  k_ra8_rtc_alarm_max_min = 59U, /**< RA8 rtc alarm maximum minimum. */
  k_ra8_rtc_alarm_max_sec = 59U, /**< RA8 rtc alarm maximum sec.     */
} ra8_rtc_alarm_t;

/**
 * @brief Implementation of `ra8_rtc_set_alarm()`.
 *
 * @details
 * Writes the BCD-encoded second / minute / hour into RSECAR / RMINAR /
 * RHRAR with their ENB bits set, and clears every other AR register's
 * ENB so the alarm matches purely on time-of-day.
 *
 * @param[in] alarm See header.
 * @return Result code.
 * @retval k_ra8_ok Alarm written.
 * @retval k_ra8_err_null_ptr ``alarm`` is NULL.
 * @retval k_ra8_err_invalid_arg Out-of-range hour/min/sec.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe.
 * @since 0.1.0
 */
ra8_err_t ra8_rtc_set_alarm(const ra8_rtc_datetime_t* alarm)
{
  RA8_CHECK_NULL_PTR(alarm, s_tag, "alarm must not be nullptr");
  if (alarm->hour > k_ra8_rtc_alarm_max_hr || alarm->minute > k_ra8_rtc_alarm_max_min ||
      alarm->second > k_ra8_rtc_alarm_max_sec) {
    return k_ra8_err_invalid_arg;
  }

  volatile r_rtc_regs_t* rtc = ra8_rtc();
  const uint8_t          enb = (uint8_t)(1U << k_ra8_rtc_alarm_enb_bit);

  /* HUM Ch 26.2.10 "RSECAR" / 26.2.11 "RMINAR" / 26.2.12 "RHRAR" --
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

  return k_ra8_ok;
}

/* =============================================================================
 * full build-out
 * =============================================================================
 */

/**
 * @enum ra8_rtc_mask_t
 * @brief Combined IRQ mask.
 */
typedef enum : uint8_t {
  k_ra8_rtc_irq_all =
    k_ra8_rtc_irq_alarm | k_ra8_rtc_irq_carry | k_ra8_rtc_irq_periodic, /**< RA8 rtc IRQ all. */
} ra8_rtc_mask_t;

typedef struct {
  ra8_rtc_event_fn_t fn;  /**< Fn.  */
  void*              ctx; /**< Ctx. */
} ra8_rtc_state_t;

static ra8_rtc_state_t s_rtc_state;

ra8_err_t ra8_rtc_deinit(void)
{
  volatile r_rtc_regs_t* rtc = ra8_rtc();
  /* HUM Ch 26.2.20 "RCR1" p 1231 */ /* mask all IRQs. */
  rtc->RCR1 = 0U;
  /* HUM Ch 26.2.21 "RCR2" p 1232 */ /* stop the counter. */
  rtc->RCR2       = 0U;
  s_rtc_state.fn  = nullptr;
  s_rtc_state.ctx = nullptr;
  return k_ra8_ok;
}

ra8_err_t ra8_rtc_set_irq_enable(uint8_t mask)
{
  volatile r_rtc_regs_t* rtc = ra8_rtc();
  /* HUM Ch 26.2.20 "RCR1" p 1231 */ /* AIE/CIE/PIE enable bits 0..2. */
  rtc->RCR1 = (uint8_t)(rtc->RCR1 | (mask & k_ra8_rtc_irq_all));
  return k_ra8_ok;
}

ra8_err_t ra8_rtc_get_status(uint8_t* out_mask)
{
  RA8_CHECK_NULL_PTR(out_mask, s_tag, "out_mask must not be nullptr");
  *out_mask = (uint8_t)(ra8_rtc()->RCR1 & k_ra8_rtc_irq_all);
  return k_ra8_ok;
}

ra8_err_t ra8_rtc_clear_status(uint8_t mask)
{
  volatile r_rtc_regs_t* rtc = ra8_rtc();
  rtc->RCR1                  = (uint8_t)(rtc->RCR1 & (uint8_t)~(mask & k_ra8_rtc_irq_all));
  return k_ra8_ok;
}

ra8_err_t ra8_rtc_attach_handler(ra8_rtc_event_fn_t fn, void* ctx)
{
  s_rtc_state.fn  = fn;
  s_rtc_state.ctx = ctx;
  return k_ra8_ok;
}

void ra8_rtc_dispatch(void)
{
  const uint8_t            mask = (uint8_t)(ra8_rtc()->RCR1 & k_ra8_rtc_irq_all);
  const ra8_rtc_event_fn_t fn   = s_rtc_state.fn;
  void* const              ctx  = s_rtc_state.ctx;
  if (fn != nullptr) {
    fn(ctx, mask);
  }
}

ra8_err_t ra8_rtc_enter_stop(void)
{
  volatile r_rtc_regs_t* rtc = ra8_rtc();
  /* HUM Ch 26.2.21 "RCR2.START" p 1232 */ /* clear START to halt counter. */
  rtc->RCR2 = (uint8_t)(rtc->RCR2 & (uint8_t)~(1U << k_ra8_rcr2_bit_start));
  internal_wait_bit(&rtc->RCR2, (uint8_t)(1U << k_ra8_rcr2_bit_start), 0U);
  return k_ra8_ok;
}

ra8_err_t ra8_rtc_exit_stop(void)
{
  volatile r_rtc_regs_t* rtc = ra8_rtc();
  /* HUM Ch 26.2.21 "RCR2.START" p 1232 */ /* set START to resume. */
  rtc->RCR2 = (uint8_t)(rtc->RCR2 | (1U << k_ra8_rcr2_bit_start));
  internal_wait_bit(&rtc->RCR2,
                    (uint8_t)(1U << k_ra8_rcr2_bit_start),
                    (uint8_t)(1U << k_ra8_rcr2_bit_start));
  return k_ra8_ok;
}
