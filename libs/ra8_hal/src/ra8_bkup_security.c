/**
 * @file ra8_bkup_security.c
 * @brief Battery Backup Function (VBATT) driver -- TrustZone partitioning
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * TrustZone attribution half of the RA8D2 battery-backup driver: the
 * security / privilege boundary registers that partition the VBTBKRn
 * backup store between the Secure and Non-secure worlds. Split out of
 * ``ra8_bkup.c`` to stay under the per-file line cap; the core
 * lifecycle, backup store and interrupt path stay there, and tamper
 * detection lives in ``ra8_bkup_tamper.c``.
 *
 * This TU touches exactly four registers -- BBFSAR, VBRSABAR,
 * VBRPABARS and VBRPABARNS -- and its validators exist only to keep
 * those four in range, so it shares nothing with the tamper path.
 *
 * @par PRCR write protection (issue #131)
 * These four are attribution registers and sit behind **PRC4**, not the
 * PRC1 that guards the rest of the block (HUM Ch 13.1 Table 13.1
 * "Association between PRCR bits and use of registers to be protected"
 * p 520-521). ``ra8_bkup_security_apply`` was broken for its whole life
 * by issuing its writes with PRC4 locked, where they were silently
 * discarded -- no bus fault, no status flag. The single
 * ``RA8_PROTECTED_WRITE(k_ra8_prcr_unlock_sar)`` window below is what
 * makes those writes land; do not separate it from the stores it
 * guards. See the ``ra8_bkup.c`` file comment for the bench evidence.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_bkup.h"
#include "ra8_bkup_internal.h"
#include "ra8_bkup_regs.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_register_protection.h"

/* =============================================================================
 * Internal helpers
 * =============================================================================
 */

/**
 * @brief Validate one boundary-address register value.
 *
 * @param[in] addr Candidate value (lower-16 of the boundary).
 * @return ``k_ra8_ok`` if 32-byte aligned and below saba_max.
 *
 * @pre None.
 * @post No side effects.
 *
 * @details See the matching header declaration for the full
 * contract; this site adds no behaviour beyond what the public
 * API documents.
 * @retval k_ra8_ok Success path.
 * @retval k_ra8_err_invalid_arg Caller violated a precondition.
 * @note Thread safety: see the header declaration.
 * @since 0.1.0
 *
 * @pre Module has been initialized.
 * @post Side effects bounded to documented state.
 */
RA8_INTERNAL static ra8_err_t internal_validate_boundary(uint16_t addr)
{
  if ((addr & k_ra8_bkup_saba_align_mask) != 0U) {
    return k_ra8_err_invalid_arg;
  }
  if (addr > k_ra8_bkup_saba_max) {
    return k_ra8_err_invalid_arg;
  }
  return k_ra8_ok;
}

/**
 * @brief Validate every field of an ``ra8_bkup_security_config_t``.
 *
 * @param[in] cfg Caller-supplied, null-checked security config.
 * @return ``k_ra8_ok`` if every field is in range.
 *
 * @pre cfg != nullptr.
 * @pre cfg->bbfsar / cfg->saba / cfg->pabas / cfg->pabans are populated.
 * @post No side effects.
 *
 * @details See the matching header declaration for the full
 * contract; this site adds no behaviour beyond what the public
 * API documents.
 * @retval k_ra8_ok Success path.
 * @retval k_ra8_err_invalid_arg Caller violated a precondition.
 * @note Thread safety: see the header declaration.
 * @since 0.1.0
 *
 * @post Side effects bounded to documented state.
 */
RA8_INTERNAL static ra8_err_t internal_validate_security_cfg(const ra8_bkup_security_config_t* cfg)
{
  if ((cfg->bbfsar & ~k_ra8_bkup_bbfsar_mask_all) != 0U) {
    return k_ra8_err_invalid_arg;
  }
  ra8_err_t err = internal_validate_boundary(cfg->saba);
  RA8_RETURN_ON_ERROR(err, g_bkup_tag, "security_apply: saba bad"); /* GCOVR_EXCL_BR_LINE */
  err = internal_validate_boundary(cfg->pabas);
  RA8_RETURN_ON_ERROR(err, g_bkup_tag, "security_apply: pabas bad"); /* GCOVR_EXCL_BR_LINE */
  err = internal_validate_boundary(cfg->pabans);
  RA8_RETURN_ON_ERROR(err, g_bkup_tag, "security_apply: pabans bad"); /* GCOVR_EXCL_BR_LINE */
  return k_ra8_ok;
}

/* =============================================================================
 * TrustZone partitioning
 * =============================================================================
 */

[[nodiscard]] ra8_err_t ra8_bkup_security_apply(const ra8_bkup_security_config_t* cfg)
{
  RA8_CHECK_NULL_PTR(cfg, g_bkup_tag, "security cfg must not be nullptr");
  const ra8_err_t v_err = internal_validate_security_cfg(cfg);
  RA8_RETURN_ON_ERROR(v_err, g_bkup_tag, "security_apply: cfg bad"); /* GCOVR_EXCL_BR_LINE */

  /* BBFSAR / VBRSABAR / VBRPABARS / VBRPABARNS are security-attribution
   * registers, so they sit behind PRC4 -- not the PRC1 that guards the rest
   * of this block (HUM Ch 13.1 Table 13.1 p 521). */
  RA8_PROTECTED_WRITE(k_ra8_prcr_unlock_sar)
  {
    /* HUM Ch 12.2.1 "BBFSAR : Battery Backup Function Security Attribute Register", p 500 */
    *ra8_bkup_bbfsar() = (uint32_t)(cfg->bbfsar & k_ra8_bkup_bbfsar_mask_all);
    /* HUM Ch 12.2.2 "VBRSABAR : VBATT Backup Register Security Attribute Boundary Address Register", p 502 */
    *ra8_bkup_vbrsabar() = cfg->saba;
    /* HUM Ch 12.2.3 "VBRPABARS : VBATT Backup Register Privilege Attribute Boundary Address Register for Secure Region", p 502 */
    *ra8_bkup_vbrpabars() = cfg->pabas;
    /* HUM Ch 12.2.4 "VBRPABARNS : VBATT Backup Register Privilege Attribute Boundary Address Register for Non-secure Region", p 503 */
    *ra8_bkup_vbrpabarns() = cfg->pabans;
  }
  return k_ra8_ok;
}

[[nodiscard]] ra8_err_t ra8_bkup_security_get(ra8_bkup_security_config_t* cfg)
{
  RA8_CHECK_NULL_PTR(cfg, g_bkup_tag, "security cfg must not be nullptr");
  /* HUM Ch 12.2.1 "BBFSAR : Battery Backup Function Security Attribute Register", p 500 */
  cfg->bbfsar = (uint32_t)(*ra8_bkup_bbfsar() & k_ra8_bkup_bbfsar_mask_all);
  /* HUM Ch 12.2.2 "VBRSABAR : VBATT Backup Register Security Attribute Boundary Address Register", p 502 */
  cfg->saba = *ra8_bkup_vbrsabar();
  /* HUM Ch 12.2.3 "VBRPABARS : VBATT Backup Register Privilege Attribute Boundary Address Register for Secure Region", p 502 */
  cfg->pabas = *ra8_bkup_vbrpabars();
  /* HUM Ch 12.2.4 "VBRPABARNS : VBATT Backup Register Privilege Attribute Boundary Address Register for Non-secure Region", p 503 */
  cfg->pabans = *ra8_bkup_vbrpabarns();
  return k_ra8_ok;
}
