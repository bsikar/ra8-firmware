/**
 * @file ra8_reflow_render.c
 * @brief Page-rasteriser for ra8_reflow.
 *
 * @details
 * Walks the slice of `engine->glyphs[]` belonging to one page and
 * blits each glyph's alpha-8 bitmap into the framebuffer bound by
 * `ra8_gfx_init()`. Each glyph is rasterised on demand through the
 * two-step `stbtt_GetCodepointBitmapBox()` + `stbtt_MakeCodepointBitmap()`
 * path so the glyph bitmap lands in a fixed file-scope mask buffer rather
 * than a `STBTT_malloc`'d one; stb's remaining per-glyph scratch (vertex /
 * edge lists) is served by the no-heap static arena in
 * `ra8_stbtt_alloc.c`. The alpha mask is composited against
 * the engine's body / link colour using a simple "alpha >= threshold"
 * test so the output is binary (no per-pixel multiplies). This keeps
 * the render path ARM-Cortex-M cheap while still producing crisp
 * anti-aliased shapes thanks to stb's coverage-based rasteriser.
 *
 *
 * [Ring 4 / Reflow]
 * {World: NS}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_gfx.h"
#include "ra8_glyph_atlas.h"
#include "ra8_reflow.h"
#include "ra8_reflow_svg.h"
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
  k_priv_alpha_threshold     = 96U,  /**< Coverage cutoff for binary blit.                */
  k_priv_alpha_max_byte      = 255U, /**< Upper bound from stb's mask.                    */
  k_priv_underline_offset_px = 2U,   /**< Pixels below baseline for the underline.        */
  k_priv_underline_thick_px  = 1U,   /**< Thickness of the anchor underline.              */
  k_priv_glyph_dim_max       = 192U, /**< Mask edge bound = 2 * k_ra8_reflow_max_font_px. */
  k_priv_svg_href_max        = 256U, /**< Max unwrapped SVG cover-image href length.      */
  k_priv_glyph_mode_aa       = 0U,   /**< Glyph-cache render mode: stb coverage AA.       */
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
 * accepted font (@c k_ra8_reflow_max_font_px) with 2x headroom so wide
 * glyphs and hinting overshoot always fit; oversized glyphs are
 * skipped rather than truncated.
 */
static uint8_t s_glyph_mask[(size_t)k_priv_glyph_dim_max * (size_t)k_priv_glyph_dim_max];

/**
 * @struct priv_glyph_render_ctx_t
 * @brief Per-glyph state handed to the glyph-atlas render-on-miss callback.
 *
 * @details The ::ra8_glyph_atlas render seam is fixed at bind time, but the font,
 *          scale, and bitmap extent for the glyph currently being drawn vary per
 *          call. The blit path computes the box once (::internal_blit_glyph) and
 *          stashes them here immediately before ::ra8_glyph_atlas_get, so a cache
 *          miss rasterises the right glyph at the known size without recomputing
 *          the box. Single instance (render is single-threaded, like
 *          @ref s_glyph_mask and @c s_faces).
 *
 * @invariant When a miss can fire, `font` is non-NULL and `w`, `h` are the
 *            positive extents the blit path already validated for the glyph.
 * @since 0.1.0
 */
typedef struct {
  const stbtt_fontinfo* font;  /**< Font for the in-flight glyph.             */
  float                 scale; /**< stb pixel-height scale for that glyph.    */
  int                   w;     /**< Glyph bitmap width (from the box query).  */
  int                   h;     /**< Glyph bitmap height (from the box query). */
} priv_glyph_render_ctx_t;

/**
 * @brief Render context shared with the bound glyph atlas (file-static, like
 *        @ref s_glyph_mask -- render is single-threaded).
 *
 * @warning Mutated by the blit path before each ::ra8_glyph_atlas_get; not
 *          re-entrant.
 */
static priv_glyph_render_ctx_t s_glyph_render_ctx;

