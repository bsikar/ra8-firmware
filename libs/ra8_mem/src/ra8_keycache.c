/**
 * @file ra8_keycache.c
 * @brief The one reusable hash + pin + evict cache engine -- impl (#147, #345).
 *
 * @par Tag
 * [Ring 2 / Core] {World: NS}
 *
 * @details
 * One keyed cache engine over fixed cells, selectable between plain LRU (a single
 * recency list) and scan-resistant SLRU / 2Q (a probationary + protected pair).
 * #345 folded ::ra8_vmem's once-duplicate machinery in here: the two policies now
 * share one set of list, hash-chain, pin, and victim-selection primitives and
 * differ only in the on-access promotion and the victim scan order. All cells
 * start invalid in the probationary list, so cold misses and real evictions share
 * one victim path; victim selection is read-only and skips pinned cells. Keys are
 * stored out-of-line in a caller-supplied array and compared byte-wise; the hash
 * is injectable (NULL selects FNV-1a); an optional per-cell user descriptor lets
 * a typed facade recover its dimensions.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_keycache.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_err.h"

/** @brief Module log tag. */
static const char* const s_tag = "ra8_keycache";

/**
 * @enum ra8_keycache_const_t
 * @brief Hashing constants and the SLRU split defaults / bounds.
 *
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_keycache_fnv_offset        = 2166136261U, /**< FNV-1a 32-bit offset basis.      */
  k_keycache_fnv_prime         = 16777619U,   /**< FNV-1a 32-bit prime.             */
  k_keycache_protected_pct_def = 75U,         /**< Default SLRU protected share.    */
  k_keycache_percent_full      = 100U,        /**< Percent denominator / max split. */
} ra8_keycache_const_t;

/**
 * @enum ra8_keycache_seg_t
 * @brief SLRU segment tags stored in ::ra8_keycache_cell_t::seg.
 *
 * @details Under LRU every cell stays ::k_keycache_seg_probation and the tag is
 *          never consulted.
 *
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_keycache_seg_probation = 0U, /**< Probationary segment (scan absorber). */
  k_keycache_seg_protected = 1U, /**< Protected segment (hot working set).  */
} ra8_keycache_seg_t;

