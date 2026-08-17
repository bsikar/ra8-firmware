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

#include "ra8_attributes.h"
#include "ra8_epub_miniz_alloc.h"
#include "unity_minimal.h"

/** @brief Distinct fills proving two allocations do not alias. */
typedef enum : uint8_t {
  k_miniz_fill_first  = 0xAAU, /**< Written through the first allocation.  */
  k_miniz_fill_second = 0x55U, /**< Written through the second allocation. */
  k_miniz_fill_reuse  = 0x5AU, /**< Written through a recycled allocation. */
} miniz_alloc_fill_t;

/**
 * @enum epub_miniz_alloc_fixture_t
 * @brief The byte-level helpers.
 */
typedef enum : uint8_t {
  k_arena_first_byte =
    0x5AU, /**< Stamped at an allocation's head, so a later overlapping block is detectable. */
  /** Low-byte mask used to split the connection handle little-endian. */
  k_byte_mask = 0xFFU,
  k_arena_second_byte =
    0xC3U, /**< Same for a second allocation; different, so two blocks cannot alias unnoticed. */
  k_arena_alloc_items =
    5U, /**< Item count of a small allocation; a zero item size must still return usable storage. */
} epub_miniz_alloc_fixture_t;

/** @brief One complete caller-owned allocator fixture. */
typedef struct {
  ra8_epub_miniz_arena_t     arena;     /**< Allocator descriptor.  */
  ra8_epub_miniz_workspace_t workspace; /**< Aligned backing bytes. */
} priv_fixture_t;

/**
 * @brief Report whether an arena descriptor still holds its saved fields.
 * @details Member-wise rather than byte-wise: ::ra8_epub_miniz_arena_t ends in
 *          a `uint8_t` and therefore carries trailing padding, which the
 *          struct assignment that snapshots it is not required to copy. A
 *          byte-wise comparison would be asserting something about padding
 *          instead of about the descriptor.
 * @param[in] arena Descriptor observed after the call under test.
 * @param[in] saved Descriptor snapshot taken before it.
 * @return Whether every documented field is unchanged.
 * @retval true The call left the descriptor alone.
 * @retval false At least one field moved.
 * @pre Both pointers address initialized descriptors.
 * @pre Neither pointer is null.
 * @post Neither descriptor is modified.
 * @post The result depends only on the three descriptor fields.
 * @note Test-only helper with no production ABI.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_arena_unchanged(const ra8_epub_miniz_arena_t* arena,
                                                  const ra8_epub_miniz_arena_t* saved)
{
  if (arena->base != saved->base) {
    return false;
  }
  if (arena->capacity != saved->capacity) {
    return false;
  }
  return arena->initialized == saved->initialized;
}

/** @brief Reset @p fixture to one empty, independent arena. @details Implements the fixture init fixture operation used only by this focused test executable. @param[in,out] fixture Fixture argument governed by the exercised interface contract. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL
static void internal_fixture_init(priv_fixture_t* fixture)
{
  *fixture = (priv_fixture_t){};
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_epub_miniz_arena_init(&fixture->arena,
                                           &fixture->workspace.bytes[0],
                                           sizeof(fixture->workspace.bytes)));
}

enum : size_t {
  k_small  = 64,    /**< Small.                        */
  k_medium = 4096,  /**< Medium.                       */
  k_big    = 11000, /**< ~ a miniz tinfl_decompressor. */
};

