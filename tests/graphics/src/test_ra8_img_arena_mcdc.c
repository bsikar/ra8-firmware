/**
 * @file test_ra8_img_arena_mcdc.c
 * @brief MC/DC vectors for the release guard behind the stb_image arena hooks.
 *
 * @details
 * `ra8_img_arena_free()` no longer holds a decision of its own: since RA8FW-308 it
 * forwards to ra8_imgdec_scratch_free(), passing the file-static bound arena
 * as the context. The compound guard that decides whether a release does
 * anything now lives in libs/ra8_imgdec/src/ra8_imgdec_scratch.c, and this
 * file drives it through the hooks the decoder actually calls, which is where
 * a NULL context (no arena bound) and a release against an already-drained
 * arena really come from.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8_img_arena.h"
#include "unity_minimal.h"

/**
 * @enum img_arena_mcdc_fixture_t
 * @brief Buffer capacities and payload sizes.
 */
typedef enum : uint8_t {
  k_arena_bytes = 64U, /**< Arena capacity for this vector. */
  k_block_bytes = 16U, /**< Payload size each vector allocates. */
} img_arena_mcdc_fixture_t;

/**
 * @test test_arena_free_null_guard_mcdc
 * @brief Independent-influence vectors for the scratch release guard, driven
 *        through the stb_image arena hooks.
 *
 * @par MC/DC:
 * Decision: `if ((scratch == nullptr) || (ptr == nullptr) || (scratch->live ==
 * 0U))` (3 conditions, OR;
 * libs/ra8_imgdec/src/ra8_imgdec_scratch.c@ra8_imgdec_scratch_free). `scratch`
 * is the bound arena, so `scratch == nullptr` is reached by releasing after
 * ra8_img_arena_unbind(), and `live == 0` by releasing twice.
 *
 * Vectors (N+1 = 4 for N=3):
 *  - V1: scratch bound, ptr real, live=1 -> F,F,F -> false (releases; live -> 0).
 *  - V2: scratch bound, ptr NULL,  live=0 -> F,T   -> true  (early return).
 *  - V3: scratch NULL,  ptr real          -> T     -> true  (early return).
 *  - V4: scratch bound, ptr real, live=0  -> F,F,T -> true  (early return).
 * V1 vs V3 flips only the context and flips the outcome; V1 vs V2 flips only
 * the pointer; V1 vs V4 flips only the live count. Each influences alone.
 *
 * @pre None.
 * @post The arena bump state is consistent with each release outcome.
 * @since 0.1.0
 */
static void test_arena_free_null_guard_mcdc(void)
{
  TEST_BEGIN("ra8_imgdec_scratch_free MC/DC via the stb_image arena hooks");
  static uint8_t  s_buf[k_arena_bytes];
  ra8_img_arena_t arena = {.base = s_buf, .cap = sizeof s_buf, .offset = 0U, .live = 0U};

  /* V1: every condition false -- a real block released while an arena is bound. */
  ra8_img_arena_bind(&arena);
  void* const block = ra8_img_arena_malloc(k_block_bytes);
  TEST_ASSERT_NOT_NULL(block);
  TEST_ASSERT_EQ(1, arena.live);
  ra8_img_arena_free(block);
  TEST_ASSERT_EQ(0, arena.live); /* the release took the body -> live decremented */
  TEST_ASSERT_EQ(0, arena.offset);

  /* V2: C2 true -- free(nullptr) with the arena still bound. No state change. */
  ra8_img_arena_free(nullptr);
  TEST_ASSERT_EQ(0, arena.live);

  /* V4: C3 true -- a stale pointer released against an already-drained arena. */
  ra8_img_arena_free(block);
  TEST_ASSERT_EQ(0, arena.live);
  TEST_ASSERT_EQ(0, arena.offset);

  /* V3: C1 true -- non-null pointer released after unbind, so the context is null. */
  void* const live_block = ra8_img_arena_malloc(k_block_bytes);
  TEST_ASSERT_NOT_NULL(live_block);
  ra8_img_arena_unbind();
  ra8_img_arena_free(live_block); /* early return: ptr non-null but no arena bound */
  /* arena.live stays 1: the unbound release could not touch the detached arena. */
  TEST_ASSERT_EQ(1, arena.live);

  TEST_END("ra8_imgdec_scratch_free MC/DC via the stb_image arena hooks");
}

/**
 * @brief Test entry point.
 * @return 0 on success; unity macros call exit(1) on the first failure.
 * @since 0.1.0
 */
int main(void)
{
  test_arena_free_null_guard_mcdc();
  return 0;
}
