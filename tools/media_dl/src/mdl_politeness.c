/*
 * Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
/**
 * @file mdl_politeness.c
 * @brief Seeded xorshift64 jitter + injectable sleep for v0 politeness.
 */
#include "mdl_politeness.h"

#include <time.h>

#include "ra8_attributes.h"

/**
 * @enum mdl_seed_t
 * @brief Fallback seed so the xorshift64 state is never 0.
 * @details A zero state is the one fixed point of xorshift64 -- it would emit
 *          zeros forever. The constant is the golden-ratio odd multiplier used
 *          by SplitMix64, chosen for good avalanche from a small seed.
 * @since 0.1.0
 */
typedef enum : uint64_t {
  k_seed_fallback = 0x9E3779B97F4A7C15ULL, /**< Substituted when the seed is 0. */
} mdl_seed_t;

/** @brief xorshift64 shift triple (Marsaglia's 13/7/17). */
typedef enum : uint8_t {
  k_xs_shift_a = 13U, /**< First left shift.  */
  k_xs_shift_b = 7U,  /**< Right shift.       */
  k_xs_shift_c = 17U, /**< Second left shift. */
} mdl_xorshift_t;

void mdl_politeness_init(mdl_politeness_t* p, uint64_t seed)
{
  mdl_politeness_init_clock(p, seed, nullptr, nullptr);
}

RA8_DI_SLOT("politeness_sleep")
void mdl_politeness_init_clock(mdl_politeness_t* p,
                               uint64_t          seed,
                               mdl_sleep_fn      sleep_fn,
                               void*             sleep_ctx)
{
  if (p == nullptr) {
    return;
  }
  p->state     = (seed == 0U) ? (uint64_t)k_seed_fallback : seed;
  p->sleep_fn  = sleep_fn;
  p->sleep_ctx = sleep_ctx;
}

/** @brief Advance the xorshift64 state and return the new value. */
RA8_INTERNAL static uint64_t next_rand(mdl_politeness_t* p)
{
  uint64_t x = p->state;
  x ^= x << (uint64_t)k_xs_shift_a;
  x ^= x >> (uint64_t)k_xs_shift_b;
  x ^= x << (uint64_t)k_xs_shift_c;
  p->state = x;
  return x;
}

/** @brief Block for `ms` milliseconds on the host clock. */
RA8_INTERNAL static void host_sleep_ms(uint32_t ms)
{
  /** @brief Unit conversions for splitting a millisecond delay into timespec. */
  enum : uint32_t {
    k_ms_per_s  = 1000U,    /**< Milliseconds per second.     */
    k_ns_per_ms = 1000000U, /**< Nanoseconds per millisecond. */
  };
  struct timespec ts = {.tv_sec  = (time_t)(ms / k_ms_per_s),
                        .tv_nsec = (long)((ms % k_ms_per_s) * k_ns_per_ms)};
  (void)nanosleep(&ts, nullptr);
}

uint32_t mdl_politeness_wait(mdl_politeness_t* p, uint32_t min_ms, uint32_t max_ms)
{
  if (p == nullptr) {
    return 0U;
  }
  if (max_ms < min_ms) {
    max_ms = min_ms;
  }
  const uint32_t span    = (max_ms - min_ms) + 1U;
  const uint32_t delayms = min_ms + (uint32_t)(next_rand(p) % (uint64_t)span);

  if (p->sleep_fn != nullptr) {
    p->sleep_fn(p->sleep_ctx, delayms);
  } else {
    host_sleep_ms(delayms);
  }
  return delayms;
}
