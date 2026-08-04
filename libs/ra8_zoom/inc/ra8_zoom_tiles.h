/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_zoom_tiles.h
 * @brief Bind a tiled gray8 atlas behind an ra8_tile_cache as a ra8_zoom source (#478).
 * @ingroup grp_ereader
 *
 * @details
 * The manga / comic half of the tap-to-zoom viewer, and the half that answers
 * the residency question. A full-resolution comic page does not fit in RAM, so
 * it lives as a tiled atlas (JOF, #231) paged through an ::ra8_tile_cache. This
 * adapter turns "give me source rectangle (x,y,w,h)" into "get exactly the tiles
 * that rectangle intersects, copy the overlap, release them" -- so the resident
 * set at any instant is the tiles under the viewport and never the page.
 *
 * Each tile is acquired and released around a single copy, so at most one cell
 * is pinned at a time and a cache smaller than the frame degrades to re-decoding
 * rather than to ::k_ra8_err_no_mem. Sizing the cache to the frame is the
 * caller's job and is what stops thrash (#338); ::ra8_zoom_tiles_prefetch warms
 * the lead edge of a pan out of whatever spare capacity the caller declares.
 *
 * @par Fail-closed on geometry
 * The tile cache carries no pixel format, so a colour atlas would otherwise be
 * read as gray8 and silently misrender (#339). Every fetched tile has its
 * decoded extent checked against the geometry declared at init, which is exactly
 * the mismatch a wrong bpp produces, and a mismatch is
 * ::k_ra8_err_invalid_size rather than a wrong picture.
 *
 * The TU compiles to nothing when `ra8_tile_cache.h` is not on the include path,
 * so an app that only magnifies `.rabook` figures does not drag `ra8_mem` in.
 *
 * @note Not thread-safe; inherits the single-threaded contract of ra8_tile_cache.
 * @see ra8_zoom.h       The viewport engine this feeds.
 * @see ra8_zoom_book.h  The `.rabook` figure source, for EPUB.
 *
 *
 * @since 0.1.0
 */

#pragma once

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_tile_cache.h"
#include "ra8_zoom.h"

/**
 * @struct ra8_zoom_tile_src_t
 * @brief A tiled gray8 image, its grid, and the cache that pages it.
 *
 * @details Everything the adapter needs to map a pixel rectangle onto tiles,
 *          held as plain integers rather than a parsed-format handle so the same
 *          binding serves a JOF atlas, a comic page reader, or a procedurally
 *          decoded fixture. The cache's decode-on-miss callback is what actually
 *          knows the container.
 *
 * @invariant `cache` is non-NULL and initialised.
 * @invariant `tile_cols == ceil(width / tile_w)` and likewise for rows.
 * @invariant Decoded tiles are gray8, one byte per pixel, tightly packed.
 *
 * @par Example:
 * @code
 * ra8_zoom_tile_src_t ts = {};
 * ra8_zoom_source_t   src = {};
 * (void)ra8_zoom_tile_src_init(&ts, &cache, k_image_id, 4096U, 3072U, 256U, 256U);
 * (void)ra8_zoom_tile_src_bind(&ts, &src);
 * @endcode
 *
 * @see ra8_zoom_tile_src_init
 * @since 0.1.0
 */
typedef struct {
  ra8_tile_cache_t* cache;     /**< Borrowed, initialised tile cache.       */
  uint32_t          image_id;  /**< Tile-cache key image id for this image. */
  uint32_t          width;     /**< Full-resolution image width, pixels.    */
  uint32_t          height;    /**< Full-resolution image height, pixels.   */
  uint16_t          tile_w;    /**< Tile width, pixels.                     */
  uint16_t          tile_h;    /**< Tile height, pixels.                    */
  uint16_t          tile_cols; /**< Derived tile columns.                   */
  uint16_t          tile_rows; /**< Derived tile rows.                      */
} ra8_zoom_tile_src_t;

/**
 * @brief Bind a tiled image and its cache, deriving the tile grid.
 *
 * @details Derives @c tile_cols / @c tile_rows rather than accepting them, so the
 *          grid can never disagree with the extent it is supposed to cover -- the
 *          disagreement that turns into an out-of-range tile fetch three layers
 *          down. Validates that the derived grid fits the 16-bit tile indices the
 *          cache key uses.
 *
 * @param[out] ts       Binding to populate.
 * @param[in]  cache    Initialised tile cache whose decoder serves this image.
 * @param[in]  image_id Tile-cache key image id (distinguishes co-resident images).
 * @param[in]  width    Full-resolution image width, pixels (`> 0`).
 * @param[in]  height   Full-resolution image height, pixels (`> 0`).
 * @param[in]  tile_w   Tile width, pixels (`> 0`).
 * @param[in]  tile_h   Tile height, pixels (`> 0`).
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok               @p ts is bound and its grid derived.
 * @retval k_ra8_err_null_ptr     @p ts or @p cache is NULL.
 * @retval k_ra8_err_invalid_arg  An extent or a tile dimension is zero.
 * @retval k_ra8_err_invalid_size The derived grid exceeds 65535 tiles on an axis.
 *
 * @pre  @p cache was initialised by ::ra8_tile_cache_init.
 * @pre  The cache's decoder produces gray8 tiles of `tile_w` x `tile_h`.
 * @post On k_ra8_ok the grid invariants of ::ra8_zoom_tile_src_t hold.
 * @post On any error @p ts is not left partially bound.
 *
 * @note Not thread-safe.
 * @see ra8_zoom_tile_src_bind
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_zoom_tile_src_init(ra8_zoom_tile_src_t* ts,
                                               ra8_tile_cache_t*    cache,
                                               uint32_t             image_id,
                                               uint32_t             width,
                                               uint32_t             height,
                                               uint16_t             tile_w,
                                               uint16_t             tile_h);

/**
 * @brief Present a bound tiled image to the zoom engine as a ::ra8_zoom_source_t.
 *
 * @param[in]  ts  Binding populated by ::ra8_zoom_tile_src_init.
 * @param[out] out Source descriptor to fill.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok              @p out is ready to hand to ::ra8_zoom_view_open.
 * @retval k_ra8_err_null_ptr    @p ts, @p out, or `ts->cache` is NULL.
 * @retval k_ra8_err_invalid_arg The bound extent is unusable (zero or too large).
 *
 * @pre  @p ts was populated by ::ra8_zoom_tile_src_init.
 * @pre  @p ts outlives every view bound to @p out.
 * @post On k_ra8_ok `out->ctx == ts` and the extent equals the image's.
 * @post @p ts is not modified.
 *
 * @note Not thread-safe.
 * @see ra8_zoom_view_open
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_zoom_tile_src_bind(ra8_zoom_tile_src_t* ts, ra8_zoom_source_t* out);

/**
 * @brief ::ra8_zoom_read_fn over a tiled atlas paged by an ra8_tile_cache.
 *
 * @details Bound by ::ra8_zoom_tile_src_bind; not normally called directly.
 *          Walks only the tiles the rectangle intersects, copying each overlap
 *          and releasing the cell before moving on, so the pin count never
 *          exceeds one and the resident set is bounded by the cache, not by the
 *          rectangle.
 *
 * @param[in]  ctx        The ::ra8_zoom_tile_src_t binding.
 * @param[in]  x          Rectangle left edge, source pixels.
 * @param[in]  y          Rectangle top edge, source pixels.
 * @param[in]  w          Rectangle width, source pixels (`> 0`).
 * @param[in]  h          Rectangle height, source pixels (`> 0`).
 * @param[out] out        gray8 destination of at least `out_stride * h` bytes.
 * @param[in]  out_stride Bytes between successive output rows (`>= w`).
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok               The rectangle was assembled from its tiles.
 * @retval k_ra8_err_null_ptr     @p ctx or @p out is NULL.
 * @retval k_ra8_err_invalid_arg  Zero extent, or `out_stride < w`.
 * @retval k_ra8_err_out_of_range The rectangle leaves the image.
 * @retval k_ra8_err_invalid_size A fetched tile is not the declared geometry.
 * @retval k_ra8_err_*            Propagated verbatim from the tile cache.
 *
 * @pre  @p ctx is a bound ::ra8_zoom_tile_src_t.
 * @pre  The rectangle lies inside the bound image.
 * @post On k_ra8_ok every output pixel is the gray8 value of its source pixel.
 * @post No cell is left pinned on any return path.
 *
 * @note Not thread-safe.
 * @see ra8_tile_cache_get
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_zoom_tile_read(void*    ctx,
                                           uint32_t x,
                                           uint32_t y,
                                           uint32_t w,
                                           uint32_t h,
                                           uint8_t* out,
                                           uint32_t out_stride);

/**
 * @brief Warm the tiles one pan step beyond what a view currently shows.
 *
 * @details Joins the two halves this library exists to keep in agreement: the
 *          view reports the source pixels it will read (::ra8_zoom_view_window),
 *          ::ra8_tile_rect_of_pixels turns that into the tile rectangle, and the
 *          cache warms the lead edge of @p dir out of @p max_tiles of spare
 *          capacity. Best-effort by construction -- read-ahead that could not
 *          complete never fails a pan.
 *
 * @param[in,out] ts         Bound tiled source.
 * @param[in]     v          The open view whose visible window drives the warm.
 * @param[in]     dir        Direction of travel; ::k_ra8_zoom_pan_none warms nothing.
 * @param[in]     max_tiles  Residency budget: warm at most this many tiles.
 * @param[out]    out_warmed Tiles warmed (may be NULL).
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok            The sweep ran (possibly warming nothing).
 * @retval k_ra8_err_null_ptr  @p ts, @p v, or `ts->cache` is NULL.
 * @retval k_ra8_err_*         Propagated from ::ra8_zoom_view_window or the cache.
 *
 * @pre  @p v is open and bound to the same image as @p ts.
 * @pre  @p max_tiles is the caller's spare capacity, not the cache size.
 * @post No on-screen tile is evicted by this call when @p max_tiles respects
 *       the cache's spare capacity.
 * @post `*out_warmed` (when given) is at most @p max_tiles.
 *
 * @note Not thread-safe. Single-threaded read-ahead only.
 *
 * @par Example:
 * @code
 * uint16_t warmed = 0U;
 * (void)ra8_zoom_tiles_prefetch(&ts, &view, k_ra8_zoom_pan_right, spare, &warmed);
 * @endcode
 *
 * @see ra8_tile_cache_prefetch_pan
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_zoom_tiles_prefetch(ra8_zoom_tile_src_t*   ts,
                                                const ra8_zoom_view_t* v,
                                                ra8_zoom_pan_t         dir,
                                                uint16_t               max_tiles,
                                                uint16_t*              out_warmed);
