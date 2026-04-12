/**
 * @file ra_glcdc.h
 * @brief Graphics LCD Controller driver (framework)
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8d2_glcdc_regs.h"
#include "ra_err.h"

/**
 * @struct ra_glcdc_config_t
 * @brief Minimal GLCDC configuration for a single-layer panel.
 */
typedef struct {
  uint32_t             framebuffer_addr; /**< SRAM or SDRAM address. */
  uint16_t             width_px;         /**< Visible width.         */
  uint16_t             height_px;        /**< Visible height.        */
  ra_glcdc_pixel_fmt_t format;           /**< Pixel format code.     */
} ra_glcdc_config_t;

/**
 * @brief Initialise GLCDC with the EK-RA8D2 1024x600 timings and the
 *        supplied graphics layer 1 framebuffer.
 */
[[nodiscard]] ra_err_t ra_glcdc_init(const ra_glcdc_config_t* cfg);

/**
 * @brief Start or stop the panel output.
 */
[[nodiscard]] ra_err_t ra_glcdc_start(bool enable);

#ifdef __cplusplus
}
#endif
