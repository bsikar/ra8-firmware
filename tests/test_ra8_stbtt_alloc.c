/**
 * @file test_ra8_stbtt_alloc.c
 * @brief Unit tests for libs/ra8_reflow/src/ra8_stbtt_alloc.c
 *
 * @details
 * Exercises the bump-arena-with-refcount-auto-reset allocator that backs
 * stb_truetype's glyph scratch on the no-heap firmware: alignment,
 * exhaustion -> nullptr, nullptr-free tolerance, partial-free does NOT
 * rewind, full-drain auto-reset, and the high-water diagnostic.
 *
 * The allocator contains no compound boolean decisions; every branch is a
 * single condition. Each test's @par MC/DC block therefore documents the
 * two-vector (true / false) coverage of the single-condition guards it
 * drives -- DO-178C 6.4.4.3 treats a single condition as trivially
 * MC/DC-covered by exercising both outcomes.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8_stbtt_alloc.h"
#include "unity_minimal.h"

/**
 * @enum t_alloc_t
 * @brief Allocation size used by every arena arm.
 *
 * @details
 * One size throughout, so successive allocations are directly comparable: the
 * bump-pointer arms assert on the exact delta between two of them.
 */
typedef enum : uint8_t {
  k_t_alloc_size = 64U, /**< Bytes requested per allocation. */
} t_alloc_t;

enum : uint32_t {
  k_align       = 16,        /**< Mirror of k_ra8_stbtt_align.       */
  k_arena_bytes = 96 * 1024, /**< Mirror of k_ra8_stbtt_arena_bytes. */
};

/**
 * @test test_alignment_and_high_water
 * Two small allocations are 16-byte aligned and spaced by exactly one
 * aligned slot; the high-water mark advances monotonically.
 *
 * @par MC/DC:
 * Decision: ``if (s_offset > s_high_water)`` in ra8_stbtt_malloc
 * (1 condition, libs/ra8_reflow/src/ra8_stbtt_alloc.c@ra8_stbtt_malloc).
 *  - V1: first malloc, s_offset(16) > s_high_water(0) = T -> high-water
 *    advances.
 * The companion false outcome (no advance once the mark is reached) is
 * driven in test_full_drain_auto_reset when a re-allocation does not
 * exceed the established mark.
 */
static void test_alignment_and_high_water(void)
{
  TEST_BEGIN("ra8_stbtt_alloc: alignment + high-water");
  uint8_t* a = (uint8_t*)ra8_stbtt_malloc(1U);
  uint8_t* b = (uint8_t*)ra8_stbtt_malloc(1U);
  TEST_ASSERT_NOT_NULL(a);
  TEST_ASSERT_NOT_NULL(b);
  TEST_ASSERT_EQ(0, ((uintptr_t)a % (uintptr_t)k_align));
  TEST_ASSERT_EQ(0, ((uintptr_t)b % (uintptr_t)k_align));
  TEST_ASSERT_EQ(k_align, ((uintptr_t)b - (uintptr_t)a));
  ra8_stbtt_free(a);
  ra8_stbtt_free(b);
  TEST_ASSERT_EQ((2 * k_align), ra8_stbtt_alloc_high_water());
  TEST_END("ra8_stbtt_alloc: alignment + high-water");
}

/**
 * @test test_full_drain_auto_reset
 * Freeing every live block rewinds the arena: the next allocation reuses
 * the base address. This is the property that makes the allocator safe
 * for callers (ra8_reflow, ra8_epub) that never call an explicit reset.
 *
 * @par MC/DC:
 * Decision: ``if (s_live == 0U)`` in ra8_stbtt_free
 * (1 condition, libs/ra8_reflow/src/ra8_stbtt_alloc.c@ra8_stbtt_free).
 *  - V1: after freeing both blocks, s_live == 0 = T -> offset rewinds to
 *    base (verified: the re-allocation returns the original address).
 * The companion s_high_water decision stays false here (the re-allocation
 * does not exceed the prior mark), covering its false outcome.
 */
static void test_full_drain_auto_reset(void)
{
  TEST_BEGIN("ra8_stbtt_alloc: full-drain auto-reset");
  uintptr_t base = (uintptr_t)ra8_stbtt_malloc(k_t_alloc_size);
  uint8_t*  p2   = (uint8_t*)ra8_stbtt_malloc(k_t_alloc_size);
  TEST_ASSERT_NOT_NULL((void*)base);
  TEST_ASSERT_NOT_NULL(p2);
  ra8_stbtt_free((void*)base);
  ra8_stbtt_free(p2);
  uintptr_t again = (uintptr_t)ra8_stbtt_malloc(k_t_alloc_size);
  TEST_ASSERT_EQ(base, again);
  ra8_stbtt_free((void*)again);
  TEST_END("ra8_stbtt_alloc: full-drain auto-reset");
}

