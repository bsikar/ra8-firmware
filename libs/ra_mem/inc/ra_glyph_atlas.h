/**
 * @file ra_glyph_atlas.h
 * @brief Fixed-RAM-budget glyph cache with LRU eviction (Layer 3, #147).
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
 * Eviction is LRU with pinned-frame skip: the current page's glyphs are pinned
 * while it is on screen, so a page-turn cannot evict a glyph still being drawn.
 * (Glyph reuse has strong per-page locality, so plain LRU suffices here -- the
 * scan-resistant SLRU lives in the ::ra_vmem page cache where linear file floods
 * happen.) Cells are fixed-size, sized to the largest glyph bitmap the budget
 * allows; a glyph larger than a cell is rejected at render time.
 *
 * Zero allocation (NASA P10 Rule 3): the caller provides the cell storage, the
 * per-cell metadata array, and the hash buckets, carved once from a tier
 * ::ra_arena / ::ra_slab at init (hot tier = SRAM/DTCM).
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

#include "ra_err.h"

/**
 * @struct ra_glyph_key_t
 * @brief Identifies one rendered glyph.
 *
 * @details Equality is field-wise (no padding is compared), so callers may
 *          memset the struct to zero before setting fields.
 *
 * @since 0.1.0
 */
typedef struct {
  uint32_t glyph_id; /**< Font glyph index.                                  */
  uint16_t face_id;  /**< Font face identifier.                              */
  uint16_t size_px;  /**< Pixel size.                                        */
  uint16_t mode;     /**< Render mode (AA / mono / hinting variant).         */
} ra_glyph_key_t;

/**
 * @struct ra_glyph_t
 * @brief A pinned view of a cached glyph bitmap returned by ::ra_glyph_get.
 *
 * @details `bitmap` points into the cache cell and stays valid until the
 *          matching ::ra_glyph_put. Stride is `width` bytes (8-bit coverage).
 *
 * @since 0.1.0
 */
typedef struct {
  const uint8_t* bitmap; /**< Glyph coverage bitmap (width*height bytes).     */
  uint16_t       width;  /**< Glyph width in pixels.                         */
  uint16_t       height; /**< Glyph height in pixels.                        */
} ra_glyph_t;

/**
 * @typedef ra_glyph_render_fn
 * @brief Rasterise a glyph into a cache cell (render-on-miss DIP seam).
 *
 * @param[in]  ctx        Opaque renderer context (`render_ctx` from the config).
 * @param[in]  key        The glyph to render.
 * @param[out] cell       Destination bitmap buffer (`cell_bytes` writable).
 * @param[in]  cell_bytes Cell capacity in bytes.
 * @param[out] out_w      Rendered glyph width in pixels.
 * @param[out] out_h      Rendered glyph height in pixels.
 *
 * @return ra_err_t k_ra_ok on success (with `out_w*out_h <= cell_bytes`); any
 *         error aborts the get with that code.
 *
 * @since 0.1.0
 */
typedef ra_err_t (*ra_glyph_render_fn)(void*                 ctx,
                                       const ra_glyph_key_t* key,
                                       uint8_t*              cell,
                                       uint32_t              cell_bytes,
                                       uint16_t*             out_w,
                                       uint16_t*             out_h);

/**
 * @struct ra_glyph_cell_t
 * @brief Per-cell metadata (one caller-owned array entry per cell).
 *
 * @details Treat as private; the cache owns the contents.
 *
 * @since 0.1.0
 */
/* cppcheck-suppress-begin [unusedStructMember] */
typedef struct {
  ra_glyph_key_t key;       /**< Cached glyph key (valid only when in use).   */
  int32_t        prev;      /**< LRU link toward MRU, or -1.                   */
  int32_t        next;      /**< LRU link toward LRU, or -1.                   */
  int32_t        hash_next; /**< Hash bucket chain link, or -1.               */
  uint16_t       width;     /**< Cached glyph width.                          */
  uint16_t       height;    /**< Cached glyph height.                         */
  uint16_t       pin_count; /**< Outstanding pins (0 => evictable).           */
  uint8_t        valid;     /**< 1 => this cell holds a glyph.                */
} ra_glyph_cell_t;
/* cppcheck-suppress-end [unusedStructMember] */

/**
 * @struct ra_glyph_atlas_cfg_t
 * @brief Caller-supplied storage + renderer for ::ra_glyph_atlas_init.
 *
 * @since 0.1.0
 */
typedef struct {
  uint8_t*           cell_mem;     /**< `cell_count * cell_bytes` of bitmap storage. */
  uint32_t           cell_bytes;   /**< Bytes per cell (max glyph bitmap).           */
  uint32_t           cell_count;   /**< Number of cells.                             */
  ra_glyph_cell_t*   meta;         /**< `cell_count` metadata entries.               */
  int32_t*           buckets;      /**< `bucket_count` hash-bucket heads.            */
  uint32_t           bucket_count; /**< Number of hash buckets (>= 1).               */
  ra_glyph_render_fn render;       /**< Render-on-miss callback.                     */
  void*              render_ctx;   /**< Opaque context passed to @c render.          */
} ra_glyph_atlas_cfg_t;

