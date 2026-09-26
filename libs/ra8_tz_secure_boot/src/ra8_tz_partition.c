/**
 * @file ra8_tz_partition.c
 * @brief Declarative TrustZone partition: validation and apply
 *
 * @par Tag
 * [Ring 1 / Boot] {World: S}
 *
 * @details
 * Implements the `ra8_tz_partition.h` contract. The SAU half delegates to
 * `ra8_sau_configure()`; the RA8D2 SRAM boundary half writes SRAMSABARn
 * through `ra8_sram_set_boundary()`. Everything this TU adds is the
 * whole-descriptor check that runs before either of them, so a partition the
 * device would reject halfway is rejected before any register moves.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_tz_partition.h"

#include <stddef.h>
#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_sau.h"
#include "ra8_sram.h"

/* =============================================================================
 * The canonical EK-RA8D2 map
 * =============================================================================
 */

/**
 * @enum ra8_tz_partition_board_geometry_t
 * @brief Base and size of each window in the validated board partition.
 *
 * @details
 * Written as base plus size rather than base plus limit: the limit register
 * wants `base + size - 32`, and deriving it in one place is exactly what
 * `ra8_sau.h` exists for. These are the same four windows the per-app
 * `trustzone_init.c` copies hardcode.
 */
typedef enum : uint32_t {
  k_ra8_tz_board_ns_mram_base  = 0x02080000U, /**< Non-Secure upper MRAM base.  */
  k_ra8_tz_board_ns_mram_size  = 0x00080000U, /**< Non-Secure upper MRAM size.  */
  k_ra8_tz_board_ns_sram_base  = 0x22100000U, /**< Non-Secure upper SRAM base.  */
  k_ra8_tz_board_ns_sram_size  = 0x00100000U, /**< Non-Secure upper SRAM size.  */
  k_ra8_tz_board_ns_sdram_base = 0x6A000000U, /**< Non-Secure upper SDRAM base. */
  k_ra8_tz_board_ns_sdram_size = 0x02000000U, /**< Non-Secure upper SDRAM size. */
  k_ra8_tz_board_nsc_base      = 0x10000000U, /**< NSC veneer alias base.       */
  k_ra8_tz_board_nsc_size      = 0x00100000U, /**< NSC veneer alias size.       */
} ra8_tz_partition_board_geometry_t;

/** @brief The four windows of the validated EK-RA8D2 partition. */
static const ra8_sau_region_t s_board_regions[k_ra8_sau_boot_region_count] = {
    {.base = (uintptr_t)k_ra8_tz_board_ns_mram_base,
     .size = (uint32_t)k_ra8_tz_board_ns_mram_size,
     .attr = k_ra8_sau_attr_ns},
    {.base = (uintptr_t)k_ra8_tz_board_ns_sram_base,
     .size = (uint32_t)k_ra8_tz_board_ns_sram_size,
     .attr = k_ra8_sau_attr_ns},
    {.base = (uintptr_t)k_ra8_tz_board_ns_sdram_base,
     .size = (uint32_t)k_ra8_tz_board_ns_sdram_size,
     .attr = k_ra8_sau_attr_ns},
    {.base = (uintptr_t)k_ra8_tz_board_nsc_base,
     .size = (uint32_t)k_ra8_tz_board_nsc_size,
     .attr = k_ra8_sau_attr_nsc},
};

/** @brief The validated EK-RA8D2 partition descriptor. */
static const ra8_tz_partition_t s_board_partition = {
    .sau_regions      = s_board_regions,
    .sram_boundary    = NULL,
    .sau_region_count = (uint8_t)k_ra8_sau_boot_region_count,
    .sau_all_ns       = false,
};

/* =============================================================================
 * Internal helpers
 * =============================================================================
 */

