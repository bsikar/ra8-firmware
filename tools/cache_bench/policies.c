/**
 * @file policies.c
 * @brief Reference eviction policies + the registry for the #147 benchmark.
 *
 * @details Baselines the scan-resistant candidates must beat: FIFO and Random
 * (no recency), true LRU (good on locality, thrashes on linear scan), and CLOCK
 * (the standard embedded second-chance LRU approximation). The scan-resistant
 * policies (2Q / Segmented-LRU, CLOCK-Pro, CAR) live in their own TUs and are
 * appended to ::g_cb_policies.
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

/* ------------------------------------------------------------------ FIFO -- */

/**
 * @brief Allocate FIFO state: a single round-robin hand over the frame ring.
 *
 * @details Allocates one zeroed `uint32_t` insertion hand and stores it in
 *          `c->policy_data`; the hand advances modulo capacity on each victim
 *          pick, giving pure first-in-first-out order with no recency bits.
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
  cb_fifo_init(cb_cache_t* c)
{
  uint32_t* hand = (uint32_t*)calloc(1U, sizeof(uint32_t));
  c->policy_data = hand;
  return (hand == nullptr) ? 1 : 0;
}
/**
 * @brief Release FIFO state (the insertion hand).
 *
 * @details Frees the `uint32_t` hand ::cb_fifo_init allocated; `free(NULL)` is
 *          safe, so calling after a failed init is harmless.
 *
 * @param[in,out] c Cache whose `policy_data` hand is freed.
 *
 * @pre @p c is non-NULL.
 * @pre `c->policy_data` is a ::cb_fifo_init hand or NULL.
 * @post The hand memory is released.
 * @post `c->policy_data` is left dangling; the caller discards the cache.
 *
 * @note Not thread-safe: frees policy state.
 * @since 0.1.0
 */
static void cb_fifo_deinit(cb_cache_t* c)
{
  free(c->policy_data);
}
/**
 * @brief Choose the FIFO victim: the frame the hand currently points at.
 *
 * @details Returns the frame under the round-robin hand, then advances the hand
 *          modulo capacity, so frames are evicted in insertion order. Reports a
 *          scan depth of exactly one (O(1), the ideal WCET).
 *
 * @param[in,out] c       Cache holding the FIFO hand in `policy_data`.
 * @param[out]    scanned Receives the frames examined (always 1).
 *
 * @return uint32_t The victim frame index (< capacity).
 * @retval <capacity The frame the hand pointed at on entry.
 *
 * @pre `c->policy_data` is a valid ::cb_fifo_init hand.
 * @pre @p scanned is non-NULL and `c->capacity > 0`.
 * @post `*scanned == 1`.
 * @post The hand has advanced by one (mod capacity).
 *
 * @note Not thread-safe: advances the shared hand.
 * @since 0.1.0
 */
static uint32_t cb_fifo_victim(cb_cache_t* c, uint32_t* scanned)
{
  uint32_t*      hand = (uint32_t*)c->policy_data;
  const uint32_t f    = *hand;
  *hand               = (*hand + 1U) % c->capacity;
  *scanned            = 1U;
  return f;
}
static const cache_policy_t s_cb_policy_fifo = {
  .name        = "FIFO",
  .meta_bytes  = 0U,
  .init        = cb_fifo_init,
  .deinit      = cb_fifo_deinit,
  .on_access   = nullptr,
  .on_insert   = nullptr,
  .pick_victim = cb_fifo_victim,
};

/* ---------------------------------------------------------------- Random -- */

/**
 * @enum cb_rand_seed_t
 * @brief Initial PRNG seed for the Random eviction policy.
 * @details A fixed non-zero 64-bit value that initialises the xorshift64
 *          state so benchmark runs are reproducible. Any non-zero odd value
 *          would work; this one is the splitmix64 gamma constant, chosen for
 *          good bit-distribution as a starting state.
 * @since 0.1.0
 */
typedef enum : uint64_t {
  k_rand_seed =
    0x123456789ABCDEF0ULL, /**< MAGIC-OK: fixed xorshift64 PRNG seed for reproducibility */
} cb_rand_seed_t;

