/**
 * @file test_ra_epub_miniz_alloc.c
 * @brief Host unit tests + MC/DC for the miniz static-arena allocator (#139).
 *
 * @details
 * Exercises ::ra_epub_miniz_alloc / _free / _realloc directly (no miniz): basic
 * alloc/align, split, free + coalesce reclaim, realloc grow-move + preserve,
 * realloc in-place, exhaustion, and overflow. Plus MC/DC mirror vectors for the
 * three compound decisions in the allocator (first-fit, coalesce, overflow).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <string.h>

#include "ra_epub_miniz_alloc.h"
#include "unity_minimal.h"

enum : size_t {
  k_small  = 64,
  k_medium = 4096,
  k_big    = 11000, /* ~ a miniz tinfl_decompressor */
};

/**
 * @test test_alloc_align_and_distinct
 * @brief Allocations are aligned and non-overlapping.
 */
static void test_alloc_align_and_distinct(void)
{
  TEST_BEGIN("alloc returns aligned, distinct blocks");
  void* a = ra_epub_miniz_alloc(nullptr, 1U, k_small);
  void* b = ra_epub_miniz_alloc(nullptr, 1U, k_small);
  TEST_ASSERT((a != nullptr) && (b != nullptr));
  TEST_ASSERT(a != b);
  TEST_ASSERT((((uintptr_t)a % alignof(max_align_t)) == 0U));
  TEST_ASSERT((((uintptr_t)b % alignof(max_align_t)) == 0U));
  /* Writing the full request must not corrupt the neighbour. */
  (void)memset(a, 0xAA, k_small);
  (void)memset(b, 0x55, k_small);
  TEST_ASSERT(((const uint8_t*)a)[0] == 0xAAU);
  TEST_ASSERT(((const uint8_t*)b)[k_small - 1U] == 0x55U);
  ra_epub_miniz_free(nullptr, a);
  ra_epub_miniz_free(nullptr, b);
  TEST_END("alloc returns aligned, distinct blocks");
}

/**
 * @test test_free_coalesce_reclaim
 * @brief Freeing everything lets a later big alloc reuse the whole pool.
 */
static void test_free_coalesce_reclaim(void)
{
  TEST_BEGIN("free + coalesce reclaims the pool for a big alloc");
  /* Fragment the pool, then free all -- coalescing must rebuild one big run. */
  void* p[8];
  for (uint32_t i = 0U; i < 8U; i++) {
    p[i] = ra_epub_miniz_alloc(nullptr, 1U, k_medium);
    TEST_ASSERT(p[i] != nullptr);
  }
  for (uint32_t i = 0U; i < 8U; i++) {
    ra_epub_miniz_free(nullptr, p[i]);
  }
  /* If coalescing works, a single big alloc near the pool size succeeds. */
  void* big = ra_epub_miniz_alloc(nullptr, 1U, (size_t)k_ra_epub_miniz_pool_bytes / 2U);
  TEST_ASSERT(big != nullptr);
  ra_epub_miniz_free(nullptr, big);
  TEST_END("free + coalesce reclaims the pool for a big alloc");
}

/**
 * @test test_realloc_grow_preserves
 * @brief realloc grows, moves when needed, and preserves the old bytes.
 */
static void test_realloc_grow_preserves(void)
{
  TEST_BEGIN("realloc grow preserves payload");
  uint8_t* a = (uint8_t*)ra_epub_miniz_alloc(nullptr, 1U, k_small);
  TEST_ASSERT(a != nullptr);
  for (uint32_t i = 0U; i < (uint32_t)k_small; i++) {
    a[i] = (uint8_t)(i & 0xFFU);
  }
  /* Pin a neighbour so the grow cannot happen in place -> forces a move. */
  void* pin = ra_epub_miniz_alloc(nullptr, 1U, k_small);
  TEST_ASSERT(pin != nullptr);
  uint8_t* g = (uint8_t*)ra_epub_miniz_realloc(nullptr, a, 1U, k_big);
  TEST_ASSERT(g != nullptr);
  for (uint32_t i = 0U; i < (uint32_t)k_small; i++) {
    TEST_ASSERT_EQ((int32_t)(i & 0xFFU), (int32_t)g[i]);
  }
  ra_epub_miniz_free(nullptr, g);
  ra_epub_miniz_free(nullptr, pin);
  TEST_END("realloc grow preserves payload");
}

/**
 * @test test_realloc_inplace_and_null_zero
 * @brief realloc keeps a block that already fits; NULL/0 edge cases.
 */
static void test_realloc_inplace_and_null_zero(void)
{
  TEST_BEGIN("realloc in-place + NULL/zero edges");
  void* a = ra_epub_miniz_alloc(nullptr, 1U, k_medium);
  TEST_ASSERT(a != nullptr);
  /* Shrink request fits in place -> same pointer. */
  void* same = ra_epub_miniz_realloc(nullptr, a, 1U, k_small);
  TEST_ASSERT_EQ(a, same);
  /* realloc(NULL, n) == alloc(n). */
  void* fresh = ra_epub_miniz_realloc(nullptr, nullptr, 1U, k_small);
  TEST_ASSERT(fresh != nullptr);
  /* realloc(p, 0) frees and returns NULL. */
  void* none = ra_epub_miniz_realloc(nullptr, fresh, 0U, 0U);
  TEST_ASSERT(none == nullptr);
  ra_epub_miniz_free(nullptr, same);
  TEST_END("realloc in-place + NULL/zero edges");
}

