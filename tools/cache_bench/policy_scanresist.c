/**
 * @file policy_scanresist.c
 * @brief Scan-resistant eviction policies for the #147 benchmark: Segmented-LRU
 *        and SRRIP -- the deterministic, low-metadata candidates a DO-178C page
 *        cache can actually ship (no ghost lists, bounded eviction scan).
 *
 * @details
 * - **SLRU** (Segmented LRU / the 2Q family without a ghost list): a
 *   probationary and a protected LRU segment. New pages enter probationary; a
 *   second touch promotes to protected; eviction always takes the probationary
 *   LRU first. A one-time scan therefore churns only the probationary segment
 *   and cannot evict the re-referenced (protected) hot set -- the scan
 *   resistance LRU/CLOCK lack.
 * - **SRRIP** (Static Re-Reference Interval Prediction, the RRIP HW-cache
 *   family): a 2-bit re-reference prediction value per frame. Inserts predict a
 *   distant re-reference (RRPV = max-1) so scanned-once pages are evicted before
 *   re-referenced ones; a hit predicts immediate re-reference (RRPV = 0).
 *   Eviction picks an RRPV == max frame, aging all frames until one appears.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * [Ring 7 / Tooling] {World: NS}
 *
 * @since 0.1.0
 */
#include <stdlib.h>

#include "cache_bench.h"

/* ------------------------------------------------------------------ SLRU -- */

/** @brief Per-frame segment tag stored in frame meta[0]. */
typedef enum : uint8_t {
  k_slru_probation = 0U, /**< Frame is in the probationary segment. */
  k_slru_protected = 1U, /**< Frame is in the protected segment.    */
} slru_seg_t;

/**
 * @enum slru_dim_t
 * @brief Protected-segment share of the cache, in percent, plus associated
 *        scaling and metadata constants.
 * @details
 * - @ref k_slru_protected_pct is the fraction of the cache reserved for the
 *   protected (re-referenced) segment, expressed as an integer percentage.
 * - @ref k_slru_pct_full_scale is the divisor that converts a ratio expressed
 *   in the same units as @ref k_slru_protected_pct into a frame count.
 * - @ref k_slru_meta_bytes is the per-frame metadata consumed by SLRU: one
 *   uint8_t segment tag (meta[0]) plus eight bytes of linked-list indices
 *   (two int32_t values: prev and next, threaded through the shared arrays).
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_slru_protected_pct  = 75U,  /**< Protected-segment target, percent of capacity.  */
  k_slru_pct_full_scale = 100U, /**< Divisor for percent-to-frame-count conversion.  */
  k_slru_meta_bytes     = 9U,   /**< Per-frame metadata: 1 B tag + 8 B list indices. */
} slru_dim_t;

/** @brief Two LRU segments threaded through shared prev/next frame arrays. */
typedef struct {
  int32_t* prev;     /**< prev[f] toward MRU within the frame's segment. */
  int32_t* next;     /**< next[f] toward LRU within the frame's segment. */
  int32_t  pb_head;  /**< Probationary MRU, or -1.                       */
  int32_t  pb_tail;  /**< Probationary LRU (first evicted), or -1.       */
  int32_t  pt_head;  /**< Protected MRU, or -1.                          */
  int32_t  pt_tail;  /**< Protected LRU, or -1.                          */
  uint32_t pt_count; /**< Frames currently in the protected segment.     */
  uint32_t pt_cap;   /**< Protected-segment capacity.                    */
} slru_t;

/** @brief Detach @p f from the list whose head/tail pointers are given. */
static void slru_unlink(slru_t* l, int32_t f, int32_t* head, int32_t* tail)
{
  if (l->prev[f] != -1) {
    l->next[l->prev[f]] = l->next[f];
  } else if (*head == f) {
    *head = l->next[f];
  }
  if (l->next[f] != -1) {
    l->prev[l->next[f]] = l->prev[f];
  } else if (*tail == f) {
    *tail = l->prev[f];
  }
}

/** @brief Push @p f to the MRU head of the list. */
static void slru_push_head(slru_t* l, int32_t f, int32_t* head, int32_t* tail)
{
  l->prev[f] = -1;
  l->next[f] = *head;
  if (*head != -1) {
    l->prev[*head] = f;
  }
  *head = f;
  if (*tail == -1) {
    *tail = f;
  }
}

static int slru_init(cb_cache_t* c)
{
  slru_t* l = (slru_t*)calloc(1U, sizeof(slru_t));
  /* cppcheck-suppress memleak ; false positive: cppcheck 2.13 does not model
   * the C23 nullptr keyword, so it cannot see l is NULL on this path. */
  if (l == nullptr) {
    return 1;
  }
  l->prev    = (int32_t*)malloc((size_t)c->capacity * sizeof(int32_t));
  l->next    = (int32_t*)malloc((size_t)c->capacity * sizeof(int32_t));
  l->pb_head = -1;
  l->pb_tail = -1;
  l->pt_head = -1;
  l->pt_tail = -1;
  l->pt_cap  = (c->capacity * (uint32_t)k_slru_protected_pct) / (uint32_t)k_slru_pct_full_scale;
  if ((l->prev == nullptr) || (l->next == nullptr)) {
    /* The replay harness never deinits a policy whose init failed, so a
     * partial allocation must be released here, not left on policy_data. */
    free(l->prev);
    free(l->next);
    free(l);
    return 1;
  }
  c->policy_data = l;
  return 0;
}