/**
 * @brief Cell payload pointer for cell @p idx.
 *
 * @details Indexes the contiguous cell storage by `idx * cell_bytes`.
 *
 * @param[in] kc  Cache providing the cell storage + stride.
 * @param[in] idx Cell index in `[0, cell_count)`.
 *
 * @return Pointer to the start of cell @p idx's payload.
 * @retval non-NULL Always (the storage pointer offset by the stride).
 *
 * @pre `kc` is non-NULL with a populated config.
 * @pre `idx < cfg.cell_count`.
 * @post No state is modified.
 * @post The result lies within the cell storage region.
 *
 * @note Pure; thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
RA8_INTERNAL static uint8_t* internal_cell_ptr(const ra8_keycache_t* kc, uint32_t idx)
{
  return &kc->cfg.cell_mem[(size_t)idx * (size_t)kc->cfg.cell_bytes];
}

/**
 * @brief Key-storage pointer for cell @p idx.
 *
 * @details Indexes the contiguous key storage by `idx * key_bytes`.
 *
 * @param[in] kc  Cache providing the key storage + stride.
 * @param[in] idx Cell index in `[0, cell_count)`.
 *
 * @return Pointer to the start of cell @p idx's stored key.
 * @retval non-NULL Always (the storage pointer offset by the stride).
 *
 * @pre `kc` is non-NULL with a populated config.
 * @pre `idx < cfg.cell_count`.
 * @post No state is modified.
 * @post The result lies within the key storage region.
 *
 * @note Pure; thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
RA8_INTERNAL static uint8_t* internal_key_ptr(const ra8_keycache_t* kc, uint32_t idx)
{
  return &kc->cfg.key_mem[(size_t)idx * (size_t)kc->cfg.key_bytes];
}

/**
 * @brief User-descriptor pointer for cell @p idx, or NULL when unused.
 *
 * @details Indexes the contiguous user-descriptor storage by `idx * user_bytes`,
 *          or returns NULL when the cache was configured with no descriptor.
 *
 * @param[in] kc  Cache providing the descriptor storage + stride.
 * @param[in] idx Cell index in `[0, cell_count)`.
 *
 * @return Pointer to cell @p idx's descriptor, or NULL when `user_bytes == 0`.
 * @retval NULL The cache carries no per-cell user descriptor.
 *
 * @pre `kc` is non-NULL with a populated config.
 * @pre `idx < cfg.cell_count`.
 * @post No state is modified.
 * @post A non-NULL result lies within the descriptor storage region.
 *
 * @note Pure; thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
RA8_INTERNAL static void* internal_user_ptr(const ra8_keycache_t* kc, uint32_t idx)
{
  if (kc->cfg.user_bytes == 0U) {
    return nullptr;
  }
  return &kc->cfg.user_mem[(size_t)idx * (size_t)kc->cfg.user_bytes];
}

/**
 * @brief Byte-wise key equality over `key_bytes`.
 *
 * @details Compares the two `key_bytes`-wide blobs with `memcmp`; callers must
 *          fully initialise keys (no indeterminate padding) for this to be sound.
 *
 * @param[in] kc Cache providing `key_bytes`.
 * @param[in] a  First key blob.
 * @param[in] b  Second key blob.
 *
 * @return true if the blobs are byte-identical, else false.
 * @retval true  Every key byte matches.
 * @retval false Some key byte differs.
 *
 * @pre `kc`, `a`, and `b` are non-NULL.
 * @pre Both blobs are at least `key_bytes` wide.
 * @post No state is modified.
 * @post The result depends only on the key bytes.
 *
 * @note Pure; thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
RA8_INTERNAL static bool internal_key_eq(const ra8_keycache_t* kc, const void* a, const void* b)
{
  return memcmp(a, b, (size_t)kc->cfg.key_bytes) == 0;
}

/**
 * @brief FNV-1a hash of a key blob (the built-in default hash).
 *
 * @details Folds the `key_bytes` of @p key through the FNV-1a mix, returning the
 *          raw 32-bit value. Bounded by `key_bytes` (NASA P10 Rule 2). Used when
 *          the config supplies no `hash` callback.
 *
 * @param[in] key       Key blob to hash.
 * @param[in] key_bytes Key width in bytes.
 *
 * @return The raw 32-bit FNV-1a hash.
 * @retval 0 The bytes folded to zero (one possible result).
 *
 * @pre `key` is at least `key_bytes` wide.
 * @pre `key_bytes` is non-zero.
 * @post No state is modified.
 * @post The result depends only on the key bytes.
 *
 * @note Pure; thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
RA8_INTERNAL static uint32_t internal_fnv1a(const void* key, uint32_t key_bytes)
{
  const uint8_t* p = (const uint8_t*)key;
  uint32_t       h = (uint32_t)k_keycache_fnv_offset;
  for (uint32_t i = 0U; i < key_bytes; ++i) {
    h ^= (uint32_t)p[i];
    h *= (uint32_t)k_keycache_fnv_prime;
  }
  return h;
}

/**
 * @brief Hash a key blob into a bucket index using the configured policy.
 *
 * @details Runs the injected `cfg.hash` (or the built-in ::internal_fnv1a when it is
 *          NULL) and folds the raw 32-bit result to `[0, bucket_count)`.
 *
 * @param[in] kc  Cache providing the bucket count, key width, and hash policy.
 * @param[in] key Key blob to hash.
 *
 * @return Bucket index in `[0, bucket_count)`.
 * @retval 0 The key folded into the first bucket (one possible result).
 *
 * @pre `kc` is non-NULL with `bucket_count > 0` and `key_bytes > 0`.
 * @pre `key` is at least `key_bytes` wide.
 * @post No state is modified.
 * @post The result is strictly less than `bucket_count`.
 *
 * @note Pure; thread-safe.
 *
 * @par MC/DC:
 * Decision: `if (kc->cfg.hash != nullptr)` (1 condition, no compound `&&`/`||`).
 * - hash set   -> the injected callback runs (::ra8_vmem's page hash).
 * - hash NULL  -> the built-in FNV-1a runs (the glyph/tile atlases + tests).
 * Both single-condition outcomes are exercised across the cache test suites.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
RA8_INTERNAL static uint32_t internal_hash(const ra8_keycache_t* kc, const void* key)
{
  uint32_t raw;
  if (kc->cfg.hash != nullptr) {
    raw = kc->cfg.hash(key, kc->cfg.key_bytes, kc->cfg.hash_ctx);
  } else {
    raw = internal_fnv1a(key, kc->cfg.key_bytes);
  }
  return raw % kc->cfg.bucket_count;
}

/**
 * @brief Detach cell @p f from the recency list owned by @p head / @p tail.
 *
 * @details Splices @p f out of the doubly-linked list, fixing the neighbours and
 *          the head/tail pointers as needed. The same primitive serves the LRU
 *          single list and either SLRU segment.
 *
 * @param[in,out] kc   Cache providing the cell metadata.
 * @param[in]     f    Cell index to unlink.
 * @param[in,out] head Segment MRU head pointer.
 * @param[in,out] tail Segment LRU tail pointer.
 *
 * @return Nothing.
 *
 * @pre `f` is currently a member of the @p head / @p tail list.
 * @pre `kc`, `head`, `tail` are non-NULL.
 * @post `f`'s neighbours and the head/tail no longer reference `f`.
 * @post Other cells are unmodified.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
RA8_INTERNAL static void
internal_unlink(ra8_keycache_t* kc, int32_t f, int32_t* head, int32_t* tail)
{
  ra8_keycache_cell_t* m = kc->cfg.meta;
  if (m[f].prev != -1) {
    m[m[f].prev].next = m[f].next;
  } else if (*head == f) {
    *head = m[f].next;
  } else {
    /* not the head and has no prev: already detached -- nothing to do */
  }
  if (m[f].next != -1) {
    m[m[f].next].prev = m[f].prev;
  } else if (*tail == f) {
    *tail = m[f].prev;
  } else {
    /* not the tail and has no next: already detached -- nothing to do */
  }
}