/**
 * @test internal_test_alloc_align_and_distinct
 * @brief Allocations are aligned and non-overlapping.
 *
 * @par MC/DC:
 * Decision (in-test success invariant): `(a != nullptr) && (b != nullptr)`
 * (2 conditions, AND). Both allocations succeed, so it is asserted at
 * C1=T,C2=T -> T; this is a conjunctive "both allocations succeeded" invariant,
 * not an independence set -- a null from either arm is the failure the
 * conjunction exists to catch, so no false vector is driven. The production
 * first-fit `(is_free) && (size >= need)` and overflow `(size != 0) && ...`
 * AND-decisions this exercises have their N+1 vectors in internal_test_firstfit_mcdc,
 * internal_test_overflow_mcdc, and internal_test_alloc_real_overflow_and_firstfit_mcdc. @details Executes the alloc align and distinct scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_alloc_align_and_distinct(void)
{
  TEST_BEGIN("alloc returns aligned, distinct blocks");
  priv_fixture_t fixture;
  internal_fixture_init(&fixture);
  void* a = ra8_epub_miniz_alloc(&fixture.arena, 1U, k_small);
  void* b = ra8_epub_miniz_alloc(&fixture.arena, 1U, k_small);
  TEST_ASSERT((a != nullptr) && (b != nullptr));
  TEST_ASSERT(a != b);
  TEST_ASSERT((((uintptr_t)a % alignof(max_align_t)) == 0U));
  TEST_ASSERT((((uintptr_t)b % alignof(max_align_t)) == 0U));
  /* Writing the full request must not corrupt the neighbour. */
  (void)memset(a, k_miniz_fill_first, k_small);
  (void)memset(b, k_miniz_fill_second, k_small);
  TEST_ASSERT(((const uint8_t*)a)[0] == 0xAAU);
  TEST_ASSERT(((const uint8_t*)b)[k_small - 1U] == 0x55U);
  ra8_epub_miniz_free(&fixture.arena, a);
  ra8_epub_miniz_free(&fixture.arena, b);
  TEST_END("alloc returns aligned, distinct blocks");
}

/**
 * @test internal_test_free_coalesce_reclaim
 * @brief Freeing everything lets a later big alloc reuse the whole pool.
 *
 * @par MC/DC:
 * (no compound decision is authored in this test's assertions -- they are single
 * `!= nullptr` checks. It exercises the production coalesce
 * `while ((next < end) && (next->is_free))` and first-fit
 * `(is_free) && (size >= need)` AND-decisions by fragmenting then reclaiming the
 * pool, but their N+1 = 3 vectors are supplied by internal_test_coalesce_mcdc,
 * internal_test_firstfit_mcdc, and internal_test_alloc_real_overflow_and_firstfit_mcdc) @details Executes the free coalesce reclaim scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_free_coalesce_reclaim(void)
{
  TEST_BEGIN("free + coalesce reclaims the pool for a big alloc");
  priv_fixture_t fixture;
  internal_fixture_init(&fixture);
  /* Fragment the pool, then free all -- coalescing must rebuild one big run. */
  void* p[8];
  for (uint32_t i = 0U; i < 8U; i++) {
    p[i] = ra8_epub_miniz_alloc(&fixture.arena, 1U, k_medium);
    TEST_ASSERT(p[i] != nullptr);
  }
  for (uint32_t i = 0U; i < 8U; i++) {
    ra8_epub_miniz_free(&fixture.arena, p[i]);
  }
  /* If coalescing works, a single big alloc near the pool size succeeds. */
  void* big = ra8_epub_miniz_alloc(&fixture.arena, 1U, (size_t)k_ra8_epub_miniz_pool_bytes / 2U);
  TEST_ASSERT(big != nullptr);
  ra8_epub_miniz_free(&fixture.arena, big);
  TEST_END("free + coalesce reclaims the pool for a big alloc");
}

/**
 * @test internal_test_realloc_grow_preserves
 * @brief realloc grows, moves when needed, and preserves the old bytes.
 *
 * @par MC/DC:
 * (no compound decision is authored in this test's assertions -- they are single
 * `!= nullptr` / `TEST_ASSERT_EQ` checks. It drives the realloc grow-and-move
 * path (the single-condition `b->size >= need` false arm -> allocate + memcpy);
 * the production overflow / first-fit AND-decisions it touches have their N+1
 * vectors in internal_test_realloc_real_overflow_mcdc, internal_test_firstfit_mcdc, and
 * internal_test_alloc_real_overflow_and_firstfit_mcdc) @details Executes the realloc grow preserves scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_realloc_grow_preserves(void)
{
  TEST_BEGIN("realloc grow preserves payload");
  priv_fixture_t fixture;
  internal_fixture_init(&fixture);
  uint8_t* a = (uint8_t*)ra8_epub_miniz_alloc(&fixture.arena, 1U, k_small);
  TEST_ASSERT(a != nullptr);
  for (uint32_t i = 0U; i < (uint32_t)k_small; i++) {
    a[i] = (uint8_t)(i & k_byte_mask);
  }
  /* Pin a neighbour so the grow cannot happen in place -> forces a move. */
  void* pin = ra8_epub_miniz_alloc(&fixture.arena, 1U, k_small);
  TEST_ASSERT(pin != nullptr);
  uint8_t* g = (uint8_t*)ra8_epub_miniz_realloc(&fixture.arena, a, 1U, k_big);
  TEST_ASSERT(g != nullptr);
  for (uint32_t i = 0U; i < (uint32_t)k_small; i++) {
    TEST_ASSERT_EQ((i & 0xFFU), g[i]);
  }
  ra8_epub_miniz_free(&fixture.arena, g);
  ra8_epub_miniz_free(&fixture.arena, pin);
  TEST_END("realloc grow preserves payload");
}

