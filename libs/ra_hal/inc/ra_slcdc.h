/**
 * @file ra_slcdc.h
 * @brief Segment LCD Controller (SLCDC) driver header
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Public API for an FSP-shaped Segment LCD Controller driver. SLCDC
 * drives passive segment-LCD glass and lives on smaller display-less
 * RA SKUs. The RA8D2 instead has the GLCDC parallel TFT controller;
 * this driver exposes the FSP ``r_slcdc`` open / write / on / off /
 * close surface so example projects targeting other RA chips build
 * unchanged.
 *
 * @warning RA8D2 silicon may not include this peripheral; verify
 *          against the BSP feature header before flashing. State is
 *          kept in a tiny static buffer -- no register writes are
 *          performed.
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
 * @enum ra_slcdc_bias_t
 * @brief Bias-method selector (mirrors FSP ``slcdc_bias_method_t``).
 */
typedef enum : uint8_t {
  k_ra_slcdc_bias_1_2 = 0U, /**< 1/2 bias method.    */
  k_ra_slcdc_bias_1_3 = 1U, /**< 1/3 bias method.    */
  k_ra_slcdc_bias_1_4 = 2U, /**< 1/4 bias method.    */
} ra_slcdc_bias_t;

/**
 * @enum ra_slcdc_drive_volt_gen_t
 * @brief Drive-voltage generator selector.
 */
typedef enum : uint8_t {
  k_ra_slcdc_drive_internal        = 0U, /**< Internal voltage boost.       */
  k_ra_slcdc_drive_capacitor_split = 1U, /**< Capacitor-split method.      */
  k_ra_slcdc_drive_external        = 2U, /**< External resistor divider.    */
} ra_slcdc_drive_volt_gen_t;

/**
 * @enum ra_slcdc_limits_t
 * @brief Backing-buffer dimensions.
 */
typedef enum : uint8_t {
  k_ra_slcdc_segment_bytes = 16U, /**< Up to 128 segments.   */
} ra_slcdc_limits_t;

/**
 * @struct ra_slcdc_cfg_t
 * @brief Open-time configuration descriptor.
 */
/* cppcheck-suppress-begin [unusedStructMember] */
typedef struct {
  ra_slcdc_bias_t           bias;       /**< Bias selector.            */
  ra_slcdc_drive_volt_gen_t drive_volt; /**< Drive-voltage source.     */
  uint8_t                   time_slice; /**< Number of time slices.    */
  uint16_t                  frame_rate; /**< Frame rate in Hz.         */
  uint8_t                   contrast;   /**< Contrast 0..15.           */
} ra_slcdc_cfg_t;
/* cppcheck-suppress-end [unusedStructMember] */

/**
 * @brief Open the SLCDC.
 * @param[in] cfg Non-NULL configuration descriptor.
 * @return ``ra_err_t`` error code.
 * @pre IRQs masked.
 * @pre ``cfg->time_slice`` is non-zero.
 * @post Driver is open.
 * @post Display buffer is cleared.
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_slcdc_open(const ra_slcdc_cfg_t* cfg);

/**
 * @brief Close the SLCDC.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_slcdc_close(void);

/**
 * @brief Turn the display on.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_slcdc_on(void);

/**
 * @brief Turn the display off.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_slcdc_off(void);

/**
 * @brief Write one segment-byte into the back buffer.
 * @param[in] index Segment-byte index 0..15.
 * @param[in] value Bit pattern (each bit = one segment).
 * @return ``ra_err_t`` error code.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_slcdc_write(uint8_t index, uint8_t value);

/**
 * @brief Read packed status word.
 * @param[out] out_status Bit 0 = open, bit 1 = display-on.
 * @return ``ra_err_t`` error code.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_slcdc_get_status(uint8_t* out_status);

#ifdef __cplusplus
}
#endif