/**
 * @brief Push cell @p f onto the MRU head of a recency list.
 *
 * @details Links @p f as the new most-recently-used entry, updating the tail when
 *          the list was empty. Serves the LRU single list and either SLRU segment.
 *
 * @param[in,out] kc   Cache providing the cell metadata.
 * @param[in]     f    Cell index to insert (must be detached).
 * @param[in,out] head Segment MRU head pointer.
 * @param[in,out] tail Segment LRU tail pointer.
 *
 * @return Nothing.
 *
 * @pre `f` is not currently linked into any list.
 * @pre `kc`, `head`, `tail` are non-NULL.
 * @post `f` is the MRU head of the list.
 * @post The tail points at `f` iff the list was previously empty.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
RA8_INTERNAL static void
internal_push_head(ra8_keycache_t* kc, int32_t f, int32_t* head, int32_t* tail)
{
  ra8_keycache_cell_t* m = kc->cfg.meta;
  m[f].prev              = -1;
  m[f].next              = *head;
  if (*head != -1) {
    m[*head].prev = f;
  }
  *head = f;
  if (*tail == -1) {
    *tail = f;
  }
}

/**
 * @brief Insert cell @p f into its hash bucket chain.
 *
 * @details Prepends @p f to the chain of the bucket its stored key hashes to.
 *
 * @param[in,out] kc Cache providing buckets + metadata.
 * @param[in]     f  Cell index (its stored key is set).
 *
 * @return Nothing.
 *
 * @pre `f`'s stored key is set and it is not already chained.
 * @pre `kc` is non-NULL with a populated config.
 * @post `f` is the head of its bucket chain.
 * @post The previous head follows `f`.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
RA8_INTERNAL static void internal_hash_insert(ra8_keycache_t* kc, int32_t f)
{
  const uint32_t b          = internal_hash(kc, internal_key_ptr(kc, (uint32_t)f));
  kc->cfg.meta[f].hash_next = kc->cfg.buckets[b];
  kc->cfg.buckets[b]        = f;
}

/**
 * @brief Remove cell @p f from its hash bucket chain.
 *
 * @details Unlinks @p f from its bucket, handling the head and mid-chain cases.
 *          The chain walk is bounded by `cfg.cell_count` via an explicit guard
 *          counter (NASA P10 Rule 2) -- a corrupted chain cannot spin forever.
 *
 * @param[in,out] kc Cache providing buckets + metadata.
 * @param[in]     f  Cell index currently chained.
 *
 * @return Nothing.
 *
 * @pre `f` is currently present in its bucket chain.
 * @pre `kc` is non-NULL with a populated config.
 * @post `f` is no longer reachable from the bucket.
 * @post `f`'s `hash_next` is reset to -1.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
RA8_INTERNAL static void internal_hash_remove(ra8_keycache_t* kc, int32_t f)
{
  const uint32_t b   = internal_hash(kc, internal_key_ptr(kc, (uint32_t)f));
  int32_t        cur = kc->cfg.buckets[b];
  if (cur == f) {
    kc->cfg.buckets[b] = kc->cfg.meta[f].hash_next;
  } else {
    for (uint32_t guard = 0U; guard < kc->cfg.cell_count; ++guard) {
      if (cur == -1) {
        break;
      }
      if (kc->cfg.meta[cur].hash_next == f) {
        kc->cfg.meta[cur].hash_next = kc->cfg.meta[f].hash_next;
        break;
      }
      cur = kc->cfg.meta[cur].hash_next;
    }
  }
  kc->cfg.meta[f].hash_next = -1;
}

/**
 * @brief Find the valid cell holding @p key, or -1.
 *
 * @details Walks the key's bucket chain for a valid cell with a matching stored
 *          key. The chain walk is bounded by `cfg.cell_count` via an explicit
 *          guard counter (NASA P10 Rule 2).
 *
 * @param[in] kc  Cache providing buckets + metadata.
 * @param[in] key Key blob to find.
 *
 * @return The matching cell index, or -1 if not resident.
 * @retval -1 No valid cell holds the key.
 *
 * @pre `kc` and `key` are non-NULL.
 * @pre `kc` has a populated config.
 * @post No state is modified.
 * @post A non-negative result indexes a valid cell with the key.
 *
 * @note Not thread-safe with respect to concurrent mutation.
 * @since 0.1.0
 */
