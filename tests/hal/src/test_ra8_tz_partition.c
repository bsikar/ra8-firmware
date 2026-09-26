/**
 *
 * @file test_ra8_tz_partition.c
 * @brief Unit tests for the declarative TrustZone partition (ra8_tz_partition.c)
 * @details Exercises descriptor validation, apply atomicity, the SRAM boundary half, and the canonical board map in hosted registers.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_sau.h"
#include "ra8_sau_regs.h"
#include "ra8_sram_regs.h"
#include "ra8_tz_partition.h"
#include "unity_minimal.h"

/**
 * @enum test_tz_partition_layout_t
 * @brief Addresses and sizes the suite drives the mock partition with.
 */
typedef enum : uint32_t {
  k_test_tz_ns_base    = 0x22100000UL, /**< 32-byte-aligned NS window base. */
  k_test_tz_ns_size    = 0x00001000UL, /**< 4 KiB NS window.                */
  k_test_tz_ns_limit   = 0x22100FE0UL, /**< base + size - 32.               */
  k_test_tz_nsc_base   = 0x10000000UL, /**< NSC veneer alias base.          */
  k_test_tz_nsc_size   = 0x00000040UL, /**< Two granules.                   */
  k_test_tz_sram_split = 0x00040000UL, /**< 4 KB-aligned Secure length.     */
  k_test_tz_bad_split  = 0x00040001UL, /**< Not 4 KB aligned.               */
} test_tz_partition_layout_t;

/**
 * @enum test_tz_partition_index_t
 * @brief Region counts the suite publishes through SAU_TYPE.
 */
typedef enum : uint8_t {
  k_test_tz_sregion_count = 8U, /**< Pretend we are an M85. */
  k_test_tz_sram_banks    = 4U, /**< SRAMSABAR0..3.         */
} test_tz_partition_index_t;

/* Publish SAU_TYPE.SREGION -- see header for full description. */
RA8_INTERNAL static void internal_set_sregion(uint8_t n)
{
  ra8_sau_regs()->TYPE = (uint32_t)n;
}

/* see header for full description.
 *
 * @brief Reset the fake register blocks this suite touches.
 *
 * @details
 * `ra8_fake_mmap_reset()` zeros ordinary backing regions but intentionally
 * excludes the architectural System Control Space window, so clear the SAU
 * block by hand, then publish SREGION = 8.
 */
RA8_INTERNAL static void internal_setup(void)
{
  ra8_fake_mmap_reset();
  *ra8_sau_regs() = (r_sau_regs_t){0};
  internal_set_sregion((uint8_t)k_test_tz_sregion_count);
}

/* A single NS window -- see header for full description. */
RA8_INTERNAL static ra8_sau_region_t internal_ns_region(void)
{
  return (ra8_sau_region_t){
      .base = (uintptr_t)k_test_tz_ns_base,
      .size = (uint32_t)k_test_tz_ns_size,
      .attr = k_ra8_sau_attr_ns,
  };
}

/**
 * @brief A NULL descriptor is refused, and so is a NULL region table.
 */
RA8_INTERNAL static void internal_test_rejects_null(void)
{
  TEST_BEGIN("a NULL descriptor and a NULL region table are refused");
  internal_setup();

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_tz_partition_validate(NULL));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_tz_partition_apply(NULL));

  const ra8_tz_partition_t headless = {
      .sau_regions      = NULL,
      .sram_boundary    = NULL,
      .sau_region_count = 1U,
      .sau_all_ns       = false,
  };
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_tz_partition_validate(&headless));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_tz_partition_apply(&headless));
  TEST_END("a NULL descriptor and a NULL region table are refused");
}

/**
 * @brief A region count above SAU_TYPE.SREGION is not supported.
 */
RA8_INTERNAL static void internal_test_rejects_oversized_table(void)
{
  TEST_BEGIN("a region count above SAU_TYPE.SREGION is not supported");
  internal_setup();
  internal_set_sregion(2U);

  const ra8_sau_region_t  regions[1] = {internal_ns_region()};
  const ra8_tz_partition_t partition = {
      .sau_regions      = regions,
      .sram_boundary    = NULL,
      .sau_region_count = 3U,
      .sau_all_ns       = false,
  };
  TEST_ASSERT_EQ(k_ra8_err_not_supported, ra8_tz_partition_validate(&partition));
  TEST_END("a region count above SAU_TYPE.SREGION is not supported");
}

