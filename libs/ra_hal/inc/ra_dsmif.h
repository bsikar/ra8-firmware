/**
 * @file ra_dsmif.h
 * @brief Delta-Sigma Demodulator Interface (DSMIF) driver -- placeholder
 *
 * @details
 * Mirrors the FSP `r_dsmif` API shape (open / scan-start / scan-stop /
 * read / status / close) so application code targeting RA-class motor
 * boards can be ported here unchanged. The DSMIF demodulates the
 * 1-bit bitstream coming back from external delta-sigma modulators
 * (typically isolated current sensors on motor inverter legs) and
 * presents 16-bit / 32-bit current samples to the host.
 *
 * @warning The Renesas RA8D2 silicon does not carry a DSMIF block
 *          (see HUM Ch 1 "Overview" feature matrix). This file
 *          ships a host-testable placeholder that exposes the
 *          FSP-shaped API and rejects every operation with
 *          `k_ra_err_not_supported` once the driver is open. The
 *          unit tests exercise NULL-arg / lifecycle behaviour only.
 *
 * Reference: FSP `r_dsmif` driver shape.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra_err.h"

/**
 * @enum ra_dsmif_channel_t
 * @brief Logical DSMIF channel identifier.
 *
 * @details
 * On real silicon this maps to one of the demodulator front-ends
 * (typically up to six on motor variants). The placeholder accepts
 * any value below `k_ra_dsmif_channel_count`.
 */
typedef enum : uint8_t {
  k_ra_dsmif_channel_0     = 0U, /**< Channel 0. */
  k_ra_dsmif_channel_1     = 1U, /**< Channel 1. */
  k_ra_dsmif_channel_2     = 2U, /**< Channel 2. */
  k_ra_dsmif_channel_3     = 3U, /**< Channel 3. */
  k_ra_dsmif_channel_4     = 4U, /**< Channel 4. */
  k_ra_dsmif_channel_5     = 5U, /**< Channel 5. */
  k_ra_dsmif_channel_count = 6U, /**< Range guard. */
} ra_dsmif_channel_t;

/**
 * @struct ra_dsmif_cfg_t
 * @brief Configuration descriptor for `ra_dsmif_open`.
 *
 * @details
 * A trimmed-down mirror of FSP `adc_cfg_t` carrying only the fields
 * the placeholder needs to emulate. `channel_mask` is a bitmask of
 * `1U << k_ra_dsmif_channel_n` selecting the channels the caller
 * wants enabled at scan-start time.
 */
/* cppcheck-suppress-begin [unusedStructMember] */
typedef struct {
  uint8_t  channel_mask;     /**< Bitmask of enabled channels.   */
  uint16_t decimation_ratio; /**< Decimation N (real HW: 16..256).*/
} ra_dsmif_cfg_t;
/* cppcheck-suppress-end [unusedStructMember] */

/**
 * @brief Open the DSMIF driver.
 *
 * @details
 * Validates `cfg` and latches it into the placeholder state. On real
 * silicon this would unmap the module-stop bit, programme the
 * decimation and oversampling registers, and arm the demodulator
 * front-end. Here it simply transitions the driver into the open
 * state so subsequent lifecycle calls behave correctly.
 *
 * @param[in] cfg Configuration. Must not be NULL.
 *
 * @return `ra_err_t` error code.
 * @retval k_ra_ok Driver opened.
 * @retval k_ra_err_null_ptr `cfg` was NULL.
 * @retval k_ra_err_invalid_arg `channel_mask` selects no channels.
 * @retval k_ra_err_exists Driver already opened.
 *
 * @pre Single-threaded init context.
 *
 * @post Driver is in the open state.
 * @post `ra_dsmif_close` will succeed.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_dsmif_open(const ra_dsmif_cfg_t* cfg);

/**
 * @brief Start a scan.
 *
 * @return `ra_err_t` error code.
 * @retval k_ra_ok Scan accepted (placeholder always reports success).
 * @retval k_ra_err_not_initialized Driver was never opened.
 * @retval k_ra_err_not_supported Real silicon would do work; placeholder
 *                                does not.
 *
 * @pre `ra_dsmif_open` succeeded.
 *
 * @post On real silicon: scan engine is running.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_dsmif_scan_start(void);

/**
 * @brief Stop the scan.
 *
 * @return `ra_err_t` error code.
 * @retval k_ra_ok Stop accepted.
 * @retval k_ra_err_not_initialized Driver was never opened.
 *
 * @pre `ra_dsmif_open` succeeded.
 *
 * @post On real silicon: scan engine is stopped.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_dsmif_scan_stop(void);

/**
 * @brief Read the latest 16-bit sample from a channel.
 *
 * @param[in]  ch       Channel id (must be < k_ra_dsmif_channel_count).
 * @param[out] out_data Receives the 16-bit sample (placeholder writes 0).
 *
 * @return `ra_err_t` error code.
 * @retval k_ra_ok Sample copied.
 * @retval k_ra_err_null_ptr `out_data` was NULL.
 * @retval k_ra_err_invalid_arg `ch` out of range.
 * @retval k_ra_err_not_initialized Driver was never opened.
 * @retval k_ra_err_not_supported Real-silicon read would block.
 *
 * @pre `ra_dsmif_open` succeeded.
 *
 * @post `*out_data` is defined (zero on placeholder).
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_dsmif_read(ra_dsmif_channel_t ch, uint16_t* out_data);

/**
 * @brief Read the latest 32-bit sample from a channel.
 *
 * @param[in]  ch       Channel id.
 * @param[out] out_data Receives the 32-bit sample (placeholder writes 0).
 *
 * @return `ra_err_t` error code.
 * @retval k_ra_ok Sample copied.
 * @retval k_ra_err_null_ptr `out_data` was NULL.
 * @retval k_ra_err_invalid_arg `ch` out of range.
 * @retval k_ra_err_not_initialized Driver was never opened.
 *
 * @pre `ra_dsmif_open` succeeded.
 *
 * @post `*out_data` is defined.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_dsmif_read32(ra_dsmif_channel_t ch, uint32_t* out_data);

/**
 * @brief Snapshot the open / scanning flags.
 *
 * @param[out] out_open     Receives 1 if the driver is open.
 * @param[out] out_scanning Receives 1 if a scan is active.
 *
 * @return `ra_err_t` error code.
 * @retval k_ra_ok Status copied.
 * @retval k_ra_err_null_ptr Either pointer was NULL.
 *
 * @pre Pointers reference writable memory.
 *
 * @post `*out_open` and `*out_scanning` reflect driver state.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_dsmif_status_get(uint8_t* out_open, uint8_t* out_scanning);

/**
 * @brief Close the DSMIF driver.
 *
 * @return `ra_err_t` error code.
 * @retval k_ra_ok Released.
 * @retval k_ra_err_invalid_state Driver was never opened.
 *
 * @pre Single-threaded shutdown context.
 *
 * @post Subsequent operations return `k_ra_err_not_initialized`.
 * @post `ra_dsmif_open` may be called again.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_dsmif_close(void);

#ifdef __cplusplus
}
#endif