/**
 * @brief Glyph-atlas render-on-miss callback: rasterise one glyph into a cell.
 *
 * @details Bridges ::ra8_glyph_atlas to the engine's stb_truetype rasteriser. On
 *          a cache miss the atlas calls this (only ever through ::ra8_glyph_atlas_get,
 *          which validates ctx/key/cell and supplies its own cell + out_w/out_h
 *          storage, so no argument null-check is needed here -- the same
 *          caller-guarantee convention as ::internal_blit_alpha_mask). The font,
 *          scale, and extent come from @ref s_glyph_render_ctx (already computed
 *          and validated by ::internal_blit_glyph before the get); the code point
 *          rides in `key->glyph_id`. `stbtt_MakeCodepointBitmap` runs with
 *          stride == w exactly as the direct path, so a cached bitmap is
 *          byte-identical to a freshly rasterised one. A glyph whose bitmap
 *          exceeds the cell is rejected so the caller falls back to direct
 *          rasterisation.
 * @param[in]  ctx        The ::priv_glyph_render_ctx_t set by ::internal_blit_glyph.
 * @param[in]  key        Glyph to render (`glyph_id` is the code point).
 * @param[out] cell       Destination cell buffer (`cell_bytes` writable).
 * @param[in]  cell_bytes Cell capacity in bytes.
 * @param[out] out_w      Rendered glyph width in pixels.
 * @param[out] out_h      Rendered glyph height in pixels.
 * @return Result code.
 * @retval k_ra8_ok               Glyph rendered; `*out_w`/`*out_h` written.
 * @retval k_ra8_err_invalid_size The glyph bitmap does not fit the cell.
 * @pre `ctx` is the bound ::s_glyph_render_ctx with a valid font/scale/w/h.
 * @pre `cell` has room for `cell_bytes` bytes (guaranteed by the keycache).
 * @post On `k_ra8_ok` the cell holds a tightly packed alpha-8 `w*h` bitmap.
 * @post On `k_ra8_err_invalid_size` the cell is untouched and stays unpinned.
 * @note Not thread-safe; uses @ref s_glyph_render_ctx.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_atlas_render_glyph(void*                  ctx,
                                             const ra8_glyph_key_t* key,
                                             uint8_t*               cell,
                                             uint32_t               cell_bytes,
                                             uint16_t*              out_w,
                                             uint16_t*              out_h)
{
  const priv_glyph_render_ctx_t* rc = (const priv_glyph_render_ctx_t*)ctx;
  /* w/h were computed and validated (> 0) by internal_blit_glyph before the get. */
  if (((size_t)rc->w * (size_t)rc->h) > (size_t)cell_bytes) {
    return k_ra8_err_invalid_size; /* Too big to cache -> caller blits directly. */
  }
  stbtt_MakeCodepointBitmap(rc->font,
                            cell,
                            rc->w,
                            rc->h,
                            rc->w,
                            rc->scale,
                            rc->scale,
                            (int)key->glyph_id);
  *out_w = (uint16_t)rc->w;
  *out_h = (uint16_t)rc->h;
  return k_ra8_ok;
}