/**
 * @brief Misaligned, empty and wrapping windows are all invalid arguments.
 */
RA8_INTERNAL static void internal_test_rejects_bad_geometry(void)
{
  TEST_BEGIN("misaligned, empty and wrapping windows are invalid");
  internal_setup();

  ra8_sau_region_t region = internal_ns_region();
  region.base             = (uintptr_t)k_test_tz_ns_base + 1U;
  ra8_tz_partition_t partition = {
      .sau_regions      = &region,
      .sram_boundary    = NULL,
      .sau_region_count = 1U,
      .sau_all_ns       = false,
  };
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_tz_partition_validate(&partition));

  region      = internal_ns_region();
  region.size = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_tz_partition_validate(&partition));

  region      = internal_ns_region();
  region.size = (uint32_t)k_test_tz_ns_size + 1U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_tz_partition_validate(&partition));

  region      = internal_ns_region();
  region.base = (uintptr_t)0xFFFFF000UL;
  region.size = 0x00002000UL;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_tz_partition_validate(&partition));
  TEST_END("misaligned, empty and wrapping windows are invalid");
}

/**
 * @brief A refused descriptor leaves the SAU exactly as it was.
 */
RA8_INTERNAL static void internal_test_apply_is_atomic_on_refusal(void)
{
  TEST_BEGIN("a refused descriptor leaves the SAU untouched");
  internal_setup();

  ra8_sau_region_t region = internal_ns_region();
  region.size             = (uint32_t)k_test_tz_ns_size + 1U;
  const ra8_tz_partition_t partition = {
      .sau_regions      = &region,
      .sram_boundary    = NULL,
      .sau_region_count = 1U,
      .sau_all_ns       = false,
  };

  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_tz_partition_apply(&partition));
  TEST_ASSERT_EQ(0U, ra8_sau_regs()->CTRL);
  TEST_ASSERT_EQ(0U, ra8_sau_regs()->RBAR);
  TEST_ASSERT_EQ(0U, ra8_sau_regs()->RLAR);
  TEST_END("a refused descriptor leaves the SAU untouched");
}

/**
 * @brief Applying a one-window partition enables the SAU and encodes the limit.
 */
RA8_INTERNAL static void internal_test_apply_programs_sau(void)
{
  TEST_BEGIN("apply programmes and enables the SAU");
  internal_setup();

  const ra8_sau_region_t   regions[1] = {internal_ns_region()};
  const ra8_tz_partition_t partition  = {
       .sau_regions      = regions,
       .sram_boundary    = NULL,
       .sau_region_count = 1U,
       .sau_all_ns       = false,
  };

  TEST_ASSERT_EQ(k_ra8_ok, ra8_tz_partition_apply(&partition));
  TEST_ASSERT(ra8_sau_is_enabled());
  TEST_END("apply programmes and enables the SAU");
}

/**
 * @brief An NSC window is accepted and an unknown attribute is not.
 */
RA8_INTERNAL static void internal_test_attr_domain(void)
{
  TEST_BEGIN("NSC is accepted and an unknown attribute is not");
  internal_setup();

  ra8_sau_region_t region = {
      .base = (uintptr_t)k_test_tz_nsc_base,
      .size = (uint32_t)k_test_tz_nsc_size,
      .attr = k_ra8_sau_attr_nsc,
  };
  const ra8_tz_partition_t partition = {
      .sau_regions      = &region,
      .sram_boundary    = NULL,
      .sau_region_count = 1U,
      .sau_all_ns       = false,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tz_partition_validate(&partition));

  region.attr = (ra8_sau_attr_t)7U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_tz_partition_validate(&partition));
  TEST_END("NSC is accepted and an unknown attribute is not");
}

/**
 * @brief A misaligned SRAM boundary is refused before the SAU is touched.
 */