/**
 * @brief Check one SAU region's geometry.
 *
 * @details Applies the Armv8-M rules `ra8_sau_configure()` applies, without
 *   writing anything: 32-byte alignment of base and size, a non-zero size, a
 *   range that does not wrap past 4 GiB, and a known attribute.
 * @param[in] region Region descriptor to check.
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok               Region is representable.
 * @retval k_ra8_err_invalid_arg  Any rule above is broken.
 * @pre @p region is non-NULL.
 * @post No device state is changed.
 * @note Not thread-safe; boot-path helper.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_check_region(const ra8_sau_region_t* region)
{
  const uint32_t granule_mask = (uint32_t)k_ra8_sau_region_granule - 1U;

  if (((uint32_t)region->base & granule_mask) != 0U) {
    return k_ra8_err_invalid_arg;
  }
  if ((region->size == 0U) || ((region->size & granule_mask) != 0U)) {
    return k_ra8_err_invalid_arg;
  }
  if ((region->attr != k_ra8_sau_attr_ns) && (region->attr != k_ra8_sau_attr_nsc)) {
    return k_ra8_err_invalid_arg;
  }

  /* The inclusive limit is base + size - 32; a window whose end runs past the
   * top of the address space is not representable at all, so refuse it here
   * rather than write a wrapped RLAR. */
  const uint64_t end = (uint64_t)region->base + (uint64_t)region->size;
  if (end > (uint64_t)UINT32_MAX + 1U) {
    return k_ra8_err_invalid_arg;
  }
  return k_ra8_ok;
}

/**
 * @brief Check the SRAM boundary half of a descriptor.
 *
 * @details A NULL array means the partition does not speak for SRAM
 *   attribution, which is legal. A non-NULL array must be 4 KB aligned in
 *   every bank, because SRAMSABARn ignores the low bits rather than
 *   rejecting them.
 * @param[in] partition Descriptor whose `sram_boundary` is checked.
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok               Boundaries are writable as given.
 * @retval k_ra8_err_invalid_arg  A boundary offset is misaligned.
 * @pre @p partition is non-NULL.
 * @post No device state is changed.
 * @note Not thread-safe; boot-path helper.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_check_sram(const ra8_tz_partition_t* partition)
{
  if (partition->sram_boundary == NULL) {
    return k_ra8_ok;
  }
  const uint32_t granule_mask = (uint32_t)k_ra8_tz_partition_sram_granule - 1U;
  for (uint8_t bank = 0U; bank < (uint8_t)k_ra8_tz_partition_sram_bank_count; bank++) {
    if ((partition->sram_boundary[bank] & granule_mask) != 0U) {
      return k_ra8_err_invalid_arg;
    }
  }
  return k_ra8_ok;
}

/* =============================================================================
 * Public entry points
 * =============================================================================
 */

ra8_err_t ra8_tz_partition_validate(const ra8_tz_partition_t* partition)
{
  if (partition == NULL) {
    return k_ra8_err_null_ptr;
  }
  if ((partition->sau_region_count != 0U) && (partition->sau_regions == NULL)) {
    return k_ra8_err_null_ptr;
  }
  if (partition->sau_region_count > ra8_sau_region_count()) {
    return k_ra8_err_not_supported;
  }

  for (uint8_t i = 0U; i < partition->sau_region_count; i++) {
    const ra8_err_t err = internal_check_region(&partition->sau_regions[i]);
    if (err != k_ra8_ok) {
      return err;
    }
  }
  return internal_check_sram(partition);
}

ra8_err_t ra8_tz_partition_apply(const ra8_tz_partition_t* partition)
{
  const ra8_err_t check = ra8_tz_partition_validate(partition);
  if (check != k_ra8_ok) {
    return check;
  }

  const ra8_sau_cfg_t cfg = {
      .regions      = partition->sau_regions,
      .region_count = partition->sau_region_count,
      .all_ns       = partition->sau_all_ns,
  };
  const ra8_err_t sau_err = ra8_sau_configure(&cfg);
  if (sau_err != k_ra8_ok) {
    return sau_err;
  }

  if (partition->sram_boundary == NULL) {
    return k_ra8_ok;
  }
  for (uint8_t bank = 0U; bank < (uint8_t)k_ra8_tz_partition_sram_bank_count; bank++) {
    const ra8_err_t sram_err = ra8_sram_set_boundary(bank, partition->sram_boundary[bank]);
    if (sram_err != k_ra8_ok) {
      return sram_err;
    }
  }
  return k_ra8_ok;
}

const ra8_tz_partition_t* ra8_tz_partition_board_map(void)
{
  return &s_board_partition;
}
