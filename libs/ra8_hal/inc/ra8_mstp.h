/**
 * @file ra8_mstp.h
 * @brief Ref-counted Module Stop Control wrapper for the RA8D2
 * @ingroup grp_hal_system
 *
 * @par Tag
 * [Ring 3 / HAL] {World: S}
 *
 * @details
 * Public API of the substrate module that owns every
 * write to the RA8D2 ``MSTPCRA..MSTPCRE`` registers (HUM Ch 11,
 * sections 11.2.6..11.2.10, pages 443..450).
 *
 * Drivers must call ``ra8_mstp_enable()`` before they touch the
 * peripheral's registers and ``ra8_mstp_disable()`` when they tear
 * down. Calls are reference-counted: two unrelated drivers that
 * both depend on the same MSTP bit (DMAC + DTC, RSIP + TRNG, ...)
 * can each request and release independently and the underlying
 * bit only flips when the last user lets go.
 *
 * Why a ref count when each peripheral is normally owned by one
 * driver? Two cases that the wave plan needs:
 *
 * 1. **Shared MSTP bits**: ``MSTPA22`` covers both DMAC0 and DTC0
 * (HUM 11.2.6 Note 1). The DMAC and DTC drivers will both
 * request that bit, and we cannot disable it until both have
 * released.
 * 2. **Wake-up coordination**: the secure-side LPM
 * handler walks every requested module to figure out what
 * should still be running across a software-standby trip.
 * A ref count is the simplest way to remember "is anyone
 * still using this module?".
 *
 * The bit-twiddling itself respects HUM 11.2.6 Note 2:
 * "When changing the value of this bit, only execute subsequent
 * instructions after reading this bit to check that the value
 * was updated." ``ra8_mstp_enable() / disable()`` therefore
 * read the register back after the modify-write and return
 * ``k_ra8_err_hw_timeout`` if the read does not see the new value
 * within a small bounded budget.
 *
 * ## Threading
 *
 * Not thread-safe. Call only from single-threaded init or with
 * interrupts masked. The bare-metal v0.x project has no RTOS, so
 * this is satisfied trivially.
 *
 * ## Peripheral identifiers
 *
 * The full list of peripherals lives in ``ra8_mstp_regs.h`` as
 * the ``ra8_mstp_t`` enum. Each entry packs the (register, bit)
 * pair from the HUM register description into a 16-bit value.
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
#include "ra8_mstp_regs.h"

/* =============================================================================
 * Module-level lifecycle
 * =============================================================================
 */

/**
 * @brief Re-establish the ra8_mstp ref-count table from current hardware state.
 *
 * @details
 * After a cold reset every MSTP bit is ``1`` (peripheral stopped) and
 * every ref-count is ``0``. After a warm reset or a NS->S
 * transition, the table may be out of sync with hardware and the
 * caller wants to start clean.
 *
 * Calling this function:
 *
 * 1. Sets every ref count back to ``0``.
 * 2. Writes ``0xFFFFFFFF`` to every MSTPCR (everything stopped).
 * 3. Reads each register back to confirm the write took effect.
 *
 * After ``ra8_mstp_init()`` returns, every subsequent
 * ``ra8_mstp_enable()`` is the first request for that module.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Refcount table reset and hardware
 * confirmed all-stopped.
 * @retval k_ra8_err_hw_timeout Read-back loop did not observe the
 * expected value within budget.
 *
 * @pre Caller holds single-threaded init context (no concurrent
 * driver init in progress).
 * @pre IRQs masked while this function runs.
 *
 * @post Every ra8_mstp_t id reports ref-count 0.
 * @post Every MSTPCR register reads back as ``0xFFFFFFFF``.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_mstp_init(void);

/* =============================================================================
 * Per-peripheral enable / disable
 * =============================================================================
 */

/**
 * @brief Reference-counted "ungate this peripheral" request.
 *
 * @details
 * Increments the ref count for ``id``. If the count transitions
 * ``0 -> 1``, clears the corresponding MSTP bit so the peripheral
 * starts receiving its clock. If the count was already non-zero,
 * the bit is left as-is (someone else is already using the module)
 * and the function returns success without touching hardware.
 *
 * Implements the read-back protocol from HUM 11.2.6 Note 2: after
 * the bit-clear, the register is read repeatedly until the cleared
 * bit is observed or the polling budget runs out.
 *
 * Algorithm:
 *
 * 1. Validate ``id``: register index must be 0..4, bit must be 0..31.
 * 2. Increment ref count.
 * 3. If ref count was 0 before increment:
 * a. Read MSTPCRx, clear the target bit, write back.
 * b. Poll the same bit until it reads as 0 or the polling
 * budget expires.
 * 4. Return.
 *
 * @param[in] id Peripheral identifier from ``ra8_mstp_t``.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Ungated successfully (or already on).
 * @retval k_ra8_err_invalid_arg ``id`` decodes to an out-of-range
 * register or bit position.
 * @retval k_ra8_err_hw_timeout Bit did not read back as cleared
 * within the spin budget.
 *
 * @pre IRQs masked or single-threaded init context.
 * @pre ``ra8_mstp_init()`` has run (or this is the first request
 * against a fresh ref-count table).
 *
 * @post On success, the peripheral is clocked and the ref count for
 * ``id`` is at least 1.
 * @post HUM 11.2.6 Note 2 read-back has been observed.
 *
 * @note Thread safety: not thread-safe.
 * @note For shared bits (DMAC + DTC, OSPI + DOTF, ...) every user
 * must call this function for the count to balance correctly.
 *
 * @code{.c}
 * if (ra8_mstp_enable(k_ra8_mstp_sci0) != k_ra8_ok) {
 * return k_ra8_err_hw_init_failed;
 * }
 * @endcode
 *
 * @see ra8_mstp_disable
 * @see ra8_mstp_get_refcount
 *
 * @since 0.1.0
 *
 * @par NASA Power of 10 Compliance:
 * - Rule 5: 2 preconditions, 2 postconditions
 * - Rule 7: returns ra8_err_t, marked [[nodiscard]]
 */
