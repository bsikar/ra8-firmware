/**
 * @file ra8_zoom_tiles.c
 * @brief Tiled-atlas source adapter for the tap-to-zoom viewer (#478).
 *
 * @details Implements ra8_zoom_tiles.h: pixel rectangle -> covering tiles ->
 *          per-tile acquire, copy the overlap, release. The acquire/release pair
 *          brackets exactly one copy, so at most one cell is ever pinned and the
 *          resident set is whatever the cache holds rather than whatever the
 *          rectangle spans -- which is what makes "only visible tiles are
 *          resident" a property of the code rather than a hope.
 *
 *          The whole TU is guarded on `ra8_tile_cache.h` being reachable, the
 *          same pattern `ra8_comic_tiles.c` uses.
 *
 *
 * [Ring 4 / Domain] {World: NS}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#ifdef __has_include
#if __has_include("ra8_tile_cache.h")
/**
 * @def RA8_ZOOM_HAVE_TILES
 * @brief Defined when `ra8_mem` is on the include path, enabling this adapter.
 *
 * @details A build-configuration flag, not a constant: it carries no value and
 *          is only ever tested with `#ifdef`. When it is absent the whole
 *          translation unit compiles to nothing, so an app that magnifies only
 *          `.rabook` figures never has to link the tile cache to satisfy a
 *          symbol it does not call. The same pattern guards `ra8_comic_tiles.c`.
 *
 * @note Set by this file alone; never define it externally to force the
 *       adapter in -- the include would then fail instead.
 * @warning Undefining it silently removes ::ra8_zoom_tile_src_init and friends
 *          from the link, which presents as an undefined-symbol error at the
 *          call site rather than here.
 *
 * @par Example:
 * @code
 * #ifdef RA8_ZOOM_HAVE_TILES
 * // ... the adapter's whole implementation ...
 * #endif
 * @endcode
 *
 * @see ra8_zoom_tiles.h
 * @since 0.1.0
 */
#define RA8_ZOOM_HAVE_TILES
#endif
#endif

#ifdef RA8_ZOOM_HAVE_TILES

#include "ra8_zoom_tiles.h"

#include <stdint.h>
#include <string.h>

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_tile_cache.h"
#include "ra8_ui.h"
#include "ra8_zoom.h"

/** @brief Component tag for `RA8_CHECK_*` log lines. */
static const char* const s_tag = "ra8_zoom_tiles";

/* ra8_zoom_tiles_prefetch forwards a ra8_zoom_pan_t straight into the cache's
 * ra8_tile_pan_dir_t rather than switching over it. That is only sound while the
 * two enumerations agree value for value, so the agreement is pinned here rather
 * than asserted in prose -- a renumbering of either enum fails the build instead
 * of quietly warming the wrong edge. */
static_assert((uint8_t)k_ra8_zoom_pan_none == (uint8_t)k_ra8_tile_pan_none,
              "ra8_zoom_pan_t and ra8_tile_pan_dir_t must agree: none");
static_assert((uint8_t)k_ra8_zoom_pan_left == (uint8_t)k_ra8_tile_pan_left,
              "ra8_zoom_pan_t and ra8_tile_pan_dir_t must agree: left");
static_assert((uint8_t)k_ra8_zoom_pan_right == (uint8_t)k_ra8_tile_pan_right,
              "ra8_zoom_pan_t and ra8_tile_pan_dir_t must agree: right");
static_assert((uint8_t)k_ra8_zoom_pan_up == (uint8_t)k_ra8_tile_pan_up,
              "ra8_zoom_pan_t and ra8_tile_pan_dir_t must agree: up");
static_assert((uint8_t)k_ra8_zoom_pan_down == (uint8_t)k_ra8_tile_pan_down,
              "ra8_zoom_pan_t and ra8_tile_pan_dir_t must agree: down");

/**
 * @enum zoom_tiles_key_t
 * @brief Fixed fields of the tile-cache key this adapter builds.
 *
 * @details The cache key carries a zoom / mip dimension. This viewer magnifies
 *          the *native* pixels rather than paging a pre-scaled pyramid -- that is
 *          the whole point of retaining full resolution -- so every key it forms
 *          names the native level. Naming the constant records that as a decision
 *          instead of leaving a bare zero in the key.
 *
 * @invariant k_zoom_tiles_native_level == 0, the tile cache's native level.
 *
 * @par Example:
 * @code
 * key.zoom = (uint16_t)k_zoom_tiles_native_level;
 * @endcode
 *
 * @see ra8_zoom_tile_read
 * @since 0.1.0
 */