/**
 * @enum cb_rand_shift_t
 * @brief xorshift64 shift-amount triple used in the Random policy's PRNG step.
 * @details The triple (13, 7, 17) is one of the parameter sets listed in
 *          Marsaglia (2003) for a full-period 64-bit xorshift generator.
 *          Changing any value breaks the period guarantee.
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_rand_shift_a = 13U, /**< MAGIC-OK: xorshift64 left-shift a (Marsaglia 2003 set)  */
  k_rand_shift_b = 7U,  /**< MAGIC-OK: xorshift64 right-shift b (Marsaglia 2003 set) */
  k_rand_shift_c = 17U, /**< MAGIC-OK: xorshift64 left-shift c (Marsaglia 2003 set)  */
} cb_rand_shift_t;

/**
 * @brief Allocate Random-policy state: a deterministic xorshift64 seed.
 *
 * @details Allocates one `uint64_t` seeded with the fixed ::k_rand_seed so
 *          eviction choices are pseudo-random yet reproducible across runs and
 *          hosts. Stored in `c->policy_data`.
 *
 * @param[in,out] c Cache whose `policy_data` receives the seed pointer.
 *
 * @return int 0 on success, 1 on allocation failure.
 * @retval 0 `c->policy_data` holds the seeded PRNG state.
 * @retval 1 Out of memory; `c->policy_data` is NULL.
 *
 * @pre @p c is non-NULL and its `policy_data` is unset.
 * @pre Called on the single benchmark thread.
 * @post On success `c->policy_data` points at state seeded with ::k_rand_seed.
 * @post No frame contents are altered.
 *
 * @note Not thread-safe: allocates and stores policy state.
 * @since 0.1.0
 */
RA8_NASA_RULE_3_OK /* host-only bench: policy state */
  static int
  cb_rand_init(cb_cache_t* c)
{
  uint64_t* s = (uint64_t*)malloc(sizeof(uint64_t));
  if (s != nullptr) {
    *s = (uint64_t)k_rand_seed;
  }
  c->policy_data = s;
  return (s == nullptr) ? 1 : 0;
}
/**
 * @brief Release Random-policy state (the PRNG seed).
 *
 * @details Frees the `uint64_t` seed ::cb_rand_init allocated; `free(NULL)` is
 *          safe after a failed init.
 *
 * @param[in,out] c Cache whose `policy_data` seed is freed.
 *
 * @pre @p c is non-NULL.
 * @pre `c->policy_data` is a ::cb_rand_init seed or NULL.
 * @post The seed memory is released.
 * @post `c->policy_data` is left dangling; the caller discards the cache.
 *
 * @note Not thread-safe: frees policy state.
 * @since 0.1.0
 */
static void cb_rand_deinit(cb_cache_t* c)
{
  free(c->policy_data);
}
/**
 * @brief Choose a uniformly random victim frame.
 *
 * @details Advances the xorshift64 state one step and returns `x % capacity`,
 *          so any resident frame is equally likely regardless of recency.
 *          Reports a scan depth of one (the choice is O(1)).
 *
 * @param[in,out] c       Cache holding the PRNG seed in `policy_data`.
 * @param[out]    scanned Receives the frames examined (always 1).
 *
 * @return uint32_t The victim frame index (< capacity).
 * @retval <capacity A pseudo-random resident frame.
 *
 * @pre `c->policy_data` is a valid ::cb_rand_init seed.
 * @pre @p scanned is non-NULL and `c->capacity > 0`.
 * @post `*scanned == 1` and the PRNG state has advanced one step.
 * @post No frame contents are altered.
 *
 * @note Not thread-safe: advances the shared PRNG state.
 * @since 0.1.0
 */
static uint32_t cb_rand_victim(cb_cache_t* c, uint32_t* scanned)
{
  uint64_t* s = (uint64_t*)c->policy_data;
  uint64_t  x = *s;
  x ^= x << (uint8_t)k_rand_shift_a;
  x ^= x >> (uint8_t)k_rand_shift_b;
  x ^= x << (uint8_t)k_rand_shift_c;
  *s       = x;
  *scanned = 1U;
  return (uint32_t)(x % (uint64_t)c->capacity);
}
static const cache_policy_t s_cb_policy_random = {
  .name        = "Random",
  .meta_bytes  = 0U,
  .init        = cb_rand_init,
  .deinit      = cb_rand_deinit,
  .on_access   = nullptr,
  .on_insert   = nullptr,
  .pick_victim = cb_rand_victim,
};

/* ------------------------------------------------------------------- LRU -- */

