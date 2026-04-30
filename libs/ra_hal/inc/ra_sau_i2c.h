/**
 * @file ra_sau_i2c.h
 * @brief Serial Array Unit (SAU) I2C controller driver -- placeholder
 *
 * @details
 * Mirrors the FSP `r_sau_i2c` API shape (open / write / read /
 * abort / status / close). The SAU I2C is a "simplified I2C" mode
 * of the SAU block on RA0 / RA2 parts (no clock-stretch / no
 * arbitration loss handling -- just controller-side push-pull).
 *
 * @warning The Renesas RA8D2 silicon does not carry a SAU block.
 *          This file ships a host-testable placeholder so portable
 *          code keeps compiling.
 *
 * Reference: FSP `r_sau_i2c` driver shape.
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
 * @struct ra_sau_i2c_cfg_t
 * @brief Configuration descriptor for `ra_sau_i2c_open`.
 */
/* cppcheck-suppress-begin [unusedStructMember] */
typedef struct {
  uint8_t  channel;    /**< SAU channel.                    */
  uint32_t rate_hz;    /**< Clock rate in Hz.               */
  uint16_t slave_addr; /**< Default 7-bit peripheral addr.  */
} ra_sau_i2c_cfg_t;
/* cppcheck-suppress-end [unusedStructMember] */

/**
 * @brief Open the SAU I2C driver.
 *
 * @param[in] cfg Configuration. Must not be NULL.
 *
 * @return `ra_err_t`.
 * @retval k_ra_ok Driver opened.
 * @retval k_ra_err_null_ptr `cfg` was NULL.
 * @retval k_ra_err_invalid_arg `rate_hz` zero.
 * @retval k_ra_err_exists Already opened.
 *
 * @pre Single-threaded init context.
 * @post Driver in open state.
 * @post `ra_sau_i2c_close` will succeed.
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_sau_i2c_open(const ra_sau_i2c_cfg_t* cfg);

/**
 * @brief Write `len` bytes to the configured peripheral.
 *
 * @param[in] src Source buffer.
 * @param[in] len Byte count.
 *
 * @return `ra_err_t`.
 * @retval k_ra_err_null_ptr `src` was NULL.
 * @retval k_ra_err_invalid_arg `len` zero.
 * @retval k_ra_err_not_initialized Driver was never opened.
 * @retval k_ra_err_not_supported Placeholder.
 *
 * @pre `ra_sau_i2c_open` succeeded.
 * @post On real silicon: bytes shifted out.
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_sau_i2c_write(const uint8_t* src, uint32_t len);

/**
 * @brief Read `len` bytes from the configured peripheral.
 *
 * @param[out] dst Destination buffer.
 * @param[in]  len Byte count.
 *
 * @return `ra_err_t`.
 * @retval k_ra_err_null_ptr `dst` was NULL.
 * @retval k_ra_err_invalid_arg `len` zero.
 * @retval k_ra_err_not_initialized Driver was never opened.
 * @retval k_ra_err_not_supported Placeholder.
 *
 * @pre `ra_sau_i2c_open` succeeded.
 * @post On real silicon: `dst` filled.
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_sau_i2c_read(uint8_t* dst, uint32_t len);

/**
 * @brief Abort an in-flight transfer.
 *
 * @return `ra_err_t`.
 * @retval k_ra_ok Abort accepted.
 * @retval k_ra_err_not_initialized Driver was never opened.
 *
 * @pre `ra_sau_i2c_open` succeeded.
 * @post No transfer in progress.
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_sau_i2c_abort(void);

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
[[nodiscard]] ra_err_t ra_sau_i2c_status_get(uint8_t* out_open, uint8_t* out_busy);

/**
 * @brief Close the SAU I2C driver.
 *
 * @return `ra_err_t`.
 * @retval k_ra_ok Released.
 * @retval k_ra_err_invalid_state Driver was never opened.
 *
 * @pre Single-threaded shutdown context.
 * @post Subsequent operations return `k_ra_err_not_initialized`.
 * @post `ra_sau_i2c_open` may be called again.
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_sau_i2c_close(void);

#ifdef __cplusplus
}
#endif
