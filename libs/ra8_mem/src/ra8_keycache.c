/**
 * @file ra8_keycache.c
 * @brief Reusable keyed-LRU cache -- implementation (Layer 3, #147).
 *
 * @par Tag
 * [Ring 2 / Core] {World: NS}
 *
 * @details
 * A keyed LRU cache over fixed cells, structurally like ::ra8_vmem but with a
 * single LRU list (the Layer-3 caches have strong per-page reuse locality, so
 * scan resistance is unnecessary here) and an opaque byte-blob key. All cells
 * start invalid in the LRU list, so cold misses and real evictions share one
 * victim path; victim selection is read-only and skips pinned cells. Keys are
 * stored out-of-line in a caller-supplied array and compared byte-wise; an
 * optional per-cell user descriptor lets a typed facade recover its dimensions.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_keycache.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ra8_check.h"
#include "ra8_err.h"

/** @brief Module log tag. */
static const char* const s_tag = "ra8_keycache";

/**
 * @enum ra8_keycache_hash_const_t
 * @brief FNV-1a hashing constants for the opaque key blob.
 *
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_keycache_fnv_offset = 2166136261U, /**< FNV-1a 32-bit offset basis. */
  k_keycache_fnv_prime  = 16777619U,   /**< FNV-1a 32-bit prime.        */
} ra8_keycache_hash_const_t;

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
static uint8_t* priv_cell_ptr(const ra8_keycache_t* kc, uint32_t idx)
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
static uint8_t* priv_key_ptr(const ra8_keycache_t* kc, uint32_t idx)
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
static void* priv_user_ptr(const ra8_keycache_t* kc, uint32_t idx)
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
static bool priv_key_eq(const ra8_keycache_t* kc, const void* a, const void* b)
{
  return memcmp(a, b, (size_t)kc->cfg.key_bytes) == 0;
}

/**
 * @brief Hash a key blob into a bucket index with FNV-1a.
 *
 * @details Folds the `key_bytes` of @p key through the FNV-1a mix and reduces to
 *          `[0, bucket_count)`. Bounded by `key_bytes` (NASA P10 Rule 2).
 *
 * @param[in] kc  Cache providing the bucket count + key width.
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
 * @since 0.1.0
 */
static uint32_t priv_hash(const ra8_keycache_t* kc, const void* key)
{
  const uint8_t* p = (const uint8_t*)key;
  uint32_t       h = (uint32_t)k_keycache_fnv_offset;
  for (uint32_t i = 0U; i < kc->cfg.key_bytes; ++i) {
    h ^= (uint32_t)p[i];
    h *= (uint32_t)k_keycache_fnv_prime;
  }
  return h % kc->cfg.bucket_count;
}

/**
 * @brief Detach cell @p f from the LRU list.
 *
 * @details Splices @p f out of the list, fixing neighbours and the head/tail.
 *
 * @param[in,out] kc Cache providing the cell metadata.
 * @param[in]     f  Cell index to unlink.
 *
 * @return Nothing.
 *
 * @pre `f` is currently a member of the LRU list.
 * @pre `kc` is non-NULL with a populated config.
 * @post `f`'s neighbours and head/tail no longer reference `f`.
 * @post Other cells are unmodified.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void priv_unlink(ra8_keycache_t* kc, int32_t f)
{
  ra8_keycache_cell_t* m = kc->cfg.meta;
  if (m[f].prev != -1) {
    m[m[f].prev].next = m[f].next;
  } else if (kc->lru_head == f) {
    kc->lru_head = m[f].next;
  } else {
    /* not the head and has no prev: already detached -- nothing to do */
  }
  if (m[f].next != -1) {
    m[m[f].next].prev = m[f].prev;
  } else if (kc->lru_tail == f) {
    kc->lru_tail = m[f].prev;
  } else {
    /* not the tail and has no next: already detached -- nothing to do */
  }
}

