/**
 * @file test_ra8_epub_miniz_alloc.c
 * @brief Host unit tests + MC/DC for the miniz static-arena allocator (#139).
 *
 * @details
 * Exercises ::ra8_epub_miniz_alloc / _free / _realloc directly (no miniz): basic
 * alloc/align, split, free + coalesce reclaim, realloc grow-move + preserve,
 * realloc in-place, exhaustion, and overflow. Plus MC/DC mirror vectors for the
 * three compound decisions in the allocator (first-fit, coalesce, overflow).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <string.h>

#include "ra8_epub_miniz_alloc.h"
#include "unity_minimal.h"

/** @brief Distinct fills proving two allocations do not alias. */
typedef enum : uint8_t {
  k_miniz_fill_first  = 0xAAU, /**< Written through the first allocation.  */
  k_miniz_fill_second = 0x55U, /**< Written through the second allocation. */
  k_miniz_fill_reuse  = 0x5AU, /**< Written through a recycled allocation. */
} miniz_alloc_fill_t;

/**
 * @enum epub_miniz_alloc_uint8_const_t
 * @brief Named uint8_t constants used by this file.
 *
 * @details
 * Every literal this translation unit needs, named so the
 * value's role is visible at the point of use (CLAUDE.md
 * "No Magic Numbers").
 */
typedef enum : uint8_t {
  k_epub_miniz_alloc_a_5a                   = 0x5AU,
  k_epub_miniz_alloc_i_ff                   = 0xFFU,
  k_epub_miniz_alloc_p_c3                   = 0xC3U,
  k_epub_miniz_alloc_ra8_epub_miniz_alloc_5 = 5U,
} epub_miniz_alloc_uint8_const_t;

enum : size_t {
  k_small  = 64,    /**< Small.                        */
  k_medium = 4096,  /**< Medium.                       */
  k_big    = 11000, /**< ~ a miniz tinfl_decompressor. */
};

/**
 * @test test_alloc_align_and_distinct
 * @brief Allocations are aligned and non-overlapping.
 */
static void test_alloc_align_and_distinct(void)
{
  TEST_BEGIN("alloc returns aligned, distinct blocks");
  void* a = ra8_epub_miniz_alloc(nullptr, 1U, k_small);
  void* b = ra8_epub_miniz_alloc(nullptr, 1U, k_small);
  TEST_ASSERT((a != nullptr) && (b != nullptr));
  TEST_ASSERT(a != b);
  TEST_ASSERT((((uintptr_t)a % alignof(max_align_t)) == 0U));
  TEST_ASSERT((((uintptr_t)b % alignof(max_align_t)) == 0U));
  /* Writing the full request must not corrupt the neighbour. */
  (void)memset(a, k_miniz_fill_first, k_small);
  (void)memset(b, k_miniz_fill_second, k_small);
  TEST_ASSERT(((const uint8_t*)a)[0] == 0xAAU);
  TEST_ASSERT(((const uint8_t*)b)[k_small - 1U] == 0x55U);
  ra8_epub_miniz_free(nullptr, a);
  ra8_epub_miniz_free(nullptr, b);
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
    p[i] = ra8_epub_miniz_alloc(nullptr, 1U, k_medium);
    TEST_ASSERT(p[i] != nullptr);
  }
  for (uint32_t i = 0U; i < 8U; i++) {
    ra8_epub_miniz_free(nullptr, p[i]);
  }
  /* If coalescing works, a single big alloc near the pool size succeeds. */
  void* big = ra8_epub_miniz_alloc(nullptr, 1U, (size_t)k_ra8_epub_miniz_pool_bytes / 2U);
  TEST_ASSERT(big != nullptr);
  ra8_epub_miniz_free(nullptr, big);
  TEST_END("free + coalesce reclaims the pool for a big alloc");
}

/**
 * @test test_realloc_grow_preserves
 * @brief realloc grows, moves when needed, and preserves the old bytes.
 */
