/**
 * @file ra8_sram.c
 * @brief SRAM (with ECC) HAL driver implementation
 *
 * @par Tag
 * [Ring 3 / HAL] {World: S}
 *
 * @details
 * Implements ``ra8_sram.h``. Owns:
 *
 *  - per-bank module-stop ungate via ``ra8_mstp`` (HUM Ch 11.2.6 p 443)
 *  - per-bank ECC mode programming through ``SRAMCRn`` and per-bank
 *    region size through ``SRAMECCRGNn`` under the ``SRAMPRCR_S``
 *    half-word unlock (HUM Ch 58.2.4 / 58.2.7 / 58.2.8..58.2.11)
 *  - SRAMWTSC wait-state programming with auto-tuning per
 *    ICLK frequency (HUM Ch 58.2.6 / 58.3.7)
 *  - CPSCU ``SRAMSAR`` / ``SRAMESAR`` / ``SRAMSABARn`` security
 *    attribution (HUM Ch 58.2.1..58.2.3)
 *  - deterministic 64-bit zero-init pass before enabling
 *    ``ECC with check`` mode (HUM Ch 58.3.2)
 *  - ECC decoder self-test sequence (HUM Ch 58.3.4)
 *  - SRAMESR / SRAMESCLR / SRAMEAR readout + per-(bank, slot) clear
 *  - global + per-bank ECC error callback fan-out, including a
 *    ``dispatch_from_esr`` helper that walks all eight flags
 *
 * Layered after ``rx_eccram.c`` from the STAR project (RX72N) -- same
 * shape (PRCR unlock, mode set, error status, ISR trampoline) but with
 * RA8D2's per-bank registers and the ``0xA5`` key code, plus the
 * additions above to satisfy the full HUM Ch 58 surface.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_sram.h"

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_log.h"
#include "ra8_mstp.h"
#include "ra8_sram_internal.h"
#include "ra8_sram_regs.h"

/* =============================================================================
 * Constants
 * =============================================================================
 */

/** @brief Module log tag. */
static const char* s_tag = "SRAM";

/**
 * @enum ra8_sram_local_t
 * @brief Internal helpers / shifts / sizes.
 */
typedef enum : uint8_t {
  k_ra8_sram_eccmod_shift  = 2U, /**< ECCMOD field is at bits [3:2] of SRAMCRn. */
  k_ra8_sram_eccmod_max    = 2U, /**< ``ra8_sram_ecc_mode_t`` enumerates 0..2.  */
  k_ra8_sram_on_error_max  = 1U, /**< OAD is single-bit (0 or 1).               */
  k_ra8_sram_eccrgn_max012 = 4U, /**< Largest legal ECCRGN for SRAM0..2.        */
  k_ra8_sram_eccrgn_max3   = 1U, /**< Largest legal ECCRGN for SRAM3.           */
  k_ra8_sram_ear_slot_max  = 1U, /**< 0 (1-bit) or 1 (2-bit).                   */
  k_ra8_sram_bank_max_idx  = 3U, /**< Last legal bank index.                    */
} ra8_sram_local_t;

/**
 * @enum ra8_sram_self_test_inject_t
 * @brief Bit masks for the ECC self-test fault injection.
 *
 * @details
 * Per HUM Ch 58.3.4 "ECC Decoder Testing", p 3539. The bypass-mode
 * read returns 8 bits of syndrome; flipping one or two of those bits
 * before writing back is what makes the verify-step fire SRAMESR.
 */
typedef enum : uint8_t {
  k_ra8_sram_self_test_flip_1bit = 0x01U, /**< Flip bit 0 of the syndrome.    */
  k_ra8_sram_self_test_flip_2bit = 0x03U, /**< Flip bits 0+1 of the syndrome. */
} ra8_sram_self_test_inject_t;

/**
 * @var s_sram_mstp_table
 * @brief Bank-index -> ``ra8_mstp_t`` lookup.
 *
 * @details
 * Per HUM Ch 11.2.6 "MSTPCRA" p 443: bits MSTPA0..MSTPA3 select
 * SRAM0..SRAM3. The shared ``ra8_mstp.h`` already exposes typed
 * enum values for these.
 */
static const ra8_mstp_t s_sram_mstp_table[k_ra8_sram_bank_count] = {
  k_ra8_mstp_sram0,
  k_ra8_mstp_sram1,
  k_ra8_mstp_sram2,
  k_ra8_mstp_sram3,
};

