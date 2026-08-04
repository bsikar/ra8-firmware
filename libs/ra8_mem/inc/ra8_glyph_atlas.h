/**
 * @file ra8_glyph_atlas.h
 * @brief Fixed-RAM-budget glyph cache with LRU eviction (Layer 3, #147).
 * @ingroup grp_ereader
 *
 * @par Tag
 * [Ring 2 / Core] {World: NS}
 *
 * @details
 * The original ask behind the #147 memory hierarchy: a glyph cache with a fixed
 * RAM budget so the text renderer never re-rasterises a glyph it drew recently,
 * while resident glyph memory stays bounded regardless of how many glyphs a book
 * touches. A glyph is keyed by (face, pixel size, glyph id, render mode), and the
 * cache returns a pinned view of the rendered bitmap; a miss renders the glyph
 * into a free/evicted cell through a caller-supplied renderer (the FreeType/STB
 * rasteriser in production, a stub in tests).
 *
 * This is a thin typed facade over the reusable ::ra8_keycache (key bytes + cell
 * bytes + render-on-miss + pin/unpin): the glyph key is the cache key, the glyph
 * bitmap is the cell payload, and the rendered width/height ride in the per-cell
 * user descriptor (::ra8_glyph_dims_t). The image-tile cache (::ra8_tile_cache) is
 * the second facade over the same machinery. Eviction is LRU with pinned-frame
 * skip: the current page's glyphs are pinned while on screen, so a page-turn
 * cannot evict a glyph still being drawn. (Glyph reuse has strong per-page
 * locality, so plain LRU suffices here -- the scan-resistant SLRU lives in the
 * ::ra8_vmem page cache where linear file floods happen.) Cells are fixed-size,
 * sized to the largest glyph bitmap the budget allows; a glyph larger than a cell
 * is rejected at render time.
 *
 * Zero allocation (NASA P10 Rule 3): the caller provides the cell storage, the
 * key storage, the per-cell dimension descriptors, the per-cell link metadata,
 * and the hash buckets, carved once from a tier ::ra8_arena / ::ra8_slab at init
 * (hot tier = SRAM/DTCM).
 *
 * @note Not thread-safe; the renderer is single-threaded.
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
 * @struct ra8_glyph_key_t
 * @brief Identifies one rendered glyph.
 *
 * @details Compared byte-wise by the underlying ::ra8_keycache, so the struct must
 *          be fully initialised before use: zero-fill it (e.g. `= {}`) before
 *          setting the fields so the @ref reserved padding never carries
 *          indeterminate bytes. The layout is padding-free (12 bytes).
 *
 * @since 0.1.0
 */
typedef struct {
  uint32_t glyph_id; /**< Font glyph index.                                 */
  uint16_t face_id;  /**< Font face identifier.                             */
  uint16_t size_px;  /**< Pixel size.                                       */
  uint16_t mode;     /**< Render mode (AA / mono / hinting variant).        */
  uint16_t reserved; /**< Reserved; keep zero (padding-free byte-wise key). */
} ra8_glyph_key_t;

/* The key is compared/hashed byte-wise over sizeof(ra8_glyph_key_t); guard the
 * padding-free layout at compile time so an accidental field change cannot
 * introduce indeterminate padding bytes into the comparison. */
static_assert(sizeof(ra8_glyph_key_t) == sizeof(uint32_t) + sizeof(uint16_t) + sizeof(uint16_t) +
                                           sizeof(uint16_t) + sizeof(uint16_t),
              "ra8_glyph_key_t must be padding-free for byte-wise key comparison");

/**
 * @struct ra8_glyph_dims_t
 * @brief Per-cell user descriptor: the rendered glyph dimensions.
 *
 * @details The render callback writes the rasterised glyph's width/height here;
 *          ::ra8_glyph_atlas_get reads them back into ::ra8_glyph_t. Treat as
 *          private; the cache owns the contents.
 *
 * @since 0.1.0
 */