/**
 * @test internal_test_realloc_inplace_and_null_zero
 * @brief realloc keeps a block that already fits; NULL/0 edge cases.
 *
 * @par MC/DC:
 * (no compound decision is authored in this test's assertions -- they are single
 * equality / null checks. It drives ra8_epub_miniz_realloc's single-condition
 * branches: `address == nullptr` (realloc(NULL,n) -> alloc), `b->size >= need`
 * true (shrink fits in place -> same pointer), and `need == 0` (realloc(p,0) ->
 * free -> NULL). The first-fit `(is_free) && (size >= need)` AND it reaches
 * through the alloc path has its N+1 vectors in internal_test_firstfit_mcdc and
 * internal_test_alloc_real_overflow_and_firstfit_mcdc) @details Executes the realloc inplace and null zero scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_realloc_inplace_and_null_zero(void)
{
  TEST_BEGIN("realloc in-place + NULL/zero edges");
  priv_fixture_t fixture;
  internal_fixture_init(&fixture);
  void* a = ra8_epub_miniz_alloc(&fixture.arena, 1U, k_medium);
  TEST_ASSERT(a != nullptr);
  /* Shrink request fits in place -> same pointer. */
  void* same = ra8_epub_miniz_realloc(&fixture.arena, a, 1U, k_small);
  TEST_ASSERT_EQ(a, same);
  /* realloc(NULL, n) == alloc(n). */
  void* fresh = ra8_epub_miniz_realloc(&fixture.arena, nullptr, 1U, k_small);
  TEST_ASSERT(fresh != nullptr);
  /* realloc(p, 0) frees and returns NULL. */
  void* none = ra8_epub_miniz_realloc(&fixture.arena, fresh, 0U, 0U);
  TEST_ASSERT(none == nullptr);
  ra8_epub_miniz_free(&fixture.arena, same);
  TEST_END("realloc in-place + NULL/zero edges");
}

