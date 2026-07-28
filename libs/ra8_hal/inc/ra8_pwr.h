/**
 * @file ra8_pwr.h
 * @brief Low Power Mode + clock-domain wrapper
 * @ingroup grp_hal_system
 *
 * @par Tag
 * [Ring 3 / HAL] {World: S}
 *
 * @details
 * Ring 3 / HAL substrate. Sits on top of ``ra8_mstp`` and ``ra8_cgc`` and
 * exposes the small set of helpers every driver checklist needs:
 *
 * - ``ra8_pwr_init()`` -- bring-up validation
 * - ``ra8_pwr_module_request(id)`` -- ref-counted ungate
 * - ``ra8_pwr_module_release(id)`` -- ref-counted gate
 * - ``ra8_pwr_set_wake_source / clear`` -- WUPEN0/1 management
 * - ``ra8_pwr_enter_sleep()`` -- WFI for CPU sleep
 * - ``ra8_pwr_enter_software_standby()`` -- WFI for SSTBY
 * - ``ra8_pwr_get_clock_hz()`` -- forwards to ra8_cgc
 *
 * The 14-checkbox per-driver feature template (see
 * ``docs/ROADMAP.md``) lists "Power transition" as
 * "ra8_pwr_module_enter_stop + restore, wake event register".
 * The naming in this file is the canonical version: ``request`` /
 * ``release`` for ungate / gate so that "stop" only ever means
 * "STBY-style stopped" rather than "MSTP bit set".
 *
 * ## Wake-up sources
 *
 * The RA8D2 has 64 distinct wake-up sources spread across two
 * 32-bit ``WUPEN`` registers (HUM 11.2.x). Sources include the
 * IRQ pins, AGT/ULPT timers, RTC alarm, USB suspend/resume, LVD
 * detectors, and the temperature monitor. ``ra8_pwr_set_wake_source()``
 * takes a packed identifier (register index + bit) and toggles
 * the corresponding bit.
 *
 * ## Threading
 *
 * Single-threaded init context only.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_cgc.h"
#include "ra8_err.h"
#include "ra8_mstp_regs.h"

/* =============================================================================
 * Wake-up source identifiers
 * =============================================================================
 */

/**
 * @enum ra8_pwr_wupen_reg_t
 * @brief Which of the two WUPEN registers a wake source lives in.
 */
typedef enum : uint8_t {
  k_ra8_pwr_wupen_reg_0 = 0U, /**< WUPEN0 (sources 0..31).  */
  k_ra8_pwr_wupen_reg_1 = 1U, /**< WUPEN1 (sources 32..63). */
} ra8_pwr_wupen_reg_t;

/**
 * @enum ra8_pwr_wake_t
 * @brief Packed (WUPEN register, bit) wake-source identifier.
 *
 * @details
 * Encoding mirrors ``ra8_mstp_t``: ``(reg << 8) | bit``. Only a
 * handful of well-known sources are listed here; full coverage of
 * all 64 sources is added as the relevant drivers come online.
 *
 * Source list comes from HUM Ch 11 "Low Power Mode" (the WUPEN0
 * and WUPEN1 register description tables).
 */
