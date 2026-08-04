/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_doc.h
 * @brief Data Operation Circuit (DOC) driver header
 * @ingroup grp_hal_system
 *
 * @details
 * Public API for the 16-bit Data Operation Circuit that performs
 * hardware add / subtract / compare operations. Useful for cheap
 * running checksums and threshold compares without burning CPU
 * cycles.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_doc_regs.h"
#include "ra8_err.h"

/**
 * @brief Reset the DOC control register to its reset state.
 *
 * @details
 * Clears `DOCR`, `DODIR`, and `DODSR0`. Leaves the block disabled
 * until the first operation.
 *
 * @return `k_ra8_ok` on success.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_doc_init(void);

/**
 * @brief Perform a 16-bit hardware add.
 *
 * @param[in]  a       First operand.
 * @param[in]  b       Second operand.
 * @param[out] out_sum Receives `a + b` (wraps mod 2^16).
 * @return `ra8_err_t` error code.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_doc_add16(uint16_t a, uint16_t b, uint16_t* out_sum);

/**
 * @brief Perform a 16-bit hardware subtract.
 *
 * @param[in]  a        Minuend.
 * @param[in]  b        Subtrahend.
 * @param[out] out_diff Receives `a - b` (wraps mod 2^16).
 * @return `ra8_err_t` error code.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_doc_sub16(uint16_t a, uint16_t b, uint16_t* out_diff);

/* =============================================================================
 * Window comparison
 * =============================================================================
 */

/**
 * @enum ra8_doc_window_polarity_t
 * @brief Window-comparison polarity for ra8_doc_set_window().
 *
 * @details
 * Selects between inside-band and outside-band detection. The silicon
 * encodes this via DOCR.DCSEL[2:0] (HUM Ch 57.2.1 p 3519):
 * - k_ra8_doc_window_inside  -> DCSEL = 100b (4)
 * - k_ra8_doc_window_outside -> DCSEL = 101b (5)
 *
 * Inside-window:  DOPCF is set when DODSR0 < DODIR < DODSR1 (strict).
 * Outside-window: DOPCF is set when DODIR < DODSR0 or DODSR1 < DODIR (strict).
 * Boundary values (DODIR == lower or DODIR == upper) do NOT trigger DOPCF
 * in either mode -- the comparisons are strictly less-than / greater-than.
 *
 * @invariant Only values 0 (inside) and 1 (outside) are valid.
 *
 * @code
 * // Configure DOC for inside-band ADC supervision
 * ra8_doc_set_window(0x0800U, 0x1800U, k_ra8_doc_window_inside);
 * @endcode
 *
 * @see ra8_doc_set_window() Configure window thresholds and polarity.
 * @see ra8_doc_window_compare() Trigger a comparison and read DOPCF.
 *
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_ra8_doc_window_inside  = 0U, /**< Flag when DODSR0 < DODIR < DODSR1 (strict inequalities). */
  k_ra8_doc_window_outside = 1U, /**< Flag when DODIR < DODSR0 or DODSR1 < DODIR.              */
} ra8_doc_window_polarity_t;