RA8_INTERNAL
RA8_INTERNAL static int32_t internal_hash_lookup(const ra8_keycache_t* kc, const void* key)
{
  const uint32_t b   = internal_hash(kc, key);
  int32_t        cur = kc->cfg.buckets[b];
  for (uint32_t guard = 0U; guard < kc->cfg.cell_count; ++guard) {
    if (cur == -1) {
      break;
    }
    const ra8_keycache_cell_t* m = &kc->cfg.meta[cur];
    if (m->valid != 0U) {
      if (internal_key_eq(kc, internal_key_ptr(kc, (uint32_t)cur), key)) {
        return cur;
      }
    }
    cur = m->hash_next;
  }
  return -1;
}

/**
 * @brief Find the first unpinned cell walking from @p tail toward the MRU.
 *
 * @details Scans a segment's list from its LRU @p tail following `prev` links
 *          until an unpinned cell is found. Bounded by `cfg.cell_count` via an
 *          explicit guard counter (NASA P10 Rule 2): each cell appears once, so
 *          the guard never trips in well-formed state and caps a corrupted list.
 *
 * @param[in] kc   Cache providing the cell metadata.
 * @param[in] tail Segment LRU tail to scan from (-1 for an empty segment).
 *
 * @return The first unpinned cell index, or -1 if the segment is empty or all its
 *         cells are pinned.
 * @retval -1 No evictable cell in this segment.
 *
 * @pre `kc` is non-NULL with a populated config.
 * @pre `tail` is -1 or a valid cell index in the segment.
 * @post No state is modified.
 * @post A non-negative result indexes an unpinned cell.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
RA8_INTERNAL static int32_t internal_first_unpinned(const ra8_keycache_t* kc, int32_t tail)
{
  int32_t cur = tail;
  for (uint32_t guard = 0U; guard < kc->cfg.cell_count; ++guard) {
    if (cur == -1) {
      break;
    }
    if (kc->cfg.meta[cur].pin_count == 0U) {
      return cur;
    }
    cur = kc->cfg.meta[cur].prev;
  }
  return -1;
}

/**
 * @brief Select an evictable victim: probationary LRU first, then protected LRU.
 *
 * @details Read-only (the caller performs the eviction), skips pinned cells. Under
 *          LRU the protected segment is always empty, so this reduces to "the LRU
 *          cell"; under SLRU one-shot scanned entries in probation are evicted
 *          before the protected hot set.
 *
 * @param[in] kc Cache providing the segment lists + metadata.
 *
 * @return The cell index to evict, or -1 if every cell is pinned.
 * @retval -1 No evictable cell (cache fully pinned).
 *
 * @pre `kc` is non-NULL with a populated config.
 * @pre The segment lists are consistent.
 * @post No state is modified (selection only).
 * @post A non-negative result indexes an unpinned cell.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
RA8_INTERNAL static int32_t internal_pick_victim(const ra8_keycache_t* kc)
{
  const int32_t pb = internal_first_unpinned(kc, kc->pb_tail);
  if (pb != -1) {
    return pb;
  }
  return internal_first_unpinned(kc, kc->pt_tail);
}

/**
 * @brief SLRU re-reference: promote / refresh cell @p f on a hit.
 *
 * @details A protected cell moves to the protected MRU. A probationary cell is
 *          promoted to protected, demoting the protected LRU back to probationary
 *          if the protected segment is at capacity.
 *
 * @param[in,out] kc Cache providing the metadata + segment lists.
 * @param[in]     f  Cell index just accessed.
 *
 * @return Nothing.
 *
 * @pre `f` is a valid, currently-linked cell.
 * @pre `kc` is non-NULL with a populated SLRU config.
 * @post `f` is at the MRU of the protected segment.
 * @post `protected_count <= protected_cap`.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
RA8_INTERNAL static void internal_slru_access(ra8_keycache_t* kc, int32_t f)
{
  if (kc->cfg.meta[f].seg == (uint8_t)k_keycache_seg_protected) {
    internal_unlink(kc, f, &kc->pt_head, &kc->pt_tail);
    internal_push_head(kc, f, &kc->pt_head, &kc->pt_tail);
    return;
  }
  internal_unlink(kc, f, &kc->pb_head, &kc->pb_tail);
  if (kc->protected_count >= kc->protected_cap) {
    const int32_t d = kc->pt_tail;
    if (d != -1) {
      internal_unlink(kc, d, &kc->pt_head, &kc->pt_tail);
      kc->cfg.meta[d].seg = (uint8_t)k_keycache_seg_probation;
      internal_push_head(kc, d, &kc->pb_head, &kc->pb_tail);
      kc->protected_count--;
    }
  }
  kc->cfg.meta[f].seg = (uint8_t)k_keycache_seg_protected;
  internal_push_head(kc, f, &kc->pt_head, &kc->pt_tail);
  kc->protected_count++;
}

/**
 * @brief Re-reference cell @p f on a hit under the configured policy.
 *
 * @details LRU moves @p f to the single list's MRU; SLRU runs ::internal_slru_access
 *          to promote it toward the protected segment.
 *
 * @param[in,out] kc Cache providing the policy + segment lists.
 * @param[in]     f  Cell index just accessed.
 *
 * @return Nothing.
 *
 * @pre `f` is a valid, currently-linked cell.
 * @pre `kc` is non-NULL with a populated config.
 * @post `f` is at the MRU of its (possibly new) segment.
 * @post The recency order otherwise reflects the access.
 *
 * @note Not thread-safe.
 *
 * @par MC/DC:
 * Decision: `if (kc->cfg.evict == k_ra8_keycache_evict_slru)` (1 condition, no
 * compound `&&`/`||`).
 * - evict == SLRU -> ::internal_slru_access (exercised by the ::ra8_vmem tests).
 * - evict == LRU  -> single-list move-to-front (glyph/tile atlases + tests).
 *
 * @since 0.1.0
 */