/**
 * @brief Initialise an stbtt_fontinfo from the engine's font blob.
 *
 * @details See implementation.
 * @param[in] engine See implementation.
 * @param[in] out_font See implementation.
 * @return Result code.
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_init_font(const ra8_reflow_t* engine, stbtt_fontinfo* out_font)
{
  const int32_t offset = stbtt_GetFontOffsetForIndex(engine->font_data, 0);
  if (offset < 0) {
    return k_ra8_err_validation_failed;
  }
  if (stbtt_InitFont(out_font, engine->font_data, (int)engine->font_len, offset) == 0) {
    return k_ra8_err_validation_failed;
  }
  return k_ra8_ok;
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
 * @param[in] ox Pixel offset added to every glyph x.
 * @param[in] oy Pixel offset added to every glyph y.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_blit_alpha_mask(const ra8_reflow_glyph_t* g,
                                     const unsigned char*      bitmap,
                                     int                       w,
                                     int                       h,
                                     int                       xoff,
                                     int                       yoff,
                                     int32_t                   ox,
                                     int32_t                   oy)
{
  for (int row = 0; row < h; ++row) {
    for (int col = 0; col < w; ++col) {
      const uint8_t alpha = (uint8_t)bitmap[((size_t)row * (size_t)w) + (size_t)col];
      if (alpha < k_priv_alpha_threshold) {
        continue;
      }
      const int32_t px = g->x + (int32_t)col + (int32_t)xoff + ox;
      const int32_t py = g->y + (int32_t)row + (int32_t)yoff + oy;
      (void)ra8_gfx_pixel(px, py, g->color);
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
 * @param[in] ox Pixel offset added to the underline x.
 * @param[in] oy Pixel offset added to the underline y.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_draw_underline(const stbtt_fontinfo*     font,
                                    const ra8_reflow_glyph_t* g,
                                    float                     scale,
                                    int32_t                   ox,
                                    int32_t                   oy)
{
  int advance_units = 0;
  int lsb           = 0;
  stbtt_GetCodepointHMetrics(font, g->cp, &advance_units, &lsb);
  const int32_t advance = (int32_t)((float)advance_units * scale);
  const int32_t y_under = g->y + (int32_t)k_priv_underline_offset_px + oy;
  for (int32_t i = 0; i < advance; ++i) {
    (void)ra8_gfx_pixel(g->x + i + ox, y_under, g->color);
  }
}

/**
 * @brief Rasterise one glyph straight into @ref s_glyph_mask and blit it.
 *
 * @details The heap-free fallback path used when no glyph cache is bound or a
 *          glyph is too large to cache. "Make" rasterises into the fixed
 *          tightly-packed @ref s_glyph_mask (stride == w, matching
 *          ::internal_blit_alpha_mask) -- the bitmap never hits the heap; stb's
 *          vertex/edge scratch is served by the static arena in
 *          ra8_stbtt_alloc.c. Glyphs larger than the mask are skipped (not
 *          truncated), exactly as before the cache existed.
 * @param[in] font  Initialised font for this glyph.
 * @param[in] scale stb pixel-height scale for @p g.
 * @param[in] g     Positioned glyph to draw.
 * @param[in] w     Glyph bitmap width from the box query.
 * @param[in] h     Glyph bitmap height from the box query.
 * @param[in] x0    stb x baseline offset from the box query.
 * @param[in] y0    stb y baseline offset from the box query.
 * @param[in] ox    Pixel offset added to every glyph x.
 * @param[in] oy    Pixel offset added to every glyph y.
 * @pre `w` and `h` are the positive extents reported for @p g.
 * @pre @p font is initialised and @p g is non-NULL.
 * @post On a fitting glyph the covered pixels are blitted; oversized are skipped.
 * @post No engine or cache state is mutated.
 * @note Not thread-safe; uses @ref s_glyph_mask.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_glyph_render_direct(const stbtt_fontinfo*     font,
                                         float                     scale,
                                         const ra8_reflow_glyph_t* g,
                                         int                       w,
                                         int                       h,
                                         int                       x0,
                                         int                       y0,
                                         int32_t                   ox,
                                         int32_t                   oy)
{
  /* Skip (do not truncate) any glyph larger than the fixed mask. */
  const size_t total = (size_t)w * (size_t)h;
  if (total <= sizeof s_glyph_mask) {
    stbtt_MakeCodepointBitmap(font, s_glyph_mask, w, h, w, scale, scale, g->cp);
    internal_blit_alpha_mask(g, s_glyph_mask, w, h, x0, y0, ox, oy);
  }
}

/**
 * @brief Draw one glyph through the bound glyph atlas (cache hit or render-miss).
 *
 * @details Keys the glyph by (face, size, code point, mode), fetches a pinned
 *          bitmap from @p atlas (rendering once on a miss via
 *          ::internal_atlas_render_glyph), blits it, then unpins. Because the cache
 *          is filled by the very same stb rasteriser as the direct path, the
 *          blitted pixels are byte-identical either way. Any atlas error
 *          (oversized glyph, or every cell pinned) returns @c false so the
 *          caller can fall back to ::internal_glyph_render_direct -- the cache is a
 *          pure optimisation and never changes output.
 * @param[in] atlas   Bound glyph cache (non-NULL).
 * @param[in] face_id Face index resolved for this glyph.
 * @param[in] font    Initialised font for this glyph.
 * @param[in] scale   stb pixel-height scale for @p g.
 * @param[in] g       Positioned glyph to draw.
 * @param[in] w       Glyph bitmap width from the box query (> 0).
 * @param[in] h       Glyph bitmap height from the box query (> 0).
 * @param[in] x0      stb x baseline offset from the box query.
 * @param[in] y0      stb y baseline offset from the box query.
 * @param[in] ox      Pixel offset added to every glyph x.
 * @param[in] oy      Pixel offset added to every glyph y.
 * @return @c true if the glyph was drawn from the cache; @c false to fall back.
 * @retval true  The glyph was fetched (hit or render-on-miss) and blitted.
 * @retval false The atlas could not service the glyph; caller blits directly.
 * @pre @p atlas, @p font, and @p g are non-NULL.
 * @pre @p w and @p h are the positive extents already validated by the caller.
 * @post On @c true the glyph is blitted and no cache pin is left held.
 * @post On @c false no pixels were drawn and no cache pin is held.
 * @note Not thread-safe; mutates @ref s_glyph_render_ctx.
 * @note The ::ra8_glyph_atlas_put return is intentionally `(void)`-cast: the
 *       `bitmap` pointer came from ::ra8_glyph_atlas_get on the same @p atlas in
 *       this call frame and the cell is still pinned, so put cannot fail with
 *       k_ra8_err_null_ptr (both args non-NULL) or k_ra8_err_invalid_arg (the cell
 *       is a member of this atlas and is pinned). The NASA Rule 7 deviation is
 *       bounded to this programmer-error-only path.
 * @since 0.1.0
 */