/**
 * @brief Configure the DOC for 16-bit window comparison.
 *
 * @details
 * Programmes DOCR (OMS=00, DOBW=0, DCSEL=inside or outside), DODSR0
 * (lower threshold), and DODSR1 (upper threshold) to set up a 16-bit
 * window comparison. After this call, each write to DODIR via
 * ra8_doc_window_compare() triggers a hardware comparison; DOSR.DOPCF is
 * set when the input satisfies the selected condition:
 *
 * - k_ra8_doc_window_inside:  DODSR0 < DODIR < DODSR1 (strict).
 * - k_ra8_doc_window_outside: DODIR < DODSR0 or DODSR1 < DODIR (strict).
 *
 * Boundary values (DODIR == lower or DODIR == upper) do NOT trigger
 * DOPCF in either mode (HUM Ch 57.2.1 p 3519: all comparisons are strict).
 *
 * The HUM constraint DODSR1 > DODSR0 (Ch 57.2.5 p 3521, Ch 57.2.6 p 3522)
 * is enforced: lower >= upper is rejected. Any stale DOPCF flag is cleared
 * before returning so the first ra8_doc_window_compare() call reflects a
 * fresh comparison only.
 *
 * @param[in] lower    Lower window threshold [0..65534]; written to DODSR0.
 *                     Must be strictly less than upper.
 * @param[in] upper    Upper window threshold [1..65535]; written to DODSR1.
 *                     Must be strictly greater than lower.
 * @param[in] polarity Detection polarity (k_ra8_doc_window_inside or
 *                     k_ra8_doc_window_outside).
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok              Window programmed and stale DOPCF cleared.
 * @retval k_ra8_err_invalid_arg lower >= upper, or polarity is out of range.
 *
 * @pre ra8_doc_init() has been called successfully.
 * @pre lower < upper (strictly; the hardware requires DODSR1 > DODSR0).
 * @post DOCR holds OMS=00, DOBW=0, DCSEL encoding polarity.
 * @post DODSR0 == lower and DODSR1 == upper and DOSR.DOPCF == 0.
 *
 * @note Not thread-safe; caller must serialize with other DOC operations.
 * @see ra8_doc_window_compare() Trigger a comparison and read DOPCF.
 * @see ra8_doc_init() Must be called before this function.
 *
 * @since 0.1.0
 *
 * @par HUM:
 * Ch 57.2.1 "DOCR : DOC Control Register" p 3519 -- DCSEL[2:0] inside (100b)
 * and outside (101b) window modes.
 * Ch 57.2.5 "DODSR0 : DOC Data Setting Register 0" p 3521 -- lower bound.
 * Ch 57.2.6 "DODSR1 : DOC Data Setting Register 1" p 3522 -- upper bound.
 */
[[nodiscard]] ra8_err_t
ra8_doc_set_window(uint16_t lower, uint16_t upper, ra8_doc_window_polarity_t polarity);

/**
 * @brief Trigger a 16-bit window comparison and return the DOPCF flag.
 *
 * @details
 * Verifies the DOC is in compare mode (DOCR.OMS == 00), then writes
 * value to DODIR which triggers the hardware comparison against the
 * thresholds programmed by ra8_doc_set_window(). DOSR.DOPCF is read back
 * and returned via out_flag. The flag is cleared before and after the
 * comparison so each call reflects only the single DODIR write.
 *
 * The window decision itself is made by the silicon comparator. The
 * RAM-backed host register file has no comparator engine, so host unit
 * tests stage DOSR.DOPCF before the call to drive both flag legs; the
 * inside/outside/boundary semantics are proven on silicon.
 *
 * @param[in]  value    16-bit data value to compare against the window.
 *                      Range: 0..65535.
 * @param[out] out_flag Set to true if the comparison condition was met
 *                      (DOSR.DOPCF == 1 after the comparison), false
 *                      otherwise. Must not be nullptr.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok               Comparison complete; *out_flag updated.
 * @retval k_ra8_err_null_ptr     out_flag is nullptr.
 * @retval k_ra8_err_invalid_state DOCR.OMS != 0 (not in compare mode;
 *                                call ra8_doc_set_window() first).
 *
 * @pre ra8_doc_set_window() has been called to configure the window.
 * @pre out_flag points to writable storage (non-NULL).
 * @post *out_flag reflects the DOSR.DOPCF result of the comparison.
 * @post DOSR.DOPCF is cleared on exit (DOSCR.DOPCFCL written).
 *
 * @note Not thread-safe; caller must serialize.
 * @see ra8_doc_set_window() Configure the window thresholds first.
 *
 * @since 0.1.0
 *
 * @par HUM:
 * Ch 57.2.4 "DODIR : DOC Data Input Register" p 3521 -- write triggers op.
 * Ch 57.2.2 "DOSR : DOC Flag Status Register" p 3520 -- DOPCF read.
 * Ch 57.2.3 "DOSCR : DOC Status Clear Register" p 3521 -- DOPCFCL clear.
 */
[[nodiscard]] ra8_err_t ra8_doc_window_compare(uint16_t value, bool* out_flag);

#ifdef __cplusplus
}
#endif
