/**
 * @file ra_mstp.c
 * @brief Ref-counted Module Stop Control implementation
 *
 * @par Tag
 * [Ring 3 / HAL] {World: S}
 *
 * @details
 * substrate. Wraps the RA8D2 ``MSTPCRA..MSTPCRE`` registers
 * (HUM Ch 11.2.6..10, p 443..450) behind a small ref-counted API
 * so two unrelated drivers can both depend on the same MSTP bit
 * (DMAC + DTC, OSPI + DOTF, SSIE0/SSIE1, ...) without trampling
 * each other's state.
 *
 * The ref-count table lives entirely in this file -- no other
 * compilation unit may write to ``MSTPCRA..MSTPCRE``.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_mstp.h"

#include <stdint.h>

#include "ra8d2_mstp_regs.h"
#include "ra_bit_constants.h"
#include "ra_check.h"
#include "ra_err.h"
#include "ra_hw_err.h"
#include "ra_log.h"
#include "ra_mstp_internal.h"

/* =============================================================================
 * Local constants
 * =============================================================================
 */

static const char* s_tag = "MSTP";

/**
 * @enum ra_mstp_dim_t
 * @brief Dimensions of the ref-count table.
 */
typedef enum : uint8_t {
  k_ra_mstp_reg_count = 5U,  /**< MSTPCRA..MSTPCRE.     */
  k_ra_mstp_bit_count = 32U, /**< 32 bits per register. */
} ra_mstp_dim_t;

/**
 * @enum ra_mstp_spin_t
 * @brief Bounded spin budget for the read-back protocol (HUM 11.2.6 N2).
 *
 * @details
 * The MSTP bit is expected to update on the next bus cycle, so the
 * realistic worst case is single digits. The budget below is the
 * 16-bit ceiling we use to keep an unconditionally-bounded loop
 * (NASA Power of 10 Rule 2).
 */
typedef enum : uint16_t {
  k_ra_mstp_readback_spin = 0x100U,
} ra_mstp_spin_t;

/**
 * @enum ra_mstp_init_val_t
 * @brief Per-register safe all-stopped patterns for MSTPCRA..MSTPCRE.
 *
 * @details
 * Most MSTPCRx bits default to 1 ("module stopped") on a power-on
 * reset. Reserved bits are "read as 1, write 1" (HUM Ch 11.2.6..10).
 *
 * Exception -- MSTPCRA bits 0..3 (SRAM0..3): these control the ECC
 * SRAM controllers that back the CPU stack and data. Stopping them
 * (writing 1) while code is running causes a precise BusFault on the
 * next stack access. They must be kept at 0 (running) at all times.
 * HUM Ch 11.2.6 p 443, MSTPA0..MSTPA3.
 */
typedef enum : uint32_t {
  k_ra_mstp_all_stopped    = 0xFFFFFFFFU,
  k_ra_mstp_safe_stopped_a = 0xFFFFFFF0U, /**< MSTPCRA: bits 0-3 (SRAM0-3) kept 0. */
} ra_mstp_init_val_t;

/**
 * @enum ra_mstp_psar_t
 * @brief R_PSCU Peripheral Security Attribution Register addresses (PSARB..E).
 *
 * @details
 * A module-stop bit follows its peripheral's PSAR attribution: once the Secure
 * side marks a peripheral Non-secure (PSARx bit = 1), that MSTPCRx bit is owned
 * by the Non-secure alias and a Secure write to it is masked (the bit does not
 * change). PSAR bit N mirrors MSTPCRx bit N one-for-one, so the PSAR value is
 * directly the Non-secure-owned bit mask for that register. R_PSCU begins with
 * a reserved word (no PSARA), so MSTPCRA has no attribution register and its
 * whole width stays Secure-owned. On a non-TrustZone system every PSAR reads 0
 * (all Secure), so the mask is empty and the read-back stays fully strict.
 * HUM Ch 3.2 "PSCU" (R_PSCU at 0x40204000; PSARB at +0x04).
 */
