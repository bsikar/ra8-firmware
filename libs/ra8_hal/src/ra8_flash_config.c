/**
 * @file ra8_flash_config.c
 * @brief MRAM configuration-set, ARC counters + extra-MRAM programming -- DANGEROUS
 *
 * @par Tag
 * [Ring 3 / HAL] {World: S}
 *
 * @details
 * Configuration / data-programming aspect of the ra8_flash driver, split
 * out of ``ra8_flash.c`` so every translation unit stays under the
 * file-size cap. Implements the slice of the HUM Ch 7 + Ch 59 surface
 * declared in ``ra8_flash.h``:
 *
 *  - Start-up area swap via MSUACR (temporary) + configuration-set
 *    (permanent) (HUM Ch 7 p 278 + HUM Ch 59 p 3593).
 *  - MACI command sequencer for configuration-set / OFS programming and
 *    extra-MRAM (data flash) write / erase (HUM Ch 59.4.4 p 3550 + HUM
 *    Ch 7 p 278..299 for OFS layout).
 *  - Anti-rollback counter read / increment (HUM Ch 7.2.21..23 p 296..297
 *    + HUM Ch 59 p 3576).
 *  - W-HUK zeroize via MREZC (HUM Ch 59 p 3565), MSAR / MSUINITR kicks,
 *    ECC encoder / decoder controls, clock-frequency update, and the
 *    update-transfer kick + status (HUM Ch 59 p 3554..3585).
 *
 * Cross-TU shared runtime state, the shared constant blocks, and the
 * promoted low-level MACI / prefetch / wait helpers live in
 * ``ra8_flash_internal.h``. Every register access carries a HUM Ch 7 or
 * Ch 59 citation.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_flash.h"
#include "ra8_flash_internal.h"
#include "ra8_flash_regs.h"
#include "ra8_hw_err.h"
#include "ra8_log.h"

/** @brief ARC bit-to-word shift and blank-flash fill byte. */
typedef enum : uint32_t {
  k_flash_bits_to_words_shift = 5U,    /**< Divide a bit count by 32 -> words. */
  k_flash_blank_byte          = 0xFFU, /**< Erased-flash fill byte.            */
} flash_const_t;

/**
 * @enum ra8_flash_cfg_word_const_t
 * @brief Bit patterns for the configuration-set word vector.
 *
 * @details
 * HUM Ch 7 "Option-Setting Memory" p 278. The configuration-set
 * vector is written as a sequence of 16-bit words; we OR in only the
 * bits we want to drive low, keeping the remaining bits as 1 to
 * preserve unused fields.
 */
typedef enum : uint16_t {
  k_ra8_flash_cfg_word_all_ones = 0xFFFFU, /**< Word filler when no bits drive low. */
  k_ra8_flash_btflg_default     = 0x8000U, /**< BTFLG bit 15 selects default boot.  */
  k_ra8_flash_btflg_alternate   = 0x0000U, /**< BTFLG cleared selects alternate.    */
  k_ra8_flash_btflg_word_keep   = 0x1FFFU, /**< Bits 12:0 kept as ones (unused).    */
} ra8_flash_cfg_word_const_t;

/* =============================================================================
 * Internal helpers: anti-rollback counter math
 * =============================================================================
 */

/**
 * @brief Translate logical ARC id to MCNTSELR field value.
 *
 * @param[in] id Logical counter id.
 * @return MCNTSELR.CNTSEL field value (0 if id is out of range).
 *
 * @pre ``id`` < ``k_ra8_flash_arc_count`` for a meaningful result.
 * @post Returned value matches FSP ``mram_counter_to_mcntselr_convert``.
 *
 * @details See the matching header declaration for the full
 * contract; this site adds no behaviour beyond what the public
 * API documents.
 * @retval k_ra8_ok Success path.
 * @retval k_ra8_err_invalid_arg Caller violated a precondition.
 * @note Thread safety: see the header declaration.
 * @since 0.1.0
 * @pre Module/state preconditions hold (see function body).
 * @post Documented side effects are visible on success.
 */
static uint8_t internal_arc_to_mcntselr(ra8_flash_arc_id_t id)
{
  switch (id) {
    case k_ra8_flash_arc_sec:
      return k_ra8_mcntselr_sec;
    case k_ra8_flash_arc_oembl:
      return k_ra8_mcntselr_oembl;
    case k_ra8_flash_arc_nsec_0:
      return k_ra8_mcntselr_nsec_0;
    case k_ra8_flash_arc_nsec_1:
      return (uint8_t)(k_ra8_mcntselr_nsec_0 + 1U);
    case k_ra8_flash_arc_nsec_2:
      return (uint8_t)(k_ra8_mcntselr_nsec_0 + 2U);
    case k_ra8_flash_arc_nsec_3:
      return (uint8_t)(k_ra8_mcntselr_nsec_0 + 3U);
    default:
      return 0U;
  }
  /* unreachable */
}

