/**
 * @file ra8_viewer_comic.c
 * @brief Comic-archive reader engine: CBZ / CBR / CBT and wrapped variants.
 *
 * @details
 * Drives ra8_comic over the shared seek+read callback. Each page is extracted to
 * an encoded-image scratch buffer and handed to ra8_reflow's
 * `ra8_img_decode_blit`, which decodes it through stb_image (backed by a caller
 * bump arena, so no heap is used inside the decode) and blits it into an RGB565
 * target.
 *
 * Two render surfaces share the extract+decode path: the fixed framebuffer
 * (scaled to fit and centred, for the headless dump) and a native-resolution
 * scroll tile (capped to the ra8_gfx max edge, for the desktop window).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>
#include <stdlib.h>

#include "ra8_comic.h"
#include "ra8_err.h"
#include "ra8_gfx.h"
#include "ra8_img_arena.h"
#include "ra8_reflow_image.h"
#include "ra8_viewer_reader.h"
#include "ra8_viewer_reader_internal.h"

/**
 * @struct viewer_fit_box_t
 * @brief Destination rectangle for a page scaled to fit the framebuffer.
 * @invariant @ref w and @ref h are >= 1 pixel.
 * @since 0.1.0
 */
typedef struct {
  int32_t x; /**< Left edge in framebuffer pixels. */
  int32_t y; /**< Top edge in framebuffer pixels.  */
  int32_t w; /**< Fitted width in pixels.          */
  int32_t h; /**< Fitted height in pixels.         */
} viewer_fit_box_t;

