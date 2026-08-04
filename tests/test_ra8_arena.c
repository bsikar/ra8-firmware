/**
 * @file test_ra8_arena.c
 * @brief Unit tests for the ra8_mem init-time bump arena (Layer 0, #147).
 *
 * @details
 * Exercises aligned carving (alignment honoured, blocks non-overlapping,
 * remaining shrinks), the over-budget no_mem path, and every validation guard
 * (NULL args, zero bytes, zero / non-power-of-two alignment).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8_arena.h"
#include "ra8_err.h"
#include "unity_minimal.h"

/**
 * @enum t_arena_const_t
 * @brief Fixture sizes.
 */
typedef enum : uint32_t {
  k_t_region_bytes = 4096U, /**< Arena region size. */
} t_arena_const_t;

[[gnu::aligned(16)]] static uint8_t s_region[(size_t)k_t_region_bytes];

/**
 * @par MC/DC:
 * (no compound decisions under test -- carves honour alignment, do not overlap,
 * and shrink the remaining count; an oversized carve returns no_mem)
 */
static void test_carve_align(void)
{
  TEST_BEGIN("arena carve / align / remaining");
  ra8_arena_t a = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_arena_init(&a, s_region, k_t_region_bytes));
  uint32_t rem = 0;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_arena_remaining(&a, &rem));
  TEST_ASSERT_EQ(k_t_region_bytes, rem);

  void* p1 = nullptr;
  void* p2 = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_arena_carve(&a, 100U, 8U, &p1));
  TEST_ASSERT_EQ(0U, ((uintptr_t)p1 % 8U)); /* 8-aligned */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_arena_carve(&a, 64U, 64U, &p2));
  TEST_ASSERT_EQ(0U, ((uintptr_t)p2 % 64U));            /* 64-aligned */
  TEST_ASSERT((uintptr_t)p2 >= ((uintptr_t)p1 + 100U)); /* no overlap */

  TEST_ASSERT_EQ(k_ra8_ok, ra8_arena_remaining(&a, &rem));
  TEST_ASSERT(rem < k_t_region_bytes); /* shrank */

  /* a carve larger than the whole region fails cleanly, arena unchanged */
  uint32_t rem_before = 0;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_arena_remaining(&a, &rem_before));
  void* big = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_no_mem, ra8_arena_carve(&a, k_t_region_bytes, 4U, &big));
  uint32_t rem_after = 0;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_arena_remaining(&a, &rem_after));
  TEST_ASSERT_EQ(rem_before, rem_after);
  TEST_END("arena carve / align / remaining");
}

/**
 * @par MC/DC:
 * (no compound decisions under test -- each guard is an independent
 * single-condition check)
 */
static void test_validation(void)
{
  TEST_BEGIN("arena validation");
  ra8_arena_t a   = {};
  void*       ptr = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_arena_init(nullptr, s_region, k_t_region_bytes));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_arena_init(&a, nullptr, k_t_region_bytes));
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, ra8_arena_init(&a, s_region, 0U));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_arena_init(&a, s_region, k_t_region_bytes));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_arena_carve(nullptr, 8U, 8U, &ptr));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_arena_carve(&a, 8U, 8U, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, ra8_arena_carve(&a, 0U, 8U, &ptr));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_arena_carve(&a, 8U, 0U, &ptr)); /* zero align     */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_arena_carve(&a, 8U, 6U, &ptr)); /* non-pow2 align */
  uint32_t rem = 0;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_arena_remaining(nullptr, &rem));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_arena_remaining(&a, nullptr));
  TEST_END("arena validation");
}

int32_t main(void)
{
  test_carve_align();
  test_validation();
  (void)fprintf(stderr, "[OK  ] test_ra8_arena.c\n");
  return 0;
}