typedef enum : uint16_t {
  k_zoom_tiles_native_level = 0U, /**< Tile-cache key zoom level: native pixels. */
} zoom_tiles_key_t;

/**
 * @struct zoom_tile_span_t
 * @brief The requested pixel rectangle, carried through the per-tile copy.
 * @details Grouped so the copy helper takes one argument instead of four, which
 *          keeps it inside the project's parameter-count and function-size bars.
 * @invariant `w > 0` and `h > 0`.
 * @par Example:
 * @code
 * const zoom_tile_span_t span = { .x = x, .y = y, .w = w, .h = h };
 * @endcode
 * @see ra8_zoom_tile_read
 * @since 0.1.0
 */
typedef struct {
  uint32_t x; /**< Rectangle left edge, source pixels. */
  uint32_t y; /**< Rectangle top edge, source pixels.  */
  uint32_t w; /**< Rectangle width, source pixels.     */
  uint32_t h; /**< Rectangle height, source pixels.    */
} zoom_tile_span_t;

/**
 * @brief Copy one tile's overlap with the requested rectangle into the output.
 *
 * @details Both rectangles are half-open, so the overlap is the usual
 *          `[max(starts), min(ends))` on each axis; an empty overlap copies
 *          nothing (the caller only ever passes intersecting tiles, so it is a
 *          guard, not a path). Row addresses are computed once per row from the
 *          tile origin and the output stride.
 *
 * @param[in]  tile  The pinned tile and its decoded extent.
 * @param[in]  org_x Tile origin column in source pixels.
 * @param[in]  org_y Tile origin row in source pixels.
 * @param[in]  span  The requested rectangle.
 * @param[out] out   gray8 destination laid out at @p stride.
 * @param[in]  stride Bytes between successive output rows.
 *
 * @return Nothing.
 *
 * @pre  @p tile is pinned and holds `tile->width * tile->height` gray8 bytes.
 * @pre  @p out addresses at least `stride * span->h` writable bytes.
 * @post Every output pixel covered by this tile holds its source value.
 * @post No output byte outside the overlap is written.
 *
 * @note Not thread-safe (writes @p out).
 * @see ra8_zoom_tile_read
 * @since 0.1.0
 */
RA8_INTERNAL
static void zoom_tile_copy(const ra8_tile_t*       tile,
                           uint32_t                org_x,
                           uint32_t                org_y,
                           const zoom_tile_span_t* span,
                           uint8_t*                out,
                           uint32_t                stride)
{
  const uint32_t x0     = (span->x > org_x) ? span->x : org_x;
  const uint32_t y0     = (span->y > org_y) ? span->y : org_y;
  const uint32_t tx_end = org_x + (uint32_t)tile->width;
  const uint32_t ty_end = org_y + (uint32_t)tile->height;
  const uint32_t x1     = ((span->x + span->w) < tx_end) ? (span->x + span->w) : tx_end;
  const uint32_t y1     = ((span->y + span->h) < ty_end) ? (span->y + span->h) : ty_end;
  for (uint32_t sy = y0; sy < y1; ++sy) {
    const uint8_t* srow = &tile->pixels[((sy - org_y) * (uint32_t)tile->width) + (x0 - org_x)];
    uint8_t*       drow = &out[((sy - span->y) * stride) + (x0 - span->x)];
    (void)memcpy(drow, srow, (size_t)(x1 - x0));
  }
}

