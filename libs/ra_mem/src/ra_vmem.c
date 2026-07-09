/**
 * @file ra_vmem.c
 * @brief Unified page cache with SLRU eviction -- implementation (Layer 2, #147).
 *
 * @par Tag
 * [Ring 2 / Core] {World: NS}
 *
 * @details
 * All frames start invalid in the probationary segment, so the same victim path
 * serves both cold misses (an invalid LRU frame) and real evictions (a valid LRU
 * frame). The SLRU lists and the hash bucket chains are threaded through the
 * caller-owned ::ra_vmem_frame_t array (prev/next, hash_next); nothing is
 * allocated after init. Victim selection is read-only and skips pinned frames;
 * the get flow performs the eviction (hash remove + unlink) only when it commits.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_vmem.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ra_check.h"
#include "ra_err.h"

/** @brief Module log tag. */
static const char* const s_tag = "ra_vmem";

/** @brief Internal sizing / policy constants. */
typedef enum : uint32_t {
  k_vmem_nil_idx       = 0xFFFFFFFFU, /**< "no frame" sentinel for list/hash links. */
  k_vmem_protected_pct = 75U,         /**< Protected-segment share of the cache.    */
  k_vmem_percent_full  = 100U,        /**< Percent denominator.                     */
  k_vmem_hash_mul_obj  = 2654435761U, /**< Knuth multiplicative hash (object id).   */
  k_vmem_hash_mul_page = 40503U,      /**< Odd multiplier mixing the page number.   */
} ra_vmem_const_t;

/** @brief SLRU segment tags stored in ::ra_vmem_frame_t::seg. */
typedef enum : uint8_t {
  k_vmem_seg_probation = 0U, /**< Probationary segment (scan absorber). */
  k_vmem_seg_protected = 1U, /**< Protected segment (hot working set).  */
} ra_vmem_seg_t;

/** @brief Pointer to frame @p idx's storage. */
static uint8_t* priv_frame_ptr(const ra_vmem_t* vm, uint32_t idx)
{
  return &vm->cfg.frame_mem[(size_t)idx * (size_t)vm->cfg.frame_bytes];
}

/**
 * @brief Detach frame @p f from the SLRU list owned by @p head / @p tail.
 *
 * @details Splices @p f out of the doubly-linked list, fixing the neighbours and
 *          the head/tail pointers as needed.
 *
 * @param[in,out] vm   Cache providing the frame metadata array.
 * @param[in]     f    Frame index to unlink.
 * @param[in,out] head Segment MRU head pointer (-1 sentinel as int32 nil).
 * @param[in,out] tail Segment LRU tail pointer.
 *
 * @return Nothing.
 *
 * @pre `f` is currently a member of the @p head / @p tail list.
 * @pre `vm`, `head`, `tail` are non-NULL.
 * @post `f`'s neighbours and the head/tail no longer reference `f`.
 * @post Other frames are unmodified.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void priv_unlink(ra_vmem_t* vm, int32_t f, int32_t* head, int32_t* tail)
{
  ra_vmem_frame_t* m = vm->cfg.meta;
  if (m[f].prev != -1) {
    m[m[f].prev].next = m[f].next;
  } else if (*head == f) {
    *head = m[f].next;
  }
  if (m[f].next != -1) {
    m[m[f].next].prev = m[f].prev;
  } else if (*tail == f) {
    *tail = m[f].prev;
  }
}

/**
 * @brief Push frame @p f onto the MRU head of an SLRU list.
 *
 * @details Links @p f in as the new most-recently-used entry, updating the tail
 *          when the list was empty.
 *
 * @param[in,out] vm   Cache providing the frame metadata array.
 * @param[in]     f    Frame index to insert (must be detached).
 * @param[in,out] head Segment MRU head pointer.
 * @param[in,out] tail Segment LRU tail pointer.
 *
 * @return Nothing.
 *
 * @pre `f` is not currently linked into any list.
 * @pre `vm`, `head`, `tail` are non-NULL.
 * @post `f` is the MRU head of the list.
 * @post The tail points at `f` iff the list was previously empty.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void priv_push_head(ra_vmem_t* vm, int32_t f, int32_t* head, int32_t* tail)
{
  ra_vmem_frame_t* m = vm->cfg.meta;
  m[f].prev          = -1;
  m[f].next          = *head;
  if (*head != -1) {
    m[*head].prev = f;
  }
  *head = f;
  if (*tail == -1) {
    *tail = f;
  }
}

/**
 * @brief Hash an (object_id, page) key into a bucket index.
 *
 * @details Derives the page number from @p aligned_off, mixes it with the object
 *          id through two odd multipliers, and folds to `[0, bucket_count)`.
 *
 * @param[in] vm          Cache providing the bucket count + frame size.
 * @param[in] object_id   Object id of the key.
 * @param[in] aligned_off Frame-aligned byte offset of the key.
 *
 * @return Bucket index in `[0, bucket_count)`.
 * @retval 0 The key folded into the first bucket (one possible result).
 *
 * @pre `vm` is non-NULL with `bucket_count > 0` and `frame_bytes > 0`.
 * @pre `aligned_off` is a multiple of `frame_bytes`.
 * @post No state is modified.
 * @post The result is strictly less than `bucket_count`.
 *
 * @note Pure; thread-safe.
 * @since 0.1.0
 */
