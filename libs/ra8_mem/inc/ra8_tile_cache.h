/**
 * @file ra8_tile_cache.h
 * @brief Fixed-RAM-budget image-tile cache with LRU eviction (Layer 3b, #147).
 * @ingroup grp_ereader
 *
 * @par Tag
 * [Ring 2 / Core] {World: NS}
 *
 * @details
 * The image counterpart of the #147 glyph atlas: a tile cache so the reader can
 * pan/zoom a comic page or a large illustration far bigger than RAM (CBZ/CBR,
 * full-bleed covers) while resident decoded-pixel memory stays bounded. A tile is
 * keyed by `(image_id, tile_x, tile_y, zoom)` and the cache returns a pinned view
 * of the decoded pixels; a miss decodes the tile region into a free/evicted cell
 * through a caller-supplied decoder (an stb_image-backed decoder in production --
 * see the bare-metal arena/TLS notes around ::ra8_img_arena -- and a synthetic
 * generator in tests).
 *
 * Like ::ra8_glyph_atlas, this is a thin typed facade over the reusable
 * ::ra8_keycache (key bytes + cell bytes + render-on-miss + pin/unpin): the tile
 * key is the cache key, the decoded tile is the cell payload, and the decoded
 * width/height ride in the per-cell user descriptor (::ra8_tile_dims_t). Tiles
 * differ from glyphs only in scale -- cells are large (a 64x64 RGB565 tile is
 * 8 KiB), so the budget yields fewer cells. Eviction is LRU with pinned-cell
 * skip: the tiles composited into the current frame are pinned while on screen,
 * so a scroll cannot evict a tile still being blitted. Edge tiles may decode
 * smaller than a full tile; the decoder reports the true `out_w`/`out_h`.
 *
 * Zero allocation (NASA P10 Rule 3): the caller provides the cell storage, the
 * key storage, the per-cell dimension descriptors, the per-cell link metadata,
 * and the hash buckets, carved once from a tier ::ra8_arena / ::ra8_slab at init
 * (tile pixels in SDRAM; the metadata in DTCM for the hot path).
 *
 * @note Not thread-safe; the decoder is single-threaded.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * @since 0.1.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_keycache.h"

/**
 * @struct ra8_tile_key_t
 * @brief Identifies one decoded image tile.
 *
 * @details Compared byte-wise by the underlying ::ra8_keycache, so the struct must
 *          be fully initialised before use: zero-fill it (e.g. `= {}`) before
 *          setting the fields so the @ref reserved padding never carries
 *          indeterminate bytes. The layout is padding-free (12 bytes).
 *
 * @since 0.1.0
 */
typedef struct {
  uint32_t image_id; /**< Source image identifier.                          */
  uint16_t tile_x;   /**< Tile column index (in tile units).                */
  uint16_t tile_y;   /**< Tile row index (in tile units).                   */
  uint16_t zoom;     /**< Zoom / mip level (0 = native).                    */
  uint16_t reserved; /**< Reserved; keep zero (padding-free byte-wise key). */
} ra8_tile_key_t;

/* The key is compared/hashed byte-wise over sizeof(ra8_tile_key_t); guard the
 * padding-free layout at compile time so an accidental field change cannot
 * introduce indeterminate padding bytes into the comparison. */
static_assert(sizeof(ra8_tile_key_t) == sizeof(uint32_t) + sizeof(uint16_t) + sizeof(uint16_t) +
                                          sizeof(uint16_t) + sizeof(uint16_t),
              "ra8_tile_key_t must be padding-free for byte-wise key comparison");

/**
 * @struct ra8_tile_dims_t
 * @brief Per-cell user descriptor: the decoded tile dimensions.
 *
 * @details The decoder writes the decoded tile's width/height here;
 *          ::ra8_tile_cache_get reads them back into ::ra8_tile_t. Treat as
 *          private; the cache owns the contents.
 *
 * @since 0.1.0
 */
typedef struct {
  uint16_t w; /**< Decoded tile width in pixels.  */
  uint16_t h; /**< Decoded tile height in pixels. */
} ra8_tile_dims_t;

/**
 * @struct ra8_tile_t
 * @brief A pinned view of a cached tile returned by ::ra8_tile_cache_get.
 *
 * @details `pixels` points into the cache cell and stays valid until the matching
 *          ::ra8_tile_cache_put. The pixel format and stride are an agreement
 *          between the caller and its decoder (e.g. RGB565, `width*2` bytes/row).
 *
 * @since 0.1.0
 */
