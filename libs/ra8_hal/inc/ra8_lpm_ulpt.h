/**
 * @file ra8_lpm_ulpt.h
 * @brief ULPT0-backed periodic deep idle (Software Standby self-wake) HAL API
 * @ingroup grp_hal_system
 *
 * @par Tag
 * [Ring 3 / HAL] {World: S}
 *
 * @details
 * Extracts the RA8D2 ULPT0-backed Software Standby self-wake sequence shared
 * across periodic examples, power profilers, and TrustZone NSC gateways:
 *
 * - Converts requested dwell milliseconds cleanly to 32-bit LOCO ticks.
 * - Keeps LOCO alive across Software Standby (``LOCOCR.LCSTP = 0``), wires
 *   ``k_ra8_elc_event_ulpt0_ulpti`` via IELSR/NVIC, and arms ``WUPEN1.ULP0U``.
 * - Starts ULPT0, polls ``ULPTCR.TCSTF = 1`` within a bounded iteration cap,
 *   unlocks ``PRCR.PRC1``, enters ``k_ra8_sleep_mode_software_std``, and
 *   unconditionally relocks ``PRCR.PRC1`` and stops ULPT0 before returning.
 *
 * Included automatically by ``ra8_lpm.h``.
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
#include "ra8_isr.h"

/**
 * @enum ra8_lpm_ulpt_const_t
 * @brief Fixed parameters for ULPT0 Software Standby self-wake (HUM Ch 11/19/25).
 */
typedef enum : uint32_t {
  k_ra8_lpm_ulpt_loco_hz =
    32768U, /**< ULPTLCLK (LOCO) clock frequency, Hz (HUM Table 9.2 p 320). */
  k_ra8_lpm_ulpt_start_poll_max = 0x40000U, /**< Max poll iterations confirming ULPTCR.TCSTF = 1. */
  k_ra8_lpm_ulpt_default_isr_prio = 4U,     /**< Default NVIC priority for the ULPT0 wake route.  */
} ra8_lpm_ulpt_const_t;

