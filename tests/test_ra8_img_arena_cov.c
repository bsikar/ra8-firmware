/**
 * @file test_ra8_img_arena_cov.c
 * @brief Coverage supplement for libs/ra8_reflow/src/ra8_img_arena.c
 *
 * @details
 * Drives the ten source lines not reached by the existing image-decode
 * integration tests:
 *
 *  - ra8_img_arena_malloc() partial-capacity OOM (source line 65): the
 *    aligned request does not fit the remaining bytes even though the
 *    raw size is within the total capacity.
 *
 *  - ra8_img_arena_realloc_sized() non-nullptr p path (lines 93-102):
 *    three scenarios -- success with data copy, failure when the arena
 *    is exhausted, and zero-copy (oldsz == 0) to hit the false arm of
 *    `if (copy > 0U)`.  A fourth scenario covers the ternary false arm
 *    (oldsz >= newsz) in the shrink case.
 *
 * All guards in this module are single-condition decisions.  DO-178C
 * Level B MC/DC is achieved by driving both outcomes of each guard: the
 * true outcomes appear here; the false outcomes are already covered by
 * test_ra8_reflow_image.c or by earlier tests within this file.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <string.h>

#include "ra8_img_arena.h"
#include "unity_minimal.h"

/** @brief Fill byte marking a block released back to the arena. */
typedef enum : uint8_t {
  k_arena_fill_stale  = 0xABU, /**< Must not survive a reallocation.               */
  k_arena_stale_bytes = 16U,   /**< Bytes of the released block that are poisoned. */
} arena_fill_t;

/**
 * @enum img_arena_cov_fixture_t
 * @brief Buffer capacities, payload sizes and fixture geometry.
 */
typedef enum : uint8_t {
  k_arena_bytes_large =
    128U, /**< Arena big enough that it succeeds, so the failure above is attributable to size. */
  /** Arena small enough that the second allocation must fail. */
  k_arena_bytes_small = 64U,
} img_arena_cov_fixture_t;

/**
 * @test test_malloc_partial_capacity_oom
 * @brief ra8_img_arena_malloc returns nullptr when the aligned request exceeds
 *        the remaining capacity even though n <= cap.
 *
 * @details
 * Binds a 32-byte arena and allocates 16 bytes first, leaving 16 bytes
 * free.  A subsequent 32-byte request passes the `n > a->cap` guard but
 * fails the `aligned > (a->cap - a->offset)` guard at line 64, hitting
 * the `return nullptr` at line 65.
 *
 * @par MC/DC:
 * Decision: `if (aligned > (a->cap - a->offset))` (1 condition,
 * ra8_img_arena.c line 64).
 *  - V-T: 32-byte request after a 16-byte offset -> aligned(32) >
 *    remaining(16) -> nullptr.  Covered here.
 *  - V-F: fresh arena, small request -> aligned fits -> success.
 *    Covered by test_ra8_reflow_image.c.
 *
 * @pre None.
 * @post The arena state is unchanged on a failed malloc.
 * @since 0.1.0
 */
static void test_malloc_partial_capacity_oom(void)
{
  TEST_BEGIN("ra8_img_arena_malloc: partial-capacity OOM (line 65)");
  static uint8_t  s_buf[32U];
  ra8_img_arena_t arena = {.base = s_buf, .cap = sizeof s_buf, .offset = 0U, .live = 0U};
  ra8_img_arena_bind(&arena);

  /* First allocation: 16 bytes, aligned to 16.  Succeeds. */
  void* const first = ra8_img_arena_malloc(16U);
  TEST_ASSERT_NOT_NULL(first);
  TEST_ASSERT_EQ(16, arena.offset);
  TEST_ASSERT_EQ(1, arena.live);

  /* Second attempt: n(32) <= cap(32) passes the cap guard, but
   * aligned(32) > remaining(16) -> line 65 -> nullptr. */
  void* const second = ra8_img_arena_malloc(32U);
  TEST_ASSERT_NULL(second);
  /* Arena must be unchanged on failure. */
  TEST_ASSERT_EQ(16, arena.offset);
  TEST_ASSERT_EQ(1, arena.live);

  ra8_img_arena_free(first);
  TEST_ASSERT_EQ(0, arena.offset);
  TEST_ASSERT_EQ(0, arena.live);

  ra8_img_arena_unbind();
  TEST_END("ra8_img_arena_malloc: partial-capacity OOM (line 65)");
}

