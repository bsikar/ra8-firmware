/**
 * @file gx_display_driver_host.c
 * @brief Eclipse GUIX <-> host (desktop) 565rgb display-driver shim
 *
 * @par Tag
 * [Ring 4 / DRV] {World: NS}
 *
 * @details
 * Stashes a caller-owned RGB565 framebuffer and installs the vendored
 * ``_gx_display_driver_565rgb_setup`` against it. GUIX paints directly
 * into that buffer; the display PAL's ``display_flush`` (macOS backend)
 * is what pushes the buffer to the window, so the buffer-toggle hook
 * here is intentionally a no-op (single-buffer, mirroring the GLCDC
 * shim's toggle).
 *
 * Plain NULL guards (not RA_CHECK_NULL_PTR) keep this in step with the
 * sibling ``gx_display_driver_ra_glcdc.c`` and avoid pulling the logging
 * subsystem into the GUIX port layer.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "gx_display_driver_host.h"

#include <stddef.h>
#include <stdint.h>

#include "gx_api.h"
#include "gx_display.h"
#include "ra_err.h"

/** @brief Bound framebuffer base (NULL until bind). */
static void* s_fb;

/** @brief Bound framebuffer width in pixels. */
static uint16_t s_width;

/** @brief Bound framebuffer height in pixels. */
static uint16_t s_height;

ra_err_t host_guix_display_driver_bind(void* framebuffer, uint16_t width_px, uint16_t height_px)
{
  if (framebuffer == nullptr) {
    return k_ra_err_null_ptr;
  }
  if ((width_px == 0U) || (height_px == 0U)) {
    return k_ra_err_invalid_arg;
  }
  s_fb     = framebuffer;
  s_width  = width_px;
  s_height = height_px;
  return k_ra_ok;
}

/**
 * @brief Single-buffer toggle hook: presentation is the PAL's job.
 *
 * @details
 * v1 single-buffer host driver: GUIX has finished drawing into the canvas
 * memory in place. The macOS window backend presents that buffer separately
 * (display_flush), so there is nothing to flip here -- the hook is a no-op.
 *
 * @param[in] canvas     GUIX canvas whose drawing completed (unused).
 * @param[in] dirty_area Touched-pixel bounding box (unused).
 *
 * @pre gx_display_create has installed the 565rgb driver.
 * @pre Called from the GUIX drawing path.
 * @post No buffer flip occurs; presentation is left to the PAL.
 * @post Canvas memory is unchanged.
 *
 * @note Not thread-safe; single-threaded host drive.
 *
 * @since 0.1.0
 */
static void host_guix_buffer_toggle(GX_CANVAS* canvas, GX_RECTANGLE* dirty_area)
{
  (void)canvas;
  (void)dirty_area;
}

UINT host_guix_display_driver_setup(GX_DISPLAY* display)
{
  if (display == nullptr) {
    return GX_INVALID_VALUE;
  }
  if (s_fb == nullptr) {
    return GX_INVALID_VALUE;
  }

  _gx_display_driver_565rgb_setup(display, GX_NULL, host_guix_buffer_toggle);

  display->gx_display_width  = (GX_VALUE)s_width;
  display->gx_display_height = (GX_VALUE)s_height;
  return GX_SUCCESS;
}
