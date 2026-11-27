/**
 * @file test_ra8p1_ereader_budget.c
 * @brief Automated unit tests for RA8P1 e-reader concurrent workload memory budget (RA8FW-259)
 *
 * @details
 * Validates the memory placement plan and concurrent workload budget defined in
 * libs/ra8_board_ra8p1/inc/ra8_board_ra8p1_workloads.h against:
 * 1. Hardware memory tier capacities and supported floors (TCM, SRAM, SDRAM, OSPI).
 * 2. Exact on-chip SRAM partitioning across SRAM0, NOINIT, and SRAM1.
 * 3. External SDRAM partitioning across display, image/manga, book, and ML domains.
 * 4. External OSPI NOR Flash model weight and font asset budgets.
 * 5. Concurrent workload peak bandwidth margin (>= 25% headroom per ADR-0021).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8_board_memmap.h"
#include "ra8_board_ra8p1.h"
#include "ra8_board_ra8p1_workloads.h"
#include "ra8_device.h"
#include "ra8_err.h"
#include "unity_minimal.h"

static void test_tcm_allocations_and_floors(void)
{
  TEST_BEGIN("TCM allocations stay within 64 KiB supported floors");

  /* Silicon capacities */
  TEST_ASSERT_EQ((uint32_t)k_ra8p1_mem_m85_itcm_cap_bytes, (uint32_t)k_ra8p1_board_cap_m85_itcm_bytes);
  TEST_ASSERT_EQ((uint32_t)k_ra8p1_mem_m85_dtcm_cap_bytes, (uint32_t)k_ra8p1_board_cap_m85_dtcm_bytes);

  /* Supported linker floors */
  TEST_ASSERT_EQ((uint32_t)k_ra8p1_mem_m85_itcm_floor_bytes, (uint32_t)k_ra8_board_itcm_size);
  TEST_ASSERT_EQ((uint32_t)k_ra8p1_mem_m85_dtcm_floor_bytes, (uint32_t)k_ra8_board_dtcm_size);

  /* Allocations strictly <= floor */
  TEST_ASSERT((uint32_t)k_ra8p1_itcm_allocated_bytes <= (uint32_t)k_ra8p1_mem_m85_itcm_floor_bytes);
  TEST_ASSERT((uint32_t)k_ra8p1_dtcm_allocated_bytes <= (uint32_t)k_ra8p1_mem_m85_dtcm_floor_bytes);

  /* Exact headroom checks (32 KiB allocated, 32 KiB free) */
  TEST_ASSERT_EQ((uint32_t)0x8000U, (uint32_t)k_ra8p1_itcm_allocated_bytes);
  TEST_ASSERT_EQ((uint32_t)0x8000U, (uint32_t)k_ra8p1_itcm_headroom_bytes);
  TEST_ASSERT_EQ((uint32_t)0x8000U, (uint32_t)k_ra8p1_dtcm_allocated_bytes);
  TEST_ASSERT_EQ((uint32_t)0x8000U, (uint32_t)k_ra8p1_dtcm_headroom_bytes);

  TEST_END("TCM allocations stay within 64 KiB supported floors");
}

static void test_sram_partitions_and_accounting(void)
{
  TEST_BEGIN("On-chip SRAM partitions match hardware limits exactly");

  /* Island accounting: User SRAM + M85 TCMs (256 KiB) + M33 TCMs (128 KiB) = 2048 KiB */
  const uint32_t m85_tcm_total = (uint32_t)k_ra8p1_board_cap_m85_itcm_bytes +
                                 (uint32_t)k_ra8p1_board_cap_m85_dtcm_bytes;
  const uint32_t m33_tcm_total = (uint32_t)k_ra8p1_board_cap_m33_ctcm_bytes +
                                 (uint32_t)k_ra8p1_board_cap_m33_stcm_bytes;
  TEST_ASSERT_EQ((uint32_t)k_ra8p1_mem_sram_island_bytes,
                 (uint32_t)k_ra8p1_mem_user_sram_cap_bytes + m85_tcm_total + m33_tcm_total);

  /* SRAM0 + NOINIT + SRAM1 = 1664 KiB user SRAM */
  const uint32_t sram0_full = (uint32_t)k_ra8p1_mem_sram0_limit_bytes +
                              (uint32_t)k_ra8p1_mem_noinit_bytes;
  TEST_ASSERT_EQ((uint32_t)0x100000U, sram0_full); /* 1 MiB */
  TEST_ASSERT_EQ((uint32_t)k_ra8p1_mem_user_sram_cap_bytes,
                 sram0_full + (uint32_t)k_ra8p1_mem_sram1_limit_bytes);

  /* SRAM0 working set fit */
  const uint32_t sram0_used = ra8p1_workload_get_sram0_bytes();
  TEST_ASSERT(sram0_used <= (uint32_t)k_ra8p1_mem_sram0_limit_bytes);
  TEST_ASSERT_EQ(sram0_used + (uint32_t)k_ra8p1_sram0_headroom_bytes,
                 (uint32_t)k_ra8p1_mem_sram0_limit_bytes);

  /* SRAM1 aperture fit: 64 KiB M33 + 64 KiB NS + 16 KiB IPC + 384 KiB NPU + 112 KiB DMA = 640 KiB */
  const uint32_t sram1_used = ra8p1_workload_get_sram1_bytes();
  TEST_ASSERT_EQ((uint32_t)k_ra8p1_mem_sram1_limit_bytes, sram1_used);

  /* Total user SRAM working set fit */
  const uint32_t total_sram = ra8p1_workload_get_total_user_sram_bytes();
  TEST_ASSERT(total_sram <= (uint32_t)k_ra8p1_mem_user_sram_cap_bytes);

  TEST_END("On-chip SRAM partitions match hardware limits exactly");
}

