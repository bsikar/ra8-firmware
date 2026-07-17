/**
 * @file ra8_io_blockdev_cache.h
 * @brief ra8_io caching block device -- an LRU sector cache over any backend.
 * @ingroup grp_io
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details
 * A decorator block device: it wraps another ::ra8_io_blockdev_t and keeps a
 * fixed set of recently-used 512-byte sectors in a caller-owned cache, so
 * repeated reads of the same blocks (filesystem metadata, a re-read EPUB page,
 * a glyph atlas) skip the slow medium. Reads are served from the cache on a hit
 * and fill it on a miss; writes are **write-through** (committed to the backend
 * immediately and reflected in the cache). Eviction is least-recently-used.
 *
 * This is the cache layer between the filesystem/VFS and the media. It composes
 * with the unified page-cache design of issue #147 (and the SLRU policy chosen
 * by `tools/cache_bench`); this first cut uses straightforward write-through LRU
 * and exposes hit/miss counters for observability.
 *
 * Zero allocation: the caller provides the sector data buffer and the slot
 * metadata array, sized to the chosen number of cached sectors.
 *
 * @code
 * static uint8_t                     s_cache_data[32U * 512U];
 * static ra8_io_blockdev_cache_slot_t s_cache_slots[32U];
 * static ra8_io_blockdev_cache_state_t s_cache_state;
 * ra8_io_blockdev_t cached = {};
 * (void)ra8_io_blockdev_cache_init(&cached, &s_cache_state, &sd_bd,
 *                                 s_cache_data, s_cache_slots, 32U);
 * // `cached` now behaves like sd_bd but caches 32 sectors.
 * @endcode
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
#include "ra8_io_blockdev.h"

/**
 * @struct ra8_io_blockdev_cache_slot_t
 * @brief Per-sector cache metadata (one caller-owned array entry per slot).
 *
 * @details Treat as private; the cache owns the contents. Allocate an array of
 *          `n_slots` of these alongside an `n_slots * 512`-byte data buffer.
 *
 * @since 0.1.0
 */
typedef struct {
  uint32_t lba;      /**< Cached logical block address (valid only if in use). */
  uint32_t last_use; /**< LRU stamp; larger == more recently used.             */
  bool     valid;    /**< true => this slot holds a cached sector.             */
} ra8_io_blockdev_cache_slot_t;

/**
 * @struct ra8_io_blockdev_cache_state_t
 * @brief Caller-owned private state for a caching block device.
 *
 * @details Zero-initialise and pass to ::ra8_io_blockdev_cache_init. The
 *          underlying device, data buffer, and slot array must out-live it.
 *
 * @invariant `clock` increases monotonically across accesses.
 *
 * @since 0.1.0
 */
typedef struct {
  const ra8_io_blockdev_t*      under;   /**< Wrapped backend (private).    */
  uint8_t*                      data;    /**< `n_slots * 512` cache bytes.  */
  ra8_io_blockdev_cache_slot_t* slots;   /**< `n_slots` metadata entries.   */
  uint32_t                      n_slots; /**< Number of cached sectors.     */
  uint32_t                      clock;   /**< Monotonic LRU access counter. */
  uint32_t                      hits;    /**< Read cache hits so far.       */
  uint32_t                      misses;  /**< Read cache misses so far.     */
} ra8_io_blockdev_cache_state_t;

/**
 * @brief Bind a caching block device over an existing backend.
 *
 * @param[out] bd      Handle to bind (zero-initialised by the caller).
 * @param[out] state   Caller-owned cache state to populate.
 * @param[in]  under   Backend to wrap (must out-live the cache).
 * @param[in]  data    Cache data buffer of `n_slots * 512` bytes.
 * @param[in]  slots   Cache metadata array of `n_slots` entries.
 * @param[in]  n_slots Number of cached sectors (>= 1).
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok               Cache bound; `bd` is usable.
 * @retval k_ra8_err_null_ptr     `bd`, `state`, `under`, `data`, or `slots` NULL.
 * @retval k_ra8_err_invalid_size `n_slots` was zero.
 *
 * @pre `data` covers `n_slots * 512` bytes and `slots` holds `n_slots` entries.
 * @pre `under`, `bd`, `state`, `data`, `slots` out-live every cached access.
 * @post On success `bd` reads/writes through an LRU cache over `under`.
 * @post On any non-ok return `bd` and `state` are left unbound/untouched.
 *
 * @note Not thread-safe with respect to the same cache.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_io_blockdev_cache_init(ra8_io_blockdev_t*             bd,
                                                   ra8_io_blockdev_cache_state_t* state,
                                                   const ra8_io_blockdev_t*       under,
                                                   uint8_t*                       data,
                                                   ra8_io_blockdev_cache_slot_t*  slots,
                                                   uint32_t                       n_slots);

/**
 * @brief Report the cache hit/miss counters.
 *
 * @param[in]  state      Bound cache state.
 * @param[out] out_hits   Read hits so far (may be NULL).
 * @param[out] out_misses Read misses so far (may be NULL).
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok           Counters reported.
 * @retval k_ra8_err_null_ptr `state` was NULL.
 *
 * @pre `state` was populated by ::ra8_io_blockdev_cache_init.
 * @pre At least one of the output pointers is non-NULL to be useful.
 * @post On success the requested counters are written.
 * @post No state is mutated.
 *
 * @note Thread-safe (pure read).
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_io_blockdev_cache_stats(const ra8_io_blockdev_cache_state_t* state,
                                                    uint32_t*                            out_hits,
                                                    uint32_t* out_misses);

#ifdef __cplusplus
}
#endif
