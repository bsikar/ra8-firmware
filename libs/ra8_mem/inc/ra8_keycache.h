/**
 * @file ra8_keycache.h
 * @brief The one reusable hash + pin + evict cache engine (#147, #345).
 * @ingroup grp_ereader
 *
 * @par Tag
 * [Ring 2 / Core] {World: NS}
 *
 * @details
 * The single hash + pin + evict engine every reader cache is built on. #345
 * folded ::ra8_vmem's once-duplicate SLRU/hash/pin machinery into this one
 * implementation, so the eviction policy is now a config choice rather than a
 * second copy of the code. A keycache maps a fixed-size opaque **key** to a
 * fixed-size opaque **cell** of filled bytes: a hit returns a pinned view of the
 * cell; a miss evicts an unpinned victim, fills the cell through a
 * caller-supplied render-on-miss callback, inserts it, and pins it. The caller
 * releases the pin with ::ra8_keycache_put once it is done reading the cell.
 *
 * ## Selectable eviction policy (`cfg.evict`)
 *
 * The victim is always an unpinned cell (pinned cells are skipped); which one
 * depends on the policy the caller selects:
 *
 *  - **LRU (`k_ra8_keycache_evict_lru`, the default).** A single recency list:
 *    the victim is the least-recently-used unpinned cell. Right for caches with
 *    strong per-entry reuse locality and no linear-scan floods -- the glyph
 *    atlas (::ra8_glyph_atlas) and the image-tile cache (::ra8_tile_cache),
 *    whose on-screen working set is re-touched every frame.
 *  - **SLRU / 2Q (`k_ra8_keycache_evict_slru`).** Two segments -- a
 *    probationary scan absorber and a protected hot set (`cfg.protected_pct`,
 *    0 selects 75%). Cold cells enter probation; a re-reference promotes to
 *    protected (demoting the protected LRU back to probation when it is full);
 *    the victim is the probationary LRU first, then the protected LRU. This is
 *    the only scan-resistant policy with deterministic O(1) (WCET = 1) victim
 *    selection: a page-turn flood ages out in probation without evicting the hot
 *    set. The byte-range page cache (::ra8_vmem) selects it, so a linear file
 *    scan does not thrash re-read metadata.
 *
 * The policy changes *which* cell is evicted, never *how many* are resident: the
 * bounded-RAM guarantee (residency <= `cell_count`) holds at every setting.
 *
 * ## Keys and hashing
 *
 * Keys are compared byte-wise (`memcmp` over `key_bytes`), so a key struct must
 * be fully initialised (zero-filled, no indeterminate padding) before use. The
 * hash is **injectable** (`cfg.hash`, `cfg.hash_ctx`); a NULL hash selects the
 * built-in FNV-1a over the key bytes. A facade whose key carries structure
 * (::ra8_vmem hashes on the derived page number) supplies its own; the folding
 * to `[0, bucket_count)` is always the engine's, so the hash callback returns a
 * raw 32-bit value.
 *
 * ## Per-cell user descriptor (`user_bytes`, may be 0)
 *
 * The render callback writes per-entry metadata there (e.g. the rendered glyph
 * or tile width/height) and ::ra8_keycache_get hands the descriptor back
 * alongside the cell data. This is how a typed facade (::ra8_glyph_atlas,
 * ::ra8_tile_cache) recovers its dimensions without a parallel side table.
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
 * @enum ra8_keycache_evict_t
 * @brief Eviction policy selected in ::ra8_keycache_cfg_t::evict.
 *
 * @details Chooses how the engine orders victims. LRU keeps a single recency
 *          list; SLRU keeps a probationary + protected pair for scan resistance.
 *          Both skip pinned cells and both guarantee residency <= `cell_count`.
 *
 * @invariant The two policies share every other config field; only the on-access
 *            promotion and the victim scan differ.
 *
 * @see ra8_keycache_cfg_t::protected_pct  Sizes the SLRU protected segment.
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_ra8_keycache_evict_lru  = 0U, /**< Single-list LRU (the default when 0). */
  k_ra8_keycache_evict_slru = 1U, /**< Segmented LRU / 2Q (scan-resistant).  */
} ra8_keycache_evict_t;

