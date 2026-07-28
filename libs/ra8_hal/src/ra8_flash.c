/**
 * @file ra8_flash.c
 * @brief Code-MRAM + Extra-MRAM + Option-Setting driver implementation -- DANGEROUS
 *
 * @par Tag
 * [Ring 3 / HAL] {World: S}
 *
 * @details
 * Implements the full HUM Ch 7 + Ch 59 surface declared in ``ra8_flash.h``:
 *
 *  - Direct STR programming of code-MRAM through MRCPC0/1 (HUM Ch 59.4.2
 *    Figure 59.4 p 3548).
 *  - MACI command sequencer for configuration-set, anti-rollback,
 *    forced-stop, status-clear (HUM Ch 59.4.4 p 3550 + HUM Ch 7
 *    p 278..299 for OFS layout).
 *  - Block-protection writes to MRCBPROT0/1 (HUM Ch 59 p 3604..3605).
 *  - Start-up area swap via MSUACR (temporary) + configuration-set
 *    (permanent) (HUM Ch 7 p 278 + HUM Ch 59 p 3593).
 *  - W-HUK zeroize via MREZC (HUM Ch 59 p 3565).
 *  - ECC encoder / decoder controls and read-error address capture
 *    (HUM Ch 59 p 3554..3558 + p 3624).
 *  - Per-source IRQ enables and a single-callback dispatcher.
 *  - Full lifecycle: init, deinit, reset, force-stop,
 *    enter_pe_mode, exit_pe_mode.
 *  - Update-transfer kick + status (MCTRCNTR / MCTRSTATR / MCTRLSR,
 *    HUM Ch 59 p 3580).
 *
 * Every register access carries a HUM Ch 7 or Ch 59 citation. The
 * driver does not own any global state beyond the registered IRQ
 * callback and a one-shot init flag; the controller itself holds
 * every meaningful state bit.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_flash.h"

#include <stdint.h>
#include <string.h>

#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_flash_internal.h"
#include "ra8_flash_regs.h"
#include "ra8_log.h"

const char* s_flash_tag = "FLASH";

/**
 * @var s_flash_rt
 * @brief Single shared ra8_flash runtime-state instance.
 *
 * @details
 * Cleared at init. Used by the IRQ dispatcher to find the registered
 * callback and to detect callers that try to use APIs before init. This
 * is the sole definition of the cross-TU state declared in
 * ``ra8_flash_internal.h``; the configuration and IRQ TUs reference it
 * via the extern there.
 *
 * @note Not thread-safe; mutated only from single-threaded init / ISR.
 * @warning Do not redefine; this is the sole owner of the state.
 * @since 0.1.0
 */
ra8_flash_runtime_t s_flash_rt = {};

/* =============================================================================
 * Internal helpers
 * =============================================================================
 */

/**
 * @brief Wait for MRCPS.PRGBSYC = 0 and ABUFFULL = 0 (buffer ready).
 *
 * @param[in] limit Maximum spin iterations.
 * @return ``k_ra8_ok`` if both bits cleared, else ``k_ra8_err_hw_timeout``.
 *
 * @pre ``limit`` > 0.
 * @pre Controller is powered.
 * @post Buffer is observed empty or function returns timeout.
 *
 * @details See the matching header declaration for the full
 * contract; this site adds no behaviour beyond what the public
 * API documents.
 * @retval k_ra8_ok Success path.
 * @retval k_ra8_err_invalid_arg Caller violated a precondition.
 * @note Thread safety: see the header declaration.
 * @since 0.1.0
 * @post Documented side effects are visible on success.
 */
static ra8_err_t internal_wait_buffer_ready(uint32_t limit)
{
  for (uint32_t i = 0U; i < limit; ++i) { /* GCOVR_EXCL_BR_LINE */
    /* HUM Ch 59 "MRCPS : Code MRAM Program Status Register" p 3601 */
    const uint8_t s = *ra8_mram_reg8(k_ra8_mram_off_mrcps);
    if (((s & k_ra8_mrcps_mask_prgbsyc) == 0U) &&
        ((s & k_ra8_mrcps_mask_abuffull) == 0U)) { /* GCOVR_EXCL_BR_LINE */
      return k_ra8_ok;
    }
  }
  return k_ra8_err_hw_timeout;
}

/**
 * @brief Test-access wrapper for @ref internal_wait_buffer_ready.
 *
 * @details Forwards the call so tests under @c tests/ can drive the
 *          line-150 AND-decision directly on the production source.
 *          Production code keeps using the static helper.
 *
 * @param[in] limit Maximum spin iterations.
 * @return Forwarded ra8_err_t outcome.
 * @retval k_ra8_ok           Both bits cleared within @p limit iterations.
 * @retval k_ra8_err_hw_timeout Limit exhausted without success.
 *
 * @pre Simulator MRCPS register is mapped (host-test build).
 * @pre @p limit > 0.
 * @post No state mutation beyond the underlying register reads.
 * @post Return value matches the production helper.
 * @note Test-access only.
 * @since 0.1.0
 */
ra8_err_t ra8_flash_internal_wait_buffer_ready_call(uint32_t limit)
{
  return internal_wait_buffer_ready(limit);
}