/**
 * @brief Fit a source image into the framebuffer preserving aspect, centred.
 * @details Scales @p src_w x @p src_h to fill the framebuffer width, drops to a
 *          height fit if that would overflow, then centres the result -- so a
 *          page of any aspect lands whole and centred with white margins.
 * @param[in]  src_w Source width in pixels (>= 1).
 * @param[in]  src_h Source height in pixels (>= 1).
 * @param[out] box   Receives the centred destination rectangle (non-NULL).
 * @pre @p src_w and @p src_h are each at least 1.
 * @pre @p box is writable.
 * @post `box->w` and `box->h` are clamped to at least 1 pixel.
 * @post `box->x` / `box->y` centre the box within the framebuffer.
 * @note Pure; thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static void viewer_fit_centered(int32_t src_w, int32_t src_h, viewer_fit_box_t* box)
{
  const int64_t fb_w  = (int64_t)k_ra8_viewer_fb_width;
  const int64_t fb_h  = (int64_t)k_ra8_viewer_fb_height;
  int64_t       fit_w = fb_w;
  int64_t       fit_h = ((int64_t)src_h * fb_w) / (int64_t)src_w;
  if (fit_h > fb_h) {
    fit_h = fb_h;
    fit_w = ((int64_t)src_w * fb_h) / (int64_t)src_h;
  }
  if (fit_w < 1) {
    fit_w = 1;
  }
  if (fit_h < 1) {
    fit_h = 1;
  }
  box->x = (int32_t)((fb_w - fit_w) / 2);
  box->y = (int32_t)((fb_h - fit_h) / 2);
  box->w = (int32_t)fit_w;
  box->h = (int32_t)fit_h;
}

/**
 * @brief Cap @p nw x @p nh to the ra8_gfx max-edge box, preserving aspect.
 * @details ra8_gfx surfaces cap each edge at ::k_ra8_gfx_max_dim, so a tall page
 *          (a manga spread or a long strip) must be rasterised at a reduced size;
 *          since the view scales every tile to the window width anyway, the only
 *          cost is resolution on very tall pages, not aspect.
 * @param[in]  nw Native width in pixels.
 * @param[in]  nh Native height in pixels.
 * @param[out] rw Receives the capped render width (non-NULL, >= 1).
 * @param[out] rh Receives the capped render height (non-NULL, >= 1).
 * @pre @p rw and @p rh are writable.
 * @pre @p nw and @p nh are the page's native dimensions.
 * @post `*rw` and `*rh` are each at least 1 and within ::k_ra8_gfx_max_dim.
 * @post The capped box preserves the source aspect ratio.
 * @note Pure; thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static void viewer_cap_render(uint32_t nw, uint32_t nh, uint32_t* rw, uint32_t* rh)
{
  const uint32_t k = (uint32_t)k_ra8_gfx_max_dim;
  uint32_t       w = nw;
  uint32_t       h = nh;
  if (w > k) {
    h = (uint32_t)(((uint64_t)h * k) / w);
    w = k;
  }
  if (h > k) {
    w = (uint32_t)(((uint64_t)w * k) / h);
    h = k;
  }
  *rw = (w < 1U) ? 1U : w;
  *rh = (h < 1U) ? 1U : h;
}

/**
 * @brief Read page @p page's encoded bytes into the reader's scratch buffer.
 * @details Queries the archive for the page's declared size, rejects a lying or
 *          decompression-bomb declaration before allocating
 *          (`ra8_decomp_check_declared`), grows the scratch buffer to fit, then
 *          reads the encoded image into it.
 * @param[in,out] r    Reader of a comic format (non-NULL).
 * @param[in]     page Page index.
 * @param[out]    got  Receives the byte count read (non-NULL).
 * @return ra8_err_t from `ra8_comic_page_info` / `ra8_comic_page_read`.
 * @retval k_ra8_ok The bytes were read and `*got` holds the count.
 * @pre @p r was opened as a comic format and @p page is valid.
 * @pre @p got is writable.
 * @post On ::k_ra8_ok `r->page_buf` holds `*got` encoded image bytes.
 * @post A rejected declaration returns before any allocation.
 * @note Not thread-safe (drives the shared scratch buffer).
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t
viewer_read_page_bytes(ra8_viewer_reader_t* r, uint32_t page, size_t* got)
{
  uint64_t  raw_size = 0U;
  ra8_err_t rc = ra8_comic_page_info(&r->comic, page, nullptr, 0U, nullptr, &raw_size, nullptr);
  if (rc != k_ra8_ok) {
    return rc;
  }
  /* Refuse a lying or bomb-ratio declaration before any allocation. The whole
   * archive length is a sound upper bound on this entry's compressed bytes, so
   * a declared uncompressed size over the output cap -- or implausible against
   * that compressed bound -- is rejected here (the CBT/tar walker has no
   * open-time size guard, so this is the viewer's own line of defence for it). */
  rc = ra8_decomp_check_declared(&r->limits, r->file.size, raw_size);
  if (rc != k_ra8_ok) {
    return rc;
  }
  rc = viewer_reserve_page_buf(r, (size_t)raw_size);
  if (rc != k_ra8_ok) {
    return rc;
  }
  *got = 0U;
  return ra8_comic_page_read(&r->comic, page, r->page_buf, r->page_cap, got);
}

ra8_err_t viewer_open_comic(ra8_viewer_reader_t* r, bool wrapped)
{
  if (wrapped) {
    return ra8_comic_open_wrapped(&r->comic,
                                  viewer_read,
                                  &r->file,
                                  r->file.size,
                                  r->pages,
                                  (uint32_t)k_viewer_page_cap,
                                  r->names,
                                  (uint32_t)k_viewer_name_cap,
                                  r->unwrap,
                                  (size_t)k_viewer_unwrap_bytes,
                                  r->xz_scratch,
                                  (uint32_t)k_viewer_xz_scratch);
  }
  return ra8_comic_open(&r->comic,
                        viewer_read,
                        &r->file,
                        r->file.size,
                        r->pages,
                        (uint32_t)k_viewer_page_cap,
                        r->names,
                        (uint32_t)k_viewer_name_cap);
}