static uint32_t priv_hash(const ra_vmem_t* vm, uint32_t object_id, uint64_t aligned_off)
{
  const uint32_t page = (uint32_t)(aligned_off / (uint64_t)vm->cfg.frame_bytes);
  const uint32_t mix =
    (object_id * (uint32_t)k_vmem_hash_mul_obj) ^ (page * (uint32_t)k_vmem_hash_mul_page);
  return mix % vm->cfg.bucket_count;
}

/**
 * @brief Insert frame @p f into its hash bucket chain.
 *
 * @details Prepends @p f to the chain of the bucket its key hashes to.
 *
 * @param[in,out] vm Cache providing buckets + metadata.
 * @param[in]     f  Frame index (its key fields are already set).
 *
 * @return Nothing.
 *
 * @pre `f`'s `object_id` / `offset` are set and it is not already chained.
 * @pre `vm` is non-NULL with a populated config.
 * @post `f` is the head of its bucket chain.
 * @post The bucket's previous head follows `f`.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void priv_hash_insert(ra_vmem_t* vm, int32_t f)
{
  const uint32_t b          = priv_hash(vm, vm->cfg.meta[f].object_id, vm->cfg.meta[f].offset);
  vm->cfg.meta[f].hash_next = vm->cfg.buckets[b];
  vm->cfg.buckets[b]        = f;
}

/**
 * @brief Remove frame @p f from its hash bucket chain.
 *
 * @details Unlinks @p f from its bucket, handling the head case and the
 *          mid-chain case via a bounded predecessor walk.
 *
 * @param[in,out] vm Cache providing buckets + metadata.
 * @param[in]     f  Frame index currently chained in its bucket.
 *
 * @return Nothing.
 *
 * @pre `f` is currently present in its bucket chain.
 * @pre `vm` is non-NULL with a populated config.
 * @post `f` is no longer reachable from the bucket.
 * @post `f`'s `hash_next` is reset to -1.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void priv_hash_remove(ra_vmem_t* vm, int32_t f)
{
  const uint32_t b   = priv_hash(vm, vm->cfg.meta[f].object_id, vm->cfg.meta[f].offset);
  int32_t        cur = vm->cfg.buckets[b];
  if (cur == f) {
    vm->cfg.buckets[b] = vm->cfg.meta[f].hash_next;
  } else {
    while (cur != -1) {
      if (vm->cfg.meta[cur].hash_next == f) {
        vm->cfg.meta[cur].hash_next = vm->cfg.meta[f].hash_next;
        break;
      }
      cur = vm->cfg.meta[cur].hash_next;
    }
  }
  vm->cfg.meta[f].hash_next = -1;
}

/**
 * @brief Test whether frame metadata @p m holds the valid key (object_id, off).
 *
 * @details Checks validity, then the object id, then the offset as three
 *          independent nested decisions (preserving the original MC/DC vectors).
 *          Extracted from ::priv_hash_lookup to keep the bucket-chain walk under
 *          the nesting threshold.
 *
 * @param[in] m           Frame metadata to test.
 * @param[in] object_id   Object id to match.
 * @param[in] aligned_off Frame-aligned offset to match.
 *
 * @return Whether @p m is a valid frame holding the key.
 * @retval true  The frame is valid and its key matches.
 * @retval false The frame is invalid or its key differs.
 *
 * @pre `m` is non-NULL.
 * @pre `aligned_off` is a multiple of `frame_bytes`.
 * @post No state is modified.
 * @post The result is true only for a valid, key-matching frame.
 *
 * @note Pure; thread-safe.
 * @since 0.1.0
 */