/**
 * @brief Fetch one tile, copy its overlap, and release it.
 *
 * @details The acquire/release bracket. The fetched tile's decoded extent is
 *          cross-checked against the geometry declared at bind time before a
 *          single byte is read: a decoder that produced RGB tiles, or a
 *          different tile edge, shows up here as a size mismatch rather than as
 *          a wrong picture (#339). The cell is released on every return path.
 *
 * @param[in,out] ts   Bound tiled source.
 * @param[in]     tx   Tile column index.
 * @param[in]     ty   Tile row index.
 * @param[in]     span The requested rectangle.
 * @param[out]    out  gray8 destination laid out at @p stride.
 * @param[in]     stride Bytes between successive output rows.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok               The tile's overlap was copied.
 * @retval k_ra8_err_invalid_size The tile decoded to an unexpected extent.
 * @retval k_ra8_err_*            Propagated from ::ra8_tile_cache_get.
 *
 * @pre  @p ts is bound and `(tx, ty)` is inside its grid.
 * @pre  The tile intersects @p span.
 * @post No cell is pinned when this returns, on any path.
 * @post On k_ra8_ok the overlap is present in @p out.
 *
 * @note Not thread-safe.
 * @see zoom_tile_copy
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t zoom_tile_fetch(ra8_zoom_tile_src_t*    ts,
                                 uint16_t                tx,
                                 uint16_t                ty,
                                 const zoom_tile_span_t* span,
                                 uint8_t*                out,
                                 uint32_t                stride)
{
  const ra8_tile_key_t key = {
    .image_id = ts->image_id,
    .tile_x   = tx,
    .tile_y   = ty,
    .zoom     = (uint16_t)k_zoom_tiles_native_level,
    .reserved = 0U,
  };
  ra8_tile_t tile = {};
  RA8_RETURN_ON_ERROR(ra8_tile_cache_get(ts->cache, &key, &tile), s_tag, "tile fetch");

  const uint32_t org_x = (uint32_t)tx * (uint32_t)ts->tile_w;
  const uint32_t org_y = (uint32_t)ty * (uint32_t)ts->tile_h;
  const uint32_t exp_w =
    ((org_x + (uint32_t)ts->tile_w) > ts->width) ? (ts->width - org_x) : (uint32_t)ts->tile_w;
  const uint32_t exp_h =
    ((org_y + (uint32_t)ts->tile_h) > ts->height) ? (ts->height - org_y) : (uint32_t)ts->tile_h;
  /* Decision: a tile that did not decode to the declared gray8 geometry is a
   * format mismatch, not a picture (2 conditions). */
  if (((uint32_t)tile.width != exp_w) || ((uint32_t)tile.height != exp_h)) {
    (void)ra8_tile_cache_put(ts->cache, tile.pixels);
    ra8_log_error(s_tag, "tile decoded to an unexpected extent (wrong bpp or geometry)");
    return k_ra8_err_invalid_size;
  }
  zoom_tile_copy(&tile, org_x, org_y, span, out, stride);
  return ra8_tile_cache_put(ts->cache, tile.pixels);
}

ra8_err_t ra8_zoom_tile_src_init(ra8_zoom_tile_src_t* ts,
                                 ra8_tile_cache_t*    cache,
                                 uint32_t             image_id,
                                 uint32_t             width,
                                 uint32_t             height,
                                 uint16_t             tile_w,
                                 uint16_t             tile_h)
{
  RA8_CHECK_NULL_PTR(ts, s_tag, "binding must not be nullptr");
  RA8_CHECK_NULL_PTR(cache, s_tag, "tile cache must not be nullptr");
  /* Decision: every extent and tile edge must be non-zero (4 conditions). */
  if ((width == 0U) || (height == 0U) || (tile_w == 0U) || (tile_h == 0U)) {
    ra8_log_error(s_tag, "image extent and tile edges must all be non-zero");
    return k_ra8_err_invalid_arg;
  }
  const uint32_t cols = ((width + (uint32_t)tile_w) - 1U) / (uint32_t)tile_w;
  const uint32_t rows = ((height + (uint32_t)tile_h) - 1U) / (uint32_t)tile_h;
  /* Decision: the derived grid must fit the cache key's 16-bit tile indices. */
  if ((cols > (uint32_t)UINT16_MAX) || (rows > (uint32_t)UINT16_MAX)) {
    ra8_log_error(s_tag, "derived tile grid exceeds the 16-bit tile index range");
    return k_ra8_err_invalid_size;
  }
  ts->cache     = cache;
  ts->image_id  = image_id;
  ts->width     = width;
  ts->height    = height;
  ts->tile_w    = tile_w;
  ts->tile_h    = tile_h;
  ts->tile_cols = (uint16_t)cols;
  ts->tile_rows = (uint16_t)rows;
  return k_ra8_ok;
}

ra8_err_t ra8_zoom_tile_src_bind(ra8_zoom_tile_src_t* ts, ra8_zoom_source_t* out)
{
  RA8_CHECK_NULL_PTR(ts, s_tag, "binding must not be nullptr");
  RA8_CHECK_NULL_PTR(out, s_tag, "source out must not be nullptr");
  RA8_CHECK_NULL_PTR(ts->cache, s_tag, "binding was never initialised");
  return ra8_zoom_source_init(out, ra8_zoom_tile_read, ts, ts->width, ts->height);
}

