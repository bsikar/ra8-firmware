/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file test_ra8_rand_stub.c
 * @brief Unit tests for libs/ra8_core/src/ra8_rand_stub.c
 *
 * @details
 * The unit under test IS the freestanding rand()/srand() replacement the
 * firmware links instead of libc, so every test here has to call rand() and
 * seed it with a fixed value -- a fixed seed is what makes the sequence
 * reproducible and therefore assertable. The per-call NOLINTs below waive the
 * weak-PRNG and constant-seed checks for that reason; nothing in this file
 * uses rand() for anything security-relevant.
 *
 * @details
 * The unit under test IS the freestanding rand()/srand() replacement the
 * firmware links instead of libc, so every test here has to call rand() and
 * seed it with a fixed value -- a fixed seed is what makes the sequence
 * reproducible and therefore assertable. The per-call NOLINTs below waive the
 * weak-PRNG and constant-seed checks for that reason; nothing in this file
 * uses rand() for anything security-relevant.
 */

#include <stdint.h>
#include <stdlib.h>

#include "unity_minimal.h"

// NOLINTBEGIN(cert-msc30-c,cert-msc50-cpp,cert-msc32-c,cert-msc51-cpp) -- rand()/srand() ARE the deterministic xorshift32 stub under test in this file.

typedef enum : uint8_t {
  k_rand_range_iters  = 16U, /**< Number of iterations for range check.         */
  k_rand_seed_repro   = 42U, /**< Arbitrary non-zero seed for determinism test. */
  k_rand_seed_range   = 7U,  /**< Arbitrary non-zero seed for range test.       */
  k_rand_seed_differ1 = 1U,  /**< First seed for distinct-sequence test.        */
  k_rand_seed_differ2 = 2U,  /**< Second seed for distinct-sequence test.       */
} rand_test_const_t;

/**
 * @brief Verify srand(0) does not lock the xorshift state.
 *
 * @details
 * A zero seed would freeze the xorshift32 generator; the implementation
 * must remap it to the default non-zero seed so rand() advances.
 *
 * @pre None.
 * @post rand() returns a non-zero sequence after srand(0).
 *
 * @note Exercises the ``seed == 0`` branch in srand().
 *
 * @par MC/DC:
 * Decision: ``s_state = (seed == 0U) ? default : seed``
 * - V1: seed=0  -> takes the default-seed branch.
 * - V2: seed=1  -> takes the identity branch.
 * V1 vs V2 independently vary the condition and flip the branch taken.
 *
 * @since 0.1.0
 */
static void test_srand_zero_does_not_lock(void)
{
  TEST_BEGIN("srand(0) remaps to default seed");
  // NOLINTNEXTLINE(bugprone-random-generator-seed,cert-msc32-c,cert-msc51-cpp)
  srand(0U);
  /* If s_state were left zero xorshift returns 0 forever. */
  // NOLINTNEXTLINE(misc-predictable-rand,cert-msc30-c,cert-msc50-cpp,clang-analyzer-security.insecureAPI.rand)
  const int r = rand();
  TEST_ASSERT(r != 0);
  TEST_END("srand(0) remaps to default seed");
}

/**
 * @brief Verify srand(seed) with a non-zero seed seeds correctly.
 *
 * @details
 * After seeding with the same value twice the sequence restarts from
 * the same point, confirming the seed was actually stored.
 *
 * @pre None.
 * @post Two rand() runs from the same seed produce identical values.
 *
 * @note Exercises the non-zero seed branch.
 *
 * @par MC/DC:
 * Decision: ``s_state = (seed == 0U) ? default : seed``
 * - V2: seed=1  -> identity branch (see test_srand_zero_does_not_lock).
 *
 * @since 0.1.0
 */
static void test_srand_nonzero_seeds_deterministically(void)
{
  TEST_BEGIN("srand(non-zero) produces deterministic sequence");
  // NOLINTNEXTLINE(bugprone-random-generator-seed,cert-msc32-c,cert-msc51-cpp)
  srand((unsigned int)k_rand_seed_repro);
  // NOLINTNEXTLINE(misc-predictable-rand,cert-msc30-c,cert-msc50-cpp,clang-analyzer-security.insecureAPI.rand)
  const int r1 = rand();

  // NOLINTNEXTLINE(bugprone-random-generator-seed,cert-msc32-c,cert-msc51-cpp)
  srand((unsigned int)k_rand_seed_repro);
  // NOLINTNEXTLINE(misc-predictable-rand,cert-msc30-c,cert-msc50-cpp,clang-analyzer-security.insecureAPI.rand)
  const int r2 = rand();

  TEST_ASSERT_EQ(r1, r2);
  TEST_END("srand(non-zero) produces deterministic sequence");
}

/**
 * @brief Verify rand() advances the state on each call.
 *
 * @details
 * Consecutive calls from the same seed must produce distinct values
 * (the xorshift state must advance with every call).
 *
 * @pre srand() has been called.
 * @post Successive rand() results are not all equal.
 *
 * @note No compound boolean decision -- linear path.
 *
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path; no ``&&`` or ``||`` in the code under test for this case)
 *
 * @since 0.1.0
 */
