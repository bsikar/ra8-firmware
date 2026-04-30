/**
 * @file ra_sau_lin.h
 * @brief Serial Array Unit (SAU) LIN driver -- placeholder
 *
 * @details
 * Mirrors the FSP `r_sau_lin` API shape (open / send-header /
 * send-response / recv-response / status / close). LIN is a
 * single-controller automotive bus that the SAU can drive in UART
 * mode with break-detect support on RA0/RA2 parts.
 *
 * @warning The Renesas RA8D2 silicon does not carry a SAU block;
 *          its LIN-style interface is provided by the SCI / SCI_B
 *          family with break support. This file ships a host-
 *          testable placeholder so portable code keeps compiling.
 *
 * Reference: FSP `r_sau_lin` driver shape.
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
 * @struct ra_sau_lin_cfg_t
 * @brief Configuration descriptor for `ra_sau_lin_open`.
 */
/* cppcheck-suppress-begin [unusedStructMember] */
typedef struct {
  uint8_t  channel;    /**< SAU channel index.                */
  uint32_t baud;       /**< Baud rate in bits/s.              */
  uint8_t  break_bits; /**< Break field length (>= 13).       */
} ra_sau_lin_cfg_t;
/* cppcheck-suppress-end [unusedStructMember] */

/**
 * @brief Open the SAU LIN driver.
 *
 * @param[in] cfg Configuration. Must not be NULL.
 *
 * @return `ra_err_t`.
 * @retval k_ra_ok Driver opened.
 * @retval k_ra_err_null_ptr `cfg` was NULL.
 * @retval k_ra_err_invalid_arg `baud` zero.
 * @retval k_ra_err_exists Already opened.
 *
 * @pre Single-threaded init context.
 * @post Driver in open state.
 * @post `ra_sau_lin_close` will succeed.
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_sau_lin_open(const ra_sau_lin_cfg_t* cfg);

/**
 * @brief Send a LIN header (break + sync + PID).
 *
 * @param[in] pid Protected Identifier byte.
 *
 * @return `ra_err_t`.
 * @retval k_ra_ok Header queued.
 * @retval k_ra_err_not_initialized Driver was never opened.
 * @retval k_ra_err_not_supported Placeholder.
 *
 * @pre `ra_sau_lin_open` succeeded.
 * @post On real silicon: header on the wire.
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_sau_lin_send_header(uint8_t pid);

/**
 * @brief Send the response data + checksum.
 *
 * @param[in] src Response buffer.
 * @param[in] len Byte count (0..8).
 *
 * @return `ra_err_t`.
 * @retval k_ra_err_null_ptr `src` was NULL.
 * @retval k_ra_err_invalid_arg `len` zero or > 8.
 * @retval k_ra_err_not_initialized Driver was never opened.
 * @retval k_ra_err_not_supported Placeholder.
 *
 * @pre `ra_sau_lin_open` succeeded.
 * @post On real silicon: response on the wire.
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_sau_lin_send_response(const uint8_t* src, uint32_t len);

/**
 * @brief Receive a response from a peripheral node.
 *
 * @param[out] dst Buffer.
 * @param[in]  len Expected byte count (1..8).
 *
 * @return `ra_err_t`.
 * @retval k_ra_err_null_ptr `dst` was NULL.
 * @retval k_ra_err_invalid_arg `len` zero or > 8.
 * @retval k_ra_err_not_initialized Driver was never opened.
 * @retval k_ra_err_not_supported Placeholder.
 *
 * @pre `ra_sau_lin_open` succeeded.
 * @post On real silicon: `dst` filled.
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_sau_lin_recv_response(uint8_t* dst, uint32_t len);

/**
 * @brief Snapshot the open / busy flags.
 *
 * @param[out] out_open Receives 1 if open.
 * @param[out] out_busy Receives 1 if a transfer is in flight.
 *
 * @return `ra_err_t`.
 * @retval k_ra_ok Status copied.
 * @retval k_ra_err_null_ptr Either pointer NULL.
 *
 * @pre Pointers reference writable memory.
 * @post `*out_open` and `*out_busy` reflect driver state.
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_sau_lin_status_get(uint8_t* out_open, uint8_t* out_busy);

/**
 * @brief Close the SAU LIN driver.
 *
 * @return `ra_err_t`.
 * @retval k_ra_ok Released.
 * @retval k_ra_err_invalid_state Driver was never opened.
 *
 * @pre Single-threaded shutdown context.
 * @post Subsequent operations return `k_ra_err_not_initialized`.
 * @post `ra_sau_lin_open` may be called again.
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_sau_lin_close(void);

#ifdef __cplusplus
}
#endif
