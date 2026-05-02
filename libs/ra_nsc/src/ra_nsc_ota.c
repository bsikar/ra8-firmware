/**
 * @file ra_nsc_ota.c
 * @brief NSC veneers: OTA bank-commit + flash-bank-config write
 *
 * @par Tag
 * [Ring 4 / NSC] {World: NSC}
 *
 * @details
 * The two veneers in this TU bridge the NS-side OTA agent into
 * the secure-only privileged operations:
 *
 * - ``ra_nsc_ota_commit``: validates ``target_bank`` and forwards
 *   to ``ra_ota_commit_swap_bank``.
 * - ``ra_nsc_flash_bank_config``: forwards a 32-bit value through
 *   ``ra_ota_commit_set_bank_config`` (which masks down to the
 *   allowed bits).
 *
 * No pointers cross the boundary, so no NS-region range check is
 * needed in these veneers.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ota_commit.h"
#include "ra_err.h"
#include "ra_nsc.h"
#include "ra_nsc_veneer.h"

RA_NSC_VENEER ra_err_t ra_nsc_ota_commit(uint8_t target_bank)
{
  if ((target_bank != (uint8_t)k_ra_ota_bank_a) && (target_bank != (uint8_t)k_ra_ota_bank_b)) {
    return k_ra_err_invalid_arg;
  }
  return ra_ota_commit_swap_bank((ra_ota_bank_t)target_bank);
}

RA_NSC_VENEER ra_err_t ra_nsc_flash_bank_config(uint32_t raw_value)
{
  /* The secure side masks; we just forward. The NSC veneer's value
   * is in *labelling* the operation as crossing the security
   * boundary, so static analysis can flag NS-side callers. */
  return ra_ota_commit_set_bank_config(raw_value);
}