/**
 * @brief Push cell @p f onto the MRU head of the LRU list.
 *
 * @details Links @p f as the most-recently-used cell, setting the tail when the
 *          list was empty.
 *
 * @param[in,out] kc Cache providing the cell metadata.
 * @param[in]     f  Cell index to insert (must be detached).
 *
 * @return Nothing.
 *
 * @pre `f` is not currently linked into the list.
 * @pre `kc` is non-NULL with a populated config.
 * @post `f` is the MRU head.
 * @post The tail points at `f` iff the list was previously empty.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void priv_push_head(ra8_keycache_t* kc, int32_t f)
{
  ra8_keycache_cell_t* m = kc->cfg.meta;
  m[f].prev              = -1;
  m[f].next              = kc->lru_head;
  if (kc->lru_head != -1) {
    m[kc->lru_head].prev = f;
  }
  kc->lru_head = f;
  if (kc->lru_tail == -1) {
    kc->lru_tail = f;
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
static void priv_hash_insert(ra8_keycache_t* kc, int32_t f)
{
  const uint32_t b          = priv_hash(kc, priv_key_ptr(kc, (uint32_t)f));
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
static void priv_hash_remove(ra8_keycache_t* kc, int32_t f)
{
  const uint32_t b   = priv_hash(kc, priv_key_ptr(kc, (uint32_t)f));
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
static int32_t priv_hash_lookup(const ra8_keycache_t* kc, const void* key)
{
  const uint32_t b   = priv_hash(kc, key);
  int32_t        cur = kc->cfg.buckets[b];
  for (uint32_t guard = 0U; guard < kc->cfg.cell_count; ++guard) {
    if (cur == -1) {
      break;
    }
    const ra8_keycache_cell_t* m = &kc->cfg.meta[cur];
    if (m->valid != 0U) {
      if (priv_key_eq(kc, priv_key_ptr(kc, (uint32_t)cur), key)) {
        return cur;
      }
    }
    cur = m->hash_next;
  }
  return -1;
}

/**
 * @brief Select an evictable victim (LRU first), or -1 if all pinned.
 *
 * @details Walks the LRU list from the tail toward the MRU for the first
 *          unpinned cell. Read-only; the caller performs the eviction. Bounded by
 *          `cfg.cell_count` via an explicit guard counter (NASA P10 Rule 2): the
 *          list holds every cell exactly once, so the guard never trips in
 *          well-formed state and caps a corrupted list otherwise.
 *
 * @param[in] kc Cache providing the LRU list + metadata.
 *
 * @return The cell index to evict, or -1 if every cell is pinned.
 * @retval -1 No evictable cell (cache fully pinned).
 *
 * @pre `kc` is non-NULL with a populated config.
 * @pre The LRU list is consistent.
 * @post No state is modified (selection only).
 * @post A non-negative result indexes an unpinned cell.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static int32_t priv_pick_victim(const ra8_keycache_t* kc)
{
  int32_t cur = kc->lru_tail;
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
static ra8_err_t priv_validate_cfg_ptrs(const ra8_keycache_cfg_t* cfg)
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
 * @pre The config pointers passed ::priv_validate_cfg_ptrs.
 * @post No state is modified (pure validation).
 * @post A non-ok return means a sizing field was zero.
 *
 * @note Not thread-safe with respect to concurrent config mutation.
 * @since 0.1.0
 */
static ra8_err_t priv_validate_cfg_sizes(const ra8_keycache_cfg_t* cfg)
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
 * @brief Seed a validated cache: clear buckets and link every cell cold.
 *
 * @details Zero-fills @p kc, copies the (already-validated) config in, resets the
 *          LRU endpoints, clears every hash bucket head to -1, and threads each
 *          cell -- invalid, unpinned, unchained -- onto the LRU list via
 *          ::priv_push_head. Both loops are bounded by config counts
 *          (NASA P10 Rule 2).
 *
 * @param[out] kc  Cache state to populate (caller-owned).
 * @param[in]  cfg Validated storage + renderer configuration (non-NULL).
 *
 * @return Nothing.
 *
 * @pre @p cfg passed ::priv_validate_cfg_ptrs and ::priv_validate_cfg_sizes.
 * @pre @p kc is a writable cache-state object.
 * @post Every bucket head is -1 and every cell is a cold LRU member.
 * @post `kc->cfg` is a copy of @p cfg with empty LRU endpoints established.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void priv_seed_cells(ra8_keycache_t* kc, const ra8_keycache_cfg_t* cfg)
{
  (void)memset(kc, 0, sizeof(*kc));
  kc->cfg      = *cfg;
  kc->lru_head = -1;
  kc->lru_tail = -1;
  for (uint32_t b = 0U; b < cfg->bucket_count; ++b) {
    cfg->buckets[b] = -1;
  }
  for (uint32_t i = 0U; i < cfg->cell_count; ++i) {
    kc->cfg.meta[i].valid     = 0U;
    kc->cfg.meta[i].pin_count = 0U;
    kc->cfg.meta[i].hash_next = -1;
    priv_push_head(kc, (int32_t)i);
  }
}

