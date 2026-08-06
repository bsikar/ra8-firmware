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
 *
 * [Ring 7 / Tooling] {World: NS}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 *
 *

 */
#include <stdlib.h>

#include "cache_bench.h"
#include "ra8_attributes.h"

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

/**
 * @brief Detach @p f from the list whose head/tail pointers are given.
 *
 * @details The generic unlink used for both SLRU segments: it repairs the
 *          neighbours' `prev`/`next` links and advances @p head / @p tail
 *          inward when @p f is an endpoint, so one routine serves the
 *          probationary and protected lists sharing the frame arrays.
 *
 * @param[in,out] l    Segment pair holding the shared `prev`/`next` arrays.
 * @param[in]     f    Frame index to detach (a member of *head..*tail).
 * @param[in,out] head The segment's head endpoint, updated if @p f was head.
 * @param[in,out] tail The segment's tail endpoint, updated if @p f was tail.
 *
 * @pre @p f is currently linked in the segment named by @p head / @p tail.
 * @pre @p head and @p tail are non-NULL and @p f < capacity.
 * @post @p f is absent from that segment; neighbour links stay consistent.
 * @post `*head`/`*tail` still name real members (or -1 if the segment emptied).
 *
 * @note Not thread-safe: mutates the shared segment lists.
 * @since 0.1.0
 */
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

/**
 * @brief Push @p f to the MRU head of the segment.
 *
 * @details The generic head-insert for both SLRU segments: sets @p f as the new
 *          head, links the former head behind it, and initializes @p tail when
 *          the segment was empty. @p f must already be detached.
 *
 * @param[in,out] l    Segment pair holding the shared `prev`/`next` arrays.
 * @param[in]     f    Frame index to insert at the head (currently detached).
 * @param[in,out] head The segment's head endpoint, set to @p f.
 * @param[in,out] tail The segment's tail endpoint, set to @p f if it was empty.
 *
 * @pre @p f is detached and @p f < capacity.
 * @pre @p head and @p tail are non-NULL.
 * @post `*head == f` and @p f precedes the former head.
 * @post `*tail == f` iff the segment was previously empty.
 *
 * @note Not thread-safe: mutates the shared segment lists.
 * @since 0.1.0
 */
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

/**
 * @brief Allocate SLRU state: two LRU segments over shared frame arrays.
 *
 * @details Allocates the ::slru_t control block and the shared `prev`/`next`
 *          index arrays, empties both segments, and sizes the protected
 *          segment at ::k_slru_protected_pct percent of capacity. On a partial
 *          allocation it frees what it took, since a failed init is never
 *          deinited by the harness.
 *
 * @param[in,out] c Cache whose `policy_data` receives the segments; capacity
 *                  sizes the index arrays and the protected cap.
 *
 * @return int 0 on success, 1 on allocation failure.
 * @retval 0 `c->policy_data` holds empty probationary + protected segments.
 * @retval 1 Out of memory; any partial allocation was freed.
 *
 * @pre @p c is non-NULL with `capacity > 0`.
 * @pre Called on the single benchmark thread.
 * @post On success both segment heads/tails are -1 and `pt_cap` is set.
 * @post On failure `c->policy_data` is untouched (nothing is leaked).
 *
 * @note Not thread-safe: allocates and stores policy state.
 * @since 0.1.0
 */