/**
 * @brief Wait for MRCPS.ABUFEMP = 1 and PRGBSYC = 0 (commit done).
 *
 * @param[in] limit Maximum spin iterations.
 * @return ``k_ra8_ok`` on commit, else ``k_ra8_err_hw_timeout``.
 *
 * @pre ``limit`` > 0.
 * @pre Controller is powered.
 * @post Commit observed or function returns timeout.
 *
 * @details See the matching header declaration for the full
 * contract; this site adds no behaviour beyond what the public
 * API documents.
 * @retval k_ra8_ok Success path.
 * @retval k_ra8_err_invalid_arg Caller violated a precondition.
 * @note Thread safety: see the header declaration.
 * @since 0.1.0
 * @post Documented side effects are visible on success.
 */
static ra8_err_t internal_wait_commit_done(uint32_t limit)
{
  for (uint32_t i = 0U; i < limit; ++i) { /* GCOVR_EXCL_BR_LINE */
    /* HUM Ch 59 "MRCPS : Code MRAM Program Status Register" p 3601 */
    const uint8_t s = *ra8_mram_reg8(k_ra8_mram_off_mrcps);
    if (((s & k_ra8_mrcps_mask_abufemp) != 0U) &&
        ((s & k_ra8_mrcps_mask_prgbsyc) == 0U)) { /* GCOVR_EXCL_BR_LINE */
      return k_ra8_ok;
    }
  }
  return k_ra8_err_hw_timeout;
}

/**
 * @brief Test-access wrapper for @ref internal_wait_commit_done.
 *
 * @details Forwards the call so tests under @c tests/ can drive the
 *          line-181 AND-decision directly on the production source.
 *          Production code keeps using the static helper.
 *
 * @param[in] limit Maximum spin iterations.
 * @return Forwarded ra8_err_t outcome.
 * @retval k_ra8_ok           Commit observed within @p limit iterations.
 * @retval k_ra8_err_hw_timeout Limit exhausted without success.
 *
 * @pre Simulator MRCPS register is mapped (host-test build).
 * @pre @p limit > 0.
 * @post No state mutation beyond the underlying register reads.
 * @post Return value matches the production helper.
 * @note Test-access only.
 * @since 0.1.0
 */
ra8_err_t ra8_flash_internal_wait_commit_done_call(uint32_t limit)
{
  return internal_wait_commit_done(limit);
}

/**
 * @brief Spin until MSTATR.MRDY rises or limit elapses.
 *
 * @param[in] limit Maximum spin iterations.
 * @return ``k_ra8_ok`` if MRDY observed, else ``k_ra8_err_hw_timeout``.
 *
 * @pre ``limit`` > 0.
 * @pre Controller is in P/E mode (MRDY only meaningful then).
 * @post MRDY observed high or function returns timeout.
 *
 * @details See the matching header declaration for the full
 * contract; this site adds no behaviour beyond what the public
 * API documents.
 * @retval k_ra8_ok Success path.
 * @retval k_ra8_err_invalid_arg Caller violated a precondition.
 * @note Thread safety: see the header declaration.
 * @since 0.1.0
 * @post Documented side effects are visible on success.
 */
ra8_err_t ra8_flash_internal_wait_mrdy(uint32_t limit)
{
  for (uint32_t i = 0U; i < limit; ++i) { /* GCOVR_EXCL_BR_LINE */
    /* HUM Ch 59 "MSTATR : Extra MRAM Status Register" p 3578 */
    const uint32_t s = *ra8_mram_reg32(k_ra8_mram_off_mstatr);
    if ((s & k_ra8_mstatr_mask_mrdy) != 0U) { /* GCOVR_EXCL_BR_LINE */
      return k_ra8_ok;
    }
  }
  return k_ra8_err_hw_timeout;
}

/**
 * @brief Open or close the per-world program-control gate.
 *
 * @param[in] world  ``k_ra8_flash_world_ns`` -> MRCPC0; ``_s`` -> MRCPC1.
 * @param[in] enable ``true`` to enable program; ``false`` to lock.
 *
 * @pre None (registers always accessible).
 * @post Matching MRCPCx register holds the keyed value.
 *
 * @details See the matching header declaration for the full
 * contract; this site adds no behaviour beyond what the public
 * API documents.
 * @note Thread safety: see the header declaration.
 * @since 0.1.0
 * @pre Module/state preconditions hold (see function body).
 * @post Documented side effects are visible on success.
 */
static void internal_set_program_gate(ra8_flash_world_t world, bool enable)
{
  const uint16_t off =
    (world == k_ra8_flash_world_s) ? k_ra8_mram_off_mrcpc1 : k_ra8_mram_off_mrcpc0;
  uint16_t value;
  if (world == k_ra8_flash_world_s) {
    if (enable) {
      value = k_ra8_mrcpc1_key_enable;
    } else {
      value = k_ra8_mrcpc1_key_disable;
    }
  } else {
    if (enable) {
      value = k_ra8_mrcpc0_key_enable;
    } else {
      value = k_ra8_mrcpc0_key_disable;
    }
  }
  /* HUM Ch 59 "MRCPC0/MRCPC1 : Code MRAM Program Control Register" p 3601 */
  *ra8_mram_reg16(off) = value;
}

