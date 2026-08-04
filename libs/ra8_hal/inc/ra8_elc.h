/**
 * @file ra8_elc.h
 * @brief Event Link Controller driver
 * @ingroup grp_hal_system
 *
 * @par Tag
 * [Ring 3 / HAL] {World: S}
 *
 * @details
 * Owns every write to the ELC register block (ELCR, ELSEGR0..3,
 * ELSR0..52, ELCSAR{A,B,C}, ELCPAR{A,B,C}) at 0x40201000
 * (HUM Ch 19, p 817..836). Layout cross-verified against FSP
 * `R_ELC_Type` in `R7KA8D2KF_core0.h`.
 *
 * The ELC lets one peripheral's event directly trigger another
 * peripheral's input without CPU involvement. The driver exposes
 * the following operation-named helpers:
 *
 * - ``ra8_elc_init`` -- reset ELSR array, enable ELC.
 * - ``ra8_elc_deinit`` -- disable ELC globally.
 * - ``ra8_elc_link`` -- route event -> ELSR slot.
 * - ``ra8_elc_unlink`` -- clear one ELSR slot.
 * - ``ra8_elc_software_trigger`` -- fire a software event using the
 *   3-step ELSEGR unlock-and-set sequence (HUM Ch 19.2.2).
 * - ``ra8_elc_is_enabled`` -- diagnostic accessor.
 *
 * Drivers call these helpers; they never touch the ELC registers
 * directly. The NSC veneer surface is ``ra8_elc_*``.
 *
 * ## ELSR index range
 *
 * HUM Ch 19.2.3 (p 817) / FSP `R_ELC_Type` assign 53 ELSR slots
 * (ELSR0..ELSR52), each a 16-bit register at a 4-byte stride that
 * stores one ELC event number (the ELS field is 10 bits wide). The
 * mapping of slot to destination peripheral is fixed and lives in
 * HUM Table 19.2; callers learn it from the peripheral datasheets.
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

#include "ra8_elc_regs.h"
#include "ra8_err.h"

/**
 * @enum ra8_elc_dim_t
 * @brief ELC register-array dimensions.
 */
typedef enum : uint8_t {
  k_ra8_elc_elsr_count = 53U, /**< ELSR0..ELSR52 (FSP R_ELC_Type).    */
  k_ra8_elc_segr_count = 4U,  /**< ELSEGR0..ELSEGR3 (FSP R_ELC_Type). */
} ra8_elc_dim_t;

/**
 * @brief Reset the ELC register block and enable the controller.
 *
 * @details
 * Powers the ELC module via MSTPCRC, clears every ELSR slot
 * (ELSR0..ELSR52), clears ELSEGR0..3, then sets
 * ``ELCR.ELCON = 1`` so the hardware routes events.
 *
 * @return ``k_ra8_ok``.
 * @pre Caller is in single-threaded init context.
 * @pre IRQs masked or single-threaded init context.
 * @post ELSR0..ELSR52 == 0.
 * @post ELCR.ELCON == 1 (controller enabled).
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_elc_init(void);

/**
 * @brief Disable the ELC globally (ELCR.ELCON = 0) and stop the module.
 *
 * @return ``k_ra8_ok``.
 * @pre IRQs masked or single-threaded init context.
 * @pre ``ra8_elc_init`` was called previously.
 * @post ELCR.ELCON == 0.
 * @post MSTPCRC ELC bit is set (module stopped).
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_elc_deinit(void);

/**
 * @brief Route an ELC event to an ELSR slot so the destination
 * peripheral triggers on it.
 *
 * @param[in] elsr_index Slot number 0..k_ra8_elc_elsr_count - 1.
 * @param[in] event ELC event number from ``ra8_elc_event_t``.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Slot programmed.
 * @retval k_ra8_err_out_of_range ``elsr_index`` out of range.
 *
 * @pre IRQs masked or single-threaded init context.
 * @post ELSR[elsr_index] holds ``event``.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_elc_link(uint8_t elsr_index, ra8_elc_event_t event);

/**
 * @brief Clear one ELSR slot so it stops routing any event.
 *
 * @param[in] elsr_index Slot number 0..k_ra8_elc_elsr_count - 1.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Slot cleared.
 * @retval k_ra8_err_out_of_range ``elsr_index`` out of range.
 *
 * @pre IRQs masked or single-threaded init context.
 * @post ELSR[elsr_index] == 0.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_elc_unlink(uint8_t elsr_index);

/**
 * @brief Fire a software event via one of the ELSEGRn registers.
 *
 * @details
 * Per HUM Ch 19.2.2 "ELSEGRn : Event Link Software Event Generation
 * Register n" (p 817) and FSP `R_ELC_SoftwareEventGenerate`, the
 * SEG bit is write-protected by WI/WE and must be set with a
 * three-step sequence:
 *
 * 1. Write 0x00 (``k_ra8_elc_elsegr_step_unlock``) -- clear WI.
 * 2. Write 0x40 (``k_ra8_elc_elsegr_step_arm``)    -- set WE, SEG=0.
 * 3. Write 0x41 (``k_ra8_elc_elsegr_step_trigger``) -- set SEG=1.
 *
 * The hardware fires the event exactly once. Useful for testing
 * event-driven paths without waiting for a real peripheral edge.
 *
 * @param[in] event_index Software event 0..3 (selects ELSEGR0..3).
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Three-step sequence written.
 * @retval k_ra8_err_invalid_arg ``event_index`` out of range.
 *
 * @pre IRQs masked or single-threaded init context.
 * @pre ELC is enabled via ``ra8_elc_init``.
 * @post ELSEGR[event_index] holds 0x41 after the call returns;
 * the event fires once on the next ELC clock edge.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_elc_software_trigger(uint8_t event_index);

/**
 * @brief Read ELCR.ELCON to verify the global enable state.
 *
 * @param[out] out_enabled On success, ``true`` if ELCON is set.
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Value returned.
 * @retval k_ra8_err_null_ptr ``out_enabled`` was NULL.
 *
 * @pre ``out_enabled`` is non-NULL.
 * @post No hardware state is modified.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_elc_is_enabled(bool* out_enabled);

#ifdef __cplusplus
}
#endif