static bool priv_frame_matches(const ra_vmem_frame_t* m, uint32_t object_id, uint64_t aligned_off)
{
  if (m->valid != 0U) {
    if (m->object_id == object_id) {
      if (m->offset == aligned_off) {
        return true;
      }
    }
  }
  return false;
}

/**
 * @brief Find the valid frame holding (object_id, aligned_off), or -1.
 *
 * @details Walks the key's hash bucket chain, returning the first valid frame
 *          whose object id and offset match.
 *
 * @param[in] vm          Cache providing the buckets + metadata.
 * @param[in] object_id   Object id to find.
 * @param[in] aligned_off Frame-aligned offset to find.
 *
 * @return The matching frame index, or -1 if not resident.
 * @retval -1 No valid frame holds the key.
 *
 * @pre `vm` is non-NULL with a populated config.
 * @pre `aligned_off` is a multiple of `frame_bytes`.
 * @post No state is modified.
 * @post A non-negative result indexes a valid frame with the key.
 *
 * @note Not thread-safe with respect to concurrent mutation.
 * @since 0.1.0
 */
static int32_t priv_hash_lookup(const ra_vmem_t* vm, uint32_t object_id, uint64_t aligned_off)
{
  const uint32_t b   = priv_hash(vm, object_id, aligned_off);
  int32_t        cur = vm->cfg.buckets[b];
  while (cur != -1) {
    const ra_vmem_frame_t* m = &vm->cfg.meta[cur];
    if (priv_frame_matches(m, object_id, aligned_off)) {
      return cur;
    }
    cur = m->hash_next;
  }
  return -1;
}

