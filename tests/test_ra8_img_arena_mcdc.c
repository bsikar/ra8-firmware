/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file test_ra8_img_arena_mcdc.c
 * @brief MC/DC vectors for the compound guard in libs/ra8_reflow/src/ra8_img_arena.c.
 *
 * @details
 * The image bump-arena free path guards on `(p == nullptr) || (a == nullptr)`
 * where `a` is the currently-bound arena (`s_arena`). The existing
 * test_ra8_img_arena_cov.c only ever frees a real block while an arena is
 * bound, so the decision is exercised with both conditions false but neither
 * condition is ever the sole cause of the early return. This file supplies the
 * two independence vectors: a NULL pointer with an arena bound, and a non-NULL
 * pointer with no arena bound (post-unbind).
 */

#include <stdint.h>
#include <stdio.h>

#include "ra8_img_arena.h"
#include "unity_minimal.h"

/**
 * @enum img_arena_mcdc_fixture_t
 * @brief Buffer capacities and payload sizes.
 */
typedef enum : uint8_t {
  k_arena_bytes = 64U, /**< Arena capacity for this vector. */
} img_arena_mcdc_fixture_t;

/**
 * @test test_arena_free_null_guard_mcdc
 * @brief Independent-influence vectors for the ra8_img_arena_free early-return
 *        guard.
 *
 * @par MC/DC:
 * Decision: `if ((p == nullptr) || (a == nullptr))` (2 conditions, OR;
 * libs/ra8_reflow/src/ra8_img_arena.c@ra8_img_arena_free). `a` is the bound arena
 * `s_arena`, so `a == nullptr` is reached by freeing after ra8_img_arena_unbind().
 *
 * Vectors (N+1 = 3 for N=2):
 *  - V1: p=real block, a=bound   -> C1 F, C2 F -> false (frees; live -> 0).
 *  - V2: p=nullptr,    a=bound   -> C1 T        -> true  (early return).
 *  - V3: p=real block, a=nullptr -> C1 F, C2 T  -> true  (early return).
 * V1 vs V2 flips only C1 (p) and flips the outcome: p influences independently.
 * V1 vs V3 flips only C2 (a) and flips the outcome: a influences independently.
 *
 * @pre None.
 * @post The arena bump state is consistent with each free outcome.
 * @since 0.1.0
 */
static void test_arena_free_null_guard_mcdc(void)
{
  TEST_BEGIN("ra8_img_arena_free MC/DC: (p==null) || (arena==null)");
  static uint8_t  s_buf[k_arena_bytes];
  ra8_img_arena_t arena = {.base = s_buf, .cap = sizeof s_buf, .offset = 0U, .live = 0U};

  /* V1: both conditions false -- a real block freed while the arena is bound. */
  ra8_img_arena_bind(&arena);
  void* const block = ra8_img_arena_malloc(16U);
  TEST_ASSERT_NOT_NULL(block);
  TEST_ASSERT_EQ(1, arena.live);
  ra8_img_arena_free(block);
  TEST_ASSERT_EQ(0, arena.live); /* free took the body -> live decremented */

  /* V2: C1 true -- free(nullptr) with the arena still bound. No state change. */
  ra8_img_arena_free(nullptr);
  TEST_ASSERT_EQ(0, arena.live);

  /* V3: C2 true -- non-null pointer freed after unbind, so s_arena == nullptr. */
  void* const live_block = ra8_img_arena_malloc(16U);
  TEST_ASSERT_NOT_NULL(live_block);
  ra8_img_arena_unbind();
  ra8_img_arena_free(live_block); /* early return: p non-null but arena is null */
  /* arena.live stays 1: the unbound free could not touch the detached arena. */
  TEST_ASSERT_EQ(1, arena.live);

  TEST_END("ra8_img_arena_free MC/DC: (p==null) || (arena==null)");
}

/**
 * @brief Test entry point.
 * @return 0 on success; unity macros call exit(1) on the first failure.
 * @since 0.1.0
 */
int32_t main(void)
{
  test_arena_free_null_guard_mcdc();
  (void)fprintf(stderr, "[OK ] test_ra8_img_arena_mcdc.c\n");
  return 0;
}
