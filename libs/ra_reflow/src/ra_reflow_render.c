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
#include <string.h>

#include "ra_err.h"
#include "ra_gfx.h"
#include "ra_reflow.h"
#include "ra_reflow_svg.h"
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
  k_priv_alpha_threshold     = 96U,  /**< Coverage cutoff for binary blit.               */
  k_priv_alpha_max_byte      = 255U, /**< Upper bound from stb's mask.                   */
  k_priv_underline_offset_px = 2U,   /**< Pixels below baseline for the underline.       */
  k_priv_underline_thick_px  = 1U,   /**< Thickness of the anchor underline.             */
  k_priv_glyph_dim_max       = 192U, /**< Mask edge bound = 2 * k_ra_reflow_max_font_px. */
  k_priv_svg_href_max        = 256U, /**< Max unwrapped SVG cover-image href length.     */
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
 * @param[in] ox Pixel offset added to every glyph x.
 * @param[in] oy Pixel offset added to every glyph y.
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
                                 int                      yoff,
                                 int32_t                  ox,
                                 int32_t                  oy)
{
  for (int row = 0; row < h; ++row) {
    for (int col = 0; col < w; ++col) {
      const uint8_t alpha = (uint8_t)bitmap[((size_t)row * (size_t)w) + (size_t)col];
      if (alpha < k_priv_alpha_threshold) {
        continue;
      }
      const int32_t px = g->x + (int32_t)col + (int32_t)xoff + ox;
      const int32_t py = g->y + (int32_t)row + (int32_t)yoff + oy;
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
 * @param[in] ox Pixel offset added to the underline x.
 * @param[in] oy Pixel offset added to the underline y.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static void priv_draw_underline(const stbtt_fontinfo*    font,
                                const ra_reflow_glyph_t* g,
                                float                    scale,
                                int32_t                  ox,
                                int32_t                  oy)
{
  int advance_units = 0;
  int lsb           = 0;
  stbtt_GetCodepointHMetrics(font, g->cp, &advance_units, &lsb);
  const int32_t advance = (int32_t)((float)advance_units * scale);
  const int32_t y_under = g->y + (int32_t)k_priv_underline_offset_px + oy;
  for (int32_t i = 0; i < advance; ++i) {
    (void)ra_gfx_pixel(g->x + i + ox, y_under, g->color);
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
 * @param[in] ox Pixel offset added to every glyph x.
 * @param[in] oy Pixel offset added to every glyph y.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static void
priv_blit_glyph(const stbtt_fontinfo* font, const ra_reflow_glyph_t* g, int32_t ox, int32_t oy)
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
      priv_blit_alpha_mask(g, s_glyph_mask, w, h, x0, y0, ox, oy);
    }
  }

  if ((g->style & k_ra_reflow_style_underline) != 0U) {
    priv_draw_underline(font, g, scale, ox, oy);
  }
}

/**
 * @brief Render an SVG image box: unwrap a cover `<image>` href, else draw shapes.
 *
 * @details
 * A cover-wrapper SVG (`<svg><image href=.../></svg>`) is detected by
 * `ra_svg_image_href()`. When found, the href string is copied into a
 * stack buffer (bounded by `k_priv_svg_href_max`) before re-invoking
 * `engine->img_loader` to fetch the referenced raster; the copy is
 * necessary because the loader call may overwrite the SVG buffer. The
 * decoded raster is then blitted by `ra_img_decode_blit()` into the
 * supplied bounding box. When no `<image>` wrapper is detected, the SVG
 * is passed directly to `ra_svg_render()` as a shape document
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
static void priv_render_svg(const ra_reflow_t* engine,
                            const uint8_t*     svg,
                            size_t             len,
                            int32_t            x,
                            int32_t            y,
                            int32_t            w,
                            int32_t            h)
{
  size_t hoff = 0U;
  size_t hlen = 0U;
  if (ra_svg_image_href(svg, len, &hoff, &hlen) != k_ra_ok) {
    (void)ra_svg_render(svg, len, x, y, w, h);
    return;
  }
  char href[k_priv_svg_href_max] = {};
  if (hlen >= sizeof(href)) {
    return;
  }
  (void)memcpy(href, &svg[hoff], hlen);
  const uint8_t* rbytes = nullptr;
  size_t         rlen   = 0U;
  if (engine->img_loader(engine->img_loader_ctx, href, (uint32_t)hlen, &rbytes, &rlen) == k_ra_ok) {
    (void)ra_img_decode_blit(engine->img_arena, rbytes, rlen, x, y, w, h, nullptr, nullptr);
  }
}

/**
 * @brief Render one image box: SVG-route or raster-decode at the page offset.
 *
 * @details
 * Resolves the source href stored at `engine->text_pool[box->src_off]`
 * (length `box->src_len`) through `engine->img_loader`. On a successful
 * load, the bytes are inspected by `ra_svg_is_svg()`: SVG content is
 * forwarded to `priv_render_svg()` (which handles both cover-wrapper and
 * shape SVGs); all other formats are decoded directly by
 * `ra_img_decode_blit()`. In both cases the bounding rectangle is
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
static void priv_render_one_image(const ra_reflow_t*           engine,
                                  const ra_reflow_image_box_t* box,
                                  int32_t                      ox,
                                  int32_t                      oy)
{
  const char*    href  = (const char*)&engine->text_pool[box->src_off];
  const uint8_t* bytes = nullptr;
  size_t         blen  = 0U;
  if (engine->img_loader(engine->img_loader_ctx, href, box->src_len, &bytes, &blen) != k_ra_ok) {
    return;
  }
  const int32_t bx = box->x + ox;
  const int32_t by = box->y + oy;
  if (ra_svg_is_svg(bytes, blen)) {
    priv_render_svg(engine, bytes, blen, bx, by, box->w, box->h);
    return;
  }
  (void)
    ra_img_decode_blit(engine->img_arena, bytes, blen, bx, by, box->w, box->h, nullptr, nullptr);
}

/**
 * @brief Decode and blit every laid-out image that belongs to one page.
 *
 * @details
 * Walks `engine->image_boxes[0..image_box_count-1]` and calls
 * `priv_render_one_image()` for each box whose `page_index` matches
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
 * @pre A framebuffer is bound via `ra_gfx_init()`.
 * @post Every decodable image on @p page_idx is blitted into the
 *       framebuffer; failed boxes are silently skipped.
 * @post Engine state is unchanged; the arena drains after each decode.
 * @note Not thread-safe; the decode arena uses a file-static allocation
 *       pool that must not be re-entered.
 * @since 0.1.0
 */
static void priv_render_images(const ra_reflow_t* engine, uint32_t page_idx, int32_t ox, int32_t oy)
{
  if ((engine->img_loader == nullptr) || (engine->img_arena == nullptr)) {
    return;
  }
  for (uint32_t i = 0U; i < engine->image_box_count; ++i) {
    const ra_reflow_image_box_t* box = &engine->image_boxes[i];
    if (box->page_index == page_idx) {
      priv_render_one_image(engine, box, ox, oy);
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
 * `priv_init_font()`. Slots `1..face_count` are the registered embedded
 * `@font-face` blobs from `engine->faces[]`. A registered blob that
 * fails `stbtt_InitFont` (already validated at register time, so this
 * path is unexpected) is replaced by a copy of the default face so that
 * every slot is always initialised and glyph rendering never indexes an
 * invalid fontinfo. `*out_n` is set to `engine->face_count + 1`.
 *
 * @param[in]  engine  Engine holding the default font blob and face registry.
 * @param[out] faces   Caller-allocated array of at least
 *                     `1 + k_ra_reflow_max_faces` `stbtt_fontinfo` entries.
 * @param[out] out_n   Receives the count of valid entries written
 *                     (`1 + engine->face_count`).
 * @return `k_ra_ok` on success, or the error returned by
 *         `priv_init_font()` if the default face cannot be initialised.
 * @retval k_ra_ok           All face slots initialised successfully.
 * @retval k_ra_err_validation_failed  Default face font data is invalid.
 * @pre @p engine is non-null and `engine->font_data` points to a valid
 *      TrueType/OpenType blob.
 * @pre @p faces and @p out_n are non-null; @p faces has capacity for at
 *      least `1 + k_ra_reflow_max_faces` entries.
 * @post On `k_ra_ok`, `faces[0..*out_n - 1]` are all initialised; each
 *       slot that failed individual init holds a copy of `faces[0]`.
 * @post `*out_n` equals `engine->face_count + 1` on success; its value
 *       is unspecified on error.
 * @note Not thread-safe; `s_faces` is a file-static array shared across
 *       calls.
 * @since 0.1.0
 */
static ra_err_t priv_init_faces(const ra_reflow_t* engine, stbtt_fontinfo* faces, uint8_t* out_n)
{
  const ra_err_t err = priv_init_font(engine, &faces[0]);
  if (err != k_ra_ok) {
    return err;
  }
  for (uint8_t k = 0U; k < engine->face_count; ++k) {
    const uint8_t* blob   = engine->faces[k].blob;
    const int32_t  offset = stbtt_GetFontOffsetForIndex(blob, 0);
    if ((offset < 0) || (stbtt_InitFont(&faces[k + 1U], blob, offset) == 0)) {
      faces[k + 1U] = faces[0]; /* graceful: bad face -> default */
    }
  }
  *out_n = (uint8_t)(engine->face_count + 1U);
  return k_ra_ok;
}

/**
 * @brief Shared render body for one page, offsetting every element by (ox, oy).
 *
 * @details
 * Validates @p engine and @p page_idx, then builds the per-render
 * `stbtt_fontinfo` array via `priv_init_faces()`. Iterates over every
 * glyph in `engine->pages[page_idx]`, extracts the face index from the
 * high bits of `g->style`, clamps any out-of-range index to 0 (the
 * default face), and calls `priv_blit_glyph()` with the resolved font.
 * After all glyphs are rendered, `priv_render_images()` blits any image
 * boxes that belong to the page. Both glyph and image positions are
 * shifted by the origin (@p ox, @p oy), allowing callers to composite
 * the page into an arbitrary framebuffer region. This function is the
 * shared body called by both `ra_reflow_render_page` (ox=oy=0) and
 * `ra_reflow_render_page_at` (caller-supplied origin).
 *
 * @param[in] engine   Engine handle; must be fully initialised.
 * @param[in] page_idx Zero-based page index to render; must be less than
 *                     `engine->page_count`.
 * @param[in] ox       Horizontal pixel offset added to every glyph and
 *                     image box x-coordinate.
 * @param[in] oy       Vertical pixel offset added to every glyph and
 *                     image box y-coordinate.
 * @return `ra_err_t` status of the render operation.
 * @retval k_ra_ok                    Page rendered successfully.
 * @retval k_ra_err_null_ptr          @p engine is null.
 * @retval k_ra_err_not_initialized   `engine->in_use` is zero.
 * @retval k_ra_err_out_of_range      @p page_idx >= `engine->page_count`.
 * @retval k_ra_err_validation_failed Default font data in the engine is
 *                                    invalid and cannot be initialised.
 * @pre @p engine is non-null and `engine->in_use` is non-zero.
 * @pre @p page_idx is a valid index within `engine->pages[]`.
 * @post On `k_ra_ok`, all glyphs and images for @p page_idx are blitted
 *       into the bound framebuffer.
 * @post Engine, glyph, and image-box state are unchanged; only the
 *       framebuffer (via `ra_gfx_pixel()`) is modified.
 * @note Not thread-safe; uses file-static buffers `s_glyph_mask` and
 *       `s_faces` that must not be accessed concurrently.
 * @since 0.1.0
 */
static ra_err_t
priv_render_page(const ra_reflow_t* engine, uint32_t page_idx, int32_t ox, int32_t oy)
{
  if (engine == nullptr) {
    return k_ra_err_null_ptr;
  }
  if (engine->in_use == 0U) {
    return k_ra_err_not_initialized;
  }
  if (page_idx >= engine->page_count) {
    return k_ra_err_out_of_range;
  }

  /* One fontinfo per face (static -- render is single-threaded, like s_glyph_mask
   * below; keeps the ~1.4 KB off the stack). Default at 0, embedded at 1..N. */
  static stbtt_fontinfo s_faces[1U + (uint32_t)k_ra_reflow_max_faces];
  uint8_t               nfaces = 0U;
  ra_err_t              err    = priv_init_faces(engine, s_faces, &nfaces);
  if (err != k_ra_ok) {
    return err;
  }

  const ra_reflow_page_t* page = &engine->pages[page_idx];
  for (uint32_t i = 0U; i < page->glyph_count; ++i) {
    const ra_reflow_glyph_t* g = &engine->glyphs[page->glyph_first + i];
    uint8_t fi = (uint8_t)(((uint32_t)g->style >> (uint32_t)k_ra_reflow_face_shift) &
                           (uint32_t)k_ra_reflow_face_mask);
    if (fi >= nfaces) {
      fi = 0U; /* defensive: out-of-range face index -> default */
    }
    priv_blit_glyph(&s_faces[fi], g, ox, oy);
  }
  priv_render_images(engine, page_idx, ox, oy);
  return k_ra_ok;
}

ra_err_t ra_reflow_render_page(const ra_reflow_t* engine, uint32_t page_idx, void* framebuffer)
{
  (void)framebuffer; /* Reserved hook -- ra_gfx is bound externally. */
  return priv_render_page(engine, page_idx, 0, 0);
}

ra_err_t ra_reflow_render_page_at(const ra_reflow_t* engine,
                                  uint32_t           page_idx,
                                  int32_t            origin_x,
                                  int32_t            origin_y)
{
  return priv_render_page(engine, page_idx, origin_x, origin_y);
}