typedef enum : uintptr_t {
  k_ra_mstp_psarb_addr = 0x40204004U, /**< PSARB: MSTPCRB attribution. */
  k_ra_mstp_psarc_addr = 0x40204008U, /**< PSARC: MSTPCRC attribution. */
  k_ra_mstp_psard_addr = 0x4020400CU, /**< PSARD: MSTPCRD attribution. */
  k_ra_mstp_psare_addr = 0x40204010U, /**< PSARE: MSTPCRE attribution. */
} ra_mstp_psar_t;

/* =============================================================================
 * State
 * =============================================================================
 */

/**
 * @var s_refcount
 * @brief Per-(register, bit) usage counter.
 *
 * @details
 * 5 x 32 = 160 entries, each ``uint8_t``. Indexed by
 * ``s_refcount[reg][bit]`` where ``reg`` is 0..4 (MSTPCRA..E) and
 * ``bit`` is 0..31. A value of 0 means "no driver currently needs
 * this peripheral"; the corresponding MSTPCR bit should be 1.
 *
 * @note Static so the table is zero-initialized at boot.
 * @warning ``ra_mstp_*`` functions are the only legal writers.
 */
static uint8_t s_refcount[k_ra_mstp_reg_count][k_ra_mstp_bit_count];

/* =============================================================================
 * Public id decoders (declared in ra8d2_mstp_regs.h)
 * =============================================================================
 */

ra_mstp_reg_t ra_mstp_id_reg(ra_mstp_t id)
{
  return (ra_mstp_reg_t)(((uint16_t)id >> k_ra_bits_per_byte) & k_ra_mask_byte);
}

uint8_t ra_mstp_id_bit(ra_mstp_t id)
{
  return (uint8_t)((uint16_t)id & k_ra_mask_byte);
}

/* =============================================================================
 * Internal helpers
 * =============================================================================
 */

/**
 * @brief Decode an ``ra_mstp_t`` id and validate its bit position.
 *
 * @param[in] id Packed id from the ``ra_mstp_t`` enum.
 * @param[out] out_reg Register index 0..4 on success.
 * @param[out] out_bit Bit position 0..31 on success.
 *
 * @return ``true`` if both fields decode to in-range values.
 *
 * @details See implementation.
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static bool internal_decode(ra_mstp_t id, uint8_t* out_reg, uint8_t* out_bit)
{
  const ra_mstp_reg_t reg_id = ra_mstp_id_reg(id);
  const uint8_t       reg    = (uint8_t)reg_id;
  const uint8_t       bit    = ra_mstp_id_bit(id);
  if (reg >= k_ra_mstp_reg_count) {
    return false;
  }
  if (bit >= k_ra_mstp_bit_count) {
    return false;
  }
  *out_reg = reg;
  *out_bit = bit;
  return true;
}

/**
 * @brief Pointer to MSTPCRA..MSTPCRE indexed by register number.
 *
 * @details
 * The five MSTPCR registers sit at consecutive 4-byte offsets in
 * the ``r_mstp_regs_t`` block, so a single base pointer plus
 * ``[reg]`` indexing is unambiguous and matches the layout asserted
 * by the static_assert in ``ra8d2_mstp_regs.h``.
 */
static volatile uint32_t* internal_reg_ptr(uint8_t reg)
{
  return &(&ra_mstp()->MSTPCRA)[reg];
}

/**
 * @brief Read-back protocol from HUM 11.2.6 Note 2.
 *
 * @param[in] reg MSTPCR register index 0..4.
 * @param[in] bit Bit position 0..31.
 * @param[in] expected_stopped True if the bit must read back as 1.
 *
 * @return ``k_ra_ok`` on observation, ``k_ra_err_hw_timeout`` on
 * spin-budget exhaustion.
 *
 * @note On the Cortex-M85 target the timeout fires if the SYSC bus is
 * wedged or the MSTP block is power-gated. The host simulator backs
 * MMIO with ordinary RAM, so the raw readback matches immediately;
 * the loop-exit decision is therefore routed through the ra_sim_mmio
 * fault seam keyed on the polled MSTPCR register, letting a test
 * drive the retry and timeout legs deterministically (T1-01).
 *
 * @details Bounded poll of one MSTPCR bit until it reads back at the
 * commanded gate state (HUM 11.2.6 Note 2), part of the NASA
 * Power-of-10 Rule 2 statically-bounded loop budget.
 * @retval k_ra_ok Operation succeeded.
 * @retval k_ra_err_hw_timeout The bit never settled within the budget.
 * @pre ``reg`` < ::k_ra_mstp_reg_count and ``bit`` < 32.
 * @pre The MSTPCR write being confirmed has already been issued.
 * @post On k_ra_ok the bit reads back at ``expected_stopped``.
 * @post No register is modified by this function.
 * @since 0.1.0
 */