typedef struct {
  const uint8_t* pixels; /**< Decoded tile pixels (cell payload). */
  uint16_t       width;  /**< Decoded tile width in pixels.       */
  uint16_t       height; /**< Decoded tile height in pixels.      */
} ra8_tile_t;

/**
 * @typedef ra8_tile_decode_fn
 * @brief Decode an image tile into a cache cell (decode-on-miss DIP seam).
 *
 * @param[in]  ctx        Opaque decoder context (`decode_ctx` from the config).
 * @param[in]  key        The tile to decode.
 * @param[out] cell       Destination pixel buffer (`cell_bytes` writable).
 * @param[in]  cell_bytes Cell capacity in bytes.
 * @param[out] out_w      Decoded tile width in pixels.
 * @param[out] out_h      Decoded tile height in pixels.
 *
 * @return ra8_err_t k_ra8_ok on success (with the decoded pixels fitting
 *         `cell_bytes`); any error aborts the get with that code.
 *
 * @since 0.1.0
 */
typedef ra8_err_t (*ra8_tile_decode_fn)(void*                 ctx,
                                        const ra8_tile_key_t* key,
                                        uint8_t*              cell,
                                        uint32_t              cell_bytes,
                                        uint16_t*             out_w,
                                        uint16_t*             out_h);

/**
 * @struct ra8_tile_cache_cfg_t
 * @brief Caller-supplied storage + decoder for ::ra8_tile_cache_init.
 *
 * @details All arrays are caller-owned and must out-live the cache. `meta`,
 *          `keys`, and `dims` are parallel per-cell arrays (`cell_count`
 *          entries each); `cell_mem` holds the decoded tiles.
 *
 * @since 0.1.0
 */
typedef struct {
  uint8_t*             cell_mem;     /**< `cell_count * cell_bytes` of tile storage. */
  uint32_t             cell_bytes;   /**< Bytes per cell (max decoded tile).         */
  uint32_t             cell_count;   /**< Number of cells.                           */
  ra8_keycache_cell_t* meta;         /**< `cell_count` link-metadata entries.        */
  ra8_tile_key_t*      keys;         /**< `cell_count` key-storage entries.          */
  ra8_tile_dims_t*     dims;         /**< `cell_count` dimension descriptors.        */
  int32_t*             buckets;      /**< `bucket_count` hash-bucket heads.          */
  uint32_t             bucket_count; /**< Number of hash buckets (>= 1).             */
  ra8_tile_decode_fn   decode;       /**< Decode-on-miss callback.                   */
  void*                decode_ctx;   /**< Opaque context passed to @c decode.        */
} ra8_tile_cache_cfg_t;

/**
 * @struct ra8_tile_cache_t
 * @brief Tile-cache state (caller-owned; treat as private).
 *
 * @invariant Every valid cell is reachable from exactly one hash bucket.
 *
 * @since 0.1.0
 */
typedef struct {
  ra8_keycache_t     kc;         /**< Underlying keyed-LRU cache. */
  ra8_tile_decode_fn decode;     /**< Caller's tile decoder.      */
  void*              decode_ctx; /**< Caller's decoder context.   */
} ra8_tile_cache_t;

/**
 * @enum ra8_tile_pan_dir_t
 * @brief Direction of viewport travel, for predictive pan prefetch.
 *
 * @details ::ra8_tile_cache_prefetch_pan warms the tile row or column one step
 *          ahead of the visible rectangle in this direction. ::k_ra8_tile_pan_none
 *          warms nothing (a still viewport, or a pan that did not move).
 *
 * @see ra8_tile_cache_prefetch_pan
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_ra8_tile_pan_none  = 0U, /**< No travel: prefetch is a no-op.        */
  k_ra8_tile_pan_left  = 1U, /**< Warm the column left of the viewport.  */
  k_ra8_tile_pan_right = 2U, /**< Warm the column right of the viewport. */
  k_ra8_tile_pan_up    = 3U, /**< Warm the row above the viewport.       */
  k_ra8_tile_pan_down  = 4U, /**< Warm the row below the viewport.       */
} ra8_tile_pan_dir_t;

/**
 * @struct ra8_tile_rect_t
 * @brief An inclusive rectangle of tile grid coordinates (the visible tiles).
 *
 * @details All four bounds are tile indices, inclusive. `tx0 <= tx1` and
 *          `ty0 <= ty1` describe the tiles the current viewport straddles; the
 *          prefetch reads the edge just beyond this rectangle.
 *
 * @invariant `tx0 <= tx1` and `ty0 <= ty1`.
 * @since 0.1.0
 */