RA8_INTERNAL
RA8_INTERNAL static void internal_access(ra8_keycache_t* kc, int32_t f)
{
  if (kc->cfg.evict == k_ra8_keycache_evict_slru) {
    internal_slru_access(kc, f);
  } else {
    internal_unlink(kc, f, &kc->pb_head, &kc->pb_tail);
    internal_push_head(kc, f, &kc->pb_head, &kc->pb_tail);
  }
}

/**
 * @brief Validate that every required config pointer is non-NULL.
 *
 * @details Runs the null-pointer preconditions for ::ra8_keycache_init: the cell,
 *          key, meta, bucket, and render pointers, plus `user_mem` when
 *          `user_bytes > 0`. Each check is an independent decision kept intact
 *          here (no compound decision is split across functions).
 *
 * @param[in] cfg Storage + renderer configuration to validate (non-NULL).
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok           Every required pointer is non-NULL.
 * @retval k_ra8_err_null_ptr A required `cfg` pointer is NULL.
 *
 * @pre `cfg` is non-NULL (the caller checked it).
 * @pre The config fields reflect the caller's intended storage layout.
 * @post No state is modified (pure validation).
 * @post A non-ok return means a required `cfg` pointer is NULL.
 *
 * @note Not thread-safe with respect to concurrent config mutation.
 * @since 0.1.0
 */
RA8_INTERNAL
RA8_INTERNAL static ra8_err_t internal_validate_cfg_ptrs(const ra8_keycache_cfg_t* cfg)
{
  RA8_CHECK_NULL_PTR(cfg->cell_mem, s_tag, "cell_mem must not be nullptr");
  RA8_CHECK_NULL_PTR(cfg->key_mem, s_tag, "key_mem must not be nullptr");
  RA8_CHECK_NULL_PTR(cfg->meta, s_tag, "meta must not be nullptr");
  RA8_CHECK_NULL_PTR(cfg->buckets, s_tag, "buckets must not be nullptr");
  RA8_CHECK_NULL_PTR(cfg->render, s_tag, "render must not be nullptr");
  if (cfg->user_bytes != 0U) {
    RA8_CHECK_NULL_PTR(cfg->user_mem, s_tag, "user_mem required when user_bytes > 0");
  }
  return k_ra8_ok;
}