RA8_INTERNAL
static bool internal_glyph_render_cached(ra8_glyph_atlas_t*        atlas,
                                         uint8_t                   face_id,
                                         const stbtt_fontinfo*     font,
                                         float                     scale,
                                         const ra8_reflow_glyph_t* g,
                                         int                       w,
                                         int                       h,
                                         int                       x0,
                                         int                       y0,
                                         int32_t                   ox,
                                         int32_t                   oy)
{
  /* Hand the render-on-miss callback the font/scale/extent for this glyph. */
  s_glyph_render_ctx.font  = font;
  s_glyph_render_ctx.scale = scale;
  s_glyph_render_ctx.w     = w;
  s_glyph_render_ctx.h     = h;

  ra8_glyph_key_t key = {};
  key.glyph_id        = (uint32_t)g->cp;
  key.face_id         = (uint16_t)face_id;
  key.size_px         = g->font_px;
  key.mode            = (uint16_t)k_priv_glyph_mode_aa;

  ra8_glyph_t glyph = {};
  if (ra8_glyph_atlas_get(atlas, &key, &glyph) != k_ra8_ok) {
    return false; /* Uncacheable (oversized / full) -> caller blits directly. */
  }
  internal_blit_alpha_mask(g, glyph.bitmap, (int)glyph.width, (int)glyph.height, x0, y0, ox, oy);
  (void)ra8_glyph_atlas_put(atlas, glyph.bitmap);
  return true;
}

/**
 * @brief Rasterise one glyph at its baseline position into the bound framebuffer.
 *
 * @details
 * `g->x` / `g->y` are the glyph's baseline-left position. The "Box" call reports
 * the bitmap extent and stb baseline offsets (x0, y0) -- cheap, no raster -- so
 * the bitmap lands in the correct row above the baseline and columns place
 * accents/descenders correctly. When @p atlas is bound the bitmap is fetched
 * from (or rendered once into) the cache; otherwise it is rasterised directly.
 * Underlined link glyphs additionally get an underline strip.
 *
 * @param[in] atlas   Bound glyph cache, or NULL for direct rasterisation.
 * @param[in] face_id Face index resolved for this glyph (for the cache key).
 * @param[in] font    Initialised font for this glyph.
 * @param[in] g       Positioned glyph to draw.
 * @param[in] ox      Pixel offset added to every glyph x.
 * @param[in] oy      Pixel offset added to every glyph y.
 * @pre @p font is initialised and @p g is non-NULL.
 * @pre @p face_id selected @p font in the caller.
 * @post The glyph (and any underline) is blitted into the bound framebuffer.
 * @post Engine state is unchanged; only the framebuffer and cache LRU move.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_blit_glyph(ra8_glyph_atlas_t*        atlas,
                                uint8_t                   face_id,
                                const stbtt_fontinfo*     font,
                                const ra8_reflow_glyph_t* g,
                                int32_t                   ox,
                                int32_t                   oy)
{
  const float scale = stbtt_ScaleForPixelHeight(font, (float)g->font_px);
  int         x0    = 0;
  int         y0    = 0;
  int         x1    = 0;
  int         y1    = 0;
  stbtt_GetCodepointBitmapBox(font, g->cp, scale, scale, &x0, &y0, &x1, &y1);
  const int w = x1 - x0;
  const int h = y1 - y0;
  /*
   * w and h are the glyph bitmap-box extents returned by
   * stbtt_GetCodepointBitmapBox(). A codepoint is either inked -- a non-empty
   * box with w > 0 AND h > 0 -- or empty -- a zero box with w == 0 AND h == 0
   * (e.g. U+0020 space). The mixed states (w <= 0, h > 0) and (w > 0, h <= 0)
   * never occur for a real font glyph, so the vectors that would give w and h
   * independent MC/DC influence are structurally unreachable.
   */
  /* mcdc-deactivated: glyph bbox w,h co-dependent (inked or empty); mixed vectors unreachable. */
  if ((w > 0) && (h > 0)) {
    bool drawn = false;
    if (atlas != nullptr) {
      drawn = internal_glyph_render_cached(atlas, face_id, font, scale, g, w, h, x0, y0, ox, oy);
    }
    if (!drawn) {
      internal_glyph_render_direct(font, scale, g, w, h, x0, y0, ox, oy);
    }
  }

  if ((g->style & k_ra8_reflow_style_underline) != 0U) {
    internal_draw_underline(font, g, scale, ox, oy);
  }
}

