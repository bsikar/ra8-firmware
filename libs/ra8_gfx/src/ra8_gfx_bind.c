/**
 * @file ra8_gfx_bind.c
 * @brief Framebuffer binding and teardown for the ra8_gfx software rasteriser.
 * @ingroup grp_ereader
 *
 * @details
 * The lifecycle half of the library: the two bind entry points and the
 * teardown, kept apart from the drawing TUs because they are the only code
 * that writes the whole module state object rather than reading it.
 *
 * Two bind forms exist. ra8_gfx_init() takes the four positional values and
 * assumes a densely packed buffer, which is what every caller before #737
 * had. ra8_gfx_init_surface() takes a descriptor that also carries the row
 * pitch, so a caller whose backend padded its rows can hand the binding over
 * intact instead of dropping the stride and hoping `width * bpp` still holds.
 * Both populate the same state; every draw call addresses rows through
 * `pitch`, so the packed case is bit-for-bit what it always was.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_gfx.h"
#include "ra8_gfx_internal.h"

/** @brief Implementation of `priv_gfx_bpp()` -- promoted for cross-TU use. */
uint8_t priv_gfx_bpp(ra8_gfx_format_t format)
{
  return (uint8_t)format;
}

/** @brief Implementation of `priv_gfx_format_ok()` -- promoted for cross-TU use. */
bool priv_gfx_format_ok(ra8_gfx_format_t f)
{
  return (f == k_ra8_gfx_format_rgb565) || (f == k_ra8_gfx_format_rgb888) ||
         (f == k_ra8_gfx_format_argb8888);
}

ra8_err_t ra8_gfx_init(void* fb, uint16_t width, uint16_t height, ra8_gfx_format_t format)
{
  if (fb == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if ((width < k_ra8_gfx_min_dim) || (width > k_ra8_gfx_max_dim)) {
    return k_ra8_err_invalid_arg;
  }
  if ((height < k_ra8_gfx_min_dim) || (height > k_ra8_gfx_max_dim)) {
    return k_ra8_err_invalid_arg;
  }
  if (!priv_gfx_format_ok(format)) {
    return k_ra8_err_invalid_arg;
  }
  g_gfx_text_state.fb          = (uint8_t*)fb;
  g_gfx_text_state.width       = width;
  g_gfx_text_state.height      = height;
  g_gfx_text_state.format      = format;
  g_gfx_text_state.bpp         = priv_gfx_bpp(format);
  /* The positional form has no stride parameter, so the buffer is densely
   * packed by definition: pitch == one packed row. */
  g_gfx_text_state.pitch = (uint32_t)width * (uint32_t)priv_gfx_bpp(format);
  g_gfx_text_state.clip_x0     = 0;
  g_gfx_text_state.clip_y0     = 0;
  g_gfx_text_state.clip_x1     = (int32_t)width; /* default clip = whole framebuffer. */
  g_gfx_text_state.clip_y1     = (int32_t)height;
  g_gfx_text_state.initialized = true;
  return k_ra8_ok;
}

ra8_err_t ra8_gfx_init_surface(const ra8_gfx_surface_t* s)
{
  if (s == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (s->pixels == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if ((s->w < k_ra8_gfx_min_dim) || (s->w > k_ra8_gfx_max_dim)) {
    return k_ra8_err_invalid_arg;
  }
  if ((s->h < k_ra8_gfx_min_dim) || (s->h > k_ra8_gfx_max_dim)) {
    return k_ra8_err_invalid_arg;
  }
  if (!priv_gfx_format_ok(s->fmt)) {
    return k_ra8_err_invalid_arg;
  }
  /* A pitch narrower than one packed row cannot describe any real buffer: row
   * y+1 would start inside row y. Reject it rather than silently drawing
   * overlapping rows. A wider pitch is padding and is honoured as given. */
  const uint32_t packed_row = (uint32_t)s->w * (uint32_t)priv_gfx_bpp(s->fmt);
  if (s->stride_bytes < packed_row) {
    return k_ra8_err_invalid_arg;
  }
  g_gfx_text_state.fb           = (uint8_t*)s->pixels;
  g_gfx_text_state.width        = s->w;
  g_gfx_text_state.height       = s->h;
  g_gfx_text_state.format       = s->fmt;
  g_gfx_text_state.bpp          = priv_gfx_bpp(s->fmt);
  g_gfx_text_state.pitch = s->stride_bytes;
  g_gfx_text_state.clip_x0      = 0;
  g_gfx_text_state.clip_y0      = 0;
  g_gfx_text_state.clip_x1      = (int32_t)s->w; /* default clip = whole surface. */
  g_gfx_text_state.clip_y1      = (int32_t)s->h;
  g_gfx_text_state.initialized  = true;
  return k_ra8_ok;
}

ra8_err_t ra8_gfx_deinit(void)
{
  if (!g_gfx_text_state.initialized) {
    return k_ra8_err_not_initialized;
  }
  /* The framebuffer belongs to the caller, so nothing is released here beyond
   * the binding itself: drop the pointer so no later draw call can reach a
   * buffer the caller has since retired, and collapse the clip to empty. */
  g_gfx_text_state.fb           = nullptr;
  g_gfx_text_state.width        = 0;
  g_gfx_text_state.height       = 0;
  g_gfx_text_state.pitch = 0;
  g_gfx_text_state.bpp          = 0;
  g_gfx_text_state.clip_x0      = 0;
  g_gfx_text_state.clip_y0      = 0;
  g_gfx_text_state.clip_x1      = 0;
  g_gfx_text_state.clip_y1      = 0;
  g_gfx_text_state.initialized  = false;
  return k_ra8_ok;
}
