/**
 * @file ra8_sau.c
 * @brief Armv8-M Security Attribution Unit partition helper
 *
 * @details
 * Implementation of the public API in `ra8_sau.h`. Programs the SAU register
 * block documented in `ra8_sau_regs.h` from a declarative region table, adding
 * the alignment and granularity validation, the `base + size - 32` limit
 * arithmetic, and the ENABLE / NSC encoding that sixteen in-tree
 * `trustzone_init.c` copies each open-code today (issue #735).
 *
 * It also owns the canonical four-region boot partition
 * (`ra8_sau_apply_boot_map()`), the single source of truth the secure reset
 * path routes through, exactly as the MPU side routes through
 * `ra8_mpu_apply_boot_map()`.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_sau.h"

#include <stddef.h>
#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_hw_intrinsics.h"
#include "ra8_sau_regs.h"

/**
 * @enum ra8_sau_boot_partition_t
 * @brief Base / size of each region in the canonical boot partition.
 *
 * @details
 * The same window set both `libs/ra8_board_ek_ra8d2` and `libs/ra8_board_ra8p1`
 * program out of reset, expressed as base + size rather than the pre-decremented
 * limits those files carry, so the driver does the `- 32` once.
 */
typedef enum : uint32_t {
  k_ra8_sau_boot_ns_mram_base  = 0x02080000UL, /**< NS upper MRAM base.      */
  k_ra8_sau_boot_ns_mram_size  = 0x00080000UL, /**< NS upper MRAM, 512 KiB.  */
  k_ra8_sau_boot_ns_sram_base  = 0x22100000UL, /**< NS upper SRAM base.      */
  k_ra8_sau_boot_ns_sram_size  = 0x00100000UL, /**< NS upper SRAM, 1 MiB.    */
  k_ra8_sau_boot_ns_sdram_base = 0x6A000000UL, /**< NS upper SDRAM base.     */
  k_ra8_sau_boot_ns_sdram_size = 0x02000000UL, /**< NS upper SDRAM, 32 MiB.  */
  k_ra8_sau_boot_nsc_base      = 0x10000000UL, /**< NSC veneer alias base.   */
  k_ra8_sau_boot_nsc_size      = 0x00100000UL, /**< NSC veneer alias, 1 MiB. */
} ra8_sau_boot_partition_t;

static const ra8_sau_region_t s_boot_regions[k_ra8_sau_boot_region_count] = {
  {.base = (uintptr_t)k_ra8_sau_boot_ns_mram_base,
   .size = (uint32_t)k_ra8_sau_boot_ns_mram_size,
   .attr = k_ra8_sau_attr_ns},
  {.base = (uintptr_t)k_ra8_sau_boot_ns_sram_base,
   .size = (uint32_t)k_ra8_sau_boot_ns_sram_size,
   .attr = k_ra8_sau_attr_ns},
  {.base = (uintptr_t)k_ra8_sau_boot_ns_sdram_base,
   .size = (uint32_t)k_ra8_sau_boot_ns_sdram_size,
   .attr = k_ra8_sau_attr_ns},
  {.base = (uintptr_t)k_ra8_sau_boot_nsc_base,
   .size = (uint32_t)k_ra8_sau_boot_nsc_size,
   .attr = k_ra8_sau_attr_nsc},
};

static const ra8_sau_cfg_t s_boot_cfg = {
  .regions      = s_boot_regions,
  .region_count = (uint8_t)k_ra8_sau_boot_region_count,
  .all_ns       = false,
};

/* Report SAU_TYPE.SREGION -- see implementation for details. */
RA8_INTERNAL static inline uint8_t internal_sregion_count(void)
{
  return (uint8_t)(ra8_sau_regs()->TYPE & (uint32_t)k_ra8_sau_type_sregion_mask);
}

/* Validate one region descriptor against the architectural granule -- see implementation for details. */
RA8_INTERNAL static bool internal_region_valid(const ra8_sau_region_t* region)
{
  const uint32_t granule_mask = (uint32_t)k_ra8_sau_region_granule - 1U;
  if ((region->attr != k_ra8_sau_attr_ns) && (region->attr != k_ra8_sau_attr_nsc)) {
    return false;
  }
  if (region->size < (uint32_t)k_ra8_sau_region_granule) {
    return false;
  }
  if ((region->size & granule_mask) != 0U) {
    return false;
  }
  if (((uint32_t)region->base & granule_mask) != 0U) {
    return false;
  }
  /* The inclusive limit must stay inside the 32-bit address space: a window
   * that wraps would program a limit below its own base. */
  const uint32_t base = (uint32_t)region->base;
  if (region->size > (UINT32_MAX - base + 1U)) {
    return false;
  }
  return true;
}