static ra_err_t internal_wait_readback(uint8_t reg, uint8_t bit, bool expected_stopped)
{
  volatile const uint32_t* p    = internal_reg_ptr(reg);
  const uint32_t           mask = (uint32_t)1U << bit;
  for (uint16_t i = 0U; i < k_ra_mstp_readback_spin; ++i) {
    const bool seen_stopped = (*p & mask) != 0U;
    const bool settled      = (seen_stopped == expected_stopped);
#if defined(RA_SIMULATOR_MODE) && defined(UNIT_TEST)
    /* Host MMIO fault seam, keyed on the polled MSTPCR register. */
    if (ra_sim_mmio_wait_eval(p, (uint32_t)i, settled)) {
      return k_ra_ok;
    }
#else
    if (settled) {
      return k_ra_ok;
    }
#endif
  }
  return k_ra_err_hw_timeout;
}

/**
 * @brief Poll one MSTPCR register until it reads back the written value.
 *
 * @details Bounded whole-register settle poll used by ::ra_mstp_init after
 * its five MSTPCR writes (HUM Ch 11.2.6..10 read-back requirement).
 * Reserved bits read 1 in both the write pattern and the read-back, so a
 * settled register compares equal to the written value exactly. On the
 * host unit-test build the loop-exit decision is routed through the
 * ra_sim_mmio fault seam keyed on the polled MSTPCR register, so a test
 * can drive the not-yet-settled branch and the timeout leg (T1-01).
 *
 * @param[in] reg    MSTPCR index (`< k_ra_mstp_reg_count`).
 * @param[in] expect Value the register must read back.
 *
 * @return ra_err_t Status code.
 * @retval k_ra_ok             The register settled to @p expect.
 * @retval k_ra_err_hw_timeout It never settled within the spin budget.
 *
 * @pre ``reg`` < ::k_ra_mstp_reg_count.
 * @pre The MSTPCR write being confirmed has already been issued.
 * @post On k_ra_ok the register reads back exactly @p expect.
 * @post No register is modified by this function.
 * @note Not thread-safe; init-time single-threaded context only.
 * @since 0.1.0
 */
ra_err_t ra_mstp_wait_reg_settle_internal(uint8_t reg, uint32_t expect, uint32_t care_mask)
{
  volatile const uint32_t* p = internal_reg_ptr(reg);
  for (uint16_t i = 0U; i < k_ra_mstp_readback_spin; ++i) {
    const bool settled = ((*p & care_mask) == (expect & care_mask));
#if defined(RA_SIMULATOR_MODE) && defined(UNIT_TEST)
    /* Host MMIO fault seam, keyed on the polled MSTPCR register. */
    if (ra_sim_mmio_wait_eval(p, (uint32_t)i, settled)) {
      return k_ra_ok;
    }
#else
    if (settled) {
      return k_ra_ok;
    }
#endif
  }
  return k_ra_err_hw_timeout;
}

/**
 * @brief Non-secure-owned bit mask for one MSTPCR register (read from PSAR).
 *
 * @details
 * Returns the bits of MSTPCR[@p reg] that are Non-secure-attributed and thus
 * NOT Secure-writable: a Secure module-stop write to such a bit is masked by
 * hardware and the bit will not read back the written value. The mask is the
 * corresponding PSAR register value (PSAR bit N mirrors MSTPCR bit N). MSTPCRA
 * (@p reg 0) has no PSAR (R_PSCU starts with a reserved word), so it is always
 * fully Secure-owned and returns 0. On a non-TrustZone system every PSAR reads
 * 0, so this returns 0 and the read-back stays strict for every register.
 *
 * @param[in] reg MSTPCR index (`< k_ra_mstp_reg_count`).
 *
 * @return The Non-secure-owned bit mask for that register (0 for MSTPCRA and
 *         on any non-TrustZone system).
 *
 * @pre ``reg`` < ::k_ra_mstp_reg_count.
 * @pre Runs in Secure / privileged context (R_PSCU is Secure-only).
 * @post No register is modified.
 * @post MSTPCRA always yields 0 (no attribution register).
 *
 * @note Not thread-safe; init-time single-threaded context only.
 * @since 0.1.0
 */
