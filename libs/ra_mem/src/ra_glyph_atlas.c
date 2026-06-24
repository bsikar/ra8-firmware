/**
 * @file ra_glyph_atlas.c
 * @brief Fixed-budget glyph cache -- implementation (Layer 3, #147).
 *
 * @par Tag
 * [Ring 2 / Core] {World: NS}
 *
 * @details
 * A keyed LRU cache over fixed cells, structurally like ::ra_vmem but with a
 * single LRU list (glyph reuse has strong per-page locality, so scan resistance
 * is unnecessary here) and a 4-field glyph key. All cells start invalid in the
 * LRU list, so cold misses and real evictions share one victim path; victim
 * selection is read-only and skips pinned cells.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_glyph_atlas.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ra_check.h"
#include "ra_err.h"

/** @brief Module log tag. */
static const char* const s_tag = "ra_glyph_atlas";

/** @brief Hash mixing constants for the glyph key. */
typedef enum : uint32_t {
  k_glyph_hash_mul_id   = 2654435761U, /**< Knuth multiplicative hash (glyph id). */
  k_glyph_hash_mul_mode = 40503U,      /**< Odd multiplier mixing the mode.       */
  k_glyph_face_shift    = 16U,         /**< Face/size packing shift.              */
} ra_glyph_hash_const_t;

/** @brief Bitmap storage pointer for cell @p idx. */
static uint8_t* priv_cell_ptr(const ra_glyph_atlas_t* atlas, uint32_t idx)
{
  return &atlas->cfg.cell_mem[(size_t)idx * (size_t)atlas->cfg.cell_bytes];
}

/**
 * @brief Field-wise glyph-key equality (no padding compared).
 *
 * @details Compares the four key fields with independent single-condition tests
 *          so padding bytes never affect the result.
 *
 * @param[in] a First key.
 * @param[in] b Second key.
 *
 * @return true if every field matches, else false.
 * @retval true  All four fields are equal.
 * @retval false Some field differs.
 *
 * @pre `a` and `b` are non-NULL.
 * @pre Both point at populated keys.
 * @post No state is modified.
 * @post The result depends only on the field values.
 *
 * @note Pure; thread-safe.
 * @since 0.1.0
 */
static bool priv_key_eq(const ra_glyph_key_t* a, const ra_glyph_key_t* b)
{
  if (a->glyph_id != b->glyph_id) {
    return false;
  }
  if (a->face_id != b->face_id) {
    return false;
  }
  if (a->size_px != b->size_px) {
    return false;
  }
  if (a->mode != b->mode) {
    return false;
  }
  return true;
}

/**
 * @brief Hash a glyph key into a bucket index.
 *
 * @details Mixes the glyph id, packed face/size, and render mode through odd
 *          multipliers and folds to `[0, bucket_count)`.
 *
 * @param[in] atlas Atlas providing the bucket count.
 * @param[in] k     Glyph key to hash.
 *
 * @return Bucket index in `[0, bucket_count)`.
 * @retval 0 The key folded into the first bucket (one possible result).
 *
 * @pre `atlas` is non-NULL with `bucket_count > 0`.
 * @pre `k` is non-NULL.
 * @post No state is modified.
 * @post The result is strictly less than `bucket_count`.
 *
 * @note Pure; thread-safe.
 * @since 0.1.0
 */
static uint32_t priv_hash(const ra_glyph_atlas_t* atlas, const ra_glyph_key_t* k)
{
  uint32_t h = k->glyph_id * (uint32_t)k_glyph_hash_mul_id;
  h ^= ((uint32_t)k->face_id << (uint32_t)k_glyph_face_shift) | (uint32_t)k->size_px;
  h ^= (uint32_t)k->mode * (uint32_t)k_glyph_hash_mul_mode;
  return h % atlas->cfg.bucket_count;
}