[[nodiscard]] ra8_err_t ra8_mstp_enable(ra8_mstp_t id);

/**
 * @brief Reference-counted "gate this peripheral" request.
 *
 * @details
 * Decrements the ref count for ``id``. If the count transitions
 * ``1 -> 0``, sets the corresponding MSTP bit so the peripheral
 * stops receiving its clock. If the count was greater than 1, the
 * bit is left cleared (other users are still active).
 *
 * Honors the same read-back protocol as ``ra8_mstp_enable()``.
 *
 * @param[in] id Peripheral identifier from ``ra8_mstp_t``.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Gated successfully (or still in use
 * by another caller).
 * @retval k_ra8_err_invalid_arg ``id`` decodes to an out-of-range
 * register or bit position.
 * @retval k_ra8_err_invalid_state Refcount was already 0 -- caller
 * disabled a peripheral they had
 * not enabled.
 * @retval k_ra8_err_hw_timeout Bit did not read back as set within
 * the spin budget.
 *
 * @pre IRQs masked or single-threaded init context.
 * @pre Caller previously called ``ra8_mstp_enable(id)``.
 *
 * @post On success, ref count is decremented by 1.
 * @post If ref count reached 0, the peripheral is gated.
 *
 * @warning HUM 11.2.6 Note 1: when releasing ``k_ra8_mstp_dmac0_dtc0``
 * or ``k_ra8_mstp_dmac1_dtc1`` (the shared DMAC/DTC bits),
 * the caller must first stop every channel of both DMAC and
 * DTC. ra8_mstp does not enforce this; the substrate driver
 * ``libs/ra8_hal/src/ra8_dma.c`` is responsible.
 *
 * @note Thread safety: not thread-safe.
 *
 * @see ra8_mstp_enable
 * @see ra8_mstp_get_refcount
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_mstp_disable(ra8_mstp_t id);

/**
 * @brief Read the current ref count for an MSTP id.
 *
 * @details
 * Diagnostic accessor. Used by unit tests to verify that
 * ``ra8_mstp_enable() / disable()`` pairs balance correctly. Should
 * not appear on hot paths -- production code should not need to
 * branch on the ref count.
 *
 * @param[in] id Peripheral identifier from ``ra8_mstp_t``.
 * @param[out] out_ref On success, the current ref count value.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Ref count returned in ``*out_ref``.
 * @retval k_ra8_err_null_ptr ``out_ref`` was nullptr.
 * @retval k_ra8_err_invalid_arg ``id`` decodes to an out-of-range
 * register or bit position.
 *
 * @pre ``out_ref`` is non-NULL.
 * @pre ``id`` is a value from ``ra8_mstp_t``.
 *
 * @post On success, ``*out_ref`` holds the live ref count.
 * @post No hardware state is modified.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_mstp_get_refcount(ra8_mstp_t id, uint8_t* out_ref);

/**
 * @brief Read the current MSTP bit value for an id.
 *
 * @details
 * Diagnostic accessor. Returns ``true`` if the peripheral is
 * currently STOPPED (bit set), ``false`` if it is RUNNING (bit
 * clear). Mostly used by tests; production code should track its
 * own state via ``ra8_mstp_get_refcount()`` or simply call
 * ``ra8_mstp_enable()`` again.
 *
 * @param[in] id Peripheral identifier from ``ra8_mstp_t``.
 * @param[out] out_stopped On success, ``true`` if the bit is set.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Bit value returned in ``*out_stopped``.
 * @retval k_ra8_err_null_ptr ``out_stopped`` was nullptr.
 * @retval k_ra8_err_invalid_arg ``id`` is out of range.
 *
 * @pre ``out_stopped`` is non-NULL.
 * @pre ``id`` is a value from ``ra8_mstp_t``.
 *
 * @post On success, ``*out_stopped`` reflects the live bit value.
 * @post No hardware state is modified.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_mstp_is_stopped(ra8_mstp_t id, bool* out_stopped);

#ifdef __cplusplus
}
#endif