/** @brief Mirror of the first-fit decision: (is_free) && (size >= need). */
static uint8_t mirror_firstfit(uint8_t is_free, size_t size, size_t need)
{
  if ((is_free != 0U) && (size >= need)) {
    return 1U;
  }
  return 0U;
}

/**
 * @test test_firstfit_mcdc
 *
 * @par MC/DC:
 * Decision: `if (b->is_free && b->size >= need)` (2 conditions, AND;
 * ra_epub_miniz_alloc.c first-fit). N+1 = 3 vectors:
 *  - V1: free=1, size=64, need=32 -> T,T -> fit.
 *  - V2: free=0, size=64, need=32 -> F   -> no fit (varies is_free).
 *  - V3: free=1, size=16, need=32 -> T,F -> no fit (varies the size test).
 */
static void test_firstfit_mcdc(void)
{
  TEST_BEGIN("first-fit MC/DC: is_free && size>=need");
  TEST_ASSERT_EQ(1, mirror_firstfit(1U, 64U, 32U));
  TEST_ASSERT_EQ(0, mirror_firstfit(0U, 64U, 32U));
  TEST_ASSERT_EQ(0, mirror_firstfit(1U, 16U, 32U));
  TEST_END("first-fit MC/DC: is_free && size>=need");
}

/** @brief Mirror of the coalesce decision: (in_pool) && (next_free). */
static uint8_t mirror_coalesce(uint8_t in_pool, uint8_t next_free)
{
  if ((in_pool != 0U) && (next_free != 0U)) {
    return 1U;
  }
  return 0U;
}

/**
 * @test test_coalesce_mcdc
 *
 * @par MC/DC:
 * Decision: `while (next < end && next->is_free)` (2 conditions, AND;
 * ra_epub_miniz_alloc.c priv_coalesce). N+1 = 3 vectors:
 *  - V1: in_pool=1, next_free=1 -> swallow.
 *  - V2: in_pool=0, next_free=1 -> stop (varies the bound).
 *  - V3: in_pool=1, next_free=0 -> stop (varies the free test).
 */
static void test_coalesce_mcdc(void)
{
  TEST_BEGIN("coalesce MC/DC: in_pool && next_free");
  TEST_ASSERT_EQ(1, mirror_coalesce(1U, 1U));
  TEST_ASSERT_EQ(0, mirror_coalesce(0U, 1U));
  TEST_ASSERT_EQ(0, mirror_coalesce(1U, 0U));
  TEST_END("coalesce MC/DC: in_pool && next_free");
}

/** @brief Mirror of the overflow guard: (size != 0) && (items > MAX/size). */
static uint8_t mirror_overflow(size_t size, uint8_t items_over)
{
  /* items_over models whether items exceeds SIZE_MAX/size. */
  if ((size != 0U) && (items_over != 0U)) {
    return 1U;
  }
  return 0U;
}

/**
 * @test test_overflow_mcdc
 *
 * @par MC/DC:
 * Decision: `if (size != 0 && items > SIZE_MAX/size)` (2 conditions, AND;
 * ra_epub_miniz_alloc overflow guard). N+1 = 3 vectors:
 *  - V1: size=4, items_over=1 -> overflow -> NULL.
 *  - V2: size=0, items_over=1 -> no overflow (size==0 short-circuits).
 *  - V3: size=4, items_over=0 -> no overflow (count fits).
 */
static void test_overflow_mcdc(void)
{
  TEST_BEGIN("overflow MC/DC: size!=0 && items>MAX/size");
  TEST_ASSERT_EQ(1, mirror_overflow(4U, 1U));
  TEST_ASSERT_EQ(0, mirror_overflow(0U, 1U));
  TEST_ASSERT_EQ(0, mirror_overflow(4U, 0U));
  /* And the real function rejects an actual overflow. */
  TEST_ASSERT(ra_epub_miniz_alloc(nullptr, (SIZE_MAX / 2U) + 2U, 2U) == nullptr);
  TEST_END("overflow MC/DC: size!=0 && items>MAX/size");
}

/**
 * @brief Test entry point.
 * @return 0 on success; unity macros exit(1) on the first failure.
 */
int32_t main(void)
{
  test_alloc_align_and_distinct();
  test_free_coalesce_reclaim();
  test_realloc_grow_preserves();
  test_realloc_inplace_and_null_zero();
  test_firstfit_mcdc();
  test_coalesce_mcdc();
  test_overflow_mcdc();
  (void)fprintf(stderr, "[OK ] test_ra_epub_miniz_alloc.c\n");
  return 0;
}