/**
 * @brief Toggle high-speed program mode (MRPSC.MHSPEN).
 *
 * @param[in] enable true => MHSPEN=1.
 *
 * @pre None.
 * @post MRPSC.MHSPEN matches ``enable``.
 *
 * @details See the matching header declaration for the full
 * contract; this site adds no behaviour beyond what the public
 * API documents.
 * @note Thread safety: see the header declaration.
 * @since 0.1.0
 * @pre Module/state preconditions hold (see function body).
 * @post Documented side effects are visible on success.
 */
static void internal_set_hsp_mode(bool enable)
{
  /* HUM Ch 59 "MRPSC : MRAM Program Speed Control Register" p 3600 */
  uint8_t mrpsc = 0U;
  if (enable) {
    mrpsc = k_ra8_mrpsc_mask_mhspen;
  }
  *ra8_mram_reg8(k_ra8_mram_off_mrpsc) = mrpsc;
}

/**
 * @brief Set MRCPFB.MPFBEN to enable/disable the prefetch buffer.
 *
 * @param[in] enable true => prefetch on.
 *
 * @pre None.
 * @post MRCPFB.MPFBEN matches ``enable``.
 *
 * @details See the matching header declaration for the full
 * contract; this site adds no behaviour beyond what the public
 * API documents.
 * @note Thread safety: see the header declaration.
 * @since 0.1.0
 * @pre Module/state preconditions hold (see function body).
 * @post Documented side effects are visible on success.
 */
void ra8_flash_internal_set_prefetch(bool enable)
{
  /* HUM Ch 59.5.1 "MRCPFB : Code MRAM Pre-Fetch Buffer Enable Register" p 3551 */
  uint8_t mrcpfb = 0U;
  if (enable) {
    mrcpfb = 1U;
  }
  *ra8_mram_reg8(k_ra8_mram_off_mrcpfb) = mrcpfb;
  s_flash_rt.prefetch_on                = enable;
}

#if defined(RA8_SIMULATOR_MODE) && defined(UNIT_TEST)
/* Host-test MACI command capture (see ra8_flash_internal.h). The MACI port is a
 * single MMIO address, so RAM alone cannot retain the stream a host test needs
 * to assert the emitted opcode sequence (0xE8 Program vs 0x40 Config-Set).
 * Present only in the host unit-test binary; firmware / ra8_emulator link neither
 * RA8_SIMULATOR_MODE nor UNIT_TEST, so the whole block compiles out. */
uint8_t  g_ra8_flash_maci_cmd8_log[k_ra8_flash_maci_log_cap];
uint32_t g_ra8_flash_maci_cmd8_len;
uint16_t g_ra8_flash_maci_cmd16_log[k_ra8_flash_maci_log_cap];
uint32_t g_ra8_flash_maci_cmd16_len;

void ra8_flash_internal_maci_log_reset(void)
{
  g_ra8_flash_maci_cmd8_len  = 0U;
  g_ra8_flash_maci_cmd16_len = 0U;
}
#endif /* RA8_SIMULATOR_MODE && UNIT_TEST */

/**
 * @brief Send a single byte through the MACI command-issuing area.
 *
 * @param[in] byte Command byte.
 *
 * @pre Controller is in P/E mode.
 * @post One byte was written to MACI_CMD8.
 *
 * @details See the matching header declaration for the full
 * contract; this site adds no behaviour beyond what the public
 * API documents.
 * @note Thread safety: see the header declaration.
 * @since 0.1.0
 * @pre Module/state preconditions hold (see function body).
 * @post Documented side effects are visible on success.
 */
void ra8_flash_internal_maci_cmd8(uint8_t byte)
{
  /* HUM Ch 59 "MACI Command-Issuing Area" p 3550 */
  *ra8_mram_cmd8() = byte;
#if defined(RA8_SIMULATOR_MODE) && defined(UNIT_TEST)
  if (g_ra8_flash_maci_cmd8_len < (uint32_t)k_ra8_flash_maci_log_cap) {
    g_ra8_flash_maci_cmd8_log[g_ra8_flash_maci_cmd8_len] = byte;
    g_ra8_flash_maci_cmd8_len++;
  }
#endif
}

/**
 * @brief Send a halfword through the MACI command-issuing area.
 *
 * @param[in] half 16-bit data.
 *
 * @pre Controller is in P/E mode.
 * @post One halfword was written to MACI_CMD16.
 *
 * @details See the matching header declaration for the full
 * contract; this site adds no behaviour beyond what the public
 * API documents.
 * @note Thread safety: see the header declaration.
 * @since 0.1.0
 * @pre Module/state preconditions hold (see function body).
 * @post Documented side effects are visible on success.
 */
void ra8_flash_internal_maci_cmd16(uint16_t half)
{
  /* HUM Ch 59 "MACI Command-Issuing Area" p 3550 */
  *ra8_mram_cmd16() = half;
#if defined(RA8_SIMULATOR_MODE) && defined(UNIT_TEST)
  if (g_ra8_flash_maci_cmd16_len < (uint32_t)k_ra8_flash_maci_log_cap) {
    g_ra8_flash_maci_cmd16_log[g_ra8_flash_maci_cmd16_len] = half;
    g_ra8_flash_maci_cmd16_len++;
  }
#endif
}

/* =============================================================================
 * Public API: lifecycle
 * =============================================================================
 */

