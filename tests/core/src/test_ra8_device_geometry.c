/**
 * @file test_ra8_device_geometry.c
 * @brief Unit tests for the RA8 cache/TCM/memory geometry in
 *        libs/ra8_core/inc/ra8_device.h
 *
 * @details
 * Issue #850 was a reconciliation bug, not a code bug: one place was storing
 * two different facts, "how big is this bank" and "how much of it do we
 * declare", and the two drifted. `ra8_device_mem_capacity_t` now holds the
 * first and `ra8_device_mem_size_t` the second. This file is what stops them
 * merging again.
 *
 * Three jobs, in order of what they protect:
 *
 * 1. PIN THE CAPACITIES to the values sourced in `ra8_device.h`, so a future
 *    edit that "corrects" a bank size has to change a test that names its
 *    citation rather than quietly changing a constant.
 * 2. PIN THE ACCOUNTING, so no region is ever counted twice. User SRAM plus
 *    both cores' TCM budgets must equal the 2048 KiB island exactly, and the
 *    per-bank splits must multiply out to the datasheet's per-core totals.
 * 3. PIN THE FLOOR. Supported allocation must never exceed capacity, and the
 *    M85 TCM entries must stay at 64 KiB until the startup copy/zero/ECC audit
 *    and a silicon run raise them deliberately (issues #226 / #229). A table
 *    correction alone must not be able to expand a linker region.
 *
 * Host-only and hardware-free: `ra8_device.h` is compile-time constants, so
 * every assertion here is about what the build believes, which is exactly the
 * thing that drifted. This proves nothing about silicon.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8_device.h"
#include "unity_minimal.h"

/** @brief Byte counts the geometry assertions are expressed in. */
typedef enum : uint32_t {
  k_geom_kib              = 1024U,     /**< One kibibyte.                      */
  k_geom_16_kib           = 0x4000U,   /**< 16 KiB: every cache bank.          */
  k_geom_64_kib           = 0x10000U,  /**< 64 KiB: M33 TCM bank, M85 floor.   */
  k_geom_128_kib          = 0x20000U,  /**< 128 KiB: M85 TCM bank, M33 total.  */
  k_geom_256_kib          = 0x40000U,  /**< 256 KiB: M85 TCM total.            */
  k_geom_32_kib           = 0x8000U,   /**< 32 KiB: per-core cache total.      */
  k_geom_1664_kib         = 0x1A0000U, /**< 1664 KiB user SRAM.                */
  k_geom_1024_kib         = 0x100000U, /**< 1024 KiB code MRAM.                */
  k_geom_2048_kib         = 0x200000U, /**< 2048 KiB SRAM island.              */
  k_geom_1792_kib         = 0x1C0000U, /**< 1792 KiB: single-core SKU user SRAM. */
  k_geom_tcm_block_bytes  = 0x2000U,   /**< 8 KiB ECC block granule of M85 TCM. */
  k_geom_tcm_block_count  = 16U,       /**< 16 blocks per M85 TCM bank.        */
} ra8_geom_const_t;

/**
 * @brief Every capacity constant equals its sourced value.
 *
 * @details
 * Each value below is cited in `ra8_device_mem_capacity_t`: the MRAM and user
 * SRAM rows to RA8P1 datasheet R01DS0439EJ0130 Table 1.15 p 11, the per-bank
 * cache and TCM rows to HUM 2.1.1 pp 111-112. Changing one of these numbers
 * should require changing this test, which is the point.
 *
 * @pre None.
 * @post No state; compile-time constants only.
 *
 * @par MC/DC:
 * (no compound decisions; each assertion is a single equality)
 *
 * @since 0.1.0
 */
static void test_capacities_match_sources(void)
{
  TEST_BEGIN("capacity constants match their cited sources");

  TEST_ASSERT_EQ((uint32_t)k_geom_1024_kib, (uint32_t)k_ra8_cap_mram_bytes);
  TEST_ASSERT_EQ((uint32_t)k_geom_1664_kib, (uint32_t)k_ra8_cap_user_sram_bytes);

  TEST_ASSERT_EQ((uint32_t)k_geom_128_kib, (uint32_t)k_ra8_cap_m85_itcm_bytes);
  TEST_ASSERT_EQ((uint32_t)k_geom_128_kib, (uint32_t)k_ra8_cap_m85_dtcm_bytes);
  TEST_ASSERT_EQ((uint32_t)k_geom_16_kib, (uint32_t)k_ra8_cap_m85_icache_bytes);
  TEST_ASSERT_EQ((uint32_t)k_geom_16_kib, (uint32_t)k_ra8_cap_m85_dcache_bytes);

  TEST_ASSERT_EQ((uint32_t)k_geom_64_kib, (uint32_t)k_ra8_cap_m33_ctcm_bytes);
  TEST_ASSERT_EQ((uint32_t)k_geom_64_kib, (uint32_t)k_ra8_cap_m33_stcm_bytes);
  TEST_ASSERT_EQ((uint32_t)k_geom_16_kib, (uint32_t)k_ra8_cap_m33_ccache_bytes);
  TEST_ASSERT_EQ((uint32_t)k_geom_16_kib, (uint32_t)k_ra8_cap_m33_scache_bytes);

  TEST_ASSERT_EQ((uint32_t)k_geom_2048_kib, (uint32_t)k_ra8_cap_sram_island_bytes);

  TEST_END("capacity constants match their cited sources");
}