/**
 * @brief Compute the maximum count for an ARC id.
 *
 * @param[in] id Logical counter id.
 * @return Max bit count.
 *
 * @pre ``id`` < ``k_ra8_flash_arc_count``.
 * @post Returned value reflects the ARCCS.ARCNS field for NSEC ids.
 *
 * @details See the matching header declaration for the full
 * contract; this site adds no behaviour beyond what the public
 * API documents.
 * @retval k_ra8_ok Success path.
 * @retval k_ra8_err_invalid_arg Caller violated a precondition.
 * @note Thread safety: see the header declaration.
 * @since 0.1.0
 * @pre Module/state preconditions hold (see function body).
 * @post Documented side effects are visible on success.
 */
static uint32_t internal_arc_max_count(ra8_flash_arc_id_t id)
{
  if (id == k_ra8_flash_arc_sec) {
    return k_ra8_arc_sec_max_bits;
  }
  if (id == k_ra8_flash_arc_oembl) {
    return k_ra8_arc_oembl_max_bits;
  }
  /* NSEC: read ARCCS.ARCNS to decide single vs multiple. */
  /* HUM Ch 7.2.21 "ARCCS Anti-Rollback Counter Configuration" p 296 */
  const uint16_t arccs = *(volatile const uint16_t*)k_ra8_flash_ofs_arccs_addr;
  const uint8_t  arcns = (uint8_t)(arccs & (uint16_t)k_ra8_arc_arccs_mask);
  if (arcns == (uint8_t)k_ra8_arc_arcns_single) {
    return k_ra8_arc_nsec_single;
  }
  return k_ra8_arc_nsec_multiple;
}

/**
 * @brief Pop-count helper for 32-bit words used by the ARC reader.
 *
 * @param[in] x Input word.
 * @return Number of set bits.
 *
 * @pre None.
 * @post Returned value in [0, 32].
 *
 * @details See the matching header declaration for the full
 * contract; this site adds no behaviour beyond what the public
 * API documents.
 * @retval k_ra8_ok Success path.
 * @retval k_ra8_err_invalid_arg Caller violated a precondition.
 * @note Thread safety: see the header declaration.
 * @since 0.1.0
 *
 * @pre Module/state preconditions hold (see function body).
 * @post Documented side effects are visible on success.
 */
static uint32_t internal_popcount32(uint32_t x)
{
  /* Avoid relying on __builtin_popcount so the simulator host build
   * does not pick up a different implementation than the firmware
   * target. NASA Rule 2 -- bounded loop. */
  uint32_t count = 0U;
  for (uint32_t i = 0U; i < 32U; ++i) {
    if ((x & (uint32_t)(1UL << i)) != 0U) {
      count++;
    }
  }
  return count;
}

/* =============================================================================
 * Public API: start-up area control
 * =============================================================================
 */

ra8_err_t ra8_flash_set_startup_area(ra8_flash_startup_t target, bool temporary)
{
  if (target > k_ra8_flash_startup_btflg) {
    return k_ra8_err_invalid_arg;
  }
  ra8_err_t err = ra8_flash_enter_pe_mode();
  if (err != k_ra8_ok) {
    return err;
  }

  if (temporary) {
    /* HUM Ch 59 "MSUACR : Start-Up Area Control Register" p 3593 */
    const uint16_t swap_bit                = (target == k_ra8_flash_startup_alternate)
                                               ? k_ra8_msuacr_swap_alternate
                                               : k_ra8_msuacr_swap_default;
    *ra8_mram_reg16(k_ra8_mram_off_msuacr) = (uint16_t)(k_ra8_msuacr_key | swap_bit);
  } else {
    /* Permanent: configuration-set write to BTFLG. */
    /* HUM Ch 7 "Option-Setting Memory" p 278 */
    uint16_t cfg_words[k_ra8_mram_config_set_word_count];
    for (uint32_t i = 0U; i < k_ra8_mram_config_set_word_count; ++i) {
      cfg_words[i] = k_ra8_flash_cfg_word_all_ones;
    }
    /* BTFLG occupies bit 15 of word index 3 (FSP MRAM_PRV_CONFIG_SET_BTFLG_OFFSET).
     * 0 selects alternate, 1 selects default (HUM Ch 7 p 278). */
    uint16_t btflg_bit = k_ra8_flash_btflg_alternate;
    if (target == k_ra8_flash_startup_default) {
      btflg_bit = k_ra8_flash_btflg_default;
    }
    /* HUM Ch 7 "OFS SAS region" p 278 */
    cfg_words[3] = (uint16_t)(btflg_bit | k_ra8_flash_btflg_word_keep);
    err          = ra8_flash_config_set_write(k_ra8_msaddr_config_set_startup, cfg_words);
  }

  ra8_err_t exit_err = ra8_flash_exit_pe_mode();
  if (err != k_ra8_ok) {
    return err;
  }
  return exit_err;
}