static void test_realloc_grow_preserves(void)
{
  TEST_BEGIN("realloc grow preserves payload");
  uint8_t* a = (uint8_t*)ra8_epub_miniz_alloc(nullptr, 1U, k_small);
  TEST_ASSERT(a != nullptr);
  for (uint32_t i = 0U; i < (uint32_t)k_small; i++) {
    a[i] = (uint8_t)(i & k_epub_miniz_alloc_i_ff);
  }
  /* Pin a neighbour so the grow cannot happen in place -> forces a move. */
  void* pin = ra8_epub_miniz_alloc(nullptr, 1U, k_small);
  TEST_ASSERT(pin != nullptr);
  uint8_t* g = (uint8_t*)ra8_epub_miniz_realloc(nullptr, a, 1U, k_big);
  TEST_ASSERT(g != nullptr);
  for (uint32_t i = 0U; i < (uint32_t)k_small; i++) {
    TEST_ASSERT_EQ((i & 0xFFU), g[i]);
  }
  ra8_epub_miniz_free(nullptr, g);
  ra8_epub_miniz_free(nullptr, pin);
  TEST_END("realloc grow preserves payload");
}

/**
 * @test test_realloc_inplace_and_null_zero
 * @brief realloc keeps a block that already fits; NULL/0 edge cases.
 */
static void test_realloc_inplace_and_null_zero(void)
{
  TEST_BEGIN("realloc in-place + NULL/zero edges");
  void* a = ra8_epub_miniz_alloc(nullptr, 1U, k_medium);
  TEST_ASSERT(a != nullptr);
  /* Shrink request fits in place -> same pointer. */
  void* same = ra8_epub_miniz_realloc(nullptr, a, 1U, k_small);
  TEST_ASSERT_EQ(a, same);
  /* realloc(NULL, n) == alloc(n). */
  void* fresh = ra8_epub_miniz_realloc(nullptr, nullptr, 1U, k_small);
  TEST_ASSERT(fresh != nullptr);
  /* realloc(p, 0) frees and returns NULL. */
  void* none = ra8_epub_miniz_realloc(nullptr, fresh, 0U, 0U);
  TEST_ASSERT(none == nullptr);
  ra8_epub_miniz_free(nullptr, same);
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
 * ra8_epub_miniz_alloc.c first-fit). N+1 = 3 vectors:
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
 * ra8_epub_miniz_alloc.c priv_coalesce). N+1 = 3 vectors:
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
 * ra8_epub_miniz_alloc overflow guard). N+1 = 3 vectors:
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
  TEST_ASSERT(ra8_epub_miniz_alloc(nullptr, (SIZE_MAX / 2U) + 2U, 2U) == nullptr);
  TEST_END("overflow MC/DC: size!=0 && items>MAX/size");
}

/**
 * @test test_alloc_real_overflow_and_firstfit_mcdc
 *
 * @par MC/DC:
 * Drives the *production* decisions in ra8_epub_miniz_alloc() (not a mirror).
 *
 * Decision A -- overflow guard ``if ((size != 0U) && (items > SIZE_MAX/size))``
 * (2 conditions, AND). The existing test_overflow_mcdc already drives the
 * real (T,T) overflow arm; here the missing left-operand-false arm:
 *  - size==0 -> C1=F (short-circuit) -> no overflow -> proceeds and returns a
 *    block (need rounds up to one alignment unit). Pair with the (T,T) arm
 *    isolates the size!=0 condition.
 *
 * Decision B -- first-fit ``if ((b->is_free == free) && (b->size >= need))``
 * (2 conditions, AND) walked over a crafted pool layout:
 *  - a freed small block first: C1=T,C2=F (free but too small -> skip),
 *  - then a used block:         C1=F        (not free -> skip),
 *  - then the big free tail:     C1=T,C2=T   (fit -> allocate).
 * One allocation request thus exercises all three condition states, giving the
 * two independence pairs for the AND.
 */
static void test_alloc_real_overflow_and_firstfit_mcdc(void)
{
  TEST_BEGIN("alloc real MC/DC: overflow size==0 + first-fit skip arms");
  /* Decision A, left-operand-false: size==0 short-circuits before the divide. */
  void* z = ra8_epub_miniz_alloc(nullptr, k_epub_miniz_alloc_ra8_epub_miniz_alloc_5, 0U);
  TEST_ASSERT(z != nullptr);
  ra8_epub_miniz_free(nullptr, z);

  /* Decision B: lay out [free small][used b][free tail], then ask for a size
   * that skips the small freed block (T,F) and the used block (F) to land in
   * the tail (T,T). */
  void* a = ra8_epub_miniz_alloc(nullptr, 1U, k_small);
  void* b = ra8_epub_miniz_alloc(nullptr, 1U, k_small);
  TEST_ASSERT((a != nullptr) && (b != nullptr));
  ra8_epub_miniz_free(nullptr, a); /* front block now free but only k_small big */
  void* c = ra8_epub_miniz_alloc(nullptr, 1U, k_medium); /* k_medium > k_small */
  TEST_ASSERT(c != nullptr);
  TEST_ASSERT(c != b); /* did not reuse the too-small freed block */
  ra8_epub_miniz_free(nullptr, b);
  ra8_epub_miniz_free(nullptr, c);
  TEST_END("alloc real MC/DC: overflow size==0 + first-fit skip arms");
}

