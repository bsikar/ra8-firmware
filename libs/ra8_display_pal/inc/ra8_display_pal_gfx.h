/**
 * @file ra8_display_pal_gfx.h
 * @brief Composition bridge binding a display PAL framebuffer to ra8_gfx.
 *
 * [Ring 4 / PAL] {World: NS}
 *
 * @details
 * Keeps ``ra8_display_pal`` (framebuffer / panel transport) decoupled from
 * ``ra8_gfx`` (software rasteriser) by placing the format translation in this
 * opt-in header rather than linking ``ra8_gfx`` into ``ra8_display_pal``.
 * Callers query a live framebuffer via ``display_get_framebuffer()`` and pass
 * it to ::display_bind_gfx(), which translates ::display_pixfmt_t into
 * ::ra8_gfx_format_t and invokes ``ra8_gfx_init_surface()``.
 *
 * Call ``ra8_gfx_deinit()`` before releasing or unmapping the underlying
 * framebuffer.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#include "ra8_display_pal.h"
#include "ra8_err.h"
#include "ra8_gfx.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Translate a display PAL framebuffer descriptor and bind it as the
 *        active ``ra8_gfx`` render target.
 *
 * @param[in] fb Display framebuffer descriptor obtained from
 *               ``display_get_framebuffer()`` or populated manually.
 * @return ::k_ra8_ok on success;
 *         ::k_ra8_err_null_ptr if ``fb`` or ``fb->pixels`` is null;
 *         ::k_ra8_err_not_supported for greyscale e-ink formats
 *         (``k_display_pixfmt_grey4``, ``k_display_pixfmt_grey1``);
 *         ::k_ra8_err_invalid_arg if ``fb->pixfmt`` is unrecognized or
 *         dimensions/stride fail ``ra8_gfx_init_surface()`` validation.
 * @note Thread safety: not thread-safe; synchronise with ``ra8_gfx`` draw calls.
 * @note ISR safety: must not be called from an ISR.
 * @since 0.1.0
 */
[[nodiscard]] static inline ra8_err_t display_bind_gfx(const display_fb_t* fb)
{
  if ((fb == nullptr) || (fb->pixels == nullptr)) {
    return k_ra8_err_null_ptr;
  }

  ra8_gfx_format_t gfx_fmt = k_ra8_gfx_format_rgb565;
  switch (fb->pixfmt) {
    case k_display_pixfmt_rgb565:
      gfx_fmt = k_ra8_gfx_format_rgb565;
      break;
    case k_display_pixfmt_rgb888:
      gfx_fmt = k_ra8_gfx_format_rgb888;
      break;
    case k_display_pixfmt_grey4:
    case k_display_pixfmt_grey1:
      return k_ra8_err_not_supported;
    default:
      return k_ra8_err_invalid_arg;
  }

  const ra8_gfx_surface_t surface = {
    .pixels       = fb->pixels,
    .w            = fb->width_px,
    .h            = fb->height_px,
    .stride_bytes = fb->stride_bytes,
    .fmt          = gfx_fmt,
  };
  return ra8_gfx_init_surface(&surface);
}

#ifdef __cplusplus
}
#endif
