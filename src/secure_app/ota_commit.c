/**
 * @file ota_commit.c
 * @brief Secure-side OTA commit + flash-bank-config implementation
 *
 * @par Tag
 * [Ring 5 / SECAPP] {World: S}
 *
 * @details
 * Maintains an in-memory shadow of the boot-bank option byte and
 * the bank-config register. The real ``ra_flash_*`` and PRCR-unlock
 * sequencing is documented inline as TODO call-sites; today's
 * implementation lets the unit tests drive the masking + state
 * machine logic against a host-safe shadow.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ota_commit.h"

#include <stdint.h>

#include "ra_check.h"
#include "ra_err.h"

static const char* s_tag = "OTACMT";

/**
 * @var s_pending
 * @brief Whether a swap-bank request is currently armed.
 *
 * @note Direct modification outside this TU is forbidden.
 * @since 0.1.0
 */
static bool s_pending = false;

/**
 * @var s_pending_target
 * @brief Bank that will be active after the next reset, when ``s_pending``.
 *
 * @note Only meaningful while ``s_pending`` is true.
 * @since 0.1.0
 */
static ra_ota_bank_t s_pending_target = k_ra_ota_bank_a;

/**
 * @var s_bank_config
 * @brief Shadow of the masked bank-config register.
 *
 * @note Direct modification outside this TU is forbidden.
 * @since 0.1.0
 */
static uint32_t s_bank_config = 0U;

/**
 * @brief Reset the OTA commit shadow registers to power-on defaults.
 *
 * @details
 * Clears the pending swap-bank request, returns the pending target
 * to bank A, and zeroes the bank-config shadow. Used by the unit
 * tests between scenarios and during secure-side bring-up.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Always; the operation cannot fail.
 *
 * @pre Caller is in the secure-side init/test path.
 * @pre No NS-side OTA orchestration is in flight.
 * @post ``s_pending == false``.
 * @post ``s_bank_config == 0``.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
ra_err_t ra_ota_commit_reset(void)
{
  s_pending        = false;
  s_pending_target = k_ra_ota_bank_a;
  s_bank_config    = 0U;
  return k_ra_ok;
}

/**
 * @brief Arm a boot-bank swap to take effect on the next reset.
 *
 * @details
 * Records the requested target bank in the pending shadow. The
 * actual OFS3 option-byte write -- protected by the PRCR unlock
 * dance -- lands in ; idempotency policy lives here so the
 * NSC veneer does not need to grow that logic.
 *
 * @param[in] target Bank to activate after the next reset
 *                   (``k_ra_ota_bank_a`` or ``k_ra_ota_bank_b``).
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok                 Swap request armed.
 * @retval k_ra_err_invalid_arg    ``target`` is not a known bank id.
 * @retval k_ra_err_invalid_state  A swap is already pending.
 *
 * @pre Caller is in the secure-side dispatch path.
 * @pre Reset is not currently in flight.
 * @post On success, ``s_pending == true`` and ``s_pending_target == target``.
 * @post On error, no shadow state is mutated.
 *
 * @note Not thread-safe.
 * @see ra_ota_commit_pending
 * @since 0.1.0
 */
ra_err_t ra_ota_commit_swap_bank(ra_ota_bank_t target)
{
  if ((target != k_ra_ota_bank_a) && (target != k_ra_ota_bank_b)) {
    return k_ra_err_invalid_arg;
  }
  if (s_pending) {
    return k_ra_err_invalid_state;
  }
  /* TODO: unlock PRCR, call ra_flash_write on the OFS3
   * option region, re-lock PRCR. The masking + idempotency policy
   * lives here so the veneer doesn't need to grow that logic. */
  s_pending_target = target;
  s_pending        = true;
  (void)s_tag;
  return k_ra_ok;
}

/* Read the currently armed swap-bank target, if any -- see implementation for details. */
ra_err_t ra_ota_commit_pending(ra_ota_bank_t* out_target)
{
  RA_CHECK_NULL_PTR(out_target, s_tag, "pending: out_target");
  if (!s_pending) {
    return k_ra_err_no_data;
  }
  *out_target = s_pending_target;
  return k_ra_ok;
}

/**
 * @brief Update the masked bank-config shadow.
 *
 * @details
 * Applies ``k_ra_ota_bank_config_allowed`` to the supplied raw
 * value so reserved bits cannot be programmed by an NS caller.
 * The real PRCR-unlocked option-byte write lands in .
 *
 * @param[in] raw_value Caller-proposed bank-config value.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Always; masking cannot fail.
 *
 * @pre Caller is in the secure-side dispatch path.
 * @pre ``raw_value`` may take any uint32_t value.
 * @post ``s_bank_config == (raw_value & k_ra_ota_bank_config_allowed)``.
 * @post No other state is mutated.
 *
 * @note Not thread-safe.
 * @see ra_ota_commit_get_bank_config
 * @since 0.1.0
 */
ra_err_t ra_ota_commit_set_bank_config(uint32_t raw_value)
{
  /* TODO: same PRCR-unlock dance as swap_bank. */
  s_bank_config = raw_value & (uint32_t)k_ra_ota_bank_config_allowed;
  return k_ra_ok;
}

/* Read the masked bank-config shadow -- see implementation for details. */
ra_err_t ra_ota_commit_get_bank_config(uint32_t* out_value)
{
  RA_CHECK_NULL_PTR(out_value, s_tag, "get_bank_config: out_value");
  *out_value = s_bank_config;
  return k_ra_ok;
}