RA8_NASA_RULE_3_OK /* host-only bench: policy state */
  static int
  slru_init(cb_cache_t* c)
{
  slru_t* l = (slru_t*)calloc(1U, sizeof(slru_t));
  if (l == NULL) {
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

/**
 * @brief Release SLRU state (control block + shared index arrays).
 *
 * @details Frees the `prev`/`next` arrays and the ::slru_t when present; a NULL
 *          `policy_data` (a failed init) is tolerated.
 *
 * @param[in,out] c Cache whose SLRU `policy_data` is freed.
 *
 * @pre @p c is non-NULL.
 * @pre `c->policy_data` is a ::slru_init state or NULL.
 * @post All segment buffers are released.
 * @post `c->policy_data` is left dangling; the caller discards the cache.
 *
 * @note Not thread-safe: frees policy state.
 * @since 0.1.0
 */
static void slru_deinit(cb_cache_t* c)
{
  slru_t* l = (slru_t*)c->policy_data;
  if (l != nullptr) {
    free(l->prev);
    free(l->next);
  }
}

/**
 * @brief SLRU insert hook: admit a fresh @p frame to the probationary segment.
 *
 * @details Tags @p frame `k_slru_probation` in `meta[0]` and pushes it to the
 *          probationary MRU head. New pages always enter probationary, so a
 *          one-time scan churns only that segment. Bound as `on_insert`.
 *
 * @param[in,out] c     Cache holding the SLRU segments in `policy_data`.
 * @param[in]     frame Frame that was just (re)populated.
 *
 * @pre `c->policy_data` is a valid ::slru_init state.
 * @pre @p frame is detached and @p frame < capacity.
 * @post @p frame is the probationary-segment head, tagged probationary.
 * @post The protected segment is unchanged.
 *
 * @note Not thread-safe: mutates the shared segment lists.
 * @since 0.1.0
 */
static void slru_insert(cb_cache_t* c, uint32_t frame)
{
  slru_t* l                = (slru_t*)c->policy_data;
  c->frames[frame].meta[0] = (uint8_t)k_slru_probation;
  slru_push_head(l, (int32_t)frame, &l->pb_head, &l->pb_tail);
}

/**
 * @brief SLRU hit hook: promote or refresh @p frame on re-reference.
 *
 * @details A protected-segment hit moves the frame to the protected MRU head.
 *          A probationary hit promotes the frame to protected; if the protected
 *          segment is full, its LRU is first demoted back to probationary, so a
 *          second touch is what earns scan-resistant residency. Bound as
 *          `on_access`.
 *
 * @param[in,out] c     Cache holding the SLRU segments in `policy_data`.
 * @param[in]     frame Frame that was just hit.
 *
 * @pre `c->policy_data` is a valid ::slru_init state.
 * @pre @p frame is currently resident and linked in a segment.
 * @post @p frame is at the protected MRU head, tagged protected.
 * @post `pt_count <= pt_cap` (a demotion restored the bound if needed).
 *
 * @note Not thread-safe: mutates the shared segment lists.
 * @since 0.1.0
 */
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

/**
 * @brief Choose the SLRU victim: probationary LRU first, else protected LRU.
 *
 * @details Evicts the probationary-segment tail when it exists (scan traffic
 *          lands here), falling back to the protected tail only when the
 *          probationary segment is empty. Reports a scan depth of one -- the
 *          victim is always an O(1) tail lookup.
 *
 * @param[in,out] c       Cache holding the SLRU segments in `policy_data`.
 * @param[out]    scanned Receives the frames examined (always 1).
 *
 * @return uint32_t The victim frame index (< capacity).
 * @retval <capacity The probationary LRU, or the protected LRU if none.
 *
 * @pre `c->policy_data` is a valid ::slru_init state with a resident frame.
 * @pre @p scanned is non-NULL.
 * @post `*scanned == 1` and the victim is unlinked from its segment.
 * @post `pt_count` drops by one only when a protected frame was evicted.
 *
 * @note Not thread-safe: mutates the shared segment lists.
 * @since 0.1.0
 */
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

/**
 * @brief Allocate SRRIP state: a sweep hand over the frame ring.
 *
 * @details Allocates one zeroed `uint32_t` aging hand in `c->policy_data`; each
 *          frame's 2-bit re-reference prediction value (RRPV) lives in
 *          `meta[0]`, set on insert/access and aged toward the max during
 *          eviction.
 *
 * @param[in,out] c Cache whose `policy_data` receives the hand pointer.
 *
 * @return int 0 on success, 1 on allocation failure.
 * @retval 0 `c->policy_data` holds a zeroed hand.
 * @retval 1 Out of memory; `c->policy_data` is NULL.
 *
 * @pre @p c is non-NULL and its `policy_data` is unset.
 * @pre Called on the single benchmark thread.
 * @post On success `c->policy_data` points at a zero-initialized hand.
 * @post No frame contents are altered.
 *
 * @note Not thread-safe: allocates and stores policy state.
 * @since 0.1.0
 */
RA8_NASA_RULE_3_OK /* host-only bench: policy state */
  static int
  srrip_init(cb_cache_t* c)
{
  uint32_t* hand = (uint32_t*)calloc(1U, sizeof(uint32_t));
  c->policy_data = hand;
  return (hand == nullptr) ? 1 : 0;
}
/**
 * @brief Release SRRIP state (the sweep hand).
 *
 * @details Frees the `uint32_t` hand ::srrip_init allocated; `free(NULL)` is
 *          safe after a failed init.
 *
 * @param[in,out] c Cache whose `policy_data` hand is freed.
 *
 * @pre @p c is non-NULL.
 * @pre `c->policy_data` is a ::srrip_init hand or NULL.
 * @post The hand memory is released.
 * @post `c->policy_data` is left dangling; the caller discards the cache.
 *
 * @note Not thread-safe: frees policy state.
 * @since 0.1.0
 */
static void srrip_deinit(cb_cache_t* c)
{
  free(c->policy_data);
}
/**
 * @brief SRRIP insert hook: predict a distant re-reference for @p frame.
 *
 * @details Sets @p frame's RRPV to ::k_rrip_long (max-1) in `meta[0]`, so a
 *          scanned-once page sits one aging step from eviction and is reclaimed
 *          before any re-referenced page. Bound as `on_insert`.
 *
 * @param[in,out] c     Cache whose frame RRPV is set.
 * @param[in]     frame Frame that was just (re)populated.
 *
 * @pre @p c is non-NULL and @p frame < capacity.
 * @pre @p frame is currently resident.
 * @post `frames[frame].meta[0] == k_rrip_long`.
 * @post No other frame or policy state changes.
 *
 * @note Not thread-safe: writes shared frame metadata.
 * @since 0.1.0
 */
static void srrip_insert(cb_cache_t* c, uint32_t frame)
{
  c->frames[frame].meta[0] = (uint8_t)k_rrip_long;
}
/**
 * @brief SRRIP hit hook: predict an immediate re-reference for @p frame.
 *
 * @details Resets @p frame's RRPV to ::k_rrip_near (0) in `meta[0]`, marking a
 *          re-referenced page as the furthest from eviction. Bound as
 *          `on_access`.
 *
 * @param[in,out] c     Cache whose frame RRPV is reset.
 * @param[in]     frame Frame that was just hit.
 *
 * @pre @p c is non-NULL and @p frame < capacity.
 * @pre @p frame is currently resident.
 * @post `frames[frame].meta[0] == k_rrip_near`.
 * @post No other frame or policy state changes.
 *
 * @note Not thread-safe: writes shared frame metadata.
 * @since 0.1.0
 */
static void srrip_access(cb_cache_t* c, uint32_t frame)
{
  c->frames[frame].meta[0] = (uint8_t)k_rrip_near;
}
/**
 * @brief Choose the SRRIP victim by aging RRPVs to the maximum.
 *
 * @details Sweeps the hand around the ring: the first frame at RRPV
 *          ::k_rrip_max is evicted; every frame below max is aged up by one on
 *          the way. Because inserts start at max-1, a victim is guaranteed
 *          within two ring passes. Reports the frames examined.
 *
 * @param[in,out] c       Cache holding the sweep hand in `policy_data`.
 * @param[out]    scanned Receives the frames examined this call (>= 1).
 *
 * @return uint32_t The victim frame index (< capacity).
 * @retval <capacity The first frame reached at RRPV ::k_rrip_max.
 *
 * @pre `c->policy_data` is a valid ::srrip_init hand and `capacity > 0`.
 * @pre @p scanned is non-NULL.
 * @post `*scanned` equals the frames inspected and the hand advanced past them.
 * @post Frames passed over below max had their RRPV incremented.
 *
 * @note Not thread-safe: advances the hand and ages RRPVs.
 * @since 0.1.0
 */
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
