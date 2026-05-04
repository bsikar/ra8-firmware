/**
 * @file ra_mpu.c
 * @brief Cortex-M85 Memory Protection Unit configuration helper
 *
 * @details
 * Implementation of the public API in `ra_mpu.h`. Programmes the
 * Armv8-M MPU register block documented in `ra8d2_mpu_regs.h` from a
 * static region table. The register-level shape (RBAR / RLAR / MAIR0
 * / MAIR1) follows the Arm Cortex-M85 TRM "MPU register summary";
 * the helper adds power-of-two validation, AP[1:0] encoding from
 * RO/RW/None pairs, and bounds checking against the implemented
 * region count reported by `MPU_TYPE.DREGION`.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_mpu.h"

#include <stddef.h>
#include <stdint.h>

#include "ra8d2_mpu_regs.h"
#include "ra_check.h"
#include "ra_err.h"

static const char* s_tag = "MPU";

/**
 * @enum ra_mpu_ap_t
 * @brief Encoded AP[1:0] values per the Cortex-M85 TRM.
 */
typedef enum : uint8_t {
  k_ra_mpu_ap_priv_rw_unpriv_none = 0U,    /**< 00: priv RW, unpriv -.   */
  k_ra_mpu_ap_priv_rw_unpriv_rw   = 1U,    /**< 01: priv RW, unpriv RW.  */
  k_ra_mpu_ap_priv_ro_unpriv_none = 2U,    /**< 10: priv RO, unpriv -.   */
  k_ra_mpu_ap_priv_ro_unpriv_ro   = 3U,    /**< 11: priv RO, unpriv RO.  */
  k_ra_mpu_ap_invalid             = 0xFFU, /**< Sentinel: not encodable. */
} ra_mpu_ap_t;

/* Test whether `value` is a positive power of two -- see implementation for details. */
static inline bool ra_mpu_is_pow2(uint32_t value)
{
  return (value != 0U) && ((value & (value - 1U)) == 0U);
}

/* Map a (priv, unpriv) permission pair onto the Armv8-M AP code -- see implementation for details. */
static uint8_t ra_mpu_encode_ap(ra_mpu_perm_t priv, ra_mpu_perm_t unpriv)
{
  /* Arm Cortex-M85 TRM "MPU_RBAR" -- AP[1:0] table. */
  if (priv == k_ra_mpu_perm_rw && unpriv == k_ra_mpu_perm_none) {
    return (uint8_t)k_ra_mpu_ap_priv_rw_unpriv_none;
  }
  if (priv == k_ra_mpu_perm_rw && unpriv == k_ra_mpu_perm_rw) {
    return (uint8_t)k_ra_mpu_ap_priv_rw_unpriv_rw;
  }
  if (priv == k_ra_mpu_perm_ro && unpriv == k_ra_mpu_perm_none) {
    return (uint8_t)k_ra_mpu_ap_priv_ro_unpriv_none;
  }
  if (priv == k_ra_mpu_perm_ro && unpriv == k_ra_mpu_perm_ro) {
    return (uint8_t)k_ra_mpu_ap_priv_ro_unpriv_ro;
  }
  return (uint8_t)k_ra_mpu_ap_invalid;
}

/* Read the implemented region count from `MPU_TYPE -- see implementation for details. */
static uint8_t ra_mpu_dregion_count(void)
{
  /* Arm Cortex-M85 TRM "MPU_TYPE": DREGION lives in bits 15:8. */
  const uint32_t type = ra_mpu_regs()->TYPE;
  return (uint8_t)((type & (uint32_t)k_ra_mpu_type_dregion_mask) >>
                   (uint32_t)k_ra_mpu_type_dregion_shift);
}

/* Validate a single region descriptor -- see implementation for details. */
static ra_err_t ra_mpu_validate_region(const ra_mpu_region_t* r)
{
  if (!ra_mpu_is_pow2(r->size) || r->size < (uint32_t)k_ra_mpu_min_region_size) {
    return k_ra_err_invalid_arg;
  }
  if ((r->base & (uintptr_t)(r->size - 1U)) != 0U) {
    return k_ra_err_invalid_arg;
  }
  if (ra_mpu_encode_ap(r->priv, r->unpriv) == (uint8_t)k_ra_mpu_ap_invalid) {
    return k_ra_err_invalid_arg;
  }
  return k_ra_ok;
}

/* Build the MPU_RBAR value for a region descriptor -- see implementation for details. */
static uint32_t ra_mpu_build_rbar(const ra_mpu_region_t* r)
{
  /* Arm Cortex-M85 TRM "MPU_RBAR": BASE[31:5] | SH[4:3] | AP[2:1] | XN[0]. */
  const uint32_t base = (uint32_t)r->base & (uint32_t)k_ra_mpu_rbar_base_mask;
  const uint32_t ap =
    ((uint32_t)ra_mpu_encode_ap(r->priv, r->unpriv) << (uint32_t)k_ra_mpu_rbar_ap_shift) &
    (uint32_t)k_ra_mpu_rbar_ap_mask;
  const uint32_t sh =
    ((uint32_t)r->shareable << (uint32_t)k_ra_mpu_rbar_sh_shift) & (uint32_t)k_ra_mpu_rbar_sh_mask;
  const uint32_t xn = r->executable ? 0U : (uint32_t)k_ra_mpu_rbar_xn_mask;
  return base | sh | ap | xn;
}

