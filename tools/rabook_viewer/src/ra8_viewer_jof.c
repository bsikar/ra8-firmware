/**
 * @file ra8_viewer_jof.c
 * @brief JOF long-strip reader engine: vertical-scroll tile atlas rendering.
 *
 * @details
 * Drives ra8_longstrip over an ra8_tile_cache for the firmware-native `.jof`
 * tile atlas the downloader writes with `ra8_jof_produce`. The whole atlas
 * is slurped into memory and read back through a memstore pread, so the engine
 * sees exactly the demand-paged interface it uses on the board; DEFLATE bands
 * are decoded on cache miss into a fixed pool of LRU cells.
 *
 * The tall canvas is paginated into viewport-height windows. Each viewer "page"
 * scrolls the strip by one viewport and composites the visible bands through a
 * viewer-owned blit that converts whatever the strip's native bit depth is
 * (gray / RGB565 / RGB / RGBA) into the RGB565 target.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * @since 0.1.0
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "ra8_err.h"
#include "ra8_jof.h"
#include "ra8_longstrip.h"
#include "ra8_tile_cache.h"
#include "ra8_viewer_reader.h"
#include "ra8_viewer_reader_internal.h"

/**
 * @enum ra8_viewer_bpp_t
 * @brief Source bit depths a long-strip band can arrive in, in bytes per pixel.
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_viewer_bpp_gray   = 1U, /**< 8-bit grayscale.              */
  k_viewer_bpp_rgb565 = 2U, /**< Packed RGB565, little-endian. */
  k_viewer_bpp_rgb    = 3U, /**< 8-bit RGB.                    */
} ra8_viewer_bpp_t;

/**
 * @enum ra8_viewer_px_idx_t
 * @brief Byte offsets of the colour channels within one source pixel.
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_viewer_px_r = 0U, /**< Red (or gray, or RGB565 low byte). */
  k_viewer_px_g = 1U, /**< Green (or RGB565 high byte).       */
  k_viewer_px_b = 2U, /**< Blue.                              */
} ra8_viewer_px_idx_t;

/**
 * @brief Convert one source pixel of @p bpp bytes into a packed RGB565 word.
 * @details Handles the four source formats the strip decoder emits: 1-byte gray
 *          (replicated to R=G=B), 2-byte little-endian RGB565 (repacked), and
 *          3-byte RGB or 4-byte RGBA (the alpha byte dropped).
 * @param[in] sp  Source pixel (non-NULL, at least @p bpp bytes readable).
 * @param[in] bpp Bytes per source pixel (1, 2, 3, or 4).
 * @return The packed RGB565 value; an RGBA alpha byte is dropped.
 * @retval 0 The source pixel is black.
 * @pre @p sp points at @p bpp readable bytes.
 * @pre @p bpp is one of 1, 2, 3 or 4.
 * @post No state is mutated.
 * @post The result is a native-endian RGB565 word.
 * @note Pure; thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static uint16_t viewer_band_pixel(const uint8_t* sp, uint8_t bpp)
{
  if (bpp == (uint8_t)k_viewer_bpp_gray) {
    return ra8_viewer_pack565(sp[k_viewer_px_r], sp[k_viewer_px_r], sp[k_viewer_px_r]);
  }
  if (bpp == (uint8_t)k_viewer_bpp_rgb565) {
    return ra8_viewer_pack565_le_pair(sp[k_viewer_px_r], sp[k_viewer_px_g]);
  }
  /* bpp 3 (RGB) or 4 (RGBA, alpha dropped). */
  return ra8_viewer_pack565(sp[k_viewer_px_r], sp[k_viewer_px_g], sp[k_viewer_px_b]);
}