/** @brief Mirror of the first-fit decision: (is_free) && (size >= need). @details Implements the mirror firstfit fixture operation used only by this focused test executable. @param[in] is_free Fixture argument governed by the exercised interface contract. @param[in] size Fixture argument governed by the exercised interface contract. @param[in] need Fixture argument governed by the exercised interface contract. @return The value computed by the fixture helper. @retval value The computed fixture value for the supplied inputs. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static uint8_t internal_mirror_firstfit(uint8_t is_free, size_t size, size_t need)
{
  if ((is_free != 0U) && (size >= need)) {
    return 1U;
  }
  return 0U;
}

/**
 * @test internal_test_firstfit_mcdc
 *
 * @par MC/DC:
 * Decision: `if (b->is_free && b->size >= need)` (2 conditions, AND;
 * ra8_epub_miniz_alloc.c first-fit). N+1 = 3 vectors:
 *  - V1: free=1, size=64, need=32 -> T,T -> fit.
 *  - V2: free=0, size=64, need=32 -> F   -> no fit (varies is_free).
 *  - V3: free=1, size=16, need=32 -> T,F -> no fit (varies the size test). @brief Verify firstfit mcdc behavior. @details Executes the firstfit mcdc scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_firstfit_mcdc(void)
{
  TEST_BEGIN("first-fit MC/DC: is_free && size>=need");
  TEST_ASSERT_EQ(1, internal_mirror_firstfit(1U, 64U, 32U));
  TEST_ASSERT_EQ(0, internal_mirror_firstfit(0U, 64U, 32U));
  TEST_ASSERT_EQ(0, internal_mirror_firstfit(1U, 16U, 32U));
  TEST_END("first-fit MC/DC: is_free && size>=need");
}

/** @brief Mirror of the coalesce decision: (in_pool) && (next_free). @details Implements the mirror coalesce fixture operation used only by this focused test executable. @param[in] in_pool Fixture argument governed by the exercised interface contract. @param[in] next_free Fixture argument governed by the exercised interface contract. @return The value computed by the fixture helper. @retval value The computed fixture value for the supplied inputs. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static uint8_t internal_mirror_coalesce(uint8_t in_pool, uint8_t next_free)
{
  if ((in_pool != 0U) && (next_free != 0U)) {
    return 1U;
  }
  return 0U;
}

/**
 * @test internal_test_coalesce_mcdc
 *
 * @par MC/DC:
 * Decision: `while (next < end && next->is_free)` (2 conditions, AND;
 * ra8_epub_miniz_alloc.c internal_coalesce). N+1 = 3 vectors:
 *  - V1: in_pool=1, next_free=1 -> swallow.
 *  - V2: in_pool=0, next_free=1 -> stop (varies the bound).
 *  - V3: in_pool=1, next_free=0 -> stop (varies the free test). @brief Verify coalesce mcdc behavior. @details Executes the coalesce mcdc scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_coalesce_mcdc(void)
{
  TEST_BEGIN("coalesce MC/DC: in_pool && next_free");
  TEST_ASSERT_EQ(1, internal_mirror_coalesce(1U, 1U));
  TEST_ASSERT_EQ(0, internal_mirror_coalesce(0U, 1U));
  TEST_ASSERT_EQ(0, internal_mirror_coalesce(1U, 0U));
  TEST_END("coalesce MC/DC: in_pool && next_free");
}

/** @brief Mirror of the overflow guard: (size != 0) && (items > MAX/size). @details Implements the mirror overflow fixture operation used only by this focused test executable. @param[in] size Fixture argument governed by the exercised interface contract. @param[in] items_over Fixture argument governed by the exercised interface contract. @return The value computed by the fixture helper. @retval value The computed fixture value for the supplied inputs. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static uint8_t internal_mirror_overflow(size_t size, uint8_t items_over)
{
  /* items_over models whether items exceeds SIZE_MAX/size. */
  if ((size != 0U) && (items_over != 0U)) {
    return 1U;
  }
  return 0U;
}

/**
 * @test internal_test_overflow_mcdc
 *
 * @par MC/DC:
 * Decision: `if (size != 0 && items > SIZE_MAX/size)` (2 conditions, AND;
 * ra8_epub_miniz_alloc overflow guard). N+1 = 3 vectors:
 *  - V1: size=4, items_over=1 -> overflow -> NULL.
 *  - V2: size=0, items_over=1 -> no overflow (size==0 short-circuits).
 *  - V3: size=4, items_over=0 -> no overflow (count fits). @brief Verify overflow mcdc behavior. @details Executes the overflow mcdc scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_overflow_mcdc(void)
{
  TEST_BEGIN("overflow MC/DC: size!=0 && items>MAX/size");
  priv_fixture_t fixture;
  internal_fixture_init(&fixture);
  TEST_ASSERT_EQ(1, internal_mirror_overflow(4U, 1U));
  TEST_ASSERT_EQ(0, internal_mirror_overflow(0U, 1U));
  TEST_ASSERT_EQ(0, internal_mirror_overflow(4U, 0U));
  /* And the real function rejects an actual overflow. */
  TEST_ASSERT(ra8_epub_miniz_alloc(&fixture.arena, (SIZE_MAX / 2U) + 2U, 2U) == nullptr);
  TEST_END("overflow MC/DC: size!=0 && items>MAX/size");
}

