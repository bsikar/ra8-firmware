/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_rand_stub.c
 * @brief Deterministic xorshift32 rand()/srand() override
 *
 * @par Tag
 * [Ring 1 / Core] {World: S}
 *
 * @details
 * Newlib's rand()/srand() heap-allocate per-thread state on first call,
 * which trips the project's ra8_sbrk_trap (NASA Power of 10 Rule 3).
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
 * secure -- callers needing CSPRNG must go through ra8_rsip TRNG.
 *
 * Linker note: because newlib's rand/srand live in libc.a, the
 * ld-evaluation order matters. As long as ra8_core/ is on the link
 * line BEFORE -lc, our strong symbols win and newlib's heap path is
 * never reached.
 */

#include <stdint.h>
#include <stdlib.h>

/** @brief Marsaglia xorshift32 shift constants. */
typedef enum : uint32_t {
  k_xorshift_a = 13U, /**< x ^= x << 13. */
  k_xorshift_b = 17U, /**< x ^= x >> 17. */
  k_xorshift_c = 5U,  /**< x ^= x << 5.  */
} xorshift32_param_t;

/** @brief Xorshift32 default seed: arbitrary non-zero constant per Marsaglia 2003. */
typedef enum : uint32_t {
  k_ra8_rand_default_seed = 0x9E3779B9UL, /**< RA8 rand default seed. */
} ra8_rand_const_t;

static uint32_t s_state = (uint32_t)k_ra8_rand_default_seed;

/* NOLINTBEGIN(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp,readability-identifier-naming) -- newlib rand()/srand() override names are fixed by libc. */
/**
 * @brief Strong override for newlib `srand()` -- xorshift32 seed.
 *
 * @details Replaces newlib's heap-backed `srand()` with a direct write
 *          to the xorshift32 state word. A zero seed is silently mapped
 *          to `k_ra8_rand_default_seed` to avoid xorshift's all-zero
 *          lockup state.
 *
 * @param[in] seed Initial state. Zero rewritten to default seed.
 *
 * @pre None.
 * @pre Linker pulls this strong symbol in before `-lc`.
 * @post `s_state` is non-zero.
 * @post Subsequent `rand()` results follow the new seed.
 *
 * @note Not thread-safe -- single shared state word.
 *
 * @since 0.1.0
 */
void srand(unsigned int seed)
{
  /* Avoid the all-zero state which would lock xorshift. */
  s_state = (seed == 0U) ? (uint32_t)k_ra8_rand_default_seed : (uint32_t)seed;
}

/**
 * @brief Strong override for newlib `rand()` -- xorshift32 step.
 *
 * @details Replaces newlib's heap-backed `rand()` with one xorshift32
 *          advance. NOT cryptographically secure -- callers needing a
 *          CSPRNG must go through `ra8_rsip` TRNG.
 *
 * @return Pseudo-random value in `[0, RAND_MAX]`.
 * @retval 0..RAND_MAX  Next xorshift32 output.
 *
 * @pre `srand()` has been called at least once OR the default seed is
 *      acceptable to the caller.
 * @pre Linker pulls this strong symbol in before `-lc`.
 * @post `s_state` advances by one xorshift32 step.
 * @post Return value lives in `[0, RAND_MAX]`.
 *
 * @note Not thread-safe -- single shared state word.
 *
 * @since 0.1.0
 */
int rand(void)
{
  /* Marsaglia xorshift32 with constants 13/17/5. */
  uint32_t       x      = s_state;
  const uint32_t k_mask = (uint32_t)RAND_MAX;
  x ^= x << k_xorshift_a;
  x ^= x >> k_xorshift_b;
  x ^= x << k_xorshift_c;
  s_state = x;
  return (int)(x & k_mask);
}
/* NOLINTEND(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp,readability-identifier-naming) */
