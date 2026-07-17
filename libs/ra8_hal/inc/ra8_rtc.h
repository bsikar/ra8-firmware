/**
 * @file ra8_rtc.h
 * @brief Real-Time Clock driver (BCD calendar mode)
 * @ingroup grp_hal_timers
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_err.h"

/**
 * @struct ra8_rtc_datetime_t
 * @brief Calendar date + time decoded from BCD.
 *
 * @details
 * cppcheck cannot see tests/ so it flags every field as unused;
 * each member is read in ``ra8_rtc_get`` and ``ra8_rtc_set`` in
 * ``libs/ra8_hal/src/ra8_rtc.c``.
 */
typedef struct {
  uint16_t year;    /**< 2000..2099.         */
  uint8_t  month;   /**< 1..12.              */
  uint8_t  day;     /**< 1..31.              */
  uint8_t  weekday; /**< 0 (Sun) .. 6 (Sat). */
  uint8_t  hour;    /**< 0..23.              */
  uint8_t  minute;  /**< 0..59.              */
  uint8_t  second;  /**< 0..59.              */
} ra8_rtc_datetime_t;

/**
 * @enum ra8_rtc_clk_src_t
 * @brief RTC count-source selection (RCR4.RCKSEL encoding).
 *
 * @details
 * The RA8D2 RTC counts on either the 32.768 kHz sub-clock crystal
 * oscillator (SOSC) or the internal LOCO. The numeric values match the
 * RCR4.RCKSEL bit pattern (HUM Ch 26.2.23 "RCR4 : RTC Control Register 4"
 * p 1236), so the enum value can be written directly into RCKSEL.
 *
 * Prefer ::k_ra8_rtc_clk_subclock on a board with the 32.768 kHz crystal
 * populated (the EK-RA8D2) for an accurate time base; fall back to
 * ::k_ra8_rtc_clk_loco on boards without the crystal.
 *
 * @see ra8_rtc_clock_init()
 */
typedef enum : uint8_t {
  k_ra8_rtc_clk_subclock = 0U, /**< RCKSEL=0: 32.768 kHz sub-clock crystal (SOSC).       */
  k_ra8_rtc_clk_loco     = 1U, /**< RCKSEL=1: internal LOCO (~32.768 kHz, crystal-free). */
} ra8_rtc_clk_src_t;

/**
 * @brief Bring up and select the RTC count source (HUM Fig 26.3).
 *
 * @details
 * The RTC counter does not advance until a count clock is both running
 * and selected. This routine performs the "Clock and Count Mode Setting
 * Procedure" of HUM Ch 26.3.2 (Figure 26.3, p 1243):
 *
 *  1. Start the requested count-source oscillator -- for the sub-clock,
 *     set SOMCR drive then clear SOSCCR.SOSTP and wait the sub-clock
 *     stabilization time; for LOCO, clear LOCOCR.LCSTP. Oscillator
 *     control registers are gated by PRCR group 0 (CGC).
 *  2. Select the count source via RCR4.RCKSEL and supply at least six
 *     count-source clocks before proceeding.
 *  3. Stop the prescaler (RCR2.START = 0) and, when LOCO is selected,
 *     program the prescaler frequency register (RFRH = 0, RFRL = 0x00FF
 *     for 32.768 kHz) per HUM Ch 26.2.24 p 1236.
 *  4. Execute an RTC software reset (RCR2.RESET = 1) so the prescaler and
 *     count registers initialize against the live count source.
 *
 * Call this once at power-on, BEFORE ::ra8_rtc_init(): it leaves the RTC
 * stopped with its count source running and selected, ready for the
 * 24-hour-calendar bring-up that ::ra8_rtc_init() finishes.
 *
 * @param[in] src Count source to bring up and select
 *                (::k_ra8_rtc_clk_subclock or ::k_ra8_rtc_clk_loco).
 *
 * @return ``ra8_err_t`` outcome.
 * @retval k_ra8_ok               Count source running, selected, RTC reset.
 * @retval k_ra8_err_invalid_arg  ``src`` is not a valid ::ra8_rtc_clk_src_t.
 * @retval k_ra8_err_hw_init_failed The selected oscillator did not leave its
 *                               stop state (stop bit still set after enable).
 *
 * @pre ``ra8_time_init()`` has run (this routine blocks on ``ra8_delay_ms``).
 * @pre Single-threaded init context (mutates PRCR-gated CGC registers).
 * @post The selected oscillator's stop bit is clear (oscillator running).
 * @post RCR4.RCKSEL selects @p src and the RTC has been software-reset
 *       with its prescaler stopped (RCR2.START = 0).
 *
 * @note Not thread-safe.
 * @see ra8_rtc_init()
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_rtc_clock_init(ra8_rtc_clk_src_t src);

/**
 * @brief Start the RTC in 24-hour calendar mode.
 *
 * @return `k_ra8_ok` on success, `k_ra8_err_hw_init_failed` otherwise.
 *
 * @note The count source must already be running and selected. Call
 *       ::ra8_rtc_clock_init() first -- this driver's counter does not
 *       advance otherwise.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_rtc_init(void);

/**
 * @brief Write the calendar registers.
 * @param[in] dt Date/time to install (BCD-encoded internally).
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_rtc_set(const ra8_rtc_datetime_t* dt);

/**
 * @brief Read the calendar registers.
 * @param[out] out Receive buffer.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_rtc_get(ra8_rtc_datetime_t* out);

/**
 * @enum ra8_rtc_irq_mask_t
 * @brief RCR1 IRQ enable bits.
 */