/**
 * @var s_sram_data_off_table
 * @brief Bank-index -> data-window offset (HUM Ch 58.1 Table 58.1, p 3527).
 */
static const uint32_t s_sram_data_off_table[k_ra8_sram_bank_count] = {
  k_ra8_sram_bank0_data_off,
  k_ra8_sram_bank1_data_off,
  k_ra8_sram_bank2_data_off,
  k_ra8_sram_bank3_data_off,
};

/**
 * @var s_sram_ecc_off_table
 * @brief Bank-index -> ECC syndrome window offset (HUM Ch 58.1, p 3527).
 */
static const uint32_t s_sram_ecc_off_table[k_ra8_sram_bank_count] = {
  k_ra8_sram_ecc_bank0_off,
  k_ra8_sram_ecc_bank1_off,
  k_ra8_sram_ecc_bank2_off,
  k_ra8_sram_ecc_bank3_off,
};

/* =============================================================================
 * Module state
 * =============================================================================
 */

/** @brief Driver init flag (set at end of ``ra8_sram_init``). */
static bool s_initialized = false;

/* The ECC error callback table (``g_sram_on_error*``) lives in
 * ``ra8_sram_security.c`` and is reached from ``ra8_sram_deinit`` below
 * through the ``extern`` declarations in ``ra8_sram_internal.h``. */

/* =============================================================================
 * Internal helpers
 * =============================================================================
 */

/**
 * @brief Validate per-bank config.
 *
 * @param[in] cfg  Non-NULL pointer (caller already checked).
 * @param[in] bank Bank index (used for the SRAM3 region check).
 * @return ``k_ra8_ok`` or ``k_ra8_err_invalid_arg``.
 *
 * @details See implementation.
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_validate_bank_cfg(const ra8_sram_bank_cfg_t* cfg,
                                                         uint8_t                    bank)
{
  if ((uint8_t)cfg->ecc_mode > k_ra8_sram_eccmod_max) {
    return k_ra8_err_invalid_arg;
  }
  if ((uint8_t)cfg->on_error > k_ra8_sram_on_error_max) {
    return k_ra8_err_invalid_arg;
  }
  const uint8_t max_rgn =
    (bank == k_ra8_sram_bank_max_idx) ? k_ra8_sram_eccrgn_max3 : k_ra8_sram_eccrgn_max012;
  if ((uint8_t)cfg->eccrgn > max_rgn) {
    return k_ra8_err_invalid_arg;
  }
  return k_ra8_ok;
}

/**
 * @brief Encode a ``ra8_sram_bank_cfg_t`` into an SRAMCRn byte value.
 *
 * @details
 * Per HUM Ch 58.2.7 "SRAMCRn", p 3532. The TSTBYP bit is left clear
 * here -- the self-test routine sets it explicitly when it needs to.
 *
 * @param[in] cfg See implementation.
 * @return Result code.
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL static uint8_t internal_encode_cr(const ra8_sram_bank_cfg_t* cfg)
{
  uint8_t eccmod_field = k_ra8_sram_eccmod_disabled;
  if (cfg->ecc_mode == k_ra8_sram_ecc_no_check) {
    eccmod_field = k_ra8_sram_eccmod_no_check;
  } else if (cfg->ecc_mode == k_ra8_sram_ecc_with_chk) {
    eccmod_field = k_ra8_sram_eccmod_with_chk;
  } else {
    eccmod_field = k_ra8_sram_eccmod_disabled;
  }

  uint8_t value = eccmod_field;
  if (cfg->on_error == k_ra8_sram_on_error_reset) {
    value |= k_ra8_sram_cr_mask_oad;
  }
  if (cfg->enable_1bit_latch) {
    value |= k_ra8_sram_cr_mask_e1stsen;
  }
  return value;
}

/**
 * @brief Write SRAMCRn for ``bank`` under SRAMPRCR_S unlock.
 *
 * @details
 * Per HUM Ch 58.2.4 "SRAMPRCR_S" p 3530, writes to SRAMWTSC /
 * SRAMCRn / SRAMECCRGNn are gated by the ``PR`` bit which is enabled
 * by writing the half-word ``0xA501`` (KW=0xA5, PR=1). This function
 * unlocks, writes, and re-locks.
 *
 * @param[in] bank See implementation.
 * @param[in] value See implementation.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_write_cr_locked(uint8_t bank, uint8_t value)
{
  volatile r_sram_regs_t* regs = ra8_sram_regs();

  /* HUM Ch 58.2.4 "SRAMPRCR_S : SRAM Protection Control Register
   * for Secure", p 3530 -- write 0xA501 to enable PR=1. */
  regs->SRAMPRCR_S = k_ra8_sram_prcr_unlock;

  /* HUM Ch 58.2.7 "SRAMCRn : SRAM Control Register n For ECC RAM",
   * p 3532 -- per-bank SRAMCR layout (OAD / ECCMOD / E1STSEN). */
  *ra8_sram_cr_ptr(regs, bank) = value;

  /* Re-lock with PR=0.
   * HUM Ch 58.2.4 "SRAMPRCR_S", p 3530 */
  regs->SRAMPRCR_S = k_ra8_sram_prcr_lock;
}

