/**
 * @file ra8_nsc_ota.c
 * @brief NSC veneers: OTA bank-commit + flash-bank-config write
 *
 * @par Tag
 * [Ring 4 / NSC] {World: NSC}
 *
 * @details
 * The two veneers in this TU bridge the NS-side OTA agent into
 * the secure-only privileged operations:
 *
 * - ``ra8_nsc_ota_commit``: validates ``target_bank`` and forwards
 *   to ``ra8_ota_commit_swap_bank``.
 * - ``ra8_nsc_flash_bank_config``: forwards a 32-bit value through
 *   ``ra8_ota_commit_set_bank_config`` (which masks down to the
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
#include "ra8_err.h"
#include "ra8_nsc.h"
#include "ra8_nsc_veneer.h"

/**
 * @brief NSC veneer: persist an OTA bank-swap commit.
 *
 * @details Validates ``target_bank`` and forwards to
 *   ``ra8_ota_commit_swap_bank`` in the secure world.
 *
 * @param[in] target_bank Bank to boot from (k_ra8_ota_bank_a/b).
 *
 * @return ra8_err_t outcome.
 * @retval k_ra8_ok              Bank-swap latched.
 * @retval k_ra8_err_invalid_arg Unknown bank index.
 *
 * @pre TrustZone substrate initialized.
 * @pre Caller has verified the new image.
 * @post On success the next NVIC reset boots from ``target_bank``.
 * @post On failure the active bank is unchanged.
 *
 * @note Thread-safe: serialised by the secure ra8_ota lock.
 * @since 0.1.0
 */
RA8_NSC_VENEER ra8_err_t ra8_nsc_ota_commit(uint8_t target_bank)
{
  if ((target_bank != (uint8_t)k_ra8_ota_bank_a) && (target_bank != (uint8_t)k_ra8_ota_bank_b)) {
    return k_ra8_err_invalid_arg;
  }
  return ra8_ota_commit_swap_bank((ra8_ota_bank_t)target_bank);
}

/**
 * @brief NSC veneer: write the flash bank-config word.
 *
 * @details
 * Forwards the 32-bit raw value through ``ra8_ota_commit_set_bank_config``
 * which masks it down to the allowed bits inside the secure world.
 * The veneer's value is in *labelling* the operation as crossing the
 * security boundary, so static analysis can flag NS-side callers.
 *
 * @param[in] raw_value Caller-supplied bank-config word.
 *
 * @return ra8_err_t outcome.
 * @retval k_ra8_ok               Bank-config word latched (after secure mask).
 * @retval k_ra8_err_invalid_arg  Secure side rejected the value.
 *
 * @pre TrustZone substrate up.
 * @pre Caller has authority to mutate the bank-config (policy is
 *      enforced in the secure world).
 * @post On success the persisted bank-config word is updated.
 * @post On failure no flash bytes were written.
 *
 * @par TrustZone:
 *   NS->S boundary via ``cmse_nonsecure_entry``. Scalar argument only;
 *   the secure side is responsible for masking off bits the NS world
 *   is not permitted to flip.
 *
 * @note Thread-safe: serialised by the secure ra8_ota lock.
 * @since 0.1.0
 */
RA8_NSC_VENEER ra8_err_t ra8_nsc_flash_bank_config(uint32_t raw_value)
{
  /* The secure side masks; we just forward. The NSC veneer's value
   * is in *labelling* the operation as crossing the security
   * boundary, so static analysis can flag NS-side callers. */
  return ra8_ota_commit_set_bank_config(raw_value);
}
