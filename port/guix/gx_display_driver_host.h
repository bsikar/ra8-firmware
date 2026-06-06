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
 * @return ra_err_t Error code.
 * @retval k_ra_ok              Bound.
 * @retval k_ra_err_null_ptr    framebuffer was NULL.
 * @retval k_ra_err_invalid_arg Zero width or height.
 *
 * @pre Called once before ``gx_display_create``.
 * @post ``host_guix_display_driver_setup`` will target this framebuffer.
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
 *
 * @pre ``host_guix_display_driver_bind`` has succeeded.
 * @post ``display`` draws into the bound framebuffer.
 *
 * @since 0.1.0
 */
UINT host_guix_display_driver_setup(GX_DISPLAY* display);

#ifdef __cplusplus
}
#endif
