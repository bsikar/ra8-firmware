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
 *
 * @details
 * cppcheck cannot see tests/ so it flags every field as unused;
 * each member is read in ``ra_glcdc_init`` in
 * ``libs/ra_hal/src/ra_glcdc.c``.
 */
/* cppcheck-suppress-begin [unusedStructMember] */
typedef struct {
  uint32_t             framebuffer_addr; /**< SRAM or SDRAM address. */
  uint16_t             width_px;         /**< Visible width.         */
  uint16_t             height_px;        /**< Visible height.        */
  ra_glcdc_pixel_fmt_t format;           /**< Pixel format code.     */
} ra_glcdc_config_t;
/* cppcheck-suppress-end [unusedStructMember] */

/**
 * @brief Initialise GLCDC with the EK-RA8D2 1024x600 timings and the
 *        supplied graphics layer 1 framebuffer.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_glcdc_init(const ra_glcdc_config_t* cfg);

/**
 * @brief Start or stop the panel output.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_glcdc_start(bool enable);

/**
 * @typedef ra_glcdc_event_fn_t
 * @brief GLCDC event callback (VSYNC / line detect / underflow).
 */
typedef void (*ra_glcdc_event_fn_t)(void* ctx, uint32_t status_mask);

/**
 * @brief Tear down GLCDC (stop + MSTP release).
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_glcdc_deinit(void);

/**
 * @brief Read GLCDC status register (VSYNC/HSYNC/underflow).
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_glcdc_get_status(uint32_t* out_mask);

/**
 * @brief Clear GLCDC status flags.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_glcdc_clear_status(uint32_t mask);

/**
 * @brief Attach a GLCDC event callback.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_glcdc_attach_handler(ra_glcdc_event_fn_t fn, void* ctx);

/**
 * @brief Dispatch a GLCDC event -- snapshot + fire callback.
 * @since 0.1.0
 */
void ra_glcdc_dispatch(void);

/**
 * @brief Put GLCDC into MSTP-gated stop.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_glcdc_enter_stop(void);

/**
 * @brief Exit MSTP-gated stop.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_glcdc_exit_stop(void);

#ifdef __cplusplus
}
#endif
