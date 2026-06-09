/**
 * @file ra_reflow_render.c
 * @brief Page-rasteriser for ra_reflow.
 *
 * @details
 * Walks the slice of `engine->glyphs[]` belonging to one page and
 * blits each glyph's alpha-8 bitmap into the framebuffer bound by
 * `ra_gfx_init()`. Each glyph is rasterised on demand through the
 * two-step `stbtt_GetCodepointBitmapBox()` + `stbtt_MakeCodepointBitmap()`
 * path so the glyph bitmap lands in a fixed file-scope mask buffer rather
 * than a `STBTT_malloc`'d one; stb's remaining per-glyph scratch (vertex /
 * edge lists) is served by the no-heap static arena in
 * `ra_stbtt_alloc.c`. The alpha mask is composited against
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
  k_priv_glyph_dim_max       = 192U, /**< Mask edge bound = 2 * k_ra_reflow_max_font_px. */
} priv_render_consts_t;

/**
 * @brief Fixed glyph rasterisation buffer (tightly-packed alpha-8).
 *
 * @details
 * stb_truetype's one-shot @c stbtt_GetCodepointBitmap() calls
 * @c STBTT_malloc internally, but the firmware has no heap (@c _sbrk
 * traps after init). Instead every glyph is rasterised through the
 * two-step "Box" + "Make" path into this file-scope buffer -- NASA
 * Power-of-10 Rule 3 (no dynamic allocation). Sized for the largest
 * accepted font (@c k_ra_reflow_max_font_px) with 2x headroom so wide
 * glyphs and hinting overshoot always fit; oversized glyphs are
 * skipped rather than truncated.
 */
static uint8_t s_glyph_mask[(size_t)k_priv_glyph_dim_max * (size_t)k_priv_glyph_dim_max];

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
  /* Bounded, heap-free glyph rasterisation: the "Box" call reports the
   * bitmap extent and baseline offsets (x0, y0), then "Make" rasterises
   * directly into the fixed s_glyph_mask (tightly packed, stride == w,
   * matching priv_blit_alpha_mask) -- so the bitmap never hits the heap.
   * stb's internal vertex/edge scratch is served by the static arena in
   * ra_stbtt_alloc.c (STBTT_malloc is redirected there at build time),
   * keeping the whole path off libc malloc. */
  int x0 = 0;
  int y0 = 0;
  int x1 = 0;
  int y1 = 0;
  stbtt_GetCodepointBitmapBox(font, g->cp, scale, scale, &x0, &y0, &x1, &y1);
  const int w = x1 - x0;
  const int h = y1 - y0;
  if (w > 0 && h > 0) {
    /* Skip (do not truncate) any glyph larger than the fixed mask. */
    const size_t total = (size_t)w * (size_t)h;
    if (total <= sizeof s_glyph_mask) {
      stbtt_MakeCodepointBitmap(font, s_glyph_mask, w, h, w, scale, scale, g->cp);
      priv_blit_alpha_mask(g, s_glyph_mask, w, h, x0, y0);
    }
  }

  if ((g->style & k_ra_reflow_style_underline) != 0U) {
    priv_draw_underline(font, g, scale);
  }
}

/* ===========================================================================
 * Public API -- render
 * ===========================================================================
 */

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