/**
 * @typedef ra8_keycache_hash_fn
 * @brief Hash a key blob to a raw 32-bit value (the engine folds to a bucket).
 *
 * @details Called on every lookup, insert, and evict to place a key in a hash
 *          bucket. Returns a raw 32-bit hash; the engine reduces it to
 *          `[0, bucket_count)`. A NULL callback in the config selects the
 *          built-in FNV-1a over the `key_bytes`. Must be a pure function of the
 *          key bytes (and any immutable `ctx`) so a stored key rehashes to the
 *          same bucket it was inserted under.
 *
 * @param[in] key       The `key_bytes`-wide key blob to hash.
 * @param[in] key_bytes Key width in bytes.
 * @param[in] ctx       Opaque context (`hash_ctx` from the config), or NULL.
 *
 * @return A raw 32-bit hash of the key.
 *
 * @since 0.1.0
 */
typedef uint32_t (*ra8_keycache_hash_fn)(const void* key, uint32_t key_bytes, void* ctx);

/**
 * @typedef ra8_keycache_render_fn
 * @brief Fill a cell with the rendered bytes for @p key (render-on-miss seam).
 *
 * @details Called on a cache miss to populate @p cell with the content keyed by
 *          @p key, and (when the cache has a user descriptor) to write that
 *          entry's metadata into @p user. The glyph atlas rasterises a glyph
 *          here; the tile cache decodes an image tile; ::ra8_vmem loads a page;
 *          tests use a synthetic generator.
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
 *          The @ref ra8_keycache_cell_t::seg tag is only meaningful under the
 *          SLRU policy; under LRU it stays at the probationary value and is
 *          never consulted.
 *
 * @since 0.1.0
 */
typedef struct {
  int32_t  prev;      /**< Recency link toward MRU within the segment, or -1. */
  int32_t  next;      /**< Recency link toward LRU within the segment, or -1. */
  int32_t  hash_next; /**< Hash bucket chain link, or -1.                     */
  uint16_t pin_count; /**< Outstanding pins (0 => evictable).                 */
  uint8_t  seg;       /**< SLRU segment tag (probationary / protected).       */
  uint8_t  valid;     /**< 1 => this cell holds an entry.                     */
} ra8_keycache_cell_t;

/**
 * @struct ra8_keycache_cfg_t
 * @brief Caller-supplied storage + policy + renderer for ::ra8_keycache_init.
 *
 * @details Every array is caller-owned and must out-live the cache. `user_mem`
 *          is required only when `user_bytes > 0`; pass NULL with `user_bytes ==
 *          0` for a cache that needs no per-cell descriptor. `evict`, `hash`,
 *          and `protected_pct` all default to their zero value (LRU, built-in
 *          FNV-1a, and the SLRU 75% default respectively), so an LRU cache need
 *          only zero-fill the struct and set the storage fields.
 *
 * @since 0.1.0
 */
typedef struct {
  uint8_t*               cell_mem;      /**< `cell_count * cell_bytes` of cell storage.  */
  uint32_t               cell_bytes;    /**< Bytes per cell (the rendered payload).      */
  uint32_t               cell_count;    /**< Number of cells.                            */
  uint8_t*               key_mem;       /**< `cell_count * key_bytes` of key storage.    */
  uint32_t               key_bytes;     /**< Bytes per key (>= 1).                       */
  uint8_t*               user_mem;      /**< `cell_count * user_bytes`, or NULL if none. */
  uint32_t               user_bytes;    /**< Bytes per user descriptor (may be 0).       */
  ra8_keycache_cell_t*   meta;          /**< `cell_count` link-metadata entries.         */
  int32_t*               buckets;       /**< `bucket_count` hash-bucket heads.           */
  uint32_t               bucket_count;  /**< Number of hash buckets (>= 1).              */
  ra8_keycache_render_fn render;        /**< Render-on-miss callback.                    */
  void*                  render_ctx;    /**< Opaque context passed to @c render.         */
  ra8_keycache_evict_t   evict;         /**< Eviction policy (0 => LRU; SLRU opt-in).    */
  uint8_t                protected_pct; /**< SLRU protected share 1..100; 0 => 75%.
                                         *   Ignored under LRU.                          */
  ra8_keycache_hash_fn   hash;          /**< Key hash; NULL selects built-in FNV-1a. */
  void*                  hash_ctx;      /**< Opaque context passed to @c hash.       */
} ra8_keycache_cfg_t;

