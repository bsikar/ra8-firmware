/**
 * @file test_ra8_cache.c
 * @brief Unit tests for ra8_cache.c (Cortex-M85 L1 D-cache maintenance).
 *
 * @details
 * The SCB cache window (0xE000Exxx) is backed by the fake MMIO map, so the
 * tests seed CTR / CCSIDR and read back the maintenance registers (DCCMVAC /
 * DCIMVAC / DCCIMVAC / DCISW) to verify the line-size decode, the by-address
 * line span, the validation guards, and the set/way invalidate-all.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8_cache.h"
#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "unity_minimal.h"

/**
 * @enum ra8_cache_test_reg_t
 * @brief SCB cache-register addresses the tests seed and read back.
 */
typedef enum : uintptr_t {
  k_t_ctr      = 0xE000ED7CUL, /**< Cache Type Register.             */
  k_t_ccsidr   = 0xE000ED80UL, /**< Cache Size ID Register.          */
  k_t_csselr   = 0xE000ED84UL, /**< Cache Size Selection.            */
  k_t_dcimvac  = 0xE000EF5CUL, /**< D-cache invalidate by MVA.       */
  k_t_dcisw    = 0xE000EF60UL, /**< D-cache invalidate by set/way.   */
  k_t_dccmvac  = 0xE000EF68UL, /**< D-cache clean by MVA.            */
  k_t_dccimvac = 0xE000EF70UL, /**< D-cache clean+invalidate by MVA. */
} ra8_cache_test_reg_t;

/**
 * @enum ra8_cache_test_const_t
 * @brief Field shifts and fixtures used by the tests.
 */
typedef enum : uint32_t {
  k_t_ctr_dmin_shift  = 16U,         /**< CTR.DminLine position.               */
  k_t_dmin_for_32     = 3U,          /**< DminLine=3 -> 4<<3 = 32-byte line.   */
  k_t_dmin_for_16     = 2U,          /**< DminLine=2 -> 4<<2 = 16-byte line.   */
  k_t_line_32         = 32U,         /**< Expected 32-byte line.               */
  k_t_line_16         = 16U,         /**< Expected 16-byte line.               */
  k_t_addr_a          = 0x00002000U, /**< Aligned synthetic buffer address.    */
  k_t_addr_unaligned  = 0x00002010U, /**< Unaligned synthetic buffer address.  */
  k_t_sentinel        = 0xDEADBEEFU, /**< Pre-set marker to detect "no write". */
  k_t_ccsidr_sets_sh  = 13U,         /**< CCSIDR.NumSets position.             */
  k_t_ccsidr_assoc_sh = 3U,          /**< CCSIDR.Associativity position.       */
} ra8_cache_test_const_t;

/** @brief Typed access to a seeded/observed SCB register in the fake map. */
static volatile uint32_t* reg(ra8_cache_test_reg_t addr)
{
  return (volatile uint32_t*)addr;
}

/** @brief Seed CTR so ra8_cache reports a @p dmin-derived line size. */
static void seed_line(uint32_t dmin)
{
  *reg(k_t_ctr) = dmin << (uint32_t)k_t_ctr_dmin_shift;
}

/**
 * @par MC/DC: not applicable -- pure field decode, no compound decision.
 */
static void test_line_bytes_decodes_ctr(void)
{
  TEST_BEGIN("ra8_cache: line size decodes CTR.DminLine");
  ra8_fake_mmap_reset();
  seed_line((uint32_t)k_t_dmin_for_32);
  TEST_ASSERT_EQ(k_t_line_32, ra8_cache_dcache_line_bytes());
  seed_line((uint32_t)k_t_dmin_for_16);
  TEST_ASSERT_EQ(k_t_line_16, ra8_cache_dcache_line_bytes());
  TEST_END("ra8_cache: line size decodes CTR.DminLine");
}

/**
 * @par MC/DC: not applicable -- success path; the only decision is the
 *      `size == 0` early-out, covered by test_zero_size_is_noop.
 */
static void test_clean_aligned_single_line(void)
{
  TEST_BEGIN("ra8_cache: clean of one aligned line hits that line");
  ra8_fake_mmap_reset();
  seed_line((uint32_t)k_t_dmin_for_32);
  *reg(k_t_dccmvac) = (uint32_t)k_t_sentinel;
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_cache_dcache_clean_by_addr((const void*)(uintptr_t)k_t_addr_a, (uint32_t)k_t_line_32));
  /* One line: last (only) DCCMVAC write is the aligned start itself. */
  TEST_ASSERT_EQ(k_t_addr_a, *reg(k_t_dccmvac));
  TEST_END("ra8_cache: clean of one aligned line hits that line");
}

/**
 * @par MC/DC: not applicable -- exercises the span arithmetic, no compound
 *      decision in the path under test.
 */
static void test_clean_spans_into_next_line(void)
{
  TEST_BEGIN("ra8_cache: clean spanning a line boundary walks both lines");
  ra8_fake_mmap_reset();
  seed_line((uint32_t)k_t_dmin_for_32);
  /* One byte past the first 32-byte line -> two lines; last write = addr+32. */
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_cache_dcache_clean_by_addr((const void*)(uintptr_t)k_t_addr_a, (uint32_t)k_t_line_32 + 1U));
  TEST_ASSERT_EQ(k_t_addr_a + (uint32_t)k_t_line_32, *reg(k_t_dccmvac));
  TEST_END("ra8_cache: clean spanning a line boundary walks both lines");
}

/**
 * @par MC/DC: not applicable -- unaligned start rounds down; no decision.
 */