/**
 * @test test_realloc_non_null_success_with_copy
 * @brief ra8_img_arena_realloc_sized with non-nullptr p and sufficient arena
 *        space; the first oldsz bytes are copied into the fresh block.
 *
 * @details
 * Allocates 16 bytes, fills them with a sentinel byte (0xAB), then
 * reallocates to 32 bytes.  The function allocates the fresh block, copies
 * 16 bytes (oldsz < newsz so copy = oldsz), frees the old block, and
 * returns the new one.  This exercises the happy path of the non-nullptr
 * branch (lines 93-102).
 *
 * @par MC/DC:
 * Decisions in ra8_img_arena_realloc_sized (ra8_img_arena.c):
 *
 * 1. `if (fresh == nullptr)` (1 condition, line 94):
 *    - V-F: fresh != nullptr (this test).
 *    - V-T: test_realloc_non_null_fail.
 *
 * 2. `if (copy > 0U)` (1 condition, line 98):
 *    - V-T: oldsz(16) < newsz(32) -> copy = oldsz(16) > 0 -> memcpy
 *      executes (this test).
 *    - V-F: test_realloc_zero_oldsz.
 *
 * 3. Ternary `(oldsz < newsz) ? oldsz : newsz` (line 97):
 *    - T arm: oldsz(16) < newsz(32) -> copy = 16 (this test).
 *    - F arm: test_realloc_shrink.
 *
 * @pre None.
 * @post The returned block holds the first oldsz bytes of the original block.
 * @since 0.1.0
 */
static void test_realloc_non_null_success_with_copy(void)
{
  TEST_BEGIN("ra8_img_arena_realloc_sized: non-nullptr p, success + copy > 0");
  static uint8_t  s_buf[k_arena_bytes_large];
  ra8_img_arena_t arena = {.base = s_buf, .cap = sizeof s_buf, .offset = 0U, .live = 0U};
  ra8_img_arena_bind(&arena);

  uint8_t* const old_blk = (uint8_t*)ra8_img_arena_malloc(16U);
  TEST_ASSERT_NOT_NULL(old_blk);
  memset(old_blk, k_arena_fill_stale, (size_t)k_arena_stale_bytes);

  /* Grow to 32 bytes: oldsz(16) < newsz(32) -> copy = 16 -> memcpy runs. */
  uint8_t* const new_blk = (uint8_t*)ra8_img_arena_realloc_sized(old_blk, 16U, 32U);
  TEST_ASSERT_NOT_NULL(new_blk);

  /* First 16 bytes must carry the sentinel value. */
  for (size_t i = 0U; i < 16U; ++i) {
    TEST_ASSERT_EQ(0xAB, new_blk[i]);
  }

  /* Bump allocator places fresh after old; addresses must differ. */
  TEST_ASSERT((uintptr_t)new_blk != (uintptr_t)old_blk);
  /* old_blk was freed inside realloc; only new_blk remains live. */
  TEST_ASSERT_EQ(1, arena.live);

  ra8_img_arena_free(new_blk);
  TEST_ASSERT_EQ(0, arena.offset);
  TEST_ASSERT_EQ(0, arena.live);

  ra8_img_arena_unbind();
  TEST_END("ra8_img_arena_realloc_sized: non-nullptr p, success + copy > 0");
}

/**
 * @test test_realloc_non_null_fail
 * @brief ra8_img_arena_realloc_sized returns nullptr when the fresh allocation
 *        fails; the original block remains live and the arena is unchanged.
 *
 * @details
 * Partially fills the arena so that the remaining capacity is insufficient
 * for the requested new size.  ra8_img_arena_malloc returns nullptr for the
 * fresh block (via line 65), so the function takes the early-return path at
 * line 95 without modifying the arena or freeing the original block.
 *
 * @par MC/DC:
 * Decision: `if (fresh == nullptr)` (1 condition, line 94):
 *  - V-T: fresh == nullptr -> return nullptr (this test).
 *  - V-F: test_realloc_non_null_success_with_copy.
 *
 * @pre None.
 * @post The arena state is unchanged; the original block is still live.
 * @since 0.1.0
 */
static void test_realloc_non_null_fail(void)
{
  TEST_BEGIN("ra8_img_arena_realloc_sized: non-nullptr p, fresh-alloc fails");
  static uint8_t  s_buf[32U];
  ra8_img_arena_t arena = {.base = s_buf, .cap = sizeof s_buf, .offset = 0U, .live = 0U};
  ra8_img_arena_bind(&arena);

  /* Consume 16 bytes, leaving only 16 bytes free. */
  void* const p = ra8_img_arena_malloc(16U);
  TEST_ASSERT_NOT_NULL(p);
  TEST_ASSERT_EQ(16, arena.offset);

  /* newsz(32) aligned to 32 > remaining(16) -> fresh == nullptr -> line 95. */
  void* const result = ra8_img_arena_realloc_sized(p, 16U, 32U);
  TEST_ASSERT_NULL(result);

  /* Arena must be unchanged: p still live, offset unchanged. */
  TEST_ASSERT_EQ(16, arena.offset);
  TEST_ASSERT_EQ(1, arena.live);

  ra8_img_arena_free(p);
  TEST_ASSERT_EQ(0, arena.offset);

  ra8_img_arena_unbind();
  TEST_END("ra8_img_arena_realloc_sized: non-nullptr p, fresh-alloc fails");
}

