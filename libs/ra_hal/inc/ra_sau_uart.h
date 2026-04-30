/**
 * @file ra_sau_uart.h
 * @brief Serial Array Unit (SAU) UART driver -- placeholder
 *
 * @details
 * Mirrors the FSP `r_sau_uart` API shape (open / send / recv /
 * abort / status / close). The Serial Array Unit (SAU) is the
 * RL78-derived multi-protocol serial block found on RA0/RA2 parts.
 *
 * @warning The Renesas RA8D2 silicon does not carry a SAU block; its
 *          equivalent functionality is provided by the SCI / SCI_B
 *          family (see `ra_sci.h`). This file ships a host-testable
 *          placeholder so portable code keeps compiling.
 *
 * Reference: FSP `r_sau_uart` driver shape.
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
 * @struct ra_sau_uart_cfg_t
 * @brief Configuration descriptor for `ra_sau_uart_open`.
 */
/* cppcheck-suppress-begin [unusedStructMember] */
typedef struct {
  uint8_t  channel;   /**< SAU channel (0..3 typical).       */
  uint32_t baud;      /**< Baud rate in bits/s.              */
  uint8_t  data_bits; /**< Data bits per frame (7..9).       */
  uint8_t  parity;    /**< 0 none, 1 even, 2 odd.            */
  uint8_t  stop_bits; /**< 1 or 2.                           */
} ra_sau_uart_cfg_t;
/* cppcheck-suppress-end [unusedStructMember] */

/**
 * @brief Open the SAU UART driver.
 *
 * @param[in] cfg Configuration. Must not be NULL.
 *
 * @return `ra_err_t` error code.
 * @retval k_ra_ok Driver opened.
 * @retval k_ra_err_null_ptr `cfg` was NULL.
 * @retval k_ra_err_invalid_arg `baud` zero.
 * @retval k_ra_err_exists Already opened.
 *
 * @pre Single-threaded init context.
 *
 * @post Driver in open state.
 * @post `ra_sau_uart_close` will succeed.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_sau_uart_open(const ra_sau_uart_cfg_t* cfg);

/**
 * @brief Send `len` bytes from `src`.
 *
 * @param[in] src Source buffer.
 * @param[in] len Byte count.
 *
 * @return `ra_err_t` error code.
 * @retval k_ra_err_null_ptr `src` was NULL.
 * @retval k_ra_err_invalid_arg `len` zero.
 * @retval k_ra_err_not_initialized Driver was never opened.
 * @retval k_ra_err_not_supported Placeholder.
 *
 * @pre `ra_sau_uart_open` succeeded.
 * @post On real silicon: bytes shifted out.
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_sau_uart_send(const uint8_t* src, uint32_t len);

/**
 * @brief Receive `len` bytes into `dst`.
 *
 * @param[out] dst Destination buffer.
 * @param[in]  len Byte count.
 *
 * @return `ra_err_t` error code.
 * @retval k_ra_err_null_ptr `dst` was NULL.
 * @retval k_ra_err_invalid_arg `len` zero.
 * @retval k_ra_err_not_initialized Driver was never opened.
 * @retval k_ra_err_not_supported Placeholder.
 *
 * @pre `ra_sau_uart_open` succeeded.
 * @post On real silicon: `dst` filled.
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_sau_uart_recv(uint8_t* dst, uint32_t len);

/**
 * @brief Abort an in-flight transfer.
 *
 * @return `ra_err_t` error code.
 * @retval k_ra_ok Abort accepted.
 * @retval k_ra_err_not_initialized Driver was never opened.
 *
 * @pre `ra_sau_uart_open` succeeded.
 * @post No transfer in progress.
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_sau_uart_abort(void);

/**
 * @brief Snapshot the open / busy flags.
 *
 * @param[out] out_open Receives 1 if open.
 * @param[out] out_busy Receives 1 if a transfer is in flight.
 *
 * @return `ra_err_t` error code.
 * @retval k_ra_ok Status copied.
 * @retval k_ra_err_null_ptr Either pointer NULL.
 *
 * @pre Pointers reference writable memory.
 * @post `*out_open` and `*out_busy` reflect driver state.
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_sau_uart_status_get(uint8_t* out_open, uint8_t* out_busy);

/**
 * @brief Close the SAU UART driver.
 *
 * @return `ra_err_t` error code.
 * @retval k_ra_ok Released.
 * @retval k_ra_err_invalid_state Driver was never opened.
 *
 * @pre Single-threaded shutdown context.
 * @post Subsequent operations return `k_ra_err_not_initialized`.
 * @post `ra_sau_uart_open` may be called again.
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_sau_uart_close(void);

#ifdef __cplusplus
}
#endif
