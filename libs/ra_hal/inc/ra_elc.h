/**
 * @file ra_elc.h
 * @brief Event Link Controller driver
 *
 * @par Tag
 * [Ring 3 / HAL] {World: S}
 *
 * @details
 * Wave 2.1 rewrite. Owns every write to the ELC register block
 * (ELCR, ELSEGR0..1, ELSR0..22) at 0x40201000 (HUM Ch 19, p 817).
 *
 * The ELC lets one peripheral's event directly trigger another
 * peripheral's input without CPU involvement. The driver exposes
 * four operation-named helpers:
 *
 *  - ``ra_elc_init``              -- reset ELSR array, enable ELC.
 *  - ``ra_elc_deinit``            -- disable ELC globally.
 *  - ``ra_elc_link``              -- route event -> ELSR slot.
 *  - ``ra_elc_unlink``            -- clear one ELSR slot.
 *  - ``ra_elc_software_trigger``  -- ELSEGR write fires a software event.
 *  - ``ra_elc_is_enabled``        -- diagnostic accessor.
 *
 * Drivers call these helpers; they never touch the ELC registers
 * directly. The Wave 9 NSC veneer surface is ``ra_elc_*``.
 *
 * ## ELSR index range
 *
 * HUM Ch 19.2.3 (p 817) assigns 23 ELSR slots (ELSR0..ELSR22),
 * each a 16-bit register that stores one ELC event number. The
 * mapping of slot to destination peripheral is fixed and lives
 * in HUM Table 19.2; callers learn it from the peripheral
 * datasheets.
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

#include "ra8d2_elc_regs.h"
#include "ra_err.h"

/**
 * @enum ra_elc_dim_t
 * @brief ELC register-array dimensions.
 */
typedef enum : uint8_t {
  k_ra_elc_elsr_count = 23U, /**< ELSR0..ELSR22.         */
  k_ra_elc_segr_count = 2U,  /**< ELSEGR0, ELSEGR1.      */
} ra_elc_dim_t;

/**
 * @brief Reset the ELC register block and enable the controller.
 *
 * @details
 * Clears every ELSR slot, clears ELSEGR0/1, then sets
 * ``ELCR.ELCON = 1`` so the hardware routes events. Replaces the
 * pre-Wave-2 ``ra_elc_enable(true)`` call in demo main.c.
 *
 * @return ``k_ra_ok``.
 * @pre Caller is in single-threaded init context.
 * @post ELSR0..ELSR22 == 0.
 * @post ELCR.ELCON == 1 (controller enabled).
 *
 * @note Thread safety: not thread-safe.
 * @since 0.2.0
 */
[[nodiscard]] ra_err_t ra_elc_init(void);

/**
 * @brief Disable the ELC globally (ELCR.ELCON = 0).
 *
 * @return ``k_ra_ok``.
 * @pre IRQs masked or single-threaded init context.
 * @post ELCR.ELCON == 0.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.2.0
 */
[[nodiscard]] ra_err_t ra_elc_deinit(void);

/**
 * @brief Enable or disable the ELC globally (legacy wrapper).
 *
 * @deprecated Prefer ``ra_elc_init`` / ``ra_elc_deinit``.
 * Preserved for backward compatibility.
 *
 * @param[in] enable ``true`` sets ELCR.ELCON, ``false`` clears it.
 * @return ``k_ra_ok``.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_elc_enable(bool enable);

/**
 * @brief Route an ELC event to an ELSR slot so the destination
 *        peripheral triggers on it.
 *
 * @param[in] elsr_index Slot number 0..k_ra_elc_elsr_count - 1.
 * @param[in] event      ELC event number from ``ra_elc_event_t``.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok                 Slot programmed.
 * @retval k_ra_err_out_of_range   ``elsr_index`` out of range.
 *
 * @pre IRQs masked or single-threaded init context.
 * @post ELSR[elsr_index] holds ``event``.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_elc_link(uint8_t elsr_index, ra_elc_event_t event);

/**
 * @brief Clear one ELSR slot so it stops routing any event.
 *
 * @param[in] elsr_index Slot number 0..k_ra_elc_elsr_count - 1.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok                 Slot cleared.
 * @retval k_ra_err_out_of_range   ``elsr_index`` out of range.
 *
 * @pre IRQs masked or single-threaded init context.
 * @post ELSR[elsr_index] == 0.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.2.0
 */
[[nodiscard]] ra_err_t ra_elc_unlink(uint8_t elsr_index);

/**
 * @brief Fire a software event via one of the ELSEGR registers.
 *
 * @details
 * Writes to ELSEGR0 or ELSEGR1 (HUM Ch 19.2.2, p 817) cause the
 * specified event to fire exactly once. Useful for testing
 * event-driven paths without waiting for a real peripheral edge.
 *
 * @param[in] group 0 = ELSEGR0, 1 = ELSEGR1.
 * @param[in] value Register value to write.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok                 Write completed.
 * @retval k_ra_err_invalid_arg    ``group`` out of range.
 *
 * @pre IRQs masked or single-threaded init context.
 * @pre ELC is enabled via ``ra_elc_init``.
 * @post ELSEGR[group] latched the requested value; the event
 *       fires once on the next ELC clock edge.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.2.0
 */
[[nodiscard]] ra_err_t ra_elc_software_trigger(uint8_t group, uint8_t value);

/**
 * @brief Read ELCR.ELCON to verify the global enable state.
 *
 * @param[out] out_enabled On success, ``true`` if ELCON is set.
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok                 Value returned.
 * @retval k_ra_err_null_ptr       ``out_enabled`` was NULL.
 *
 * @pre ``out_enabled`` is non-NULL.
 * @post No hardware state is modified.
 * @since 0.2.0
 */
[[nodiscard]] ra_err_t ra_elc_is_enabled(bool* out_enabled);

#ifdef __cplusplus
}
#endif