typedef struct {
  uint16_t w; /**< Rendered glyph width in pixels.  */
  uint16_t h; /**< Rendered glyph height in pixels. */
} ra8_glyph_dims_t;

/**
 * @struct ra8_glyph_t
 * @brief A pinned view of a cached glyph bitmap returned by ::ra8_glyph_atlas_get.
 *
 * @details `bitmap` points into the cache cell and stays valid until the
 *          matching ::ra8_glyph_atlas_put. Stride is `width` bytes (8-bit coverage).
 *
 * @since 0.1.0
 */
typedef struct {
  const uint8_t* bitmap; /**< Glyph coverage bitmap (width*height bytes). */
  uint16_t       width;  /**< Glyph width in pixels.                      */
  uint16_t       height; /**< Glyph height in pixels.                     */
} ra8_glyph_t;

/**
 * @typedef ra8_glyph_render_fn
 * @brief Rasterise a glyph into a cache cell (render-on-miss DIP seam).
 *
 * @param[in]  ctx        Opaque renderer context (`render_ctx` from the config).
 * @param[in]  key        The glyph to render.
 * @param[out] cell       Destination bitmap buffer (`cell_bytes` writable).
 * @param[in]  cell_bytes Cell capacity in bytes.
 * @param[out] out_w      Rendered glyph width in pixels.
 * @param[out] out_h      Rendered glyph height in pixels.
 *
 * @return ra8_err_t k_ra8_ok on success (with `out_w*out_h <= cell_bytes`); any
 *         error aborts the get with that code.
 *
 * @since 0.1.0
 */
typedef ra8_err_t (*ra8_glyph_render_fn)(void*                  ctx,
                                         const ra8_glyph_key_t* key,
                                         uint8_t*               cell,
                                         uint32_t               cell_bytes,
                                         uint16_t*              out_w,
                                         uint16_t*              out_h);

/**
 * @struct ra8_glyph_atlas_cfg_t
 * @brief Caller-supplied storage + renderer for ::ra8_glyph_atlas_init.
 *
 * @details All arrays are caller-owned and must out-live the atlas. `meta`,
 *          `keys`, and `dims` are parallel per-cell arrays (`cell_count`
 *          entries each); `cell_mem` holds the bitmaps.
 *
 * @since 0.1.0
 */
typedef struct {
  uint8_t*             cell_mem;     /**< `cell_count * cell_bytes` of bitmap storage. */
  uint32_t             cell_bytes;   /**< Bytes per cell (max glyph bitmap).           */
  uint32_t             cell_count;   /**< Number of cells.                             */
  ra8_keycache_cell_t* meta;         /**< `cell_count` link-metadata entries.          */
  ra8_glyph_key_t*     keys;         /**< `cell_count` key-storage entries.            */
  ra8_glyph_dims_t*    dims;         /**< `cell_count` dimension descriptors.          */
  int32_t*             buckets;      /**< `bucket_count` hash-bucket heads.            */
  uint32_t             bucket_count; /**< Number of hash buckets (>= 1).               */
  ra8_glyph_render_fn  render;       /**< Render-on-miss callback.                     */
  void*                render_ctx;   /**< Opaque context passed to @c render.          */
} ra8_glyph_atlas_cfg_t;

/**
 * @struct ra8_glyph_atlas_t
 * @brief Glyph-cache state (caller-owned; treat as private).
 *
 * @invariant Every valid cell is reachable from exactly one hash bucket.
 *
 * @since 0.1.0
 */
typedef struct {
  ra8_keycache_t      kc;         /**< Underlying keyed-LRU cache. */
  ra8_glyph_render_fn render;     /**< Caller's glyph renderer.    */
  void*               render_ctx; /**< Caller's renderer context.  */
} ra8_glyph_atlas_t;