/** @brief LRU recency list threaded through prev/next frame-index arrays. */
typedef struct {
  int32_t* prev; /**< prev[f] -- frame nearer MRU, or -1.     */
  int32_t* next; /**< next[f] -- frame nearer LRU, or -1.     */
  int32_t  head; /**< Most-recently-used frame, or -1.        */
  int32_t  tail; /**< Least-recently-used frame (the victim). */
} cb_lru_t;

/**
 * @brief Allocate true-LRU state: a doubly-linked recency list over frames.
 *
 * @details Allocates the ::cb_lru_t control block plus `prev`/`next` index
 *          arrays (one entry per frame) and marks the list empty (head/tail
 *          -1). On a partial allocation it frees what it took and reports
 *          failure, since the harness never deinits a policy whose init failed.
 *
 * @param[in,out] c Cache whose `policy_data` receives the list; capacity sizes
 *                  the `prev`/`next` arrays.
 *
 * @return int 0 on success, 1 on allocation failure.
 * @retval 0 `c->policy_data` holds an empty recency list.
 * @retval 1 Out of memory; any partial allocation was freed.
 *
 * @pre @p c is non-NULL with `capacity > 0`.
 * @pre Called on the single benchmark thread.
 * @post On success `c->policy_data` is a list with head == tail == -1.
 * @post On failure `c->policy_data` is untouched (nothing is leaked).
 *
 * @note Not thread-safe: allocates and stores policy state.
 * @since 0.1.0
 */