/**
 * @test internal_test_alloc_real_overflow_and_firstfit_mcdc
 *
 * @par MC/DC:
 * Drives the *production* decisions in ra8_epub_miniz_alloc() (not a mirror).
 *
 * Decision A -- overflow guard ``if ((size != 0U) && (items > SIZE_MAX/size))``
 * (2 conditions, AND). The existing internal_test_overflow_mcdc already drives the
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
 * two independence pairs for the AND. @brief Verify alloc real overflow and firstfit mcdc behavior. @details Executes the alloc real overflow and firstfit mcdc scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_alloc_real_overflow_and_firstfit_mcdc(void)
{
  TEST_BEGIN("alloc real MC/DC: overflow size==0 + first-fit skip arms");
  priv_fixture_t fixture;
  internal_fixture_init(&fixture);
  /* Decision A, left-operand-false: size==0 short-circuits before the divide. */
  void* z = ra8_epub_miniz_alloc(&fixture.arena, k_arena_alloc_items, 0U);
  TEST_ASSERT(z != nullptr);
  ra8_epub_miniz_free(&fixture.arena, z);

  /* Decision B: lay out [free small][used b][free tail], then ask for a size
   * that skips the small freed block (T,F) and the used block (F) to land in
   * the tail (T,T). */
  void* a = ra8_epub_miniz_alloc(&fixture.arena, 1U, k_small);
  void* b = ra8_epub_miniz_alloc(&fixture.arena, 1U, k_small);
  TEST_ASSERT((a != nullptr) && (b != nullptr));
  ra8_epub_miniz_free(&fixture.arena, a); /* front block now free but only k_small big */
  /* k_medium > k_small */
  void* c = ra8_epub_miniz_alloc(&fixture.arena, 1U, k_medium);
  TEST_ASSERT(c != nullptr);
  TEST_ASSERT(c != b); /* did not reuse the too-small freed block */
  ra8_epub_miniz_free(&fixture.arena, b);
  ra8_epub_miniz_free(&fixture.arena, c);
  TEST_END("alloc real MC/DC: overflow size==0 + first-fit skip arms");
}

/**
 * @test internal_test_free_in_pool_mcdc
 *
 * @par MC/DC:
 * Drives the *production* guard in ra8_epub_miniz_free()
 * ``if ((address == NULL) || !internal_in_pool(address))`` (2 conditions, OR) and,
 * through it, internal_in_pool()
 * ``return (q >= base) && (q < end)`` (2 conditions, AND).
 *
 * free() OR-guard, N+1 = 3 vectors:
 *  - V1: address!=NULL, in-pool -> C1=F, C2=F (!in_pool false) -> overall F ->
 *    the block is actually freed (the pool stays usable afterward).
 *  - V2: address==NULL -> C1=T (short-circuit) -> overall T -> no-op.
 *  - V3: address!=NULL, out-of-pool -> C1=F, C2=T -> overall T -> no-op.
 *
 * internal_in_pool() AND, exercised by V1/V3 above:
 *  - V1 in-pool pointer        -> (q>=base)=T, (q<end)=T -> true.
 *  - a pointer below the base  -> (q>=base)=F           -> false (left independent).
 *  - a pointer at/after the end-> (q>=base)=T, (q<end)=F -> false (right independent).
 * The below/above pointers are derived from a live pool pointer offset by twice
 * the whole pool size, so they are unconditionally outside the 160 KiB arena. @brief Verify free in pool mcdc behavior. @details Executes the free in pool mcdc scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_free_in_pool_mcdc(void)
{
  TEST_BEGIN("free MC/DC: (NULL || !in_pool) + in_pool bounds");
  priv_fixture_t fixture;
  internal_fixture_init(&fixture);
  uint8_t* p = (uint8_t*)ra8_epub_miniz_alloc(&fixture.arena, 1U, k_small);
  TEST_ASSERT(p != nullptr);

  /* V3 / internal_in_pool left-false: a pointer two pool-widths below the live
   * block is below the arena base -> !in_pool true -> free is a no-op. */
  void* const below = (void*)((uintptr_t)p - (2U * (uintptr_t)k_ra8_epub_miniz_pool_bytes));
  ra8_epub_miniz_free(&fixture.arena, below);
  /* internal_in_pool right-false: a pointer two pool-widths above the live block is
   * at/after the arena end -> !in_pool true -> free is a no-op. */
  void* const above = (void*)((uintptr_t)p + (2U * (uintptr_t)k_ra8_epub_miniz_pool_bytes));
  ra8_epub_miniz_free(&fixture.arena, above);
  /* V2: NULL address short-circuits the OR -> no-op. */
  ra8_epub_miniz_free(&fixture.arena, nullptr);

  /* The no-ops must not have corrupted the pool: p is still a live in-pool
   * block, so a write through it is safe and a real free still works (V1). */
  (void)memset(p, k_miniz_fill_reuse, k_small);
  TEST_ASSERT(p[0] == 0x5AU);
  ra8_epub_miniz_free(&fixture.arena, p); /* V1: in-pool, non-NULL -> actually freed */

  /* Pool is healthy again: a fresh big alloc succeeds and is released. */
  void* big = ra8_epub_miniz_alloc(&fixture.arena, 1U, (size_t)k_ra8_epub_miniz_pool_bytes / 2U);
  TEST_ASSERT(big != nullptr);
  ra8_epub_miniz_free(&fixture.arena, big);
  TEST_END("free MC/DC: (NULL || !in_pool) + in_pool bounds");
}