/**
 * @brief Per-bank splits multiply out to the datasheet's per-core totals.
 *
 * @details
 * The datasheet gives totals only (`CPU0 TCM 256 KB`, `CPU0 I/D Caches 32 KB`,
 * `CPU1 TCM 128 KB`, `CPU1 C/S Caches 32 KB`, RA8P1 Table 1.15 p 11); the HUM
 * gives the split. This is the cross-check that made the HUM split trustworthy
 * in the first place, so it is worth keeping executable: if someone edits one
 * bank without the other, the product stops matching the datasheet row.
 *
 * @pre None.
 * @post No state.
 *
 * @par MC/DC:
 * (no compound decisions; each assertion is a single equality)
 *
 * @since 0.1.0
 */
static void test_splits_reproduce_datasheet_totals(void)
{
  TEST_BEGIN("per-bank splits reproduce the datasheet per-core totals");

  const uint32_t m85_tcm_total =
      (uint32_t)k_ra8_cap_m85_itcm_bytes + (uint32_t)k_ra8_cap_m85_dtcm_bytes;
  const uint32_t m85_cache_total =
      (uint32_t)k_ra8_cap_m85_icache_bytes + (uint32_t)k_ra8_cap_m85_dcache_bytes;
  const uint32_t m33_tcm_total =
      (uint32_t)k_ra8_cap_m33_ctcm_bytes + (uint32_t)k_ra8_cap_m33_stcm_bytes;
  const uint32_t m33_cache_total =
      (uint32_t)k_ra8_cap_m33_ccache_bytes + (uint32_t)k_ra8_cap_m33_scache_bytes;

  TEST_ASSERT_EQ((uint32_t)k_geom_256_kib, m85_tcm_total);
  TEST_ASSERT_EQ((uint32_t)k_geom_32_kib, m85_cache_total);
  TEST_ASSERT_EQ((uint32_t)k_geom_128_kib, m33_tcm_total);
  TEST_ASSERT_EQ((uint32_t)k_geom_32_kib, m33_cache_total);

  TEST_END("per-bank splits reproduce the datasheet per-core totals");
}

/**
 * @brief No physical region is counted twice.
 *
 * @details
 * User SRAM plus both TCM budgets must equal the 2048 KiB island EXACTLY, which
 * is what makes "1664 KB user SRAM" and "2 MB total RAM" both true without
 * being summable. The single-core corroboration (1792 KiB user SRAM + 256 KiB
 * M85 TCM + no M33 TCM) lands on the same island size, which is the reason the
 * carve-out reading is right and the add-on-top reading is wrong.
 *
 * @pre None.
 * @post No state.
 *
 * @par MC/DC:
 * (no compound decisions; each assertion is a single equality)
 *
 * @since 0.1.0
 */
static void test_sram_island_counts_each_region_once(void)
{
  TEST_BEGIN("SRAM island accounting counts each region exactly once");

  const uint32_t m85_tcm =
      (uint32_t)k_ra8_cap_m85_itcm_bytes + (uint32_t)k_ra8_cap_m85_dtcm_bytes;
  const uint32_t m33_tcm =
      (uint32_t)k_ra8_cap_m33_ctcm_bytes + (uint32_t)k_ra8_cap_m33_stcm_bytes;

  TEST_ASSERT_EQ((uint32_t)k_ra8_cap_sram_island_bytes,
                 (uint32_t)k_ra8_cap_user_sram_bytes + m85_tcm + m33_tcm);

  /* Single-core SKU corroboration: same island, no M33 TCM, more user SRAM. */
  TEST_ASSERT_EQ((uint32_t)k_ra8_cap_sram_island_bytes,
                 (uint32_t)k_geom_1792_kib + m85_tcm);

  /* User SRAM is strictly smaller than the island: TCM is carved out of it. */
  TEST_ASSERT((uint32_t)k_ra8_cap_user_sram_bytes < (uint32_t)k_ra8_cap_sram_island_bytes);

  TEST_END("SRAM island accounting counts each region exactly once");
}

