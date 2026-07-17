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
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * [Ring 7 / Tooling] {World: NS}
 *
 * @since 0.1.0
 */
#include <stdlib.h>

#include "cache_bench.h"

/* ------------------------------------------------------------------ FIFO -- */

/** @brief FIFO state: a single round-robin hand over the frame ring. */
static int cb_fifo_init(cb_cache_t* c)
{
  uint32_t* hand = (uint32_t*)calloc(1U, sizeof(uint32_t));
  c->policy_data = hand;
  return (hand == nullptr) ? 1 : 0;
}
static void cb_fifo_deinit(cb_cache_t* c)
{
  free(c->policy_data);
}
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

/** @brief Random state: a deterministic xorshift seed. */
static int cb_rand_init(cb_cache_t* c)
{
  uint64_t* s = (uint64_t*)malloc(sizeof(uint64_t));
  if (s != nullptr) {
    *s = (uint64_t)k_rand_seed;
  }
  c->policy_data = s;
  return (s == nullptr) ? 1 : 0;
}
static void cb_rand_deinit(cb_cache_t* c)
{
  free(c->policy_data);
}
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

static int cb_lru_init(cb_cache_t* c)
{
  cb_lru_t* l = (cb_lru_t*)calloc(1U, sizeof(cb_lru_t));
  /* cppcheck-suppress memleak ; false positive: cppcheck 2.13 does not model
   * the C23 nullptr keyword, so it cannot see l is NULL on this path. */
  if (l == nullptr) {
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
static void cb_lru_deinit(cb_cache_t* c)
{
  cb_lru_t* l = (cb_lru_t*)c->policy_data;
  if (l != nullptr) {
    free(l->prev);
    free(l->next);
    free(l);
  }
}
/** @brief Unlink frame @p f from the recency list. */
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
/** @brief Splice frame @p f to the MRU head. */
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
static void cb_lru_touch(cb_cache_t* c, uint32_t frame)
{
  cb_lru_t* l = (cb_lru_t*)c->policy_data;
  cb_lru_unlink(l, (int32_t)frame);
  cb_lru_to_head(l, (int32_t)frame);
}
static void cb_lru_insert(cb_cache_t* c, uint32_t frame)
{
  cb_lru_t* l = (cb_lru_t*)c->policy_data;
  cb_lru_to_head(l, (int32_t)frame);
}
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

/** @brief CLOCK: one reference bit (frame meta[0]) + a sweep hand. */
static int cb_clock_init(cb_cache_t* c)
{
  uint32_t* hand = (uint32_t*)calloc(1U, sizeof(uint32_t));
  c->policy_data = hand;
  return (hand == nullptr) ? 1 : 0;
}
static void cb_clock_deinit(cb_cache_t* c)
{
  free(c->policy_data);
}
static void cb_clock_set(cb_cache_t* c, uint32_t frame)
{
  c->frames[frame].meta[0] = 1U;
}
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
