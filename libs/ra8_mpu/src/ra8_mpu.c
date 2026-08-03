/**
 * @file ra8_mpu.c
 * @brief Cortex-M85 Memory Protection Unit configuration helper
 *
 * @details
 * Implementation of the public API in `ra8_mpu.h`. Programmes the
 * Armv8-M MPU register block documented in `ra8_mpu_regs.h` from a
 * static region table. The register-level shape (RBAR / RLAR / MAIR0
 * / MAIR1) follows the Arm Cortex-M85 TRM "MPU register summary";
 * the helper adds power-of-two validation, AP[1:0] encoding from
 * RO/RW/None pairs, and bounds checking against the implemented
 * region count reported by `MPU_TYPE.DREGION`.
 *
 * It also owns the canonical 5-region boot memory-attribute map
 * (`ra8_mpu_apply_boot_map()`), the single source of truth the reset path
 * routes through instead of hand-rolling MAIR/RBAR/RLAR/CTRL pokes in each
 * app's `system_init.c` (issue #576).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_mpu.h"

#include <stddef.h>
#include <stdint.h>

#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_hw_intrinsics.h"
#include "ra8_mpu_regs.h"

static const char* s_tag = "MPU";

/**
 * @enum ra8_mpu_ap_t
 * @brief Encoded AP[1:0] values per the Cortex-M85 TRM.
 */
typedef enum : uint8_t {
  k_ra8_mpu_ap_priv_rw_unpriv_none = 0U,    /**< 00: priv RW, unpriv -.   */
  k_ra8_mpu_ap_priv_rw_unpriv_rw   = 1U,    /**< 01: priv RW, unpriv RW.  */
  k_ra8_mpu_ap_priv_ro_unpriv_none = 2U,    /**< 10: priv RO, unpriv -.   */
  k_ra8_mpu_ap_priv_ro_unpriv_ro   = 3U,    /**< 11: priv RO, unpriv RO.  */
  k_ra8_mpu_ap_invalid             = 0xFFU, /**< Sentinel: not encodable. */
} ra8_mpu_ap_t;

/**
 * @enum ra8_mpu_shcsr_addr_t
 * @brief SCB->SHCSR address + bits needed to dispatch MemManage faults.
 *
 * @details
 * Armv8-M ARM B3.2.10 "SHCSR, System Handler Control and State
 * Register" / Cortex-M85 TRM SCB+0x24. Bit 16 MEMFAULTENA enables
 * the MemManage exception; without it any MPU permission violation
 * escalates to HardFault and the strong MemManage_Handler the
 * application installed never runs.
 */
typedef enum : uintptr_t {
  k_ra8_mpu_shcsr_addr = 0xE000ED24U, /**< SCB->SHCSR (Armv8-M B3.2.10). */
} ra8_mpu_shcsr_addr_t;

typedef enum : uint32_t {
  k_ra8_mpu_shcsr_memfaultena = (1U << 16U), /**< SHCSR.MEMFAULTENA bit. */
} ra8_mpu_shcsr_bits_t;

/* Test whether `value` is a positive power of two -- see implementation for details. */
static inline bool ra8_mpu_is_pow2(uint32_t value)
{
  return (value != 0U) && ((value & (value - 1U)) == 0U);
}

/* Map a (priv, unpriv) permission pair onto the Armv8-M AP code -- see implementation for details. */
static uint8_t ra8_mpu_encode_ap(ra8_mpu_perm_t priv, ra8_mpu_perm_t unpriv)
{
  /* Arm Cortex-M85 TRM "MPU_RBAR" -- AP[1:0] table. */
  if (priv == k_ra8_mpu_perm_rw && unpriv == k_ra8_mpu_perm_none) {
    return (uint8_t)k_ra8_mpu_ap_priv_rw_unpriv_none;
  }
  if (priv == k_ra8_mpu_perm_rw && unpriv == k_ra8_mpu_perm_rw) {
    return (uint8_t)k_ra8_mpu_ap_priv_rw_unpriv_rw;
  }
  if (priv == k_ra8_mpu_perm_ro && unpriv == k_ra8_mpu_perm_none) {
    return (uint8_t)k_ra8_mpu_ap_priv_ro_unpriv_none;
  }
  if (priv == k_ra8_mpu_perm_ro && unpriv == k_ra8_mpu_perm_ro) {
    return (uint8_t)k_ra8_mpu_ap_priv_ro_unpriv_ro;
  }
  return (uint8_t)k_ra8_mpu_ap_invalid;
}