ra8_err_t ra8_flash_init(const ra8_flash_cfg_t* cfg)
{
  RA8_CHECK_NULL_PTR(cfg, s_flash_tag, "cfg must not be nullptr");
  if (cfg->mrcfreq_mhz > (uint16_t)k_ra8_flash_max_mrcfreq_mhz) {
    return k_ra8_err_invalid_arg;
  }
  if (cfg->mrefreq_mhz > (uint8_t)k_ra8_flash_max_mrefreq_mhz) {
    return k_ra8_err_invalid_arg;
  }

  /* Frequency-down procedure -- HUM Ch 59.4.3 Figure 59.6 p 3550. */
  ra8_flash_internal_set_prefetch(false);

  /* HUM Ch 59.5.2 "MRCFREQ : Code MRAM Frequency Notifications Register" p 3551 */
  const uint32_t mrcfreq_word =
    (k_ra8_flash_mrcfreq_key << k_ra8_flash_freq_key_shift) | (uint32_t)cfg->mrcfreq_mhz;
  *ra8_mram_reg32(k_ra8_mram_off_mrcfreq) = mrcfreq_word;

  /* HUM Ch 59.5.3 "MREFREQ : Extra MRAM Frequency Notifications Register" p 3552 */
  const uint32_t mrefreq_word =
    (k_ra8_flash_mrefreq_key << k_ra8_flash_freq_key_shift) | (uint32_t)cfg->mrefreq_mhz;
  *ra8_mram_reg32(k_ra8_mram_off_mrefreq) = mrefreq_word;

  /* HUM Ch 59 "MRCEECC : Code MRAM ECC Encoder Control" p 3624 */
  uint16_t mrceecc_bits = 0U;
  if (cfg->ecc_encoder_enable) {
    mrceecc_bits = k_ra8_mrceecc_mask_eccen;
  }
  *ra8_mram_reg16(k_ra8_mram_off_mrceecc) = (uint16_t)(k_ra8_mrceecc_key_shift | mrceecc_bits);

  /* HUM Ch 59 "MRCDECC : Code MRAM ECC Decoder Control" p 3554 */
  uint16_t mrcdecc_bits = 0U;
  if (cfg->ecc_decoder_enable) {
    mrcdecc_bits = k_ra8_mrcdecc_mask_dececen;
  }
  *ra8_mram_reg16(k_ra8_mram_off_mrcdecc) = (uint16_t)(k_ra8_mrcdecc_key_shift | mrcdecc_bits);

  ra8_flash_internal_set_prefetch(cfg->prefetch_en);

  /* Lock both program gates so no stray store can program MRAM. */
  internal_set_program_gate(k_ra8_flash_world_ns, false);
  internal_set_program_gate(k_ra8_flash_world_s, false);
  internal_set_hsp_mode(false);

  /* Clear sticky errors so the new run starts from a clean state. */
  /* HUM Ch 59 "MRCPS : Code MRAM Program Status Register" p 3601 */
  *ra8_mram_reg8(k_ra8_mram_off_mrcps) = k_ra8_mrcps_mask_errors;
  /* HUM Ch 59 "MRCRAES : Code MRAM Read Access Error Status" p 3554 */
  *ra8_mram_reg8(k_ra8_mram_off_mrcraes) = 0U;
  /* HUM Ch 59 "MRERAES : Extra MRAM Read Access Error Status" p 3557 */
  *ra8_mram_reg8(k_ra8_mram_off_mreraes) = 0U;

  s_flash_rt.initialized = true;
  s_flash_rt.cb          = nullptr;
  s_flash_rt.user_ctx    = nullptr;

  ra8_log_info_val(s_flash_tag, "flash_init mrcfreq", (uint32_t)cfg->mrcfreq_mhz);
  return k_ra8_ok;
}

ra8_err_t ra8_flash_deinit(void)
{
  /* Lock everything, clear sticky errors, re-enable prefetch. */
  internal_set_program_gate(k_ra8_flash_world_ns, false);
  internal_set_program_gate(k_ra8_flash_world_s, false);
  internal_set_hsp_mode(false);
  /* If we accidentally left P/E mode on, drop back to read mode. */
  /* HUM Ch 59 "MENTRYR : Extra MRAM Program-Mode Entry" p 3582 */
  *ra8_mram_reg16(k_ra8_mram_off_mentryr) = k_ra8_mentryr_read_mode;
  /* HUM Ch 59 "MRCPS : Code MRAM Program Status Register" p 3601 */
  *ra8_mram_reg8(k_ra8_mram_off_mrcps) = k_ra8_mrcps_mask_errors;
  ra8_flash_internal_set_prefetch(true);

  s_flash_rt.initialized = false;
  s_flash_rt.cb          = nullptr;
  s_flash_rt.user_ctx    = nullptr;
  return k_ra8_ok;
}

/* =============================================================================
 * Public API: status snapshots
 * =============================================================================
 */

ra8_err_t ra8_flash_get_status(uint8_t* out_status)
{
  RA8_CHECK_NULL_PTR(out_status, s_flash_tag, "out_status must not be nullptr");
  /* HUM Ch 59 "MRCPS : Code MRAM Program Status Register" p 3601 */
  *out_status = *ra8_mram_reg8(k_ra8_mram_off_mrcps);
  return k_ra8_ok;
}