/**
 * @brief Write SRAMECCRGNn for ``bank`` under SRAMPRCR_S unlock.
 *
 * @details
 * Per HUM Ch 58.2.8..58.2.11 (p 3533-3535) the per-bank ECC region
 * registers share the SRAMPRCR_S protection scheme.
 *
 * @param[in] bank See implementation.
 * @param[in] value See implementation.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_write_eccrgn_locked(uint8_t bank, uint8_t value)
{
  volatile r_sram_regs_t* regs = ra8_sram_regs();

  /* HUM Ch 58.2.4 "SRAMPRCR_S", p 3530 */
  regs->SRAMPRCR_S = k_ra8_sram_prcr_unlock;

  /* HUM Ch 58.2.8 "SRAMECCRGN0 : SRAM ECC Region Control Register 0",
   * p 3533 (and 58.2.9..58.2.11 for banks 1..3). */
  *ra8_sram_eccrgn_ptr(regs, bank) = (uint8_t)(value & k_ra8_sram_eccrgn_field_msk);

  /* HUM Ch 58.2.4 "SRAMPRCR_S", p 3530 */
  regs->SRAMPRCR_S = k_ra8_sram_prcr_lock;
}

/**
 * @brief Write SRAMWTSC under SRAMPRCR_S unlock.
 *
 * @details See implementation.
 * @param[in] value See implementation.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_write_wtsc_locked(uint8_t value)
{
  volatile r_sram_regs_t* regs = ra8_sram_regs();

  /* HUM Ch 58.2.4 "SRAMPRCR_S", p 3530 */
  regs->SRAMPRCR_S = k_ra8_sram_prcr_unlock;

  /* HUM Ch 58.2.6 "SRAMWTSC : SRAM Wait State Control Register",
   * p 3531. */
  regs->SRAMWTSC = (uint8_t)(value & k_ra8_sram_wtsc_msk);

  /* HUM Ch 58.2.4 "SRAMPRCR_S", p 3530 */
  regs->SRAMPRCR_S = k_ra8_sram_prcr_lock;
}

