/**
 * @file ra8_keycache.h
 * @brief Reusable keyed-LRU cache with render-on-miss + pin/unpin (Layer 3, #147).
 *
 * @par Tag
 * [Ring 2 / Core] {World: NS}
 *
 * @details
 * The shared machinery behind the #147 Layer-3 caches (the glyph atlas and the
 * image-tile cache). A keycache maps a fixed-size opaque **key** to a fixed-size
 * opaque **cell** of rendered bytes: a hit returns a pinned view of the cell; a
 * miss evicts an unpinned LRU victim, fills the cell through a caller-supplied
 * render-on-miss callback, inserts it, and pins it. The caller releases the pin
 * with ::ra8_keycache_put once it is done reading the cell.
 *
 * Eviction is plain LRU with a pinned-cell skip: the victim is the least-recently
 * used cell whose pin count is zero. (The scan-resistant SLRU policy lives one
 * layer down in the ::ra8_vmem page cache, where linear file floods happen; the
 * Layer-3 caches have strong per-page reuse locality, so single-list LRU
 * suffices.) Keys are compared byte-wise (`memcmp` over `key_bytes`) and hashed
 * with FNV-1a, so a key struct must be fully initialised (zero-filled, no
 * indeterminate padding) before use.
 *
 * Each cell may carry a fixed-size **user descriptor** (`user_bytes`, may be 0):
 * the render callback writes per-entry metadata there (e.g. the rendered glyph or
 * tile width/height) and ::ra8_keycache_get hands the descriptor back alongside
 * the cell data. This is how a typed facade (::ra8_glyph_atlas, ::ra8_tile_cache)
 * recovers its dimensions without a parallel side table.
 *
 * Zero allocation (NASA P10 Rule 3): the caller provides every array -- the cell
 * storage, the key storage, the optional user-descriptor storage, the per-cell
 * link metadata, and the hash buckets -- carved once from a tier ::ra8_arena /
 * ::ra8_slab at init (hot tier = SRAM/DTCM).
 *
 * @note Not thread-safe; the renderer and the cache are single-threaded.
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

/**
 * @typedef ra8_keycache_render_fn
 * @brief Fill a cell with the rendered bytes for @p key (render-on-miss seam).
 *
 * @details Called on a cache miss to populate @p cell with the content keyed by
 *          @p key, and (when the cache has a user descriptor) to write that
 *          entry's metadata into @p user. The glyph atlas rasterises a glyph
 *          here; the tile cache decodes an image tile; tests use a synthetic
 *          generator.
 *
 * @param[in]  ctx        Opaque render context (`render_ctx` from the config).
 * @param[in]  key        The `key_bytes`-wide key being filled.
 * @param[out] cell       Destination cell buffer (`cell_bytes` writable).
 * @param[in]  cell_bytes Cell capacity in bytes.
 * @param[out] user       Per-cell user descriptor (`user_bytes`), or NULL when
 *                        the cache was configured with `user_bytes == 0`.
 *
 * @return ra8_err_t k_ra8_ok on success; any error aborts the get with that code
 *         and leaves the victim cell cold (no stale entry survives).
 *
 * @since 0.1.0
 */
typedef ra8_err_t (*ra8_keycache_render_fn)(void*       ctx,
                                            const void* key,
                                            uint8_t*    cell,
                                            uint32_t    cell_bytes,
                                            void*       user);

/**
 * @struct ra8_keycache_cell_t
 * @brief Per-cell link metadata (one caller-owned array entry per cell).
 *
 * @details Treat as private; the cache owns the contents. Allocate an array of
 *          `cell_count` of these alongside the cell, key, and bucket storage.
 *
 * @since 0.1.0
 */
/* cppcheck-suppress-begin [unusedStructMember] */
typedef struct {
  int32_t  prev;      /**< LRU link toward MRU, or -1.        */
  int32_t  next;      /**< LRU link toward LRU, or -1.        */
  int32_t  hash_next; /**< Hash bucket chain link, or -1.     */
  uint16_t pin_count; /**< Outstanding pins (0 => evictable). */
  uint8_t  valid;     /**< 1 => this cell holds an entry.     */
} ra8_keycache_cell_t;
/* cppcheck-suppress-end [unusedStructMember] */

/**
 * @struct ra8_keycache_cfg_t
 * @brief Caller-supplied storage + renderer for ::ra8_keycache_init.
 *
 * @details Every array is caller-owned and must out-live the cache. `user_mem`
 *          is required only when `user_bytes > 0`; pass NULL with `user_bytes ==
 *          0` for a cache that needs no per-cell descriptor.
 *
 * @since 0.1.0
 */
typedef struct {
  uint8_t*               cell_mem;     /**< `cell_count * cell_bytes` of cell storage.  */
  uint32_t               cell_bytes;   /**< Bytes per cell (the rendered payload).      */
  uint32_t               cell_count;   /**< Number of cells.                            */
  uint8_t*               key_mem;      /**< `cell_count * key_bytes` of key storage.    */
  uint32_t               key_bytes;    /**< Bytes per key (>= 1).                       */
  uint8_t*               user_mem;     /**< `cell_count * user_bytes`, or NULL if none. */
  uint32_t               user_bytes;   /**< Bytes per user descriptor (may be 0).       */
  ra8_keycache_cell_t*   meta;         /**< `cell_count` link-metadata entries.         */
  int32_t*               buckets;      /**< `bucket_count` hash-bucket heads.           */
  uint32_t               bucket_count; /**< Number of hash buckets (>= 1).              */
  ra8_keycache_render_fn render;       /**< Render-on-miss callback.                    */
  void*                  render_ctx;   /**< Opaque context passed to @c render.         */
} ra8_keycache_cfg_t;