static void test_rand_advances_each_call(void)
{
  // NOLINTNEXTLINE(misc-predictable-rand,cert-msc30-c,cert-msc50-cpp,clang-analyzer-security.insecureAPI.rand)
  TEST_BEGIN("rand() advances state each call");
  // NOLINTNEXTLINE(bugprone-random-generator-seed,cert-msc32-c,cert-msc51-cpp)
  srand((unsigned int)k_rand_seed_differ1);
  // NOLINTNEXTLINE(misc-predictable-rand,cert-msc30-c,cert-msc50-cpp,clang-analyzer-security.insecureAPI.rand)
  const int a = rand();
  // NOLINTNEXTLINE(misc-predictable-rand,cert-msc30-c,cert-msc50-cpp,clang-analyzer-security.insecureAPI.rand)
  const int b = rand();
  // NOLINTNEXTLINE(misc-predictable-rand,cert-msc30-c,cert-msc50-cpp,clang-analyzer-security.insecureAPI.rand)
  const int c = rand();
  /* All three values must not be identical -- xorshift cannot cycle
   * in fewer than 2^32-1 steps from a non-zero seed. */
  TEST_ASSERT(a != b || b != c);
  // NOLINTNEXTLINE(misc-predictable-rand,cert-msc30-c,cert-msc50-cpp,clang-analyzer-security.insecureAPI.rand)
  TEST_END("rand() advances state each call");
}

/**
 * @brief Verify rand() returns values within [0, RAND_MAX].
 *
 * @details
 * The implementation masks with RAND_MAX before returning; any value
 * outside the range would indicate a broken mask.
 *
 * @pre srand() has been called.
 * @post Every rand() result satisfies 0 <= result <= RAND_MAX.
 *
 * @par MC/DC:
 * (no compound decisions in this test -- exercises a simple range
 * post-condition; no ``&&`` or ``||`` in the code under test)
 *
 * @since 0.1.0
 */
static void test_rand_within_range(void)
{
  // NOLINTNEXTLINE(misc-predictable-rand,cert-msc30-c,cert-msc50-cpp,clang-analyzer-security.insecureAPI.rand)
  TEST_BEGIN("rand() stays within [0, RAND_MAX]");
  // NOLINTNEXTLINE(bugprone-random-generator-seed,cert-msc32-c,cert-msc51-cpp)
  srand((unsigned int)k_rand_seed_range);
  for (uint8_t i = 0U; i < (uint8_t)k_rand_range_iters; ++i) {
    // NOLINTNEXTLINE(misc-predictable-rand,cert-msc30-c,cert-msc50-cpp,clang-analyzer-security.insecureAPI.rand)
    const int r = rand();
    TEST_ASSERT(r >= 0);
    TEST_ASSERT(r <= RAND_MAX);
  }
  // NOLINTNEXTLINE(misc-predictable-rand,cert-msc30-c,cert-msc50-cpp,clang-analyzer-security.insecureAPI.rand)
  TEST_END("rand() stays within [0, RAND_MAX]");
}

/**
 * @brief Verify srand(1) produces a different sequence from srand(2).
 *
 * @details
 * Distinct non-zero seeds must yield distinct outputs, confirming the
 * seed is stored verbatim (not collapsed to a single canonical value).
 *
 * @pre None.
 * @post rand() output after srand(1) differs from srand(2).
 *
 * @par MC/DC:
 * (no compound decisions in this test -- verifies identity vs
 * remapping; no ``&&`` or ``||`` in the code under test)
 *
 * @since 0.1.0
 */
static void test_srand_different_seeds_differ(void)
{
  TEST_BEGIN("srand with different seeds produces different sequences");
  // NOLINTNEXTLINE(bugprone-random-generator-seed,cert-msc32-c,cert-msc51-cpp)
  srand((unsigned int)k_rand_seed_differ1);
  // NOLINTNEXTLINE(misc-predictable-rand,cert-msc30-c,cert-msc50-cpp,clang-analyzer-security.insecureAPI.rand)
  const int r1 = rand();

  // NOLINTNEXTLINE(bugprone-random-generator-seed,cert-msc32-c,cert-msc51-cpp)
  srand((unsigned int)k_rand_seed_differ2);
  // NOLINTNEXTLINE(misc-predictable-rand,cert-msc30-c,cert-msc50-cpp,clang-analyzer-security.insecureAPI.rand)
  const int r2 = rand();

  TEST_ASSERT(r1 != r2);
  TEST_END("srand with different seeds produces different sequences");
}

int32_t main(void)
{
  test_srand_zero_does_not_lock();
  test_srand_nonzero_seeds_deterministically();
  test_rand_advances_each_call();
  test_rand_within_range();
  test_srand_different_seeds_differ();
  (void)fprintf(stderr, "[OK  ] test_ra8_rand_stub.c\n");
  return 0;
}

// NOLINTEND(cert-msc30-c,cert-msc50-cpp,cert-msc32-c,cert-msc51-cpp)