/**
 * @brief ra8_longstrip composite sink: convert a band sub-window to RGB565.
 * @details Coordinates are viewport-relative; the whole viewport is centred
 *          horizontally in the target via `jof.x_off`. Pixels outside the target
 *          are clipped, so a partial last page keeps its background margin.
 * @param[in] ctx   The ::ra8_viewer_reader_t (for the blit target and x_off).
 * @param[in] px    Band pixels, row-major, @p bpp bytes per pixel.
 * @param[in] sw    Sub-window width in pixels.
 * @param[in] sh    Sub-window height in pixels.
 * @param[in] bpp   Bytes per source pixel.
 * @param[in] dst_x Viewport-relative destination x of the sub-window.
 * @param[in] dst_y Viewport-relative destination y of the sub-window.
 * @return Always ::k_ra8_ok -- out-of-bounds pixels are dropped, not an error.
 * @retval k_ra8_ok Always; clipping is not treated as failure.
 * @pre @p ctx is the ::ra8_viewer_reader_t whose blit target is set.
 * @pre @p px points at @p sw * @p sh * @p bpp readable bytes.
 * @post In-bounds pixels of the sub-window are written to the blit target.
 * @post Pixels outside the target rectangle are dropped, not clamped.
 * @note Not thread-safe (writes the shared blit target).
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t viewer_jof_blit(void*          ctx,
                                              const uint8_t* px,
                                              uint16_t       sw,
                                              uint16_t       sh,
                                              uint8_t        bpp,
                                              int32_t        dst_x,
                                              int32_t        dst_y)
{
  ra8_viewer_reader_t* r     = (ra8_viewer_reader_t*)ctx;
  const int32_t        x_off = r->jof.x_off;
  const int32_t        tw    = (int32_t)r->rt_w;
  const int32_t        th    = (int32_t)r->rt_h;
  for (int32_t row = 0; row < (int32_t)sh; ++row) {
    const int32_t fy = dst_y + row;
    if ((fy < 0) || (fy >= th)) {
      continue;
    }
    for (int32_t col = 0; col < (int32_t)sw; ++col) {
      const int32_t fx = dst_x + col + x_off;
      if ((fx < 0) || (fx >= tw)) {
        continue;
      }
      const uint8_t* sp = &px[(((size_t)row * (size_t)sw) + (size_t)col) * (size_t)bpp];
      r->rt565[((size_t)fy * (size_t)tw) + (size_t)fx] = viewer_band_pixel(sp, bpp);
    }
  }
  return k_ra8_ok;
}

/**
 * @brief Slurp the open JOF file into @p r->jof.atlas and set up the memstore.
 * @details Refuses a file larger than the per-unit output cap before allocating
 *          (the atlas is held wholly resident), then reads the whole file into an
 *          owned buffer and points the memstore pread at it, so tile decodes read
 *          straight from RAM.
 * @param[in,out] r Reader whose file is already open (non-NULL).
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                    The atlas is resident and memstore wired.
 * @retval k_ra8_err_decomp_output_cap The file exceeds the per-unit output cap.
 * @retval k_ra8_err_no_mem            The atlas buffer could not be allocated.
 * @retval k_ra8_err_not_found         The file could not be rewound or read.
 * @pre @p r->file wraps the open `.jof` stream.
 * @pre @p r->jof.atlas is not already allocated.
 * @post On ::k_ra8_ok `r->jof.atlas` owns the file and the memstore spans it.
 * @post On any error the partial buffer is freed.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t viewer_jof_slurp(ra8_viewer_reader_t* r)
{
  /* The whole atlas is held resident in RAM, so a file larger than the per-unit
   * output cap is refused before allocating -- the same working-set ceiling the
   * board's SDRAM decode obeys, and the check that keeps `(size_t)file.size`
   * from truncating a > 4 GiB length on the 32-bit target. */
  if (r->file.size > r->limits.max_output_bytes) {
    return k_ra8_err_decomp_output_cap;
  }
  const size_t sz  = (size_t)r->file.size;
  uint8_t*     buf = (uint8_t*)malloc(sz);
  if (buf == nullptr) {
    return k_ra8_err_no_mem;
  }
  if (fseeko(r->file.fp, 0, SEEK_SET) != 0) {
    free(buf);
    return k_ra8_err_not_found;
  }
  if (fread(buf, 1U, sz, r->file.fp) != sz) {
    free(buf);
    return k_ra8_err_not_found;
  }
  r->jof.atlas = buf;
  r->jof.store = (ra8_jof_memstore_t){.buf = buf, .cap = sz, .len = sz};
  return k_ra8_ok;
}