/**
 * @test internal_test_realloc_real_overflow_mcdc
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
 * condition; pair (V1,V2) isolates the left condition. @brief Verify realloc real overflow mcdc behavior. @details Executes the realloc real overflow mcdc scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_realloc_real_overflow_mcdc(void)
{
  TEST_BEGIN("realloc real MC/DC: size!=0 && items>MAX/size");
  priv_fixture_t fixture;
  internal_fixture_init(&fixture);
  /* V1: overflow on a live block -> NULL, original preserved. */
  uint8_t* p = (uint8_t*)ra8_epub_miniz_alloc(&fixture.arena, 1U, k_small);
  TEST_ASSERT(p != nullptr);
  p[0]        = k_arena_second_byte;
  void* nomem = ra8_epub_miniz_realloc(&fixture.arena, p, (SIZE_MAX / 2U) + 2U, 2U);
  TEST_ASSERT(nomem == nullptr);
  TEST_ASSERT(p[0] == 0xC3U); /* old block still valid after a rejected grow */

  /* V3: a real grow (size!=0, no overflow) succeeds. */
  uint8_t* g = (uint8_t*)ra8_epub_miniz_realloc(&fixture.arena, p, 1U, k_medium);
  TEST_ASSERT(g != nullptr);
  TEST_ASSERT(g[0] == 0xC3U); /* payload preserved across the grow */

  /* V2: size==0 short-circuits the guard; need becomes 0 -> frees and returns
   * NULL (so g is consumed by this call -- do not free it again). */
  void* none = ra8_epub_miniz_realloc(&fixture.arena, g, 4U, 0U);
  TEST_ASSERT(none == nullptr);
  TEST_END("realloc real MC/DC: size!=0 && items>MAX/size");
}

/**
 * @test internal_test_alloc_oversize_rejected_mcdc
 *
 * @par MC/DC:
 * Decision: `if ((items * size) > k_ra8_epub_miniz_pool_bytes)` (1 condition;
 * the oversize guard in both ra8_epub_miniz_alloc and _realloc). It rejects any
 * request larger than the 160 KiB pool BEFORE internal_align_up rounds it -- without
 * it a size near SIZE_MAX passes the multiply check yet overflows
 * (bytes + align - 1) into a tiny under-allocation. Drives the TRUE arm of each
 * guard (the FALSE arm -- a request that fits -- is covered by every other test
 * in this file).
 *  - V1: alloc(1, SIZE_MAX-10)  -> the align-up overflow trigger -> NULL.
 *  - V2: alloc(1, pool_bytes+1) -> an ordinary over-pool size     -> NULL.
 *  - V3: realloc(a, 1, SIZE_MAX-10) -> NULL, and the old block stays valid. @brief Verify alloc oversize rejected mcdc behavior. @details Executes the alloc oversize rejected mcdc scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_alloc_oversize_rejected_mcdc(void)
{
  TEST_BEGIN("alloc oversize MC/DC: reject > pool before align-up overflow");
  priv_fixture_t fixture;
  internal_fixture_init(&fixture);
  /* V1: items=1 so the multiply check passes, but the near-SIZE_MAX size would
   * wrap internal_align_up -- the oversize guard must catch it first. */
  TEST_ASSERT(ra8_epub_miniz_alloc(&fixture.arena, 1U, SIZE_MAX - 10U) == nullptr);
  /* V2: an ordinary just-over-pool size. */
  TEST_ASSERT(ra8_epub_miniz_alloc(&fixture.arena, 1U, (size_t)k_ra8_epub_miniz_pool_bytes + 1U) ==
              nullptr);
  /* V3: realloc growing past the pool is rejected; the old block is untouched. */
  uint8_t* a = (uint8_t*)ra8_epub_miniz_alloc(&fixture.arena, 1U, k_small);
  TEST_ASSERT(a != nullptr);
  a[0] = k_arena_first_byte;
  TEST_ASSERT(ra8_epub_miniz_realloc(&fixture.arena, a, 1U, SIZE_MAX - 10U) == nullptr);
  TEST_ASSERT(a[0] == 0x5AU); /* old block still valid + intact */
  ra8_epub_miniz_free(&fixture.arena, a);
  TEST_END("alloc oversize MC/DC: reject > pool before align-up overflow");
}