/**
 * @brief Fetch, copy and release one inclusive run of tiles along a tile row.
 *
 * @details Split out of ::zoom_walk_tiles purely so the tile walk stays inside
 *          the project's nesting bar: two nested loops plus the error-check
 *          macro's own `do`/`if` is five levels deep, and the ceiling is four.
 *          Behaviourally it is the inner loop, unchanged.
 *
 * @param[in,out] ts     Bound tiled source.
 * @param[in]     ty     Tile row index.
 * @param[in]     tx0    First tile column (inclusive).
 * @param[in]     tx1    Last tile column (inclusive).
 * @param[in]     span   The requested pixel rectangle.
 * @param[out]    out    gray8 destination laid out at @p stride.
 * @param[in]     stride Bytes between successive output rows.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok    Every tile of the run contributed its overlap.
 * @retval k_ra8_err_* The first failing fetch's code, verbatim.
 *
 * @pre  `(tx0, ty)` and `(tx1, ty)` lie inside the bound tile grid.
 * @pre  @p out addresses at least `stride * span->h` writable bytes.
 * @post No cell is left pinned on any return path.
 * @post On failure the output holds a partial assembly.
 *
 * @note Not thread-safe.
 * @see zoom_tile_fetch
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t zoom_walk_tile_row(ra8_zoom_tile_src_t*    ts,
                                    uint16_t                ty,
                                    uint16_t                tx0,
                                    uint16_t                tx1,
                                    const zoom_tile_span_t* span,
                                    uint8_t*                out,
                                    uint32_t                stride)
{
  for (uint16_t tx = tx0; tx <= tx1; ++tx) {
    RA8_RETURN_ON_ERROR(zoom_tile_fetch(ts, tx, ty, span, out, stride), s_tag, "tile copy");
  }
  return k_ra8_ok;
}

/**
 * @brief Fetch, copy and release every tile of an inclusive tile rectangle.
 *
 * @details The residency claim in one loop: tiles are visited one at a time and
 *          each is released before the next is acquired, so the pin count never
 *          exceeds one and a cache smaller than the rectangle degrades to
 *          re-decoding rather than to ::k_ra8_err_no_mem.
 *
 * @param[in,out] ts     Bound tiled source.
 * @param[in]     tiles  Inclusive tile rectangle covering the request.
 * @param[in]     span   The requested pixel rectangle.
 * @param[out]    out    gray8 destination laid out at @p stride.
 * @param[in]     stride Bytes between successive output rows.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok    Every covering tile contributed its overlap.
 * @retval k_ra8_err_* The first failing fetch's code, verbatim.
 *
 * @pre  @p tiles lies inside the bound grid (::ra8_tile_rect_of_pixels clamps it).
 * @pre  @p out addresses at least `stride * span->h` writable bytes.
 * @post No cell is left pinned on any return path.
 * @post On failure the output holds a partial assembly and must not be shown.
 *
 * @note Not thread-safe.
 * @see zoom_tile_fetch
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t zoom_walk_tiles(ra8_zoom_tile_src_t*    ts,
                                 const ra8_tile_rect_t*  tiles,
                                 const zoom_tile_span_t* span,
                                 uint8_t*                out,
                                 uint32_t                stride)
{
  for (uint16_t ty = tiles->ty0; ty <= tiles->ty1; ++ty) {
    RA8_RETURN_ON_ERROR(zoom_walk_tile_row(ts, ty, tiles->tx0, tiles->tx1, span, out, stride),
                        s_tag,
                        "tile row");
  }
  return k_ra8_ok;
}

/**
 * @brief Validate a requested read rectangle against the bound image.
 *
 * @details Two fail-closed rules, kept out of ::ra8_zoom_tile_read so the read
 *          body is the tile walk and nothing else. Neither is a clamp: the zoom
 *          engine only ever asks for rectangles inside the extent it was handed
 *          at bind time, so anything else is a caller defect, and silently
 *          shrinking it would hide the real bug behind a partly-correct image.
 *
 * @param[in] ts         Bound tiled source.
 * @param[in] x          Rectangle left edge, source pixels.
 * @param[in] y          Rectangle top edge, source pixels.
 * @param[in] w          Rectangle width, source pixels.
 * @param[in] h          Rectangle height, source pixels.
 * @param[in] out_stride Bytes between successive output rows.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok               The rectangle is assemblable.
 * @retval k_ra8_err_invalid_arg  Zero extent, or a stride narrower than the rect.
 * @retval k_ra8_err_out_of_range The rectangle leaves the bound image.
 *
 * @pre  @p ts is bound (its extent fields are populated).
 * @pre  @p out_stride describes the caller's real destination buffer.
 * @post No state is modified.
 * @post On k_ra8_ok every pixel of the rectangle lies inside the image.
 *
 * @note Not thread-safe (logs).
 * @see ra8_zoom_tile_read
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t zoom_read_rect_ok(const ra8_zoom_tile_src_t* ts,
                                   uint32_t                   x,
                                   uint32_t                   y,
                                   uint32_t                   w,
                                   uint32_t                   h,
                                   uint32_t                   out_stride)
{
  /* Decision: an empty rectangle, or one narrower than its own stride, is a
   * caller defect rather than something to clamp (3 conditions). */
  if ((w == 0U) || (h == 0U) || (out_stride < w)) {
    ra8_log_error(s_tag, "read rect must be non-empty and fit its stride");
    return k_ra8_err_invalid_arg;
  }
  /* Decision: the rectangle must lie wholly inside the bound image. */
  if (((x + w) > ts->width) || ((y + h) > ts->height)) {
    ra8_log_error(s_tag, "read rect leaves the bound image");
    return k_ra8_err_out_of_range;
  }
  return k_ra8_ok;
}