typedef enum : uint16_t {
  k_ra8_pwr_wake_irq0 = (uint16_t)(k_ra8_pwr_wupen_reg_0 << 8),        /**< RA8 pwr wake irq0. */
  k_ra8_pwr_wake_irq1 = (uint16_t)((k_ra8_pwr_wupen_reg_0 << 8) | 1U), /**< RA8 pwr wake irq1. */
  k_ra8_pwr_wake_irq2 = (uint16_t)((k_ra8_pwr_wupen_reg_0 << 8) | 2U), /**< RA8 pwr wake irq2. */
  k_ra8_pwr_wake_irq3 = (uint16_t)((k_ra8_pwr_wupen_reg_0 << 8) | 3U), /**< RA8 pwr wake irq3. */
  k_ra8_pwr_wake_irq4 = (uint16_t)((k_ra8_pwr_wupen_reg_0 << 8) | 4U), /**< RA8 pwr wake irq4. */
  k_ra8_pwr_wake_irq5 = (uint16_t)((k_ra8_pwr_wupen_reg_0 << 8) | 5U), /**< RA8 pwr wake irq5. */
  k_ra8_pwr_wake_irq6 = (uint16_t)((k_ra8_pwr_wupen_reg_0 << 8) | 6U), /**< RA8 pwr wake irq6. */
  k_ra8_pwr_wake_irq7 = (uint16_t)((k_ra8_pwr_wupen_reg_0 << 8) | 7U), /**< RA8 pwr wake irq7. */

  k_ra8_pwr_wake_iwdt = (uint16_t)((k_ra8_pwr_wupen_reg_0 << 8) | 16U), /**< RA8 pwr wake iwdt. */
  k_ra8_pwr_wake_lvd1 = (uint16_t)((k_ra8_pwr_wupen_reg_0 << 8) | 18U), /**< RA8 pwr wake lvd1. */
  k_ra8_pwr_wake_lvd2 = (uint16_t)((k_ra8_pwr_wupen_reg_0 << 8) | 19U), /**< RA8 pwr wake lvd2. */
  k_ra8_pwr_wake_rtc_alarm =
    (uint16_t)((k_ra8_pwr_wupen_reg_0 << 8) | 24U), /**< RA8 pwr wake rtc alarm. */
  k_ra8_pwr_wake_rtc_period =
    (uint16_t)((k_ra8_pwr_wupen_reg_0 << 8) | 25U), /**< RA8 pwr wake rtc period. */
  k_ra8_pwr_wake_usbhs_resume =
    (uint16_t)((k_ra8_pwr_wupen_reg_0 << 8) | 26U), /**< RA8 pwr wake usbhs resume. */
  k_ra8_pwr_wake_usbfs_resume =
    (uint16_t)((k_ra8_pwr_wupen_reg_0 << 8) | 27U), /**< RA8 pwr wake usbfs resume. */

  k_ra8_pwr_wake_agt0  = (uint16_t)(k_ra8_pwr_wupen_reg_1 << 8),        /**< RA8 pwr wake agt0.  */
  k_ra8_pwr_wake_agt1  = (uint16_t)((k_ra8_pwr_wupen_reg_1 << 8) | 1U), /**< RA8 pwr wake agt1.  */
  k_ra8_pwr_wake_ulpt0 = (uint16_t)((k_ra8_pwr_wupen_reg_1 << 8) | 2U), /**< RA8 pwr wake ulpt0. */
  k_ra8_pwr_wake_ulpt1 = (uint16_t)((k_ra8_pwr_wupen_reg_1 << 8) | 3U), /**< RA8 pwr wake ulpt1. */
} ra8_pwr_wake_t;

/* =============================================================================
 * Lifecycle
 * =============================================================================
 */

/**
 * @brief Initialise the LPM substrate.
 *
 * @details
 * Validates the post-reset state and clears every WUPEN bit so
 * that subsequent ``ra8_pwr_set_wake_source()`` calls are the only
 * source of wake events. Also calls ``ra8_mstp_init()`` so the
 * MSTP ref-count table starts in lockstep with hardware.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Substrate ready.
 * @retval k_ra8_err_hw_timeout ra8_mstp_init() did not see its
 * read-back within budget.
 *
 * @pre Caller is in single-threaded init context.
 * @pre Cold reset has just occurred (warm-reset entry is OK too --
 * the function tolerates either).
 * @post WUPEN0 == 0 and WUPEN1 == 0.
 * @post ra8_mstp ref-count table is all zero.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_pwr_init(void);

/* =============================================================================
 * Module request / release
 * =============================================================================
 */

/**
 * @brief Ask for a peripheral's clock to be enabled.
 *
 * @details
 * Forwards to ``ra8_mstp_enable(id)`` after logging. Future
 * versions will validate the requested module against the current
 * LPM state and route the request through an NSC veneer.
 *
 * @param[in] id Peripheral identifier from ``ra8_mstp_t``.
 * @return Result of the underlying ``ra8_mstp_enable()``.
 *
 * @pre IRQs masked or single-threaded init context.
 * @pre ``ra8_pwr_init()`` has been called.
 * @post On success, the peripheral is clocked.
 * @post Ref count for ``id`` is at least 1.
 *
 * @note Thread safety: not thread-safe.
 * @see ra8_mstp_enable
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_pwr_module_request(ra8_mstp_t id);

/**
 * @brief Release a previously-requested peripheral.
 *
 * @details
 * Forwards to ``ra8_mstp_disable(id)``.
 *
 * @param[in] id Peripheral identifier from ``ra8_mstp_t``.
 * @return Result of the underlying ``ra8_mstp_disable()``.
 *
 * @pre IRQs masked or single-threaded init context.
 * @pre Caller previously called ``ra8_pwr_module_request(id)``.
 * @post On the last release, the peripheral is gated.
 * @post Ref count for ``id`` is decremented by 1.
 *
 * @note Thread safety: not thread-safe.
 * @see ra8_mstp_disable
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_pwr_module_release(ra8_mstp_t id);

/* =============================================================================
 * Wake source enable / disable
 * =============================================================================
 */