static void test_sdram_working_memory_budget(void)
{
  TEST_BEGIN("External SDRAM working memory satisfies 64 MiB baseline budget");

  /* Sum of all four subsystems: Display (8M) + Image (8M) + Book (16M) + ML (32M) = 64M */
  TEST_ASSERT_EQ((uint32_t)0x00800000U, (uint32_t)k_ra8p1_sdram_display_total_bytes);
  TEST_ASSERT_EQ((uint32_t)0x00800000U, (uint32_t)k_ra8p1_sdram_image_total_bytes);
  TEST_ASSERT_EQ((uint32_t)0x01000000U, (uint32_t)k_ra8p1_sdram_book_total_bytes);
  TEST_ASSERT_EQ((uint32_t)0x02000000U, (uint32_t)k_ra8p1_sdram_ml_total_bytes);

  const uint32_t sdram_total = ra8p1_workload_get_sdram_bytes();
  TEST_ASSERT_EQ((uint32_t)k_ra8p1_mem_sdram_ek_base_bytes, sdram_total);
  TEST_ASSERT_EQ((uint32_t)0x04000000U, sdram_total); /* 64 MiB */

  /* Verify controller maximum headroom (128 MiB ceiling) */
  TEST_ASSERT_EQ((uint32_t)k_ra8p1_mem_sdram_max_cap_bytes,
                 sdram_total + (uint32_t)k_ra8p1_sdram_ceiling_expansion_b);

  TEST_END("External SDRAM working memory satisfies 64 MiB baseline budget");
}

static void test_ospi_flash_asset_budget(void)
{
  TEST_BEGIN("External OSPI NOR Flash assets fit within 128 MiB target package");

  /* Check CS0 asset total: weights (55M) + NPU (7M) + CJK font (12M) + Latin font (1.25M) + UI (2M) */
  const uint32_t ospi_total = ra8p1_workload_get_ospi_cs0_bytes();
  TEST_ASSERT_EQ((uint32_t)k_ra8p1_ospi_cs0_allocated_bytes, ospi_total);

  /* Proves 78.31 MiB fits comfortably inside standard 128 MiB Octal NOR with >= 35% headroom */
  TEST_ASSERT(ospi_total < (uint32_t)0x08000000U); /* < 128 MiB */
  TEST_ASSERT_EQ((uint32_t)0x08000000U, ospi_total + (uint32_t)k_ra8p1_ospi_cs0_128m_headroom_b);

  /* Proves fits inside hardware controller 256 MiB window */
  TEST_ASSERT(ospi_total <= (uint32_t)k_ra8p1_mem_ospi_cs_max_bytes);

  TEST_END("External OSPI NOR Flash assets fit within 128 MiB target package");
}

static void test_concurrency_and_bandwidth_headroom(void)
{
  TEST_BEGIN("Concurrent peak workload maintains >= 25% memory bandwidth headroom");

  /* Check full budget validation function */
  const ra8_err_t err = ra8p1_workload_validate_budget();
  TEST_ASSERT_EQ(k_ra8_ok, err);

  /* Bandwidth headroom: 180 MB/s peak on 532 MB/s raw bus -> 66.1% headroom (well above 25%) */
  const uint32_t raw_bw = (uint32_t)k_ra8p1_sdram_raw_bw_mbps;
  const uint32_t peak_bw = (uint32_t)k_ra8p1_bw_concurrent_peak_mbps;
  const uint32_t bus_utilization_pct = peak_bw * 100U / raw_bw;
  TEST_ASSERT(bus_utilization_pct <= 75U); /* Headroom >= 25% */
  TEST_ASSERT(bus_utilization_pct >= 30U); /* Realistic load */

  TEST_END("Concurrent peak workload maintains >= 25% memory bandwidth headroom");
}

int main(void)
{
  test_tcm_allocations_and_floors();
  test_sram_partitions_and_accounting();
  test_sdram_working_memory_budget();
  test_ospi_flash_asset_budget();
  test_concurrency_and_bandwidth_headroom();
  return 0;
}