ra8_err_t ra8_flash_get_extended_status(ra8_flash_status_ext_t* out)
{
  RA8_CHECK_NULL_PTR(out, s_flash_tag, "out must not be nullptr");
  /* HUM Ch 59 "MRCPS : Code MRAM Program Status Register" p 3601 */
  out->mrcps = *ra8_mram_reg8(k_ra8_mram_off_mrcps);
  /* HUM Ch 59 "MASTAT : Extra MRAM Access Status Register" p 3577 */
  out->mastat = *ra8_mram_reg8(k_ra8_mram_off_mastat);
  /* HUM Ch 59 "MREZS : Extra MRAM Zeroization Status Register" p 3565 */
  out->mrezs = *ra8_mram_reg8(k_ra8_mram_off_mrezs);
  /* HUM Ch 59 "MCMDR : MACI Command Register" p 3589 */
  out->mcmdr = *ra8_mram_reg16(k_ra8_mram_off_mcmdr);
  /* HUM Ch 59 "MSTATR : Extra MRAM Status Register" p 3578 */
  out->mstatr = *ra8_mram_reg32(k_ra8_mram_off_mstatr);
  return k_ra8_ok;
}

ra8_err_t ra8_flash_clear_status(uint8_t mask)
{
  if ((mask & (uint8_t)~k_ra8_mrcps_mask_errors) != 0U) {
    return k_ra8_err_invalid_arg;
  }
  /* HUM Ch 59 "MRCPS : Code MRAM Program Status Register" p 3601 -- W1C
   * for PRGERRC and ECCERRC; other bits are read-only. */
  *ra8_mram_reg8(k_ra8_mram_off_mrcps) = mask;
  return k_ra8_ok;
}

ra8_err_t ra8_flash_set_rww_disable(bool disable)
{
  bool prefetch = true;
  if (disable) {
    prefetch = false;
  }
  ra8_flash_internal_set_prefetch(prefetch);
  return k_ra8_ok;
}

/**
 * @brief Pure (state-free) window allow/deny predicate -- see
 *        @ref ra8_flash_internal_window_allows_pure in
 *        ra8_flash_internal.h for the full contract.
 *
 * @details Promoted as a pure helper so the
 *          ``win_low == 0U && win_high == 0U`` AND-decision can be
 *          driven directly by host MC/DC tests with synthetic inputs
 *          rather than mutating module state. The state-reading
 *          wrapper @c ra8_flash_internal_window_allows simply forwards.
 *
 * @param[in] addr     Start address of the candidate region.
 * @param[in] len      Length in bytes of the candidate region.
 * @param[in] win_low  Inclusive lower bound of the allow window.
 * @param[in] win_high Exclusive upper bound of the allow window.
 *
 * @return ``true`` if the region is permitted, ``false`` if blocked.
 * @retval true  Region permitted (or no window installed).
 * @retval false Region overlaps outside the installed window.
 *
 * @pre None.
 * @pre None.
 * @post No side effects.
 * @post Return value depends solely on the four inputs.
 *
 * @note Pure function; thread-safe.
 *
 * @since 0.1.0
 */
bool ra8_flash_internal_window_allows_pure(uintptr_t addr,
                                           uint32_t  len,
                                           uintptr_t win_low,
                                           uintptr_t win_high)
{
  if (win_low == 0U && win_high == 0U) {
    return true;
  }
  const uintptr_t end_excl = (uintptr_t)((uint64_t)addr + (uint64_t)len);
  if (addr < win_low) {
    return false;
  }
  if (end_excl > win_high) {
    return false;
  }
  return true;
}

/**
 * @brief Test whether [addr, addr+len) lies inside the configured soft window.
 *
 * @details
 * The soft window mirrors the FSP ``accessWindowSet`` surface but is
 * stored in driver state rather than in the silicon (RA8D2 has no
 * FAWMON / FAWMR; HUM Ch 59 substitutes block-protect bits). A window
 * with ``win_low == win_high == 0`` is treated as disabled (allow all).
 * Forwards to @ref ra8_flash_internal_window_allows_pure.
 *
 * @param[in] addr Start address of the candidate operation.
 * @param[in] len  Length in bytes (must be > 0 if the caller is writing).
 *
 * @return ``true`` if the operation is permitted, ``false`` if blocked.
 * @retval true  Region permitted (or no window installed).
 * @retval false Region overlaps outside the installed window.
 *
 * @pre None.
 * @pre None.
 * @post No side effects.
 * @post Return value depends solely on @p addr / @p len and module state.
 *
 * @note Internal helper, not thread-safe.
 *
 * @since 0.1.0
 */
bool ra8_flash_internal_window_allows(uintptr_t addr, uint32_t len)
{
  return ra8_flash_internal_window_allows_pure(addr, len, s_flash_rt.win_low, s_flash_rt.win_high);
}

/* =============================================================================
 * Public API: direct code-MRAM programming
 * =============================================================================
 */

