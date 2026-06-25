/**
 * @file ra_vmem.h
 * @brief Unified page cache with SLRU eviction (Layer 2, #147).
 *
 * @par Tag
 * [Ring 2 / Core] {World: NS}
 *
 * @details
 * The heart of the #147 memory hierarchy: a software page cache that lets the
 * reader touch objects far larger than RAM (GB-class EPUB/CBZ) while the
 * resident set stays bounded by a fixed frame budget, independent of file size.
 * There is no MMU, so access is **handle-based** -- `ra_vmem_get(object_id,
 * offset)` returns a pointer to a pinned frame holding that page; a miss evicts
 * an unpinned victim, loads the page through a caller-supplied loader, and
 * inserts it. The caller `ra_vmem_put`s the frame when done.
 *
 * Eviction is **SLRU (Segmented LRU / 2Q)** -- the policy chosen in #147 because
 * it is the only scan-resistant policy with deterministic O(1) (WCET = 1)
 * victim selection: a probationary segment absorbs one-shot linear scans while a
 * protected segment holds the re-referenced working set, so a page-turn flood
 * does not evict the hot set. The victim is always the probationary LRU (then
 * the protected LRU); pinned frames are skipped.
 *
 * Zero allocation (NASA P10 Rule 3): the caller provides the frame storage, the
 * per-frame metadata array, and the hash-bucket array -- all carved once from a
 * tier arena (::ra_arena) at init. The frames themselves come from a tier's
 * ::ra_slab in production, but `ra_vmem` only needs a contiguous frame region.
 *
 * @note Not thread-safe; the reader is single-threaded (or serialises access).
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
 * @typedef ra_vmem_loader_fn
 * @brief Fill a frame with one page of an object (the storage DIP seam).
 *
 * @details Called on a cache miss to populate @p frame with `frame_bytes` of
 *          object @p object_id starting at byte @p offset (a frame-aligned
 *          offset). Layer 1 (SD-paged / OSPI-XIP backends) implements this; tests
 *          use a synthetic generator.
 *
 * @param[in]  ctx         Opaque loader context (`loader_ctx` from the config).
 * @param[in]  object_id   Object being paged in.
 * @param[in]  offset      Frame-aligned byte offset within the object.
 * @param[out] frame       Destination frame (`frame_bytes` writable).
 * @param[in]  frame_bytes Frame size in bytes.
 *
 * @return ra_err_t k_ra_ok on success; any error aborts the get with that code.
 *
 * @since 0.1.0
 */
typedef ra_err_t (*ra_vmem_loader_fn)(void*    ctx,
                                      uint32_t object_id,
                                      uint64_t offset,
                                      uint8_t* frame,
                                      uint32_t frame_bytes);

/**
 * @struct ra_vmem_frame_t
 * @brief Per-frame metadata (one caller-owned array entry per frame).
 *
 * @details Treat as private; the cache owns the contents. Allocate an array of
 *          `frame_count` of these alongside the frame storage and hash buckets.
 *
 * @since 0.1.0
 */
/* cppcheck-suppress-begin [unusedStructMember] */
typedef struct {
  uint32_t object_id; /**< Cached object id (valid only when in use).        */
  uint64_t offset;    /**< Frame-aligned byte offset of the cached page.     */
  int32_t  prev;      /**< SLRU list link toward MRU within the segment, or -1. */
  int32_t  next;      /**< SLRU list link toward LRU within the segment, or -1. */
  int32_t  hash_next; /**< Next frame in the hash bucket chain, or -1.        */
  uint16_t pin_count; /**< Outstanding ::ra_vmem_get pins (0 => evictable).   */
  uint8_t  seg;       /**< SLRU segment tag (probationary / protected).       */
  uint8_t  valid;     /**< 1 => this frame holds a cached page.               */
} ra_vmem_frame_t;
/* cppcheck-suppress-end [unusedStructMember] */

/**
 * @struct ra_vmem_cfg_t
 * @brief Caller-supplied storage + loader for ::ra_vmem_init.
 *
 * @details All arrays are caller-owned and must out-live the cache. Carve them
 *          once from a tier arena at bring-up (frame storage from SDRAM, the
 *          metadata + buckets from DTCM for the hot path).
 *
 * @since 0.1.0
 */
typedef struct {
  uint8_t*          frame_mem;    /**< `frame_count * frame_bytes` of page storage. */
  uint32_t          frame_bytes;  /**< Bytes per frame (page size, e.g. 4096).      */
  uint32_t          frame_count;  /**< Number of frames.                            */
  ra_vmem_frame_t*  meta;         /**< `frame_count` metadata entries.              */
  int32_t*          buckets;      /**< `bucket_count` hash-bucket heads.            */
  uint32_t          bucket_count; /**< Number of hash buckets (>= 1).               */
  ra_vmem_loader_fn loader;       /**< Page-fill callback (the storage DIP seam).   */
  void*             loader_ctx;   /**< Opaque context passed to @c loader.          */
} ra_vmem_cfg_t;

/**
 * @struct ra_vmem_t
 * @brief Page-cache state (caller-owned; treat as private).
 *
 * @invariant `protected_count <= protected_cap <= frame_count`.
 * @invariant Every valid frame is reachable from exactly one hash bucket.
 *
 * @since 0.1.0
 */