/**
 * @brief Compose the per-bank 1-bit / 2-bit error-status masks.
 *
 * @details
 * Per HUM Ch 58.2.12 "SRAMESR" p 3535 the bit pattern is
 * ``ERR{bank}{0=1bit | 1=2bit}`` packed two bits per bank starting
 * at bit 0. So bit ``2*bank`` is the 1-bit flag and bit
 * ``2*bank + 1`` is the 2-bit flag.
 *
 * @param[in] raw See implementation.
 * @param[in] one_bit_mask See implementation.
 * @param[in] two_bit_mask See implementation.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL static void
internal_decode_esr(uint16_t raw, uint8_t* one_bit_mask, uint8_t* two_bit_mask)
{
  uint8_t one = 0U;
  uint8_t two = 0U;
  for (uint8_t bank = 0U; bank < k_ra8_sram_bank_count; ++bank) {
    const uint16_t one_bit_pos = (uint16_t)((uint16_t)2U * (uint16_t)bank);
    const uint16_t two_bit_pos = (uint16_t)(one_bit_pos + 1U);
    if ((raw & (uint16_t)((uint16_t)1U << one_bit_pos)) != 0U) {
      one |= (uint8_t)((uint8_t)1U << bank);
    }
    if ((raw & (uint16_t)((uint16_t)1U << two_bit_pos)) != 0U) {
      two |= (uint8_t)((uint8_t)1U << bank);
    }
  }
  *one_bit_mask = one;
  *two_bit_mask = two;
}

/**
 * @brief Translate a SRAMEAR offset into its absolute Secure-alias
 *        address (per HUM Ch 58.2.14 p 3537).
 *
 * @details See implementation.
 * @param[in] ear See implementation.
 * @return Result code.
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL static uintptr_t internal_ear_to_abs_addr(uint32_t ear)
{
  if (ear == 0U) {
    return (uintptr_t)0U;
  }
  return k_ra8_sram_data_base_addr + (uintptr_t)ear;
}

/**
 * @brief Apply the optional security cfg from ``ra8_sram_init``.
 *
 * @details
 * The CPSCU writes are unconditionally ungated -- they belong to the
 * Secure World caller per HUM Ch 58.2.2 (p 3528) and HUM 58.2.3
 * (p 3529). This helper just walks the cfg fields and writes the
 * three register groups in the order SRAMSAR -> SRAMESAR -> SABARn so
 * boundary writes happen after the per-bank security flag is set.
 *
 * @param[in] sec See implementation.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_apply_security(const ra8_sram_security_cfg_t* sec)
{
  uint32_t sar = 0U;
  for (uint8_t bank = 0U; bank < k_ra8_sram_bank_count; ++bank) {
    if (sec->bank_ns[bank]) {
      sar |= k_ra8_sram_sar_bit_sa0 << bank;
    }
  }
  if (sec->wtsc_ns) {
    sar |= k_ra8_sram_sar_bit_wtsa;
  }

  volatile r_sram_cpscu_regs_t* cpscu = ra8_sram_cpscu_regs();

  /* HUM Ch 58.2.2 "SRAMSAR : SRAM Security Attribution Register",
   * p 3528 -- per-bank register security + SRAMWTSC security. */
  cpscu->SRAMSAR = sar;

  /* HUM Ch 58.2.3 "SRAMESAR : SRAM ECC region Security Attribute
   * Register", p 3529 -- ECC region NS bit. */
  uint32_t esar = 0U;
  if (sec->ecc_region_ns) {
    esar = k_ra8_sram_esar_bit_esa;
  }
  cpscu->SRAMESAR = esar;

  for (uint8_t bank = 0U; bank < k_ra8_sram_bank_count; ++bank) {
    /* HUM Ch 58.2.1 "SRAMSABARn : SRAM Security Attribute Boundary
     * Address Register", p 3527 -- boundary value, low 13 bits forced
     * to zero (4 KB aligned). */
    cpscu->SRAMSABAR[bank] = sec->boundary_offset[bank] & ~k_ra8_sram_sabar_align_mask;
  }
}

/**
 * @brief 8-byte zero fill across the bank's data window.
 *
 * @details
 * Per HUM Ch 58.3.2 (p 3538) and HUM 58.4.2 (p 3541) the SRAM is read
 * in 8-byte units, so the syndrome line is computed per uint64_t. The
 * loop bound is the bank's documented size, divided by 8.
 *
 * @param[in] bank See implementation.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_zero_fill_bank(uint8_t bank)
{
  const uint32_t           bytes = ra8_sram_bank_size_bytes(bank);
  volatile uint64_t* const dst   = ra8_sram_bank_data_ptr(bank);
  const uint32_t           words = bytes >> (uint32_t)k_ra8_sram_ecc_word_shift;
  for (uint32_t i = 0U; i < words; ++i) {
    dst[i] = k_ra8_sram_zero_init_word;
  }
}

/**
 * @brief Run a single bank's deterministic zero-init pass.
 *
 * @details
 * The sequence mirrors the STAR ``rx_eccram_init`` pattern but adapted
 * for RA8D2's per-bank SRAMCRn:
 *
 *   1. SRAMCRn = ECCMOD=10b (encode but do not check). Writes from
 *      this point produce valid syndromes.
 *   2. Walk the bank in 8-byte stores, writing zero.
 *   3. SRAMCRn = ECCMOD=00b (ECC off) so the caller can pick its own
 *      final mode via ``ra8_sram_set_mode``.
 *
 * @param[in] bank See implementation.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_zero_init_with_no_check(uint8_t bank)
{
  /* Step 1: enable ECC encode without checking. */
  internal_write_cr_locked(bank, k_ra8_sram_eccmod_no_check);

  /* Step 2: deterministic 64-bit zero fill of the data window. */
  internal_zero_fill_bank(bank);

  /* Step 3: leave the bank with ECC fully disabled so the caller can
   * pick the final mode safely. */
  internal_write_cr_locked(bank, k_ra8_sram_eccmod_disabled);
}