/**
 * @brief SLRU re-reference: promote / refresh frame @p f on a hit.
 *
 * @details A protected frame moves to the protected MRU. A probationary frame is
 *          promoted to protected, demoting the protected LRU back to probationary
 *          if the protected segment is at capacity.
 *
 * @param[in,out] vm Cache providing the metadata + segment lists.
 * @param[in]     f  Frame index just accessed.
 *
 * @return Nothing.
 *
 * @pre `f` is a valid, currently-linked frame.
 * @pre `vm` is non-NULL with a populated config.
 * @post `f` is at the MRU of the protected segment.
 * @post `protected_count <= protected_cap`.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void priv_slru_access(ra_vmem_t* vm, int32_t f)
{
  if (vm->cfg.meta[f].seg == (uint8_t)k_vmem_seg_protected) {
    priv_unlink(vm, f, &vm->pt_head, &vm->pt_tail);
    priv_push_head(vm, f, &vm->pt_head, &vm->pt_tail);
    return;
  }
  priv_unlink(vm, f, &vm->pb_head, &vm->pb_tail);
  if (vm->protected_count >= vm->protected_cap) {
    const int32_t d = vm->pt_tail;
    if (d != -1) {
      priv_unlink(vm, d, &vm->pt_head, &vm->pt_tail);
      vm->cfg.meta[d].seg = (uint8_t)k_vmem_seg_probation;
      priv_push_head(vm, d, &vm->pb_head, &vm->pb_tail);
      vm->protected_count--;
    }
  }
  vm->cfg.meta[f].seg = (uint8_t)k_vmem_seg_protected;
  priv_push_head(vm, f, &vm->pt_head, &vm->pt_tail);
  vm->protected_count++;
}

/**
 * @brief Find the first unpinned frame walking from @p tail toward the MRU.
 *
 * @details Scans a segment's list starting at its LRU @p tail and following
 *          `prev` links until an unpinned frame is found. Bounded by the frame
 *          count (NASA P10 Rule 2).
 *
 * @param[in] vm   Cache providing the frame metadata.
 * @param[in] tail Segment LRU tail to scan from (-1 for an empty segment).
 *
 * @return The first unpinned frame index, or -1 if the segment is empty or all
 *         its frames are pinned.
 * @retval -1 No evictable frame in this segment.
 *
 * @pre `vm` is non-NULL with a populated config.
 * @pre `tail` is -1 or a valid frame index in the segment.
 * @post No state is modified.
 * @post A non-negative result indexes an unpinned frame.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static int32_t priv_first_unpinned(const ra_vmem_t* vm, int32_t tail)
{
  int32_t cur = tail;
  while (cur != -1) {
    if (vm->cfg.meta[cur].pin_count == 0U) {
      return cur;
    }
    cur = vm->cfg.meta[cur].prev;
  }
  return -1;
}

/**
 * @brief Select an evictable victim: probationary LRU first, then protected LRU.
 *
 * @details The scan-resistance core: one-shot scanned pages live in the
 *          probationary segment and are evicted before the protected hot set.
 *          Selection is read-only -- the caller performs the eviction. Skips
 *          pinned frames.
 *
 * @param[in] vm Cache providing the segment lists + metadata.
 *
 * @return The frame index to evict, or -1 if every frame is pinned.
 * @retval -1 No evictable frame (cache fully pinned).
 *
 * @pre `vm` is non-NULL with a populated config.
 * @pre The segment lists are consistent.
 * @post No state is modified (selection only).
 * @post A non-negative result indexes an unpinned frame.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static int32_t priv_pick_victim(const ra_vmem_t* vm)
{
  const int32_t pb = priv_first_unpinned(vm, vm->pb_tail);
  if (pb != -1) {
    return pb;
  }
  return priv_first_unpinned(vm, vm->pt_tail);
}

/**
 * @brief Validate a ::ra_vmem_cfg_t before it is adopted by ::ra_vmem_init.
 *
 * @details Rejects a NULL config, any NULL buffer / callback it carries, and any
 *          zero sizing field. Extracted from ::ra_vmem_init to keep the public
 *          entry point under the statement threshold.
 *
 * @param[in] cfg Caller-supplied configuration to validate.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok               The config is structurally valid.
 * @retval k_ra_err_null_ptr     `cfg` or one of its buffers/callbacks is NULL.
 * @retval k_ra_err_invalid_size A frame/bucket count or frame size is zero.
 *
 * @pre `s_tag` is initialised (always true for this TU).
 * @pre The caller propagates a non-OK result unchanged.
 * @post No state is modified.
 * @post A `k_ra_ok` result guarantees every pointer is non-NULL and every size
 *       field is non-zero.
 *
 * @note Pure; thread-safe.
 * @since 0.1.0
 */
static ra_err_t priv_vmem_validate_cfg(const ra_vmem_cfg_t* cfg)
{
  RA_CHECK_NULL_PTR(cfg, s_tag, "cfg must not be nullptr");
  RA_CHECK_NULL_PTR(cfg->frame_mem, s_tag, "frame_mem must not be nullptr");
  RA_CHECK_NULL_PTR(cfg->meta, s_tag, "meta must not be nullptr");
  RA_CHECK_NULL_PTR(cfg->buckets, s_tag, "buckets must not be nullptr");
  RA_CHECK_NULL_PTR(cfg->loader, s_tag, "loader must not be nullptr");
  if (cfg->frame_count == 0U) {
    return k_ra_err_invalid_size;
  }
  if (cfg->frame_bytes == 0U) {
    return k_ra_err_invalid_size;
  }
  if (cfg->bucket_count == 0U) {
    return k_ra_err_invalid_size;
  }
  return k_ra_ok;
}