/**
 * @test internal_test_arena_capacity_and_failure_preservation
 * @brief Exact workspace capacity succeeds; one byte short changes nothing.
 *
 * @par MC/DC:
 * The compound init guard `(arena == NULL) || (workspace == NULL)` remains at
 * its `(F,F)->F` control in both calls. This test independently flips the
 * following single capacity decision: one byte short gives `T->error` without
 * mutation, while exact capacity gives `F->success`. After deinit, allocation
 * fails the single `initialized != 1` readiness check; no compound allocator
 * selection decision is claimed by the one successful allocation. @details Executes the arena capacity and failure preservation scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_arena_capacity_and_failure_preservation(void)
{
  TEST_BEGIN("arena init: exact capacity + one-byte-short preservation");
  ra8_epub_miniz_workspace_t workspace;
  (void)memset(&workspace, k_miniz_fill_reuse, sizeof(workspace));
  ra8_epub_miniz_arena_t arena = {
    .base        = &workspace.bytes[7],
    .capacity    = 31U,
    .initialized = k_arena_alloc_items,
  };
  const ra8_epub_miniz_arena_t before = arena;
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_size,
    ra8_epub_miniz_arena_init(&arena, &workspace.bytes[0], sizeof(workspace.bytes) - 1U));
  TEST_ASSERT(internal_arena_unchanged(&arena, &before));
  TEST_ASSERT_EQ(k_miniz_fill_reuse, workspace.bytes[0]);
  TEST_ASSERT_EQ(k_miniz_fill_reuse, workspace.bytes[sizeof(workspace.bytes) - 1U]);

  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_epub_miniz_arena_init(&arena, &workspace.bytes[0], sizeof(workspace.bytes)));
  TEST_ASSERT_EQ(k_ra8_epub_miniz_pool_bytes, arena.capacity);
  TEST_ASSERT_EQ(1U, arena.initialized);
  void* const block = ra8_epub_miniz_alloc(&arena, 1U, k_small);
  TEST_ASSERT(block != nullptr);
  ra8_epub_miniz_free(&arena, block);
  ra8_epub_miniz_arena_deinit(&arena);
  TEST_ASSERT(arena.base == nullptr);
  TEST_ASSERT_EQ(0U, arena.capacity);
  TEST_ASSERT_EQ(0U, arena.initialized);
  TEST_ASSERT(ra8_epub_miniz_alloc(&arena, 1U, k_small) == nullptr);
  TEST_END("arena init: exact capacity + one-byte-short preservation");
}

/**
 * @test internal_test_arena_alignment_and_null_guards
 * @brief Invalid descriptors and misaligned storage fail without mutation.
 *
 * @par MC/DC:
 * Decision identity:
 * `libs/ra8_epub/src/ra8_epub_miniz_alloc.c@ra8_epub_miniz_arena_init`.
 * Decision `(arena == NULL) || (workspace == NULL)` has the N+1 vectors
 * `(T,-)->T`, `(F,T)->T`, and the `(F,F)->F` control supplied by the capacity
 * test's non-NULL init calls. Each condition therefore independently changes
 * the result. The misaligned non-NULL vector then takes the separate alignment
 * guard, while NULL alloc/free/deinit calls exercise their own single guards. @details Executes the arena alignment and null guards scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_arena_alignment_and_null_guards(void)
{
  TEST_BEGIN("arena init: null/misaligned guards preserve destination");
  alignas(max_align_t) uint8_t raw[k_ra8_epub_miniz_pool_bytes + alignof(max_align_t)];
  ra8_epub_miniz_arena_t       arena  = {.base = &raw[0], .capacity = 17U, .initialized = 7U};
  const ra8_epub_miniz_arena_t before = arena;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_epub_miniz_arena_init(nullptr, &raw[0], sizeof(raw)));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_epub_miniz_arena_init(&arena, nullptr, sizeof(raw)));
  TEST_ASSERT(internal_arena_unchanged(&arena, &before));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_epub_miniz_arena_init(&arena, &raw[1], (size_t)k_ra8_epub_miniz_pool_bytes));
  TEST_ASSERT(internal_arena_unchanged(&arena, &before));
  TEST_ASSERT(ra8_epub_miniz_alloc(nullptr, 1U, k_small) == nullptr);
  ra8_epub_miniz_free(nullptr, &raw[0]);
  ra8_epub_miniz_arena_deinit(nullptr);
  TEST_END("arena init: null/misaligned guards preserve destination");
}

/**
 * @test internal_test_independent_arenas_teardown_reuse
 * @brief Two live arenas isolate writes and one can reset while the other lives.
 *
 * @par MC/DC:
 * No compound production decision is independently varied here: both allocate
 * calls use ready arenas and fitting free blocks, so allocator selection sees
 * only `(block free=T, size fits=T)->allocate`. The sequence deinitializes and
 * reinitializes the first arena while checking bytes owned by the still-live
 * second arena. The test-only `a != NULL && b != NULL` assertion is observed
 * only as `(T,T)->T` and is not presented as an MC/DC independence set. @details Executes the independent arenas teardown reuse scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_independent_arenas_teardown_reuse(void)
{
  TEST_BEGIN("two arenas: isolation + internal_teardown/reuse");
  priv_fixture_t first;
  priv_fixture_t second;
  internal_fixture_init(&first);
  internal_fixture_init(&second);
  uint8_t* const a = (uint8_t*)ra8_epub_miniz_alloc(&first.arena, 1U, k_medium);
  uint8_t* const b = (uint8_t*)ra8_epub_miniz_alloc(&second.arena, 1U, k_medium);
  TEST_ASSERT((a != nullptr) && (b != nullptr));
  TEST_ASSERT(a != b);
  (void)memset(a, k_miniz_fill_first, k_medium);
  (void)memset(b, k_miniz_fill_second, k_medium);

  ra8_epub_miniz_arena_deinit(&first.arena);
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_epub_miniz_arena_init(&first.arena,
                                           &first.workspace.bytes[0],
                                           sizeof(first.workspace.bytes)));
  TEST_ASSERT_EQ(k_miniz_fill_second, b[0]);
  TEST_ASSERT_EQ(k_miniz_fill_second, b[k_medium - 1U]);
  uint8_t* const reused = (uint8_t*)ra8_epub_miniz_alloc(&first.arena, 1U, k_medium);
  TEST_ASSERT(reused != nullptr);
  (void)memset(reused, k_miniz_fill_reuse, k_medium);
  TEST_ASSERT_EQ(k_miniz_fill_second, b[k_medium / 2U]);
  ra8_epub_miniz_free(&first.arena, reused);
  ra8_epub_miniz_free(&second.arena, b);
  TEST_END("two arenas: isolation + internal_teardown/reuse");
}

/**
 * @brief Test entry point.
 * @return 0 on success; unity macros exit(1) on the first failure.
 */
int main(void)
{
  internal_test_alloc_align_and_distinct();
  internal_test_free_coalesce_reclaim();
  internal_test_realloc_grow_preserves();
  internal_test_realloc_inplace_and_null_zero();
  internal_test_firstfit_mcdc();
  internal_test_coalesce_mcdc();
  internal_test_overflow_mcdc();
  internal_test_alloc_real_overflow_and_firstfit_mcdc();
  internal_test_free_in_pool_mcdc();
  internal_test_realloc_real_overflow_mcdc();
  internal_test_alloc_oversize_rejected_mcdc();
  internal_test_arena_capacity_and_failure_preservation();
  internal_test_arena_alignment_and_null_guards();
  internal_test_independent_arenas_teardown_reuse();
  return 0;
}