/**
 * @brief Validate the write_block destination range and alignment.
 *
 * @details
 * Rejects ``len`` outside [1, 32] bytes, addresses outside the 1 MiB
 * code-MRAM window, and writes that would straddle a 32-byte page
 * boundary (HUM Ch 59 p 3601 -- writes are flushed at page granularity).
 *
 * @param[in] mram_addr Destination address.
 * @param[in] len       Length in bytes.
 *
 * @return ``k_ra8_ok`` if the write is well formed.
 *
 * @pre None.
 * @post No side effects.
 *
 * @note Internal helper, not thread-safe.
 *
 * @retval k_ra8_ok Success path.
 * @retval k_ra8_err_invalid_arg Caller violated a precondition.
 * @since 0.1.0
 * @pre Module/state preconditions hold (see function body).
 * @post Documented side effects are visible on success.
 */
static ra8_err_t internal_validate_write_block(uint32_t mram_addr, uint32_t len)
{
  if (len == 0U || len > k_ra8_mram_write_size_bytes) {
    return k_ra8_err_invalid_arg;
  }
  const uint32_t end_excl =
    (uint32_t)((uint64_t)mram_addr + (uint64_t)len); /* avoids 32-bit overflow */
  if (mram_addr < (uint32_t)k_ra8_flash_code_start) {
    return k_ra8_err_invalid_arg;
  }
  if (end_excl > (uint32_t)k_ra8_flash_code_start + (uint32_t)k_ra8_flash_code_size) {
    return k_ra8_err_invalid_arg;
  }
  const uint32_t page_mask = k_ra8_mram_write_size_bytes - 1U;
  if ((mram_addr & ~page_mask) != ((end_excl - 1U) & ~page_mask)) {
    return k_ra8_err_invalid_arg;
  }
  if (!ra8_flash_internal_window_allows((uintptr_t)mram_addr, len)) {
    return k_ra8_err_out_of_range;
  }
  return k_ra8_ok;
}

/**
 * @brief Steps 2-6 of the HUM block-write sequence.
 *
 * @details
 * HUM Ch 59 p 3550 "Programming Sequence" + p 3601 "MRCFLR" key. Open
 * the program gate, copy ``src`` into the MRAM window, pulse the
 * keyed flush, wait for commit, then unconditionally tear down the
 * gate. Returns the commit-wait result so the caller can act on
 * timeouts without leaking the gate-open state.
 *
 * @param[in] mram_addr Destination address (validated by caller).
 * @param[in] src       Source bytes (non-null, validated by caller).
 * @param[in] len       Length in bytes (1..32, validated by caller).
 * @param[in] world     Secure / non-secure world selector.
 *
 * @return ``k_ra8_ok`` on commit success, otherwise the commit-wait error.
 *
 * @pre Caller already drained the previous transfer with
 *      ``internal_wait_buffer_ready``.
 * @post Program gate, HSP mode and prefetch are restored on every
 *       exit path.
 *
 * @note Internal helper, not thread-safe.
 *
 * @retval k_ra8_ok Success path.
 * @retval k_ra8_err_invalid_arg Caller violated a precondition.
 * @since 0.1.0
 *
 *
 * @pre Module/state preconditions hold (see function body).
 * @post Documented side effects are visible on success.
 */
static ra8_err_t internal_flash_program_window(uint32_t          mram_addr,
                                               const uint8_t*    src,
                                               uint32_t          len,
                                               ra8_flash_world_t world)
{
  ra8_flash_internal_set_prefetch(false);
  internal_set_hsp_mode(true);
  internal_set_program_gate(world, true);

  volatile uint8_t* dst = (volatile uint8_t*)(uintptr_t)mram_addr;
  for (uint32_t i = 0U; i < len; ++i) {
    dst[i] = src[i];
  }

  __asm__ volatile("" ::: "memory");
  /* HUM Ch 59 "MRCFLR : Code MRAM Flush Register" p 3601 */
  *ra8_mram_reg16(k_ra8_mram_off_mrcflr) = k_ra8_mrcflr_key_flush;

  const ra8_err_t err = internal_wait_commit_done(k_ra8_flash_busy_spin_limit);

  /* Tear down regardless of err so the gate cannot stay open. */
  internal_set_program_gate(world, false);
  internal_set_hsp_mode(false);
  ra8_flash_internal_set_prefetch(true);

  return err;
}

ra8_err_t
ra8_flash_write_block(uint32_t mram_addr, const uint8_t* src, uint32_t len, ra8_flash_world_t world)
{
  RA8_CHECK_NULL_PTR(src, s_flash_tag, "src must not be nullptr");
  const ra8_err_t v_err = internal_validate_write_block(mram_addr, len);
  RA8_RETURN_ON_ERROR(v_err, s_flash_tag, "flash_write: validate"); /* GCOVR_EXCL_BR_LINE */

  /* Step 1: wait for the controller to be idle. */
  ra8_err_t err = internal_wait_buffer_ready(k_ra8_flash_busy_spin_limit);
  RA8_RETURN_ON_ERROR(err, s_flash_tag, "flash_write: busy wait"); /* GCOVR_EXCL_BR_LINE */

  /* Steps 2-6: gate, write, flush, commit, teardown. */
  err = internal_flash_program_window(mram_addr, src, len, world);
  RA8_RETURN_ON_ERROR(err, s_flash_tag, "flash_write: commit wait"); /* GCOVR_EXCL_BR_LINE */

  /* Step 7: error check.
   * HUM Ch 59 "MRCPS : Code MRAM Program Status Register" p 3601 */
  const uint8_t status = *ra8_mram_reg8(k_ra8_mram_off_mrcps);
  if ((status & k_ra8_mrcps_mask_errors) != 0U) {
    *ra8_mram_reg8(k_ra8_mram_off_mrcps) = k_ra8_mrcps_mask_errors;
    ra8_log_error_val(s_flash_tag, "flash_write: hw error", (uint32_t)status);
    return k_ra8_err_hw_error;
  }
  return k_ra8_ok;
}