/**
 * @brief Reset the hash buckets and thread every frame onto the probation LRU.
 *
 * @details Clears all bucket heads to the nil sentinel, then marks each frame
 *          invalid/unpinned in the probationary segment and pushes it onto the
 *          probation list (cold frames become the natural eviction order).
 *          Extracted from ::ra_vmem_init to keep the entry point short.
 *
 * @param[in,out] vm Cache whose config has already been adopted.
 *
 * @return Nothing.
 *
 * @pre `vm->cfg` is fully populated with non-NULL buffers + non-zero counts.
 * @pre The segment list head/tail pointers are already set to the nil sentinel.
 * @post Every bucket head is the nil sentinel.
 * @post Every frame is invalid, unpinned, and linked into the probation list.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void priv_vmem_link_frames(ra_vmem_t* vm)
{
  for (uint32_t b = 0U; b < vm->cfg.bucket_count; ++b) {
    vm->cfg.buckets[b] = -1;
  }
  /* All frames start invalid in the probationary segment (cold == LRU). */
  for (uint32_t i = 0U; i < vm->cfg.frame_count; ++i) {
    vm->cfg.meta[i].valid     = 0U;
    vm->cfg.meta[i].pin_count = 0U;
    vm->cfg.meta[i].seg       = (uint8_t)k_vmem_seg_probation;
    vm->cfg.meta[i].hash_next = -1;
    priv_push_head(vm, (int32_t)i, &vm->pb_head, &vm->pb_tail);
  }
}

ra_err_t ra_vmem_init(ra_vmem_t* vm, const ra_vmem_cfg_t* cfg)
{
  RA_CHECK_NULL_PTR(vm, s_tag, "vm must not be nullptr");
  const ra_err_t verr = priv_vmem_validate_cfg(cfg);
  if (verr != k_ra_ok) {
    return verr;
  }
  (void)memset(vm, 0, sizeof(*vm));
  vm->cfg     = *cfg;
  vm->pb_head = -1;
  vm->pb_tail = -1;
  vm->pt_head = -1;
  vm->pt_tail = -1;
  vm->protected_cap =
    (cfg->frame_count * (uint32_t)k_vmem_protected_pct) / (uint32_t)k_vmem_percent_full;
  priv_vmem_link_frames(vm);
  return k_ra_ok;
}