/* Read the implemented region count from `MPU_TYPE -- see implementation for details. */
static uint8_t ra8_mpu_dregion_count(void)
{
  /* Arm Cortex-M85 TRM "MPU_TYPE": DREGION lives in bits 15:8. */
  const uint32_t type = ra8_mpu_regs()->TYPE;
  return (uint8_t)((type & (uint32_t)k_ra8_mpu_type_dregion_mask) >>
                   (uint32_t)k_ra8_mpu_type_dregion_shift);
}

/* Validate a single region descriptor -- see implementation for details. */
static ra8_err_t ra8_mpu_validate_region(const ra8_mpu_region_t* r)
{
  if (!ra8_mpu_is_pow2(r->size) || r->size < (uint32_t)k_ra8_mpu_min_region_size) {
    return k_ra8_err_invalid_arg;
  }
  if ((r->base & (uintptr_t)(r->size - 1U)) != 0U) {
    return k_ra8_err_invalid_arg;
  }
  if (ra8_mpu_encode_ap(r->priv, r->unpriv) == (uint8_t)k_ra8_mpu_ap_invalid) {
    return k_ra8_err_invalid_arg;
  }
  return k_ra8_ok;
}

/* Build the MPU_RBAR value for a region descriptor -- see implementation for details. */
static uint32_t ra8_mpu_build_rbar(const ra8_mpu_region_t* r)
{
  /* Arm Cortex-M85 TRM "MPU_RBAR": BASE[31:5] | SH[4:3] | AP[2:1] | XN[0]. */
  const uint32_t base = (uint32_t)r->base & (uint32_t)k_ra8_mpu_rbar_base_mask;
  const uint32_t ap =
    ((uint32_t)ra8_mpu_encode_ap(r->priv, r->unpriv) << (uint32_t)k_ra8_mpu_rbar_ap_shift) &
    (uint32_t)k_ra8_mpu_rbar_ap_mask;
  const uint32_t sh = ((uint32_t)r->shareable << (uint32_t)k_ra8_mpu_rbar_sh_shift) &
                      (uint32_t)k_ra8_mpu_rbar_sh_mask;
  const uint32_t xn = r->executable ? 0U : (uint32_t)k_ra8_mpu_rbar_xn_mask;
  return base | sh | ap | xn;
}

/* Build the MPU_RLAR value for a region descriptor -- see implementation for details. */
static uint32_t ra8_mpu_build_rlar(const ra8_mpu_region_t* r)
{
  /* Arm Cortex-M85 TRM "MPU_RLAR": LIMIT[31:5] | AttrIndx[3:1] | EN[0]. */
  const uint32_t limit_inclusive = (uint32_t)(r->base + r->size - 1U);
  const uint32_t limit           = limit_inclusive & (uint32_t)k_ra8_mpu_rlar_limit_mask;
  const uint32_t idx = ((uint32_t)r->attr_idx << (uint32_t)k_ra8_mpu_rlar_attridx_shift) &
                       (uint32_t)k_ra8_mpu_rlar_attridx_mask;
  return limit | idx | (uint32_t)k_ra8_mpu_rlar_en_mask;
}

/* Write a single region's RBAR/RLAR pair via MPU_RNR selection -- see implementation for details. */
static void ra8_mpu_program_region(uint8_t region, const ra8_mpu_region_t* r)
{
  volatile r_mpu_regs_t* mpu = ra8_mpu_regs();
  mpu->RNR                   = (uint32_t)region;
  mpu->RBAR                  = ra8_mpu_build_rbar(r);
  mpu->RLAR                  = ra8_mpu_build_rlar(r);
}

/* Disable a single region by clearing RLAR -- see implementation for details. */
static void ra8_mpu_clear_region(uint8_t region)
{
  volatile r_mpu_regs_t* mpu = ra8_mpu_regs();
  mpu->RNR                   = (uint32_t)region;
  mpu->RLAR                  = 0U;
}

