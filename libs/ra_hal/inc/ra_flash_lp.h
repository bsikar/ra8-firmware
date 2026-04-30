/**
 * @file ra_flash_lp.h
 * @brief Low-power flash variant (FLASH_LP) driver -- placeholder
 *
 * @details
 * Mirrors the FSP `r_flash_lp` API shape (open / write / erase /
 * blank-check / status / close) so application code targeting RA0 /
 * RA2 / RA4 low-power MCUs can be ported here unchanged.
 *
 * @warning The Renesas RA8D2 silicon ships **MRAM** as code memory
 *          (HUM Ch 1 "Overview"), not embedded flash, so neither the
 *          `flash_hp` nor `flash_lp` block exists on this part. This
 *          file ships a host-testable placeholder that exposes the
 *          FSP-shaped API and rejects every program / erase op with
 *          `k_ra_err_not_supported` so misuse is loud, while still
 *          letting code that merely opens / closes the driver link.
 *
 * Reference: FSP `r_flash_lp` driver shape.
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
 * @struct ra_flash_lp_cfg_t
 * @brief Configuration descriptor for `ra_flash_lp_open`.
 */
/* cppcheck-suppress-begin [unusedStructMember] */
typedef struct {
  uint32_t fclk_hz;      /**< Flash clock in Hz (real HW: 1..32 MHz). */
  uint8_t  data_flash;   /**< Non-zero to enable data-flash region.   */
  uint8_t  bg_operation; /**< Non-zero to enable BGO (background ops).*/
} ra_flash_lp_cfg_t;
/* cppcheck-suppress-end [unusedStructMember] */

/**
 * @brief Open the FLASH_LP driver.
 *
 * @param[in] cfg Configuration. Must not be NULL.
 *
 * @return `ra_err_t` error code.
 * @retval k_ra_ok Driver opened.
 * @retval k_ra_err_null_ptr `cfg` was NULL.
 * @retval k_ra_err_invalid_arg `cfg->fclk_hz` zero.
 * @retval k_ra_err_exists Driver already opened.
 *
 * @pre Single-threaded init context.
 *
 * @post Driver in open state.
 * @post `ra_flash_lp_close` will succeed.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_flash_lp_open(const ra_flash_lp_cfg_t* cfg);

/**
 * @brief Programme `len` bytes at `flash_addr`.
 *
 * @param[in] flash_addr Destination flash address.
 * @param[in] src        Source buffer.
 * @param[in] len        Byte count.
 *
 * @return `ra_err_t` error code.
 * @retval k_ra_err_null_ptr `src` was NULL.
 * @retval k_ra_err_invalid_arg `len` zero.
 * @retval k_ra_err_not_initialized Driver was never opened.
 * @retval k_ra_err_not_supported Real silicon would programme; placeholder
 *                                refuses (no flash on RA8D2).
 *
 * @pre `ra_flash_lp_open` succeeded.
 *
 * @post No flash mutation performed.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_flash_lp_write(uint32_t flash_addr, const uint8_t* src, uint32_t len);

/**
 * @brief Erase `block_count` blocks starting at `flash_addr`.
 *
 * @param[in] flash_addr Block-aligned address.
 * @param[in] block_count Number of blocks to erase.
 *
 * @return `ra_err_t` error code.
 * @retval k_ra_err_invalid_arg `block_count` zero.
 * @retval k_ra_err_not_initialized Driver was never opened.
 * @retval k_ra_err_not_supported Placeholder.
 *
 * @pre `ra_flash_lp_open` succeeded.
 *
 * @post No flash mutation performed.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_flash_lp_erase(uint32_t flash_addr, uint32_t block_count);

/**
 * @brief Blank-check a flash region.
 *
 * @param[in]  flash_addr Region base.
 * @param[in]  len        Byte count.
 * @param[out] out_blank  Receives 1 if blank, else 0 (placeholder always 0).
 *
 * @return `ra_err_t` error code.
 * @retval k_ra_ok Result copied.
 * @retval k_ra_err_null_ptr `out_blank` was NULL.
 * @retval k_ra_err_invalid_arg `len` zero.
 * @retval k_ra_err_not_initialized Driver was never opened.
 *
 * @pre `ra_flash_lp_open` succeeded.
 *
 * @post `*out_blank` is defined.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t
ra_flash_lp_blank_check(uint32_t flash_addr, uint32_t len, uint8_t* out_blank);

/**
 * @brief Snapshot the open / busy flags.
 *
 * @param[out] out_open Receives 1 if open.
 * @param[out] out_busy Receives 1 if a long op is in flight.
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
[[nodiscard]] ra_err_t ra_flash_lp_status_get(uint8_t* out_open, uint8_t* out_busy);

/**
 * @brief Close the FLASH_LP driver.
 *
 * @return `ra_err_t` error code.
 * @retval k_ra_ok Released.
 * @retval k_ra_err_invalid_state Driver was never opened.
 *
 * @pre Single-threaded shutdown context.
 *
 * @post Subsequent operations return `k_ra_err_not_initialized`.
 * @post `ra_flash_lp_open` may be called again.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_flash_lp_close(void);

#ifdef __cplusplus
}
#endif