/**
 * @brief Allocate the JOF tile-cache backing arrays for @p cell_count bands.
 * @details Allocates the six backing buffers the band LRU needs -- the decode
 *          scratch (one band plus slack), the cell pool, and the metadata, key,
 *          dims and bucket arrays. Partial allocations are left in place for
 *          viewer_free() to release, so there is no cleanup on the error path.
 * @param[in,out] w          JOF state to populate (non-NULL).
 * @param[in]     cell_count Resident-band count (>= 1).
 * @param[in]     band_bytes Bytes per decoded band (> 0).
 * @return ra8_err_t; ::k_ra8_err_no_mem on any allocation failure.
 * @retval k_ra8_ok         Every backing buffer was allocated.
 * @retval k_ra8_err_no_mem At least one allocation failed.
 * @pre @p w is a JOF state with NULL backing pointers.
 * @pre @p cell_count >= 1 and @p band_bytes > 0.
 * @post On ::k_ra8_ok every cache backing buffer is allocated.
 * @post Partial allocations are retained for viewer_free() to release.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t
viewer_jof_alloc_cache(viewer_jof_t* w, uint32_t cell_count, size_t band_bytes)
{
  w->scratch = (uint8_t*)malloc(band_bytes + (size_t)k_viewer_jof_scratch_pad);
  w->cells   = (uint8_t*)malloc((size_t)cell_count * band_bytes);
  w->meta    = (ra8_keycache_cell_t*)calloc(cell_count, sizeof(*w->meta));
  w->keys    = (ra8_tile_key_t*)calloc(cell_count, sizeof(*w->keys));
  w->dims    = (ra8_tile_dims_t*)calloc(cell_count, sizeof(*w->dims));
  w->buckets = (int32_t*)calloc((size_t)k_viewer_jof_buckets, sizeof(*w->buckets));
  if ((w->scratch == nullptr) || (w->cells == nullptr) || (w->meta == nullptr) ||
      (w->keys == nullptr) || (w->dims == nullptr) || (w->buckets == nullptr)) {
    return k_ra8_err_no_mem;
  }
  return k_ra8_ok;
}

/**
 * @brief Wire the tile cache over the parsed atlas and open the strip engine.
 * @details Composition happens at the strip's native width; the desktop view
 *          scales each tile to the window width, and the headless framebuffer
 *          path recentres/clips into its fixed width via `x_off` set per render.
 * @param[in,out] r          Reader whose JOF buffers are allocated (non-NULL).
 * @param[in]     info       Parsed atlas geometry (non-NULL).
 * @param[in]     band_bytes Bytes per decoded band.
 * @param[in]     cell_count Resident-band count for the LRU cache.
 * @return ra8_err_t from the tile-cache init / long-strip-open steps.
 * @retval k_ra8_ok The cache and strip engine are initialised and ready.
 * @pre @p r's JOF backing arrays are allocated (viewer_jof_alloc_cache()).
 * @pre @p info is the parsed geometry of the resident atlas.
 * @post On ::k_ra8_ok `r->jof.strip` is ready to scroll and render.
 * @post On any error the strip is left unopened.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t viewer_jof_wire(ra8_viewer_reader_t*  r,
                                              const ra8_jof_info_t* info,
                                              size_t                band_bytes,
                                              uint32_t              cell_count)
{
  viewer_jof_t* w     = &r->jof;
  w->dctx.scratch     = w->scratch;
  w->dctx.scratch_cap = (uint32_t)(band_bytes + (size_t)k_viewer_jof_scratch_pad);

  const ra8_tile_cache_cfg_t ccfg = {.cell_mem     = w->cells,
                                     .cell_bytes   = (uint32_t)band_bytes,
                                     .cell_count   = cell_count,
                                     .meta         = w->meta,
                                     .keys         = w->keys,
                                     .dims         = w->dims,
                                     .buckets      = w->buckets,
                                     .bucket_count = (uint32_t)k_viewer_jof_buckets,
                                     .decode       = ra8_longstrip_tile_decode,
                                     .decode_ctx   = &w->dctx};
  const ra8_err_t            rc   = ra8_tile_cache_init(&w->cache, &ccfg);
  if (rc != k_ra8_ok) {
    return rc;
  }

  w->viewport_h = (uint32_t)k_ra8_viewer_fb_height;
  w->x_off      = 0;

  const ra8_longstrip_cfg_t wcfg = {.pread      = ra8_jof_memstore_pread,
                                    .pread_ctx  = &w->store,
                                    .atlas_size = r->file.size,
                                    .cache      = &w->cache,
                                    .image_id   = 1U,
                                    .viewport_w = info->width,
                                    .viewport_h = (uint16_t)k_ra8_viewer_fb_height,
                                    .blit       = viewer_jof_blit,
                                    .blit_ctx   = r};
  return ra8_longstrip_open(&w->strip, &wcfg);
}

/**
 * @brief Scroll the strip so that page @p page sits at the top of the viewport.
 * @details The strip's scroll clamp is derived from its viewport height, so a
 *          paginated caller must first tell the engine how tall the page it is
 *          about to draw actually is. The final page of a strip whose height is
 *          not a whole multiple of the page height is SHORT; leaving the engine
 *          at the full page height would clamp this seek backwards to
 *          `canvas_h - page_height` and re-composite a window the previous page
 *          already showed -- visible as duplicated content with a seam.
 *          Setting the viewport to the page's real content height makes the
 *          requested position land exactly on the recomputed clamp.
 * @param[in,out] w    JOF state of an open strip (non-NULL).
 * @param[in]     page Vertical page index.
 * @return ra8_err_t from `ra8_longstrip_set_viewport` / `ra8_longstrip_scroll_by`.
 * @retval k_ra8_ok               The strip is positioned at page @p page.
 * @retval k_ra8_err_out_of_range @p page lies past the end of the strip.
 * @pre @p w is an open JOF strip.
 * @pre @p page is a candidate page index down the strip.
 * @post On ::k_ra8_ok the viewport top sits at page @p page's content.
 * @post The viewport height is set to the page's real (possibly short) height.
 * @note Not thread-safe (mutates the strip's scroll state).
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t viewer_jof_seek(viewer_jof_t* w, uint32_t page)
{
  const uint32_t top     = page * w->viewport_h;
  const uint32_t canvas  = w->dctx.info.height;
  const uint32_t remain  = (canvas > top) ? (canvas - top) : 0U;
  const uint32_t content = (remain < w->viewport_h) ? remain : w->viewport_h;
  if (content == 0U) {
    return k_ra8_err_out_of_range;
  }
  const ra8_err_t vrc =
    ra8_longstrip_set_viewport(&w->strip, w->dctx.info.width, (uint16_t)content);
  if (vrc != k_ra8_ok) {
    return vrc;
  }
  const int32_t delta = (int32_t)top - w->strip.scroll_y;
  return ra8_longstrip_scroll_by(&w->strip, delta);
}

ra8_err_t viewer_open_jof(ra8_viewer_reader_t* r)
{
  viewer_jof_t* w  = &r->jof;
  ra8_err_t     rc = viewer_jof_slurp(r);
  if (rc != k_ra8_ok) {
    return rc;
  }

  w->dctx.pread     = ra8_jof_memstore_pread;
  w->dctx.pread_ctx = &w->store;
  rc                = ra8_jof_parse(ra8_jof_memstore_pread, &w->store, r->file.size, &w->dctx.info);
  if (rc != k_ra8_ok) {
    return rc;
  }
  const ra8_jof_info_t info = w->dctx.info;
  /* One decoded band is tile_w * tile_h * bpp bytes. tile_w/tile_h are untrusted
   * 16-bit header fields that ra8_jof_parse does NOT bound against the image
   * size, so form the product in 64 bits (u16*u16*u8 cannot wrap) and refuse it
   * against the policy before narrowing to size_t: an absurd geometry -- e.g.
   * 65535 x 65535 x 4 ~= 17 GiB -- can neither overflow the allocation size nor
   * be attempted. */
  const uint64_t band_bytes64 = (uint64_t)info.tile_w * (uint64_t)info.tile_h * (uint64_t)info.bpp;
  if (band_bytes64 == 0U) {
    return k_ra8_err_invalid_size;
  }
  rc = ra8_decomp_check_declared(&r->limits, r->file.size, band_bytes64);
  if (rc != k_ra8_ok) {
    return rc;
  }
  const size_t band_bytes = (size_t)band_bytes64; /* proven <= max_output_bytes */
  uint32_t     cell_count = info.tile_rows;
  if (cell_count > (uint32_t)k_viewer_jof_max_cells) {
    cell_count = (uint32_t)k_viewer_jof_max_cells;
  }
  if (cell_count == 0U) {
    cell_count = 1U;
  }
  /* Bound the resident cache (cell_count * band_bytes) by the same output cap:
   * keep at most as many bands resident as the budget admits, always >= 1 since
   * one band already fits the cap. Fewer resident bands only means more
   * decode-on-miss churn -- every tile still decodes correctly -- so this bounds
   * the allocation without ever refusing a valid atlas. */
  uint32_t max_resident = (uint32_t)(r->limits.max_output_bytes / band_bytes64);
  if (max_resident == 0U) {
    max_resident = 1U;
  }
  if (cell_count > max_resident) {
    cell_count = max_resident;
  }
  rc = viewer_jof_alloc_cache(w, cell_count, band_bytes);
  if (rc != k_ra8_ok) {
    return rc;
  }
  return viewer_jof_wire(r, &info, band_bytes, cell_count);
}