uint32_t ra_mstp_ns_mask_internal(uint8_t reg)
{
  /* MSTPCRA has no PSAR: always fully Secure-owned. */
  static const uintptr_t k_psar_addr[k_ra_mstp_reg_count] = {
    0U,
    (uintptr_t)k_ra_mstp_psarb_addr,
    (uintptr_t)k_ra_mstp_psarc_addr,
    (uintptr_t)k_ra_mstp_psard_addr,
    (uintptr_t)k_ra_mstp_psare_addr,
  };
  if (k_psar_addr[reg] == 0U) {
    return 0U;
  }
  /* HUM Ch 3.2 "PSARB..PSARE" -- Non-secure-attributed peripheral bits. */
  return *(volatile const uint32_t*)k_psar_addr[reg];
}

/* =============================================================================
 * Public API
 * =============================================================================
 */

ra_err_t ra_mstp_init(void)
{
  ra_log_info(s_tag, "ra_mstp_init -- gating all modules");

  /* Reset every ref count. */
  for (uint8_t reg = 0U; reg < k_ra_mstp_reg_count; ++reg) {
    for (uint8_t bit = 0U; bit < k_ra_mstp_bit_count; ++bit) {
      s_refcount[reg][bit] = 0U;
    }
  }

  /* Stop all peripherals. MSTPCRA bits 0..3 (SRAM0..3) must stay 0
   * (running) or the CPU stack becomes inaccessible and causes a
   * BusFault. All other registers use the full all-stopped pattern.
   * HUM Ch 11.2.6..10 p 443..449. */
  static const uint32_t k_init_vals[k_ra_mstp_reg_count] = {
    (uint32_t)k_ra_mstp_safe_stopped_a, /* MSTPCRA: SRAM0-3 kept running */
    (uint32_t)k_ra_mstp_all_stopped,    /* MSTPCRB                       */
    (uint32_t)k_ra_mstp_all_stopped,    /* MSTPCRC                       */
    (uint32_t)k_ra_mstp_all_stopped,    /* MSTPCRD                       */
    (uint32_t)k_ra_mstp_all_stopped,    /* MSTPCRE                       */
  };
  /* HUM Ch 11.2.6 "MSTPCRA : Module Stop Control Register A", p 443 */
  *internal_reg_ptr(0U) = k_init_vals[0U];
  /* HUM Ch 11.2.7 "MSTPCRB : Module Stop Control Register B", p 444 */
  *internal_reg_ptr(1U) = k_init_vals[1U];
  /* HUM Ch 11.2.8 "MSTPCRC : Module Stop Control Register C", p 446 */
  *internal_reg_ptr(2U) = k_init_vals[2U];
  /* HUM Ch 11.2.9 "MSTPCRD : Module Stop Control Register D", p 448 */
  *internal_reg_ptr(3U) = k_init_vals[3U];
  /* HUM Ch 11.2.10 "MSTPCRE : Module Stop Control Register E", p 449 */
  *internal_reg_ptr(4U) = k_init_vals[4U];

  /* Read-back: each register must settle to the value written, but only for
   * the bits the Secure side actually owns. A Non-secure-attributed module
   * (PSAR bit set -- e.g. a USB controller delegated to a Non-secure world)
   * ignores the Secure module-stop write, so its bit will not read back the
   * commanded state; requiring it would spuriously fail boot on every
   * TrustZone system that delegates a peripheral. Masking the read-back by the
   * Secure-owned bits keeps a genuine stuck Secure module a hard failure while
   * tolerating the by-design Non-secure ones. On a non-TrustZone system the
   * mask is all-ones, so the check stays fully strict. */
  for (uint8_t reg = 0U; reg < k_ra_mstp_reg_count; ++reg) {
    const uint32_t care = ~ra_mstp_ns_mask_internal(reg);
    if (ra_mstp_wait_reg_settle_internal(reg, k_init_vals[reg], care) != k_ra_ok) {
      ra_log_error_val(s_tag, "init read-back failed reg", (uint32_t)reg);
      return k_ra_err_hw_timeout;
    }
  }
  return k_ra_ok;
}