RA8_NASA_RULE_3_OK /* host-only bench: policy state */
  static int
  cb_lru_init(cb_cache_t* c)
{
  cb_lru_t* l = (cb_lru_t*)calloc(1U, sizeof(cb_lru_t));
  if (l == NULL) {
    return 1;
  }
  l->prev = (int32_t*)malloc((size_t)c->capacity * sizeof(int32_t));
  l->next = (int32_t*)malloc((size_t)c->capacity * sizeof(int32_t));
  l->head = -1;
  l->tail = -1;
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
 * @brief Release true-LRU state (control block + index arrays).
 *
 * @details Frees the `prev`/`next` arrays and the ::cb_lru_t itself when
 *          present; a NULL `policy_data` (a failed init) is tolerated.
 *
 * @param[in,out] c Cache whose LRU `policy_data` is freed.
 *
 * @pre @p c is non-NULL.
 * @pre `c->policy_data` is a ::cb_lru_init list or NULL.
 * @post All list buffers are released.
 * @post `c->policy_data` is left dangling; the caller discards the cache.
 *
 * @note Not thread-safe: frees policy state.
 * @since 0.1.0
 */
static void cb_lru_deinit(cb_cache_t* c)
{
  cb_lru_t* l = (cb_lru_t*)c->policy_data;
  if (l != nullptr) {
    free(l->prev);
    free(l->next);
  }
}
/**
 * @brief Unlink frame @p f from the recency list.
 *
 * @details Repairs its neighbours' `prev`/`next` links and, when @p f was the
 *          head or tail, advances that endpoint inward, leaving the list
 *          consistent with @p f detached.
 *
 * @param[in,out] l The recency list to edit.
 * @param[in]     f Frame index to detach (must be a member).
 *
 * @pre @p l is a valid list and @p f is currently linked in it.
 * @pre @p f is a valid frame index < capacity.
 * @post @p f is absent from the list; neighbour links stay consistent.
 * @post `l->head`/`l->tail` still name real members (or -1 if now empty).
 *
 * @note Not thread-safe: mutates the shared list.
 * @since 0.1.0
 */
static void cb_lru_unlink(cb_lru_t* l, int32_t f)
{
  if (l->prev[f] != -1) {
    l->next[l->prev[f]] = l->next[f];
  } else if (l->head == f) {
    l->head = l->next[f];
  }
  if (l->next[f] != -1) {
    l->prev[l->next[f]] = l->prev[f];
  } else if (l->tail == f) {
    l->tail = l->prev[f];
  }
}
/**
 * @brief Splice frame @p f to the MRU (most-recently-used) head.
 *
 * @details Makes @p f the new head, linking the former head behind it and
 *          setting the tail to @p f when the list was empty. @p f must already
 *          be detached (see ::cb_lru_unlink).
 *
 * @param[in,out] l The recency list to edit.
 * @param[in]     f Frame index to place at the head.
 *
 * @pre @p l is a valid list and @p f is currently detached.
 * @pre @p f is a valid frame index < capacity.
 * @post `l->head == f` and @p f precedes the former head.
 * @post `l->tail` names @p f iff the list was previously empty.
 *
 * @note Not thread-safe: mutates the shared list.
 * @since 0.1.0
 */
static void cb_lru_to_head(cb_lru_t* l, int32_t f)
{
  l->prev[f] = -1;
  l->next[f] = l->head;
  if (l->head != -1) {
    l->prev[l->head] = f;
  }
  l->head = f;
  if (l->tail == -1) {
    l->tail = f;
  }
}
/**
 * @brief LRU hit hook: move the just-accessed @p frame to the MRU head.
 *
 * @details Unlinks @p frame from its current position and re-inserts it at the
 *          head, so the least-recently-used frame stays at the tail (the next
 *          victim). Bound as the policy's `on_access`.
 *
 * @param[in,out] c     Cache holding the LRU list in `policy_data`.
 * @param[in]     frame Frame that was just hit.
 *
 * @pre `c->policy_data` is a valid ::cb_lru_init list.
 * @pre @p frame is currently resident and linked.
 * @post @p frame is at the MRU head of the list.
 * @post The list length is unchanged.
 *
 * @note Not thread-safe: mutates the shared list.
 * @since 0.1.0
 */
static void cb_lru_touch(cb_cache_t* c, uint32_t frame)
{
  cb_lru_t* l = (cb_lru_t*)c->policy_data;
  cb_lru_unlink(l, (int32_t)frame);
  cb_lru_to_head(l, (int32_t)frame);
}
/**
 * @brief LRU insert hook: place a freshly-loaded @p frame at the MRU head.
 *
 * @details Links the newly populated @p frame in at the head (it is not yet in
 *          the list), so it becomes the most-recently-used. Bound as the
 *          policy's `on_insert`.
 *
 * @param[in,out] c     Cache holding the LRU list in `policy_data`.
 * @param[in]     frame Frame that was just (re)populated.
 *
 * @pre `c->policy_data` is a valid ::cb_lru_init list.
 * @pre @p frame is detached (its old key was unlinked on eviction).
 * @post @p frame is at the MRU head of the list.
 * @post The list grows by one member.
 *
 * @note Not thread-safe: mutates the shared list.
 * @since 0.1.0
 */
static void cb_lru_insert(cb_cache_t* c, uint32_t frame)
{
  cb_lru_t* l = (cb_lru_t*)c->policy_data;
  cb_lru_to_head(l, (int32_t)frame);
}
/**
 * @brief Choose the LRU victim: the frame at the list tail.
 *
 * @details Returns the least-recently-used frame (the tail) and unlinks it so
 *          the caller can repopulate it. Reports a scan depth of one -- true
 *          LRU finds its victim in O(1).
 *
 * @param[in,out] c       Cache holding the LRU list in `policy_data`.
 * @param[out]    scanned Receives the frames examined (always 1).
 *
 * @return uint32_t The victim frame index (< capacity).
 * @retval <capacity The least-recently-used resident frame.
 *
 * @pre `c->policy_data` is a valid, non-empty ::cb_lru_init list.
 * @pre @p scanned is non-NULL.
 * @post `*scanned == 1` and the victim is unlinked from the list.
 * @post The list shrinks by one member.
 *
 * @note Not thread-safe: mutates the shared list.
 * @since 0.1.0
 */
static uint32_t cb_lru_victim(cb_cache_t* c, uint32_t* scanned)
{
  cb_lru_t*     l = (cb_lru_t*)c->policy_data;
  const int32_t f = l->tail;
  cb_lru_unlink(l, f);
  *scanned = 1U;
  return (uint32_t)f;
}
static const cache_policy_t s_cb_policy_lru = {
  .name        = "LRU",
  .meta_bytes  = 8U,
  .init        = cb_lru_init,
  .deinit      = cb_lru_deinit,
  .on_access   = cb_lru_touch,
  .on_insert   = cb_lru_insert,
  .pick_victim = cb_lru_victim,
};

/* ----------------------------------------------------------------- CLOCK -- */

/**
 * @brief Allocate CLOCK state: one reference bit per frame + a sweep hand.
 *
 * @details Allocates one zeroed `uint32_t` sweep hand in `c->policy_data`; the
 *          reference bit lives in each frame's `meta[0]`, set on access/insert
 *          and cleared as the hand gives a frame its second chance.
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
  cb_clock_init(cb_cache_t* c)
{
  uint32_t* hand = (uint32_t*)calloc(1U, sizeof(uint32_t));
  c->policy_data = hand;
  return (hand == nullptr) ? 1 : 0;
}
/**
 * @brief Release CLOCK state (the sweep hand).
 *
 * @details Frees the `uint32_t` hand ::cb_clock_init allocated; `free(NULL)` is
 *          safe after a failed init.
 *
 * @param[in,out] c Cache whose `policy_data` hand is freed.
 *
 * @pre @p c is non-NULL.
 * @pre `c->policy_data` is a ::cb_clock_init hand or NULL.
 * @post The hand memory is released.
 * @post `c->policy_data` is left dangling; the caller discards the cache.
 *
 * @note Not thread-safe: frees policy state.
 * @since 0.1.0
 */
static void cb_clock_deinit(cb_cache_t* c)
{
  free(c->policy_data);
}
/**
 * @brief CLOCK reference hook: set @p frame's reference bit.
 *
 * @details Writes 1 to `frames[frame].meta[0]`, marking the frame as recently
 *          used so the sweep hand grants it one second chance before eviction.
 *          Bound as both `on_access` and `on_insert`.
 *
 * @param[in,out] c     Cache whose frame reference bit is set.
 * @param[in]     frame Frame just accessed or inserted.
 *
 * @pre @p c is non-NULL and @p frame < capacity.
 * @pre @p frame is currently resident.
 * @post `frames[frame].meta[0] == 1`.
 * @post No other frame or policy state changes.
 *
 * @note Not thread-safe: writes shared frame metadata.
 * @since 0.1.0
 */
static void cb_clock_set(cb_cache_t* c, uint32_t frame)
{
  c->frames[frame].meta[0] = 1U;
}
/**
 * @brief Choose the CLOCK victim by second-chance sweep.
 *
 * @details Advances the hand around the ring: a frame with its reference bit
 *          set is spared once (the bit is cleared) and skipped; the first frame
 *          found with a clear bit is evicted. Reports the number of frames
 *          examined, so a full ring of set bits costs one extra pass at most.
 *
 * @param[in,out] c       Cache holding the sweep hand in `policy_data`.
 * @param[out]    scanned Receives the frames examined this call (>= 1).
 *
 * @return uint32_t The victim frame index (< capacity).
 * @retval <capacity The first frame reached with a clear reference bit.
 *
 * @pre `c->policy_data` is a valid ::cb_clock_init hand and `capacity > 0`.
 * @pre @p scanned is non-NULL.
 * @post `*scanned` equals the frames inspected and the hand advanced past them.
 * @post Every spared frame's reference bit was cleared.
 *
 * @note Not thread-safe: advances the hand and clears reference bits.
 * @since 0.1.0
 */
static uint32_t cb_clock_victim(cb_cache_t* c, uint32_t* scanned)
{
  uint32_t* hand = (uint32_t*)c->policy_data;
  uint32_t  seen = 0U;
  for (;;) {
    const uint32_t f = *hand;
    *hand            = (*hand + 1U) % c->capacity;
    seen++;
    if (c->frames[f].meta[0] != 0U) {
      c->frames[f].meta[0] = 0U; /* second chance */
    } else {
      *scanned = seen;
      return f;
    }
  }
}
static const cache_policy_t s_cb_policy_clock = {
  .name        = "CLOCK",
  .meta_bytes  = 1U,
  .init        = cb_clock_init,
  .deinit      = cb_clock_deinit,
  .on_access   = cb_clock_set,
  .on_insert   = cb_clock_set,
  .pick_victim = cb_clock_victim,
};

/* -------------------------------------------------------------- registry -- */

const cache_policy_t* const g_cb_policies[] = {
  &s_cb_policy_fifo,
  &s_cb_policy_random,
  &s_cb_policy_lru,
  &s_cb_policy_clock,
  &g_cb_policy_slru,
  &g_cb_policy_srrip,
};
const uint32_t g_cb_policy_count = (uint32_t)(sizeof(g_cb_policies) / sizeof(g_cb_policies[0]));