/**
 * @brief Validate that every config sizing field is non-zero.
 *
 * @details Rejects a zero `cell_count`, `cell_bytes`, `key_bytes`, or
 *          `bucket_count` -- each would make the cache storage degenerate. Each
 *          check is an independent decision kept intact here (no compound
 *          decision is split across functions).
 *
 * @param[in] cfg Storage + renderer configuration to validate (non-NULL).
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok               Every sizing field is non-zero.
 * @retval k_ra8_err_invalid_size A sizing field (`cell_count`, `cell_bytes`,
 *                               `key_bytes`, `bucket_count`) was zero.
 *
 * @pre `cfg` is non-NULL (the caller checked it).
 * @pre The config pointers passed ::internal_validate_cfg_ptrs.
 * @post No state is modified (pure validation).
 * @post A non-ok return means a sizing field was zero.
 *
 * @note Not thread-safe with respect to concurrent config mutation.
 * @since 0.1.0
 */
RA8_INTERNAL
RA8_INTERNAL static ra8_err_t internal_validate_cfg_sizes(const ra8_keycache_cfg_t* cfg)
{
  if (cfg->cell_count == 0U) {
    return k_ra8_err_invalid_size;
  }
  if (cfg->cell_bytes == 0U) {
    return k_ra8_err_invalid_size;
  }
  if (cfg->key_bytes == 0U) {
    return k_ra8_err_invalid_size;
  }
  if (cfg->bucket_count == 0U) {
    return k_ra8_err_invalid_size;
  }
  return k_ra8_ok;
}

/**
 * @brief Validate the SLRU split knob when the SLRU policy is selected.
 *
 * @details An SLRU cache rejects a `protected_pct` above 100; every other value
 *          (0 selecting the 75% default) is accepted. Under LRU the knob is
 *          unused, so nothing is checked. Two nested single-condition decisions;
 *          no compound `&&`/`||`.
 *
 * @param[in] cfg Storage + policy configuration to validate (non-NULL).
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok              The policy knob is in range (or unused under LRU).
 * @retval k_ra8_err_invalid_arg SLRU with `protected_pct` above 100.
 *
 * @pre `cfg` is non-NULL (the caller checked it).
 * @pre The config passed ::internal_validate_cfg_ptrs / ::internal_validate_cfg_sizes.
 * @post No state is modified (pure validation).
 * @post A non-ok return means SLRU was selected with an out-of-range split.
 *
 * @note Not thread-safe with respect to concurrent config mutation.
 *
 * @par MC/DC:
 * Decision A: `if (cfg->evict == SLRU)` (1 condition); Decision B:
 * `if (protected_pct > 100)` (1 condition). Neither is compound.
 * - LRU                       -> both skipped, k_ra8_ok (glyph/tile tests).
 * - SLRU, pct == 101          -> A true, B true, k_ra8_err_invalid_arg (vmem).
 * - SLRU, pct == 25/50        -> A true, B false, k_ra8_ok (vmem split knob).
 *
 * @since 0.1.0
 */
RA8_INTERNAL
RA8_INTERNAL static ra8_err_t internal_validate_cfg_policy(const ra8_keycache_cfg_t* cfg)
{
  if (cfg->evict == k_ra8_keycache_evict_slru) {
    if ((uint32_t)cfg->protected_pct > (uint32_t)k_keycache_percent_full) {
      return k_ra8_err_invalid_arg;
    }
  }
  return k_ra8_ok;
}

/**
 * @brief Resolve the SLRU protected-segment capacity, in cells.
 *
 * @details Maps `cfg->protected_pct` (0 selects the 75% default; 1..100 verbatim)
 *          to an absolute cell count via `cell_count * pct / 100`. A small
 *          non-zero split can floor to zero protected cells -- a valid degenerate
 *          policy where every re-reference stays probationary.
 *
 * @param[in] cfg Validated SLRU configuration (`protected_pct <= 100`).
 *
 * @return Protected-segment capacity in `[0, cell_count]`.
 * @retval 0 The split floored to no protected cells.
 *
 * @pre `cfg` passed ::internal_validate_cfg_policy (so `protected_pct <= 100`).
 * @pre `cfg->cell_count` is non-zero.
 * @post No state is modified.
 * @post The result is <= `cfg->cell_count`.
 *
 * @note Pure; thread-safe.
 *
 * @par MC/DC:
 * Decision: `protected_pct == 0` selector (1 condition, no compound `&&`/`||`).
 * - pct == 0  -> 75% default (the ::ra8_vmem default config).
 * - pct != 0  -> the given split (the vmem 25%/50% split-knob probes).
 *
 * @since 0.1.0
 */
RA8_INTERNAL
RA8_INTERNAL static uint32_t internal_protected_cap(const ra8_keycache_cfg_t* cfg)
{
  const uint32_t pct = (cfg->protected_pct == 0U) ? (uint32_t)k_keycache_protected_pct_def
                                                  : (uint32_t)cfg->protected_pct;
  return (cfg->cell_count * pct) / (uint32_t)k_keycache_percent_full;
}