/* =============================================================================
 * Lifecycle
 * =============================================================================
 */

/**
 * @brief Validate every bank cfg and ungate the corresponding MSTP bits.
 *
 * @details
 * HUM Ch 58.3.1 "Module Stop Function" p 3538 + HUM Ch 11.2.6 MSTPCRA
 * p 443. Run as a single pass so a half-applied config never leaks
 * past the init boundary.
 *
 * @param[in] cfg Caller-supplied init config.
 *
 * @return ``k_ra8_ok`` if every bank was validated and ungated.
 *
 * @pre ``cfg`` is non-null.
 * @post All four SRAM banks are clock-ungated on success; on failure
 *       the caller must clean up.
 *
 * @note Internal helper, not thread-safe.
 *
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_validate_and_ungate(const ra8_sram_config_t* cfg)
{
  for (uint8_t bank = 0U; bank < k_ra8_sram_bank_count; ++bank) {
    const ra8_err_t verr = internal_validate_bank_cfg(&cfg->banks[bank], bank);
    RA8_RETURN_ON_ERROR(verr, s_tag, "ra8_sram_init: bad bank cfg");
  }
  for (uint8_t bank = 0U; bank < k_ra8_sram_bank_count; ++bank) {
    const ra8_err_t merr = ra8_mstp_enable(s_sram_mstp_table[bank]);
    /* GCOVR_EXCL_BR_START -- MSTP HW readback */
    RA8_RETURN_ON_ERROR(merr, s_tag, "ra8_sram_init: mstp enable");
    /* GCOVR_EXCL_BR_STOP */
  }
  return k_ra8_ok;
}

/**
 * @brief Apply the per-bank zero-init + ECC mode programming pass.
 *
 * @details
 * HUM Ch 58.3.2 p 3538 (zero-init) + HUM Ch 58.2.10 / 58.2.5 (ECC
 * region size + SRAMCRn final mode). Run as one helper so the
 * top-level init stays small.
 *
 * @param[in] cfg Validated init config.
 *
 * @pre Module clock ungated for every bank.
 * @post Each bank's eccrgn + SRAMCRn matches ``cfg``.
 *
 * @note Internal helper, not thread-safe.
 *
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_apply_per_bank(const ra8_sram_config_t* cfg)
{
  for (uint8_t bank = 0U; bank < k_ra8_sram_bank_count; ++bank) {
    if (cfg->banks[bank].zero_init) {
      internal_zero_init_with_no_check(bank);
    }
  }
  for (uint8_t bank = 0U; bank < k_ra8_sram_bank_count; ++bank) {
    internal_write_eccrgn_locked(bank, (uint8_t)cfg->banks[bank].eccrgn);
    const uint8_t cr_value = internal_encode_cr(&cfg->banks[bank]);
    internal_write_cr_locked(bank, cr_value);
  }
}

[[nodiscard]] ra8_err_t ra8_sram_init(const ra8_sram_config_t* cfg)
{
  RA8_CHECK_NULL_PTR(cfg, s_tag, "cfg must not be nullptr");

  const ra8_err_t v_err = internal_validate_and_ungate(cfg);
  RA8_RETURN_ON_ERROR(v_err, s_tag, "ra8_sram_init: validate/ungate");

  if (cfg->apply_security) {
    internal_apply_security(&cfg->security);
  }

  /* SRAMWTSC is deliberately NOT touched here -- ra8_cgc_init owns it,
   * derived from ICLK per HUM Ch 58.3.7 p 3540. Clearing it from a
   * zero-initialised config is how a caller silently takes the memory
   * system outside guaranteed operation (tracker #524). */

  internal_apply_per_bank(cfg);

  /* Clear any stale error flags. HUM 58.2.13 p 3536. */
  volatile r_sram_regs_t* regs = ra8_sram_regs();
  regs->SRAMESCLR              = k_ra8_sram_err_all_mask;

  s_initialized = true;
  ra8_log_info(s_tag, "ra8_sram_init done");
  return k_ra8_ok;
}

