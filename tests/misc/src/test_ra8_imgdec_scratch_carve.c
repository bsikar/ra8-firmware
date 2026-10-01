/**
 * @file test_ra8_imgdec_scratch_carve.c
 * @brief Host tests for carving a decode scratch out of an arena (RA8FW-308).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <string.h>

#include "ra8_arena.h"
#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_imgdec_scratch.h"
#include "unity_minimal.h"

/** @brief Fixture sizes (no magic numbers). */
enum : uint32_t {
  k_pool_bytes    = 1024, /**< Arena backing region every case carves from. */
  k_budget_bytes  = 256,  /**< A backend's published scratch budget.        */
  k_small_bytes   = 8,    /**< Request smaller than the block alignment.    */
  k_strong_align  = 32,   /**< Stronger than the contract's block align.    */
  k_broken_align  = 24,   /**< Not a power of two.                          */
  k_fill_byte     = 0x5A  /**< Non-zero pre-fill, so a carve is visible.    */
};

alignas(64) static uint8_t g_pool[k_pool_bytes];

/**
 * @brief Fresh arena over the shared pool, pre-filled non-zero.
 *
 * @param[out] arena Arena to initialise.
 *
 * @return None.
 */
RA8_INTERNAL static void internal_fresh_arena(ra8_arena_t* arena) {
  (void)memset(g_pool, k_fill_byte, sizeof(g_pool));
  *arena = (ra8_arena_t){0};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_arena_init(arena, g_pool, sizeof(g_pool)));
}

RA8_INTERNAL static void internal_test_carve_binds(void) {
  TEST_BEGIN("a carve binds the published budget and nothing more");

  ra8_arena_t arena;
  internal_fresh_arena(&arena);

  ra8_imgdec_scratch_t scratch = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_imgdec_scratch_carve(&scratch, &arena, k_budget_bytes,
                                                    (uint32_t)k_ra8_imgdec_scratch_align));

  TEST_ASSERT_NOT_NULL(scratch.base);
  TEST_ASSERT(scratch.cap == (size_t)k_budget_bytes);
  TEST_ASSERT(scratch.offset == 0U);
  TEST_ASSERT(scratch.live == 0U);
  TEST_ASSERT(ra8_imgdec_scratch_high_water(&scratch) == 0U);

  /* The arena advanced by exactly the budget, not by a rounded-up guess. */
  uint32_t remaining = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_arena_remaining(&arena, &remaining));
  TEST_ASSERT(remaining == (uint32_t)(sizeof(g_pool) - k_budget_bytes));

  TEST_END("a carve binds the published budget and nothing more");
}

RA8_INTERNAL static void internal_test_carved_scratch_allocates(void) {
  TEST_BEGIN("the carved block is the scratch's whole store");

  ra8_arena_t arena;
  internal_fresh_arena(&arena);

  ra8_imgdec_scratch_t scratch = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_imgdec_scratch_carve(&scratch, &arena, k_budget_bytes, 0U));

  void* const whole = ra8_imgdec_scratch_alloc(&scratch, k_budget_bytes);
  TEST_ASSERT_NOT_NULL(whole);
  TEST_ASSERT(whole == (void*)scratch.base);

  /* One byte past the budget is refused: the carve is the ceiling. */
  TEST_ASSERT_NULL(ra8_imgdec_scratch_alloc(&scratch, k_small_bytes));

  /* Draining rewinds inside the carve; the arena is not touched again. */
  ra8_imgdec_scratch_free(&scratch, whole);
  TEST_ASSERT(scratch.offset == 0U);
  TEST_ASSERT_NOT_NULL(ra8_imgdec_scratch_alloc(&scratch, k_budget_bytes));

  uint32_t remaining = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_arena_remaining(&arena, &remaining));
  TEST_ASSERT(remaining == (uint32_t)(sizeof(g_pool) - k_budget_bytes));

  TEST_END("the carved block is the scratch's whole store");
}

RA8_INTERNAL static void internal_test_default_align(void) {
  TEST_BEGIN("a zero alignment means the contract's block alignment");

  ra8_arena_t arena;
  internal_fresh_arena(&arena);

  /* Push the cursor off alignment first, so the carve has to correct it. */
  void* const crumb = nullptr;
  void*       taken = crumb;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_arena_carve(&arena, 1U, 1U, &taken));

  ra8_imgdec_scratch_t scratch = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_imgdec_scratch_carve(&scratch, &arena, k_budget_bytes, 0U));

  const uintptr_t addr = (uintptr_t)scratch.base;
  TEST_ASSERT((addr % (uintptr_t)k_ra8_imgdec_scratch_align) == 0U);

  TEST_END("a zero alignment means the contract's block alignment");
}

RA8_INTERNAL static void internal_test_guards(void) {
  TEST_BEGIN("a missing record, arena or budget is refused");

  ra8_arena_t arena;
  internal_fresh_arena(&arena);

  ra8_imgdec_scratch_t scratch = {};

  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_imgdec_scratch_carve(nullptr, &arena, k_budget_bytes, 0U));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_imgdec_scratch_carve(&scratch, nullptr, k_budget_bytes, 0U));
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, ra8_imgdec_scratch_carve(&scratch, &arena, 0U, 0U));

  /* Nothing was carved on any refusal. */
  uint32_t remaining = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_arena_remaining(&arena, &remaining));
  TEST_ASSERT(remaining == (uint32_t)sizeof(g_pool));

  TEST_END("a missing record, arena or budget is refused");
}