ra8_err_t ra8_flash_get_startup_area(uint8_t* out_btflg, uint8_t* out_fspr)
{
  RA8_CHECK_NULL_PTR(out_btflg, s_flash_tag, "out_btflg must not be nullptr");
  RA8_CHECK_NULL_PTR(out_fspr, s_flash_tag, "out_fspr must not be nullptr");
  /* HUM Ch 59 "MSUASMON : Start-Up Area Monitor" p 3593 */
  const uint32_t v = *ra8_mram_reg32(k_ra8_mram_off_msuasmon);
  *out_btflg       = (uint8_t)((v & k_ra8_msuasmon_mask_btflg) != 0U);
  *out_fspr        = (uint8_t)((v & k_ra8_msuasmon_mask_fspr) != 0U);
  return k_ra8_ok;
}

/* =============================================================================
 * Public API: configuration-set write (low-level OFS update)
 * =============================================================================
 */

ra8_err_t ra8_flash_config_set_write(uint32_t target_addr, const uint16_t* words)
{
  RA8_CHECK_NULL_PTR(words, s_flash_tag, "words must not be nullptr");
  /* One MACI byte-stream shape -- ``<opener>, N, 8 halfwords, 0xD0`` -- serves
   * two target regions, and the opener opcode is chosen per region below:
   *   - OFS configuration area (HUM Ch 7 "Option-Setting Memory" p 278) at
   *     0x02C9F000: the Configuration Set command (HUM Ch 59.7.4.8 p 3594).
   *   - Extra-MRAM option-setting / OTP area (HUM Ch 59.7.4.5 Table 59.15
   *     p 3592) at 0x02E07600: the Program command (HUM Ch 59.7.4.5 "Program
   *     Command" Fig 59.13 p 3591). Config-Set is NOT valid for the data area
   *     -- it raises
   *     MSTATR.CFGSETERR and leaves the target blank, so a later read of the
   *     un-programmed cells bus-faults on the blank-MRAM ECC error.
   * Accept both ranges; reject everything else. */
  const uint32_t ofs_end   = (uint32_t)k_ra8_flash_ofs_start + (uint32_t)k_ra8_flash_ofs_size;
  const uint32_t extra_end = (uint32_t)k_ra8_flash_extra_start + (uint32_t)k_ra8_flash_extra_size;
  const bool     in_ofs =
    (bool)((target_addr >= (uint32_t)k_ra8_flash_ofs_start) && (target_addr < ofs_end));
  const bool in_extra =
    (bool)((target_addr >= (uint32_t)k_ra8_flash_extra_start) && (target_addr < extra_end));
  if (!in_ofs && !in_extra) {
    return k_ra8_err_invalid_arg;
  }

  /* HUM Ch 59.5.19 "MSADDR : MACI Command Start Address Register" p 3573 */
  *ra8_mram_reg32(k_ra8_mram_off_msaddr) = target_addr;
  /* Opener: Program (0xE8) for the extra-MRAM data area, else Configuration
   * Set (0x40) for the OFS config area. HUM Ch 59.7.4.5 Fig 59.13 p 3591 /
   * HUM Ch 59.7.4.8 p 3594. */
  const uint8_t opener =
    in_extra ? (uint8_t)k_ra8_maci_cmd_program : (uint8_t)k_ra8_maci_cmd_config_set;
  ra8_flash_internal_maci_cmd8(opener);
  ra8_flash_internal_maci_cmd8(k_ra8_maci_cmd_word_count_n);

  /* N = k_ra8_mram_config_set_word_count halfwords of payload.
   * HUM Ch 59.7.4.5 Fig 59.13 p 3592 / HUM Ch 59.7.4.8 p 3595. */
  for (uint32_t i = 0U; i < k_ra8_mram_config_set_word_count; ++i) {
    ra8_flash_internal_maci_cmd16(words[i]);
  }
  /* Trailer 0xD0 starts command processing. HUM Ch 59.7.4.5 Fig 59.13 p 3592. */
  ra8_flash_internal_maci_cmd8(k_ra8_maci_cmd_final);

  ra8_err_t err = ra8_flash_internal_wait_mrdy(k_ra8_flash_maci_spin_limit);
  if (err != k_ra8_ok) {
    return err;
  }

  /* HUM Ch 59 "MSTATR : Extra MRAM Status Register" p 3578 */
  const uint32_t s = *ra8_mram_reg32(k_ra8_mram_off_mstatr);
  if ((s & k_ra8_mstatr_mask_any_err) != 0U) {
    return k_ra8_err_hw_error;
  }
  return k_ra8_ok;
}