ra_err_t ra_mstp_enable(ra_mstp_t id)
{
  uint8_t reg = 0U;
  uint8_t bit = 0U;
  if (!internal_decode(id, &reg, &bit)) {
    ra_log_error_val(s_tag, "enable: invalid id", (uint32_t)id);
    return k_ra_err_invalid_arg;
  }

  const uint8_t prev = s_refcount[reg][bit];
  if (prev == UINT8_MAX) {
    /* Defensive: 255 simultaneous users would mean a bug elsewhere. */
    ra_log_error_val(s_tag, "enable: refcount saturated", (uint32_t)id);
    return k_ra_err_invalid_state;
  }
  s_refcount[reg][bit] = (uint8_t)(prev + 1U);

  if (prev != 0U) {
    /* Already running. Nothing else to do. */
    return k_ra_ok;
  }

  /* HUM Ch 11.2.6 "MSTPCRA : Module Stop Control Register A", p 443
   * -- bit clear == ungate the peripheral. */
  volatile uint32_t* p    = internal_reg_ptr(reg);
  const uint32_t     mask = (uint32_t)1U << bit;
  *p                      = *p & ~mask;

  const ra_err_t err = internal_wait_readback(reg, bit, false);
  if (err != k_ra_ok) {
    /* Roll the ref count back so a retry from the caller starts
     * fresh rather than thinking the module is already on. */
    s_refcount[reg][bit] = prev;
    ra_log_error_val(s_tag, "enable: read-back timeout id", (uint32_t)id);
    return err;
  }
  return k_ra_ok;
}

ra_err_t ra_mstp_disable(ra_mstp_t id)
{
  uint8_t reg = 0U;
  uint8_t bit = 0U;
  if (!internal_decode(id, &reg, &bit)) {
    ra_log_error_val(s_tag, "disable: invalid id", (uint32_t)id);
    return k_ra_err_invalid_arg;
  }

  const uint8_t prev = s_refcount[reg][bit];
  if (prev == 0U) {
    ra_log_error_val(s_tag, "disable: refcount underflow", (uint32_t)id);
    return k_ra_err_invalid_state;
  }
  s_refcount[reg][bit] = (uint8_t)(prev - 1U);

  if (prev != 1U) {
    /* Other users still active. Leave the bit cleared. */
    return k_ra_ok;
  }

  /* HUM Ch 11.2.6 "MSTPCRA : Module Stop Control Register A", p 443
   * -- bit set == gate the peripheral. */
  volatile uint32_t* p    = internal_reg_ptr(reg);
  const uint32_t     mask = (uint32_t)1U << bit;
  *p                      = *p | mask;

  const ra_err_t err = internal_wait_readback(reg, bit, true);
  if (err != k_ra_ok) {
    /* Roll back so the next caller still sees the resource as in
     * use and we don't lose track of pending ungates. */
    s_refcount[reg][bit] = prev;
    ra_log_error_val(s_tag, "disable: read-back timeout id", (uint32_t)id);
    return err;
  }
  return k_ra_ok;
}

ra_err_t ra_mstp_get_refcount(ra_mstp_t id, uint8_t* out_ref)
{
  RA_CHECK_NULL_PTR(out_ref, s_tag, "get_refcount: out_ref");
  uint8_t reg = 0U;
  uint8_t bit = 0U;
  if (!internal_decode(id, &reg, &bit)) {
    return k_ra_err_invalid_arg;
  }
  *out_ref = s_refcount[reg][bit];
  return k_ra_ok;
}

ra_err_t ra_mstp_is_stopped(ra_mstp_t id, bool* out_stopped)
{
  RA_CHECK_NULL_PTR(out_stopped, s_tag, "is_stopped: out_stopped");
  uint8_t reg = 0U;
  uint8_t bit = 0U;
  if (!internal_decode(id, &reg, &bit)) {
    return k_ra_err_invalid_arg;
  }
  volatile const uint32_t* p    = internal_reg_ptr(reg);
  const uint32_t           mask = (uint32_t)1U << bit;
  *out_stopped                  = ((*p & mask) != 0U);
  return k_ra_ok;
}