typedef struct {
  uint16_t tx0; /**< Leftmost visible tile column (inclusive).  */
  uint16_t ty0; /**< Topmost visible tile row (inclusive).      */
  uint16_t tx1; /**< Rightmost visible tile column (inclusive). */
  uint16_t ty1; /**< Bottommost visible tile row (inclusive).   */
} ra8_tile_rect_t;

/**
 * @struct ra8_tile_prefetch_req_t
 * @brief A predictive pan-prefetch request: what is visible + where it heads.
 *
 * @details Describes the visible tile rectangle (@ref view), the tile grid extent
 *          used to clamp the lead edge to the image (@ref tile_cols / @ref
 *          tile_rows), the direction of travel (@ref dir), and the residency
 *          budget (@ref max_tiles) that caps how many tiles the prefetch may warm
 *          so it never exceeds the cache's spare capacity.
 *
 * @invariant `view.tx1 < tile_cols` and `view.ty1 < tile_rows`.
 * @see ra8_tile_cache_prefetch_pan
 * @since 0.1.0
 */
typedef struct {
  uint32_t           image_id;  /**< Tile-cache key image id (the image panned). */
  ra8_tile_rect_t    view;      /**< The tiles the viewport currently straddles. */
  uint16_t           tile_cols; /**< Image tile columns (lead-edge clamp bound). */
  uint16_t           tile_rows; /**< Image tile rows (lead-edge clamp bound).    */
  uint16_t           zoom;      /**< Zoom / mip level for the key (0 = native).  */
  uint16_t           max_tiles; /**< Residency budget: warm at most this many.   */
  ra8_tile_pan_dir_t dir;       /**< Direction of viewport travel.               */
} ra8_tile_prefetch_req_t;