/* cppcheck-suppress-begin [unusedStructMember] */
typedef struct {
  ra_vmem_cfg_t cfg;             /**< The configuration (copied at init).      */
  int32_t       pb_head;         /**< Probationary MRU frame, or -1.           */
  int32_t       pb_tail;         /**< Probationary LRU frame, or -1.           */
  int32_t       pt_head;         /**< Protected MRU frame, or -1.              */
  int32_t       pt_tail;         /**< Protected LRU frame, or -1.              */
  uint32_t      protected_count; /**< Frames in the protected segment.         */
  uint32_t      protected_cap;   /**< Protected-segment capacity.              */
  uint32_t      free_count;      /**< Frames never yet used (cold).            */
  uint32_t      hits;            /**< Get hits so far.                         */
  uint32_t      misses;          /**< Get misses so far.                       */
  uint32_t      evictions;       /**< Pages evicted so far.                    */
} ra_vmem_t;
/* cppcheck-suppress-end [unusedStructMember] */

/**
 * @brief Initialise a page cache over caller-supplied storage.
 *
 * @param[out] vm  Cache state to populate (zero-initialised by the caller).
 * @param[in]  cfg Storage + loader configuration (see ::ra_vmem_cfg_t).
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok               Cache ready; all frames cold/free.
 * @retval k_ra_err_null_ptr     `vm`, `cfg`, or a required `cfg` pointer is NULL.
 * @retval k_ra_err_invalid_size `frame_count`, `frame_bytes`, or `bucket_count`
 *                               was zero.
 *
 * @pre `cfg`'s arrays cover their declared sizes and out-live the cache.
 * @pre `cfg->loader` is non-NULL.
 * @post On success `vm` is empty (0 valid frames) and the buckets are cleared.
 * @post On any non-ok return `vm` is left unbound.
 *
 * @note Not thread-safe.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_vmem_init(ra_vmem_t* vm, const ra_vmem_cfg_t* cfg);

/**
 * @brief Get (and pin) the frame holding object @p object_id at @p offset.
 *
 * @details On a hit the frame is re-referenced (SLRU promote) and pinned. On a
 *          miss an unpinned victim is evicted (SLRU: probationary LRU first), the
 *          page is loaded through the configured loader, inserted, and pinned.
 *          The returned pointer stays valid until the matching ::ra_vmem_put.
 *
 * @param[in]  vm        Initialised cache.
 * @param[in]  object_id Object to page in.
 * @param[in]  offset    Byte offset; rounded down to a frame boundary internally.
 * @param[out] out_page  Receives the pinned frame pointer (`frame_bytes` wide).
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok           Page resident and pinned; `*out_page` set.
 * @retval k_ra_err_null_ptr `vm` or `out_page` was NULL.
 * @retval k_ra_err_no_mem   Every frame is pinned (cannot evict for the miss).
 * @retval k_ra_err_*        The loader failed (returned verbatim).
 *
 * @pre `vm` was populated by ::ra_vmem_init.
 * @pre The caller will ::ra_vmem_put the returned frame.
 * @post On success the frame's `pin_count` increased by one.
 * @post On any non-ok return no new pin is held.
 *
 * @note Not thread-safe.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t
ra_vmem_get(ra_vmem_t* vm, uint32_t object_id, uint64_t offset, void** out_page);

/**
 * @brief Release one pin on a frame previously returned by ::ra_vmem_get.
 *
 * @param[in] vm   Initialised cache.
 * @param[in] page A frame pointer returned by ::ra_vmem_get.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok              Pin released.
 * @retval k_ra_err_null_ptr    `vm` or `page` was NULL.
 * @retval k_ra_err_invalid_arg `page` is not a frame of this cache, or the frame
 *                              was not pinned.
 *
 * @pre `page` came from ::ra_vmem_get on this cache and is still pinned.
 * @pre `vm` was populated by ::ra_vmem_init.
 * @post On success the frame's `pin_count` decreased by one (evictable at zero).
 * @post On any non-ok return no state changed.
 *
 * @note Not thread-safe.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_vmem_put(ra_vmem_t* vm, void* page);

/**
 * @brief Report the cache hit / miss / eviction counters.
 *
 * @param[in]  vm            Initialised cache.
 * @param[out] out_hits      Get hits so far (may be NULL).
 * @param[out] out_misses    Get misses so far (may be NULL).
 * @param[out] out_evictions Pages evicted so far (may be NULL).
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok           Counters reported.
 * @retval k_ra_err_null_ptr `vm` was NULL.
 *
 * @pre `vm` was populated by ::ra_vmem_init.
 * @pre At least one output pointer is non-NULL to be useful.
 * @post On success the requested counters are written.
 * @post No cache state is mutated.
 *
 * @note Thread-safe with respect to a quiescent cache (pure read).
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_vmem_stats(const ra_vmem_t* vm,
                                     uint32_t*        out_hits,
                                     uint32_t*        out_misses,
                                     uint32_t*        out_evictions);

#ifdef __cplusplus
}
#endif