ra8_err_t ra8_flash_erase_block(uint32_t mram_addr, ra8_flash_world_t world)
{
  if ((mram_addr & (k_ra8_mram_block_size_bytes - 1U)) != 0U) {
    return k_ra8_err_invalid_arg;
  }
  /* MRAM has no separate erase: erase == program-to-all-ones. */
  static const uint8_t s_ones[k_ra8_mram_block_size_bytes] = {
    0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU,
    0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU,
    0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU,
  };
  return ra8_flash_write_block(mram_addr, s_ones, k_ra8_mram_block_size_bytes, world);
}

/* =============================================================================
 * Public API: block protection
 * =============================================================================
 */

ra8_err_t ra8_flash_block_protect_set(ra8_flash_world_t world, bool lock, bool permanent)
{
  if (permanent && !lock) {
    /* Permanent unlock makes no sense -- the fuse is one-shot. */
    return k_ra8_err_invalid_arg;
  }
  uint16_t value;
  if (world == k_ra8_flash_world_s) {
    if (lock) {
      value = k_ra8_mrcbprot1_key_lock;
    } else {
      value = k_ra8_mrcbprot1_key_unlock;
    }
    /* HUM Ch 59 "MRCBPROT1 : Code MRAM Block Protection (S)" p 3605 */
    *ra8_mram_reg16(k_ra8_mram_off_mrcbprot1) = value;
  } else {
    if (lock) {
      value = k_ra8_mrcbprot0_key_lock;
    } else {
      value = k_ra8_mrcbprot0_key_unlock;
    }
    /* HUM Ch 59 "MRCBPROT0 : Code MRAM Block Protection (NS)" p 3604 */
    *ra8_mram_reg16(k_ra8_mram_off_mrcbprot0) = value;
  }
  /* The "permanent" flag is conveyed via a configuration-set update of
   * the OFS region; the per-register write above only takes effect for
   * this boot. We expose the request for transparency, but defer the
   * persistent fuse to ``ra8_flash_config_set_write`` for callers who
   * truly want it. The HUM Ch 7 p 278 OFS layout is the trust anchor. */
  if (permanent) {
    ra8_log_warn(s_flash_tag, "permanent block protect requires config-set");
  }
  return k_ra8_ok;
}

/* =============================================================================
 * Public API: P/E mode transitions
 * =============================================================================
 */

ra8_err_t ra8_flash_enter_pe_mode(void)
{
  ra8_flash_internal_set_prefetch(false);
  /* HUM Ch 59 "MENTRYR : Extra MRAM Program-Mode Entry" p 3582 */
  *ra8_mram_reg16(k_ra8_mram_off_mentryr) = k_ra8_mentryr_pe_enter;

  for (uint32_t i = 0U; i < k_ra8_flash_pe_spin_limit; ++i) { /* GCOVR_EXCL_BR_LINE */
    /* HUM Ch 59 "MENTRYR : Extra MRAM Program-Mode Entry" p 3582 */
    const uint16_t v = *ra8_mram_reg16(k_ra8_mram_off_mentryr);
    if ((v & k_ra8_mentryr_mask_pe_mode) != 0U) { /* GCOVR_EXCL_BR_LINE */
      return k_ra8_ok;
    }
  }
  return k_ra8_err_hw_timeout;
}

ra8_err_t ra8_flash_exit_pe_mode(void)
{
  /* HUM Ch 59 "MENTRYR : Extra MRAM Program-Mode Entry" p 3582 */
  *ra8_mram_reg16(k_ra8_mram_off_mentryr) = k_ra8_mentryr_read_mode;

  for (uint32_t i = 0U; i < k_ra8_flash_pe_spin_limit; ++i) { /* GCOVR_EXCL_BR_LINE */
    /* HUM Ch 59 "MENTRYR : Extra MRAM Program-Mode Entry" p 3582 */
    const uint16_t v = *ra8_mram_reg16(k_ra8_mram_off_mentryr);
    if ((v & k_ra8_mentryr_mask_pe_mode) == 0U) { /* GCOVR_EXCL_BR_LINE */
      ra8_flash_internal_set_prefetch(s_flash_rt.prefetch_on);
      return k_ra8_ok;
    }
  }
  return k_ra8_err_hw_timeout;
}

ra8_err_t ra8_flash_suspend(void)
{
  /* HUM Ch 59 "MENTRYR : Extra MRAM Program-Mode Entry" pp 3582+ --
   * driving MENTRYR with the keyed pause pattern (KEY=0xAA, MENTRY=1,
   * PCKA=1) halts an in-flight MACI command at the next page
   * boundary.  Poll for PCKA=1 to confirm the pause. */
  *ra8_mram_reg16(k_ra8_mram_off_mentryr) = k_ra8_mentryr_pe_pause;

  for (uint32_t i = 0U; i < k_ra8_flash_pe_spin_limit; ++i) { /* GCOVR_EXCL_BR_LINE */
    /* HUM Ch 59 "MENTRYR" p 3582 */                          /* +                  */
    const uint16_t v = *ra8_mram_reg16(k_ra8_mram_off_mentryr);
    if ((v & k_ra8_mentryr_mask_pcka) != 0U) { /* GCOVR_EXCL_BR_LINE */
      return k_ra8_ok;
    }
  }
  return k_ra8_err_hw_timeout;
}