/* Sole writer of MPU_CTRL for wholesale (re)configuration -- see implementation for details. */
static void ra8_mpu_write_ctrl(uint32_t ctrl)
{
  /* Arm Cortex-M85 TRM "MPU_CTRL": ENABLE[0] | HFNMIENA[1] | PRIVDEFENA[2]. */
  ra8_mpu_regs()->CTRL = ctrl;
}

/* Write the MAIR0/MAIR1 attribute-indirection pair -- see implementation for details. */
static void ra8_mpu_write_mair(uint32_t mair0, uint32_t mair1)
{
  volatile r_mpu_regs_t* mpu = ra8_mpu_regs();
  /* Arm Cortex-M85 TRM "MPU_MAIR0 / MPU_MAIR1": attribute-indirection bytes. */
  mpu->MAIR0 = mair0;
  mpu->MAIR1 = mair1;
}

/* Build the MPU_CTRL value from the static config -- see implementation for details. */
static uint32_t ra8_mpu_build_ctrl(const ra8_mpu_cfg_t* cfg)
{
  uint32_t ctrl = (uint32_t)k_ra8_mpu_ctrl_enable;
  if (cfg->privdefena) {
    ctrl |= (uint32_t)k_ra8_mpu_ctrl_privdefena;
  }
  if (cfg->hfnmiena) {
    ctrl |= (uint32_t)k_ra8_mpu_ctrl_hfnmiena;
  }
  return ctrl;
}

/* Validate the whole config block before any register write -- see implementation for details. */
static ra8_err_t ra8_mpu_validate_cfg(const ra8_mpu_cfg_t* cfg)
{
  const uint8_t implemented = ra8_mpu_dregion_count();
  if (cfg->region_count > implemented) {
    return k_ra8_err_invalid_arg;
  }
  if (cfg->region_count > 0U && cfg->regions == nullptr) {
    return k_ra8_err_null_ptr;
  }
  for (uint8_t i = 0U; i < cfg->region_count; ++i) {
    const ra8_err_t err = ra8_mpu_validate_region(&cfg->regions[i]);
    if (err != k_ra8_ok) {
      return err;
    }
  }
  return k_ra8_ok;
}

ra8_err_t ra8_mpu_configure(const ra8_mpu_cfg_t* cfg)
{
  RA8_CHECK_NULL_PTR(cfg, s_tag, "cfg must not be nullptr");
  const ra8_err_t verr = ra8_mpu_validate_cfg(cfg);
  if (verr != k_ra8_ok) {
    return verr;
  }

  /* Arm Cortex-M85 TRM "MPU_CTRL": disable before reprogramming. */
  ra8_mpu_write_ctrl(0U);
  ra8_mpu_write_mair(cfg->mair0, cfg->mair1);

  for (uint8_t i = 0U; i < cfg->region_count; ++i) {
    ra8_mpu_program_region(i, &cfg->regions[i]);
  }
  const uint8_t implemented = ra8_mpu_dregion_count();
  for (uint8_t i = cfg->region_count; i < implemented; ++i) {
    ra8_mpu_clear_region(i);
  }
  ra8_mpu_write_ctrl(ra8_mpu_build_ctrl(cfg));

  /* Enable MemManage fault dispatch via SHCSR.MEMFAULTENA (bit 16).
   * Without this bit, every MPU permission violation silently escalates
   * to HardFault -- the strong MemManage_Handler the application
   * installed never runs. Armv8-M ARM "B3.2.10 SHCSR, System Handler
   * Control and State Register" + Cortex-M85 TRM SCB+0x24. The MPU is
   * useless as a runtime safety mechanism without this enable; making
   * it the default of ra8_mpu_configure means every consumer gets
   * working MemFault delivery without having to know about the
   * separate SHCSR concern. Callers that genuinely want
   * MemFault->HardFault escalation can clear SHCSR.MEMFAULTENA
   * themselves after the configure call. */
  volatile uint32_t* shcsr = (volatile uint32_t*)k_ra8_mpu_shcsr_addr;
  *shcsr |= (uint32_t)k_ra8_mpu_shcsr_memfaultena;
  return k_ra8_ok;
}

