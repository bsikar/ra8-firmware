/**
 * @file ra_iic.h
 * @brief I2C (IIC) polling-mode driver header
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
 * @brief Initialise an IIC channel as an I2C controller.
 *
 * @param[in] channel IIC channel number (0..2).
 * @return `ra_err_t` error code.
 *
 * @note Pin routing + MSTP gating must be done before calling this.
 */
[[nodiscard]] ra_err_t ra_iic_controller_init(uint8_t channel);

/**
 * @brief Blocking write of `len` bytes to a 7-bit target.
 *
 * @param[in] channel    IIC channel number.
 * @param[in] target_7b  7-bit slave address.
 * @param[in] data       Bytes to send.
 * @param[in] len        Number of bytes.
 * @return `ra_err_t` error code.
 */
[[nodiscard]] ra_err_t
ra_iic_write(uint8_t channel, uint8_t target_7b, const uint8_t* data, uint32_t len);

/**
 * @brief Blocking read of `len` bytes from a 7-bit target.
 *
 * @param[in] channel    IIC channel number.
 * @param[in] target_7b  7-bit slave address.
 * @param[out] out       Receive buffer.
 * @param[in] len        Number of bytes to read.
 * @return `ra_err_t` error code.
 */
[[nodiscard]] ra_err_t ra_iic_read(uint8_t channel, uint8_t target_7b, uint8_t* out, uint32_t len);

#ifdef __cplusplus
}
#endif