/**
 * @brief Convert a pixel rectangle into the inclusive tile rectangle covering it.
 *
 * @details
 * The producer ::ra8_tile_rect_t never had. Every consumer that needed "which
 * tiles does this viewport touch?" open-coded the same four divisions in its own
 * app, so the residency question was answered in a slightly different place from
 * the prefetch that acts on it. Answering it once, here, is what lets a zoom
 * viewer state its resident set and warm its lead edge from the same number.
 *
 * The result is clamped to the tile grid, so a rectangle that runs past the image
 * edge yields the last tile rather than an out-of-grid index the prefetch would
 * then have to re-clamp. An empty rectangle (`pw == 0` or `ph == 0`) is rejected
 * rather than collapsed: a viewport showing no pixels is a caller defect, and
 * silently returning tile (0,0) would understate residency.
 *
 * @param[in]  px        Rectangle left edge, source pixels.
 * @param[in]  py        Rectangle top edge, source pixels.
 * @param[in]  pw        Rectangle width, source pixels (`> 0`).
 * @param[in]  ph        Rectangle height, source pixels (`> 0`).
 * @param[in]  tile_w    Tile width, pixels (`> 0`).
 * @param[in]  tile_h    Tile height, pixels (`> 0`).
 * @param[in]  tile_cols Tile columns in the image (`> 0`; the clamp bound).
 * @param[in]  tile_rows Tile rows in the image (`> 0`; the clamp bound).
 * @param[out] out       Receives the inclusive tile rectangle.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok               @p out spans exactly the covering tiles.
 * @retval k_ra8_err_null_ptr     @p out is NULL.
 * @retval k_ra8_err_invalid_arg  An extent or a grid dimension is zero.
 *
 * @pre  @p out addresses writable storage for one ::ra8_tile_rect_t.
 * @pre  The tile geometry describes the same image the pixel rectangle indexes.
 * @post `out->tx0 <= out->tx1 < tile_cols` and `out->ty0 <= out->ty1 < tile_rows`.
 * @post Every pixel of the rectangle inside the image lies in a tile of @p out.
 *
 * @note Pure integer arithmetic; thread-safe.
 *
 * @par Example:
 * @code
 * ra8_ui_rect_t win = {};
 * (void)ra8_zoom_view_window(&view, &win);
 * ra8_tile_rect_t tiles = {};
 * (void)ra8_tile_rect_of_pixels((uint32_t)win.x, (uint32_t)win.y,
 *                               (uint32_t)win.w, (uint32_t)win.h,
 *                               tile_w, tile_h, tile_cols, tile_rows, &tiles);
 * @endcode
 *
 * @see ra8_tile_cache_prefetch_pan()
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_tile_rect_of_pixels(uint32_t         px,
                                                uint32_t         py,
                                                uint32_t         pw,
                                                uint32_t         ph,
                                                uint16_t         tile_w,
                                                uint16_t         tile_h,
                                                uint16_t         tile_cols,
                                                uint16_t         tile_rows,
                                                ra8_tile_rect_t* out);

/**
 * @brief Initialise a tile cache over caller-supplied storage.
 *
 * @param[out] tc  Cache state to populate (zero-initialised by the caller).
 * @param[in]  cfg Storage + decoder configuration.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok               Cache ready; all cells cold.
 * @retval k_ra8_err_null_ptr     `tc`, `cfg`, or a required `cfg` pointer NULL.
 * @retval k_ra8_err_invalid_size `cell_count`, `cell_bytes`, or `bucket_count` 0.
 *
 * @pre `cfg`'s arrays cover their sizes and out-live the cache.
 * @pre `cfg->decode` is non-NULL.
 * @post On success the cache is empty and buckets are cleared.
 * @post On any non-ok return `tc` is left unbound.
 *
 * @note Not thread-safe.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_tile_cache_init(ra8_tile_cache_t* tc, const ra8_tile_cache_cfg_t* cfg);

/**
 * @brief Get (and pin) the decoded tile for @p key.
 *
 * @details On a hit the cell is moved to the MRU and pinned. On a miss an
 *          unpinned LRU victim is evicted, the tile is decoded into the cell,
 *          inserted, and pinned. The returned pixels stay valid until
 *          ::ra8_tile_cache_put.
 *
 * @param[in]  tc       Initialised cache.
 * @param[in]  key      Tile to fetch.
 * @param[out] out_tile Receives the pinned tile view.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok           Tile resident and pinned; `*out_tile` set.
 * @retval k_ra8_err_null_ptr `tc`, `key`, or `out_tile` was NULL.
 * @retval k_ra8_err_no_mem   Every cell is pinned (cannot evict for the miss).
 * @retval k_ra8_err_*        The decoder failed (returned verbatim).
 *
 * @pre `tc` was populated by ::ra8_tile_cache_init.
 * @pre The caller will ::ra8_tile_cache_put the returned tile.
 * @post On success the cell's pin count increased by one.
 * @post On any non-ok return no new pin is held.
 *
 * @note Not thread-safe.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_tile_cache_get(ra8_tile_cache_t* tc, const ra8_tile_key_t* key, ra8_tile_t* out_tile);

/**
 * @brief Release one pin on a tile previously returned by ::ra8_tile_cache_get.
 *
 * @param[in] tc     Initialised cache.
 * @param[in] pixels The `pixels` pointer from a returned ::ra8_tile_t.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok              Pin released.
 * @retval k_ra8_err_null_ptr    `tc` or `pixels` was NULL.
 * @retval k_ra8_err_invalid_arg `pixels` is not a cell of this cache, or the cell
 *                              was not pinned.
 *
 * @pre `pixels` came from ::ra8_tile_cache_get on this cache and is still pinned.
 * @pre `tc` was populated by ::ra8_tile_cache_init.
 * @post On success the cell's pin count decreased by one.
 * @post On any non-ok return no state changed.
 *
 * @note Not thread-safe.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_tile_cache_put(ra8_tile_cache_t* tc, const uint8_t* pixels);

/**
 * @brief Report the number of cells the cache can hold.
 *
 * @details The cache capacity in tiles, used by a pan-prefetch caller to size its
 *          residency budget (spare = capacity - currently-visible tiles) so a
 *          prefetch cannot evict an on-screen tile.
 *
 * @param[in] tc Initialised cache.
 *
 * @return uint32_t The cell count, or 0 if @p tc is NULL / uninitialised.
 * @retval 0     @p tc was NULL or was never ::ra8_tile_cache_init'd.
 * @retval >0    The configured cell count.
 *
 * @pre `tc` was populated by ::ra8_tile_cache_init (else 0 is returned).
 * @pre None beyond the above.
 * @post No cache state is mutated.
 * @post The result is the exact configured cell count.
 *
 * @note Thread-safe with respect to a quiescent cache (pure read).
 * @since 0.1.0
 */