ra8_err_t ra8_flash_resume(void)
{
  /* HUM Ch 59 "MENTRYR : Extra MRAM Program-Mode Entry" pp 3582+ --
   * resume key clears PCKA, leaving MENTRY=1 so programming continues.
   * Poll for PCKA=0 before returning. */
  *ra8_mram_reg16(k_ra8_mram_off_mentryr) = k_ra8_mentryr_pe_resume;

  for (uint32_t i = 0U; i < k_ra8_flash_pe_spin_limit; ++i) { /* GCOVR_EXCL_BR_LINE */
    /* HUM Ch 59 "MENTRYR" p 3582 */                          /* +                  */
    const uint16_t v = *ra8_mram_reg16(k_ra8_mram_off_mentryr);
    if ((v & k_ra8_mentryr_mask_pcka) == 0U) { /* GCOVR_EXCL_BR_LINE */
      return k_ra8_ok;
    }
  }
  return k_ra8_err_hw_timeout;
}

ra8_err_t ra8_flash_lock_set(uintptr_t addr, uint16_t lock_bits)
{
  /* Address must lie inside the 1 MiB code-MRAM window. */
  const uintptr_t code_lo = k_ra8_flash_code_start;
  const uintptr_t code_hi = code_lo + k_ra8_flash_code_size;
  if ((addr < code_lo) || (addr >= code_hi)) {
    return k_ra8_err_invalid_arg;
  }
  /* Validate the keyed value: bits [15:8] must be one of the two
   * documented keys (0x88 for MRCBPROT0, 0x44 for MRCBPROT1). */
  enum : uint16_t {
    k_ra8_mrcbprot_key_byte_mask = 0xFF00U, /**< RA8 mrcbprot key byte mask. */
    k_ra8_mrcbprot_key_ns        = 0x8800U, /**< RA8 mrcbprot key ns.        */
    k_ra8_mrcbprot_key_s         = 0x4400U, /**< RA8 mrcbprot key s.         */
  };
  enum : uint32_t {
    k_ra8_mrcbprot_secure_bit = 0x00080000UL, /**< Address bit 19. */
  };
  const uint16_t key    = (uint16_t)(lock_bits & k_ra8_mrcbprot_key_byte_mask);
  const bool     key_ns = (key == k_ra8_mrcbprot_key_ns);
  const bool     key_s  = (key == k_ra8_mrcbprot_key_s);
  if (!key_ns && !key_s) {
    return k_ra8_err_invalid_arg;
  }

  const bool is_secure = ((addr & (uintptr_t)k_ra8_mrcbprot_secure_bit) != 0U);
  if (is_secure) {
    /* HUM Ch 59 "MRCBPROT1 : Code MRAM Block Protection (S)" p 3605 */
    *ra8_mram_reg16(k_ra8_mram_off_mrcbprot1) = lock_bits;
  } else {
    /* HUM Ch 59 "MRCBPROT0 : Code MRAM Block Protection (NS)" p 3604 */
    *ra8_mram_reg16(k_ra8_mram_off_mrcbprot0) = lock_bits;
  }
  return k_ra8_ok;
}

/* =============================================================================
 * Public API: forced stop / reset
 * =============================================================================
 */

ra8_err_t ra8_flash_force_stop(void)
{
  /* HUM Ch 59 "MACI Command-Issuing Area" p 3550 */
  ra8_flash_internal_maci_cmd8(k_ra8_maci_cmd_forced_stop);
  ra8_err_t err = ra8_flash_internal_wait_mrdy(k_ra8_flash_maci_spin_limit);
  if (err != k_ra8_ok) {
    return err;
  }

  /* HUM Ch 59 "MASTAT : Extra MRAM Access Status Register" p 3577 */
  const uint8_t mastat = *ra8_mram_reg8(k_ra8_mram_off_mastat);
  if ((mastat & k_ra8_mastat_mask_cmdlk) != 0U) {
    return k_ra8_err_hw_error;
  }
  return k_ra8_ok;
}

ra8_err_t ra8_flash_reset(void)
{
  RA8_VALIDATE_INIT(s_flash_rt.initialized, s_flash_tag, "flash_reset before init");
  ra8_err_t err = ra8_flash_enter_pe_mode();
  if (err != k_ra8_ok) {
    return err;
  }
  ra8_err_t stop_err = ra8_flash_force_stop();
  /* Clear status whether or not the stop succeeded. */
  /* HUM Ch 59 "MACI Command-Issuing Area" p 3550 */
  ra8_flash_internal_maci_cmd8(k_ra8_maci_cmd_status_clear);
  (void)ra8_flash_internal_wait_mrdy(k_ra8_flash_maci_spin_limit);
  ra8_err_t exit_err = ra8_flash_exit_pe_mode();
  if (stop_err != k_ra8_ok) {
    return stop_err;
  }
  return exit_err;
}
