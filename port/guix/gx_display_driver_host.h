/**
 * @file gx_display_driver_host.h
 * @brief Eclipse GUIX <-> host (desktop) 565rgb display-driver shim
 *
 * @par Tag
 * [Ring 4 / DRV] {World: NS}
 *
 * @details
 * Host-side counterpart of ``gx_display_driver_ra_glcdc.c``. GUIX renders
 * straight into a caller-owned RGB565 framebuffer via the vendored
 * ``_gx_display_driver_565rgb_*`` routines; presentation to the actual
 * window happens through the display PAL's ``display_flush`` (the macOS
 * backend), so this shim's buffer-toggle hook is a no-op.
 *
 * The macOS PAL backend's ``bind_guix`` slot forwards to
 * ``host_guix_display_driver_bind`` + ``host_guix_display_driver_setup``,
 * exactly the way the LCD backend forwards to its ``ra_guix_*`` pair.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * @since 0.1.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "gx_api.h"
#include "ra_err.h"

/**
 * @brief Bind a caller-owned RGB565 framebuffer for the host GUIX driver.
 *
 * @param[in] framebuffer RGB565 pixel buffer GUIX will paint into.
 * @param[in] width_px    Width in pixels (>= 1).
 * @param[in] height_px   Height in pixels (>= 1).
 *
 * @details Records the framebuffer + dimensions for the next
 *          ``host_guix_display_driver_setup`` to install into the 565rgb driver.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok              Bound.
 * @retval k_ra_err_null_ptr    framebuffer was NULL.
 * @retval k_ra_err_invalid_arg Zero width or height.
 *
 * @pre Called once before ``gx_display_create``.
 * @pre ``framebuffer`` holds at least width_px*height_px RGB565 pixels.
 * @post ``host_guix_display_driver_setup`` will target this framebuffer.
 * @post The bound dimensions size the GUIX display.
 *
 * @note Not thread-safe; call once during single-threaded setup.
 *
 * @since 0.1.0
 */
ra_err_t host_guix_display_driver_bind(void* framebuffer, uint16_t width_px, uint16_t height_px);

/**
 * @brief GUIX ``display_driver_setup`` callback for the host backend.
 *
 * @details Pass its address (cast to the opaque PAL slot) into
 *          ``gx_display_create``. Installs the vendored 565rgb driver
 *          pointed at the bound framebuffer.
 *
 * @param[in] display GUIX display being created.
 *
 * @return UINT GX_SUCCESS, or GX_INVALID_VALUE if no framebuffer is bound.
 * @retval GX_SUCCESS       The 565rgb driver was installed.
 * @retval GX_INVALID_VALUE No framebuffer bound, or ``display`` was NULL.
 *
 * @pre ``host_guix_display_driver_bind`` has succeeded.
 * @pre Called by GUIX from ``gx_display_create``.
 * @post ``display`` draws into the bound framebuffer.
 * @post ``display`` width/height match the bound dimensions.
 *
 * @note Not thread-safe; single-threaded host setup.
 *
 * @since 0.1.0
 */
UINT host_guix_display_driver_setup(GX_DISPLAY* display);

#ifdef __cplusplus
}
#endif
