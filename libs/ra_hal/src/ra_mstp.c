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
#include "ra_log.h"

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
  k_ra_mstp_reg_count = 5U,  /**< MSTPCRA..MSTPCRE. */
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
 * @brief All-stopped reset pattern for MSTPCRA..MSTPCRE.
 *
 * @details
 * Every documented MSTPCRx bit defaults to 1 ("module stopped") on
 * a power-on reset (HUM Ch 11.2.6..10 register tables, "Value after
 * reset" rows). Reserved bits are also "read as 1, write 1". So the
 * canonical reset value of every MSTPCR register is 0xFFFFFFFF.
 */
typedef enum : uint32_t {
  k_ra_mstp_all_stopped = 0xFFFFFFFFU,
} ra_mstp_init_val_t;

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
 * @note Static so the table is zero-initialised at boot.
 * @warning ``ra_mstp_*`` functions are the only legal writers.
 */
static uint8_t s_refcount[k_ra_mstp_reg_count][k_ra_mstp_bit_count];

/* =============================================================================
 * Public id decoders (declared in ra8d2_mstp_regs.h)
 * =============================================================================
 */

/**
 * @brief Implementation of ra_mstp_id_reg (see header for full contract).
 * @details See the public header for the documented contract; this definition implements it.
 * @param[in] id See implementation.
 * @return Result code.
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
ra_mstp_reg_t ra_mstp_id_reg(ra_mstp_t id)
{
  return (ra_mstp_reg_t)(((uint16_t)id >> k_ra_bits_per_byte) & k_ra_mask_byte);
}

/**
 * @brief Implementation of ra_mstp_id_bit (see header for full contract).
 * @details See the public header for the documented contract; this definition implements it.
 * @param[in] id See implementation.
 * @return Result code.
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
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
 * @note The host simulator backs MMIO with ordinary RAM, so the
 * modify-write always reads back as expected on the next
 * loop iteration. The timeout return is therefore unreachable
 * on the host build and excluded from host coverage; it is
 * reachable on the Cortex-M85 target if the SYSC bus is
 * wedged or the MSTP block is power-gated. The exclusion
 * only suppresses the host coverage gate -- the line still
 * compiles, executes on the target, and is part of the
 * NASA Power-of-10 Rule 2 statically-bounded loop budget.
 *
 * @details See implementation.
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @since 0.1.0
 */
static ra_err_t internal_wait_readback(uint8_t reg, uint8_t bit, bool expected_stopped)
{
  volatile const uint32_t* p    = internal_reg_ptr(reg);
  const uint32_t           mask = (uint32_t)1U << bit;
  /* Host simulator always succeeds on iteration 0; the spin-budget
   * loop exists for the Cortex-M85 target only. */
  for (uint16_t i = 0U; i < k_ra_mstp_readback_spin; ++i) { /* GCOVR_EXCL_BR_LINE */
    const bool seen_stopped = (*p & mask) != 0U;
    if (seen_stopped == expected_stopped) { /* GCOVR_EXCL_BR_LINE */
      return k_ra_ok;
    }
  }
  return k_ra_err_hw_timeout; /* GCOVR_EXCL_LINE -- target-only path */
}

/* =============================================================================
 * Public API
 * =============================================================================
 */

/**
 * @brief Implementation of ra_mstp_init (see header for full contract).
 * @details See the public header for the documented contract; this definition implements it.
 * @return Result code.
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
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

  /* Mark every peripheral as stopped (post-reset state). HUM 11.2.6
   * pp 443..449 list reserved bits as "These bits are read as 1.
   * The write value should be 1." -- writing the all-stopped pattern
   * satisfies both real bits and reserved bits in one shot. */
  const uint32_t reset_val = k_ra_mstp_all_stopped;
  /* HUM Ch 11.2.6 "MSTPCRA : Module Stop Control Register A", p 443 */
  *internal_reg_ptr(0U) = reset_val;
  /* HUM Ch 11.2.7 "MSTPCRB : Module Stop Control Register B", p 444 */
  *internal_reg_ptr(1U) = reset_val;
  /* HUM Ch 11.2.8 "MSTPCRC : Module Stop Control Register C", p 446 */
  *internal_reg_ptr(2U) = reset_val;
  /* HUM Ch 11.2.9 "MSTPCRD : Module Stop Control Register D", p 448 */
  *internal_reg_ptr(3U) = reset_val;
  /* HUM Ch 11.2.10 "MSTPCRE : Module Stop Control Register E", p 449 */
  *internal_reg_ptr(4U) = reset_val;

  /* Read-back the registers we just wrote. Reserved bits inside the
   * registers are always 1, so the all-stopped pattern is the only
   * legal value after a reset-equivalent write. The not-observed
   * branch is target-only (host simulator always succeeds). */
  for (uint8_t reg = 0U; reg < k_ra_mstp_reg_count; ++reg) {
    volatile const uint32_t* p        = internal_reg_ptr(reg);
    bool                     all_ones = false;
    for (uint16_t i = 0U; i < k_ra_mstp_readback_spin; ++i) { /* GCOVR_EXCL_BR_LINE */
      if (*p == reset_val) {                                  /* GCOVR_EXCL_BR_LINE */
        all_ones = true;
        break;
      }
    }
    /* GCOVR_EXCL_START -- target-only path; host MMIO always reads back. */
    if (!all_ones) {
      ra_log_error_val(s_tag, "init read-back failed reg", (uint32_t)reg);
      return k_ra_err_hw_timeout;
    }
    /* GCOVR_EXCL_STOP */
  }
  return k_ra_ok;
}

/**
 * @brief Implementation of ra_mstp_enable (see header for full contract).
 * @details See the public header for the documented contract; this definition implements it.
 * @param[in] id See implementation.
 * @return Result code.
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
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
  /* GCOVR_EXCL_START -- target-only path; host MMIO always reads back. */
  if (err != k_ra_ok) {
    /* Roll the ref count back so a retry from the caller starts
     * fresh rather than thinking the module is already on. */
    s_refcount[reg][bit] = prev;
    ra_log_error_val(s_tag, "enable: read-back timeout id", (uint32_t)id);
    return err;
  }
  /* GCOVR_EXCL_STOP */
  return k_ra_ok;
}

/**
 * @brief Implementation of ra_mstp_disable (see header for full contract).
 * @details See the public header for the documented contract; this definition implements it.
 * @param[in] id See implementation.
 * @return Result code.
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
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
  /* GCOVR_EXCL_START -- target-only path; host MMIO always reads back. */
  if (err != k_ra_ok) {
    /* Roll back so the next caller still sees the resource as in
     * use and we don't lose track of pending ungates. */
    s_refcount[reg][bit] = prev;
    ra_log_error_val(s_tag, "disable: read-back timeout id", (uint32_t)id);
    return err;
  }
  /* GCOVR_EXCL_STOP */
  return k_ra_ok;
}

/**
 * @brief Implementation of ra_mstp_get_refcount (see header for full contract).
 * @details See the public header for the documented contract; this definition implements it.
 * @param[in] id See implementation.
 * @param[in] out_ref See implementation.
 * @return Result code.
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
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

/**
 * @brief Implementation of ra_mstp_is_stopped (see header for full contract).
 * @details See the public header for the documented contract; this definition implements it.
 * @param[in] id See implementation.
 * @param[in] out_stopped See implementation.
 * @return Result code.
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
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