[[nodiscard]] uint32_t ra8_tile_cache_capacity(const ra8_tile_cache_t* tc);

/**
 * @brief Warm one tile into the cache without holding a pin (read-ahead).
 *
 * @details Decode-on-miss + immediate unpin, so on return the tile is resident but
 *          evictable. A thin typed facade over ::ra8_keycache_prefetch: warming
 *          changes only residency, never the pixels a later ::ra8_tile_cache_get
 *          returns, so rendered output is unchanged and goldens hold.
 *
 * @param[in,out] tc  Initialised cache.
 * @param[in]     key Tile to warm (fully initialised; zero-fill before setting).
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok           The tile is resident and unpinned (warmed or hit).
 * @retval k_ra8_err_null_ptr `tc` or `key` was NULL.
 * @retval k_ra8_err_no_mem   Every cell is pinned (cannot evict to warm).
 * @retval k_ra8_err_*        The decoder failed (returned verbatim).
 *
 * @pre `tc` was populated by ::ra8_tile_cache_init.
 * @pre `key` names a valid tile of a decodable image.
 * @post On success the tile is resident with pin count zero.
 * @post On any non-ok return no pin is held.
 *
 * @note Not thread-safe. Single-threaded read-ahead only.
 * @see ra8_tile_cache_prefetch_pan()
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_tile_cache_prefetch(ra8_tile_cache_t* tc, const ra8_tile_key_t* key);

/**
 * @brief Predictively warm the tiles one step ahead of a panning viewport.
 *
 * @details
 * On a pan, warms the tile row or column immediately beyond @p req->view in
 * @p req->dir, so the next tiles the viewport exposes are resident before they
 * are needed -- the image counterpart of ::ra8_book_src_prefetch_chapter's text
 * read-ahead. The lead edge is clamped to the tile grid (@p req->tile_cols /
 * @p req->tile_rows): a pan already at the image edge warms nothing. The number
 * of tiles warmed is bounded by @p req->max_tiles (the caller's spare-capacity
 * budget) and by the edge length, so the prefetch cannot exceed the cache budget
 * or evict an on-screen tile. Best-effort: each tile is warmed with
 * ::ra8_tile_cache_prefetch and a per-tile failure stops the sweep without
 * failing the call, so a pan never errors because read-ahead could not complete.
 *
 * @param[in,out] tc         Initialised cache.
 * @param[in]     req        The visible rectangle + direction + budget.
 * @param[out]    out_warmed Tiles warmed by this call (may be NULL).
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok              The sweep ran (0 or more tiles warmed).
 * @retval k_ra8_err_null_ptr    `tc` or `req` was NULL.
 * @retval k_ra8_err_invalid_arg `req->view` is not `tx0<=tx1` / `ty0<=ty1`, or a
 *                              bound lies outside the tile grid.
 *
 * @pre `tc` was populated by ::ra8_tile_cache_init.
 * @pre `req->view` is a valid inclusive rectangle inside the tile grid.
 * @post On success at most `req->max_tiles` lead-edge tiles are resident.
 * @post No on-screen (already-visible) tile is evicted by this call.
 *
 * @note Not thread-safe. Single-threaded read-ahead only.
 * @see ra8_tile_cache_prefetch()
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_tile_cache_prefetch_pan(ra8_tile_cache_t*              tc,
                                                    const ra8_tile_prefetch_req_t* req,
                                                    uint16_t*                      out_warmed);

/**
 * @brief Report the cache hit / miss / eviction counters.
 *
 * @param[in]  tc            Initialised cache.
 * @param[out] out_hits      Hits so far (may be NULL).
 * @param[out] out_misses    Misses so far (may be NULL).
 * @param[out] out_evictions Evictions so far (may be NULL).
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok               Counters reported.
 * @retval k_ra8_err_null_ptr     `tc` was NULL.
 * @retval k_ra8_err_invalid_state The cache was not initialised.
 *
 * @pre `tc` was populated by ::ra8_tile_cache_init.
 * @pre At least one output pointer is non-NULL to be useful.
 * @post On success the requested counters are written.
 * @post No cache state is mutated.
 *
 * @note Thread-safe with respect to a quiescent cache (pure read).
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_tile_cache_stats(const ra8_tile_cache_t* tc,
                                             uint32_t*               out_hits,
                                             uint32_t*               out_misses,
                                             uint32_t*               out_evictions);

#ifdef __cplusplus
}
#endif
