/**
 * @file ra_uart.h
 * @brief Polling UART driver (SCI async mode) public header
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
 * @brief Initialise an SCI channel in 8-N-1 UART mode.
 *
 * @param[in] channel SCI channel number (0..9).
 * @param[in] brr     Pre-computed BRR register value.
 * @return `ra_err_t` error code.
 */
[[nodiscard]] ra_err_t ra_uart_init(uint8_t channel, uint8_t brr);

/**
 * @brief Polling TX of a single byte.
 *
 * @param[in] channel SCI channel number.
 * @param[in] byte    Byte to transmit.
 * @return `ra_err_t` error code (`k_ra_err_hw_timeout` on TDRE poll failure).
 */
[[nodiscard]] ra_err_t ra_uart_putc(uint8_t channel, uint8_t byte);

#ifdef __cplusplus
}
#endif