ra8_err_t viewer_render_jof(ra8_viewer_reader_t* r, uint32_t page)
{
  viewer_jof_t* w = &r->jof;
  /* Headless path: compose into the fixed framebuffer, recentring the
   * native-width strip and clipping any overhang. */
  r->rt565         = r->fb;
  r->rt_w          = (uint32_t)k_ra8_viewer_fb_width;
  r->rt_h          = (uint32_t)k_ra8_viewer_fb_height;
  const int32_t vw = (int32_t)w->dctx.info.width;
  w->x_off         = ((int32_t)r->rt_w > vw) ? (((int32_t)r->rt_w - vw) / 2) : 0;
  /* Clear to white first: a partial last page keeps a paper-white bottom margin.
   * 0xFF bytes == 0xFFFF RGB565 == white. */
  const size_t fb_pixels = (size_t)r->rt_w * (size_t)r->rt_h;
  memset(r->fb, (int)k_viewer_white_byte, fb_pixels * sizeof(*r->fb));

  const ra8_err_t rc = viewer_jof_seek(w, page);
  if (rc != k_ra8_ok) {
    return rc;
  }
  ra8_longstrip_render_stats_t st = {};
  return ra8_longstrip_render(&w->strip, &st);
}

ra8_err_t
viewer_tile_jof(ra8_viewer_reader_t* r, uint32_t i, uint32_t* w, uint32_t* h, uint16_t** out)
{
  viewer_jof_t*  wt = &r->jof;
  const uint32_t nw = r->tile_wpx[i];
  const uint32_t nh = r->tile_hpx[i];
  if ((nw == 0U) || (nh == 0U)) {
    return k_ra8_err_invalid_size;
  }
  /* nw is the atlas width (bounded to k_ra8_jof_max_dim by ra8_jof_parse) and nh
   * is one viewport band (<= the fixed framebuffer height), so this product is
   * bounded by those prior policy checks and cannot wrap size_t. */
  const size_t px  = (size_t)nw * (size_t)nh;
  uint16_t*    buf = (uint16_t*)malloc(px * sizeof(uint16_t));
  if (buf == nullptr) {
    return k_ra8_err_no_mem;
  }
  /* Point the long-strip blit at this native-width band buffer (restored after). */
  r->rt565  = buf;
  r->rt_w   = nw;
  r->rt_h   = nh;
  wt->x_off = 0;
  memset(buf, (int)k_viewer_white_byte, px * sizeof(uint16_t));

  ra8_err_t rc = viewer_jof_seek(wt, i);
  if (rc == k_ra8_ok) {
    ra8_longstrip_render_stats_t st = {};
    rc                              = ra8_longstrip_render(&wt->strip, &st);
  }
  r->rt565 = r->fb; /* restore the headless default target */
  r->rt_w  = (uint32_t)k_ra8_viewer_fb_width;
  r->rt_h  = (uint32_t)k_ra8_viewer_fb_height;
  if (rc != k_ra8_ok) {
    free(buf);
    return rc;
  }
  *w   = nw;
  *h   = nh;
  *out = buf;
  return k_ra8_ok;
}

void viewer_size_jof_tiles(ra8_viewer_reader_t* r, uint32_t n)
{
  const uint32_t vw = r->jof.dctx.info.width;
  const uint32_t vh = r->jof.viewport_h;
  const uint32_t hh = r->jof.dctx.info.height;
  for (uint32_t i = 0U; i < n; ++i) {
    const uint32_t y0 = i * vh;
    uint32_t       bh = (hh > y0) ? (hh - y0) : 0U;
    if (bh > vh) {
      bh = vh;
    }
    r->tile_wpx[i] = vw;
    r->tile_hpx[i] = bh;
  }
}
