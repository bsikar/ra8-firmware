/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ota_commit.c
 * @brief Secure-side OTA commit + flash-bank-config implementation
 *
 * @par Tag
 * [Ring 5 / SECAPP] {World: S}
 *
 * @details
 * The commit + bank-config writes are option-region (``OFS3`` / ``BTFLG``)
 * programs behind the PRCR unlock -- brick-risky and not yet wired, so they are
 * bench-gated. On silicon both are **FAIL-CLOSED**: they run their argument /
 * idempotency checks and then return ``k_ra8_err_not_supported`` rather than a
 * fake ``k_ra8_ok`` for a commit that never touched flash (T5-10). Under
 * ``RA8_OFF_TARGET`` they instead maintain an in-memory shadow of the
 * boot-bank option byte and the bank-config register so the unit tests can
 * drive the masking + single-shot state-machine logic host-safely. The real
 * ``ra8_flash_*`` + PRCR-unlock call sites are marked ``TODO`` at each fail-closed
 * branch.
 */

#include "ota_commit.h"

#include <stdint.h>

#include "ra8_check.h"
#include "ra8_err.h"

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
static ra8_ota_bank_t s_pending_target = k_ra8_ota_bank_a;

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
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Always; the operation cannot fail.
 *
 * @pre Caller is in the secure-side init/test path.
 * @pre No NS-side OTA orchestration is in flight.
 * @post ``s_pending == false``.
 * @post ``s_bank_config == 0``.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
ra8_err_t ra8_ota_commit_reset(void)
{
  s_pending        = false;
  s_pending_target = k_ra8_ota_bank_a;
  s_bank_config    = 0U;
  return k_ra8_ok;
}

/** @brief Implementation of `ra8_ota_commit_swap_bank()` -- host shadow under
 *  RA8_OFF_TARGET; fail-closed on silicon (the real option-byte write is
 *  bench-gated and brick-risky, so it must not fake success -- T5-10). */
ra8_err_t ra8_ota_commit_swap_bank(ra8_ota_bank_t target)
{
  if ((target != k_ra8_ota_bank_a) && (target != k_ra8_ota_bank_b)) {
    return k_ra8_err_invalid_arg;
  }
  if (s_pending) {
    return k_ra8_err_invalid_state;
  }
#ifdef RA8_OFF_TARGET
  /* Host shadow: record the armed target so the argument-validation + single-shot
   * idempotency policy stays unit-testable without touching real flash. */
  s_pending_target = target;
  s_pending        = true;
  (void)s_tag;
  return k_ra8_ok;
#else
  /* FAIL-CLOSED on silicon (T5-10). Arming a swap means programming the boot
   * option region (an OFS3 / BTFLG option-byte write behind the PRCR unlock),
   * which is brick-risky and not yet wired -- it is bench-gated. Refuse with a
   * real error instead of a fake k_ra8_ok so a caller can never believe a swap
   * was armed when no flash work happened.
   * TODO(real OFS3/BTFLG boot-bank option-byte swap write -- bench-gated,
   * brick-risky): unlock PRCR, program the boot option byte via ra8_flash_*,
   * re-lock PRCR, confirm by read-back, then arm the shadow and return k_ra8_ok. */
  (void)s_tag;
  return k_ra8_err_not_supported;
#endif
}

ra8_err_t ra8_ota_commit_pending(ra8_ota_bank_t* out_target)
{
  RA8_CHECK_NULL_PTR(out_target, s_tag, "pending: out_target");
  if (!s_pending) {
    return k_ra8_err_no_data;
  }
  *out_target = s_pending_target;
  return k_ra8_ok;
}

/** @brief Implementation of `ra8_ota_commit_set_bank_config()` -- masks reserved
 *  bits, then host shadow under RA8_OFF_TARGET / fail-closed on silicon (the
 *  real option-region write is bench-gated and brick-risky -- T5-10). */
ra8_err_t ra8_ota_commit_set_bank_config(uint32_t raw_value)
{
  /* Mask reserved bits so an NS caller can only touch the bank-select field.
   * This is the value the real option-region write would persist. */
  const uint32_t masked = raw_value & (uint32_t)k_ra8_ota_bank_config_allowed;
#ifdef RA8_OFF_TARGET
  s_bank_config = masked;
  return k_ra8_ok;
#else
  /* FAIL-CLOSED on silicon (T5-10): persisting the masked value is the same
   * brick-risky, bench-gated option-region write as swap_bank, so refuse rather
   * than report a fake success.
   * TODO(real bank-config option-region write -- bench-gated, brick-risky):
   * unlock PRCR, program `masked` into the option region, re-lock, confirm. */
  (void)masked;
  return k_ra8_err_not_supported;
#endif
}

ra8_err_t ra8_ota_commit_get_bank_config(uint32_t* out_value)
{
  RA8_CHECK_NULL_PTR(out_value, s_tag, "get_bank_config: out_value");
  *out_value = s_bank_config;
  return k_ra8_ok;
}