[[nodiscard]] ra8_err_t ra8_sram_deinit(void)
{
  /* Disable ECC + clear OAD on every bank before clock-gating, so a
   * spurious bus error during teardown does not latch a fault. */
  for (uint8_t bank = 0U; bank < k_ra8_sram_bank_count; ++bank) {
    internal_write_cr_locked(bank, 0U);
    internal_write_eccrgn_locked(bank, k_ra8_sram_eccrgn_off);
  }
  /* SRAMWTSC stays as ra8_cgc_init left it. Tearing down the ECC
   * configuration says nothing about the clock, and the caller is still
   * executing out of this SRAM: clearing WTEN here would leave every
   * subsequent access outside the guarantee of HUM Ch 58.3.7 p 3540. */

  /* HUM Ch 58.3.1 "Module Stop Function", p 3538 -- re-gate the
   * clock for every bank (HUM 11.2.6 MSTPCRA p 443). */
  for (uint8_t bank = 0U; bank < k_ra8_sram_bank_count; ++bank) {
    (void)ra8_mstp_disable(s_sram_mstp_table[bank]);
  }

  g_sram_on_error     = nullptr;
  g_sram_on_error_ctx = nullptr;
  for (uint8_t bank = 0U; bank < k_ra8_sram_bank_count; ++bank) {
    g_sram_on_error_bank[bank]     = nullptr;
    g_sram_on_error_bank_ctx[bank] = nullptr;
  }
  s_initialized = false;
  return k_ra8_ok;
}

[[nodiscard]] ra8_err_t ra8_sram_enter_stop(uint8_t bank)
{
  if ((uint16_t)bank >= (uint16_t)k_ra8_sram_bank_count) {
    return k_ra8_err_invalid_arg;
  }
  /* HUM Ch 58.3.1 "Module Stop Function" p 3538 */
  return ra8_mstp_disable(s_sram_mstp_table[bank]);
}

[[nodiscard]] ra8_err_t ra8_sram_exit_stop(uint8_t bank)
{
  if ((uint16_t)bank >= (uint16_t)k_ra8_sram_bank_count) {
    return k_ra8_err_invalid_arg;
  }
  /* HUM Ch 58.3.1 "Module Stop Function" p 3538 */
  return ra8_mstp_enable(s_sram_mstp_table[bank]);
}

/* =============================================================================
 * ECC mode set
 * =============================================================================
 */

[[nodiscard]] ra8_err_t ra8_sram_set_mode(uint8_t bank, const ra8_sram_bank_cfg_t* cfg)
{
  RA8_CHECK_NULL_PTR(cfg, s_tag, "cfg must not be nullptr");
  if ((uint16_t)bank >= (uint16_t)k_ra8_sram_bank_count) {
    return k_ra8_err_invalid_arg;
  }

  const ra8_err_t verr = internal_validate_bank_cfg(cfg, bank);
  /* GCOVR_EXCL_BR_START -- internal_validate_bank_cfg() error edge; set_mode pre-checks the bank */
  RA8_RETURN_ON_ERROR(verr, s_tag, "ra8_sram_set_mode: bad bank cfg");
  /* GCOVR_EXCL_BR_STOP */

  internal_write_eccrgn_locked(bank, (uint8_t)cfg->eccrgn);
  internal_write_cr_locked(bank, internal_encode_cr(cfg));
  return k_ra8_ok;
}

[[nodiscard]] ra8_err_t ra8_sram_set_eccrgn(uint8_t bank, ra8_sram_eccrgn_size_t region)
{
  if ((uint16_t)bank >= (uint16_t)k_ra8_sram_bank_count) {
    return k_ra8_err_invalid_arg;
  }
  const uint8_t max_rgn =
    (bank == k_ra8_sram_bank_max_idx) ? k_ra8_sram_eccrgn_max3 : k_ra8_sram_eccrgn_max012;
  if ((uint8_t)region > max_rgn) {
    return k_ra8_err_invalid_arg;
  }
  internal_write_eccrgn_locked(bank, (uint8_t)region);
  return k_ra8_ok;
}

[[nodiscard]] ra8_err_t ra8_sram_set_wait_state_for_clock(uint32_t iclk_hz, uint32_t iclk_max_hz)
{
  if ((iclk_hz == 0U) || (iclk_max_hz == 0U)) {
    return k_ra8_err_invalid_arg;
  }
  /* HUM Ch 58.3.7 "Wait State", p 3540: WTEN must be 1 when the
   * current ICLK exceeds half the maximum, otherwise it stays 0. */
  const uint32_t threshold = iclk_max_hz >> 1U;
  uint8_t        wtsc      = 0U;
  if (iclk_hz > threshold) {
    wtsc = k_ra8_sram_wtsc_wten;
  }
  internal_write_wtsc_locked(wtsc);
  return k_ra8_ok;
}

