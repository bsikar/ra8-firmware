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
 *  - Anti-rollback counters moved to flash_arc_abi.zig (RA8FW-802).
 *  - MSUINITR kick and clock-frequency update (HUM Ch 59 p 3551..3572).
 *    Zeroize, MSAR, ECC controls, error addresses and the update
 *    transfer moved to flash_ctl_abi.zig (RA8FW-806).
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

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_flash.h"
#include "ra8_flash_internal.h"
#include "ra8_flash_regs.h"
#include "ra8_hw_err.h"
#include "ra8_log.h"

/** @brief Blank-flash fill byte. */
typedef enum : uint32_t {
  k_flash_blank_byte = 0xFFU, /**< Erased-flash fill byte. */
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
    /* HUM Ch 59 "MSUACR : Start-Up Area Control Register" p 3574 */
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
  RA8_CHECK_NULL_PTR(out_btflg, g_flash_tag, "out_btflg must not be nullptr");
  RA8_CHECK_NULL_PTR(out_fspr, g_flash_tag, "out_fspr must not be nullptr");
  /* HUM Ch 59 "MSUASMON : Start-Up Area Monitor" p 3573 */
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
  RA8_CHECK_NULL_PTR(words, g_flash_tag, "words must not be nullptr");
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

  /* HUM Ch 59.5.19 "MSADDR : MACI Command Start Address Register" p 3564 */
  *ra8_mram_reg32(k_ra8_mram_off_msaddr) = target_addr;
  /* Opener: Program (0xE8) for the extra-MRAM data area, else Configuration
   * Set (0x40) for the OFS config area. HUM Ch 59.7.4.5 Fig 59.13 p 3591 /
   * HUM Ch 59.7.4.8 p 3594. */
  const uint8_t opener =
    in_extra ? (uint8_t)k_ra8_maci_cmd_program : (uint8_t)k_ra8_maci_cmd_config_set;
  priv_ra8_flash_internal_maci_cmd8(opener);
  priv_ra8_flash_internal_maci_cmd8(k_ra8_maci_cmd_word_count_n);

  /* N = k_ra8_mram_config_set_word_count halfwords of payload.
   * HUM Ch 59.7.4.5 Fig 59.13 p 3592 / HUM Ch 59.7.4.8 p 3595. */
  for (uint32_t i = 0U; i < k_ra8_mram_config_set_word_count; ++i) {
    priv_ra8_flash_internal_maci_cmd16(words[i]);
  }
  /* Trailer 0xD0 starts command processing. HUM Ch 59.7.4.5 Fig 59.13 p 3592. */
  priv_ra8_flash_internal_maci_cmd8(k_ra8_maci_cmd_final);

  ra8_err_t err = priv_ra8_flash_internal_wait_mrdy(k_ra8_flash_maci_spin_limit);
  if (err != k_ra8_ok) {
    return err;
  }

  /* HUM Ch 59 "MSTATR : Extra MRAM Status Register" p 3568 */
  const uint32_t s = *ra8_mram_reg32(k_ra8_mram_off_mstatr);
  if ((s & k_ra8_mstatr_mask_any_err) != 0U) {
    return k_ra8_err_hw_error;
  }
  return k_ra8_ok;
}

/* Anti-rollback counters (ra8_flash_arc_increment / ra8_flash_arc_read)
 * live in libs/ra8_hal/src/flash_arc_abi.zig (RA8FW-802). */

/* =============================================================================
 * Public API: MSUINITR kick, clock-frequency update
 * =============================================================================
 */

ra8_err_t ra8_flash_msuinitr_kick(void)
{
  /* HUM Ch 59 "MSUINITR : Extra MRAM Sequencer Set-Up Init" p 3572 */
  *ra8_mram_reg16(k_ra8_mram_off_msuinitr) = k_ra8_msuinitr_full_init;

  for (uint32_t i = 0U; i < k_ra8_flash_pe_spin_limit; ++i) {
    /* HUM Ch 59 "MSUINITR : Extra MRAM Sequencer Set-Up Init" p 3572 */
    const uint16_t v = *ra8_mram_reg16(k_ra8_mram_off_msuinitr);
#if defined(RA8_OFF_TARGET) && defined(UNIT_TEST)
    /* Host MMIO fault seam: on real HW the sequencer auto-clears
     * SUINIT once the init completes; host RAM cannot, so the seam
     * owns the loop-exit decision (first-poll success unless a test
     * arms a fault to drive the retry / timeout legs). */
    if (ra8_fake_mmio_wait_eval(ra8_mram_reg16(k_ra8_mram_off_msuinitr),
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

ra8_err_t ra8_flash_update_clock_freq(uint16_t mrcfreq_mhz, uint8_t mrefreq_mhz)
{
  if (mrcfreq_mhz > (uint16_t)k_ra8_flash_max_mrcfreq_mhz) {
    return k_ra8_err_invalid_arg;
  }
  if (mrefreq_mhz > (uint8_t)k_ra8_flash_max_mrefreq_mhz) {
    return k_ra8_err_invalid_arg;
  }
  const bool prefetch_was = g_flash_rt.prefetch_on;
  priv_ra8_flash_internal_set_prefetch(false);

  /* HUM Ch 59.5.2 "MRCFREQ : Code MRAM Frequency Notifications Register" p 3551 */
  *ra8_mram_reg32(k_ra8_mram_off_mrcfreq) =
    (k_ra8_flash_mrcfreq_key << k_ra8_flash_freq_key_shift) | (uint32_t)mrcfreq_mhz;
  /* HUM Ch 59.5.3 "MREFREQ : Extra MRAM Frequency Notifications Register" p 3552 */
  *ra8_mram_reg32(k_ra8_mram_off_mrefreq) =
    (k_ra8_flash_mrefreq_key << k_ra8_flash_freq_key_shift) | (uint32_t)mrefreq_mhz;

  priv_ra8_flash_internal_set_prefetch(prefetch_was);
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
RA8_INTERNAL static void
internal_pack_config_words(const uint8_t* src,
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
  RA8_CHECK_NULL_PTR(src, g_flash_tag, "src must not be nullptr");
  if (len == 0U || len > k_ra8_mram_write_size_bytes) {
    return k_ra8_err_invalid_arg;
  }
  if (mram_addr < (uint32_t)k_ra8_flash_extra_start) {
    return k_ra8_err_invalid_arg;
  }
  const uint32_t end_excl = (uint32_t)((uint64_t)mram_addr + (uint64_t)len);
  /* OTP-misuse guard: cap the general-purpose write path at
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
    internal_pack_config_words(src, len, done, cfg_words);
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
  static const uint8_t local_ones[k_ra8_mram_block_size_bytes] = {
    0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU,
    0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU,
    0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU,
  };
  return ra8_flash_extra_mram_write(mram_addr, local_ones, k_ra8_mram_block_size_bytes);
}