/**
 * @brief Detach cell @p f from the LRU list.
 *
 * @details Splices @p f out of the list, fixing neighbours and the head/tail.
 *
 * @param[in,out] atlas Atlas providing the cell metadata.
 * @param[in]     f     Cell index to unlink.
 *
 * @return Nothing.
 *
 * @pre `f` is currently a member of the LRU list.
 * @pre `atlas` is non-NULL with a populated config.
 * @post `f`'s neighbours and head/tail no longer reference `f`.
 * @post Other cells are unmodified.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void priv_unlink(ra_glyph_atlas_t* atlas, int32_t f)
{
  ra_glyph_cell_t* m = atlas->cfg.meta;
  if (m[f].prev != -1) {
    m[m[f].prev].next = m[f].next;
  } else if (atlas->lru_head == f) {
    atlas->lru_head = m[f].next;
  }
  if (m[f].next != -1) {
    m[m[f].next].prev = m[f].prev;
  } else if (atlas->lru_tail == f) {
    atlas->lru_tail = m[f].prev;
  }
}

/**
 * @brief Push cell @p f onto the MRU head of the LRU list.
 *
 * @details Links @p f as the most-recently-used cell, setting the tail when the
 *          list was empty.
 *
 * @param[in,out] atlas Atlas providing the cell metadata.
 * @param[in]     f     Cell index to insert (must be detached).
 *
 * @return Nothing.
 *
 * @pre `f` is not currently linked into the list.
 * @pre `atlas` is non-NULL with a populated config.
 * @post `f` is the MRU head.
 * @post The tail points at `f` iff the list was previously empty.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void priv_push_head(ra_glyph_atlas_t* atlas, int32_t f)
{
  ra_glyph_cell_t* m = atlas->cfg.meta;
  m[f].prev          = -1;
  m[f].next          = atlas->lru_head;
  if (atlas->lru_head != -1) {
    m[atlas->lru_head].prev = f;
  }
  atlas->lru_head = f;
  if (atlas->lru_tail == -1) {
    atlas->lru_tail = f;
  }
}

/**
 * @brief Insert cell @p f into its hash bucket chain.
 *
 * @details Prepends @p f to the chain of the bucket its key hashes to.
 *
 * @param[in,out] atlas Atlas providing buckets + metadata.
 * @param[in]     f     Cell index (its key is set).
 *
 * @return Nothing.
 *
 * @pre `f`'s key is set and it is not already chained.
 * @pre `atlas` is non-NULL with a populated config.
 * @post `f` is the head of its bucket chain.
 * @post The previous head follows `f`.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void priv_hash_insert(ra_glyph_atlas_t* atlas, int32_t f)
{
  const uint32_t b            = priv_hash(atlas, &atlas->cfg.meta[f].key);
  atlas->cfg.meta[f].hash_next = atlas->cfg.buckets[b];
  atlas->cfg.buckets[b]        = f;
}

/**
 * @brief Remove cell @p f from its hash bucket chain.
 *
 * @details Unlinks @p f from its bucket, handling the head and mid-chain cases.
 *
 * @param[in,out] atlas Atlas providing buckets + metadata.
 * @param[in]     f     Cell index currently chained.
 *
 * @return Nothing.
 *
 * @pre `f` is currently present in its bucket chain.
 * @pre `atlas` is non-NULL with a populated config.
 * @post `f` is no longer reachable from the bucket.
 * @post `f`'s `hash_next` is reset to -1.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void priv_hash_remove(ra_glyph_atlas_t* atlas, int32_t f)
{
  const uint32_t b   = priv_hash(atlas, &atlas->cfg.meta[f].key);
  int32_t        cur = atlas->cfg.buckets[b];
  if (cur == f) {
    atlas->cfg.buckets[b] = atlas->cfg.meta[f].hash_next;
  } else {
    while (cur != -1) {
      if (atlas->cfg.meta[cur].hash_next == f) {
        atlas->cfg.meta[cur].hash_next = atlas->cfg.meta[f].hash_next;
        break;
      }
      cur = atlas->cfg.meta[cur].hash_next;
    }
  }
  atlas->cfg.meta[f].hash_next = -1;
}

/**
 * @brief Find the valid cell holding @p key, or -1.
 *
 * @details Walks the key's bucket chain for a valid cell with a matching key.
 *
 * @param[in] atlas Atlas providing buckets + metadata.
 * @param[in] key   Glyph key to find.
 *
 * @return The matching cell index, or -1 if not resident.
 * @retval -1 No valid cell holds the key.
 *
 * @pre `atlas` and `key` are non-NULL.
 * @pre `atlas` has a populated config.
 * @post No state is modified.
 * @post A non-negative result indexes a valid cell with the key.
 *
 * @note Not thread-safe with respect to concurrent mutation.
 * @since 0.1.0
 */