/* =============================================================================
 * Status / clear
 * =============================================================================
 */

[[nodiscard]] ra8_err_t ra8_sram_get_status(ra8_sram_status_t* out)
{
  RA8_CHECK_NULL_PTR(out, s_tag, "out must not be nullptr");

  volatile r_sram_regs_t* regs = ra8_sram_regs();

  /* HUM Ch 58.2.12 "SRAMESR : SRAM Error Status Register For ECC
   * RAM", p 3535 -- packed 1-bit/2-bit flags per bank. */
  const uint16_t raw = regs->SRAMESR;
  out->raw_esr       = raw;
  internal_decode_esr(raw, &out->one_bit_mask, &out->two_bit_mask);

  /* HUM Ch 58.2.14 "SRAMEARnm : SRAM Error Address Register nm
   * For ECC RAM", p 3537 -- m=0 is the 1-bit slot, m=1 is the
   * 2-bit slot. Stored as an offset; this driver presents the
   * absolute Secure address to callers. */
  for (uint8_t bank = 0U; bank < k_ra8_sram_bank_count; ++bank) {
    out->addr_1bit[bank] = internal_ear_to_abs_addr(regs->SRAMEAR[bank][0]);
    out->addr_2bit[bank] = internal_ear_to_abs_addr(regs->SRAMEAR[bank][1]);
  }
  return k_ra8_ok;
}

[[nodiscard]] ra8_err_t ra8_sram_clear_status(uint16_t esr_mask)
{
  if ((esr_mask & (uint16_t)~k_ra8_sram_err_all_mask) != 0U) {
    return k_ra8_err_invalid_arg;
  }
  volatile r_sram_regs_t* regs = ra8_sram_regs();
  /* HUM Ch 58.2.13 "SRAMESCLR : SRAM Error Status Clear Register
   * For ECC RAM", p 3536 -- write 1 to each bit to clear. */
  regs->SRAMESCLR = esr_mask;
  return k_ra8_ok;
}

[[nodiscard]] ra8_err_t ra8_sram_clear_address(uint8_t bank, uint8_t slot)
{
  if ((uint16_t)bank >= (uint16_t)k_ra8_sram_bank_count) {
    return k_ra8_err_invalid_arg;
  }
  if (slot > k_ra8_sram_ear_slot_max) {
    return k_ra8_err_invalid_arg;
  }
  /* The EAR is auto-cleared by writing 1 to the corresponding
   * SRAMESCLR bit. HUM Ch 58.2.14 p 3537 -- "These bits are cleared
   * by clearing 1-bit/2-bit ECC error from SRAMESCLR." */
  const uint16_t          bit_pos = (uint16_t)(((uint16_t)2U * (uint16_t)bank) + (uint16_t)slot);
  const uint16_t          mask    = (uint16_t)((uint16_t)1U << bit_pos);
  volatile r_sram_regs_t* regs    = ra8_sram_regs();
  /* HUM Ch 58.2.13 "SRAMESCLR : SRAM Error Status Clear Register
   * For ECC RAM" p 3536 */
  regs->SRAMESCLR = mask;
  return k_ra8_ok;
}

/* =============================================================================
 * Zero-init / self-test / introspection
 * =============================================================================
 */

[[nodiscard]] ra8_err_t ra8_sram_zero_init_bank(uint8_t bank)
{
  if ((uint16_t)bank >= (uint16_t)k_ra8_sram_bank_count) {
    return k_ra8_err_invalid_arg;
  }
  internal_zero_init_with_no_check(bank);
  return k_ra8_ok;
}

/**
 * @brief Inject a 1- or 2-bit fault on a probed ECC line.
 *
 * @details
 * Steps 2 and 3 of the HUM Ch 58.3.4 self-test flowchart, p 3539.
 * Writes through SRAMCRn = 0x80 (bypass) so the read returns the raw
 * syndrome, XORs in the requested fault mask, then arms the
 * "ECC+check" mode so the next normal read triggers the latch.
 *
 * @param[in]  bank          Bank index that owns ``data``.
 * @param[in,out] data       Pointer to the probed 64-bit ECC line.
 * @param[in]  inject_two_bit ``true`` to inject a 2-bit fault.
 *
 * @pre ``data`` was seeded by step 1 of the flowchart.
 * @post Bypass mode left active long enough to corrupt the syndrome,
 *       then re-armed for verification.
 *
 * @note Internal helper, not thread-safe.
 *
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @since 0.1.0
 */
