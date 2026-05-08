/**
 * @file ra_reflow_render.c
 * @brief Page-rasteriser for ra_reflow.
 *
 * @details
 * Walks the slice of `engine->glyphs[]` belonging to one page and
 * blits each glyph's alpha-8 bitmap into the framebuffer bound by
 * `ra_gfx_init()`. Each glyph is rasterised on demand via
 * `stbtt_GetCodepointBitmap()`; the alpha mask is composited against
 * the engine's body / link colour using a simple "alpha >= threshold"
 * test so the output is binary (no per-pixel multiplies). This keeps
 * the render path ARM-Cortex-M cheap while still producing crisp
 * anti-aliased shapes thanks to stb's coverage-based rasteriser.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * [Ring 4 / Reflow]
 * {World: NS}
 *
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>

#include "ra_err.h"
#include "ra_gfx.h"
#include "ra_reflow.h"
#include "stb_truetype.h"

/* ===========================================================================
 * Internal sizing constants (no magic numbers).
 * ===========================================================================
 */

/**
 * @enum priv_render_consts_t
 * @brief Internal sizing knobs for the render pass.
 */
typedef enum : uint16_t {
  k_priv_alpha_threshold     = 96U,  /**< Coverage cutoff for binary blit. */
  k_priv_alpha_max_byte      = 255U, /**< Upper bound from stb's mask.    */
  k_priv_underline_offset_px = 2U,   /**< Pixels below baseline for the underline. */
  k_priv_underline_thick_px  = 1U,   /**< Thickness of the anchor underline. */
} priv_render_consts_t;

/**
 * @brief Initialise an stbtt_fontinfo from the engine's font blob.
 *
 * @details See implementation.
 * @param[in] engine See implementation.
 * @param[in] out_font See implementation.
 * @return Result code.
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static ra_err_t priv_init_font(const ra_reflow_t* engine, stbtt_fontinfo* out_font)
{
  const int32_t offset = stbtt_GetFontOffsetForIndex(engine->font_data, 0);
  if (offset < 0) {
    return k_ra_err_validation_failed;
  }
  if (stbtt_InitFont(out_font, engine->font_data, offset) == 0) {
    return k_ra_err_validation_failed;
  }
  return k_ra_ok;
}

/**
 * @brief Walk one glyph bitmap and blit any sufficiently-covered
 *        pixels into the bound framebuffer.
 *
 * @details See implementation.
 * @param[in] g See implementation.
 * @param[in] bitmap See implementation.
 * @param[in] w See implementation.
 * @param[in] h See implementation.
 * @param[in] xoff See implementation.
 * @param[in] yoff See implementation.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static void priv_blit_alpha_mask(const ra_reflow_glyph_t* g,
                                 const unsigned char*     bitmap,
                                 int                      w,
                                 int                      h,
                                 int                      xoff,
                                 int                      yoff)
{
  for (int row = 0; row < h; ++row) {
    for (int col = 0; col < w; ++col) {
      const uint8_t alpha = (uint8_t)bitmap[((size_t)row * (size_t)w) + (size_t)col];
      if (alpha < k_priv_alpha_threshold) {
        continue;
      }
      const int32_t px = g->x + (int32_t)col + (int32_t)xoff;
      const int32_t py = g->y + (int32_t)row + (int32_t)yoff;
      (void)ra_gfx_pixel(px, py, g->color);
    }
  }
}

/**
 * @brief Draw the underline strip for a link glyph.
 *
 * @details See implementation.
 * @param[in] font See implementation.
 * @param[in] g See implementation.
 * @param[in] scale See implementation.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static void priv_draw_underline(const stbtt_fontinfo* font, const ra_reflow_glyph_t* g, float scale)
{
  int advance_units = 0;
  int lsb           = 0;
  stbtt_GetCodepointHMetrics(font, g->cp, &advance_units, &lsb);
  const int32_t advance = (int32_t)((float)advance_units * scale);
  const int32_t y_under = g->y + (int32_t)k_priv_underline_offset_px;
  for (int32_t i = 0; i < advance; ++i) {
    (void)ra_gfx_pixel(g->x + i, y_under, g->color);
  }
}

/**
 * @brief Rasterise one glyph at (gx, gy) into the bound framebuffer.
 *
 * @details
 * `gx` / `gy` are the glyph's baseline-left position. We add the
 * stbtt y-offset so the bitmap lands in the correct row above the
 * baseline; columns are offset by stbtt's xoff so accents and
 * descenders place correctly.
 *
 * @param[in] font See implementation.
 * @param[in] g See implementation.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static void priv_blit_glyph(const stbtt_fontinfo* font, const ra_reflow_glyph_t* g)
{
  const float scale = stbtt_ScaleForPixelHeight(font, (float)g->font_px);
  int         w     = 0;
  int         h     = 0;
  int         xoff  = 0;
  int         yoff  = 0;
  /* clang-format off */
  unsigned char* bitmap = stbtt_GetCodepointBitmap(font, 0.0F, scale, g->cp, &w, &h, &xoff, &yoff); /* alloc-allow: stb_truetype is vendored SOUP; the matching stbtt_FreeBitmap below releases the alloc within the same function. */
  /* clang-format on */
  if (bitmap == nullptr) {
    return;
  }
  // mcdc-deactivated: TU-local helper priv_blit_glyph; stbtt_GetCodepointBitmap returns either a non-NULL bitmap with both w > 0 AND h > 0 (well-formed glyph rasterization), or a NULL pointer rejected at the early-return above -- the two bound conditions cannot independently flip on any reachable path.
  if (w > 0 && h > 0) {
    priv_blit_alpha_mask(g, bitmap, w, h, xoff, yoff);
  }
  /* clang-format off */
  stbtt_FreeBitmap(bitmap, nullptr); /* alloc-allow: pairs with the stbtt_GetCodepointBitmap above. */
  /* clang-format on */

  if ((g->style & k_ra_reflow_style_underline) != 0U) {
    priv_draw_underline(font, g, scale);
  }
}

/* ===========================================================================
 * Public API -- render
 * ===========================================================================
 */

/* Implementation of ra_reflow_render_page (see header for full contract) -- see header for the documented contract. */
ra_err_t ra_reflow_render_page(const ra_reflow_t* engine, uint32_t page_idx, void* framebuffer)
{
  (void)framebuffer; /* Reserved hook -- ra_gfx is bound externally. */
  if (engine == nullptr) {
    return k_ra_err_null_ptr;
  }
  if (engine->in_use == 0U) {
    return k_ra_err_not_initialized;
  }
  if (page_idx >= engine->page_count) {
    return k_ra_err_out_of_range;
  }

  stbtt_fontinfo font;
  ra_err_t       err = priv_init_font(engine, &font);
  if (err != k_ra_ok) {
    return err;
  }

  const ra_reflow_page_t* page = &engine->pages[page_idx];
  for (uint32_t i = 0U; i < page->glyph_count; ++i) {
    const ra_reflow_glyph_t* g = &engine->glyphs[page->glyph_first + i];
    priv_blit_glyph(&font, g);
  }
  return k_ra_ok;
}