ra8_err_t ra8_zoom_tile_read(void*    ctx,
                             uint32_t x,
                             uint32_t y,
                             uint32_t w,
                             uint32_t h,
                             uint8_t* out,
                             uint32_t out_stride)
{
  RA8_CHECK_NULL_PTR(ctx, s_tag, "read ctx must not be nullptr");
  RA8_CHECK_NULL_PTR(out, s_tag, "read destination must not be nullptr");
  ra8_zoom_tile_src_t* ts = (ra8_zoom_tile_src_t*)ctx;
  RA8_CHECK_NULL_PTR(ts->cache, s_tag, "binding was never initialised");
  RA8_RETURN_ON_ERROR(zoom_read_rect_ok(ts, x, y, w, h, out_stride), s_tag, "read rect check");

  ra8_tile_rect_t tiles = {};
  RA8_RETURN_ON_ERROR(ra8_tile_rect_of_pixels(x,
                                              y,
                                              w,
                                              h,
                                              ts->tile_w,
                                              ts->tile_h,
                                              ts->tile_cols,
                                              ts->tile_rows,
                                              &tiles),
                      s_tag,
                      "pixel rect to tile rect");

  const zoom_tile_span_t span = {.x = x, .y = y, .w = w, .h = h};
  return zoom_walk_tiles(ts, &tiles, &span, out, out_stride);
}

ra8_err_t ra8_zoom_tiles_prefetch(ra8_zoom_tile_src_t*   ts,
                                  const ra8_zoom_view_t* v,
                                  ra8_zoom_pan_t         dir,
                                  uint16_t               max_tiles,
                                  uint16_t*              out_warmed)
{
  RA8_CHECK_NULL_PTR(ts, s_tag, "binding must not be nullptr");
  RA8_CHECK_NULL_PTR(v, s_tag, "view must not be nullptr");
  RA8_CHECK_NULL_PTR(ts->cache, s_tag, "binding was never initialised");

  ra8_ui_rect_t win = {};
  RA8_RETURN_ON_ERROR(ra8_zoom_view_window(v, &win), s_tag, "visible window");

  ra8_tile_rect_t tiles = {};
  RA8_RETURN_ON_ERROR(ra8_tile_rect_of_pixels((uint32_t)win.x,
                                              (uint32_t)win.y,
                                              (uint32_t)win.w,
                                              (uint32_t)win.h,
                                              ts->tile_w,
                                              ts->tile_h,
                                              ts->tile_cols,
                                              ts->tile_rows,
                                              &tiles),
                      s_tag,
                      "visible window to tile rect");

  const ra8_tile_prefetch_req_t req = {
    .image_id  = ts->image_id,
    .view      = tiles,
    .tile_cols = ts->tile_cols,
    .tile_rows = ts->tile_rows,
    .zoom      = (uint16_t)k_zoom_tiles_native_level,
    .max_tiles = max_tiles,
    .dir       = (ra8_tile_pan_dir_t)dir,
  };
  return ra8_tile_cache_prefetch_pan(ts->cache, &req, out_warmed);
}

#endif /* RA8_ZOOM_HAVE_TILES */