/**
 * @struct ra8_keycache_t
 * @brief Cache engine state (caller-owned; treat as private).
 *
 * @details Under LRU only the probationary list is used (it is the single
 *          recency list) and the protected segment stays empty; under SLRU both
 *          segments are live. `protected_cap` is 0 under LRU.
 *
 * @invariant Every valid cell is reachable from exactly one hash bucket.
 * @invariant Every cell is a member of exactly one segment list.
 * @invariant `protected_count <= protected_cap <= cell_count`.
 *
 * @since 0.1.0
 */
typedef struct {
  ra8_keycache_cfg_t cfg;             /**< Configuration (copied at init).        */
  int32_t            pb_head;         /**< Probationary MRU cell, or -1.          */
  int32_t            pb_tail;         /**< Probationary LRU cell, or -1.          */
  int32_t            pt_head;         /**< Protected MRU cell, or -1 (SLRU only). */
  int32_t            pt_tail;         /**< Protected LRU cell, or -1 (SLRU only). */
  uint32_t           protected_count; /**< Cells in the protected segment.        */
  uint32_t           protected_cap;   /**< Protected-segment capacity (0 = LRU).  */
  uint32_t           hits;            /**< Get hits so far.                       */
  uint32_t           misses;          /**< Get misses so far.                     */
  uint32_t           evictions;       /**< Entries evicted so far.                */
} ra8_keycache_t;

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
 * @brief Initialise a cache engine over caller-supplied storage.
 *
 * @param[out] kc  Cache state to populate (zero-initialised by the caller).
 * @param[in]  cfg Storage + policy + renderer configuration (see
 *                 ::ra8_keycache_cfg_t).
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok               Cache ready; all cells cold.
 * @retval k_ra8_err_null_ptr     `kc`, `cfg`, or a required `cfg` pointer is NULL.
 * @retval k_ra8_err_invalid_size `cell_count`, `cell_bytes`, `key_bytes`, or
 *                               `bucket_count` was zero.
 * @retval k_ra8_err_invalid_arg  `cfg->protected_pct` exceeds 100 (SLRU).
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
 * @details On a hit the cell is re-referenced (LRU: moved to the MRU; SLRU:
 *          promoted toward the protected segment) and pinned. On a miss an
 *          unpinned victim is evicted (LRU: the LRU cell; SLRU: the probationary
 *          LRU first, then the protected LRU), the cell is filled through the
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
 * @brief Warm the cell for @p key into the cache without holding a pin.
 *
 * @details The read-ahead / prefetch primitive: a ::ra8_keycache_get immediately
 *          followed by a ::ra8_keycache_put, so on return the cell is resident but
 *          unpinned (evictable). A hit is a no-op refresh; a miss renders the cell
 *          through the configured callback and inserts it. Warming changes only
 *          residency -- never the bytes a later ::ra8_keycache_get returns -- so it
 *          is transparent to the caller. This is the image-tile analogue of
 *          ::ra8_vmem_prefetch (which warms a page-cache frame). The cell is
 *          inserted at the MRU (single-list LRU), so a wrong read-ahead guess can
 *          age out hot data before itself; the scan-resistant probationary insert
 *          is tracked by the cache-consolidation work (#345).
 *
 * @param[in,out] kc  Initialised cache.
 * @param[in]     key `key_bytes`-wide key to warm (fully initialised).
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok           The cell is resident and unpinned (warmed or hit).
 * @retval k_ra8_err_null_ptr `kc` or `key` was NULL.
 * @retval k_ra8_err_no_mem   Every cell is pinned (cannot evict to warm).
 * @retval k_ra8_err_*        The render callback failed (returned verbatim).
 *
 * @pre `kc` was populated by ::ra8_keycache_init.
 * @pre `key` is fully initialised (no indeterminate padding bytes).
 * @post On success the entry is resident with pin count zero.
 * @post On any non-ok return no pin is held and no entry was warmed.
 *
 * @note Not thread-safe. Single-threaded read-ahead only.
 * @note A warmed-but-unused cell is evicted before any pinned cell.
 *
 * @see ra8_keycache_get()
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_keycache_prefetch(ra8_keycache_t* kc, const void* key);

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