ra8_err_t viewer_render_comic(ra8_viewer_reader_t* r, uint32_t page)
{
  size_t    got = 0U;
  ra8_err_t rc  = viewer_read_page_bytes(r, page, &got);
  if (rc != k_ra8_ok) {
    return rc;
  }

  rc = ra8_gfx_init(r->fb, k_ra8_viewer_fb_width, k_ra8_viewer_fb_height, k_ra8_gfx_format_rgb565);
  if (rc != k_ra8_ok) {
    return rc;
  }
  (void)ra8_gfx_clear((uint32_t)k_viewer_bg);

  /* stb's allocator is routed through ra8_img_arena; a JPEG header probe needs a
   * bound arena to allocate its decode context (PNG does not), so bind around the
   * probe. ra8_img_decode_blit binds its own arena below. */
  int32_t         src_w       = 0;
  int32_t         src_h       = 0;
  ra8_img_arena_t probe_arena = {.base = r->arena_mem, .cap = (size_t)k_viewer_arena_bytes};
  ra8_img_arena_bind(&probe_arena);
  rc = ra8_img_probe_size(r->page_buf, got, &src_w, &src_h);
  ra8_img_arena_unbind();
  if (rc != k_ra8_ok) {
    return rc;
  }
  viewer_fit_box_t box = {.x = 0, .y = 0, .w = k_ra8_viewer_fb_width, .h = k_ra8_viewer_fb_height};
  viewer_fit_centered(src_w, src_h, &box);

  ra8_img_arena_t arena = {.base = r->arena_mem, .cap = (size_t)k_viewer_arena_bytes};
  return ra8_img_decode_blit(&arena,
                             r->page_buf,
                             got,
                             box.x,
                             box.y,
                             box.w,
                             box.h,
                             nullptr,
                             nullptr);
}

ra8_err_t
viewer_tile_comic(ra8_viewer_reader_t* r, uint32_t i, uint32_t* w, uint32_t* h, uint16_t** out)
{
  const uint32_t nw = r->tile_wpx[i];
  const uint32_t nh = r->tile_hpx[i];
  if ((nw == 0U) || (nh == 0U)) {
    return k_ra8_err_not_supported; /* page that failed to probe at open */
  }
  uint32_t rw = 0U;
  uint32_t rh = 0U;
  viewer_cap_render(nw, nh, &rw, &rh);

  size_t    got = 0U;
  ra8_err_t rc  = viewer_read_page_bytes(r, i, &got);
  if (rc != k_ra8_ok) {
    return rc;
  }
  /* rw/rh are already clamped to k_ra8_gfx_max_dim, but size the destination
   * through a widened multiply and the output cap so the allocation is
   * provably bounded (and cannot wrap size_t) whatever the render cap becomes. */
  const uint64_t buf_bytes = (uint64_t)rw * (uint64_t)rh * (uint64_t)sizeof(uint16_t);
  if (buf_bytes > r->limits.max_output_bytes) {
    return k_ra8_err_decomp_output_cap;
  }
  uint16_t* buf = (uint16_t*)malloc((size_t)buf_bytes);
  if (buf == nullptr) {
    free(buf);
    return k_ra8_err_no_mem;
  }
  rc = ra8_gfx_init(buf, (uint16_t)rw, (uint16_t)rh, k_ra8_gfx_format_rgb565);
  if (rc == k_ra8_ok) {
    (void)ra8_gfx_clear((uint32_t)k_viewer_bg);
    ra8_img_arena_t arena = {.base = r->arena_mem, .cap = (size_t)k_viewer_arena_bytes};
    rc                    = ra8_img_decode_blit(&arena,
                                                r->page_buf,
                                                got,
                                                0,
                                                0,
                                                (int32_t)rw,
                                                (int32_t)rh,
                                                nullptr,
                                                nullptr);
  }
  if (rc != k_ra8_ok) {
    free(buf);
    return rc;
  }
  *w   = rw;
  *h   = rh;
  *out = buf;
  return k_ra8_ok;
}

void viewer_probe_comic_tile(ra8_viewer_reader_t* r, uint32_t i, ra8_img_arena_t* arena)
{
  size_t got = 0U;
  if (viewer_read_page_bytes(r, i, &got) != k_ra8_ok) {
    return;
  }
  int32_t pw = 0;
  int32_t ph = 0;
  ra8_img_arena_bind(arena);
  const ra8_err_t prc = ra8_img_probe_size(r->page_buf, got, &pw, &ph);
  ra8_img_arena_unbind();
  if ((prc == k_ra8_ok) && (pw > 0) && (ph > 0)) {
    r->tile_wpx[i] = (uint32_t)pw;
    r->tile_hpx[i] = (uint32_t)ph;
  }
}