/**
 * @brief Handle a get miss: evict a victim, load the page, insert + pin it.
 *
 * @details Picks an unpinned victim, drops its old page (hash remove + unlink),
 *          loads the requested page through the configured loader, and on success
 *          re-keys the frame, re-hashes it, and pins it at the probationary MRU.
 *          A loader failure leaves the frame cold (invalid) so no stale page
 *          survives.
 *
 * @param[in,out] vm          Initialised cache.
 * @param[in]     object_id   Object to page in.
 * @param[in]     aligned_off Frame-aligned offset.
 * @param[out]    out_page    Receives the pinned frame pointer.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok         Page loaded, inserted, and pinned.
 * @retval k_ra_err_no_mem Every frame is pinned.
 * @retval k_ra_err_*      The loader's error (the victim is dropped, cold).
 *
 * @pre `vm`, `out_page` are non-NULL; the key is not already resident.
 * @pre A frame is evictable unless all are pinned.
 * @post On success one new valid, pinned frame holds the page.
 * @post On loader failure the victim is invalidated (no stale page survives).
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static ra_err_t
priv_vmem_miss(ra_vmem_t* vm, uint32_t object_id, uint64_t aligned_off, void** out_page)
{
  const int32_t v = priv_pick_victim(vm);
  if (v < 0) {
    return k_ra_err_no_mem;
  }
  ra_vmem_frame_t* m = &vm->cfg.meta[v];
  if (m->valid != 0U) {
    priv_hash_remove(vm, v);
    vm->evictions++;
  }
  const uint8_t seg = m->seg;
  priv_unlink(vm,
              v,
              (seg == (uint8_t)k_vmem_seg_protected) ? &vm->pt_head : &vm->pb_head,
              (seg == (uint8_t)k_vmem_seg_protected) ? &vm->pt_tail : &vm->pb_tail);
  if (seg == (uint8_t)k_vmem_seg_protected) {
    vm->protected_count--;
  }
  const ra_err_t lerr = vm->cfg.loader(vm->cfg.loader_ctx,
                                       object_id,
                                       aligned_off,
                                       priv_frame_ptr(vm, (uint32_t)v),
                                       vm->cfg.frame_bytes);
  if (lerr != k_ra_ok) {
    m->valid = 0U;
    m->seg   = (uint8_t)k_vmem_seg_probation;
    priv_push_head(vm, v, &vm->pb_head, &vm->pb_tail);
    return lerr;
  }
  m->object_id = object_id;
  m->offset    = aligned_off;
  m->valid     = 1U;
  m->pin_count = 1U;
  m->seg       = (uint8_t)k_vmem_seg_probation;
  priv_hash_insert(vm, v);
  priv_push_head(vm, v, &vm->pb_head, &vm->pb_tail);
  *out_page = priv_frame_ptr(vm, (uint32_t)v);
  return k_ra_ok;
}

ra_err_t ra_vmem_get(ra_vmem_t* vm, uint32_t object_id, uint64_t offset, void** out_page)
{
  RA_CHECK_NULL_PTR(vm, s_tag, "vm must not be nullptr");
  RA_CHECK_NULL_PTR(out_page, s_tag, "out_page must not be nullptr");
  const uint64_t aligned_off = offset - (offset % (uint64_t)vm->cfg.frame_bytes);
  const int32_t  f           = priv_hash_lookup(vm, object_id, aligned_off);
  if (f >= 0) {
    vm->hits++;
    vm->cfg.meta[f].pin_count++;
    priv_slru_access(vm, f);
    *out_page = priv_frame_ptr(vm, (uint32_t)f);
    return k_ra_ok;
  }
  vm->misses++;
  return priv_vmem_miss(vm, object_id, aligned_off, out_page);
}

ra_err_t ra_vmem_put(ra_vmem_t* vm, void* page)
{
  RA_CHECK_NULL_PTR(vm, s_tag, "vm must not be nullptr");
  RA_CHECK_NULL_PTR(page, s_tag, "page must not be nullptr");
  const uintptr_t base = (uintptr_t)vm->cfg.frame_mem;
  const uintptr_t addr = (uintptr_t)page;
  if (addr < base) {
    return k_ra_err_invalid_arg;
  }
  const uintptr_t off  = addr - base;
  const uintptr_t span = (uintptr_t)vm->cfg.frame_count * (uintptr_t)vm->cfg.frame_bytes;
  if (off >= span) {
    return k_ra_err_invalid_arg;
  }
  if ((off % (uintptr_t)vm->cfg.frame_bytes) != 0U) {
    return k_ra_err_invalid_arg;
  }
  const uint32_t idx = (uint32_t)(off / (uintptr_t)vm->cfg.frame_bytes);
  if (vm->cfg.meta[idx].pin_count == 0U) {
    return k_ra_err_invalid_arg;
  }
  vm->cfg.meta[idx].pin_count--;
  return k_ra_ok;
}

ra_err_t ra_vmem_prefetch(ra_vmem_t* vm, uint32_t object_id, uint64_t offset)
{
  RA_CHECK_NULL_PTR(vm, s_tag, "vm must not be nullptr");
  void*          page     = nullptr;
  const ra_err_t load_err = ra_vmem_get(vm, object_id, offset, &page);
  if (load_err != k_ra_ok) {
    return load_err;
  }
  /* Drop the pin now: the page is resident but evictable (2Q probationary), so a
   * wrong read-ahead guess ages out before hot data. */
  return ra_vmem_put(vm, page);
}

ra_err_t ra_vmem_stats(const ra_vmem_t* vm,
                       uint32_t*        out_hits,
                       uint32_t*        out_misses,
                       uint32_t*        out_evictions)
{
  RA_CHECK_NULL_PTR(vm, s_tag, "vm must not be nullptr");
  if (vm->cfg.frame_mem == nullptr) {
    return k_ra_err_invalid_state;
  }
  if (out_hits != nullptr) {
    *out_hits = vm->hits;
  }
  if (out_misses != nullptr) {
    *out_misses = vm->misses;
  }
  if (out_evictions != nullptr) {
    *out_evictions = vm->evictions;
  }
  return k_ra_ok;
}
