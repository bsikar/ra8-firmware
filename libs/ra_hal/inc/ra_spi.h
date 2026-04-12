/**
 * @file ra_spi.h
 * @brief SPI polling driver public header
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
 * @brief Initialise a standard SPI channel as an 8-bit master.
 *
 * @param[in] channel SPI channel (0 or 1).
 * @return `ra_err_t` error code.
 */
[[nodiscard]] ra_err_t ra_spi_master_init(uint8_t channel);

/**
 * @brief Full-duplex 8-bit exchange.
 *
 * @param[in]  channel SPI channel (0 or 1).
 * @param[in]  tx      Byte to transmit.
 * @param[out] rx      Pointer to receive the shifted-in byte (may be NULL).
 * @return `ra_err_t` error code.
 */
[[nodiscard]] ra_err_t ra_spi_xfer8(uint8_t channel, uint8_t tx, uint8_t* rx);

#ifdef __cplusplus
}
#endif