/**
 * @brief Render an SVG image box: unwrap a cover `<image>` href, else draw shapes.
 *
 * @details
 * A cover-wrapper SVG (`<svg><image href=.../></svg>`) is detected by
 * `ra8_svg_image_href()`. When found, the href string is copied into a
 * stack buffer (bounded by `k_priv_svg_href_max`) before re-invoking
 * `engine->img_loader` to fetch the referenced raster; the copy is
 * necessary because the loader call may overwrite the SVG buffer. The
 * decoded raster is then blitted by `ra8_img_decode_blit()` into the
 * supplied bounding box. When no `<image>` wrapper is detected, the SVG
 * is passed directly to `ra8_svg_render()` as a shape document
 * (`<rect>`, `<circle>`, `<line>`, `<path>`, etc.). Href strings longer
 * than `k_priv_svg_href_max - 1` bytes are silently dropped.
 *
 * @param[in] engine  Engine handle; `img_loader` and `img_loader_ctx`
 *                    are used when a raster href is found.
 * @param[in] svg     Pointer to the raw SVG bytes.
 * @param[in] len     Byte length of @p svg.
 * @param[in] x       Left edge of the destination bounding box in pixels.
 * @param[in] y       Top edge of the destination bounding box in pixels.
 * @param[in] w       Width of the destination bounding box in pixels.
 * @param[in] h       Height of the destination bounding box in pixels.
 * @return Nothing.
 * @pre @p engine is non-null and its `img_loader` field is set.
 * @pre @p svg is non-null and @p len bytes are readable.
 * @post If a valid cover-image href is found, the referenced raster is
 *       decoded and blitted into (`x`, `y`, `w`, `h`); otherwise the SVG
 *       shapes are rendered into the same box.
 * @post The framebuffer state reflects the drawn content; engine state is
 *       unchanged.
 * @note Not thread-safe; shares the engine's `img_arena` with callers.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_render_svg(const ra8_reflow_t* engine,
                                const uint8_t*      svg,
                                size_t              len,
                                int32_t             x,
                                int32_t             y,
                                int32_t             w,
                                int32_t             h)
{
  size_t hoff = 0U;
  size_t hlen = 0U;
  if (ra8_svg_image_href(svg, len, &hoff, &hlen) != k_ra8_ok) {
    (void)ra8_svg_render(svg, len, x, y, w, h);
    return;
  }
  char href[k_priv_svg_href_max] = {};
  if (hlen >= sizeof(href)) {
    return;
  }
  (void)memcpy(href, &svg[hoff], hlen);
  const uint8_t* rbytes = nullptr;
  size_t         rlen   = 0U;
  if (engine->img_loader(engine->img_loader_ctx, href, (uint32_t)hlen, &rbytes, &rlen) ==
      k_ra8_ok) {
    (void)ra8_img_decode_blit(engine->img_arena, rbytes, rlen, x, y, w, h, nullptr, nullptr);
  }
}

/**
 * @brief Render one image box: SVG-route or raster-decode at the page offset.
 *
 * @details
 * Resolves the source href stored at `engine->text_pool[box->src_off]`
 * (length `box->src_len`) through `engine->img_loader`. On a successful
 * load, the bytes are inspected by `ra8_svg_is_svg()`: SVG content is
 * forwarded to `internal_render_svg()` (which handles both cover-wrapper and
 * shape SVGs); all other formats are decoded directly by
 * `ra8_img_decode_blit()`. In both cases the bounding rectangle is
 * `(box->x + ox, box->y + oy, box->w, box->h)`. A loader failure causes
 * an early return, leaving a blank gap in the framebuffer rather than
 * aborting the page render.
 *
 * @param[in] engine  Engine handle; loader, arena, and text pool are read.
 * @param[in] box     Image layout descriptor for the box to render.
 * @param[in] ox      Horizontal page-origin offset in pixels.
 * @param[in] oy      Vertical page-origin offset in pixels.
 * @return Nothing.
 * @pre @p engine is non-null with a valid `img_loader` and `text_pool`.
 * @pre @p box is non-null and its `src_off` + `src_len` are within
 *      `engine->text_pool`.
 * @post On loader success, the image is blitted into the framebuffer
 *       at the computed destination rectangle.
 * @post Engine and box state are unchanged; the arena drains after each
 *       decode call.
 * @note Not thread-safe; shares `engine->img_arena` with the caller.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_render_one_image(const ra8_reflow_t*           engine,
                                      const ra8_reflow_image_box_t* box,
                                      int32_t                       ox,
                                      int32_t                       oy)
{
  const char*    href  = (const char*)&engine->text_pool[box->src_off];
  const uint8_t* bytes = nullptr;
  size_t         blen  = 0U;
  if (engine->img_loader(engine->img_loader_ctx, href, box->src_len, &bytes, &blen) != k_ra8_ok) {
    return;
  }
  const int32_t bx = box->x + ox;
  const int32_t by = box->y + oy;
  if (ra8_svg_is_svg(bytes, blen)) {
    internal_render_svg(engine, bytes, blen, bx, by, box->w, box->h);
    return;
  }
  (void)
    ra8_img_decode_blit(engine->img_arena, bytes, blen, bx, by, box->w, box->h, nullptr, nullptr);
}

/**
 * @brief Decode and blit every laid-out image that belongs to one page.
 *
 * @details
 * Walks `engine->image_boxes[0..image_box_count-1]` and calls
 * `internal_render_one_image()` for each box whose `page_index` matches
 * @p page_idx. The decode is on-demand: decoded pixels are never stored
 * persistently and the arena drains after each box. Boxes whose loader
 * or decode fails are skipped, leaving a blank gap rather than aborting
 * the rest of the page. The function is a no-op if `engine->img_loader`
 * or `engine->img_arena` is null, allowing callers that do not bind an
 * image loader to share the same render path.
 *
 * @param[in] engine   Engine handle; image-box array and loader are read.
 * @param[in] page_idx Index of the page being rendered.
 * @param[in] ox       Horizontal page-origin offset in pixels.
 * @param[in] oy       Vertical page-origin offset in pixels.
 * @return Nothing.
 * @pre @p engine is non-null and fully initialized.
 * @pre A framebuffer is bound via `ra8_gfx_init()`.
 * @post Every decodable image on @p page_idx is blitted into the
 *       framebuffer; failed boxes are silently skipped.
 * @post Engine state is unchanged; the arena drains after each decode.
 * @note Not thread-safe; the decode arena uses a file-static allocation
 *       pool that must not be re-entered.
 * @since 0.1.0
 */