static void slru_deinit(cb_cache_t* c)
{
  slru_t* l = (slru_t*)c->policy_data;
  if (l != nullptr) {
    free(l->prev);
    free(l->next);
    free(l);
  }
}

static void slru_insert(cb_cache_t* c, uint32_t frame)
{
  slru_t* l                = (slru_t*)c->policy_data;
  c->frames[frame].meta[0] = (uint8_t)k_slru_probation;
  slru_push_head(l, (int32_t)frame, &l->pb_head, &l->pb_tail);
}

static void slru_access(cb_cache_t* c, uint32_t frame)
{
  slru_t*       l = (slru_t*)c->policy_data;
  const int32_t f = (int32_t)frame;
  if (c->frames[frame].meta[0] == (uint8_t)k_slru_protected) {
    slru_unlink(l, f, &l->pt_head, &l->pt_tail);
    slru_push_head(l, f, &l->pt_head, &l->pt_tail);
    return;
  }
  /* Promote probationary -> protected; demote protected LRU if over cap. */
  slru_unlink(l, f, &l->pb_head, &l->pb_tail);
  if ((l->pt_cap > 0U) && (l->pt_count == l->pt_cap)) {
    const int32_t d = l->pt_tail;
    slru_unlink(l, d, &l->pt_head, &l->pt_tail);
    c->frames[d].meta[0] = (uint8_t)k_slru_probation;
    slru_push_head(l, d, &l->pb_head, &l->pb_tail);
    l->pt_count--;
  }
  c->frames[frame].meta[0] = (uint8_t)k_slru_protected;
  slru_push_head(l, f, &l->pt_head, &l->pt_tail);
  l->pt_count++;
}

static uint32_t slru_victim(cb_cache_t* c, uint32_t* scanned)
{
  slru_t* l = (slru_t*)c->policy_data;
  *scanned  = 1U;
  if (l->pb_tail != -1) {
    const int32_t f = l->pb_tail;
    slru_unlink(l, f, &l->pb_head, &l->pb_tail);
    return (uint32_t)f;
  }
  const int32_t f = l->pt_tail;
  slru_unlink(l, f, &l->pt_head, &l->pt_tail);
  l->pt_count--;
  return (uint32_t)f;
}

const cache_policy_t g_cb_policy_slru = {
  .name        = "SLRU",
  .meta_bytes  = (size_t)k_slru_meta_bytes,
  .init        = slru_init,
  .deinit      = slru_deinit,
  .on_access   = slru_access,
  .on_insert   = slru_insert,
  .pick_victim = slru_victim,
};

/* ----------------------------------------------------------------- SRRIP -- */

/** @brief RRIP re-reference prediction values (2-bit). */
typedef enum : uint8_t {
  k_rrip_near = 0U, /**< Immediate re-reference (just hit).   */
  k_rrip_long = 2U, /**< Distant re-reference (fresh insert). */
  k_rrip_max  = 3U, /**< Furthest -- the eviction candidate.  */
} rrip_rrpv_t;

/** @brief SRRIP state: a sweep hand over the frame ring. */
static int srrip_init(cb_cache_t* c)
{
  uint32_t* hand = (uint32_t*)calloc(1U, sizeof(uint32_t));
  c->policy_data = hand;
  return (hand == nullptr) ? 1 : 0;
}
static void srrip_deinit(cb_cache_t* c)
{
  free(c->policy_data);
}
static void srrip_insert(cb_cache_t* c, uint32_t frame)
{
  c->frames[frame].meta[0] = (uint8_t)k_rrip_long;
}
static void srrip_access(cb_cache_t* c, uint32_t frame)
{
  c->frames[frame].meta[0] = (uint8_t)k_rrip_near;
}
static uint32_t srrip_victim(cb_cache_t* c, uint32_t* scanned)
{
  uint32_t* hand = (uint32_t*)c->policy_data;
  uint32_t  seen = 0U;
  for (;;) {
    const uint32_t f = *hand;
    *hand            = (*hand + 1U) % c->capacity;
    seen++;
    if (c->frames[f].meta[0] == (uint8_t)k_rrip_max) {
      *scanned = seen;
      return f;
    }
    /* Age toward the max so a victim is guaranteed within two ring passes. */
    if (c->frames[f].meta[0] < (uint8_t)k_rrip_max) {
      c->frames[f].meta[0]++;
    }
  }
}

const cache_policy_t g_cb_policy_srrip = {
  .name        = "SRRIP",
  .meta_bytes  = 1U,
  .init        = srrip_init,
  .deinit      = srrip_deinit,
  .on_access   = srrip_access,
  .on_insert   = srrip_insert,
  .pick_victim = srrip_victim,
};