/**
 * @brief Supported allocation never exceeds silicon capacity.
 *
 * @details
 * The one invariant that must hold for every bank in every direction. MRAM and
 * user SRAM allocate their full capacity; the TCM entries allocate less. None
 * may allocate more.
 *
 * @pre None.
 * @post No state.
 *
 * @par MC/DC:
 * Decision: ``declared <= capacity`` per bank.
 * - V1: MRAM, declared == capacity -> boundary, true.
 * - V2: ITCM, declared <  capacity -> strict, true.
 * Together they exercise both sides of the <= without a false case, which is
 * deliberate: a false case here is the bug this test exists to fail on.
 *
 * @since 0.1.0
 */
static void test_declared_never_exceeds_capacity(void)
{
  TEST_BEGIN("declared sizes never exceed silicon capacity");

  TEST_ASSERT((uint32_t)k_ra8_mem_mram_size <= (uint32_t)k_ra8_cap_mram_bytes);
  TEST_ASSERT((uint32_t)k_ra8_mem_sram_size <= (uint32_t)k_ra8_cap_user_sram_bytes);
  TEST_ASSERT((uint32_t)k_ra8_mem_itcm_size <= (uint32_t)k_ra8_cap_m85_itcm_bytes);
  TEST_ASSERT((uint32_t)k_ra8_mem_dtcm_size <= (uint32_t)k_ra8_cap_m85_dtcm_bytes);

  /* MRAM and user SRAM are declared at full capacity. */
  TEST_ASSERT_EQ((uint32_t)k_ra8_cap_mram_bytes, (uint32_t)k_ra8_mem_mram_size);
  TEST_ASSERT_EQ((uint32_t)k_ra8_cap_user_sram_bytes, (uint32_t)k_ra8_mem_sram_size);

  TEST_END("declared sizes never exceed silicon capacity");
}

/**
 * @brief The 64 KiB M85 TCM floor is still in force.
 *
 * @details
 * This is the assertion that makes issue #850 safe to land. Correcting the
 * capacity table must NOT expand what the linker scripts declare: the ITCM and
 * DTCM banks are ECC and 16-block-granular, so growing the declared region
 * changes what `Reset_Handler` must copy, zero and ECC-initialize, changes the
 * window `ra8_emulator` maps (`k_dtcm_end == 0x20010000`), and invalidates the
 * link of every HIL-validated image. Raising this floor is a deliberate change
 * behind that audit and a silicon run (#226 / #229), and it should fail here
 * first so whoever raises it has to come and read this comment.
 *
 * @pre None.
 * @post No state.
 *
 * @par MC/DC:
 * (no compound decisions; each assertion is a single equality)
 *
 * @since 0.1.0
 */
static void test_m85_tcm_floor_is_unchanged(void)
{
  TEST_BEGIN("M85 TCM declaration is still the 64 KiB floor");

  TEST_ASSERT_EQ((uint32_t)k_geom_64_kib, (uint32_t)k_ra8_mem_itcm_size);
  TEST_ASSERT_EQ((uint32_t)k_geom_64_kib, (uint32_t)k_ra8_mem_dtcm_size);

  /* The floor is half of capacity, so exactly half of each bank is unused. */
  TEST_ASSERT_EQ((uint32_t)k_ra8_cap_m85_itcm_bytes, (uint32_t)k_ra8_mem_itcm_size * 2U);
  TEST_ASSERT_EQ((uint32_t)k_ra8_cap_m85_dtcm_bytes, (uint32_t)k_ra8_mem_dtcm_size * 2U);

  TEST_END("M85 TCM declaration is still the 64 KiB floor");
}

/**
 * @brief M85 TCM banks are a whole number of 8 KiB ECC blocks.
 *
 * @details
 * HUM 2.1.1 p 111 describes each M85 TCM bank as 16 blocks x 8 KiB. That
 * granule is why a partial expansion is not free, so it is worth asserting
 * rather than leaving in prose: both the capacity and the declared floor must
 * land on a block boundary.
 *
 * @pre None.
 * @post No state.
 *
 * @par MC/DC:
 * (no compound decisions; each assertion is a single equality)
 *
 * @since 0.1.0
 */