RA8_INTERNAL static void internal_test_rejects_unaligned_sram_boundary(void)
{
  TEST_BEGIN("a misaligned SRAM boundary is refused before any write");
  internal_setup();

  const ra8_sau_region_t regions[1] = {internal_ns_region()};
  const uint32_t         bad[k_test_tz_sram_banks] = {
      (uint32_t)k_test_tz_sram_split,
      (uint32_t)k_test_tz_bad_split,
      0U,
      0U,
  };
  const ra8_tz_partition_t partition = {
      .sau_regions      = regions,
      .sram_boundary    = bad,
      .sau_region_count = 1U,
      .sau_all_ns       = false,
  };

  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_tz_partition_apply(&partition));
  TEST_ASSERT_EQ(0U, ra8_sau_regs()->CTRL);
  TEST_END("a misaligned SRAM boundary is refused before any write");
}

/**
 * @brief A well-formed boundary array reaches all four SRAMSABARn registers.
 */
RA8_INTERNAL static void internal_test_writes_sram_boundaries(void)
{
  TEST_BEGIN("a boundary array reaches all four SRAMSABARn registers");
  internal_setup();

  const ra8_sau_region_t regions[1] = {internal_ns_region()};
  const uint32_t         split[k_test_tz_sram_banks] = {
      (uint32_t)k_test_tz_sram_split,
      (uint32_t)k_test_tz_sram_split,
      0U,
      (uint32_t)k_test_tz_sram_split,
  };
  const ra8_tz_partition_t partition = {
      .sau_regions      = regions,
      .sram_boundary    = split,
      .sau_region_count = 1U,
      .sau_all_ns       = false,
  };

  TEST_ASSERT_EQ(k_ra8_ok, ra8_tz_partition_apply(&partition));
  volatile r_sram_cpscu_regs_t* cpscu = ra8_sram_cpscu_regs();
  TEST_ASSERT_EQ((uint32_t)k_test_tz_sram_split, cpscu->SRAMSABAR[0]);
  TEST_ASSERT_EQ((uint32_t)k_test_tz_sram_split, cpscu->SRAMSABAR[1]);
  TEST_ASSERT_EQ(0U, cpscu->SRAMSABAR[2]);
  TEST_ASSERT_EQ((uint32_t)k_test_tz_sram_split, cpscu->SRAMSABAR[3]);
  TEST_END("a boundary array reaches all four SRAMSABARn registers");
}

/**
 * @brief The canonical board map is applicable and keeps lower memory Secure.
 */
RA8_INTERNAL static void internal_test_board_map(void)
{
  TEST_BEGIN("the canonical board map is applicable and default-deny");
  internal_setup();

  const ra8_tz_partition_t* map = ra8_tz_partition_board_map();
  TEST_ASSERT_NOT_NULL(map);
  TEST_ASSERT_EQ((uint8_t)k_ra8_sau_boot_region_count, map->sau_region_count);
  TEST_ASSERT_NULL(map->sram_boundary);
  TEST_ASSERT(!map->sau_all_ns);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tz_partition_validate(map));

  /* Lower MRAM and lower SRAM are absent on purpose: that absence is what
   * keeps the secure image and the key vault Secure. */
  for (uint8_t i = 0U; i < map->sau_region_count; i++) {
    TEST_ASSERT(map->sau_regions[i].base != 0x02000000UL);
    TEST_ASSERT(map->sau_regions[i].base != 0x22000000UL);
  }

  TEST_ASSERT_EQ(k_ra8_ok, ra8_tz_partition_apply(map));
  TEST_ASSERT(ra8_sau_is_enabled());
  TEST_END("the canonical board map is applicable and default-deny");
}

int main(void)
{
  internal_test_rejects_null();
  internal_test_rejects_oversized_table();
  internal_test_rejects_bad_geometry();
  internal_test_apply_is_atomic_on_refusal();
  internal_test_apply_programs_sau();
  internal_test_attr_domain();
  internal_test_rejects_unaligned_sram_boundary();
  internal_test_writes_sram_boundaries();
  internal_test_board_map();
  return 0;
}
