/**
 * @file ra_rand_stub.c
 * @brief Deterministic xorshift32 rand()/srand() override
 *
 * @par Tag
 * [Ring 1 / Core] {World: S}
 *
 * @details
 * Newlib's rand()/srand() heap-allocate per-thread state on first call,
 * which trips the project's ra_sbrk_trap (NASA Power of 10 Rule 3).
 * NetX Duo and other vendor stacks call rand() for ISN/cookie/jitter
 * generation, so we need a working rand() but cannot tolerate malloc.
 *
 * This translation unit provides strong overrides for:
 *   - int rand(void)
 *   - void srand(unsigned int seed)
 *
 * The implementation is a single xorshift32 state in BSS. It satisfies
 * RAND_MAX (>= 32767) per ISO C and never returns the same sequence
 * twice in a row from a non-zero seed. It is NOT cryptographically
 * secure -- callers needing CSPRNG must go through ra_rsip TRNG.
 *
 * Linker note: because newlib's rand/srand live in libc.a, the
 * ld-evaluation order matters. As long as ra_core/ is on the link
 * line BEFORE -lc, our strong symbols win and newlib's heap path is
 * never reached.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <stdlib.h>

/** Xorshift32 default seed: arbitrary non-zero constant per Marsaglia 2003. */
typedef enum : uint32_t {
  k_ra_rand_default_seed = 0x9E3779B9UL,
} ra_rand_const_t;

static uint32_t s_state = (uint32_t)k_ra_rand_default_seed;

/* NOLINTBEGIN(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp,readability-identifier-naming) */
void srand(unsigned int seed)
{
  /* Avoid the all-zero state which would lock xorshift. */
  s_state = (seed == 0U) ? (uint32_t)k_ra_rand_default_seed : (uint32_t)seed;
}

int rand(void)
{
  /* Marsaglia xorshift32 with constants 13/17/5. */
  uint32_t       x      = s_state;
  const uint32_t k_x13  = 13U;
  const uint32_t k_x17  = 17U;
  const uint32_t k_x5   = 5U;
  const uint32_t k_mask = (uint32_t)RAND_MAX;
  x ^= x << k_x13;
  x ^= x >> k_x17;
  x ^= x << k_x5;
  s_state = x;
  return (int)(x & k_mask);
}
/* NOLINTEND(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp,readability-identifier-naming) */