ra8_err_t ra8_mpu_enable(void)
{
  volatile r_mpu_regs_t* mpu = ra8_mpu_regs();
  /* Arm Cortex-M85 TRM "MPU_CTRL.ENABLE" -- bit 0. */
  mpu->CTRL = mpu->CTRL | (uint32_t)k_ra8_mpu_ctrl_enable;
  return k_ra8_ok;
}

ra8_err_t ra8_mpu_disable(void)
{
  volatile r_mpu_regs_t* mpu = ra8_mpu_regs();
  /* Arm Cortex-M85 TRM "MPU_CTRL.ENABLE" -- bit 0. */
  mpu->CTRL = mpu->CTRL & ~(uint32_t)k_ra8_mpu_ctrl_enable;
  return k_ra8_ok;
}

ra8_err_t ra8_mpu_set_region(uint8_t region, const ra8_mpu_region_t* region_cfg)
{
  RA8_CHECK_NULL_PTR(region_cfg, s_tag, "region_cfg must not be nullptr");
  if (region >= ra8_mpu_dregion_count()) {
    return k_ra8_err_invalid_arg;
  }
  const ra8_err_t verr = ra8_mpu_validate_region(region_cfg);
  if (verr != k_ra8_ok) {
    return verr;
  }
  ra8_mpu_program_region(region, region_cfg);
  return k_ra8_ok;
}

/* =============================================================================
 * Canonical boot memory-attribute map (issue #576)
 * =============================================================================
 * The fixed 5-region map every RA8D2 image needs out of reset. This is the one
 * source of truth the reset path routes through instead of the raw
 * MAIR/RBAR/RLAR/CTRL pokes each app's system_init.c used to duplicate.
 */

/**
 * @enum ra8_mpu_boot_base_t
 * @brief Region base addresses for the canonical boot map.
 * @details Each base is 32-byte aligned (the Armv8-M region quantum), asserted
 *          below. Addresses are 32-bit on the target; `uintptr_t` keeps the
 *          host build (64-bit) casts honest.
 */
typedef enum : uintptr_t {
  k_ra8_mpu_boot_base_mram  = 0x02000000UL, /**< MRAM code base.             */
  k_ra8_mpu_boot_base_sram  = 0x22000000UL, /**< M85-private SRAM0+1 base.   */
  k_ra8_mpu_boot_base_sdram = 0x68000000UL, /**< External SDRAM base.        */
  k_ra8_mpu_boot_base_peri  = 0x40000000UL, /**< Peripheral window base.     */
  k_ra8_mpu_boot_base_shram = 0x22100000UL, /**< Shared M85<->M33 SRAM base. */
} ra8_mpu_boot_base_t;

/**
 * @enum ra8_mpu_boot_size_t
 * @brief Region byte sizes for the canonical boot map (base+size form).
 * @details Region 4 (the shared M85<->M33 bank) is 640 KiB -- deliberately NOT
 *          a power of two, so the size-checked `ra8_mpu_set_region()` rejects
 *          it, but the Armv8-M base+limit RBAR/RLAR pair encodes it exactly.
 *          Every size is a 32-byte multiple (asserted below).
 */
typedef enum : uint32_t {
  k_ra8_mpu_boot_size_mram  = 0x00100000UL, /**< 1 MiB MRAM code.             */
  k_ra8_mpu_boot_size_sram  = 0x00100000UL, /**< 1 MiB M85-private SRAM.      */
  k_ra8_mpu_boot_size_sdram = 0x04000000UL, /**< 64 MiB SDRAM.                */
  k_ra8_mpu_boot_size_peri  = 0x08000000UL, /**< 128 MiB peripheral window.   */
  k_ra8_mpu_boot_size_shram = 0x000A0000UL, /**< 640 KiB shared SRAM (!pow2). */
} ra8_mpu_boot_size_t;