/**
 * @struct ra8_keycache_t
 * @brief Keyed-LRU cache state (caller-owned; treat as private).
 *
 * @invariant Every valid cell is reachable from exactly one hash bucket.
 * @invariant Every cell is a member of the single LRU list.
 *
 * @since 0.1.0
 */
/* cppcheck-suppress-begin [unusedStructMember] */
typedef struct {
  ra8_keycache_cfg_t cfg;       /**< Configuration (copied at init). */
  int32_t            lru_head;  /**< MRU cell, or -1.                */
  int32_t            lru_tail;  /**< LRU cell, or -1.                */
  uint32_t           hits;      /**< Get hits so far.                */
  uint32_t           misses;    /**< Get misses so far.              */
  uint32_t           evictions; /**< Entries evicted so far.         */
} ra8_keycache_t;
/* cppcheck-suppress-end [unusedStructMember] */

/**
 * @struct ra8_keycache_view_t
 * @brief A pinned view of a cached cell returned by ::ra8_keycache_get.
 *
 * @details `data` points into the cell storage and stays valid until the
 *          matching ::ra8_keycache_put. `user` points at this cell's user
 *          descriptor (or is NULL when `user_bytes == 0`) and has the **same**
 *          lifetime as `data`: both are tied to the cell's pin, so once it is
 *          unpinned the cell may be evicted and the descriptor storage reused.
 *          Copy out any descriptor fields before ::ra8_keycache_put.
 *
 * @since 0.1.0
 */
typedef struct {
  uint8_t* data; /**< Cell payload (`cell_bytes` wide).  */
  void*    user; /**< Per-cell user descriptor, or NULL. */
} ra8_keycache_view_t;

/**
 * @brief Initialise a keyed-LRU cache over caller-supplied storage.
 *
 * @param[out] kc  Cache state to populate (zero-initialised by the caller).
 * @param[in]  cfg Storage + renderer configuration (see ::ra8_keycache_cfg_t).
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok               Cache ready; all cells cold.
 * @retval k_ra8_err_null_ptr     `kc`, `cfg`, or a required `cfg` pointer is NULL.
 * @retval k_ra8_err_invalid_size `cell_count`, `cell_bytes`, `key_bytes`, or
 *                               `bucket_count` was zero.
 *
 * @pre `cfg`'s arrays cover their declared sizes and out-live the cache.
 * @pre `cfg->render` is non-NULL, and `cfg->user_mem` is non-NULL when
 *      `cfg->user_bytes > 0`.
 * @post On success the cache is empty and the buckets are cleared.
 * @post On any non-ok return `kc` is left unbound.
 *
 * @note Not thread-safe.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_keycache_init(ra8_keycache_t* kc, const ra8_keycache_cfg_t* cfg);

/**
 * @brief Get (and pin) the cell for @p key, rendering it on a miss.
 *
 * @details On a hit the cell is moved to the MRU and pinned. On a miss an
 *          unpinned LRU victim is evicted, the cell is filled through the
 *          configured render callback, inserted, and pinned. The returned view
 *          stays valid until ::ra8_keycache_put.
 *
 * @param[in]  kc       Initialised cache.
 * @param[in]  key      `key_bytes`-wide key to fetch (fully initialised).
 * @param[out] out_view Receives the pinned cell view.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok           Cell resident and pinned; `*out_view` set.
 * @retval k_ra8_err_null_ptr `kc`, `key`, or `out_view` was NULL.
 * @retval k_ra8_err_no_mem   Every cell is pinned (cannot evict for the miss).
 * @retval k_ra8_err_*        The render callback failed (returned verbatim).
 *
 * @pre `kc` was populated by ::ra8_keycache_init.
 * @pre The caller will ::ra8_keycache_put the returned cell.
 * @post On success the cell's pin count increased by one.
 * @post On any non-ok return no new pin is held.
 *
 * @note Not thread-safe.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_keycache_get(ra8_keycache_t* kc, const void* key, ra8_keycache_view_t* out_view);

/**
 * @brief Release one pin on a cell previously returned by ::ra8_keycache_get.
 *
 * @param[in] kc   Initialised cache.
 * @param[in] data The `data` pointer from a returned ::ra8_keycache_view_t.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok              Pin released.
 * @retval k_ra8_err_null_ptr    `kc` or `data` was NULL.
 * @retval k_ra8_err_invalid_arg `data` is not a cell of this cache, or the cell
 *                              was not pinned.
 *
 * @pre `data` came from ::ra8_keycache_get on this cache and is still pinned.
 * @pre `kc` was populated by ::ra8_keycache_init.
 * @post On success the cell's pin count decreased by one.
 * @post On any non-ok return no state changed.
 *
 * @note Not thread-safe.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_keycache_put(ra8_keycache_t* kc, const uint8_t* data);

/**
 * @brief Report the cache hit / miss / eviction counters.
 *
 * @param[in]  kc            Initialised cache.
 * @param[out] out_hits      Hits so far (may be NULL).
 * @param[out] out_misses    Misses so far (may be NULL).
 * @param[out] out_evictions Evictions so far (may be NULL).
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok              Counters reported.
 * @retval k_ra8_err_null_ptr    `kc` was NULL.
 * @retval k_ra8_err_invalid_state The cache was not initialised.
 *
 * @pre `kc` was populated by ::ra8_keycache_init.
 * @pre At least one output pointer is non-NULL to be useful.
 * @post On success the requested counters are written.
 * @post No cache state is mutated.
 *
 * @note Thread-safe with respect to a quiescent cache (pure read).
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_keycache_stats(const ra8_keycache_t* kc,
                                           uint32_t*             out_hits,
                                           uint32_t*             out_misses,
                                           uint32_t*             out_evictions);

#ifdef __cplusplus
}
#endif