/* =============================================================================
 * Public API: anti-rollback counters
 * =============================================================================
 */

/**
 * @brief Issue a single MACI counter command (read or increment).
 *
 * @param[in] mcntselr MCNTSELR field value.
 * @param[in] cmd      MACI opcode.
 * @return ``ra8_err_t``.
 *
 * @pre Controller already in P/E mode.
 * @post MRDY observed or function returns timeout/error.
 *
 * @details See the matching header declaration for the full
 * contract; this site adds no behaviour beyond what the public
 * API documents.
 * @retval k_ra8_ok Success path.
 * @retval k_ra8_err_invalid_arg Caller violated a precondition.
 * @note Thread safety: see the header declaration.
 * @since 0.1.0
 * @pre Module/state preconditions hold (see function body).
 * @post Documented side effects are visible on success.
 */
static ra8_err_t internal_arc_cmd(uint8_t mcntselr, uint8_t cmd)
{
  /* HUM Ch 59 "MCNTSELR : MRAM Counter Select Register" p 3576 */
  *ra8_mram_reg8(k_ra8_mram_off_mcntselr) = (uint8_t)(mcntselr & k_ra8_mcntselr_mask);
  ra8_flash_internal_maci_cmd8(cmd);
  ra8_flash_internal_maci_cmd8(k_ra8_maci_cmd_final);
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

/**
 * @brief Sum the population count of one of the four ARC_NSEC slots.
 *
 * @details
 * HUM Ch 7.2.21 "ARCCS" p 296 + HUM Ch 7.2.23 "ARC_NSEC" p 297. The
 * ARCNS field selects between a single 16-word counter or four 2-word
 * counters. ``id`` selects which sub-counter to sum.
 *
 * @param[in] id  One of ``k_ra8_flash_arc_nsec_0``..3.
 * @return Total set-bit count.
 *
 * @pre ``id`` belongs to the NSEC family.
 * @post No side effects.
 *
 * @note Internal helper, not thread-safe.
 *
 * @retval k_ra8_ok Success path.
 * @retval k_ra8_err_invalid_arg Caller violated a precondition.
 * @since 0.1.0
 *
 * @pre Module/state preconditions hold (see function body).
 * @post Documented side effects are visible on success.
 */
static uint32_t internal_arc_nsec_count(ra8_flash_arc_id_t id)
{
  /* HUM Ch 7.2.21 "ARCCS" p 296 */
  const uint16_t arccs = *(volatile const uint16_t*)k_ra8_flash_ofs_arccs_addr;
  const uint8_t  arcns = (uint8_t)(arccs & (uint16_t)k_ra8_arc_arccs_mask);
  /* HUM Ch 7.2.23 "ARC_NSEC" p 297 */
  const volatile uint32_t* base      = (const volatile uint32_t*)k_ra8_flash_ofs_arc_nsec_addr;
  uint32_t                 words_per = (arcns == (uint8_t)k_ra8_arc_arcns_single) ? 16U : 2U;
  if (words_per > k_ra8_mram_arc_max_words) {
    words_per = k_ra8_mram_arc_max_words;
  }
  uint32_t base_idx = words_per * 3U;
  if (id == k_ra8_flash_arc_nsec_0) {
    base_idx = 0U;
  } else if (id == k_ra8_flash_arc_nsec_1) {
    base_idx = words_per;
  } else if (id == k_ra8_flash_arc_nsec_2) {
    base_idx = words_per * 2U;
  } else {
    /* falls through to 3*words_per default. */
  }
  uint32_t count = 0U;
  for (uint32_t w = 0U; w < words_per; ++w) {
    count += internal_popcount32(base[base_idx + w]);
  }
  return count;
}

/* internal_arc_read_locked -- see header for full description. */
static ra8_err_t internal_arc_read_locked(ra8_flash_arc_id_t id, uint32_t* out_count)
{
  uint8_t  mcntselr = internal_arc_to_mcntselr(id);
  uint32_t count    = 0U;

  if (id == k_ra8_flash_arc_oembl) {
    /* OEMBL needs the MACI read so RSIP-E50D can intercept. */
    ra8_err_t err = internal_arc_cmd(mcntselr, k_ra8_maci_cmd_read_counter);
    if (err != k_ra8_ok) {
      return err;
    }
    /* HUM Ch 59 "MCNTDTR0 : MRAM Counter Data 0" p 3576 */
    const uint32_t lo = *ra8_mram_reg32(k_ra8_mram_off_mcntdtr0);
    /* HUM Ch 59 "MCNTDTR1 : MRAM Counter Data 1" p 3577 */
    const uint32_t hi = *ra8_mram_reg32(k_ra8_mram_off_mcntdtr1);
    count             = internal_popcount32(lo) + internal_popcount32(hi);
  } else if (id == k_ra8_flash_arc_sec) {
    /* HUM Ch 7.2.22 "ARC_SEC" p 296: ARC_SEC is a 64-bit unary counter, i.e.
     * 2 x uint32_t words. FSP r_mram.c L1196 derives the same count via
     * BSP_FEATURE_FLASH_ARC_SEC_MAX_COUNT (64) >> 5. The previous loop walked
     * 8 words, which over-counted into adjacent OFS state. */
    const volatile uint32_t* p         = (const volatile uint32_t*)k_ra8_flash_ofs_arc_sec_addr;
    const uint32_t           sec_words = k_ra8_arc_sec_max_bits >> k_flash_bits_to_words_shift;
    for (uint32_t w = 0U; w < sec_words; ++w) {
      count += internal_popcount32(p[w]);
    }
  } else {
    count = internal_arc_nsec_count(id);
  }

  *out_count = count;
  (void)mcntselr; /* used in OEMBL branch only */
  return k_ra8_ok;
}

ra8_err_t ra8_flash_arc_increment(ra8_flash_arc_id_t counter)
{
  if (counter >= k_ra8_flash_arc_count) {
    return k_ra8_err_invalid_arg;
  }
  RA8_VALIDATE_INIT(s_flash_rt.initialized, s_flash_tag, "arc_inc before init");

  ra8_err_t err = ra8_flash_enter_pe_mode();
  if (err != k_ra8_ok) {
    return err;
  }

  /* NASA Power-of-10 Rule 1 forbids goto, so the read-bound-check-increment
   * sequence is expressed as a short-circuit chain: each step runs only if
   * the previous one returned k_ra8_ok, and the unconditional exit-PE step
   * replaces the prior goto-out label.
   * HUM Ch 59.4.4 "MACI Increment Counter" p 3550. */
  uint32_t cur = 0U;
  err          = internal_arc_read_locked(counter, &cur);
  if (err == k_ra8_ok) {
    const uint32_t max = internal_arc_max_count(counter);
    if (cur + 1U > max) {
      err = k_ra8_err_out_of_range;
    } else {
      err = internal_arc_cmd(internal_arc_to_mcntselr(counter), k_ra8_maci_cmd_increment_counter);
    }
  }

  const ra8_err_t exit_err = ra8_flash_exit_pe_mode();
  if (err == k_ra8_ok) {
    err = exit_err;
  }
  return err;
}

ra8_err_t ra8_flash_arc_read(ra8_flash_arc_id_t counter, uint32_t* out_count)
{
  RA8_CHECK_NULL_PTR(out_count, s_flash_tag, "out_count must not be nullptr");
  if (counter >= k_ra8_flash_arc_count) {
    return k_ra8_err_invalid_arg;
  }
  RA8_VALIDATE_INIT(s_flash_rt.initialized, s_flash_tag, "arc_read before init");

  if (counter == k_ra8_flash_arc_oembl) {
    ra8_err_t err = ra8_flash_enter_pe_mode();
    if (err != k_ra8_ok) {
      return err;
    }
    ra8_err_t r        = internal_arc_read_locked(counter, out_count);
    ra8_err_t exit_err = ra8_flash_exit_pe_mode();
    if (r != k_ra8_ok) {
      return r;
    }
    return exit_err;
  }
  return internal_arc_read_locked(counter, out_count);
}

/* =============================================================================
 * Public API: zeroize, MSAR, MSUINITR, ECC controls
 * =============================================================================
 */

ra8_err_t ra8_flash_zeroize_huk(void)
{
  RA8_VALIDATE_INIT(s_flash_rt.initialized, s_flash_tag, "zeroize before init");
  /* HUM Ch 59 "MREZC : Extra MRAM Zeroization Control" p 3565 */
  *ra8_mram_reg16(k_ra8_mram_off_mrezc) = k_ra8_mrezc_full_zero;

  for (uint32_t i = 0U; i < k_ra8_flash_zeroize_spin; ++i) { /* GCOVR_EXCL_BR_LINE */
    /* HUM Ch 59 "MREZS : Extra MRAM Zeroization Status" p 3565 */
    const uint8_t s = *ra8_mram_reg8(k_ra8_mram_off_mrezs);
    if ((s & k_ra8_mrezs_mask_whukexe) == 0U) { /* GCOVR_EXCL_BR_LINE */
      return k_ra8_ok;
    }
  }
  return k_ra8_err_hw_timeout;
}

ra8_err_t ra8_flash_set_security_attribution(uint16_t new_msar)
{
  /* HUM Ch 59.5.13 "MSAR : MRAM Security Attribution Register" p 3559 */
  *ra8_mram_reg16(k_ra8_mram_off_msar) = new_msar;
  return k_ra8_ok;
}

ra8_err_t ra8_flash_msuinitr_kick(void)
{
  /* HUM Ch 59 "MSUINITR : Extra MRAM Sequencer Set-Up Init" p 3585 */
  *ra8_mram_reg16(k_ra8_mram_off_msuinitr) = k_ra8_msuinitr_full_init;

  for (uint32_t i = 0U; i < k_ra8_flash_pe_spin_limit; ++i) {
    /* HUM Ch 59 "MSUINITR : Extra MRAM Sequencer Set-Up Init" p 3585 */
    const uint16_t v = *ra8_mram_reg16(k_ra8_mram_off_msuinitr);
#if defined(RA8_SIMULATOR_MODE) && defined(UNIT_TEST)
    /* Host MMIO fault seam: on real HW the sequencer auto-clears
     * SUINIT once the init completes; host RAM cannot, so the seam
     * owns the loop-exit decision (first-poll success unless a test
     * arms a fault to drive the retry / timeout legs). */
    if (ra8_sim_mmio_wait_eval(ra8_mram_reg16(k_ra8_mram_off_msuinitr),
                               i,
                               ((v & k_ra8_msuinitr_mask_suinit) == 0U))) {
      return k_ra8_ok;
    }
#else
    if ((v & k_ra8_msuinitr_mask_suinit) == 0U) {
      return k_ra8_ok;
    }
#endif
  }
  return k_ra8_err_hw_timeout;
}

ra8_err_t ra8_flash_set_ecc_encoder_enable(bool enable)
{
  /* HUM Ch 59 "MRCEECC : Code MRAM ECC Encoder Control" p 3624 */
  uint16_t enc_bit = 0U;
  if (enable) {
    enc_bit = k_ra8_mrceecc_mask_eccen;
  }
  *ra8_mram_reg16(k_ra8_mram_off_mrceecc) = (uint16_t)(k_ra8_mrceecc_key_shift | enc_bit);
  return k_ra8_ok;
}

ra8_err_t ra8_flash_set_ecc_decoder_enable(bool enable)
{
  /* HUM Ch 59 "MRCDECC : Code MRAM ECC Decoder Control" p 3554 */
  uint16_t dec_bit = 0U;
  if (enable) {
    dec_bit = k_ra8_mrcdecc_mask_dececen;
  }
  *ra8_mram_reg16(k_ra8_mram_off_mrcdecc) = (uint16_t)(k_ra8_mrcdecc_key_shift | dec_bit);
  return k_ra8_ok;
}

ra8_err_t ra8_flash_get_ecc_error_addr(uint32_t* out_code_ted,
                                       uint32_t* out_code_dec,
                                       uint32_t* out_extra_ted,
                                       uint32_t* out_extra_dec)
{
  RA8_CHECK_NULL_PTR(out_code_ted, s_flash_tag, "out_code_ted null");
  RA8_CHECK_NULL_PTR(out_code_dec, s_flash_tag, "out_code_dec null");
  RA8_CHECK_NULL_PTR(out_extra_ted, s_flash_tag, "out_extra_ted null");
  RA8_CHECK_NULL_PTR(out_extra_dec, s_flash_tag, "out_extra_dec null");

  /* HUM Ch 59 "MRCRTEA : Code MRAM TED Error Address" p 3555 */
  *out_code_ted = *ra8_mram_reg32(k_ra8_mram_off_mrcrtea);
  /* HUM Ch 59 "MRCRDEA : Code MRAM DEC Error Address" p 3555 */
  *out_code_dec = *ra8_mram_reg32(k_ra8_mram_off_mrcrdea);
  /* HUM Ch 59 "MRERTEA : Extra MRAM TED Error Address" p 3558 */
  *out_extra_ted = *ra8_mram_reg32(k_ra8_mram_off_mrertea);
  /* HUM Ch 59 "MRERDEA : Extra MRAM DEC Error Address" p 3558 */
  *out_extra_dec = *ra8_mram_reg32(k_ra8_mram_off_mrerdea);
  return k_ra8_ok;
}

ra8_err_t ra8_flash_get_program_error_addr(uint32_t* out_addr)
{
  RA8_CHECK_NULL_PTR(out_addr, s_flash_tag, "out_addr must not be nullptr");
  /* HUM Ch 59 "MRCPEA : Code MRAM Program Error Address" p 3601 */
  *out_addr = *ra8_mram_reg32(k_ra8_mram_off_mrcpea);
  return k_ra8_ok;
}

ra8_err_t ra8_flash_update_clock_freq(uint16_t mrcfreq_mhz, uint8_t mrefreq_mhz)
{
  if (mrcfreq_mhz > (uint16_t)k_ra8_flash_max_mrcfreq_mhz) {
    return k_ra8_err_invalid_arg;
  }
  if (mrefreq_mhz > (uint8_t)k_ra8_flash_max_mrefreq_mhz) {
    return k_ra8_err_invalid_arg;
  }
  const bool prefetch_was = s_flash_rt.prefetch_on;
  ra8_flash_internal_set_prefetch(false);

  /* HUM Ch 59.5.2 "MRCFREQ : Code MRAM Frequency Notifications Register" p 3551 */
  *ra8_mram_reg32(k_ra8_mram_off_mrcfreq) =
    (k_ra8_flash_mrcfreq_key << k_ra8_flash_freq_key_shift) | (uint32_t)mrcfreq_mhz;
  /* HUM Ch 59.5.3 "MREFREQ : Extra MRAM Frequency Notifications Register" p 3552 */
  *ra8_mram_reg32(k_ra8_mram_off_mrefreq) =
    (k_ra8_flash_mrefreq_key << k_ra8_flash_freq_key_shift) | (uint32_t)mrefreq_mhz;

  ra8_flash_internal_set_prefetch(prefetch_was);
  return k_ra8_ok;
}

/* =============================================================================
 * Public API: update transfer
 * =============================================================================
 */

ra8_err_t ra8_flash_set_update_transfer(uint8_t list_select)
{
  if (list_select > (uint8_t)k_ra8_flash_max_list_select) {
    return k_ra8_err_invalid_arg;
  }
  /* HUM Ch 59 "MCTRLSR : MRAM Update Transfer List Select" p 3580 */
  *ra8_mram_reg8(k_ra8_mram_off_mctrlsr) = (uint8_t)(list_select & k_ra8_mctrlsr_mask_list_sel);
  /* HUM Ch 59 "MCTRCNTR : MRAM Update Transfer Control" p 3580 */
  *ra8_mram_reg16(k_ra8_mram_off_mctrcntr) =
    (uint16_t)(k_ra8_mctrcntr_key | k_ra8_mctrcntr_mask_start);
  return k_ra8_ok;
}

ra8_err_t ra8_flash_get_update_status(uint8_t* out_busy, uint8_t* out_done, uint8_t* out_err)
{
  RA8_CHECK_NULL_PTR(out_busy, s_flash_tag, "out_busy null");
  RA8_CHECK_NULL_PTR(out_done, s_flash_tag, "out_done null");
  RA8_CHECK_NULL_PTR(out_err, s_flash_tag, "out_err null");
  /* HUM Ch 59 "MCTRSTATR : MRAM Update Transfer Status" p 3580 */
  const uint16_t v = *ra8_mram_reg16(k_ra8_mram_off_mctrstatr);
  *out_busy        = (uint8_t)((v & k_ra8_mctrstatr_mask_busy) != 0U);
  *out_done        = (uint8_t)((v & k_ra8_mctrstatr_mask_done) != 0U);
  *out_err         = (uint8_t)((v & k_ra8_mctrstatr_mask_err) != 0U);
  return k_ra8_ok;
}

/* =============================================================================
 * Public API: extra-MRAM (data flash) program / erase
 * =============================================================================
 */

/**
 * @brief Pack one config-set's worth of source bytes into 8 halfwords.
 *
 * @details Builds the ``k_ra8_mram_config_set_word_count``-halfword payload for
 *          the config-set starting at byte offset @p done into @p src, packing
 *          two bytes per halfword (little-endian) and padding any byte at or
 *          beyond @p len with ``k_flash_blank_byte`` (0xFF). Extracted from
 *          `ra8_flash_extra_mram_write` so the multi-config-set loop stays under
 *          the complexity gate.
 *
 * @param[in]  src   Source buffer being programmed.
 * @param[in]  len   Total valid source length in bytes.
 * @param[in]  done  Byte offset of this config-set within @p src.
 * @param[out] words Receives the packed halfword payload.
 *
 * @return Nothing.
 *
 * @pre @p src and @p words are non-NULL.
 * @pre @p words holds ``k_ra8_mram_config_set_word_count`` entries.
 * @post @p words[i] holds src[done+2i] in its low byte (0xFF past @p len).
 * @post No other state is modified.
 *
 * @note Trivially thread-safe; operates only on the caller's buffers.
 * @since 0.1.0
 * @pre Module/state preconditions hold (see function body).
 * @post Documented side effects are visible on success.
 */
static void priv_pack_config_words(const uint8_t* src,
                                   uint32_t       len,
                                   uint32_t       done,
                                   uint16_t       words[k_ra8_mram_config_set_word_count])
{
  for (uint32_t i = 0U; i < k_ra8_mram_config_set_word_count; ++i) {
    const uint32_t base = done + (i * 2U);
    const uint8_t  lo   = (base < len) ? src[base] : k_flash_blank_byte;
    const uint8_t  hi   = (base + 1U < len) ? src[base + 1U] : k_flash_blank_byte;
    words[i]            = (uint16_t)((uint16_t)lo | ((uint16_t)hi << 8U));
  }
}

ra8_err_t ra8_flash_extra_mram_write(uint32_t mram_addr, const uint8_t* src, uint32_t len)
{
  RA8_CHECK_NULL_PTR(src, s_flash_tag, "src must not be nullptr");
  if (len == 0U || len > k_ra8_mram_write_size_bytes) {
    return k_ra8_err_invalid_arg;
  }
  if (mram_addr < (uint32_t)k_ra8_flash_extra_start) {
    return k_ra8_err_invalid_arg;
  }
  const uint32_t end_excl = (uint32_t)((uint64_t)mram_addr + (uint64_t)len);
  /* OTP-misuse guard (#397): cap the general-purpose write path at
   * k_ra8_flash_extra_locked_start. The permanent / irreversible option-setting
   * structures (PBPS, POFSPS, REVOKE, Zeroization-HUK enable, anti-rollback)
   * begin there; programming any of them can brick the part or destroy the
   * wrapped HUK, so they require the deliberate, separately-named
   * ra8_flash_config_set_write. This bound is stricter than the full window end
   * (k_ra8_flash_extra_start + k_ra8_flash_extra_size), so the single check also
   * rejects an overrun past the window. HUM Ch 59.7.4.5 Table 59.15 p 3592. */
  if (end_excl > (uint32_t)k_ra8_flash_extra_locked_start) {
    return k_ra8_err_invalid_arg;
  }
  const uint32_t page_mask = k_ra8_mram_write_size_bytes - 1U;
  if ((mram_addr & ~page_mask) != ((end_excl - 1U) & ~page_mask)) {
    return k_ra8_err_invalid_arg;
  }

  ra8_err_t err = ra8_flash_enter_pe_mode();
  if (err != k_ra8_ok) {
    return err;
  }

  /* The extra-MRAM data area is programmed with the MACI Program command (HUM
   * Ch 59.7.4.5 "Program Command" Fig 59.13 p 3591) -- NOT the Configuration
   * Set command, which is only valid for the OFS config area. One Program
   * command carries ``k_ra8_mram_config_set_word_count`` halfwords (16 bytes),
   * so a write spanning more than that issues back-to-back Program commands,
   * each retargeting MSADDR ``k_ra8_mram_config_set_bytes`` further on, until the
   * whole ``len`` is programmed. ``ra8_flash_config_set_write`` picks the Program
   * opener for this extra-MRAM range and sets MSADDR per call, so no separate
   * MSADDR write is needed here. Tail bytes pad to 0xFFFF. */
  for (uint32_t done = 0U; done < len; done += (uint32_t)k_ra8_mram_config_set_bytes) {
    uint16_t cfg_words[k_ra8_mram_config_set_word_count] = {};
    priv_pack_config_words(src, len, done, cfg_words);
    err = ra8_flash_config_set_write(mram_addr + done, cfg_words);
    if (err != k_ra8_ok) {
      break;
    }
  }

  ra8_err_t exit_err = ra8_flash_exit_pe_mode();
  if (err == k_ra8_ok) {
    err = exit_err;
  }
  return err;
}

ra8_err_t ra8_flash_extra_mram_erase(uint32_t mram_addr)
{
  if ((mram_addr & (k_ra8_mram_block_size_bytes - 1U)) != 0U) {
    return k_ra8_err_invalid_arg;
  }
  static const uint8_t s_ones[k_ra8_mram_block_size_bytes] = {
    0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU,
    0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU,
    0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU,
  };
  return ra8_flash_extra_mram_write(mram_addr, s_ones, k_ra8_mram_block_size_bytes);
}
