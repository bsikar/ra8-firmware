/*
 * Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
/**
 * @file mdl_politeness.c
 * @brief Seeded xorshift64 jitter + host sleep for v0 politeness.
 */
#include "mdl_politeness.h"

#include <time.h>

/** @brief Fallback seed so the PRNG state is never 0. */
typedef enum : uint64_t {
  k_seed_fallback = 0x9E3779B97F4A7C15ULL,
} mdl_seed_t;

/** @brief xorshift64 shift triple (Marsaglia's 13/7/17). */
typedef enum : uint8_t {
  k_xs_shift_a = 13U, /**< First left shift. */
  k_xs_shift_b = 7U,  /**< Right shift. */
  k_xs_shift_c = 17U, /**< Second left shift. */
} mdl_xorshift_t;

void mdl_politeness_init(mdl_politeness_t* p, uint64_t seed)
{
  if (p == nullptr) {
    return;
  }
  p->state = (seed == 0U) ? (uint64_t)k_seed_fallback : seed;
}

/** @brief Advance the xorshift64 state and return the new value. */
static uint64_t next_rand(mdl_politeness_t* p)
{
  uint64_t x = p->state;
  x ^= x << (uint64_t)k_xs_shift_a;
  x ^= x >> (uint64_t)k_xs_shift_b;
  x ^= x << (uint64_t)k_xs_shift_c;
  p->state = x;
  return x;
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

  enum : uint32_t {
    k_ms_per_s  = 1000U,
    k_ns_per_ms = 1000000U,
  };
  struct timespec ts = {.tv_sec  = (time_t)(delayms / k_ms_per_s),
                        .tv_nsec = (long)((delayms % k_ms_per_s) * k_ns_per_ms)};
  (void)nanosleep(&ts, nullptr);
  return delayms;
}
