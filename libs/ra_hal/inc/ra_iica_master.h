/**
 * @file ra_iica_master.h
 * @brief IICA controller (master) driver -- placeholder
 *
 * @details
 * Mirrors the FSP `r_iica_master` API shape (open / read / write /
 * abort / address-set / status / close). The IICA block is the
 * automotive-grade I2C peripheral on selected RA0 / RA2 parts.
 *
 * @warning The Renesas RA8D2 silicon does not carry an IICA block;
 *          its automotive-grade I2C is provided by the IIC_B family
 *          (see `ra_iic_b.h`). This file ships a host-testable
 *          placeholder that exposes the FSP-shaped API and rejects
 *          transfer ops with `k_ra_err_not_supported` so misuse is
 *          loud while still allowing portable code to compile.
 *
 * Reference: FSP `r_iica_master` driver shape.
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
 * @enum ra_iica_master_addr_t
 * @brief 7- or 10-bit address mode selector.
 */
typedef enum : uint8_t {
  k_ra_iica_master_addr_7bit  = 0U,
  k_ra_iica_master_addr_10bit = 1U,
  k_ra_iica_master_addr_max   = 2U,
} ra_iica_master_addr_t;

/**
 * @struct ra_iica_master_cfg_t
 * @brief Configuration descriptor for `ra_iica_master_open`.
 */
/* cppcheck-suppress-begin [unusedStructMember] */
typedef struct {
  uint8_t               channel;    /**< IICA channel index.        */
  ra_iica_master_addr_t addr_mode;  /**< 7- or 10-bit addressing.   */
  uint32_t              rate_hz;    /**< Clock rate in Hz.          */
  uint16_t              slave_addr; /**< Slave (peripheral) addr.   */
} ra_iica_master_cfg_t;
/* cppcheck-suppress-end [unusedStructMember] */

/**
 * @brief Open the IICA master driver.
 *
 * @param[in] cfg Configuration. Must not be NULL.
 *
 * @return `ra_err_t` error code.
 * @retval k_ra_ok Driver opened.
 * @retval k_ra_err_null_ptr `cfg` was NULL.
 * @retval k_ra_err_invalid_arg `addr_mode` out of range / `rate_hz` zero.
 * @retval k_ra_err_exists Driver already opened.
 *
 * @pre Single-threaded init context.
 *
 * @post Driver in open state.
 * @post `ra_iica_master_close` will succeed.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_iica_master_open(const ra_iica_master_cfg_t* cfg);

/**
 * @brief Read `len` bytes from the configured peripheral.
 *
 * @param[out] dst Buffer (must not be NULL).
 * @param[in]  len Byte count (must be > 0).
 * @param[in]  restart Non-zero to issue REPEATED-START instead of STOP.
 *
 * @return `ra_err_t` error code.
 * @retval k_ra_err_null_ptr `dst` was NULL.
 * @retval k_ra_err_invalid_arg `len` zero.
 * @retval k_ra_err_not_initialized Driver was never opened.
 * @retval k_ra_err_not_supported Placeholder.
 *
 * @pre `ra_iica_master_open` succeeded.
 *
 * @post On real silicon: `dst` filled.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_iica_master_read(uint8_t* dst, uint32_t len, uint8_t restart);

/**
 * @brief Write `len` bytes to the configured peripheral.
 *
 * @param[in] src Buffer.
 * @param[in] len Byte count.
 * @param[in] restart Non-zero to issue REPEATED-START.
 *
 * @return `ra_err_t` error code.
 * @retval k_ra_err_null_ptr `src` was NULL.
 * @retval k_ra_err_invalid_arg `len` zero.
 * @retval k_ra_err_not_initialized Driver was never opened.
 * @retval k_ra_err_not_supported Placeholder.
 *
 * @pre `ra_iica_master_open` succeeded.
 *
 * @post On real silicon: bytes shifted out.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_iica_master_write(const uint8_t* src, uint32_t len, uint8_t restart);

/**
 * @brief Abort an in-flight transfer.
 *
 * @return `ra_err_t` error code.
 * @retval k_ra_ok Abort accepted.
 * @retval k_ra_err_not_initialized Driver was never opened.
 *
 * @pre `ra_iica_master_open` succeeded.
 *
 * @post No transfer in progress.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_iica_master_abort(void);

/**
 * @brief Update the latched peripheral address.
 *
 * @param[in] new_addr New 7- or 10-bit address.
 *
 * @return `ra_err_t` error code.
 * @retval k_ra_ok Latched.
 * @retval k_ra_err_invalid_arg Address exceeds 10-bit range.
 * @retval k_ra_err_not_initialized Driver was never opened.
 *
 * @pre `ra_iica_master_open` succeeded.
 *
 * @post `_read` / `_write` use `new_addr`.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_iica_master_slave_address_set(uint16_t new_addr);

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
 *
 * @post `*out_open` and `*out_busy` reflect driver state.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_iica_master_status_get(uint8_t* out_open, uint8_t* out_busy);

/**
 * @brief Close the IICA master driver.
 *
 * @return `ra_err_t` error code.
 * @retval k_ra_ok Released.
 * @retval k_ra_err_invalid_state Driver was never opened.
 *
 * @pre Single-threaded shutdown context.
 *
 * @post Subsequent operations return `k_ra_err_not_initialized`.
 * @post `ra_iica_master_open` may be called again.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_iica_master_close(void);

#ifdef __cplusplus
}
#endif