/* Build the MPU_RLAR value for a region descriptor -- see implementation for details. */
static uint32_t ra_mpu_build_rlar(const ra_mpu_region_t* r)
{
  /* Arm Cortex-M85 TRM "MPU_RLAR": LIMIT[31:5] | AttrIndx[3:1] | EN[0]. */
  const uint32_t limit_inclusive = (uint32_t)(r->base + r->size - 1U);
  const uint32_t limit           = limit_inclusive & (uint32_t)k_ra_mpu_rlar_limit_mask;
  const uint32_t idx = ((uint32_t)r->attr_idx << (uint32_t)k_ra_mpu_rlar_attridx_shift) &
                       (uint32_t)k_ra_mpu_rlar_attridx_mask;
  return limit | idx | (uint32_t)k_ra_mpu_rlar_en_mask;
}

/* Write a single region's RBAR/RLAR pair via MPU_RNR selection -- see implementation for details. */
static void ra_mpu_program_region(uint8_t region, const ra_mpu_region_t* r)
{
  volatile r_mpu_regs_t* mpu = ra_mpu_regs();
  mpu->RNR                   = (uint32_t)region;
  mpu->RBAR                  = ra_mpu_build_rbar(r);
  mpu->RLAR                  = ra_mpu_build_rlar(r);
}

/* Disable a single region by clearing RLAR -- see implementation for details. */
static void ra_mpu_clear_region(uint8_t region)
{
  volatile r_mpu_regs_t* mpu = ra_mpu_regs();
  mpu->RNR                   = (uint32_t)region;
  mpu->RLAR                  = 0U;
}

/* Build the MPU_CTRL value from the static config -- see implementation for details. */
static uint32_t ra_mpu_build_ctrl(const ra_mpu_cfg_t* cfg)
{
  uint32_t ctrl = (uint32_t)k_ra_mpu_ctrl_enable;
  if (cfg->privdefena) {
    ctrl |= (uint32_t)k_ra_mpu_ctrl_privdefena;
  }
  if (cfg->hfnmiena) {
    ctrl |= (uint32_t)k_ra_mpu_ctrl_hfnmiena;
  }
  return ctrl;
}

/* Validate the whole config block before any register write -- see implementation for details. */
static ra_err_t ra_mpu_validate_cfg(const ra_mpu_cfg_t* cfg)
{
  const uint8_t implemented = ra_mpu_dregion_count();
  if (cfg->region_count > implemented) {
    return k_ra_err_invalid_arg;
  }
  if (cfg->region_count > 0U && cfg->regions == nullptr) {
    return k_ra_err_null_ptr;
  }
  for (uint8_t i = 0U; i < cfg->region_count; ++i) {
    const ra_err_t err = ra_mpu_validate_region(&cfg->regions[i]);
    if (err != k_ra_ok) {
      return err;
    }
  }
  return k_ra_ok;
}

/* Ra mpu configure -- see implementation for details. */
ra_err_t ra_mpu_configure(const ra_mpu_cfg_t* cfg)
{
  RA_CHECK_NULL_PTR(cfg, s_tag, "cfg must not be nullptr");
  const ra_err_t verr = ra_mpu_validate_cfg(cfg);
  if (verr != k_ra_ok) {
    return verr;
  }

  volatile r_mpu_regs_t* mpu = ra_mpu_regs();
  /* Arm Cortex-M85 TRM "MPU_CTRL": disable before reprogramming. */
  mpu->CTRL  = 0U;
  mpu->MAIR0 = cfg->mair0;
  mpu->MAIR1 = cfg->mair1;

  for (uint8_t i = 0U; i < cfg->region_count; ++i) {
    ra_mpu_program_region(i, &cfg->regions[i]);
  }
  const uint8_t implemented = ra_mpu_dregion_count();
  for (uint8_t i = cfg->region_count; i < implemented; ++i) {
    ra_mpu_clear_region(i);
  }
  mpu->CTRL = ra_mpu_build_ctrl(cfg);
  return k_ra_ok;
}

/* Ra mpu enable -- see implementation for details. */
ra_err_t ra_mpu_enable(void)
{
  volatile r_mpu_regs_t* mpu = ra_mpu_regs();
  /* Arm Cortex-M85 TRM "MPU_CTRL.ENABLE" -- bit 0. */
  mpu->CTRL = mpu->CTRL | (uint32_t)k_ra_mpu_ctrl_enable;
  return k_ra_ok;
}

/* Ra mpu disable -- see implementation for details. */
ra_err_t ra_mpu_disable(void)
{
  volatile r_mpu_regs_t* mpu = ra_mpu_regs();
  /* Arm Cortex-M85 TRM "MPU_CTRL.ENABLE" -- bit 0. */
  mpu->CTRL = mpu->CTRL & ~(uint32_t)k_ra_mpu_ctrl_enable;
  return k_ra_ok;
}

/* Ra mpu set region -- see implementation for details. */
ra_err_t ra_mpu_set_region(uint8_t region, const ra_mpu_region_t* region_cfg)
{
  RA_CHECK_NULL_PTR(region_cfg, s_tag, "region_cfg must not be nullptr");
  if (region >= ra_mpu_dregion_count()) {
    return k_ra_err_invalid_arg;
  }
  const ra_err_t verr = ra_mpu_validate_region(region_cfg);
  if (verr != k_ra_ok) {
    return verr;
  }
  ra_mpu_program_region(region, region_cfg);
  return k_ra_ok;
}