/**
 * @brief Seed a validated cache: clear buckets and link every cell cold.
 *
 * @details Zero-fills @p kc, copies the (already-validated) config in, resets the
 *          segment endpoints, clears every hash bucket head to -1, and threads
 *          each cell -- invalid, unpinned, unchained, probationary -- onto the
 *          probationary list via ::internal_push_head. Both loops are bounded by
 *          config counts (NASA P10 Rule 2).
 *
 * @param[out] kc  Cache state to populate (caller-owned).
 * @param[in]  cfg Validated storage + policy + renderer configuration (non-NULL).
 *
 * @return Nothing.
 *
 * @pre @p cfg passed ::internal_validate_cfg_ptrs / _sizes / _policy.
 * @pre @p kc is a writable cache-state object.
 * @post Every bucket head is -1 and every cell is a cold probationary member.
 * @post `kc->cfg` is a copy of @p cfg with empty segment endpoints established.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
RA8_INTERNAL static void internal_seed_cells(ra8_keycache_t* kc, const ra8_keycache_cfg_t* cfg)
{
  (void)memset(kc, 0, sizeof(*kc));
  kc->cfg     = *cfg;
  kc->pb_head = -1;
  kc->pb_tail = -1;
  kc->pt_head = -1;
  kc->pt_tail = -1;
  for (uint32_t b = 0U; b < cfg->bucket_count; ++b) {
    cfg->buckets[b] = -1;
  }
  for (uint32_t i = 0U; i < cfg->cell_count; ++i) {
    kc->cfg.meta[i].valid     = 0U;
    kc->cfg.meta[i].pin_count = 0U;
    kc->cfg.meta[i].seg       = (uint8_t)k_keycache_seg_probation;
    kc->cfg.meta[i].hash_next = -1;
    internal_push_head(kc, (int32_t)i, &kc->pb_head, &kc->pb_tail);
  }
}

ra8_err_t ra8_keycache_init(ra8_keycache_t* kc, const ra8_keycache_cfg_t* cfg)
{
  RA8_CHECK_NULL_PTR(kc, s_tag, "kc must not be nullptr");
  RA8_CHECK_NULL_PTR(cfg, s_tag, "cfg must not be nullptr");
  const ra8_err_t perr = internal_validate_cfg_ptrs(cfg);
  if (perr != k_ra8_ok) {
    return perr;
  }
  const ra8_err_t serr = internal_validate_cfg_sizes(cfg);
  if (serr != k_ra8_ok) {
    return serr;
  }
  const ra8_err_t verr = internal_validate_cfg_policy(cfg);
  if (verr != k_ra8_ok) {
    return verr;
  }
  internal_seed_cells(kc, cfg);
  kc->protected_cap = (cfg->evict == k_ra8_keycache_evict_slru) ? internal_protected_cap(cfg) : 0U;
  return k_ra8_ok;
}

/**
 * @brief Handle a get miss: evict a victim, render the cell, insert + pin it.
 *
 * @details Picks an unpinned victim, drops its old entry (hash remove + unlink
 *          from its segment), renders the requested cell, and on success stores
 *          the key, re-hashes, and pins it at the probationary MRU. A render
 *          failure leaves the cell cold (probationary, invalid) so no stale entry
 *          survives.
 *
 * @param[in,out] kc       Initialised cache.
 * @param[in]     key      Key to render.
 * @param[out]    out_view Receives the pinned cell view.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok         Cell rendered, inserted, and pinned.
 * @retval k_ra8_err_no_mem Every cell is pinned.
 * @retval k_ra8_err_*      The render callback's error (the victim is dropped).
 *
 * @pre `kc`, `key`, `out_view` are non-NULL; the key is not resident.
 * @pre A cell is evictable unless all are pinned.
 * @post On success one valid, pinned cell holds the entry at the probationary MRU.
 * @post On render failure the victim is invalidated (no stale entry survives).
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
RA8_INTERNAL static ra8_err_t
internal_miss(ra8_keycache_t* kc, const void* key, ra8_keycache_view_t* out_view)
{
  const int32_t v = internal_pick_victim(kc);
  if (v < 0) {
    return k_ra8_err_no_mem;
  }
  ra8_keycache_cell_t* m = &kc->cfg.meta[v];
  if (m->valid != 0U) {
    internal_hash_remove(kc, v);
    kc->evictions++;
  }
  const uint8_t seg = m->seg;
  internal_unlink(kc,
                  v,
                  (seg == (uint8_t)k_keycache_seg_protected) ? &kc->pt_head : &kc->pb_head,
                  (seg == (uint8_t)k_keycache_seg_protected) ? &kc->pt_tail : &kc->pb_tail);
  if (seg == (uint8_t)k_keycache_seg_protected) {
    kc->protected_count--;
  }
  uint8_t*        cell = internal_cell_ptr(kc, (uint32_t)v);
  void*           user = internal_user_ptr(kc, (uint32_t)v);
  const ra8_err_t rerr = kc->cfg.render(kc->cfg.render_ctx, key, cell, kc->cfg.cell_bytes, user);
  if (rerr != k_ra8_ok) {
    m->valid = 0U;
    m->seg   = (uint8_t)k_keycache_seg_probation;
    internal_push_head(kc, v, &kc->pb_head, &kc->pb_tail);
    return rerr;
  }
  (void)memcpy(internal_key_ptr(kc, (uint32_t)v), key, (size_t)kc->cfg.key_bytes);
  m->valid     = 1U;
  m->pin_count = 1U;
  m->seg       = (uint8_t)k_keycache_seg_probation;
  internal_hash_insert(kc, v);
  internal_push_head(kc, v, &kc->pb_head, &kc->pb_tail);
  *out_view = (ra8_keycache_view_t){.data = cell, .user = user};
  return k_ra8_ok;
}

ra8_err_t ra8_keycache_get(ra8_keycache_t* kc, const void* key, ra8_keycache_view_t* out_view)
{
  RA8_CHECK_NULL_PTR(kc, s_tag, "kc must not be nullptr");
  RA8_CHECK_NULL_PTR(key, s_tag, "key must not be nullptr");
  RA8_CHECK_NULL_PTR(out_view, s_tag, "out_view must not be nullptr");
  const int32_t f = internal_hash_lookup(kc, key);
  if (f >= 0) {
    kc->hits++;
    kc->cfg.meta[f].pin_count++;
    internal_access(kc, f);
    *out_view = (ra8_keycache_view_t){.data = internal_cell_ptr(kc, (uint32_t)f),
                                      .user = internal_user_ptr(kc, (uint32_t)f)};
    return k_ra8_ok;
  }
  kc->misses++;
  return internal_miss(kc, key, out_view);
}

ra8_err_t ra8_keycache_prefetch(ra8_keycache_t* kc, const void* key)
{
  RA8_CHECK_NULL_PTR(kc, s_tag, "kc must not be nullptr");
  RA8_CHECK_NULL_PTR(key, s_tag, "key must not be nullptr");
  ra8_keycache_view_t view = {};
  const ra8_err_t     gerr = ra8_keycache_get(kc, key, &view);
  if (gerr != k_ra8_ok) {
    return gerr;
  }
  /* Drop the pin now: the cell is resident but evictable, so a wrong read-ahead
   * guess ages out before hot data is displaced by a pinned request. */
  return ra8_keycache_put(kc, view.data);
}