/* Validate a whole partition descriptor before any register is written -- see implementation for details. */
RA8_INTERNAL static ra8_err_t internal_cfg_valid(const ra8_sau_cfg_t* cfg)
{
  if (cfg == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if ((cfg->regions == nullptr) && (cfg->region_count != 0U)) {
    return k_ra8_err_null_ptr;
  }
  if (cfg->region_count > internal_sregion_count()) {
    return k_ra8_err_invalid_arg;
  }
  for (uint8_t i = 0U; i < cfg->region_count; ++i) {
    if (!internal_region_valid(&cfg->regions[i])) {
      return k_ra8_err_invalid_arg;
    }
  }
  return k_ra8_ok;
}

/* Write one already-validated region through RNR/RBAR/RLAR -- see implementation for details. */
RA8_INTERNAL static void internal_write_region(uint8_t index, const ra8_sau_region_t* region)
{
  const uint32_t base  = (uint32_t)region->base;
  const uint32_t limit = base + region->size - (uint32_t)k_ra8_sau_region_granule;

  uint32_t rlar = (limit & (uint32_t)k_ra8_sau_rlar_limit_mask) | (uint32_t)k_ra8_sau_rlar_enable;
  if (region->attr == k_ra8_sau_attr_nsc) {
    rlar |= (uint32_t)k_ra8_sau_rlar_nsc;
  }

  ra8_sau_regs()->RNR  = (uint32_t)index;
  ra8_sau_regs()->RBAR = base & (uint32_t)k_ra8_sau_rbar_base_mask;
  ra8_sau_regs()->RLAR = rlar;
}

/* Clear one region so a stale partition cannot survive a reconfigure -- see implementation for details. */
RA8_INTERNAL static void internal_clear_region(uint8_t index)
{
  ra8_sau_regs()->RNR  = (uint32_t)index;
  ra8_sau_regs()->RLAR = 0U;
  ra8_sau_regs()->RBAR = 0U;
}

/* Program a validated descriptor and enable the unit -- see implementation for details. */
RA8_INTERNAL static void internal_install(const ra8_sau_cfg_t* cfg)
{
  const uint8_t implemented = internal_sregion_count();

  ra8_sau_regs()->CTRL = 0U;

  for (uint8_t i = 0U; i < cfg->region_count; ++i) {
    internal_write_region(i, &cfg->regions[i]);
  }
  for (uint8_t i = cfg->region_count; i < implemented; ++i) {
    internal_clear_region(i);
  }

  uint32_t ctrl = (uint32_t)k_ra8_sau_ctrl_enable;
  if (cfg->all_ns) {
    ctrl |= (uint32_t)k_ra8_sau_ctrl_allns;
  }

  ra8_hw_dsb();
  ra8_sau_regs()->CTRL = ctrl;
  ra8_hw_dsb();
  ra8_hw_isb();
}

ra8_err_t ra8_sau_configure(const ra8_sau_cfg_t* cfg)
{
  const ra8_err_t err = internal_cfg_valid(cfg);
  if (err != k_ra8_ok) {
    return err;
  }
  internal_install(cfg);
  return k_ra8_ok;
}

ra8_err_t ra8_sau_set_region(uint8_t region, const ra8_sau_region_t* region_cfg)
{
  if (region_cfg == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (region >= internal_sregion_count()) {
    return k_ra8_err_invalid_arg;
  }
  if (!internal_region_valid(region_cfg)) {
    return k_ra8_err_invalid_arg;
  }
  internal_write_region(region, region_cfg);
  return k_ra8_ok;
}

ra8_err_t ra8_sau_enable(void)
{
  ra8_hw_dsb();
  ra8_sau_regs()->CTRL |= (uint32_t)k_ra8_sau_ctrl_enable;
  ra8_hw_dsb();
  ra8_hw_isb();
  return k_ra8_ok;
}

ra8_err_t ra8_sau_disable(void)
{
  ra8_hw_dsb();
  ra8_sau_regs()->CTRL &= ~(uint32_t)k_ra8_sau_ctrl_enable;
  ra8_hw_dsb();
  ra8_hw_isb();
  return k_ra8_ok;
}

bool ra8_sau_is_enabled(void)
{
  return (ra8_sau_regs()->CTRL & (uint32_t)k_ra8_sau_ctrl_enable) != 0U;
}

uint8_t ra8_sau_region_count(void)
{
  return internal_sregion_count();
}

const ra8_sau_cfg_t* ra8_sau_boot_map(void)
{
  return &s_boot_cfg;
}

ra8_err_t ra8_sau_apply_boot_map(void)
{
  if (internal_sregion_count() < (uint8_t)k_ra8_sau_boot_region_count) {
    return k_ra8_err_invalid_arg;
  }
  internal_install(&s_boot_cfg);
  return k_ra8_ok;
}