static int32_t priv_hash_lookup(const ra_glyph_atlas_t* atlas, const ra_glyph_key_t* key)
{
  const uint32_t b   = priv_hash(atlas, key);
  int32_t        cur = atlas->cfg.buckets[b];
  while (cur != -1) {
    const ra_glyph_cell_t* m = &atlas->cfg.meta[cur];
    if (m->valid != 0U) {
      if (priv_key_eq(&m->key, key)) {
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
 *          the cell count (NASA P10 Rule 2).
 *
 * @param[in] atlas Atlas providing the LRU list + metadata.
 *
 * @return The cell index to evict, or -1 if every cell is pinned.
 * @retval -1 No evictable cell (atlas fully pinned).
 *
 * @pre `atlas` is non-NULL with a populated config.
 * @pre The LRU list is consistent.
 * @post No state is modified (selection only).
 * @post A non-negative result indexes an unpinned cell.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static int32_t priv_pick_victim(const ra_glyph_atlas_t* atlas)
{
  int32_t cur = atlas->lru_tail;
  while (cur != -1) {
    if (atlas->cfg.meta[cur].pin_count == 0U) {
      return cur;
    }
    cur = atlas->cfg.meta[cur].prev;
  }
  return -1;
}

ra_err_t ra_glyph_atlas_init(ra_glyph_atlas_t* atlas, const ra_glyph_atlas_cfg_t* cfg)
{
  RA_CHECK_NULL_PTR(atlas, s_tag, "atlas must not be nullptr");
  RA_CHECK_NULL_PTR(cfg, s_tag, "cfg must not be nullptr");
  RA_CHECK_NULL_PTR(cfg->cell_mem, s_tag, "cell_mem must not be nullptr");
  RA_CHECK_NULL_PTR(cfg->meta, s_tag, "meta must not be nullptr");
  RA_CHECK_NULL_PTR(cfg->buckets, s_tag, "buckets must not be nullptr");
  RA_CHECK_NULL_PTR(cfg->render, s_tag, "render must not be nullptr");
  if (cfg->cell_count == 0U) {
    return k_ra_err_invalid_size;
  }
  if (cfg->cell_bytes == 0U) {
    return k_ra_err_invalid_size;
  }
  if (cfg->bucket_count == 0U) {
    return k_ra_err_invalid_size;
  }
  (void)memset(atlas, 0, sizeof(*atlas));
  atlas->cfg      = *cfg;
  atlas->lru_head = -1;
  atlas->lru_tail = -1;
  for (uint32_t b = 0U; b < cfg->bucket_count; ++b) {
    cfg->buckets[b] = -1;
  }
  for (uint32_t i = 0U; i < cfg->cell_count; ++i) {
    atlas->cfg.meta[i].valid     = 0U;
    atlas->cfg.meta[i].pin_count = 0U;
    atlas->cfg.meta[i].hash_next = -1;
    priv_push_head(atlas, (int32_t)i);
  }
  return k_ra_ok;
}

/**
 * @brief Handle a get miss: evict a victim, render the glyph, insert + pin it.
 *
 * @details Picks an unpinned victim, drops its old glyph (hash remove + unlink),
 *          renders the requested glyph into the cell, and on success re-keys,
 *          re-hashes, and pins it at the MRU. A render failure leaves the cell
 *          cold so no stale glyph survives.
 *
 * @param[in,out] atlas     Initialised atlas.
 * @param[in]     key       Glyph to render.
 * @param[out]    out_glyph Receives the pinned glyph view.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok         Glyph rendered, inserted, and pinned.
 * @retval k_ra_err_no_mem Every cell is pinned.
 * @retval k_ra_err_*      The renderer's error (the victim is dropped, cold).
 *
 * @pre `atlas`, `key`, `out_glyph` are non-NULL; the key is not resident.
 * @pre A cell is evictable unless all are pinned.
 * @post On success one valid, pinned cell holds the glyph.
 * @post On render failure the victim is invalidated.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static ra_err_t priv_glyph_miss(ra_glyph_atlas_t*     atlas,
                                const ra_glyph_key_t* key,
                                ra_glyph_t*           out_glyph)
{
  const int32_t v = priv_pick_victim(atlas);
  if (v < 0) {
    return k_ra_err_no_mem;
  }
  ra_glyph_cell_t* m = &atlas->cfg.meta[v];
  if (m->valid != 0U) {
    priv_hash_remove(atlas, v);
    atlas->evictions++;
  }
  priv_unlink(atlas, v);
  uint16_t        w    = 0;
  uint16_t        h    = 0;
  uint8_t*        cell = priv_cell_ptr(atlas, (uint32_t)v);
  const ra_err_t  rerr = atlas->cfg.render(atlas->cfg.render_ctx, key, cell,
                                          atlas->cfg.cell_bytes, &w, &h);
  if (rerr != k_ra_ok) {
    m->valid = 0U;
    priv_push_head(atlas, v);
    return rerr;
  }
  m->key       = *key;
  m->width     = w;
  m->height    = h;
  m->valid     = 1U;
  m->pin_count = 1U;
  priv_hash_insert(atlas, v);
  priv_push_head(atlas, v);
  *out_glyph = (ra_glyph_t){.bitmap = cell, .width = w, .height = h};
  return k_ra_ok;
}

ra_err_t ra_glyph_atlas_get(ra_glyph_atlas_t* atlas, const ra_glyph_key_t* key,
                            ra_glyph_t* out_glyph)
{
  RA_CHECK_NULL_PTR(atlas, s_tag, "atlas must not be nullptr");
  RA_CHECK_NULL_PTR(key, s_tag, "key must not be nullptr");
  RA_CHECK_NULL_PTR(out_glyph, s_tag, "out_glyph must not be nullptr");
  const int32_t f = priv_hash_lookup(atlas, key);
  if (f >= 0) {
    atlas->hits++;
    atlas->cfg.meta[f].pin_count++;
    priv_unlink(atlas, f);
    priv_push_head(atlas, f);
    *out_glyph = (ra_glyph_t){.bitmap = priv_cell_ptr(atlas, (uint32_t)f),
                              .width   = atlas->cfg.meta[f].width,
                              .height  = atlas->cfg.meta[f].height};
    return k_ra_ok;
  }
  atlas->misses++;
  return priv_glyph_miss(atlas, key, out_glyph);
}

ra_err_t ra_glyph_atlas_put(ra_glyph_atlas_t* atlas, const uint8_t* bitmap)
{
  RA_CHECK_NULL_PTR(atlas, s_tag, "atlas must not be nullptr");
  RA_CHECK_NULL_PTR(bitmap, s_tag, "bitmap must not be nullptr");
  const uintptr_t base = (uintptr_t)atlas->cfg.cell_mem;
  const uintptr_t addr = (uintptr_t)bitmap;
  if (addr < base) {
    return k_ra_err_invalid_arg;
  }
  const uintptr_t off  = addr - base;
  const uintptr_t span = (uintptr_t)atlas->cfg.cell_count * (uintptr_t)atlas->cfg.cell_bytes;
  if (off >= span) {
    return k_ra_err_invalid_arg;
  }
  if ((off % (uintptr_t)atlas->cfg.cell_bytes) != 0U) {
    return k_ra_err_invalid_arg;
  }
  const uint32_t idx = (uint32_t)(off / (uintptr_t)atlas->cfg.cell_bytes);
  if (atlas->cfg.meta[idx].pin_count == 0U) {
    return k_ra_err_invalid_arg;
  }
  atlas->cfg.meta[idx].pin_count--;
  return k_ra_ok;
}

ra_err_t ra_glyph_atlas_stats(const ra_glyph_atlas_t* atlas, uint32_t* out_hits,
                              uint32_t* out_misses, uint32_t* out_evictions)
{
  RA_CHECK_NULL_PTR(atlas, s_tag, "atlas must not be nullptr");
  if (atlas->cfg.cell_mem == nullptr) {
    return k_ra_err_invalid_state;
  }
  if (out_hits != nullptr) {
    *out_hits = atlas->hits;
  }
  if (out_misses != nullptr) {
    *out_misses = atlas->misses;
  }
  if (out_evictions != nullptr) {
    *out_evictions = atlas->evictions;
  }
  return k_ra_ok;
}
