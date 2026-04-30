/**
 * @file ra_iica_slave.h
 * @brief IICA peripheral (slave) driver -- placeholder
 *
 * @details
 * Mirrors the FSP `r_iica_slave` API shape (open / read / write /
 * status / close). Companion to `ra_iica_master.h`.
 *
 * @warning The Renesas RA8D2 silicon does not carry an IICA block
 *          (its automotive-grade I2C is the IIC_B family). This file
 *          ships a host-testable placeholder so portable code keeps
 *          compiling; transfer ops return `k_ra_err_not_supported`.
 *
 * Reference: FSP `r_iica_slave` driver shape.
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
 * @struct ra_iica_slave_cfg_t
 * @brief Configuration descriptor for `ra_iica_slave_open`.
 */
/* cppcheck-suppress-begin [unusedStructMember] */
typedef struct {
  uint8_t  channel;      /**< IICA channel index.                */
  uint16_t own_addr;     /**< Local 7- or 10-bit address.        */
  uint8_t  general_call; /**< Non-zero to honour general-call.   */
} ra_iica_slave_cfg_t;
/* cppcheck-suppress-end [unusedStructMember] */

/**
 * @brief Open the IICA slave driver.
 *
 * @param[in] cfg Configuration. Must not be NULL.
 *
 * @return `ra_err_t` error code.
 * @retval k_ra_ok Driver opened.
 * @retval k_ra_err_null_ptr `cfg` was NULL.
 * @retval k_ra_err_invalid_arg `own_addr` exceeds 10-bit range.
 * @retval k_ra_err_exists Driver already opened.
 *
 * @pre Single-threaded init context.
 *
 * @post Driver in open state.
 * @post `ra_iica_slave_close` will succeed.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_iica_slave_open(const ra_iica_slave_cfg_t* cfg);

/**
 * @brief Service a controller-write transaction (data flowing into us).
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
 * @pre `ra_iica_slave_open` succeeded.
 *
 * @post On real silicon: `dst` filled.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_iica_slave_read(uint8_t* dst, uint32_t len);

/**
 * @brief Service a controller-read transaction (data flowing out of us).
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
 * @pre `ra_iica_slave_open` succeeded.
 *
 * @post On real silicon: bytes shifted out.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_iica_slave_write(const uint8_t* src, uint32_t len);

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
[[nodiscard]] ra_err_t ra_iica_slave_status_get(uint8_t* out_open, uint8_t* out_busy);

/**
 * @brief Close the IICA slave driver.
 *
 * @return `ra_err_t` error code.
 * @retval k_ra_ok Released.
 * @retval k_ra_err_invalid_state Driver was never opened.
 *
 * @pre Single-threaded shutdown context.
 *
 * @post Subsequent operations return `k_ra_err_not_initialized`.
 * @post `ra_iica_slave_open` may be called again.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_iica_slave_close(void);

#ifdef __cplusplus
}
#endif