typedef enum : uint8_t {
  k_ra8_rtc_irq_none     = 0x00U, /**< RA8 rtc IRQ none. */
  k_ra8_rtc_irq_alarm    = 0x01U, /**< RCR1.AIE.         */
  k_ra8_rtc_irq_carry    = 0x02U, /**< RCR1.CIE.         */
  k_ra8_rtc_irq_periodic = 0x04U, /**< RCR1.PIE.         */
} ra8_rtc_irq_mask_t;

/**
 * @typedef ra8_rtc_event_fn_t
 * @brief RTC event callback.
 */
typedef void (*ra8_rtc_event_fn_t)(void* ctx, uint8_t status_mask);

/**
 * @brief Programme the alarm-match registers (hour/minute/second).
 *
 * @details
 * Writes RSECAR / RMINAR / RHRAR with the BCD-encoded match values
 * from @p alarm and sets each register's ENB bit (bit 7) so the
 * field participates in the match. Day, month, year, weekday alarm
 * registers are wildcarded (ENB cleared) so the alarm fires on the
 * next time-of-day match within the current day.
 *
 * Caller is responsible for enabling RCR1.AIE via
 * ``ra8_rtc_set_irq_enable(k_ra8_rtc_irq_alarm)`` after this call.
 *
 * @param[in] alarm Hour/minute/second to match. ``year`` and ``month``
 *                  fields are ignored. Must not be NULL.
 *
 * @return ``ra8_err_t`` outcome.
 * @retval k_ra8_ok               Alarm registers written.
 * @retval k_ra8_err_null_ptr     ``alarm`` is NULL.
 * @retval k_ra8_err_invalid_arg  ``alarm->hour > 23`` or
 *                               ``alarm->minute > 59`` or
 *                               ``alarm->second > 59``.
 *
 * @pre ``ra8_rtc_init()`` previously succeeded.
 * @pre ``alarm != nullptr``.
 * @post RSECAR / RMINAR / RHRAR carry the encoded match values with
 *       their ENB bits set.
 * @post Day / month / year / weekday alarm-enable bits are clear.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_rtc_set_alarm(const ra8_rtc_datetime_t* alarm);

/**
 * @brief Tear down the RTC (stop counter + disable IRQs).
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_rtc_deinit(void);

/**
 * @brief Enable one or more RTC IRQ sources via RCR1.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_rtc_set_irq_enable(uint8_t mask);

/**
 * @brief Read RCR1 IRQ enable bits.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_rtc_get_status(uint8_t* out_mask);

/**
 * @brief Clear RCR1 IRQ enable bits.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_rtc_clear_status(uint8_t mask);

/**
 * @brief Attach a callback for the RTC alarm / periodic event.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_rtc_attach_handler(ra8_rtc_event_fn_t fn, void* ctx);

/**
 * @brief Dispatch an RTC event -- snapshot RCR1 + fire callback.
 *
 * @details
 * Reads ``RTC.RCR1`` (HUM Ch 25.2.18 "RCR1 : RTC Control Register 1",
 * p ~1097) to capture which interrupt flag (alarm AIE, carry CIE, or
 * periodic PIE) is asserted, clears the captured bits, and invokes the
 * handler installed via ``ra8_rtc_attach_handler()`` with the snapshot
 * mask. Silently returns if no handler is installed.
 *
 * @pre ``ra8_rtc_init()`` previously succeeded.
 * @pre Called from ISR context or unit-test driver.
 * @post ``RCR1`` interrupt-enable bits captured at entry are cleared.
 * @post Registered handler invoked at most once with the snapshot mask.
 *
 * @note Thread safety: ISR context only; not re-entrant.
 * @since 0.1.0
 */
void ra8_rtc_dispatch(void);

/**
 * @brief Stop the RTC counter for low-power mode.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_rtc_enter_stop(void);

/**
 * @brief Restart the RTC counter from stop.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_rtc_exit_stop(void);

#ifdef __cplusplus
}
#endif