/**
 * @var s_ra8_mpu_boot_regions
 * @brief The canonical 5-region boot memory-attribute map.
 * @details Region 0 maps MRAM code RO+executable cacheable; regions 1-2 map
 *          the M85-private SRAM and SDRAM RW/XN cacheable; region 3 maps the
 *          peripheral window RW/XN Device-nGnRE; region 4 maps the shared
 *          M85<->M33 SRAM RW/XN Normal non-cacheable so cross-core hand-offs
 *          stay coherent with the M85 D-cache on. Lives in `.rodata`, so it is
 *          readable from `SystemInit()` before the `.data` copy.
 * @note Const; installed verbatim by `ra8_mpu_apply_boot_map()`.
 * @warning Never modify at run time; exposed read-only via `ra8_mpu_boot_map()`.
 * @since 0.1.0
 */
static const ra8_mpu_region_t s_ra8_mpu_boot_regions[k_ra8_mpu_boot_region_count] = {
  /* 0: MRAM code -- RO + executable, Normal inner/outer WB/WA cacheable. */
  {
    .base       = (uintptr_t)k_ra8_mpu_boot_base_mram,
    .size       = (uint32_t)k_ra8_mpu_boot_size_mram,
    .priv       = k_ra8_mpu_perm_ro,
    .unpriv     = k_ra8_mpu_perm_ro,
    .executable = true,
    .shareable  = k_ra8_mpu_share_non,
    .attr_idx   = k_ra8_mpu_attr_idx_0,
  },
  /* 1: M85-private SRAM0+1 -- RW/XN, Normal WB/WA cacheable. */
  {
    .base       = (uintptr_t)k_ra8_mpu_boot_base_sram,
    .size       = (uint32_t)k_ra8_mpu_boot_size_sram,
    .priv       = k_ra8_mpu_perm_rw,
    .unpriv     = k_ra8_mpu_perm_rw,
    .executable = false,
    .shareable  = k_ra8_mpu_share_non,
    .attr_idx   = k_ra8_mpu_attr_idx_0,
  },
  /* 2: External SDRAM -- RW/XN, Normal WB/WA cacheable. */
  {
    .base       = (uintptr_t)k_ra8_mpu_boot_base_sdram,
    .size       = (uint32_t)k_ra8_mpu_boot_size_sdram,
    .priv       = k_ra8_mpu_perm_rw,
    .unpriv     = k_ra8_mpu_perm_rw,
    .executable = false,
    .shareable  = k_ra8_mpu_share_non,
    .attr_idx   = k_ra8_mpu_attr_idx_0,
  },
  /* 3: Peripheral window -- RW/XN, Device-nGnRE (AttrIdx 2). */
  {
    .base       = (uintptr_t)k_ra8_mpu_boot_base_peri,
    .size       = (uint32_t)k_ra8_mpu_boot_size_peri,
    .priv       = k_ra8_mpu_perm_rw,
    .unpriv     = k_ra8_mpu_perm_rw,
    .executable = false,
    .shareable  = k_ra8_mpu_share_non,
    .attr_idx   = k_ra8_mpu_attr_idx_2,
  },
  /* 4: Shared M85<->M33 SRAM2+3 (640 KiB) -- RW/XN, Normal non-cacheable
     (AttrIdx 1) so the mailbox + CPU1 RAM stay coherent across cores. */
  {
    .base       = (uintptr_t)k_ra8_mpu_boot_base_shram,
    .size       = (uint32_t)k_ra8_mpu_boot_size_shram,
    .priv       = k_ra8_mpu_perm_rw,
    .unpriv     = k_ra8_mpu_perm_rw,
    .executable = false,
    .shareable  = k_ra8_mpu_share_non,
    .attr_idx   = k_ra8_mpu_attr_idx_1,
  },
};

/* The boot map must be 5 regions, every base 32-byte aligned and every size a
   non-zero 32-byte multiple -- the Armv8-M RBAR/RLAR granularity. Region 4 is
   intentionally 640 KiB (not a power of two). */
static_assert((sizeof(s_ra8_mpu_boot_regions) / sizeof(s_ra8_mpu_boot_regions[0])) ==
                (size_t)k_ra8_mpu_boot_region_count,
              "boot map must hold exactly k_ra8_mpu_boot_region_count regions");