ra8_err_t ra8_keycache_init(ra8_keycache_t* kc, const ra8_keycache_cfg_t* cfg)
{
  RA8_CHECK_NULL_PTR(kc, s_tag, "kc must not be nullptr");
  RA8_CHECK_NULL_PTR(cfg, s_tag, "cfg must not be nullptr");
  const ra8_err_t perr = priv_validate_cfg_ptrs(cfg);
  if (perr != k_ra8_ok) {
    return perr;
  }
  const ra8_err_t serr = priv_validate_cfg_sizes(cfg);
  if (serr != k_ra8_ok) {
    return serr;
  }
  priv_seed_cells(kc, cfg);
  return k_ra8_ok;
}

/**
 * @brief Handle a get miss: evict a victim, render the cell, insert + pin it.
 *
 * @details Picks an unpinned victim, drops its old entry (hash remove + unlink),
 *          renders the requested cell, and on success stores the key, re-hashes,
 *          and pins it at the MRU. A render failure leaves the cell cold so no
 *          stale entry survives.
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
 * @post On success one valid, pinned cell holds the entry.
 * @post On render failure the victim is invalidated.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static ra8_err_t priv_miss(ra8_keycache_t* kc, const void* key, ra8_keycache_view_t* out_view)
{
  const int32_t v = priv_pick_victim(kc);
  if (v < 0) {
    return k_ra8_err_no_mem;
  }
  ra8_keycache_cell_t* m = &kc->cfg.meta[v];
  if (m->valid != 0U) {
    priv_hash_remove(kc, v);
    kc->evictions++;
  }
  priv_unlink(kc, v);
  uint8_t*        cell = priv_cell_ptr(kc, (uint32_t)v);
  void*           user = priv_user_ptr(kc, (uint32_t)v);
  const ra8_err_t rerr = kc->cfg.render(kc->cfg.render_ctx, key, cell, kc->cfg.cell_bytes, user);
  if (rerr != k_ra8_ok) {
    m->valid = 0U;
    priv_push_head(kc, v);
    return rerr;
  }
  (void)memcpy(priv_key_ptr(kc, (uint32_t)v), key, (size_t)kc->cfg.key_bytes);
  m->valid     = 1U;
  m->pin_count = 1U;
  priv_hash_insert(kc, v);
  priv_push_head(kc, v);
  *out_view = (ra8_keycache_view_t){.data = cell, .user = user};
  return k_ra8_ok;
}

ra8_err_t ra8_keycache_get(ra8_keycache_t* kc, const void* key, ra8_keycache_view_t* out_view)
{
  RA8_CHECK_NULL_PTR(kc, s_tag, "kc must not be nullptr");
  RA8_CHECK_NULL_PTR(key, s_tag, "key must not be nullptr");
  RA8_CHECK_NULL_PTR(out_view, s_tag, "out_view must not be nullptr");
  const int32_t f = priv_hash_lookup(kc, key);
  if (f >= 0) {
    kc->hits++;
    kc->cfg.meta[f].pin_count++;
    priv_unlink(kc, f);
    priv_push_head(kc, f);
    *out_view = (ra8_keycache_view_t){.data = priv_cell_ptr(kc, (uint32_t)f),
                                      .user = priv_user_ptr(kc, (uint32_t)f)};
    return k_ra8_ok;
  }
  kc->misses++;
  return priv_miss(kc, key, out_view);
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