/**
 * @struct ra_glyph_atlas_t
 * @brief Glyph-cache state (caller-owned; treat as private).
 *
 * @invariant Every valid cell is reachable from exactly one hash bucket.
 *
 * @since 0.1.0
 */
/* cppcheck-suppress-begin [unusedStructMember] */
typedef struct {
  ra_glyph_atlas_cfg_t cfg;       /**< Configuration (copied at init).        */
  int32_t              lru_head;  /**< MRU cell, or -1.                       */
  int32_t              lru_tail;  /**< LRU cell, or -1.                       */
  uint32_t             hits;      /**< Get hits so far.                       */
  uint32_t             misses;    /**< Get misses so far.                     */
  uint32_t             evictions; /**< Glyphs evicted so far.                 */
} ra_glyph_atlas_t;
/* cppcheck-suppress-end [unusedStructMember] */

/**
 * @brief Initialise a glyph atlas over caller-supplied storage.
 *
 * @param[out] atlas Atlas state to populate (zero-initialised by the caller).
 * @param[in]  cfg   Storage + renderer configuration.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok               Atlas ready; all cells cold.
 * @retval k_ra_err_null_ptr     `atlas`, `cfg`, or a required `cfg` pointer NULL.
 * @retval k_ra_err_invalid_size `cell_count`, `cell_bytes`, or `bucket_count` 0.
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
[[nodiscard]] ra_err_t ra_glyph_atlas_init(ra_glyph_atlas_t* atlas, const ra_glyph_atlas_cfg_t* cfg);

/**
 * @brief Get (and pin) the rendered glyph for @p key.
 *
 * @details On a hit the cell is moved to the MRU and pinned. On a miss an
 *          unpinned LRU victim is evicted, the glyph is rendered into the cell,
 *          inserted, and pinned. The returned bitmap stays valid until
 *          ::ra_glyph_put.
 *
 * @param[in]  atlas     Initialised atlas.
 * @param[in]  key       Glyph to fetch.
 * @param[out] out_glyph Receives the pinned glyph view.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok           Glyph resident and pinned; `*out_glyph` set.
 * @retval k_ra_err_null_ptr `atlas`, `key`, or `out_glyph` was NULL.
 * @retval k_ra_err_no_mem   Every cell is pinned (cannot evict for the miss).
 * @retval k_ra_err_*        The renderer failed (returned verbatim).
 *
 * @pre `atlas` was populated by ::ra_glyph_atlas_init.
 * @pre The caller will ::ra_glyph_put the returned glyph.
 * @post On success the cell's pin count increased by one.
 * @post On any non-ok return no new pin is held.
 *
 * @note Not thread-safe.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_glyph_atlas_get(ra_glyph_atlas_t*     atlas,
                                          const ra_glyph_key_t* key,
                                          ra_glyph_t*           out_glyph);

/**
 * @brief Release one pin on a glyph previously returned by ::ra_glyph_atlas_get.
 *
 * @param[in] atlas  Initialised atlas.
 * @param[in] bitmap The `bitmap` pointer from a returned ::ra_glyph_t.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok              Pin released.
 * @retval k_ra_err_null_ptr    `atlas` or `bitmap` was NULL.
 * @retval k_ra_err_invalid_arg `bitmap` is not a cell of this atlas, or the cell
 *                              was not pinned.
 *
 * @pre `bitmap` came from ::ra_glyph_atlas_get on this atlas and is still pinned.
 * @pre `atlas` was populated by ::ra_glyph_atlas_init.
 * @post On success the cell's pin count decreased by one.
 * @post On any non-ok return no state changed.
 *
 * @note Not thread-safe.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_glyph_atlas_put(ra_glyph_atlas_t* atlas, const uint8_t* bitmap);

/**
 * @brief Report the atlas hit / miss / eviction counters.
 *
 * @param[in]  atlas         Initialised atlas.
 * @param[out] out_hits      Hits so far (may be NULL).
 * @param[out] out_misses    Misses so far (may be NULL).
 * @param[out] out_evictions Evictions so far (may be NULL).
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok           Counters reported.
 * @retval k_ra_err_null_ptr `atlas` was NULL.
 *
 * @pre `atlas` was populated by ::ra_glyph_atlas_init.
 * @pre At least one output pointer is non-NULL to be useful.
 * @post On success the requested counters are written.
 * @post No atlas state is mutated.
 *
 * @note Thread-safe with respect to a quiescent atlas (pure read).
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_glyph_atlas_stats(const ra_glyph_atlas_t* atlas,
                                            uint32_t*               out_hits,
                                            uint32_t*               out_misses,
                                            uint32_t*               out_evictions);

#ifdef __cplusplus
}
#endif