static_assert(((uint32_t)k_ra8_mpu_boot_base_mram & ((uint32_t)k_ra8_mpu_min_region_size - 1U)) ==
                0U,
              "boot MRAM base must be 32-byte aligned");
static_assert(((uint32_t)k_ra8_mpu_boot_base_sram & ((uint32_t)k_ra8_mpu_min_region_size - 1U)) ==
                0U,
              "boot SRAM base must be 32-byte aligned");
static_assert(((uint32_t)k_ra8_mpu_boot_base_sdram & ((uint32_t)k_ra8_mpu_min_region_size - 1U)) ==
                0U,
              "boot SDRAM base must be 32-byte aligned");
static_assert(((uint32_t)k_ra8_mpu_boot_base_peri & ((uint32_t)k_ra8_mpu_min_region_size - 1U)) ==
                0U,
              "boot peripheral base must be 32-byte aligned");
static_assert(((uint32_t)k_ra8_mpu_boot_base_shram & ((uint32_t)k_ra8_mpu_min_region_size - 1U)) ==
                0U,
              "boot shared-SRAM base must be 32-byte aligned");
static_assert((((uint32_t)k_ra8_mpu_boot_size_mram | (uint32_t)k_ra8_mpu_boot_size_sram |
                (uint32_t)k_ra8_mpu_boot_size_sdram | (uint32_t)k_ra8_mpu_boot_size_peri |
                (uint32_t)k_ra8_mpu_boot_size_shram) &
               ((uint32_t)k_ra8_mpu_min_region_size - 1U)) == 0U,
              "every boot region size must be a 32-byte multiple");
static_assert((uint32_t)k_ra8_mpu_boot_size_shram >= (uint32_t)k_ra8_mpu_min_region_size,
              "boot shared-SRAM size must be at least the 32-byte region quantum");

const ra8_mpu_region_t* ra8_mpu_boot_map(uint8_t* out_count)
{
  if (out_count == nullptr) {
    return nullptr;
  }
  *out_count = (uint8_t)k_ra8_mpu_boot_region_count;
  return s_ra8_mpu_boot_regions;
}

ra8_err_t ra8_mpu_apply_boot_map(void)
{
  /* Precondition: the silicon must implement at least the map's region count.
     On the RA8D2 M85 (16 regions) this always holds; the guard keeps the boot
     path from installing a truncated, half-covered map on an unexpected part. */
  const uint8_t implemented = ra8_mpu_dregion_count();
  if (implemented < (uint8_t)k_ra8_mpu_boot_region_count) {
    return k_ra8_err_invalid_arg;
  }

  /* Arm Cortex-M85 TRM "MPU_CTRL": disable before reprogramming. */
  ra8_mpu_write_ctrl(0U);
  ra8_mpu_write_mair((uint32_t)k_ra8_mpu_boot_mair0, (uint32_t)k_ra8_mpu_boot_mair1);

  /* Clear any stale high-index regions FIRST, then install the map, so an
     aborted reprogram never leaves a stale enabled region beside the new map,
     and the last register write reflects the highest map region. */
  for (uint8_t i = (uint8_t)k_ra8_mpu_boot_region_count; i < implemented; ++i) {
    ra8_mpu_clear_region(i);
  }
  for (uint8_t i = 0U; i < (uint8_t)k_ra8_mpu_boot_region_count; ++i) {
    ra8_mpu_program_region(i, &s_ra8_mpu_boot_regions[i]);
  }

  /* Retire the region writes, enable the MPU with PRIVDEFENA, then flush the
     pipeline so the very next access sees the new attribute map. */
  ra8_hw_dsb();
  ra8_mpu_write_ctrl((uint32_t)k_ra8_mpu_ctrl_enable | (uint32_t)k_ra8_mpu_ctrl_privdefena);
  ra8_hw_dsb();
  ra8_hw_isb();
  return k_ra8_ok;
}

bool ra8_mpu_is_enabled(void)
{
  /* Arm Cortex-M85 TRM "MPU_CTRL.ENABLE" -- bit 0. */
  return (ra8_mpu_regs()->CTRL & (uint32_t)k_ra8_mpu_ctrl_enable) != 0U;
}