ra8_err_t ra8_keycache_put(ra8_keycache_t* kc, const uint8_t* data)
{
  RA8_CHECK_NULL_PTR(kc, s_tag, "kc must not be nullptr");
  RA8_CHECK_NULL_PTR(data, s_tag, "data must not be nullptr");
  const uintptr_t base = (uintptr_t)kc->cfg.cell_mem;
  const uintptr_t addr = (uintptr_t)data;
  if (addr < base) {
    return k_ra8_err_invalid_arg;
  }
  const uintptr_t off  = addr - base;
  const uintptr_t span = (uintptr_t)kc->cfg.cell_count * (uintptr_t)kc->cfg.cell_bytes;
  if (off >= span) {
    return k_ra8_err_invalid_arg;
  }
  if ((off % (uintptr_t)kc->cfg.cell_bytes) != 0U) {
    return k_ra8_err_invalid_arg;
  }
  const uint32_t idx = (uint32_t)(off / (uintptr_t)kc->cfg.cell_bytes);
  if (kc->cfg.meta[idx].pin_count == 0U) {
    return k_ra8_err_invalid_arg;
  }
  kc->cfg.meta[idx].pin_count--;
  return k_ra8_ok;
}

ra8_err_t ra8_keycache_stats(const ra8_keycache_t* kc,
                             uint32_t*             out_hits,
                             uint32_t*             out_misses,
                             uint32_t*             out_evictions)
{
  RA8_CHECK_NULL_PTR(kc, s_tag, "kc must not be nullptr");
  if (kc->cfg.cell_mem == nullptr) {
    return k_ra8_err_invalid_state;
  }
  if (out_hits != nullptr) {
    *out_hits = kc->hits;
  }
  if (out_misses != nullptr) {
    *out_misses = kc->misses;
  }
  if (out_evictions != nullptr) {
    *out_evictions = kc->evictions;
  }
  return k_ra8_ok;
}