RA8_INTERNAL
static void
internal_render_images(const ra8_reflow_t* engine, uint32_t page_idx, int32_t ox, int32_t oy)
{
  if ((engine->img_loader == nullptr) || (engine->img_arena == nullptr)) {
    return;
  }
  for (uint32_t i = 0U; i < engine->image_box_count; ++i) {
    const ra8_reflow_image_box_t* box = &engine->image_boxes[i];
    if (box->page_index == page_idx) {
      internal_render_one_image(engine, box, ox, oy);
    }
  }
}

/* ===========================================================================
 * Public API -- render
 * ===========================================================================
 */

/**
 * @brief Build the per-render `stbtt_fontinfo` set: default at 0, faces at 1..N.
 *
 * @details
 * Slot 0 is always the engine's bound default face, initialised through
 * `internal_init_font()`. Slots `1..face_count` are the registered embedded
 * `@font-face` blobs from `engine->faces[]`. A registered blob that
 * fails `stbtt_InitFont` (already validated at register time, so this
 * path is unexpected) is replaced by a copy of the default face so that
 * every slot is always initialised and glyph rendering never indexes an
 * invalid fontinfo. `*out_n` is set to `engine->face_count + 1`.
 *
 * @param[in]  engine  Engine holding the default font blob and face registry.
 * @param[out] faces   Caller-allocated array of at least
 *                     `1 + k_ra8_reflow_max_faces` `stbtt_fontinfo` entries.
 * @param[out] out_n   Receives the count of valid entries written
 *                     (`1 + engine->face_count`).
 * @return `k_ra8_ok` on success, or the error returned by
 *         `internal_init_font()` if the default face cannot be initialised.
 * @retval k_ra8_ok           All face slots initialised successfully.
 * @retval k_ra8_err_validation_failed  Default face font data is invalid.
 * @pre @p engine is non-null and `engine->font_data` points to a valid
 *      TrueType/OpenType blob.
 * @pre @p faces and @p out_n are non-null; @p faces has capacity for at
 *      least `1 + k_ra8_reflow_max_faces` entries.
 * @post On `k_ra8_ok`, `faces[0..*out_n - 1]` are all initialised; each
 *       slot that failed individual init holds a copy of `faces[0]`.
 * @post `*out_n` equals `engine->face_count + 1` on success; its value
 *       is unspecified on error.
 * @note Not thread-safe; `s_faces` is a file-static array shared across
 *       calls.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t
internal_init_faces(const ra8_reflow_t* engine, stbtt_fontinfo* faces, uint8_t* out_n)
{
  const ra8_err_t err = internal_init_font(engine, &faces[0]);
  if (err != k_ra8_ok) {
    return err;
  }
  for (uint8_t k = 0U; k < engine->face_count; ++k) {
    const uint8_t* blob   = engine->faces[k].blob;
    const int32_t  offset = stbtt_GetFontOffsetForIndex(blob, 0);
    if ((offset < 0) ||
        (stbtt_InitFont(&faces[k + 1U], blob, (int)engine->faces[k].len, offset) == 0)) {
      faces[k + 1U] = faces[0]; /* graceful: bad face -> default */
    }
  }
  *out_n = (uint8_t)(engine->face_count + 1U);
  return k_ra8_ok;
}