/**
 * @brief Enable an LPM wake source.
 *
 * @details
 * Sets the corresponding bit in WUPEN0 or WUPEN1 (HUM 11.2.x
 * register descriptions). Wake sources are addressed by the
 * packed ``ra8_pwr_wake_t`` enum.
 *
 * @param[in] source Packed ``ra8_pwr_wake_t`` identifier.
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok The bit is set.
 * @retval k_ra8_err_invalid_arg ``source`` decodes to an
 * out-of-range register or bit.
 *
 * @pre IRQs masked or single-threaded init context.
 * @pre ``ra8_pwr_init()`` has been called.
 * @post The relevant WUPEN bit is set.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_pwr_set_wake_source(ra8_pwr_wake_t source);

/**
 * @brief Disable an LPM wake source.
 *
 * @details
 * Clears the corresponding bit in WUPEN0 or WUPEN1.
 *
 * @param[in] source Packed ``ra8_pwr_wake_t`` identifier.
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok The bit is clear.
 * @retval k_ra8_err_invalid_arg ``source`` decodes to an
 * out-of-range register or bit.
 *
 * @pre IRQs masked or single-threaded init context.
 * @pre ``ra8_pwr_init()`` has been called.
 * @post The relevant WUPEN bit is clear.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_pwr_clear_wake_source(ra8_pwr_wake_t source);

/**
 * @brief Read the current WUPEN bit value for a source.
 *
 * @details
 * Diagnostic accessor used by tests. Returns ``true`` if the
 * source is currently armed.
 *
 * @param[in] source Packed ``ra8_pwr_wake_t`` identifier.
 * @param[out] out_enabled ``true`` on success if the bit is set.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Bit value returned.
 * @retval k_ra8_err_null_ptr ``out_enabled`` was NULL.
 * @retval k_ra8_err_invalid_arg ``source`` decoded out of range.
 *
 * @pre ``out_enabled`` is non-NULL.
 * @pre ``source`` is a value from ``ra8_pwr_wake_t``.
 * @post No hardware state is modified.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_pwr_wake_source_is_enabled(ra8_pwr_wake_t source, bool* out_enabled);

/* =============================================================================
 * Sleep entry
 * =============================================================================
 */

/**
 * @brief Enter CPU sleep mode (peripherals continue running).
 *
 * @details
 * Issues a single ``WFI`` instruction. The Cortex-M85 enters Sleep
 * mode and the CPU clock is gated until any enabled interrupt
 * fires. Peripherals run normally because LPMD bits are not
 * touched.
 *
 * On the host (``RA8_OFF_TARGET``) this is a no-op so unit
 * tests do not stall.
 *
 * @pre IRQs are masked at the level the caller wants to wake on.
 * @pre At least one enabled interrupt source exists, otherwise
 * the CPU never wakes.
 *
 * @post On wake, the function returns to the caller. Caller is
 * responsible for draining whatever woke them.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 *
 * @post Caller-visible state matches the documented contract.
 */
void ra8_pwr_enter_sleep(void);

/**
 * @brief Enter Software Standby mode.
 *
 * @details
 * Sets ``LPSCR.LPMD = 0x5`` and the Cortex-M85 ``SCR.SLEEPDEEP``
 * bit, then issues ``WFI`` (HUM 11.6.2.1 p 482). Most oscillators
 * stop, the CPU stops, peripherals freeze. Wakes from any source
 * armed via ``ra8_pwr_set_wake_source()``.
 *
 * On the host this is a no-op.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Returned cleanly after wake.
 * @retval k_ra8_err_invalid_state Caller failed to arm any wake
 * source first.
 *
 * @pre At least one wake source is armed in WUPEN0/1.
 * @pre IRQs are configured per HUM 11.6.2.1 (the wake interrupt
 * must have its IELSRn entry programmed).
 * @post On wake, the function returns to the caller.
 * @post LPMD has been written back to 0 (normal mode).
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_pwr_enter_software_standby(void);

/* =============================================================================
 * Clock-domain forwarding
 * =============================================================================
 */

/**
 * @brief Forward to ``ra8_cgc_get_clock_hz()`` for naming consistency.
 *
 * @details
 * Plain forwarding helper. Drivers that already include
 * ``ra8_pwr.h`` for module gating do not need to also include
 * ``ra8_cgc.h`` just to look up a baud-rate divider.
 *
 * @param[in] id Clock identifier.
 * @param[out] out_hz On success, current frequency in Hz.
 * @return Result of the underlying ``ra8_cgc_get_clock_hz()``.
 *
 * @pre ``out_hz`` is non-NULL.
 * @pre ``id`` is a value from ``ra8_clock_id_t``.
 * @post On success, ``*out_hz`` holds the live frequency.
 * @post No hardware state is modified.
 *
 * @note Thread safety: not thread-safe.
 * @see ra8_cgc_get_clock_hz
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_pwr_get_clock_hz(ra8_clock_id_t id, uint32_t* out_hz);

#ifdef __cplusplus
}
#endif
