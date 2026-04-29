/**
 * @file ra_ctsu.h
 * @brief Capacitive Touch Sensing Unit (CTSU2) driver
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Mirrors the public surface of FSP ``r_ctsu`` in the project HAL
 * style. The driver supports self-capacitance and mutual-capacitance
 * scan modes, programmable measurement clock divider, per-electrode
 * enable bitmaps and a software-side touch threshold table that FSP
 * also keeps in driver state rather than silicon.
 *
 * Reference: HUM Ch 51 "Capacitive Touch Sensing Unit (CTSU)" plus
 * the Renesas FSP ``r_ctsu.c`` implementation.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8d2_ctsu_regs.h"
#include "ra_err.h"

/**
 * @enum ra_ctsu_mode_t
 * @brief Scan / measurement mode written to ``CTSUCR1.MD[1:0]``.
 */
typedef enum : uint8_t {
  k_ra_ctsu_mode_self_single = 0U, /**< Self-cap, single scan. */
  k_ra_ctsu_mode_self_multi  = 1U, /**< Self-cap, multi-scan (FSP default). */
  k_ra_ctsu_mode_mutual_full = 2U, /**< Mutual-cap, full scan. */
  k_ra_ctsu_mode_current     = 3U, /**< Current measurement (calibration). */
} ra_ctsu_mode_t;

/**
 * @enum ra_ctsu_clock_div_t
 * @brief Measurement clock divider written to ``CTSUDCLKC``.
 */
typedef enum : uint8_t {
  k_ra_ctsu_clock_div_2  = 0U, /**< PCLKB / 2. */
  k_ra_ctsu_clock_div_4  = 1U, /**< PCLKB / 4. */
  k_ra_ctsu_clock_div_8  = 2U, /**< PCLKB / 8. */
  k_ra_ctsu_clock_div_16 = 3U, /**< PCLKB / 16 (FSP default). */
} ra_ctsu_clock_div_t;

/**
 * @struct ra_ctsu_cfg_t
 * @brief Open-time configuration descriptor.
 *
 * @details
 * Mirrors the relevant subset of FSP ``ctsu_instance_ctrl_t`` /
 * ``ctsu_cfg_t`` open arguments. ``electrode_mask`` is a packed
 * 36-bit-into-uint64_t bitmap of TS pins to enable in
 * CTSUCHAC0..CTSUCHAC4. ``transmit_mask`` selects the transmit-side
 * electrodes for mutual-cap scans (CTSUCHTRC0..CTSUCHTRC4) and is
 * ignored for self-cap modes.
 */
/* cppcheck-suppress-begin [unusedStructMember] */
typedef struct {
  ra_ctsu_mode_t      mode;           /**< Scan-mode select. */
  ra_ctsu_clock_div_t clock_div;      /**< Measurement clock divider. */
  uint8_t             scan_period;    /**< CTSUSDPRS scan-period setting. */
  uint8_t             stabilise_wait; /**< CTSUSST sensor-stabilisation wait. */
  uint64_t            electrode_mask; /**< CHAC0..CHAC4 packed bitmap. */
  uint64_t            transmit_mask;  /**< CHTRC0..CHTRC4 packed bitmap. */
  uint16_t            offset_initial; /**< Initial CTSUSO0 sensor offset. */
} ra_ctsu_cfg_t;
/* cppcheck-suppress-end [unusedStructMember] */

/**
 * @typedef ra_ctsu_event_fn_t
 * @brief Scan-complete / error callback signature.
 *
 * @param[in] ctx Caller context pointer.
 * @param[in] electrode Electrode index that fired the event (0..35).
 * @param[in] err ``k_ra_ok`` for scan-complete, otherwise an error code.
 */
typedef void (*ra_ctsu_event_fn_t)(void* ctx, uint8_t electrode, ra_err_t err);

/* =============================================================================
 * Lifecycle
 * =============================================================================
 */

/**
 * @brief Power up the CTSU block and apply ``cfg``.
 *
 * @details
 * 1. Validate ``cfg`` and the electrode mask.
 * 2. Software-reset CTSU via ``CTSUCR0.INIT``.
 * 3. Set ``CTSUCR0.PON`` and clear INIT.
 * 4. Programme mode, clock divider, scan period, stabilise wait,
 * electrode and transmit bitmaps, and the initial sensor offset.
 *
 * @param[in] cfg Non-NULL configuration descriptor.
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Block configured and powered.
 * @retval k_ra_err_null_ptr ``cfg`` was nullptr.
 * @retval k_ra_err_invalid_arg Reserved field or all-zero mask.
 *
 * @pre Caller holds single-threaded init context.
 * @pre IRQs masked.
 * @post CTSUCR0.PON == 1.
 * @post Driver state machine moved from CLOSED to OPEN.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_ctsu_open(const ra_ctsu_cfg_t* cfg);

/**
 * @brief Power down the CTSU block.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Block powered off and driver state CLOSED.
 *
 * @pre Driver was previously opened.
 * @post CTSUCR0.PON == 0.
 * @post Subsequent calls (other than open) return ``k_ra_err_not_initialized``.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_ctsu_close(void);

/* =============================================================================
 * Scan control
 * =============================================================================
 */