/**
 * @brief Shared render body for one page, offsetting every element by (ox, oy).
 *
 * @details
 * Validates @p engine and @p page_idx, then builds the per-render
 * `stbtt_fontinfo` array via `internal_init_faces()`. Iterates over every
 * glyph in `engine->pages[page_idx]`, extracts the face index from the
 * high bits of `g->style`, clamps any out-of-range index to 0 (the
 * default face), and calls `internal_blit_glyph()` with the resolved font.
 * After all glyphs are rendered, `internal_render_images()` blits any image
 * boxes that belong to the page. Both glyph and image positions are
 * shifted by the origin (@p ox, @p oy), allowing callers to composite
 * the page into an arbitrary framebuffer region. This function is the
 * shared body called by both `ra8_reflow_render_page` (ox=oy=0) and
 * `ra8_reflow_render_page_at` (caller-supplied origin).
 *
 * @param[in] engine   Engine handle; must be fully initialised.
 * @param[in] page_idx Zero-based page index to render; must be less than
 *                     `engine->page_count`.
 * @param[in] ox       Horizontal pixel offset added to every glyph and
 *                     image box x-coordinate.
 * @param[in] oy       Vertical pixel offset added to every glyph and
 *                     image box y-coordinate.
 * @return `ra8_err_t` status of the render operation.
 * @retval k_ra8_ok                    Page rendered successfully.
 * @retval k_ra8_err_null_ptr          @p engine is null.
 * @retval k_ra8_err_not_initialized   `engine->in_use` is zero.
 * @retval k_ra8_err_out_of_range      @p page_idx >= `engine->page_count`.
 * @retval k_ra8_err_validation_failed Default font data in the engine is
 *                                    invalid and cannot be initialised.
 * @pre @p engine is non-null and `engine->in_use` is non-zero.
 * @pre @p page_idx is a valid index within `engine->pages[]`.
 * @post On `k_ra8_ok`, all glyphs and images for @p page_idx are blitted
 *       into the bound framebuffer.
 * @post Engine, glyph, and image-box state are unchanged; only the
 *       framebuffer (via `ra8_gfx_pixel()`) is modified.
 * @note Not thread-safe; uses file-static buffers `s_glyph_mask` and
 *       `s_faces` that must not be accessed concurrently.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t
internal_render_page(const ra8_reflow_t* engine, uint32_t page_idx, int32_t ox, int32_t oy)
{
  if (engine == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (engine->in_use == 0U) {
    return k_ra8_err_not_initialized;
  }
  if (page_idx >= engine->page_count) {
    return k_ra8_err_out_of_range;
  }

  /* One fontinfo per face (static -- render is single-threaded, like s_glyph_mask
   * below; keeps the ~1.4 KB off the stack). Default at 0, embedded at 1..N. */
  static stbtt_fontinfo s_faces[1U + (uint32_t)k_ra8_reflow_max_faces];
  uint8_t               nfaces = 0U;
  ra8_err_t             err    = internal_init_faces(engine, s_faces, &nfaces);
  if (err != k_ra8_ok) {
    return err;
  }

  const ra8_reflow_page_t* page = &engine->pages[page_idx];
  for (uint32_t i = 0U; i < page->glyph_count; ++i) {
    const ra8_reflow_glyph_t* g = &engine->glyphs[page->glyph_first + i];
    uint8_t fi = (uint8_t)(((uint32_t)g->style >> (uint32_t)k_ra8_reflow_face_shift) &
                           (uint32_t)k_ra8_reflow_face_mask);
    if (fi >= nfaces) {
      fi = 0U; /* defensive: out-of-range face index -> default */
    }
    internal_blit_glyph(engine->glyph_atlas, fi, &s_faces[fi], g, ox, oy);
  }
  internal_render_images(engine, page_idx, ox, oy);
  return k_ra8_ok;
}