RA8_INTERNAL static void internal_test_align_beyond_contract(void) {
  TEST_BEGIN("an alignment stronger than the blocks is refused, not faked");

  ra8_arena_t arena;
  internal_fresh_arena(&arena);

  ra8_imgdec_scratch_t scratch = {};

  TEST_ASSERT_EQ(k_ra8_err_not_supported,
                 ra8_imgdec_scratch_carve(&scratch, &arena, k_budget_bytes, k_strong_align));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_imgdec_scratch_carve(&scratch, &arena, k_budget_bytes, k_broken_align));

  uint32_t remaining = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_arena_remaining(&arena, &remaining));
  TEST_ASSERT(remaining == (uint32_t)sizeof(g_pool));

  TEST_END("an alignment stronger than the blocks is refused, not faked");
}

RA8_INTERNAL static void internal_test_arena_too_small(void) {
  TEST_BEGIN("a budget the arena cannot hold is reported as no memory");

  ra8_arena_t arena;
  internal_fresh_arena(&arena);

  ra8_imgdec_scratch_t scratch = {};
  TEST_ASSERT_EQ(k_ra8_err_no_mem,
                 ra8_imgdec_scratch_carve(&scratch, &arena, (uint32_t)sizeof(g_pool) + 1U, 0U));

  uint32_t remaining = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_arena_remaining(&arena, &remaining));
  TEST_ASSERT(remaining == (uint32_t)sizeof(g_pool));

  TEST_END("a budget the arena cannot hold is reported as no memory");
}

RA8_INTERNAL static void internal_test_failure_empties_the_record(void) {
  TEST_BEGIN("a refused carve leaves no usable scratch behind");

  ra8_arena_t arena;
  internal_fresh_arena(&arena);

  ra8_imgdec_scratch_t scratch = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_imgdec_scratch_carve(&scratch, &arena, k_budget_bytes, 0U));
  TEST_ASSERT_NOT_NULL(ra8_imgdec_scratch_alloc(&scratch, k_small_bytes));

  /* Re-carving with a budget that cannot fit must not leave the old store
     bound: a backend that ignored the return would otherwise keep drawing
     from the previous decode's block. */
  TEST_ASSERT_EQ(k_ra8_err_no_mem,
                 ra8_imgdec_scratch_carve(&scratch, &arena, (uint32_t)sizeof(g_pool), 0U));

  TEST_ASSERT_NULL(scratch.base);
  TEST_ASSERT(scratch.cap == 0U);
  TEST_ASSERT(scratch.live == 0U);
  TEST_ASSERT_NULL(ra8_imgdec_scratch_alloc(&scratch, k_small_bytes));

  TEST_END("a refused carve leaves no usable scratch behind");
}

RA8_INTERNAL static void internal_test_two_backends_share_one_arena(void) {
  TEST_BEGIN("two backends carve side by side without overlapping");

  ra8_arena_t arena;
  internal_fresh_arena(&arena);

  ra8_imgdec_scratch_t first  = {};
  ra8_imgdec_scratch_t second = {};

  TEST_ASSERT_EQ(k_ra8_ok, ra8_imgdec_scratch_carve(&first, &arena, k_budget_bytes, 0U));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_imgdec_scratch_carve(&second, &arena, k_budget_bytes, 0U));

  TEST_ASSERT(second.base >= (first.base + first.cap));

  uint8_t* const a = (uint8_t*)ra8_imgdec_scratch_alloc(&first, k_budget_bytes);
  uint8_t* const b = (uint8_t*)ra8_imgdec_scratch_alloc(&second, k_budget_bytes);
  TEST_ASSERT_NOT_NULL(a);
  TEST_ASSERT_NOT_NULL(b);
  TEST_ASSERT((a + k_budget_bytes) <= b);

  uint32_t remaining = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_arena_remaining(&arena, &remaining));
  TEST_ASSERT(remaining == (uint32_t)(sizeof(g_pool) - (2U * k_budget_bytes)));

  TEST_END("two backends carve side by side without overlapping");
}

RA8_INTERNAL static void internal_test_high_water_is_the_carve_not_the_pool(void) {
  TEST_BEGIN("the scratch peak measures the carve, the arena measures the carve out");

  ra8_arena_t arena;
  internal_fresh_arena(&arena);

  ra8_imgdec_scratch_t scratch = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_imgdec_scratch_carve(&scratch, &arena, k_budget_bytes, 0U));

  TEST_ASSERT_NOT_NULL(ra8_imgdec_scratch_alloc(&scratch, k_small_bytes));
  TEST_ASSERT(ra8_imgdec_scratch_high_water(&scratch) ==
              (size_t)k_ra8_imgdec_scratch_align);

  uint32_t peak = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_arena_high_water(&arena, &peak));
  TEST_ASSERT(peak == (uint32_t)k_budget_bytes);

  TEST_END("the scratch peak measures the carve, the arena measures the carve out");
}

int main(void) {
  internal_test_carve_binds();
  internal_test_carved_scratch_allocates();
  internal_test_default_align();
  internal_test_guards();
  internal_test_align_beyond_contract();
  internal_test_arena_too_small();
  internal_test_failure_empties_the_record();
  internal_test_two_backends_share_one_arena();
  internal_test_high_water_is_the_carve_not_the_pool();
  return 0;
}