static void test_m85_tcm_is_block_aligned(void)
{
  TEST_BEGIN("M85 TCM capacity and floor are whole 8 KiB ECC blocks");

  TEST_ASSERT_EQ((uint32_t)k_ra8_cap_m85_itcm_bytes,
                 (uint32_t)k_geom_tcm_block_bytes * (uint32_t)k_geom_tcm_block_count);
  TEST_ASSERT_EQ(0U, (uint32_t)k_ra8_cap_m85_dtcm_bytes % (uint32_t)k_geom_tcm_block_bytes);
  TEST_ASSERT_EQ(0U, (uint32_t)k_ra8_mem_itcm_size % (uint32_t)k_geom_tcm_block_bytes);
  TEST_ASSERT_EQ(0U, (uint32_t)k_ra8_mem_dtcm_size % (uint32_t)k_geom_tcm_block_bytes);

  TEST_END("M85 TCM capacity and floor are whole 8 KiB ECC blocks");
}

/**
 * @brief Every geometry value is a whole number of KiB.
 *
 * @details
 * The documented invariant on both enums. Cheap to assert and it catches a
 * fat-fingered hex literal immediately.
 *
 * @pre None.
 * @post No state.
 *
 * @par MC/DC:
 * (no compound decisions; each assertion is a single modulo equality)
 *
 * @since 0.1.0
 */
static void test_every_value_is_whole_kib(void)
{
  TEST_BEGIN("every geometry value is a whole number of KiB");

  const uint32_t values[] = {
      (uint32_t)k_ra8_cap_mram_bytes,        (uint32_t)k_ra8_cap_user_sram_bytes,
      (uint32_t)k_ra8_cap_m85_itcm_bytes,    (uint32_t)k_ra8_cap_m85_dtcm_bytes,
      (uint32_t)k_ra8_cap_m85_icache_bytes,  (uint32_t)k_ra8_cap_m85_dcache_bytes,
      (uint32_t)k_ra8_cap_m33_ctcm_bytes,    (uint32_t)k_ra8_cap_m33_stcm_bytes,
      (uint32_t)k_ra8_cap_m33_ccache_bytes,  (uint32_t)k_ra8_cap_m33_scache_bytes,
      (uint32_t)k_ra8_cap_sram_island_bytes, (uint32_t)k_ra8_mem_mram_size,
      (uint32_t)k_ra8_mem_sram_size,         (uint32_t)k_ra8_mem_itcm_size,
      (uint32_t)k_ra8_mem_dtcm_size,
  };

  for (uint32_t i = 0U; i < (uint32_t)(sizeof(values) / sizeof(values[0])); i++) {
    TEST_ASSERT_EQ(0U, values[i] % (uint32_t)k_geom_kib);
    TEST_ASSERT(values[i] > 0U);
  }

  TEST_END("every geometry value is a whole number of KiB");
}

/**
 * @brief The TCM and SRAM base addresses are unchanged by this reconciliation.
 *
 * @details
 * Issue #850 corrects a capacity table and nothing else. If a base address
 * moved, a region moved, which is exactly what this change promised not to do.
 *
 * @pre None.
 * @post No state.
 *
 * @par MC/DC:
 * (no compound decisions; each assertion is a single equality)
 *
 * @since 0.1.0
 */
static void test_bases_unchanged(void)
{
  TEST_BEGIN("memory bases are unchanged");

  TEST_ASSERT_EQ((uintptr_t)0x00000000U, (uintptr_t)k_ra8_mem_itcm_base);
  TEST_ASSERT_EQ((uintptr_t)0x20000000U, (uintptr_t)k_ra8_mem_dtcm_base);
  TEST_ASSERT_EQ((uintptr_t)0x22000000U, (uintptr_t)k_ra8_mem_sram_base);
  TEST_ASSERT_EQ((uintptr_t)0x02000000U, (uintptr_t)k_ra8_mem_mram_base);

  /* The emulator maps DTCM base..base+64 KiB; the floor must still match it. */
  TEST_ASSERT_EQ((uintptr_t)0x20010000U,
                 (uintptr_t)k_ra8_mem_dtcm_base + (uintptr_t)k_ra8_mem_dtcm_size);

  TEST_END("memory bases are unchanged");
}

int main(void)
{
  test_capacities_match_sources();
  test_splits_reproduce_datasheet_totals();
  test_sram_island_counts_each_region_once();
  test_declared_never_exceeds_capacity();
  test_m85_tcm_floor_is_unchanged();
  test_m85_tcm_is_block_aligned();
  test_every_value_is_whole_kib();
  test_bases_unchanged();
  return 0;
}