/**
 * @brief Kick a measurement by setting ``CTSUMR.STRT`` (CTSUCR0.STRT).
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Scan started.
 * @retval k_ra_err_not_initialized ``ra_ctsu_open`` not called.
 *
 * @pre Block is open.
 * @pre Previous scan has completed (DTSR bit clear).
 *
 * @post STRT bit asserted in CTSUCR0.
 * @post Driver state moved from OPEN to SCANNING.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_ctsu_scan_start(void);

/**
 * @brief Cancel an in-flight scan by clearing ``CTSUMR.STRT``.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok STRT cleared and driver state restored to OPEN.
 * @retval k_ra_err_not_initialized ``ra_ctsu_open`` not called.
 *
 * @pre Block is open.
 * @post STRT bit cleared in CTSUCR0.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_ctsu_scan_stop(void);

/**
 * @brief Read the most recent raw count for an electrode.
 *
 * @param[in]  electrode Electrode index 0..35.
 * @param[out] raw_count Receives ``CTSUSC0 | (CTSUSC1 << 16)``.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Result valid.
 * @retval k_ra_err_null_ptr ``raw_count`` was nullptr.
 * @retval k_ra_err_invalid_arg Electrode index out of range.
 * @retval k_ra_err_not_initialized Driver not opened.
 *
 * @pre Driver opened.
 * @pre A scan completed since last call (caller-enforced).
 * @post ``*raw_count`` populated.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_ctsu_get_data(uint8_t electrode, uint32_t* raw_count);

/* =============================================================================
 * Callback / IRQ plumbing
 * =============================================================================
 */

/**
 * @brief Register the scan-complete + error callback.
 *
 * @param[in] fn Event callback (may be nullptr to detach).
 * @param[in] ctx Opaque context passed back unchanged.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Callback installed (or detached).
 *
 * @pre Driver state is any (the callback table is module-static).
 * @post The next dispatched event invokes ``fn(ctx, ...)``.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_ctsu_attach_handler(ra_ctsu_event_fn_t fn, void* ctx);

/**
 * @brief Dispatch a scan-complete or error event to the registered callback.
 *
 * @param[in] electrode Electrode index that fired (0..35).
 * @param[in] err Result code (``k_ra_ok`` for normal completion).
 *
 * @details
 * Used by the IRQ entry point and unit tests. Out-of-range electrode
 * indices are silently dropped.
 *
 * @since 0.1.0
 */
void ra_ctsu_dispatch(uint8_t electrode, ra_err_t err);

/* =============================================================================
 * Calibration / threshold table
 * =============================================================================
 */

/**
 * @brief Run the FSP-style auto-calibration sequence for one electrode.
 *
 * @details
 * Performs ``k_ra_ctsu_calib_dummy_cnt`` (3) dummy single-channel
 * scans, averages the resulting CTSUSC0 counts, and stores the result
 * as the electrode's offset baseline. CTSUSO0 is updated with the new
 * offset on success.
 *
 * @param[in] electrode Electrode index 0..35.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Offset stored.
 * @retval k_ra_err_invalid_arg Electrode out of range.
 * @retval k_ra_err_not_initialized Driver not opened.
 *
 * @pre Driver opened.
 * @post Internal offset table updated for ``electrode``.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_ctsu_calibrate(uint8_t electrode);

/**
 * @brief Set the touch-detection threshold for an electrode.
 *
 * @details
 * The threshold lives entirely in driver RAM (FSP also keeps it in
 * the host-side ``ctsu_instance_ctrl_t`` rather than silicon). User
 * code typically compares ``raw - offset`` against the threshold.
 *
 * @param[in] electrode Electrode index 0..35.
 * @param[in] threshold Raw count delta that counts as a touch.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Threshold stored.
 * @retval k_ra_err_invalid_arg Electrode out of range.
 *
 * @pre None (callable before open).
 * @post Internal threshold table updated.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_ctsu_set_threshold(uint8_t electrode, uint16_t threshold);

/**
 * @brief Read the previously-stored threshold for an electrode.
 *
 * @param[in]  electrode Electrode index 0..35.
 * @param[out] out_threshold Receives the stored threshold.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Threshold returned.
 * @retval k_ra_err_null_ptr ``out_threshold`` was nullptr.
 * @retval k_ra_err_invalid_arg Electrode out of range.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_ctsu_get_threshold(uint8_t electrode, uint16_t* out_threshold);

#ifdef __cplusplus
}
#endif
