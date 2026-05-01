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

ra_err_t ra_ota_commit_reset(void)
{
  s_pending        = false;
  s_pending_target = k_ra_ota_bank_a;
  s_bank_config    = 0U;
  return k_ra_ok;
}

ra_err_t ra_ota_commit_swap_bank(ra_ota_bank_t target)
{
  if ((target != k_ra_ota_bank_a) && (target != k_ra_ota_bank_b)) {
    return k_ra_err_invalid_arg;
  }
  if (s_pending) {
    return k_ra_err_invalid_state;
  }
  /* TODO(wave-13): unlock PRCR, call ra_flash_write on the OFS3
   * option region, re-lock PRCR. The masking + idempotency policy
   * lives here so the veneer doesn't need to grow that logic. */
  s_pending_target = target;
  s_pending        = true;
  (void)s_tag;
  return k_ra_ok;
}

ra_err_t ra_ota_commit_pending(ra_ota_bank_t* out_target)
{
  RA_CHECK_NULL_PTR(out_target, s_tag, "pending: out_target");
  if (!s_pending) {
    return k_ra_err_no_data;
  }
  *out_target = s_pending_target;
  return k_ra_ok;
}

ra_err_t ra_ota_commit_set_bank_config(uint32_t raw_value)
{
  /* TODO(wave-13): same PRCR-unlock dance as swap_bank. */
  s_bank_config = raw_value & (uint32_t)k_ra_ota_bank_config_allowed;
  return k_ra_ok;
}

ra_err_t ra_ota_commit_get_bank_config(uint32_t* out_value)
{
  RA_CHECK_NULL_PTR(out_value, s_tag, "get_bank_config: out_value");
  *out_value = s_bank_config;
  return k_ra_ok;
}