/**
 * @test test_realloc_zero_oldsz
 * @brief ra8_img_arena_realloc_sized with oldsz == 0 skips the memcpy
 *        (false arm of `if (copy > 0U)` at line 98).
 *
 * @details
 * A zero-byte malloc is valid: it returns a non-null pointer, leaves the
 * offset unchanged (aligned = 0), and increments live.  Reallocating it
 * with oldsz == 0 and newsz == 16 produces copy = min(0, 16) = 0, so the
 * body of `if (copy > 0U)` at line 99 is skipped.
 *
 * @par MC/DC:
 * Decision: `if (copy > 0U)` (1 condition, line 98):
 *  - V-F: oldsz(0), newsz(16) -> copy = 0 -> memcpy skipped (this test).
 *  - V-T: test_realloc_non_null_success_with_copy (copy = 16).
 *
 * @pre None.
 * @post The returned 16-byte block is live; the zero-byte block is freed.
 * @since 0.1.0
 */
static void test_realloc_zero_oldsz(void)
{
  TEST_BEGIN("ra8_img_arena_realloc_sized: oldsz=0 skips memcpy (line 98 false)");
  static uint8_t  s_buf[k_arena_bytes_small];
  ra8_img_arena_t arena = {.base = s_buf, .cap = sizeof s_buf, .offset = 0U, .live = 0U};
  ra8_img_arena_bind(&arena);

  /* A zero-byte allocation: aligned = 0, so offset stays at 0, live = 1. */
  void* const zero_blk = ra8_img_arena_malloc(0U);
  TEST_ASSERT_NOT_NULL(zero_blk);
  TEST_ASSERT_EQ(0, arena.offset);
  TEST_ASSERT_EQ(1, arena.live);

  /* oldsz(0) < newsz(16) -> copy = 0 -> memcpy skipped; returns 16-byte block. */
  void* const fresh = ra8_img_arena_realloc_sized(zero_blk, 0U, 16U);
  TEST_ASSERT_NOT_NULL(fresh);
  /* zero_blk freed inside realloc; only fresh remains live. */
  TEST_ASSERT_EQ(1, arena.live);

  ra8_img_arena_free(fresh);
  TEST_ASSERT_EQ(0, arena.offset);
  TEST_ASSERT_EQ(0, arena.live);

  ra8_img_arena_unbind();
  TEST_END("ra8_img_arena_realloc_sized: oldsz=0 skips memcpy (line 98 false)");
}

/**
 * @test test_realloc_shrink
 * @brief ra8_img_arena_realloc_sized with oldsz > newsz selects copy = newsz
 *        (false arm of the ternary at line 97).
 *
 * @details
 * Allocates 32 bytes filled with incrementing values (0..31), then shrinks
 * to 16 bytes.  The ternary `(oldsz < newsz) ? oldsz : newsz` evaluates
 * false (oldsz(32) >= newsz(16)), so copy = newsz = 16 and only the first
 * 16 bytes of the original block are preserved in the new block.
 *
 * @par MC/DC:
 * Ternary `(oldsz < newsz) ? oldsz : newsz` (line 97):
 *  - F arm (oldsz >= newsz): oldsz(32), newsz(16) -> copy = 16 (this test).
 *  - T arm: test_realloc_non_null_success_with_copy (oldsz(16) < newsz(32)).
 *
 * @pre None.
 * @post The returned block holds the first newsz bytes of the original.
 * @since 0.1.0
 */
static void test_realloc_shrink(void)
{
  TEST_BEGIN("ra8_img_arena_realloc_sized: shrink (oldsz > newsz -> copy = newsz)");
  static uint8_t  s_buf[k_arena_bytes_large];
  ra8_img_arena_t arena = {.base = s_buf, .cap = sizeof s_buf, .offset = 0U, .live = 0U};
  ra8_img_arena_bind(&arena);

  uint8_t* const old_blk = (uint8_t*)ra8_img_arena_malloc(32U);
  TEST_ASSERT_NOT_NULL(old_blk);
  for (size_t i = 0U; i < 32U; ++i) {
    old_blk[i] = (uint8_t)i;
  }

  /* Shrink: oldsz(32) >= newsz(16) -> copy = newsz(16). */
  uint8_t* const new_blk = (uint8_t*)ra8_img_arena_realloc_sized(old_blk, 32U, 16U);
  TEST_ASSERT_NOT_NULL(new_blk);

  /* Only the first 16 bytes should be copied. */
  for (size_t i = 0U; i < 16U; ++i) {
    TEST_ASSERT_EQ(i, new_blk[i]);
  }
  TEST_ASSERT_EQ(1, arena.live);

  ra8_img_arena_free(new_blk);
  TEST_ASSERT_EQ(0, arena.live);

  ra8_img_arena_unbind();
  TEST_END("ra8_img_arena_realloc_sized: shrink (oldsz > newsz -> copy = newsz)");
}

/**
 * @brief Test entry point.
 * @return 0 on success; unity macros call exit(1) on the first failure.
 * @since 0.1.0
 */
int main(void)
{
  test_malloc_partial_capacity_oom();
  test_realloc_non_null_success_with_copy();
  test_realloc_non_null_fail();
  test_realloc_zero_oldsz();
  test_realloc_shrink();
  return 0;
}