/**
 * @brief Initialise a glyph atlas over caller-supplied storage.
 *
 * @param[out] atlas Atlas state to populate (zero-initialised by the caller).
 * @param[in]  cfg   Storage + renderer configuration.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok               Atlas ready; all cells cold.
 * @retval k_ra8_err_null_ptr     `atlas`, `cfg`, or a required `cfg` pointer NULL.
 * @retval k_ra8_err_invalid_size `cell_count`, `cell_bytes`, or `bucket_count` 0.
 *
 * @pre `cfg`'s arrays cover their sizes and out-live the atlas.
 * @pre `cfg->render` is non-NULL.
 * @post On success the atlas is empty and buckets are cleared.
 * @post On any non-ok return `atlas` is left unbound.
 *
 * @note Not thread-safe.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_glyph_atlas_init(ra8_glyph_atlas_t*           atlas,
                                             const ra8_glyph_atlas_cfg_t* cfg);

/**
 * @brief Get (and pin) the rendered glyph for @p key.
 *
 * @details On a hit the cell is moved to the MRU and pinned. On a miss an
 *          unpinned LRU victim is evicted, the glyph is rendered into the cell,
 *          inserted, and pinned. The returned bitmap stays valid until
 *          ::ra8_glyph_atlas_put.
 *
 * @param[in]  atlas     Initialised atlas.
 * @param[in]  key       Glyph to fetch.
 * @param[out] out_glyph Receives the pinned glyph view.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok           Glyph resident and pinned; `*out_glyph` set.
 * @retval k_ra8_err_null_ptr `atlas`, `key`, or `out_glyph` was NULL.
 * @retval k_ra8_err_no_mem   Every cell is pinned (cannot evict for the miss).
 * @retval k_ra8_err_*        The renderer failed (returned verbatim).
 *
 * @pre `atlas` was populated by ::ra8_glyph_atlas_init.
 * @pre The caller will ::ra8_glyph_atlas_put the returned glyph.
 * @post On success the cell's pin count increased by one.
 * @post On any non-ok return no new pin is held.
 *
 * @note Not thread-safe.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_glyph_atlas_get(ra8_glyph_atlas_t* atlas, const ra8_glyph_key_t* key, ra8_glyph_t* out_glyph);

/**
 * @brief Release one pin on a glyph previously returned by ::ra8_glyph_atlas_get.
 *
 * @param[in] atlas  Initialised atlas.
 * @param[in] bitmap The `bitmap` pointer from a returned ::ra8_glyph_t.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok              Pin released.
 * @retval k_ra8_err_null_ptr    `atlas` or `bitmap` was NULL.
 * @retval k_ra8_err_invalid_arg `bitmap` is not a cell of this atlas, or the cell
 *                              was not pinned.
 *
 * @pre `bitmap` came from ::ra8_glyph_atlas_get on this atlas and is still pinned.
 * @pre `atlas` was populated by ::ra8_glyph_atlas_init.
 * @post On success the cell's pin count decreased by one.
 * @post On any non-ok return no state changed.
 *
 * @note Not thread-safe.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_glyph_atlas_put(ra8_glyph_atlas_t* atlas, const uint8_t* bitmap);

/**
 * @brief Report the atlas hit / miss / eviction counters.
 *
 * @param[in]  atlas         Initialised atlas.
 * @param[out] out_hits      Hits so far (may be NULL).
 * @param[out] out_misses    Misses so far (may be NULL).
 * @param[out] out_evictions Evictions so far (may be NULL).
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok               Counters reported.
 * @retval k_ra8_err_null_ptr     `atlas` was NULL.
 * @retval k_ra8_err_invalid_state The atlas was not initialised.
 *
 * @pre `atlas` was populated by ::ra8_glyph_atlas_init.
 * @pre At least one output pointer is non-NULL to be useful.
 * @post On success the requested counters are written.
 * @post No atlas state is mutated.
 *
 * @note Thread-safe with respect to a quiescent atlas (pure read).
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_glyph_atlas_stats(const ra8_glyph_atlas_t* atlas,
                                              uint32_t*                out_hits,
                                              uint32_t*                out_misses,
                                              uint32_t*                out_evictions);

#ifdef __cplusplus
}
#endif