static void test_unaligned_start_rounds_down(void)
{
  TEST_BEGIN("ra8_cache: invalidate of an unaligned range covers the head line");
  ra8_fake_mmap_reset();
  seed_line((uint32_t)k_t_dmin_for_32);
  /* 0x2010 + 32 bytes -> lines 0x2000 and 0x2020; last DCIMVAC write = 0x2020. */
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_cache_dcache_invalidate_by_addr((void*)(uintptr_t)k_t_addr_unaligned,
                                                     (uint32_t)k_t_line_32));
  TEST_ASSERT_EQ(k_t_addr_a + (uint32_t)k_t_line_32, *reg(k_t_dcimvac));
  TEST_END("ra8_cache: invalidate of an unaligned range covers the head line");
}

/**
 * @par MC/DC: not applicable -- routes to the clean+invalidate register only.
 */
static void test_clean_invalidate_uses_dccimvac(void)
{
  TEST_BEGIN("ra8_cache: clean+invalidate writes DCCIMVAC");
  ra8_fake_mmap_reset();
  seed_line((uint32_t)k_t_dmin_for_32);
  *reg(k_t_dccimvac) = (uint32_t)k_t_sentinel;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_cache_dcache_clean_invalidate_by_addr((const void*)(uintptr_t)k_t_addr_a,
                                                           (uint32_t)k_t_line_32));
  TEST_ASSERT_EQ(k_t_addr_a, *reg(k_t_dccimvac));
  TEST_END("ra8_cache: clean+invalidate writes DCCIMVAC");
}

/**
 * @par MC/DC: not applicable -- the NULL guard is a single RA8_CHECK_NULL_PTR
 *      condition; this case proves the rejection branch.
 */
static void test_null_addr_rejected(void)
{
  TEST_BEGIN("ra8_cache: NULL address is rejected");
  ra8_fake_mmap_reset();
  seed_line((uint32_t)k_t_dmin_for_32);
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_cache_dcache_clean_by_addr(nullptr, (uint32_t)k_t_line_32));
  TEST_END("ra8_cache: NULL address is rejected");
}

/**
 * @par MC/DC: not applicable -- proves the `size == 0` early-out leaves the
 *      maintenance register untouched.
 */
static void test_zero_size_is_noop(void)
{
  TEST_BEGIN("ra8_cache: zero size is a no-op success");
  ra8_fake_mmap_reset();
  seed_line((uint32_t)k_t_dmin_for_32);
  *reg(k_t_dccmvac) = (uint32_t)k_t_sentinel;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_cache_dcache_clean_by_addr((const void*)(uintptr_t)k_t_addr_a, 0U));
  TEST_ASSERT_EQ(k_t_sentinel, *reg(k_t_dccmvac));
  TEST_END("ra8_cache: zero size is a no-op success");
}

/** @brief Compose a CCSIDR with one set and one way (small, fast to walk). */
static uint32_t ccsidr_one_set_one_way(void)
{
  return (1U << (uint32_t)k_t_ccsidr_sets_sh) | (1U << (uint32_t)k_t_ccsidr_assoc_sh);
}

/**
 * @test test_invalidate_all_geometry_guard
 *
 * @par MC/DC:
 * Decision: `if ((ccsidr == 0) || (ccsidr == UINT32_MAX))` (2 conditions)
 * - Vector 1: ccsidr=valid (1 set/1 way) -> false: loop runs, DCISW is written.
 *   (control -- both conditions false)
 * - Vector 2: ccsidr=0           -> true: early return, DCISW untouched.
 *   (varies condition 1 only)
 * - Vector 3: ccsidr=UINT32_MAX  -> true: early return, DCISW untouched.
 *   (varies condition 2 only)
 * Vectors 1+2 prove `ccsidr == 0` independently affects the outcome; 1+3 prove
 * the same for `ccsidr == UINT32_MAX`. N+1 = 3 vectors for N=2: minimal MC/DC.
 */
static void test_invalidate_all_geometry_guard(void)
{
  TEST_BEGIN("ra8_cache: invalidate_all guards a degenerate CCSIDR (MC/DC)");

  /* Vector 1: valid geometry -> the set/way loop runs (DCISW changes). */
  ra8_fake_mmap_reset();
  *reg(k_t_ccsidr) = ccsidr_one_set_one_way();
  *reg(k_t_dcisw)  = (uint32_t)k_t_sentinel;
  ra8_cache_dcache_invalidate_all();
  TEST_ASSERT(*reg(k_t_dcisw) != (uint32_t)k_t_sentinel);

  /* Vector 2: CCSIDR == 0 -> early return, DCISW untouched. */
  ra8_fake_mmap_reset();
  *reg(k_t_ccsidr) = 0U;
  *reg(k_t_dcisw)  = (uint32_t)k_t_sentinel;
  ra8_cache_dcache_invalidate_all();
  TEST_ASSERT_EQ(k_t_sentinel, *reg(k_t_dcisw));

  /* Vector 3: CCSIDR == all-ones -> early return, DCISW untouched. */
  ra8_fake_mmap_reset();
  *reg(k_t_ccsidr) = UINT32_MAX;
  *reg(k_t_dcisw)  = (uint32_t)k_t_sentinel;
  ra8_cache_dcache_invalidate_all();
  TEST_ASSERT_EQ(k_t_sentinel, *reg(k_t_dcisw));

  TEST_END("ra8_cache: invalidate_all guards a degenerate CCSIDR (MC/DC)");
}

int32_t main(void)
{
  test_line_bytes_decodes_ctr();
  test_clean_aligned_single_line();
  test_clean_spans_into_next_line();
  test_unaligned_start_rounds_down();
  test_clean_invalidate_uses_dccimvac();
  test_null_addr_rejected();
  test_zero_size_is_noop();
  test_invalidate_all_geometry_guard();
  return 0;
}