/**
 * @test test_partial_free_no_rewind
 * A free that leaves blocks live must NOT rewind the offset, or a live
 * block would be handed out twice.
 *
 * @par MC/DC:
 * Decision: ``if (s_live == 0U)`` in ra8_stbtt_free
 * (1 condition, libs/ra8_reflow/src/ra8_stbtt_alloc.c@ra8_stbtt_free).
 *  - V2: after one of two blocks is freed, s_live == 0 = F -> offset is
 *    NOT rewound (verified: the next allocation aliases neither live
 *    block). Pairs with V1 in test_full_drain_auto_reset for full MC/DC.
 */
static void test_partial_free_no_rewind(void)
{
  TEST_BEGIN("ra8_stbtt_alloc: partial free does not rewind");
  uint8_t* a = (uint8_t*)ra8_stbtt_malloc(k_t_alloc_size);
  uint8_t* b = (uint8_t*)ra8_stbtt_malloc(k_t_alloc_size);
  TEST_ASSERT_NOT_NULL(a);
  TEST_ASSERT_NOT_NULL(b);
  ra8_stbtt_free(a); /* b still live */
  uint8_t* c = (uint8_t*)ra8_stbtt_malloc(k_t_alloc_size);
  TEST_ASSERT_NOT_NULL(c);
  TEST_ASSERT((uintptr_t)c != (uintptr_t)a);
  TEST_ASSERT((uintptr_t)c != (uintptr_t)b);
  ra8_stbtt_free(b);
  ra8_stbtt_free(c); /* drains -> reset */
  TEST_END("ra8_stbtt_alloc: partial free does not rewind");
}

/**
 * @test test_exhaustion_returns_null
 * Over-capacity and oversized requests return nullptr (stb tolerates it
 * and skips the glyph) without disturbing the live arena.
 *
 * @par MC/DC:
 * Decisions in ra8_stbtt_malloc
 * (libs/ra8_reflow/src/ra8_stbtt_alloc.c@ra8_stbtt_malloc), each 1 condition:
 *  - ``if (n > k_ra8_stbtt_arena_bytes)``: V-T request exceeds capacity
 *    outright -> nullptr; V-F normal requests elsewhere proceed.
 *  - ``if (aligned > k_ra8_stbtt_arena_bytes - s_offset)``: V-T the
 *    follow-on 1-byte request after the arena is full -> nullptr; V-F the
 *    whole-arena and post-drain requests succeed.
 */
static void test_exhaustion_returns_null(void)
{
  TEST_BEGIN("ra8_stbtt_alloc: exhaustion -> nullptr");
  TEST_ASSERT_NULL(ra8_stbtt_malloc((size_t)k_arena_bytes + 1U));
  void* whole = ra8_stbtt_malloc((size_t)k_arena_bytes);
  TEST_ASSERT_NOT_NULL(whole);
  TEST_ASSERT_NULL(ra8_stbtt_malloc(1U)); /* nothing left    */
  ra8_stbtt_free(whole);                  /* drains -> reset */
  void* small = ra8_stbtt_malloc(1U);
  TEST_ASSERT_NOT_NULL(small);
  ra8_stbtt_free(small);
  TEST_END("ra8_stbtt_alloc: exhaustion -> nullptr");
}

/**
 * @test test_null_free_is_noop
 * Freeing nullptr must not underflow the live count (which would corrupt
 * the auto-reset bookkeeping).
 *
 * @par MC/DC:
 * Decision: ``if (p == nullptr)`` in ra8_stbtt_free
 * (1 condition, libs/ra8_reflow/src/ra8_stbtt_alloc.c@ra8_stbtt_free).
 *  - V-T: free(nullptr) returns early, leaving s_live untouched.
 *  - V-F: free of a real block decrements s_live (exercised by every
 *    other test). Verified here: after two nullptr-frees a real
 *    alloc/free/alloc round-trip still auto-resets to the base address.
 */
static void test_null_free_is_noop(void)
{
  TEST_BEGIN("ra8_stbtt_alloc: free(nullptr) is a no-op");
  ra8_stbtt_free(nullptr);
  ra8_stbtt_free(nullptr);
  uintptr_t base = (uintptr_t)ra8_stbtt_malloc(32U);
  ra8_stbtt_free((void*)base);
  uintptr_t again = (uintptr_t)ra8_stbtt_malloc(32U);
  TEST_ASSERT_EQ(base, again);
  ra8_stbtt_free((void*)again);
  TEST_END("ra8_stbtt_alloc: free(nullptr) is a no-op");
}

int32_t main(void)
{
  test_alignment_and_high_water();
  test_full_drain_auto_reset();
  test_partial_free_no_rewind();
  test_exhaustion_returns_null();
  test_null_free_is_noop();
  return 0;
}