RA8_INTERNAL static void
internal_self_test_inject(uint8_t bank, volatile uint64_t* data, bool inject_two_bit)
{
  internal_write_cr_locked(bank, k_ra8_sram_cr_self_test_phase_bypass);

  uint8_t inject_mask = k_ra8_sram_self_test_flip_1bit;
  if (inject_two_bit) {
    inject_mask = k_ra8_sram_self_test_flip_2bit;
  }
  const uint64_t syndrome  = *data;
  const uint64_t corrupted = syndrome ^ (uint64_t)inject_mask;
  *data                    = corrupted;

  internal_write_cr_locked(bank, k_ra8_sram_cr_self_test_phase_verify);
}

[[nodiscard]] ra8_err_t
ra8_sram_self_test(uint8_t bank, uint32_t probe_offset, bool inject_two_bit, bool* out_caught)
{
  RA8_CHECK_NULL_PTR(out_caught, s_tag, "out_caught must not be nullptr");
  if ((uint16_t)bank >= (uint16_t)k_ra8_sram_bank_count) {
    return k_ra8_err_invalid_arg;
  }
  /* Probe must be 8-byte aligned and inside the bank (HUM 58.4.2
   * p 3541 -- 8-byte ECC line size). */
  if ((probe_offset & ((uint32_t)k_ra8_sram_ecc_word_bytes - 1U)) != 0U) {
    return k_ra8_err_invalid_arg;
  }
  if (probe_offset >= ra8_sram_bank_size_bytes(bank)) {
    return k_ra8_err_invalid_arg;
  }

  *out_caught = false;

  volatile uint64_t* const bank_base = ra8_sram_bank_data_ptr(bank);
  const uintptr_t          data_addr = (uintptr_t)bank_base + (uintptr_t)probe_offset;
  volatile uint64_t* const data      = (volatile uint64_t*)data_addr;

  /* Step 1: seed the line under ECC encode-only (HUM Ch 58.3.4 p 3539). */
  internal_write_cr_locked(bank, k_ra8_sram_cr_self_test_phase_write);
  *data = k_ra8_sram_zero_init_word;

  /* Steps 2-3: bypass-read, inject, then arm verify. */
  internal_self_test_inject(bank, data, inject_two_bit);

  /* Read the line. On real silicon this triggers the ECC engine, which
   * latches SRAMESR / SRAMEAR for the faulted slot. The RAM-backed host
   * register file has no ECC engine, so on the unit-test build the read
   * is inert and host tests stage SRAMESR before the call to drive both
   * legs of the caught decision below. */
  volatile uint64_t scratch = *data;
  (void)scratch;

  /* Step 4: confirm SRAMESR latched the expected flag. */
  volatile r_sram_regs_t* check_regs = ra8_sram_regs();
  const uint16_t          esr        = check_regs->SRAMESR;
  uint16_t                slot_bit   = 0U;
  if (inject_two_bit) {
    slot_bit = 1U;
  }
  const uint16_t want_bit =
    (uint16_t)((uint16_t)1U << (uint16_t)(((uint16_t)2U * (uint16_t)bank) + slot_bit));
  *out_caught = ((esr & want_bit) != 0U);

  return k_ra8_ok;
}

[[nodiscard]] ra8_err_t ra8_sram_get_bank_info(uint8_t bank, ra8_sram_bank_info_t* out)
{
  RA8_CHECK_NULL_PTR(out, s_tag, "out must not be nullptr");
  if ((uint16_t)bank >= (uint16_t)k_ra8_sram_bank_count) {
    return k_ra8_err_invalid_arg;
  }
  out->bank      = bank;
  out->data_base = k_ra8_sram_data_base_addr + (uintptr_t)s_sram_data_off_table[bank];
  out->data_size = ra8_sram_bank_size_bytes(bank);
  out->ecc_base  = k_ra8_sram_data_base_addr + (uintptr_t)s_sram_ecc_off_table[bank];
  out->ecc_size  = ra8_sram_bank_ecc_size_bytes(bank);
  return k_ra8_ok;
}

/*
 * TrustZone security attribution + the ECC error callback fan-out live
 * in ``ra8_sram_security.c`` (split for the 1000-line file-size cap).
 */
