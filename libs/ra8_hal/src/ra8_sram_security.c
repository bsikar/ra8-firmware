/**
 * @file ra8_sram_security.c
 * @brief SRAM HAL: TrustZone security attribution + ECC error callbacks
 *
 * @par Tag
 * [Ring 3 / HAL] {World: S}
 *
 * @details
 * Split out of ``ra8_sram.c`` (file-size cap). Owns:
 *
 *  - CPSCU ``SRAMSAR`` / ``SRAMESAR`` / ``SRAMSABARn`` security
 *    attribution setters (HUM Ch 58.2.1..58.2.3)
 *  - the registered global + per-bank ECC error callbacks and the
 *    ``ra8_sram_dispatch`` / ``ra8_sram_dispatch_from_esr`` fan-out that
 *    walks all eight SRAMESR flags (HUM Ch 58.2.12 p 3535)
 *
 * The callback state defined here (``g_sram_on_error*``) is also referenced
 * by ``ra8_sram_deinit`` in ``ra8_sram.c`` via the ``extern`` declarations
 * in ``ra8_sram_internal.h`` so teardown can drop every registration.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_log.h"
#include "ra8_sram.h"
#include "ra8_sram_internal.h"
#include "ra8_sram_regs.h"

/* =============================================================================
 * Constants
 * =============================================================================
 */

/** @brief Module log tag. */
static const char* s_tag = "SRAM";

/* =============================================================================
 * Module state
 * =============================================================================
 */

/** @brief Registered global ECC error callback (NULL until attach). */
ra8_sram_error_fn_t g_sram_on_error = nullptr;

/** @brief Caller context forwarded to ``g_sram_on_error``. */
void* g_sram_on_error_ctx = nullptr;

/** @brief Per-bank ECC error callback (NULL until attach). */
ra8_sram_error_fn_t g_sram_on_error_bank[k_ra8_sram_bank_count] = {
  nullptr,
  nullptr,
  nullptr,
  nullptr,
};

/** @brief Per-bank context forwarded to ``g_sram_on_error_bank``. */
void* g_sram_on_error_bank_ctx[k_ra8_sram_bank_count] = {
  nullptr,
  nullptr,
  nullptr,
  nullptr,
};

/* =============================================================================
 * TrustZone security attribution
 * =============================================================================
 */

[[nodiscard]] ra8_err_t ra8_sram_set_security(uint32_t sa_mask)
{
  if ((sa_mask & ~k_ra8_sram_sar_writable) != 0U) {
    return k_ra8_err_invalid_arg;
  }
  volatile r_sram_cpscu_regs_t* cpscu = ra8_sram_cpscu_regs();
  /* HUM Ch 58.2.2 "SRAMSAR : SRAM Security Attribution Register",
   * p 3528. */
  cpscu->SRAMSAR = sa_mask;
  return k_ra8_ok;
}

[[nodiscard]] ra8_err_t ra8_sram_set_ecc_security(bool non_secure)
{
  volatile r_sram_cpscu_regs_t* cpscu = ra8_sram_cpscu_regs();
  /* HUM Ch 58.2.3 "SRAMESAR : SRAM ECC region Security Attribute
   * Register", p 3529. */
  uint32_t value = 0U;
  if (non_secure) {
    value = k_ra8_sram_esar_bit_esa;
  }
  cpscu->SRAMESAR = value;
  return k_ra8_ok;
}

[[nodiscard]] ra8_err_t ra8_sram_set_boundary(uint8_t bank, uint32_t offset)
{
  if ((uint16_t)bank >= (uint16_t)k_ra8_sram_bank_count) {
    return k_ra8_err_invalid_arg;
  }
  if ((offset & k_ra8_sram_sabar_align_mask) != 0U) {
    return k_ra8_err_invalid_arg;
  }
  volatile r_sram_cpscu_regs_t* cpscu = ra8_sram_cpscu_regs();
  /* HUM Ch 58.2.1 "SRAMSABARn : SRAM Security Attribute Boundary
   * Address Register", p 3527 -- write the absolute Secure offset. */
  cpscu->SRAMSABAR[bank] = offset;
  return k_ra8_ok;
}

/* =============================================================================
 * Error callback path
 * =============================================================================
 */

[[nodiscard]] ra8_err_t ra8_sram_attach_handler(ra8_sram_error_fn_t fn, void* ctx)
{
  RA8_CHECK_NULL_PTR(fn, s_tag, "fn must not be nullptr");
  g_sram_on_error     = fn;
  g_sram_on_error_ctx = ctx;
  return k_ra8_ok;
}

[[nodiscard]] ra8_err_t
ra8_sram_attach_bank_handler(uint8_t bank, ra8_sram_error_fn_t fn, void* ctx)
{
  RA8_CHECK_NULL_PTR(fn, s_tag, "fn must not be nullptr");
  if ((uint16_t)bank >= (uint16_t)k_ra8_sram_bank_count) {
    return k_ra8_err_invalid_arg;
  }
  g_sram_on_error_bank[bank]     = fn;
  g_sram_on_error_bank_ctx[bank] = ctx;
  return k_ra8_ok;
}

void ra8_sram_dispatch(uint8_t bank, bool is_2bit, uintptr_t err_addr)
{
  if ((uint16_t)bank >= (uint16_t)k_ra8_sram_bank_count) {
    return;
  }
  const ra8_sram_error_fn_t global_fn  = g_sram_on_error;
  void* const               global_ctx = g_sram_on_error_ctx;
  if (global_fn != nullptr) {
    global_fn(global_ctx, bank, is_2bit, err_addr);
  }
  const ra8_sram_error_fn_t bank_fn  = g_sram_on_error_bank[bank];
  void* const               bank_ctx = g_sram_on_error_bank_ctx[bank];
  if (bank_fn != nullptr) {
    bank_fn(bank_ctx, bank, is_2bit, err_addr);
  }
}

uint16_t ra8_sram_dispatch_from_esr(ra8_sram_status_t* out_status)
{
  ra8_sram_status_t local = {};
  /* HUM Ch 58.2.12 "SRAMESR : SRAM Error Status Register For ECC
   * RAM" p 3535-3537 -- read SRAMESR + EAR via the helper. */
  const ra8_err_t err = ra8_sram_get_status(&local);
  if (err != k_ra8_ok) {
    return 0U;
  }
  if (out_status != nullptr) {
    *out_status = local;
  }

  uint16_t fired = 0U;
  for (uint8_t bank = 0U; bank < k_ra8_sram_bank_count; ++bank) {
    const uint16_t one_bit_pos = (uint16_t)((uint16_t)2U * (uint16_t)bank);
    const uint16_t two_bit_pos = (uint16_t)(one_bit_pos + 1U);
    const uint16_t one_bit_msk = (uint16_t)((uint16_t)1U << one_bit_pos);
    const uint16_t two_bit_msk = (uint16_t)((uint16_t)1U << two_bit_pos);

    if ((local.raw_esr & one_bit_msk) != 0U) {
      ra8_sram_dispatch(bank, false, local.addr_1bit[bank]);
      fired = (uint16_t)(fired | one_bit_msk);
    }
    if ((local.raw_esr & two_bit_msk) != 0U) {
      ra8_sram_dispatch(bank, true, local.addr_2bit[bank]);
      fired = (uint16_t)(fired | two_bit_msk);
    }
  }
  return fired;
}