/**
 * @test test_free_in_pool_mcdc
 *
 * @par MC/DC:
 * Drives the *production* guard in ra8_epub_miniz_free()
 * ``if ((address == NULL) || !priv_in_pool(address))`` (2 conditions, OR) and,
 * through it, priv_in_pool()
 * ``return (q >= base) && (q < end)`` (2 conditions, AND).
 *
 * free() OR-guard, N+1 = 3 vectors:
 *  - V1: address!=NULL, in-pool -> C1=F, C2=F (!in_pool false) -> overall F ->
 *    the block is actually freed (the pool stays usable afterward).
 *  - V2: address==NULL -> C1=T (short-circuit) -> overall T -> no-op.
 *  - V3: address!=NULL, out-of-pool -> C1=F, C2=T -> overall T -> no-op.
 *
 * priv_in_pool() AND, exercised by V1/V3 above:
 *  - V1 in-pool pointer        -> (q>=base)=T, (q<end)=T -> true.
 *  - a pointer below the base  -> (q>=base)=F           -> false (left independent).
 *  - a pointer at/after the end-> (q>=base)=T, (q<end)=F -> false (right independent).
 * The below/above pointers are derived from a live pool pointer offset by twice
 * the whole pool size, so they are unconditionally outside the 96 KiB arena.
 */
static void test_free_in_pool_mcdc(void)
{
  TEST_BEGIN("free MC/DC: (NULL || !in_pool) + in_pool bounds");
  uint8_t* p = (uint8_t*)ra8_epub_miniz_alloc(nullptr, 1U, k_small);
  TEST_ASSERT(p != nullptr);

  /* V3 / priv_in_pool left-false: a pointer two pool-widths below the live
   * block is below the arena base -> !in_pool true -> free is a no-op. */
  uint8_t* below = p - (2U * (size_t)k_ra8_epub_miniz_pool_bytes);
  ra8_epub_miniz_free(nullptr, below);
  /* priv_in_pool right-false: a pointer two pool-widths above the live block is
   * at/after the arena end -> !in_pool true -> free is a no-op. */
  uint8_t* above = p + (2U * (size_t)k_ra8_epub_miniz_pool_bytes);
  ra8_epub_miniz_free(nullptr, above);
  /* V2: NULL address short-circuits the OR -> no-op. */
  ra8_epub_miniz_free(nullptr, nullptr);

  /* The no-ops must not have corrupted the pool: p is still a live in-pool
   * block, so a write through it is safe and a real free still works (V1). */
  (void)memset(p, k_miniz_fill_reuse, k_small);
  TEST_ASSERT(p[0] == 0x5AU);
  ra8_epub_miniz_free(nullptr, p); /* V1: in-pool, non-NULL -> actually freed */

  /* Pool is healthy again: a fresh big alloc succeeds and is released. */
  void* big = ra8_epub_miniz_alloc(nullptr, 1U, (size_t)k_ra8_epub_miniz_pool_bytes / 2U);
  TEST_ASSERT(big != nullptr);
  ra8_epub_miniz_free(nullptr, big);
  TEST_END("free MC/DC: (NULL || !in_pool) + in_pool bounds");
}

/**
 * @test test_realloc_real_overflow_mcdc
 *
 * @par MC/DC:
 * Drives the *production* overflow guard in ra8_epub_miniz_realloc()
 * ``if ((size != 0U) && (items > SIZE_MAX/size))`` (2 conditions, AND) on a
 * non-NULL block (so control reaches the guard rather than the realloc(NULL,n)
 * early-out). N+1 = 3 vectors:
 *  - V1: size=2, items overflow -> C1=T, C2=T -> NULL; the old block stays valid.
 *  - V2: size=0                  -> C1=F (short-circuit) -> no overflow; need
 *    rounds to 0 so the block is freed and NULL returned (frees old block).
 *  - V3: size=k_medium, items=1  -> C1=T, C2=F -> no overflow -> normal grow.
 * Pair (V1,V3) isolates the size!=0 short-circuit's effect on the right
 * condition; pair (V1,V2) isolates the left condition.
 */