/**
 * @brief Convert a deep-idle duration in milliseconds to ULPT0 LOCO ticks.
 *
 * @details
 * Rounds upward via ``((uint64_t)period_ms * 32768 + 999) / 1000`` so the sleep
 * interval is never shorter than requested and catches both zero durations and
 * 32-bit counter overflow cleanly.
 *
 * @param[in]  period_ms Desired standby interval in milliseconds (> 0).
 * @param[out] out_ticks Receives the 32-bit ULPT0 reload count.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok              Conversion succeeded.
 * @retval k_ra8_err_null_ptr    ``out_ticks`` was nullptr.
 * @retval k_ra8_err_invalid_arg ``period_ms`` was 0 or overflowed ``UINT32_MAX`` ticks.
 *
 * @pre ``out_ticks`` is non-NULL.
 * @pre ``period_ms`` is in ``1 .. 131071999``.
 * @post ``*out_ticks`` >= 1 on ``k_ra8_ok``.
 * @post Hardware registers are untouched.
 *
 * @note Thread safety: reentrant pure calculation.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_lpm_ulpt_ms_to_ticks(uint32_t period_ms, uint32_t* out_ticks);

/**
 * @brief Prepare ULPT0, LOCO, ICU/NVIC, and WUPEN1 for Software Standby self-wake.
 *
 * @details
 * Encapsulates the full RA8D2 Software Standby ULPT0 wake contract:
 *  1. Initializes the ULPT driver (``ra8_ulpt_init``).
 *  2. Clears ``LOCOCR.LCSTP`` so ULPTLCLK keeps ticking in Software Standby
 *     (HUM Table 11.3 footnote *2 p 433).
 *  3. Installs an IELSRn + NVIC route for ``k_ra8_elc_event_ulpt0_ulpti`` via
 *     ``ra8_isr_register`` (HUM 11.6.2.1 p 482, Table 11.3 footnote *28 p 434).
 *     If already registered, unregisters the stale slot first so re-arming is
 *     idempotent. Passing ``nullptr`` for ``isr_cb`` installs a minimal no-op
 *     wake handler.
 *  4. Arms ``WUPEN1.ULP0U`` via ``ra8_lpm_arm_wupen1_bits`` (HUM Ch 14.2.20 p 552).
 *
 * @param[in] isr_cb   Optional wake handler invoked when ULPT0 underflows, or nullptr.
 * @param[in] isr_ctx  Caller context forwarded to ``isr_cb``.
 * @param[in] priority NVIC interrupt priority (0 .. ``k_ra8_isr_prio_max``).
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok              ULPT0 wake path is armed and ready for standby.
 * @retval k_ra8_err_invalid_arg ``priority`` exceeds ``k_ra8_isr_prio_max``.
 * @retval (other)               First error from ULPT, clock, ISR, or WUPEN setup.
 *
 * @pre Global interrupts or NVIC slot pool available (``ra8_isr_init`` called).
 * @pre Secure-world execution (SYSC / ICU / WUPEN accessible).
 * @post On ``k_ra8_ok``, ``LOCOCR.LCSTP == 0``, IELSR/NVIC routed for ULPT0
 *       underflow, and ``WUPEN1.ULP0U == 1``.
 * @post On failure after ISR registration, the ISR route is cleanly rolled back.
 *
 * @note Thread safety: not thread-safe.
 * @see ra8_lpm_ulpt_standby_ticks, ra8_lpm_ulpt_wake_disarm
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_lpm_ulpt_wake_prepare(ra8_isr_handler_t isr_cb, void* isr_ctx, uint8_t priority);

/**
 * @brief Start ULPT0 for ``period_ticks`` and enter Software Standby until underflow.
 *
 * @details
 * Executes the complete per-period Software Standby cycle:
 *  1. Starts ULPT0 with ``period_ticks`` (``ra8_ulpt_start``).
 *  2. Boundedly polls ``ULPTCR.TCSTF == 1`` up to ``k_ra8_lpm_ulpt_start_poll_max``
 *     iterations so Standby entry cannot stall the counter before it starts
 *     (HUM Ch 25.4.7 p 1214).
 *  3. Unlocks ``PRCR.PRC1``, enters ``k_ra8_sleep_mode_software_std``, and
 *     unconditionally relocks ``PRCR.PRC1``.
 *  4. Calls ``ra8_ulpt_stop(0)`` to clear ``ULPTCR.TUNDF`` cleanly for the
 *     next period (HUM Ch 25.2.1 p 1190).
 *
 * @param[in] period_ticks Non-zero ULPTLCLK reload count.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok              Woke from Software Standby and stopped ULPT0.
 * @retval k_ra8_err_invalid_arg ``period_ticks`` was zero.
 * @retval k_ra8_err_hw_timeout  ``ULPTCR.TCSTF`` failed to assert within poll limit.
 * @retval (other)               Error from start, PRCR, sleep, or stop.
 *
 * @pre ``ra8_lpm_ulpt_wake_prepare`` succeeded (and global IRQs are enabled on target).
 * @pre ``ra8_lpm_init`` programmed SBYCR/DPSBYCR/SSCR1 policy if non-default desired.
 * @post ``PRCR.PRC1`` is locked and ULPT0 is stopped on every return path.
 * @post ``LPSCR.LPMD`` and ``SCR.SLEEPDEEP`` are restored to active sleep state.
 *
 * @note Thread safety: not thread-safe.
 * @see ra8_lpm_ulpt_standby_ms, ra8_lpm_ulpt_wake_prepare
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_lpm_ulpt_standby_ticks(uint32_t period_ticks);

/**
 * @brief Convenience wrapper converting ``period_ms`` to ticks and entering standby.
 *
 * @param[in] period_ms Desired standby interval in milliseconds (> 0).
 *
 * @return ``ra8_err_t`` error code from ``ra8_lpm_ulpt_ms_to_ticks`` or
 *         ``ra8_lpm_ulpt_standby_ticks``.
 * @retval k_ra8_ok              Woke from Software Standby cleanly.
 * @retval k_ra8_err_invalid_arg ``period_ms`` was 0 or overflowed 32-bit ticks.
 * @retval k_ra8_err_hw_timeout  Counter start or stop confirmation timed out.
 *
 * @pre ``ra8_lpm_ulpt_wake_prepare`` succeeded.
 * @pre ``period_ms`` is in ``1 .. 131071999``.
 * @post ``PRCR.PRC1`` is locked and ULPT0 is stopped on every return path.
 * @post System has resumed active mode.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_lpm_ulpt_standby_ms(uint32_t period_ms);

/**
 * @brief Disarm ULPT0 wake path cleanly (stop timer, clear WUPEN1, unregister ISR).
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok All stop / disarm / unregister steps succeeded.
 * @retval (other)  First non-OK error encountered during teardown (all teardown
 *                  steps are still attempted).
 *
 * @pre ULPT0 / ICU / LPM MMIO windows are accessible in Secure state.
 * @pre Called in single-threaded or masked-IRQ context.
 * @post ``ULPT0`` stopped and ``WUPEN1.ULP0U`` cleared.
 * @post Any registered ``k_ra8_elc_event_ulpt0_ulpti`` ISR slot is released.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_lpm_ulpt_wake_disarm(void);

#ifdef __cplusplus
}
#endif
