/**
 * @file ra8_viewer_jof.c
 * @brief Streamed JOF long-strip rendering over a one-band caller cache.
 * @details Binds the portable JOF and long-strip engines to raw positional
 * reads, one caller-owned decoded band, and caller-selected RGB565 targets.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>
#include <string.h>

#include "jof.h"
#include "longstrip.h"
#include "ra8_attributes.h"
#include "ra8_decomp_limits.h"
#include "ra8_err.h"
#include "ra8_tile_cache.h"
#include "ra8_viewer_reader.h"
#include "ra8_viewer_reader_internal.h"

/** @brief Supported source pixel widths and channel indices. */
typedef enum : uint8_t {
  k_viewer_bpp_gray   = 1U, /**< 8-bit grayscale.               */
  k_viewer_bpp_rgb565 = 2U, /**< Little-endian RGB565.          */
  k_viewer_px_r       = 0U, /**< Red, gray, or RGB565 low byte. */
  k_viewer_px_g       = 1U, /**< Green or RGB565 high byte.     */
  k_viewer_px_b       = 2U, /**< Blue byte.                     */
} viewer_pixel_t;

/**
 * @brief Convert one source pixel to native RGB565.
 * @details Handles grayscale, little-endian RGB565, RGB, and RGBA-with-alpha-dropped.
 * @param[in] source Source pixel bytes.
 * @param[in] bpp Bytes per source pixel.
 * @return Packed RGB565 value.
 * @retval 0 A black source pixel.
 * @pre @p source spans @p bpp readable bytes.
 * @pre @p bpp is one of the JOF-supported pixel widths.
 * @post No state is mutated.
 * @post Alpha, when present, is ignored.
 * @note Pure and thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static uint16_t internal_band_pixel(const uint8_t* source, uint8_t bpp)
{
  if (bpp == (uint8_t)k_viewer_bpp_gray) {
    return ra8_viewer_pack565(source[k_viewer_px_r], source[k_viewer_px_r], source[k_viewer_px_r]);
  }
  if (bpp == (uint8_t)k_viewer_bpp_rgb565) {
    return ra8_viewer_pack565_le_pair(source[k_viewer_px_r], source[k_viewer_px_g]);
  }
  return ra8_viewer_pack565(source[k_viewer_px_r], source[k_viewer_px_g], source[k_viewer_px_b]);
}

/**
 * @brief Composite one decoded band window into the active RGB565 target.
 * @details Converts pixels row-wise while clipping to the caller-selected target.
 * @param[in,out] ctx Bound reader.
 * @param[in] pixels Decoded source pixels.
 * @param[in] source_width Source window width.
 * @param[in] source_height Source window height.
 * @param[in] bpp Source bytes per pixel.
 * @param[in] destination_x Target-relative x coordinate.
 * @param[in] destination_y Target-relative y coordinate.
 * @return Composite status.
 * @retval k_ra8_ok Pixels were converted; clipping is not an error.
 * @pre @p ctx is a bound reader with an active target.
 * @pre @p pixels spans the declared source window.
 * @post Every in-bounds target pixel is written once.
 * @post Out-of-bounds source pixels are ignored.
 * @note Not thread-safe; writes reader target state.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_jof_blit(void*          ctx,
                                                const uint8_t* pixels,
                                                uint16_t       source_width,
                                                uint16_t       source_height,
                                                uint8_t        bpp,
                                                int32_t        destination_x,
                                                int32_t        destination_y)
{
  ra8_viewer_reader_t* reader        = (ra8_viewer_reader_t*)ctx;
  const int32_t        target_width  = (int32_t)reader->rt_w;
  const int32_t        target_height = (int32_t)reader->rt_h;
  for (int32_t row = 0; row < (int32_t)source_height; ++row) {
    const int32_t y = destination_y + row;
    if ((y < 0) || (y >= target_height)) {
      continue;
    }
    for (int32_t column = 0; column < (int32_t)source_width; ++column) {
      const int32_t x = destination_x + column + reader->jof.x_off;
      if ((x < 0) || (x >= target_width)) {
        continue;
      }
      const size_t source_index =
        (((size_t)row * (size_t)source_width) + (size_t)column) * (size_t)bpp;
      const size_t target_index   = ((size_t)y * (size_t)target_width) + (size_t)x;
      reader->rt565[target_index] = internal_band_pixel(&pixels[source_index], bpp);
    }
  }
  return k_ra8_ok;
}

/**
 * @brief Wire the one-cell cache and long-strip engine.
 * @details Binds caller slices to the cache and raw-descriptor pread seam.
 * @param[in,out] reader Bound reader with parsed JOF information.
 * @param[in] band_bytes Exact decoded band capacity.
 * @return Cache or long-strip open status.
 * @retval k_ra8_ok Both engines are ready.
 * @pre @p reader owns an open descriptor.
 * @pre @p band_bytes fits the bound cache cell.
 * @post Success publishes an open strip over one cache cell.
 * @post Failure allocates and maps nothing.
 * @note Not thread-safe; initialises shared engine state.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_wire(ra8_viewer_reader_t* reader, size_t band_bytes)
{
  viewer_jof_t* jof                       = &reader->jof;
  jof->dctx.scratch                       = jof->scratch;
  jof->dctx.scratch_cap                   = (uint32_t)jof->scratch_cap;
  const ra8_tile_cache_cfg_t cache_config = {
    .cell_mem     = jof->cells,
    .cell_bytes   = (uint32_t)band_bytes,
    .cell_count   = (uint32_t)k_viewer_jof_cells,
    .meta         = jof->meta,
    .keys         = jof->keys,
    .dims         = jof->dims,
    .buckets      = jof->buckets,
    .bucket_count = (uint32_t)k_viewer_jof_buckets,
    .decode       = longstrip_tile_decode,
    .decode_ctx   = &jof->dctx,
  };
  ra8_err_t error = ra8_tile_cache_init(&jof->cache, &cache_config);
  if (error != k_ra8_ok) {
    return error;
  }
  jof->viewport_h                    = (uint32_t)k_ra8_viewer_fb_height;
  jof->x_off                         = 0;
  const longstrip_cfg_t strip_config = {
    .pread      = priv_viewer_pread,
    .pread_ctx  = &reader->file,
    .atlas_size = reader->file.size,
    .cache      = &jof->cache,
    .image_id   = 1U,
    .viewport_w = jof->dctx.info.width,
    .viewport_h = (uint16_t)k_ra8_viewer_fb_height,
    .blit       = internal_jof_blit,
    .blit_ctx   = reader,
  };
  return longstrip_open(&jof->strip, &strip_config);
}

/**
 * @brief Seek to a viewport, including the short final page.
 * @details Resizes the final viewport before applying its absolute scroll delta.
 * @param[in,out] jof Open JOF state.
 * @param[in] page Viewport index.
 * @return Scroll status.
 * @retval k_ra8_ok The viewport begins at @p page.
 * @retval k_ra8_err_out_of_range No canvas rows remain.
 * @pre @p jof is open.
 * @pre @p page is a candidate unsigned index.
 * @post Success sets the exact final-page height.
 * @post Failure performs no composite.
 * @note Not thread-safe; mutates strip scroll state.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_seek(viewer_jof_t* jof, uint32_t page)
{
  const uint32_t top       = page * jof->viewport_h;
  const uint32_t canvas    = jof->dctx.info.height;
  const uint32_t remaining = (canvas > top) ? (canvas - top) : 0U;
  const uint32_t content   = (remaining < jof->viewport_h) ? remaining : jof->viewport_h;
  if (content == 0U) {
    return k_ra8_err_out_of_range;
  }
  const ra8_err_t error =
    longstrip_set_viewport(&jof->strip, jof->dctx.info.width, (uint16_t)content);
  if (error != k_ra8_ok) {
    return error;
  }
  return longstrip_scroll_by(&jof->strip, (int32_t)top - jof->strip.scroll_y);
}

ra8_err_t priv_viewer_open_jof(ra8_viewer_reader_t* reader)
{
  viewer_jof_t* jof   = &reader->jof;
  jof->dctx.pread     = priv_viewer_pread;
  jof->dctx.pread_ctx = &reader->file;
  ra8_err_t error = jof_parse(priv_viewer_pread, &reader->file, reader->file.size, &jof->dctx.info);
  const uint64_t band = (uint64_t)jof->dctx.info.tile_w * (uint64_t)jof->dctx.info.tile_h *
                        (uint64_t)jof->dctx.info.bpp;
  if ((error == k_ra8_ok) && (band == 0U)) {
    error = k_ra8_err_invalid_size;
  }
  const ra8_decomp_limits_t limits = ra8_decomp_limits_default();
  if (error == k_ra8_ok) {
    error = ra8_decomp_check_declared(&limits, reader->file.size, band);
  }
  const size_t scratch_required = (jof->dctx.info.codec == (uint8_t)k_jof_codec_raw)
                                    ? 1U
                                    : (size_t)jof_stored_bound((uint32_t)band);
  if ((error == k_ra8_ok) &&
      (((size_t)band > jof->cell_cap) || (scratch_required > jof->scratch_cap))) {
    error = k_ra8_err_invalid_size;
  }
  if (error != k_ra8_ok) {
    return error;
  }
  return internal_wire(reader, (size_t)band);
}

ra8_err_t priv_viewer_render_jof(ra8_viewer_reader_t* reader, uint32_t page)
{
  viewer_jof_t* jof         = &reader->jof;
  reader->rt565             = reader->fb;
  reader->rt_w              = (uint32_t)k_ra8_viewer_fb_width;
  reader->rt_h              = (uint32_t)k_ra8_viewer_fb_height;
  const int32_t strip_width = (int32_t)jof->dctx.info.width;
  jof->x_off =
    ((int32_t)reader->rt_w > strip_width) ? (((int32_t)reader->rt_w - strip_width) / 2) : 0;
  memset(reader->fb,
         (int)k_viewer_white_byte,
         (size_t)reader->rt_w * (size_t)reader->rt_h * sizeof(*reader->fb));
  ra8_err_t error = internal_seek(jof, page);
  if (error == k_ra8_ok) {
    longstrip_render_stats_t stats = {};
    error                          = longstrip_render(&jof->strip, &stats);
  }
  return error;
}

ra8_err_t priv_viewer_tile_jof(ra8_viewer_reader_t* reader,
                               uint32_t             index,
                               uint16_t*            pixels,
                               size_t               pixel_bytes,
                               uint32_t*            width,
                               uint32_t*            height)
{
  viewer_jof_t*  jof           = &reader->jof;
  const uint32_t native_width  = reader->tile_wpx[index];
  const uint32_t native_height = reader->tile_hpx[index];
  const size_t   required      = (size_t)native_width * (size_t)native_height * sizeof(*pixels);
  if ((native_width == 0U) || (native_height == 0U) || (pixel_bytes < required)) {
    return k_ra8_err_invalid_size;
  }
  reader->rt565 = pixels;
  reader->rt_w  = native_width;
  reader->rt_h  = native_height;
  jof->x_off    = 0;
  memset(pixels, (int)k_viewer_white_byte, required);
  ra8_err_t error = internal_seek(jof, index);
  if (error == k_ra8_ok) {
    longstrip_render_stats_t stats = {};
    error                          = longstrip_render(&jof->strip, &stats);
  }
  reader->rt565 = reader->fb;
  reader->rt_w  = (uint32_t)k_ra8_viewer_fb_width;
  reader->rt_h  = (uint32_t)k_ra8_viewer_fb_height;
  if (error == k_ra8_ok) {
    *width  = native_width;
    *height = native_height;
  }
  return error;
}

void priv_viewer_size_jof_tiles(ra8_viewer_reader_t* reader, uint32_t count)
{
  const uint32_t width    = reader->jof.dctx.info.width;
  const uint32_t viewport = reader->jof.viewport_h;
  const uint32_t canvas   = reader->jof.dctx.info.height;
  for (uint32_t index = 0U; index < count; ++index) {
    const uint32_t top    = index * viewport;
    uint32_t       height = (canvas > top) ? (canvas - top) : 0U;
    if (height > viewport) {
      height = viewport;
    }
    reader->tile_wpx[index] = width;
    reader->tile_hpx[index] = height;
  }
}