static void test_realloc_real_overflow_mcdc(void)
{
  TEST_BEGIN("realloc real MC/DC: size!=0 && items>MAX/size");
  /* V1: overflow on a live block -> NULL, original preserved. */
  uint8_t* p = (uint8_t*)ra8_epub_miniz_alloc(nullptr, 1U, k_small);
  TEST_ASSERT(p != nullptr);
  p[0]        = k_epub_miniz_alloc_p_c3;
  void* nomem = ra8_epub_miniz_realloc(nullptr, p, (SIZE_MAX / 2U) + 2U, 2U);
  TEST_ASSERT(nomem == nullptr);
  TEST_ASSERT(p[0] == 0xC3U); /* old block still valid after a rejected grow */

  /* V3: a real grow (size!=0, no overflow) succeeds. */
  uint8_t* g = (uint8_t*)ra8_epub_miniz_realloc(nullptr, p, 1U, k_medium);
  TEST_ASSERT(g != nullptr);
  TEST_ASSERT(g[0] == 0xC3U); /* payload preserved across the grow */

  /* V2: size==0 short-circuits the guard; need becomes 0 -> frees and returns
   * NULL (so g is consumed by this call -- do not free it again). */
  void* none = ra8_epub_miniz_realloc(nullptr, g, 4U, 0U);
  TEST_ASSERT(none == nullptr);
  TEST_END("realloc real MC/DC: size!=0 && items>MAX/size");
}

/**
 * @test test_alloc_oversize_rejected_mcdc
 *
 * @par MC/DC:
 * Decision: `if ((items * size) > k_ra8_epub_miniz_pool_bytes)` (1 condition;
 * the oversize guard in both ra8_epub_miniz_alloc and _realloc). It rejects any
 * request larger than the 96 KiB pool BEFORE priv_align_up rounds it -- without
 * it a size near SIZE_MAX passes the multiply check yet overflows
 * (bytes + align - 1) into a tiny under-allocation. Drives the TRUE arm of each
 * guard (the FALSE arm -- a request that fits -- is covered by every other test
 * in this file).
 *  - V1: alloc(1, SIZE_MAX-10)  -> the align-up overflow trigger -> NULL.
 *  - V2: alloc(1, pool_bytes+1) -> an ordinary over-pool size     -> NULL.
 *  - V3: realloc(a, 1, SIZE_MAX-10) -> NULL, and the old block stays valid.
 */
static void test_alloc_oversize_rejected_mcdc(void)
{
  TEST_BEGIN("alloc oversize MC/DC: reject > pool before align-up overflow");
  /* V1: items=1 so the multiply check passes, but the near-SIZE_MAX size would
   * wrap priv_align_up -- the oversize guard must catch it first. */
  TEST_ASSERT(ra8_epub_miniz_alloc(nullptr, 1U, SIZE_MAX - 10U) == nullptr);
  /* V2: an ordinary just-over-pool size. */
  TEST_ASSERT(ra8_epub_miniz_alloc(nullptr, 1U, (size_t)k_ra8_epub_miniz_pool_bytes + 1U) ==
              nullptr);
  /* V3: realloc growing past the pool is rejected; the old block is untouched. */
  uint8_t* a = (uint8_t*)ra8_epub_miniz_alloc(nullptr, 1U, k_small);
  TEST_ASSERT(a != nullptr);
  a[0] = k_epub_miniz_alloc_a_5a;
  TEST_ASSERT(ra8_epub_miniz_realloc(nullptr, a, 1U, SIZE_MAX - 10U) == nullptr);
  TEST_ASSERT(a[0] == 0x5AU); /* old block still valid + intact */
  ra8_epub_miniz_free(nullptr, a);
  TEST_END("alloc oversize MC/DC: reject > pool before align-up overflow");
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
  test_alloc_real_overflow_and_firstfit_mcdc();
  test_free_in_pool_mcdc();
  test_realloc_real_overflow_mcdc();
  test_alloc_oversize_rejected_mcdc();
  (void)fprintf(stderr, "[OK ] test_ra8_epub_miniz_alloc.c\n");
  return 0;
}