ra8_err_t ra8_reflow_render_page(const ra8_reflow_t* engine, uint32_t page_idx, void* framebuffer)
{
  (void)framebuffer; /* Reserved hook -- ra8_gfx is bound externally. */
  return internal_render_page(engine, page_idx, 0, 0);
}

ra8_err_t ra8_reflow_render_page_at(const ra8_reflow_t* engine,
                                    uint32_t            page_idx,
                                    int32_t             origin_x,
                                    int32_t             origin_y)
{
  return internal_render_page(engine, page_idx, origin_x, origin_y);
}

ra8_err_t ra8_reflow_set_glyph_atlas(ra8_reflow_t*                           engine,
                                     ra8_glyph_atlas_t*                      atlas,
                                     const ra8_reflow_glyph_atlas_storage_t* storage)
{
  if (engine == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (engine->in_use == 0U) {
    return k_ra8_err_not_initialized;
  }
  if (atlas == nullptr) {
    engine->glyph_atlas = nullptr; /* Detach -> revert to direct rasterisation. */
    return k_ra8_ok;
  }
  if (storage == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if ((storage->cell_bytes == 0U) || (storage->cell_count == 0U) || (storage->bucket_count == 0U)) {
    return k_ra8_err_invalid_size;
  }

  const ra8_glyph_atlas_cfg_t cfg = {
    .cell_mem     = storage->cell_mem,
    .cell_bytes   = storage->cell_bytes,
    .cell_count   = storage->cell_count,
    .meta         = storage->meta,
    .keys         = storage->keys,
    .dims         = storage->dims,
    .buckets      = storage->buckets,
    .bucket_count = storage->bucket_count,
    .render       = internal_atlas_render_glyph,
    .render_ctx   = &s_glyph_render_ctx,
  };
  const ra8_err_t err = ra8_glyph_atlas_init(atlas, &cfg);
  if (err != k_ra8_ok) {
    return err;
  }
  engine->glyph_atlas = atlas;
  return k_ra8_ok;
}
