/**
 * @file ra8_viewer_comic.c
 * @brief Caller-workspace CBZ, CBR, and CBT reader engine.
 * @details Extracts one bounded encoded page through `ra8_comic`, probes and
 * decodes it through the caller-bound stb arena, and renders into either the
 * fixed framebuffer or a caller-owned scroll tile. The archive, page index,
 * names, encoded bytes, decode arena, and output pixels never use the heap.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_comic.h"
#include "ra8_decomp_limits.h"
#include "ra8_err.h"
#include "ra8_gfx.h"
#include "ra8_img_arena.h"
#include "ra8_reflow_image.h"
#include "ra8_viewer_reader_internal.h"

/** @brief Destination rectangle for aspect-preserving page placement. */
typedef struct {
  int32_t x;      /**< Left edge in target pixels. */
  int32_t y;      /**< Top edge in target pixels.  */
  int32_t width;  /**< Render width in pixels.     */
  int32_t height; /**< Render height in pixels.    */
} viewer_fit_box_t;

/**
 * @brief Fit an image into a target while preserving aspect ratio.
 * @details Tries the full target width, falls back to a height fit, and centres
 * the result on both axes with every output edge clamped to at least one pixel.
 * @param[in] source_width Positive source width.
 * @param[in] source_height Positive source height.
 * @param[in] target_width Positive target width.
 * @param[in] target_height Positive target height.
 * @param[out] box Resulting centred rectangle.
 * @pre @p box is writable.
 * @pre Every dimension is positive and fits int32_t.
 * @post @p box lies inside the target.
 * @post @p box preserves the source aspect to integer precision.
 * @note Pure apart from @p box.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_fit_centered(int32_t           source_width,
                                               int32_t           source_height,
                                               int32_t           target_width,
                                               int32_t           target_height,
                                               viewer_fit_box_t* box)
{
  const int64_t tw     = (int64_t)target_width;
  const int64_t th     = (int64_t)target_height;
  int64_t       width  = tw;
  int64_t       height = ((int64_t)source_height * tw) / (int64_t)source_width;
  if (height > th) {
    height = th;
    width  = ((int64_t)source_width * th) / (int64_t)source_height;
  }
  if (width < 1) {
    width = 1;
  }
  if (height < 1) {
    height = 1;
  }
  *box = (viewer_fit_box_t){.x      = (int32_t)((tw - width) / 2),
                            .y      = (int32_t)((th - height) / 2),
                            .width  = (int32_t)width,
                            .height = (int32_t)height};
}

/**
 * @brief Cap a page to the graphics engine edge limit.
 * @details Applies width and height caps in sequence while preserving aspect.
 * @param[in] native_width Positive source width.
 * @param[in] native_height Positive source height.
 * @param[out] render_width Capped positive width.
 * @param[out] render_height Capped positive height.
 * @pre Output pointers are writable.
 * @pre Native dimensions are positive.
 * @post Both outputs are within ::k_ra8_gfx_max_dim.
 * @post The output aspect follows the source to integer precision.
 * @note Pure apart from the outputs.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_cap_render(uint32_t  native_width,
                                             uint32_t  native_height,
                                             uint32_t* render_width,
                                             uint32_t* render_height)
{
  const uint32_t edge   = (uint32_t)k_ra8_gfx_max_dim;
  uint32_t       width  = native_width;
  uint32_t       height = native_height;
  if (width > edge) {
    height = (uint32_t)(((uint64_t)height * (uint64_t)edge) / (uint64_t)width);
    width  = edge;
  }
  if (height > edge) {
    width  = (uint32_t)(((uint64_t)width * (uint64_t)edge) / (uint64_t)height);
    height = edge;
  }
  *render_width  = (width == 0U) ? 1U : width;
  *render_height = (height == 0U) ? 1U : height;
}

/**
 * @brief Extract one bounded encoded page into its resident slice.
 * @details Checks the shared output/ratio policy and the viewer's explicit page
 * capacity before asking the container backend to copy or decompress bytes.
 * @param[in,out] reader Open comic reader.
 * @param[in] page Page index.
 * @param[out] out_bytes Exact extracted byte count.
 * @return Extraction status.
 * @retval k_ra8_ok The resident page slice is populated.
 * @retval k_ra8_err_invalid_size The page exceeds the explicit resident cap.
 * @pre @p reader owns an open comic and @p page is in range.
 * @pre @p out_bytes is writable.
 * @post Success publishes no more than `reader->comic.page_cap` bytes.
 * @post Failure performs no unbounded allocation.
 * @note Not thread-safe; reuses the resident page slice.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_read_page(ra8_viewer_reader_t* reader, uint32_t page, size_t* out_bytes)
{
  uint64_t  declared = 0U;
  ra8_err_t error =
    ra8_comic_page_info(&reader->comic.archive, page, nullptr, 0U, nullptr, &declared, nullptr);
  if (error == k_ra8_ok) {
    error = ra8_decomp_check_declared(&reader->limits, reader->file.size, declared);
  }
  if ((error == k_ra8_ok) && (declared > (uint64_t)reader->comic.page_cap)) {
    error = k_ra8_err_invalid_size;
  }
  if (error != k_ra8_ok) {
    return error;
  }
  *out_bytes = 0U;
  error      = ra8_comic_page_read(&reader->comic.archive,
                                   page,
                                   reader->comic.page,
                                   reader->comic.page_cap,
                                   out_bytes);
  if ((error == k_ra8_ok) && ((uint64_t)*out_bytes != declared)) {
    return k_ra8_err_invalid_size;
  }
  return error;
}

/**
 * @brief Probe one page and publish its bounded render dimensions.
 * @details Extracts only the encoded page, binds the caller arena around the
 * image header probe, validates positive geometry, and applies the gfx cap.
 * @param[in,out] reader Open comic reader.
 * @param[in] page Page index.
 * @return Probe status.
 * @retval k_ra8_ok Dimension arrays are populated for @p page.
 * @retval k_ra8_err_invalid_size The decoded geometry is not positive.
 * @pre @p reader has bound dimension arrays and comic storage.
 * @pre @p page is below the comic page count.
 * @post Success publishes positive bounded dimensions.
 * @post The decode arena is unbound on every path.
 * @note Not thread-safe; drives the shared image arena binding.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_probe_page(ra8_viewer_reader_t* reader, uint32_t page)
{
  size_t    encoded = 0U;
  ra8_err_t error   = internal_read_page(reader, page, &encoded);
  int32_t   width   = 0;
  int32_t   height  = 0;
  if (error == k_ra8_ok) {
    ra8_img_arena_t arena = {.base = reader->comic.arena, .cap = reader->comic.arena_cap};
    ra8_img_arena_bind(&arena);
    error = ra8_img_probe_size(reader->comic.page, encoded, &width, &height);
    ra8_img_arena_unbind();
  }
  if ((error == k_ra8_ok) && ((width <= 0) || (height <= 0))) {
    error = k_ra8_err_invalid_size;
  }
  if (error == k_ra8_ok) {
    internal_cap_render((uint32_t)width,
                        (uint32_t)height,
                        &reader->tile_wpx[page],
                        &reader->tile_hpx[page]);
  }
  return error;
}

/**
 * @brief Decode one page into the currently selected graphics target.
 * @details Re-extracts the encoded page, clears the target white, then invokes
 * the zero-heap image decoder over the bound arena and requested fit rectangle.
 * @param[in,out] reader Open comic reader.
 * @param[in] page Page index.
 * @param[in] fit Aspect-preserving destination box.
 * @return Decode or graphics status.
 * @retval k_ra8_ok The requested page is present in the graphics target.
 * @retval k_ra8_err_invalid_size The encoded page exceeds its bounded slice.
 * @pre The graphics target was initialized for the supplied dimensions.
 * @pre @p fit lies inside the target.
 * @post Success leaves decoded RGB565 pixels in the current target.
 * @post The image arena is unbound on every path.
 * @note Not thread-safe; drives global gfx and arena state.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_decode(ra8_viewer_reader_t* reader, uint32_t page, const viewer_fit_box_t* fit)
{
  size_t    encoded = 0U;
  ra8_err_t error   = internal_read_page(reader, page, &encoded);
  if (error == k_ra8_ok) {
    error = ra8_gfx_clear((uint32_t)k_viewer_background);
  }
  if (error == k_ra8_ok) {
    ra8_img_arena_t arena = {.base = reader->comic.arena, .cap = reader->comic.arena_cap};
    error                 = ra8_img_decode_blit(&arena,
                                                reader->comic.page,
                                                encoded,
                                                fit->x,
                                                fit->y,
                                                fit->width,
                                                fit->height,
                                                nullptr,
                                                nullptr);
  }
  return error;
}

size_t priv_viewer_comic_read(void* ctx, uint64_t offset, void* buffer, size_t length)
{
  size_t got = 0U;
  return (priv_viewer_pread(ctx, offset, (uint8_t*)buffer, length, &got) == k_ra8_ok) ? got : 0U;
}

ra8_err_t priv_viewer_open_comic(ra8_viewer_reader_t* reader)
{
  return ra8_comic_open(&reader->comic.archive,
                        priv_viewer_comic_read,
                        &reader->file,
                        reader->file.size,
                        reader->comic.pages,
                        (uint32_t)k_viewer_comic_page_cap,
                        reader->comic.names,
                        (uint32_t)k_viewer_comic_name_bytes);
}

ra8_err_t priv_viewer_size_comic_tiles(ra8_viewer_reader_t* reader)
{
  for (uint32_t page = 0U; page < reader->tile_n; ++page) {
    const ra8_err_t error = internal_probe_page(reader, page);
    if (error != k_ra8_ok) {
      return error;
    }
  }
  return k_ra8_ok;
}

ra8_err_t priv_viewer_render_comic(ra8_viewer_reader_t* reader, uint32_t page)
{
  const uint32_t   source_width  = reader->tile_wpx[page];
  const uint32_t   source_height = reader->tile_hpx[page];
  viewer_fit_box_t fit           = {};
  internal_fit_centered((int32_t)source_width,
                        (int32_t)source_height,
                        (int32_t)k_ra8_viewer_fb_width,
                        (int32_t)k_ra8_viewer_fb_height,
                        &fit);
  const ra8_err_t error = ra8_gfx_init(reader->fb,
                                       k_ra8_viewer_fb_width,
                                       k_ra8_viewer_fb_height,
                                       k_ra8_gfx_format_rgb565);
  return (error == k_ra8_ok) ? internal_decode(reader, page, &fit) : error;
}

ra8_err_t priv_viewer_tile_comic(ra8_viewer_reader_t* reader,
                                 uint32_t             index,
                                 uint16_t*            pixels,
                                 size_t               pixel_bytes,
                                 uint32_t*            width,
                                 uint32_t*            height)
{
  const uint32_t render_width  = reader->tile_wpx[index];
  const uint32_t render_height = reader->tile_hpx[index];
  const uint64_t required =
    (uint64_t)render_width * (uint64_t)render_height * (uint64_t)sizeof(uint16_t);
  if ((required > (uint64_t)pixel_bytes) || (render_width > UINT16_MAX) ||
      (render_height > UINT16_MAX)) {
    return k_ra8_err_invalid_size;
  }
  ra8_err_t error =
    ra8_gfx_init(pixels, (uint16_t)render_width, (uint16_t)render_height, k_ra8_gfx_format_rgb565);
  if (error == k_ra8_ok) {
    const viewer_fit_box_t fit = {.x      = 0,
                                  .y      = 0,
                                  .width  = (int32_t)render_width,
                                  .height = (int32_t)render_height};
    error                      = internal_decode(reader, index, &fit);
  }
  const ra8_err_t restored = ra8_gfx_init(reader->fb,
                                          k_ra8_viewer_fb_width,
                                          k_ra8_viewer_fb_height,
                                          k_ra8_gfx_format_rgb565);
  if ((error == k_ra8_ok) && (restored != k_ra8_ok)) {
    error = restored;
  }
  if (error == k_ra8_ok) {
    *width  = render_width;
    *height = render_height;
  }
  return error;
}
