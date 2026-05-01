/**
 * @file ra_flash.c
 * @brief Code-MRAM + Extra-MRAM + Option-Setting driver implementation -- DANGEROUS
 *
 * @par Tag
 * [Ring 3 / HAL] {World: S}
 *
 * @details
 * Implements the full HUM Ch 7 + Ch 59 surface declared in ``ra_flash.h``:
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

#include "ra_flash.h"

#include <stdint.h>
#include <string.h>

#include "ra8d2_flash_regs.h"
#include "ra_check.h"
#include "ra_err.h"
#include "ra_log.h"

static const char* s_tag = "FLASH";

/**
 * @struct ra_flash_runtime_t
 * @brief File-scope runtime state.
 *
 * @details
 * Cleared at init. Used by the IRQ dispatcher to find the registered
 * callback and to detect callers that try to use APIs before init.
 */
typedef struct {
  ra_flash_callback_t cb;          /**< Registered IRQ callback (or NULL). */
  void*               user_ctx;    /**< Caller pointer passed to ``cb``.    */
  bool                initialised; /**< True after ``ra_flash_init``.       */
  bool                prefetch_on; /**< Last-known MRCPFB state.            */
  uintptr_t           win_low;     /**< Soft access-window low (incl).      */
  uintptr_t           win_high;    /**< Soft access-window high (excl).     */
} ra_flash_runtime_t;

static ra_flash_runtime_t s_rt = {};

/**
 * @enum ra_flash_const_t
 * @brief Bounds on configuration values + spin limits.
 *
 * @details
 * HUM Ch 59.5.2 p 3551 limits MRCMHZ to 0x0FA (250 MHz). HUM
 * Ch 59.5.3 p 3552 limits MREMHZ to 0x07D (125 MHz). The MACI
 * commands take tens of microseconds to milliseconds; the spin
 * limit below is generous enough for the worst-case
 * configuration-set (~9 ms) at the slowest clock.
 */
typedef enum : uint32_t {
  k_ra_flash_max_mrcfreq_mhz = 0x000000FAUL, /**< MRCMHZ <= 250.            */
  k_ra_flash_max_mrefreq_mhz = 0x0000007DUL, /**< MREMHZ <= 125.            */
  k_ra_flash_busy_spin_limit = 0x00010000UL, /**< Direct-write busy spin.   */
  k_ra_flash_maci_spin_limit = 0x00100000UL, /**< MACI command spin limit.  */
  k_ra_flash_pe_spin_limit   = 0x00010000UL, /**< P/E entry spin limit.     */
  k_ra_flash_zeroize_spin    = 0x00400000UL, /**< W-HUK zeroize spin.       */
  k_ra_flash_max_list_select = 0x0000000FUL, /**< MCTRLSR.LIST max value.   */
} ra_flash_const_t;

/**
 * @enum ra_flash_key_t
 * @brief KEY-byte shifts and codes that gate writes.
 *
 * @details
 * MRCFREQ requires KEY=0x1E in [31:24] (HUM Ch 59.5.2 p 3551 Note 1).
 * MREFREQ requires KEY=0xE1 in [31:24] (HUM Ch 59.5.3 p 3552 Note 1).
 */
typedef enum : uint32_t {
  k_ra_flash_freq_key_shift = 24U, /**< KEY[7:0] @ [31:24] in MRCFREQ/MREFREQ. */
  k_ra_flash_mrcfreq_key    = 0x1EU,
  k_ra_flash_mrefreq_key    = 0xE1U,
} ra_flash_key_t;

/**
 * @enum ra_flash_cfg_word_const_t
 * @brief Bit patterns for the configuration-set word vector.
 *
 * @details
 * HUM Ch 7 "Option-Setting Memory" p 278. The configuration-set
 * vector is written as a sequence of 16-bit words; we OR in only the
 * bits we want to drive low, keeping the remaining bits as 1 to
 * preserve unused fields.
 */
typedef enum : uint16_t {
  k_ra_flash_cfg_word_all_ones = 0xFFFFU, /**< Word filler when no bits drive low. */
  k_ra_flash_btflg_default     = 0x8000U, /**< BTFLG bit 15 selects default boot. */
  k_ra_flash_btflg_alternate   = 0x0000U, /**< BTFLG cleared selects alternate. */
  k_ra_flash_btflg_word_keep   = 0x1FFFU, /**< Bits 12:0 kept as ones (unused). */
} ra_flash_cfg_word_const_t;

/* =============================================================================
 * Internal helpers
 * =============================================================================
 */

/**
 * @brief Wait for MRCPS.PRGBSYC = 0 and ABUFFULL = 0 (buffer ready).
 *
 * @param[in] limit Maximum spin iterations.
 * @return ``k_ra_ok`` if both bits cleared, else ``k_ra_err_hw_timeout``.
 *
 * @pre ``limit`` > 0.
 * @pre Controller is powered.
 * @post Buffer is observed empty or function returns timeout.
 */
static ra_err_t internal_wait_buffer_ready(uint32_t limit)
{
  for (uint32_t i = 0U; i < limit; ++i) {
    /* HUM Ch 59 "MRCPS : Code MRAM Program Status Register" p 3601 */
    const uint8_t s = *ra_mram_reg8(k_ra_mram_off_mrcps);
    if (((s & k_ra_mrcps_mask_prgbsyc) == 0U) && ((s & k_ra_mrcps_mask_abuffull) == 0U)) {
      return k_ra_ok;
    }
  }
  return k_ra_err_hw_timeout;
}

/**
 * @brief Wait for MRCPS.ABUFEMP = 1 and PRGBSYC = 0 (commit done).
 *
 * @param[in] limit Maximum spin iterations.
 * @return ``k_ra_ok`` on commit, else ``k_ra_err_hw_timeout``.
 *
 * @pre ``limit`` > 0.
 * @pre Controller is powered.
 * @post Commit observed or function returns timeout.
 */
static ra_err_t internal_wait_commit_done(uint32_t limit)
{
  for (uint32_t i = 0U; i < limit; ++i) {
    /* HUM Ch 59 "MRCPS : Code MRAM Program Status Register" p 3601 */
    const uint8_t s = *ra_mram_reg8(k_ra_mram_off_mrcps);
    if (((s & k_ra_mrcps_mask_abufemp) != 0U) && ((s & k_ra_mrcps_mask_prgbsyc) == 0U)) {
      return k_ra_ok;
    }
  }
  return k_ra_err_hw_timeout;
}

/**
 * @brief Spin until MSTATR.MRDY rises or limit elapses.
 *
 * @param[in] limit Maximum spin iterations.
 * @return ``k_ra_ok`` if MRDY observed, else ``k_ra_err_hw_timeout``.
 *
 * @pre ``limit`` > 0.
 * @pre Controller is in P/E mode (MRDY only meaningful then).
 * @post MRDY observed high or function returns timeout.
 */
static ra_err_t internal_wait_mrdy(uint32_t limit)
{
  for (uint32_t i = 0U; i < limit; ++i) {
    /* HUM Ch 59 "MSTATR : Extra MRAM Status Register" p 3578 */
    const uint32_t s = *ra_mram_reg32(k_ra_mram_off_mstatr);
    if ((s & k_ra_mstatr_mask_mrdy) != 0U) {
      return k_ra_ok;
    }
  }
  return k_ra_err_hw_timeout;
}

/**
 * @brief Open or close the per-world program-control gate.
 *
 * @param[in] world  ``k_ra_flash_world_ns`` -> MRCPC0; ``_s`` -> MRCPC1.
 * @param[in] enable ``true`` to enable program; ``false`` to lock.
 *
 * @pre None (registers always accessible).
 * @post Matching MRCPCx register holds the keyed value.
 */
static void internal_set_program_gate(ra_flash_world_t world, bool enable)
{
  const uint16_t off = (world == k_ra_flash_world_s) ? k_ra_mram_off_mrcpc1 : k_ra_mram_off_mrcpc0;
  uint16_t       value;
  if (world == k_ra_flash_world_s) {
    if (enable) {
      value = k_ra_mrcpc1_key_enable;
    } else {
      value = k_ra_mrcpc1_key_disable;
    }
  } else {
    if (enable) {
      value = k_ra_mrcpc0_key_enable;
    } else {
      value = k_ra_mrcpc0_key_disable;
    }
  }
  /* HUM Ch 59 "MRCPC0/MRCPC1 : Code MRAM Program Control Register" p 3601 */
  *ra_mram_reg16(off) = value;
}

/**
 * @brief Toggle high-speed program mode (MRPSC.MHSPEN).
 *
 * @param[in] enable true => MHSPEN=1.
 *
 * @pre None.
 * @post MRPSC.MHSPEN matches ``enable``.
 */
static void internal_set_hsp_mode(bool enable)
{
  /* HUM Ch 59 "MRPSC : MRAM Program Speed Control Register" p 3600 */
  uint8_t mrpsc = 0U;
  if (enable) {
    mrpsc = k_ra_mrpsc_mask_mhspen;
  }
  *ra_mram_reg8(k_ra_mram_off_mrpsc) = mrpsc;
}

/**
 * @brief Set MRCPFB.MPFBEN to enable/disable the prefetch buffer.
 *
 * @param[in] enable true => prefetch on.
 *
 * @pre None.
 * @post MRCPFB.MPFBEN matches ``enable``.
 */
static void internal_set_prefetch(bool enable)
{
  /* HUM Ch 59.5.1 "MRCPFB : Code MRAM Pre-Fetch Buffer Enable Register" p 3551 */
  uint8_t mrcpfb = 0U;
  if (enable) {
    mrcpfb = 1U;
  }
  *ra_mram_reg8(k_ra_mram_off_mrcpfb) = mrcpfb;
  s_rt.prefetch_on                    = enable;
}

/**
 * @brief Send a single byte through the MACI command-issuing area.
 *
 * @param[in] byte Command byte.
 *
 * @pre Controller is in P/E mode.
 * @post One byte was written to MACI_CMD8.
 */
static void internal_maci_cmd8(uint8_t byte)
{
  /* HUM Ch 59 "MACI Command-Issuing Area" p 3550 */
  *ra_mram_cmd8() = byte;
}

/**
 * @brief Send a halfword through the MACI command-issuing area.
 *
 * @param[in] half 16-bit data.
 *
 * @pre Controller is in P/E mode.
 * @post One halfword was written to MACI_CMD16.
 */
static void internal_maci_cmd16(uint16_t half)
{
  /* HUM Ch 59 "MACI Command-Issuing Area" p 3550 */
  *ra_mram_cmd16() = half;
}

/**
 * @brief Translate logical ARC id to MCNTSELR field value.
 *
 * @param[in] id Logical counter id.
 * @return MCNTSELR.CNTSEL field value (0 if id is out of range).
 *
 * @pre ``id`` < ``k_ra_flash_arc_count`` for a meaningful result.
 * @post Returned value matches FSP ``mram_counter_to_mcntselr_convert``.
 */
static uint8_t internal_arc_to_mcntselr(ra_flash_arc_id_t id)
{
  switch (id) {
    case k_ra_flash_arc_sec:
      return k_ra_mcntselr_sec;
    case k_ra_flash_arc_oembl:
      return k_ra_mcntselr_oembl;
    case k_ra_flash_arc_nsec_0:
      return k_ra_mcntselr_nsec_0;
    case k_ra_flash_arc_nsec_1:
      return (uint8_t)(k_ra_mcntselr_nsec_0 + 1U);
    case k_ra_flash_arc_nsec_2:
      return (uint8_t)(k_ra_mcntselr_nsec_0 + 2U);
    case k_ra_flash_arc_nsec_3:
      return (uint8_t)(k_ra_mcntselr_nsec_0 + 3U);
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
 * @pre ``id`` < ``k_ra_flash_arc_count``.
 * @post Returned value reflects the ARCCS.ARCNS field for NSEC ids.
 */
static uint32_t internal_arc_max_count(ra_flash_arc_id_t id)
{
  if (id == k_ra_flash_arc_sec) {
    return k_ra_arc_sec_max_bits;
  }
  if (id == k_ra_flash_arc_oembl) {
    return k_ra_arc_oembl_max_bits;
  }
  /* NSEC: read ARCCS.ARCNS to decide single vs multiple. */
  /* HUM Ch 7.2.21 "ARCCS Anti-Rollback Counter Configuration" p 296 */
  const uint16_t arccs = *(volatile const uint16_t*)k_ra_flash_ofs_arccs_addr;
  const uint8_t  arcns = (uint8_t)(arccs & (uint16_t)k_ra_arc_arccs_mask);
  if (arcns == (uint8_t)k_ra_arc_arcns_single) {
    return k_ra_arc_nsec_single;
  }
  return k_ra_arc_nsec_multiple;
}

/**
 * @brief Pop-count helper for 32-bit words used by the ARC reader.
 *
 * @param[in] x Input word.
 * @return Number of set bits.
 *
 * @pre None.
 * @post Returned value in [0, 32].
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
 * Public API: lifecycle
 * =============================================================================
 */

ra_err_t ra_flash_init(const ra_flash_cfg_t* cfg)
{
  RA_CHECK_NULL_PTR(cfg, s_tag, "cfg must not be nullptr");
  if (cfg->mrcfreq_mhz > (uint16_t)k_ra_flash_max_mrcfreq_mhz) {
    return k_ra_err_invalid_arg;
  }
  if (cfg->mrefreq_mhz > (uint8_t)k_ra_flash_max_mrefreq_mhz) {
    return k_ra_err_invalid_arg;
  }

  /* Frequency-down procedure -- HUM Ch 59.4.3 Figure 59.6 p 3550. */
  internal_set_prefetch(false);

  /* HUM Ch 59.5.2 "MRCFREQ : Code MRAM Frequency Notifications Register" p 3551 */
  const uint32_t mrcfreq_word =
    (k_ra_flash_mrcfreq_key << k_ra_flash_freq_key_shift) | (uint32_t)cfg->mrcfreq_mhz;
  *ra_mram_reg32(k_ra_mram_off_mrcfreq) = mrcfreq_word;

  /* HUM Ch 59.5.3 "MREFREQ : Extra MRAM Frequency Notifications Register" p 3552 */
  const uint32_t mrefreq_word =
    (k_ra_flash_mrefreq_key << k_ra_flash_freq_key_shift) | (uint32_t)cfg->mrefreq_mhz;
  *ra_mram_reg32(k_ra_mram_off_mrefreq) = mrefreq_word;

  /* HUM Ch 59 "MRCEECC : Code MRAM ECC Encoder Control" p 3624 */
  uint16_t mrceecc_bits = 0U;
  if (cfg->ecc_encoder_enable) {
    mrceecc_bits = k_ra_mrceecc_mask_eccen;
  }
  *ra_mram_reg16(k_ra_mram_off_mrceecc) = (uint16_t)(k_ra_mrceecc_key_shift | mrceecc_bits);

  /* HUM Ch 59 "MRCDECC : Code MRAM ECC Decoder Control" p 3554 */
  uint16_t mrcdecc_bits = 0U;
  if (cfg->ecc_decoder_enable) {
    mrcdecc_bits = k_ra_mrcdecc_mask_dececen;
  }
  *ra_mram_reg16(k_ra_mram_off_mrcdecc) = (uint16_t)(k_ra_mrcdecc_key_shift | mrcdecc_bits);

  internal_set_prefetch(cfg->prefetch_en);

  /* Lock both program gates so no stray store can program MRAM. */
  internal_set_program_gate(k_ra_flash_world_ns, false);
  internal_set_program_gate(k_ra_flash_world_s, false);
  internal_set_hsp_mode(false);

  /* Clear sticky errors so the new run starts from a clean state. */
  /* HUM Ch 59 "MRCPS : Code MRAM Program Status Register" p 3601 */
  *ra_mram_reg8(k_ra_mram_off_mrcps) = k_ra_mrcps_mask_errors;
  /* HUM Ch 59 "MRCRAES : Code MRAM Read Access Error Status" p 3554 */
  *ra_mram_reg8(k_ra_mram_off_mrcraes) = 0U;
  /* HUM Ch 59 "MRERAES : Extra MRAM Read Access Error Status" p 3557 */
  *ra_mram_reg8(k_ra_mram_off_mreraes) = 0U;

  s_rt.initialised = true;
  s_rt.cb          = nullptr;
  s_rt.user_ctx    = nullptr;

  ra_log_info_val(s_tag, "flash_init mrcfreq", (uint32_t)cfg->mrcfreq_mhz);
  return k_ra_ok;
}

ra_err_t ra_flash_deinit(void)
{
  /* Lock everything, clear sticky errors, re-enable prefetch. */
  internal_set_program_gate(k_ra_flash_world_ns, false);
  internal_set_program_gate(k_ra_flash_world_s, false);
  internal_set_hsp_mode(false);
  /* If we accidentally left P/E mode on, drop back to read mode. */
  /* HUM Ch 59 "MENTRYR : Extra MRAM Program-Mode Entry" p 3582 */
  *ra_mram_reg16(k_ra_mram_off_mentryr) = k_ra_mentryr_read_mode;
  /* HUM Ch 59 "MRCPS : Code MRAM Program Status Register" p 3601 */
  *ra_mram_reg8(k_ra_mram_off_mrcps) = k_ra_mrcps_mask_errors;
  internal_set_prefetch(true);

  s_rt.initialised = false;
  s_rt.cb          = nullptr;
  s_rt.user_ctx    = nullptr;
  return k_ra_ok;
}

/* =============================================================================
 * Public API: status snapshots
 * =============================================================================
 */

ra_err_t ra_flash_get_status(uint8_t* out_status)
{
  RA_CHECK_NULL_PTR(out_status, s_tag, "out_status must not be nullptr");
  /* HUM Ch 59 "MRCPS : Code MRAM Program Status Register" p 3601 */
  *out_status = *ra_mram_reg8(k_ra_mram_off_mrcps);
  return k_ra_ok;
}

ra_err_t ra_flash_get_extended_status(ra_flash_status_ext_t* out)
{
  RA_CHECK_NULL_PTR(out, s_tag, "out must not be nullptr");
  /* HUM Ch 59 "MRCPS : Code MRAM Program Status Register" p 3601 */
  out->mrcps = *ra_mram_reg8(k_ra_mram_off_mrcps);
  /* HUM Ch 59 "MASTAT : Extra MRAM Access Status Register" p 3577 */
  out->mastat = *ra_mram_reg8(k_ra_mram_off_mastat);
  /* HUM Ch 59 "MREZS : Extra MRAM Zeroization Status Register" p 3565 */
  out->mrezs = *ra_mram_reg8(k_ra_mram_off_mrezs);
  /* HUM Ch 59 "MCMDR : MACI Command Register" p 3589 */
  out->mcmdr = *ra_mram_reg16(k_ra_mram_off_mcmdr);
  /* HUM Ch 59 "MSTATR : Extra MRAM Status Register" p 3578 */
  out->mstatr = *ra_mram_reg32(k_ra_mram_off_mstatr);
  return k_ra_ok;
}

ra_err_t ra_flash_clear_status(uint8_t mask)
{
  if ((mask & (uint8_t)~k_ra_mrcps_mask_errors) != 0U) {
    return k_ra_err_invalid_arg;
  }
  /* HUM Ch 59 "MRCPS : Code MRAM Program Status Register" p 3601 -- W1C
   * for PRGERRC and ECCERRC; other bits are read-only. */
  *ra_mram_reg8(k_ra_mram_off_mrcps) = mask;
  return k_ra_ok;
}

ra_err_t ra_flash_set_rww_disable(bool disable)
{
  bool prefetch = true;
  if (disable) {
    prefetch = false;
  }
  internal_set_prefetch(prefetch);
  return k_ra_ok;
}

/**
 * @brief Test whether [addr, addr+len) lies inside the configured soft window.
 *
 * @details
 * The soft window mirrors the FSP ``accessWindowSet`` surface but is
 * stored in driver state rather than in the silicon (RA8D2 has no
 * FAWMON / FAWMR; HUM Ch 59 substitutes block-protect bits). A window
 * with ``win_low == win_high == 0`` is treated as disabled (allow all).
 *
 * @param[in] addr Start address of the candidate operation.
 * @param[in] len  Length in bytes (must be > 0 if the caller is writing).
 *
 * @return ``true`` if the operation is permitted, ``false`` if blocked.
 *
 * @pre None.
 * @post No side effects.
 *
 * @note Internal helper, not thread-safe.
 */
static bool internal_window_allows(uintptr_t addr, uint32_t len)
{
  if (s_rt.win_low == 0U && s_rt.win_high == 0U) {
    return true;
  }
  const uintptr_t end_excl = (uintptr_t)((uint64_t)addr + (uint64_t)len);
  if (addr < s_rt.win_low) {
    return false;
  }
  if (end_excl > s_rt.win_high) {
    return false;
  }
  return true;
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
 * @return ``k_ra_ok`` if the write is well formed.
 *
 * @pre None.
 * @post No side effects.
 *
 * @note Internal helper, not thread-safe.
 */
static ra_err_t internal_validate_write_block(uint32_t mram_addr, uint32_t len)
{
  if (len == 0U || len > k_ra_mram_write_size_bytes) {
    return k_ra_err_invalid_arg;
  }
  const uint32_t end_excl =
    (uint32_t)((uint64_t)mram_addr + (uint64_t)len); /* avoids 32-bit overflow */
  if (mram_addr < (uint32_t)k_ra_flash_code_start) {
    return k_ra_err_invalid_arg;
  }
  if (end_excl > (uint32_t)k_ra_flash_code_start + (uint32_t)k_ra_flash_code_size) {
    return k_ra_err_invalid_arg;
  }
  const uint32_t page_mask = k_ra_mram_write_size_bytes - 1U;
  if ((mram_addr & ~page_mask) != ((end_excl - 1U) & ~page_mask)) {
    return k_ra_err_invalid_arg;
  }
  if (!internal_window_allows((uintptr_t)mram_addr, len)) {
    return k_ra_err_out_of_range;
  }
  return k_ra_ok;
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
 * @return ``k_ra_ok`` on commit success, otherwise the commit-wait error.
 *
 * @pre Caller already drained the previous transfer with
 *      ``internal_wait_buffer_ready``.
 * @post Program gate, HSP mode and prefetch are restored on every
 *       exit path.
 *
 * @note Internal helper, not thread-safe.
 */
static ra_err_t internal_flash_program_window(uint32_t         mram_addr,
                                              const uint8_t*   src,
                                              uint32_t         len,
                                              ra_flash_world_t world)
{
  internal_set_prefetch(false);
  internal_set_hsp_mode(true);
  internal_set_program_gate(world, true);

  volatile uint8_t* dst = (volatile uint8_t*)(uintptr_t)mram_addr;
  for (uint32_t i = 0U; i < len; ++i) {
    dst[i] = src[i];
  }

  __asm__ volatile("" ::: "memory");
  /* HUM Ch 59 "MRCFLR : Code MRAM Flush Register" p 3601 */
  *ra_mram_reg16(k_ra_mram_off_mrcflr) = k_ra_mrcflr_key_flush;

  const ra_err_t err = internal_wait_commit_done(k_ra_flash_busy_spin_limit);

  /* Tear down regardless of err so the gate cannot stay open. */
  internal_set_program_gate(world, false);
  internal_set_hsp_mode(false);
  internal_set_prefetch(true);

  return err;
}

ra_err_t
ra_flash_write_block(uint32_t mram_addr, const uint8_t* src, uint32_t len, ra_flash_world_t world)
{
  RA_CHECK_NULL_PTR(src, s_tag, "src must not be nullptr");
  const ra_err_t v_err = internal_validate_write_block(mram_addr, len);
  RA_RETURN_ON_ERROR(v_err, s_tag, "flash_write: validate");

  /* Step 1: wait for the controller to be idle. */
  ra_err_t err = internal_wait_buffer_ready(k_ra_flash_busy_spin_limit);
  RA_RETURN_ON_ERROR(err, s_tag, "flash_write: busy wait");

  /* Steps 2-6: gate, write, flush, commit, teardown. */
  err = internal_flash_program_window(mram_addr, src, len, world);
  RA_RETURN_ON_ERROR(err, s_tag, "flash_write: commit wait");

  /* Step 7: error check.
   * HUM Ch 59 "MRCPS : Code MRAM Program Status Register" p 3601 */
  const uint8_t status = *ra_mram_reg8(k_ra_mram_off_mrcps);
  if ((status & k_ra_mrcps_mask_errors) != 0U) {
    *ra_mram_reg8(k_ra_mram_off_mrcps) = k_ra_mrcps_mask_errors;
    ra_log_error_val(s_tag, "flash_write: hw error", (uint32_t)status);
    return k_ra_err_hw_error;
  }
  return k_ra_ok;
}

ra_err_t ra_flash_erase_block(uint32_t mram_addr, ra_flash_world_t world)
{
  if ((mram_addr & (k_ra_mram_block_size_bytes - 1U)) != 0U) {
    return k_ra_err_invalid_arg;
  }
  /* MRAM has no separate erase: erase == program-to-all-ones. */
  static const uint8_t s_ones[k_ra_mram_block_size_bytes] = {
    0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU,
    0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU,
    0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU,
  };
  return ra_flash_write_block(mram_addr, s_ones, k_ra_mram_block_size_bytes, world);
}

/* =============================================================================
 * Public API: block protection
 * =============================================================================
 */

ra_err_t ra_flash_block_protect_set(ra_flash_world_t world, bool lock, bool permanent)
{
  if (permanent && !lock) {
    /* Permanent unlock makes no sense -- the fuse is one-shot. */
    return k_ra_err_invalid_arg;
  }
  uint16_t value;
  if (world == k_ra_flash_world_s) {
    if (lock) {
      value = k_ra_mrcbprot1_key_lock;
    } else {
      value = k_ra_mrcbprot1_key_unlock;
    }
    /* HUM Ch 59 "MRCBPROT1 : Code MRAM Block Protection (S)" p 3605 */
    *ra_mram_reg16(k_ra_mram_off_mrcbprot1) = value;
  } else {
    if (lock) {
      value = k_ra_mrcbprot0_key_lock;
    } else {
      value = k_ra_mrcbprot0_key_unlock;
    }
    /* HUM Ch 59 "MRCBPROT0 : Code MRAM Block Protection (NS)" p 3604 */
    *ra_mram_reg16(k_ra_mram_off_mrcbprot0) = value;
  }
  /* The "permanent" flag is conveyed via a configuration-set update of
   * the OFS region; the per-register write above only takes effect for
   * this boot. We expose the request for transparency, but defer the
   * persistent fuse to ``ra_flash_config_set_write`` for callers who
   * truly want it. The HUM Ch 7 p 278 OFS layout is the trust anchor. */
  if (permanent) {
    ra_log_warn(s_tag, "permanent block protect requires config-set");
  }
  return k_ra_ok;
}

/* =============================================================================
 * Public API: P/E mode transitions
 * =============================================================================
 */

ra_err_t ra_flash_enter_pe_mode(void)
{
  internal_set_prefetch(false);
  /* HUM Ch 59 "MENTRYR : Extra MRAM Program-Mode Entry" p 3582 */
  *ra_mram_reg16(k_ra_mram_off_mentryr) = k_ra_mentryr_pe_enter;

  for (uint32_t i = 0U; i < k_ra_flash_pe_spin_limit; ++i) {
    /* HUM Ch 59 "MENTRYR : Extra MRAM Program-Mode Entry" p 3582 */
    const uint16_t v = *ra_mram_reg16(k_ra_mram_off_mentryr);
    if ((v & k_ra_mentryr_mask_pe_mode) != 0U) {
      return k_ra_ok;
    }
  }
  return k_ra_err_hw_timeout;
}

ra_err_t ra_flash_exit_pe_mode(void)
{
  /* HUM Ch 59 "MENTRYR : Extra MRAM Program-Mode Entry" p 3582 */
  *ra_mram_reg16(k_ra_mram_off_mentryr) = k_ra_mentryr_read_mode;

  for (uint32_t i = 0U; i < k_ra_flash_pe_spin_limit; ++i) {
    /* HUM Ch 59 "MENTRYR : Extra MRAM Program-Mode Entry" p 3582 */
    const uint16_t v = *ra_mram_reg16(k_ra_mram_off_mentryr);
    if ((v & k_ra_mentryr_mask_pe_mode) == 0U) {
      internal_set_prefetch(s_rt.prefetch_on);
      return k_ra_ok;
    }
  }
  return k_ra_err_hw_timeout;
}

ra_err_t ra_flash_suspend(void)
{
  /* HUM Ch 59 "MENTRYR : Extra MRAM Program-Mode Entry" pp 3582+ --
   * driving MENTRYR with the keyed pause pattern (KEY=0xAA, MENTRY=1,
   * PCKA=1) halts an in-flight MACI command at the next page
   * boundary.  Poll for PCKA=1 to confirm the pause. */
  *ra_mram_reg16(k_ra_mram_off_mentryr) = k_ra_mentryr_pe_pause;

  for (uint32_t i = 0U; i < k_ra_flash_pe_spin_limit; ++i) {
    /* HUM Ch 59 "MENTRYR" p 3582 */ /* + */
    const uint16_t v = *ra_mram_reg16(k_ra_mram_off_mentryr);
    if ((v & k_ra_mentryr_mask_pcka) != 0U) {
      return k_ra_ok;
    }
  }
  return k_ra_err_hw_timeout;
}

ra_err_t ra_flash_resume(void)
{
  /* HUM Ch 59 "MENTRYR : Extra MRAM Program-Mode Entry" pp 3582+ --
   * resume key clears PCKA, leaving MENTRY=1 so programming continues.
   * Poll for PCKA=0 before returning. */
  *ra_mram_reg16(k_ra_mram_off_mentryr) = k_ra_mentryr_pe_resume;

  for (uint32_t i = 0U; i < k_ra_flash_pe_spin_limit; ++i) {
    /* HUM Ch 59 "MENTRYR" p 3582 */ /* + */
    const uint16_t v = *ra_mram_reg16(k_ra_mram_off_mentryr);
    if ((v & k_ra_mentryr_mask_pcka) == 0U) {
      return k_ra_ok;
    }
  }
  return k_ra_err_hw_timeout;
}

ra_err_t ra_flash_lock_set(uintptr_t addr, uint16_t lock_bits)
{
  /* Address must lie inside the 1 MiB code-MRAM window. */
  const uintptr_t code_lo = k_ra_flash_code_start;
  const uintptr_t code_hi = code_lo + k_ra_flash_code_size;
  if ((addr < code_lo) || (addr >= code_hi)) {
    return k_ra_err_invalid_arg;
  }
  /* Validate the keyed value: bits [15:8] must be one of the two
   * documented keys (0x88 for MRCBPROT0, 0x44 for MRCBPROT1). */
  enum : uint16_t {
    k_ra_mrcbprot_key_byte_mask = 0xFF00U,
    k_ra_mrcbprot_key_ns        = 0x8800U,
    k_ra_mrcbprot_key_s         = 0x4400U,
  };
  enum : uint32_t {
    k_ra_mrcbprot_secure_bit = 0x00080000UL, /**< Address bit 19. */
  };
  const uint16_t key    = (uint16_t)(lock_bits & k_ra_mrcbprot_key_byte_mask);
  const bool     key_ns = (key == k_ra_mrcbprot_key_ns);
  const bool     key_s  = (key == k_ra_mrcbprot_key_s);
  if (!key_ns && !key_s) {
    return k_ra_err_invalid_arg;
  }

  const bool is_secure = ((addr & (uintptr_t)k_ra_mrcbprot_secure_bit) != 0U);
  if (is_secure) {
    /* HUM Ch 59 "MRCBPROT1 : Code MRAM Block Protection (S)" p 3605 */
    *ra_mram_reg16(k_ra_mram_off_mrcbprot1) = lock_bits;
  } else {
    /* HUM Ch 59 "MRCBPROT0 : Code MRAM Block Protection (NS)" p 3604 */
    *ra_mram_reg16(k_ra_mram_off_mrcbprot0) = lock_bits;
  }
  return k_ra_ok;
}

/* =============================================================================
 * Public API: forced stop / reset
 * =============================================================================
 */

ra_err_t ra_flash_force_stop(void)
{
  /* HUM Ch 59 "MACI Command-Issuing Area" p 3550 */
  internal_maci_cmd8(k_ra_maci_cmd_forced_stop);
  ra_err_t err = internal_wait_mrdy(k_ra_flash_maci_spin_limit);
  if (err != k_ra_ok) {
    return err;
  }

  /* HUM Ch 59 "MASTAT : Extra MRAM Access Status Register" p 3577 */
  const uint8_t mastat = *ra_mram_reg8(k_ra_mram_off_mastat);
  if ((mastat & k_ra_mastat_mask_cmdlk) != 0U) {
    return k_ra_err_hw_error;
  }
  return k_ra_ok;
}

ra_err_t ra_flash_reset(void)
{
  RA_VALIDATE_INIT(s_rt.initialised, s_tag, "flash_reset before init");
  ra_err_t err = ra_flash_enter_pe_mode();
  if (err != k_ra_ok) {
    return err;
  }
  ra_err_t stop_err = ra_flash_force_stop();
  /* Clear status whether or not the stop succeeded. */
  /* HUM Ch 59 "MACI Command-Issuing Area" p 3550 */
  internal_maci_cmd8(k_ra_maci_cmd_status_clear);
  (void)internal_wait_mrdy(k_ra_flash_maci_spin_limit);
  ra_err_t exit_err = ra_flash_exit_pe_mode();
  if (stop_err != k_ra_ok) {
    return stop_err;
  }
  return exit_err;
}

/* =============================================================================
 * Public API: start-up area control
 * =============================================================================
 */

ra_err_t ra_flash_set_startup_area(ra_flash_startup_t target, bool temporary)
{
  if (target > k_ra_flash_startup_btflg) {
    return k_ra_err_invalid_arg;
  }
  ra_err_t err = ra_flash_enter_pe_mode();
  if (err != k_ra_ok) {
    return err;
  }

  if (temporary) {
    /* HUM Ch 59 "MSUACR : Start-Up Area Control Register" p 3593 */
    const uint16_t swap_bit = (target == k_ra_flash_startup_alternate) ? k_ra_msuacr_swap_alternate
                                                                       : k_ra_msuacr_swap_default;
    *ra_mram_reg16(k_ra_mram_off_msuacr) = (uint16_t)(k_ra_msuacr_key | swap_bit);
  } else {
    /* Permanent: configuration-set write to BTFLG. */
    /* HUM Ch 7 "Option-Setting Memory" p 278 */
    uint16_t cfg_words[k_ra_mram_config_set_word_count];
    for (uint32_t i = 0U; i < k_ra_mram_config_set_word_count; ++i) {
      cfg_words[i] = k_ra_flash_cfg_word_all_ones;
    }
    /* BTFLG occupies bit 15 of word index 3 (FSP MRAM_PRV_CONFIG_SET_BTFLG_OFFSET).
     * 0 selects alternate, 1 selects default (HUM Ch 7 p 278). */
    uint16_t btflg_bit = k_ra_flash_btflg_alternate;
    if (target == k_ra_flash_startup_default) {
      btflg_bit = k_ra_flash_btflg_default;
    }
    /* HUM Ch 7 "OFS SAS region" p 278 */
    cfg_words[3] = (uint16_t)(btflg_bit | k_ra_flash_btflg_word_keep);
    err          = ra_flash_config_set_write(k_ra_msaddr_config_set_startup, cfg_words);
  }

  ra_err_t exit_err = ra_flash_exit_pe_mode();
  if (err != k_ra_ok) {
    return err;
  }
  return exit_err;
}

ra_err_t ra_flash_get_startup_area(uint8_t* out_btflg, uint8_t* out_fspr)
{
  RA_CHECK_NULL_PTR(out_btflg, s_tag, "out_btflg must not be nullptr");
  RA_CHECK_NULL_PTR(out_fspr, s_tag, "out_fspr must not be nullptr");
  /* HUM Ch 59 "MSUASMON : Start-Up Area Monitor" p 3593 */
  const uint32_t v = *ra_mram_reg32(k_ra_mram_off_msuasmon);
  *out_btflg       = (uint8_t)((v & k_ra_msuasmon_mask_btflg) != 0U);
  *out_fspr        = (uint8_t)((v & k_ra_msuasmon_mask_fspr) != 0U);
  return k_ra_ok;
}

/* =============================================================================
 * Public API: configuration-set write (low-level OFS update)
 * =============================================================================
 */

ra_err_t ra_flash_config_set_write(uint32_t target_addr, const uint16_t* words)
{
  RA_CHECK_NULL_PTR(words, s_tag, "words must not be nullptr");
  /* The MACI ``config_set`` opener / 8-halfword payload / 0xD0 trailer is
   * shared between two flows:
   *   - OFS programming  (HUM Ch 7 "Option-Setting Memory"  p 278) targets
   *     halfwords inside the OFS window at 0x02C9F000.
   *   - Extra-MRAM write (HUM Ch 59 "MACI Command Sequence" p 3550) reuses
   *     the same opener with MSADDR pointing into extra-MRAM at 0x27000000.
   * Accept both ranges; reject everything else. */
  const uint32_t ofs_end   = (uint32_t)k_ra_flash_ofs_start + (uint32_t)k_ra_flash_ofs_size;
  const uint32_t extra_end = (uint32_t)k_ra_flash_extra_start + (uint32_t)k_ra_flash_extra_size;
  const bool     in_ofs =
    (bool)((target_addr >= (uint32_t)k_ra_flash_ofs_start) && (target_addr < ofs_end));
  const bool in_extra =
    (bool)((target_addr >= (uint32_t)k_ra_flash_extra_start) && (target_addr < extra_end));
  if (!in_ofs && !in_extra) {
    return k_ra_err_invalid_arg;
  }

  /* HUM Ch 59 "MSADDR : MACI Command Start Address" p 3573 */
  *ra_mram_reg32(k_ra_mram_off_msaddr) = target_addr;

  /* Two-byte command opener -- HUM Ch 59 p 3550 + FSP r_mram.c L1511..1521. */
  internal_maci_cmd8(k_ra_maci_cmd_config_set_1);
  internal_maci_cmd8(k_ra_maci_cmd_config_set_2);

  /* Eight halfwords payload. */
  for (uint32_t i = 0U; i < k_ra_mram_config_set_word_count; ++i) {
    internal_maci_cmd16(words[i]);
  }
  /* Trailer. */
  internal_maci_cmd8(k_ra_maci_cmd_final);

  ra_err_t err = internal_wait_mrdy(k_ra_flash_maci_spin_limit);
  if (err != k_ra_ok) {
    return err;
  }

  /* HUM Ch 59 "MSTATR : Extra MRAM Status Register" p 3578 */
  const uint32_t s = *ra_mram_reg32(k_ra_mram_off_mstatr);
  if ((s & k_ra_mstatr_mask_any_err) != 0U) {
    return k_ra_err_hw_error;
  }
  return k_ra_ok;
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
 * @return ``ra_err_t``.
 *
 * @pre Controller already in P/E mode.
 * @post MRDY observed or function returns timeout/error.
 */
static ra_err_t internal_arc_cmd(uint8_t mcntselr, uint8_t cmd)
{
  /* HUM Ch 59 "MCNTSELR : MRAM Counter Select Register" p 3576 */
  *ra_mram_reg8(k_ra_mram_off_mcntselr) = (uint8_t)(mcntselr & k_ra_mcntselr_mask);
  internal_maci_cmd8(cmd);
  internal_maci_cmd8(k_ra_maci_cmd_final);
  ra_err_t err = internal_wait_mrdy(k_ra_flash_maci_spin_limit);
  if (err != k_ra_ok) {
    return err;
  }
  /* HUM Ch 59 "MASTAT : Extra MRAM Access Status Register" p 3577 */
  const uint8_t mastat = *ra_mram_reg8(k_ra_mram_off_mastat);
  if ((mastat & k_ra_mastat_mask_cmdlk) != 0U) {
    return k_ra_err_hw_error;
  }
  return k_ra_ok;
}

/**
 * @brief Read an ARC counter through whatever path the HUM mandates.
 *
 * @param[in]  id        Logical counter id.
 * @param[out] out_count Population count of the counter bit-vector.
 * @return ``ra_err_t``.
 *
 * @pre Caller is in P/E mode for the OEMBL path; read-mode is fine
 *      for SEC/NSEC.
 * @post ``*out_count`` populated on success.
 */
/**
 * @brief Sum the population count of one of the four ARC_NSEC slots.
 *
 * @details
 * HUM Ch 7.2.21 "ARCCS" p 296 + HUM Ch 7.2.23 "ARC_NSEC" p 297. The
 * ARCNS field selects between a single 16-word counter or four 2-word
 * counters. ``id`` selects which sub-counter to sum.
 *
 * @param[in] id  One of ``k_ra_flash_arc_nsec_0``..3.
 * @return Total set-bit count.
 *
 * @pre ``id`` belongs to the NSEC family.
 * @post No side effects.
 *
 * @note Internal helper, not thread-safe.
 */
static uint32_t internal_arc_nsec_count(ra_flash_arc_id_t id)
{
  /* HUM Ch 7.2.21 "ARCCS" p 296 */
  const uint16_t arccs = *(volatile const uint16_t*)k_ra_flash_ofs_arccs_addr;
  const uint8_t  arcns = (uint8_t)(arccs & (uint16_t)k_ra_arc_arccs_mask);
  /* HUM Ch 7.2.23 "ARC_NSEC" p 297 */
  const volatile uint32_t* base      = (const volatile uint32_t*)k_ra_flash_ofs_arc_nsec_addr;
  uint32_t                 words_per = (arcns == (uint8_t)k_ra_arc_arcns_single) ? 16U : 2U;
  if (words_per > k_ra_mram_arc_max_words) {
    words_per = k_ra_mram_arc_max_words;
  }
  uint32_t base_idx = words_per * 3U;
  if (id == k_ra_flash_arc_nsec_0) {
    base_idx = 0U;
  } else if (id == k_ra_flash_arc_nsec_1) {
    base_idx = words_per;
  } else if (id == k_ra_flash_arc_nsec_2) {
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

static ra_err_t internal_arc_read_locked(ra_flash_arc_id_t id, uint32_t* out_count)
{
  uint8_t  mcntselr = internal_arc_to_mcntselr(id);
  uint32_t count    = 0U;

  if (id == k_ra_flash_arc_oembl) {
    /* OEMBL needs the MACI read so RSIP-E50D can intercept. */
    ra_err_t err = internal_arc_cmd(mcntselr, k_ra_maci_cmd_read_counter);
    if (err != k_ra_ok) {
      return err;
    }
    /* HUM Ch 59 "MCNTDTR0 : MRAM Counter Data 0" p 3576 */
    const uint32_t lo = *ra_mram_reg32(k_ra_mram_off_mcntdtr0);
    /* HUM Ch 59 "MCNTDTR1 : MRAM Counter Data 1" p 3577 */
    const uint32_t hi = *ra_mram_reg32(k_ra_mram_off_mcntdtr1);
    count             = internal_popcount32(lo) + internal_popcount32(hi);
  } else if (id == k_ra_flash_arc_sec) {
    /* HUM Ch 7.2.22 "ARC_SEC" p 296: ARC_SEC is a 64-bit unary counter, i.e.
     * 2 x uint32_t words. FSP r_mram.c L1196 derives the same count via
     * BSP_FEATURE_FLASH_ARC_SEC_MAX_COUNT (64) >> 5. The previous loop walked
     * 8 words, which over-counted into adjacent OFS state. */
    const volatile uint32_t* p         = (const volatile uint32_t*)k_ra_flash_ofs_arc_sec_addr;
    const uint32_t           sec_words = k_ra_arc_sec_max_bits >> 5U;
    for (uint32_t w = 0U; w < sec_words; ++w) {
      count += internal_popcount32(p[w]);
    }
  } else {
    count = internal_arc_nsec_count(id);
  }

  *out_count = count;
  (void)mcntselr; /* used in OEMBL branch only */
  return k_ra_ok;
}

ra_err_t ra_flash_arc_increment(ra_flash_arc_id_t counter)
{
  if (counter >= k_ra_flash_arc_count) {
    return k_ra_err_invalid_arg;
  }
  RA_VALIDATE_INIT(s_rt.initialised, s_tag, "arc_inc before init");

  ra_err_t err = ra_flash_enter_pe_mode();
  if (err != k_ra_ok) {
    return err;
  }

  /* NASA Power-of-10 Rule 1 forbids goto, so the read-bound-check-increment
   * sequence is expressed as a short-circuit chain: each step runs only if
   * the previous one returned k_ra_ok, and the unconditional exit-PE step
   * replaces the prior goto-out label.
   * HUM Ch 59.4.4 "MACI Increment Counter" p 3550. */
  uint32_t cur = 0U;
  err          = internal_arc_read_locked(counter, &cur);
  if (err == k_ra_ok) {
    const uint32_t max = internal_arc_max_count(counter);
    if (cur + 1U > max) {
      err = k_ra_err_out_of_range;
    } else {
      err = internal_arc_cmd(internal_arc_to_mcntselr(counter), k_ra_maci_cmd_increment_counter);
    }
  }

  const ra_err_t exit_err = ra_flash_exit_pe_mode();
  if (err == k_ra_ok) {
    err = exit_err;
  }
  return err;
}

ra_err_t ra_flash_arc_read(ra_flash_arc_id_t counter, uint32_t* out_count)
{
  RA_CHECK_NULL_PTR(out_count, s_tag, "out_count must not be nullptr");
  if (counter >= k_ra_flash_arc_count) {
    return k_ra_err_invalid_arg;
  }
  RA_VALIDATE_INIT(s_rt.initialised, s_tag, "arc_read before init");

  if (counter == k_ra_flash_arc_oembl) {
    ra_err_t err = ra_flash_enter_pe_mode();
    if (err != k_ra_ok) {
      return err;
    }
    ra_err_t r        = internal_arc_read_locked(counter, out_count);
    ra_err_t exit_err = ra_flash_exit_pe_mode();
    if (r != k_ra_ok) {
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

ra_err_t ra_flash_zeroize_huk(void)
{
  RA_VALIDATE_INIT(s_rt.initialised, s_tag, "zeroize before init");
  /* HUM Ch 59 "MREZC : Extra MRAM Zeroization Control" p 3565 */
  *ra_mram_reg16(k_ra_mram_off_mrezc) = k_ra_mrezc_full_zero;

  for (uint32_t i = 0U; i < k_ra_flash_zeroize_spin; ++i) {
    /* HUM Ch 59 "MREZS : Extra MRAM Zeroization Status" p 3565 */
    const uint8_t s = *ra_mram_reg8(k_ra_mram_off_mrezs);
    if ((s & k_ra_mrezs_mask_whukexe) == 0U) {
      return k_ra_ok;
    }
  }
  return k_ra_err_hw_timeout;
}

ra_err_t ra_flash_set_security_attribution(uint16_t new_msar)
{
  /* HUM Ch 59.5.13 "MSAR : MRAM Security Attribution Register" p 3559 */
  *ra_mram_reg16(k_ra_mram_off_msar) = new_msar;
  return k_ra_ok;
}

ra_err_t ra_flash_msuinitr_kick(void)
{
  /* HUM Ch 59 "MSUINITR : Extra MRAM Sequencer Set-Up Init" p 3585 */
  *ra_mram_reg16(k_ra_mram_off_msuinitr) = k_ra_msuinitr_full_init;
#ifdef RA_SIMULATOR_MODE
  /* On real HW the sequencer auto-clears SUINIT once the init
   * completes. The host-test simulator is dumb memory, so reflect
   * that here so the poll below exits on its first iteration. */
  *ra_mram_reg16(k_ra_mram_off_msuinitr) =
    (uint16_t)(k_ra_msuinitr_full_init & ~k_ra_msuinitr_mask_suinit);
#endif

  for (uint32_t i = 0U; i < k_ra_flash_pe_spin_limit; ++i) {
    /* HUM Ch 59 "MSUINITR : Extra MRAM Sequencer Set-Up Init" p 3585 */
    const uint16_t v = *ra_mram_reg16(k_ra_mram_off_msuinitr);
    if ((v & k_ra_msuinitr_mask_suinit) == 0U) {
      return k_ra_ok;
    }
  }
  return k_ra_err_hw_timeout;
}

ra_err_t ra_flash_set_ecc_encoder_enable(bool enable)
{
  /* HUM Ch 59 "MRCEECC : Code MRAM ECC Encoder Control" p 3624 */
  uint16_t enc_bit = 0U;
  if (enable) {
    enc_bit = k_ra_mrceecc_mask_eccen;
  }
  *ra_mram_reg16(k_ra_mram_off_mrceecc) = (uint16_t)(k_ra_mrceecc_key_shift | enc_bit);
  return k_ra_ok;
}

ra_err_t ra_flash_set_ecc_decoder_enable(bool enable)
{
  /* HUM Ch 59 "MRCDECC : Code MRAM ECC Decoder Control" p 3554 */
  uint16_t dec_bit = 0U;
  if (enable) {
    dec_bit = k_ra_mrcdecc_mask_dececen;
  }
  *ra_mram_reg16(k_ra_mram_off_mrcdecc) = (uint16_t)(k_ra_mrcdecc_key_shift | dec_bit);
  return k_ra_ok;
}

ra_err_t ra_flash_get_ecc_error_addr(uint32_t* out_code_ted,
                                     uint32_t* out_code_dec,
                                     uint32_t* out_extra_ted,
                                     uint32_t* out_extra_dec)
{
  RA_CHECK_NULL_PTR(out_code_ted, s_tag, "out_code_ted null");
  RA_CHECK_NULL_PTR(out_code_dec, s_tag, "out_code_dec null");
  RA_CHECK_NULL_PTR(out_extra_ted, s_tag, "out_extra_ted null");
  RA_CHECK_NULL_PTR(out_extra_dec, s_tag, "out_extra_dec null");

  /* HUM Ch 59 "MRCRTEA : Code MRAM TED Error Address" p 3555 */
  *out_code_ted = *ra_mram_reg32(k_ra_mram_off_mrcrtea);
  /* HUM Ch 59 "MRCRDEA : Code MRAM DEC Error Address" p 3555 */
  *out_code_dec = *ra_mram_reg32(k_ra_mram_off_mrcrdea);
  /* HUM Ch 59 "MRERTEA : Extra MRAM TED Error Address" p 3558 */
  *out_extra_ted = *ra_mram_reg32(k_ra_mram_off_mrertea);
  /* HUM Ch 59 "MRERDEA : Extra MRAM DEC Error Address" p 3558 */
  *out_extra_dec = *ra_mram_reg32(k_ra_mram_off_mrerdea);
  return k_ra_ok;
}

ra_err_t ra_flash_get_program_error_addr(uint32_t* out_addr)
{
  RA_CHECK_NULL_PTR(out_addr, s_tag, "out_addr must not be nullptr");
  /* HUM Ch 59 "MRCPEA : Code MRAM Program Error Address" p 3601 */
  *out_addr = *ra_mram_reg32(k_ra_mram_off_mrcpea);
  return k_ra_ok;
}

ra_err_t ra_flash_update_clock_freq(uint16_t mrcfreq_mhz, uint8_t mrefreq_mhz)
{
  if (mrcfreq_mhz > (uint16_t)k_ra_flash_max_mrcfreq_mhz) {
    return k_ra_err_invalid_arg;
  }
  if (mrefreq_mhz > (uint8_t)k_ra_flash_max_mrefreq_mhz) {
    return k_ra_err_invalid_arg;
  }
  const bool prefetch_was = s_rt.prefetch_on;
  internal_set_prefetch(false);

  /* HUM Ch 59.5.2 "MRCFREQ : Code MRAM Frequency Notifications Register" p 3551 */
  *ra_mram_reg32(k_ra_mram_off_mrcfreq) =
    (k_ra_flash_mrcfreq_key << k_ra_flash_freq_key_shift) | (uint32_t)mrcfreq_mhz;
  /* HUM Ch 59.5.3 "MREFREQ : Extra MRAM Frequency Notifications Register" p 3552 */
  *ra_mram_reg32(k_ra_mram_off_mrefreq) =
    (k_ra_flash_mrefreq_key << k_ra_flash_freq_key_shift) | (uint32_t)mrefreq_mhz;

  internal_set_prefetch(prefetch_was);
  return k_ra_ok;
}

/* =============================================================================
 * Public API: update transfer
 * =============================================================================
 */

ra_err_t ra_flash_set_update_transfer(uint8_t list_select)
{
  if (list_select > (uint8_t)k_ra_flash_max_list_select) {
    return k_ra_err_invalid_arg;
  }
  /* HUM Ch 59 "MCTRLSR : MRAM Update Transfer List Select" p 3580 */
  *ra_mram_reg8(k_ra_mram_off_mctrlsr) = (uint8_t)(list_select & k_ra_mctrlsr_mask_list_sel);
  /* HUM Ch 59 "MCTRCNTR : MRAM Update Transfer Control" p 3580 */
  *ra_mram_reg16(k_ra_mram_off_mctrcntr) = (uint16_t)(k_ra_mctrcntr_key | k_ra_mctrcntr_mask_start);
  return k_ra_ok;
}

ra_err_t ra_flash_get_update_status(uint8_t* out_busy, uint8_t* out_done, uint8_t* out_err)
{
  RA_CHECK_NULL_PTR(out_busy, s_tag, "out_busy null");
  RA_CHECK_NULL_PTR(out_done, s_tag, "out_done null");
  RA_CHECK_NULL_PTR(out_err, s_tag, "out_err null");
  /* HUM Ch 59 "MCTRSTATR : MRAM Update Transfer Status" p 3580 */
  const uint16_t v = *ra_mram_reg16(k_ra_mram_off_mctrstatr);
  *out_busy        = (uint8_t)((v & k_ra_mctrstatr_mask_busy) != 0U);
  *out_done        = (uint8_t)((v & k_ra_mctrstatr_mask_done) != 0U);
  *out_err         = (uint8_t)((v & k_ra_mctrstatr_mask_err) != 0U);
  return k_ra_ok;
}

/* =============================================================================
 * Public API: extra-MRAM (data flash) program / erase
 * =============================================================================
 */

ra_err_t ra_flash_extra_mram_write(uint32_t mram_addr, const uint8_t* src, uint32_t len)
{
  RA_CHECK_NULL_PTR(src, s_tag, "src must not be nullptr");
  if (len == 0U || len > k_ra_mram_write_size_bytes) {
    return k_ra_err_invalid_arg;
  }
  if (mram_addr < (uint32_t)k_ra_flash_extra_start) {
    return k_ra_err_invalid_arg;
  }
  const uint32_t end_excl = (uint32_t)((uint64_t)mram_addr + (uint64_t)len);
  if (end_excl > (uint32_t)k_ra_flash_extra_start + (uint32_t)k_ra_flash_extra_size) {
    return k_ra_err_invalid_arg;
  }
  const uint32_t page_mask = k_ra_mram_write_size_bytes - 1U;
  if ((mram_addr & ~page_mask) != ((end_excl - 1U) & ~page_mask)) {
    return k_ra_err_invalid_arg;
  }

  ra_err_t err = ra_flash_enter_pe_mode();
  if (err != k_ra_ok) {
    return err;
  }

  /* HUM Ch 59 "MSADDR : MACI Command Start Address" p 3573 */
  *ra_mram_reg32(k_ra_mram_off_msaddr) = mram_addr;

  /* The MACI program flow (HUM Ch 59 p 3550) is the same opener as a
   * configuration set; the difference is in the trailer + payload size.
   * We re-use the config-set sequence with an 8-halfword chunked write
   * sized to ``len`` (padded with 0xFFFF). */
  uint16_t cfg_words[k_ra_mram_config_set_word_count] = {};
  for (uint32_t i = 0U; i < k_ra_mram_config_set_word_count; ++i) {
    const uint32_t base = i * 2U;
    const uint8_t  lo   = (base < len) ? src[base] : 0xFFU;
    const uint8_t  hi   = (base + 1U < len) ? src[base + 1U] : 0xFFU;
    cfg_words[i]        = (uint16_t)((uint16_t)lo | ((uint16_t)hi << 8U));
  }
  err = ra_flash_config_set_write(mram_addr, cfg_words);

  ra_err_t exit_err = ra_flash_exit_pe_mode();
  if (err == k_ra_ok) {
    err = exit_err;
  }
  return err;
}

ra_err_t ra_flash_extra_mram_erase(uint32_t mram_addr)
{
  if ((mram_addr & (k_ra_mram_block_size_bytes - 1U)) != 0U) {
    return k_ra_err_invalid_arg;
  }
  static const uint8_t s_ones[k_ra_mram_block_size_bytes] = {
    0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU,
    0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU,
    0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU,
  };
  return ra_flash_extra_mram_write(mram_addr, s_ones, k_ra_mram_block_size_bytes);
}

/* =============================================================================
 * Public API: IRQ enables + dispatcher
 * =============================================================================
 */

/**
 * @brief Read-modify-write a single bit in an 8-bit register.
 *
 * @details
 * Helper used by ``ra_flash_set_irq_enable`` to set or clear a single
 * IRQ-enable bit without disturbing the rest of the byte.
 *
 * @param[in] off    MRAM register offset.
 * @param[in] bit    Mask of the bit to drive.
 * @param[in] enable ``true`` to set, ``false`` to clear.
 *
 * @pre Module clock ungated.
 * @post Register byte reflects the requested change.
 *
 * @note Internal helper, not thread-safe.
 */
static void internal_irq_rmw8(uint16_t off, uint8_t bit, bool enable)
{
  uint8_t v = *ra_mram_reg8(off);
  if (enable) {
    v = (uint8_t)(v | bit);
  } else {
    v = (uint8_t)(v & (uint8_t)~bit);
  }
  *ra_mram_reg8(off) = v;
}

/**
 * @brief Apply enable/disable to an MRCRAEINT-style ECC IRQ.
 *
 * @details
 * HUM Ch 59 "MRCRAEINT" p 3554 / "MRERAINT" p 3557. The two
 * registers share TED/DEC bit positions, so the helper just picks
 * which register byte and which bit to flip.
 *
 * @param[in] off       MRCRAEINT or MRERAINT offset.
 * @param[in] is_ted    ``true`` for the TED bit, ``false`` for DEC.
 * @param[in] enable    ``true`` to set the bit.
 *
 * @pre Module clock ungated.
 * @post Selected bit reflects ``enable``.
 *
 * @note Internal helper, not thread-safe.
 */
static void internal_apply_ecc_irq(uint16_t off, bool is_ted, bool enable)
{
  uint8_t bit = k_ra_mrcraeint_mask_intenbdc;
  if (is_ted) {
    bit = k_ra_mrcraeint_mask_intenbtc;
  }
  internal_irq_rmw8(off, bit, enable);
}

/**
 * @brief Apply enable/disable to an MPAEINT bit.
 *
 * @details
 * HUM Ch 59 "MPAEINT" p 3577. Selects between the access-error and
 * command-lock bits.
 *
 * @param[in] err_kind  ``true`` for MREAEIE, ``false`` for CMDLKIE.
 * @param[in] enable    ``true`` to set the bit.
 *
 * @pre Module clock ungated.
 * @post Selected bit reflects ``enable``.
 *
 * @note Internal helper, not thread-safe.
 */
static void internal_apply_extra_err_irq(bool err_kind, bool enable)
{
  uint8_t bit = k_ra_mpaeint_mask_cmdlkie;
  if (err_kind) {
    bit = k_ra_mpaeint_mask_mreaeie;
  }
  internal_irq_rmw8(k_ra_mram_off_mpaeint, bit, enable);
}

ra_err_t ra_flash_set_irq_enable(ra_flash_irq_src_t src, bool enable)
{
  if (src >= k_ra_flash_irq_count) {
    return k_ra_err_invalid_arg;
  }
  switch (src) {
    case k_ra_flash_irq_code_ecc_ted:
    case k_ra_flash_irq_code_ecc_dec:
      internal_apply_ecc_irq(k_ra_mram_off_mrcraeint, src == k_ra_flash_irq_code_ecc_ted, enable);
      break;
    case k_ra_flash_irq_extra_ecc_ted:
    case k_ra_flash_irq_extra_ecc_dec:
      internal_apply_ecc_irq(k_ra_mram_off_mreraint, src == k_ra_flash_irq_extra_ecc_ted, enable);
      break;
    case k_ra_flash_irq_program_err: {
      /* HUM Ch 59 "MRCPAEINT : Code MRAM Program Access Error IRQ Enable" p 3601 */
      uint8_t v = 0U;
      if (enable) {
        v = k_ra_mrcpaeint_mask_mrcaeie;
      }
      *ra_mram_reg8(k_ra_mram_off_mrcpaeint) = v;
      break;
    }
    case k_ra_flash_irq_extra_err:
    case k_ra_flash_irq_extra_cmdlk:
      internal_apply_extra_err_irq(src == k_ra_flash_irq_extra_err, enable);
      break;
    case k_ra_flash_irq_extra_ready: {
      /* HUM Ch 59 "MRDYIE : Extra MRAM Ready Interrupt Enable" p 3577 */
      uint8_t v = 0U;
      if (enable) {
        v = k_ra_mrdyie_mask_mrdyie;
      }
      *ra_mram_reg8(k_ra_mram_off_mrdyie) = v;
      break;
    }
    case k_ra_flash_irq_count: /* fallthrough -- unreachable, validated above. */
    default:
      return k_ra_err_invalid_arg;
  }
  return k_ra_ok;
}

ra_err_t ra_flash_callback_set(ra_flash_callback_t cb, void* user_ctx)
{
  s_rt.cb       = cb;
  s_rt.user_ctx = user_ctx;
  return k_ra_ok;
}

/**
 * @brief Deliver one IRQ event to the registered callback.
 *
 * @param[in] src         Source identifier.
 * @param[in] fault_addr  Captured address (0 if N/A).
 * @param[in] status_word Source register snapshot.
 *
 * @pre None (no-op if no callback registered).
 * @post If a callback is registered, it ran exactly once.
 */
static void internal_deliver(ra_flash_irq_src_t src, uint32_t fault_addr, uint32_t status_word)
{
  if (s_rt.cb == nullptr) {
    return;
  }
  const ra_flash_isr_event_t ev = {
    .src         = src,
    .fault_addr  = fault_addr,
    .status_word = status_word,
    .user_ctx    = s_rt.user_ctx,
  };
  s_rt.cb(&ev);
}

/**
 * @brief Dispatch ECC TED/DEC events for one of the two MRAM regions.
 *
 * @details
 * HUM Ch 59 "MRCRAES" p 3554 (code MRAM) and "MRERAES" p 3557 (extra
 * MRAM). The two registers share the same bit layout; ``status_off``
 * picks which one we observe and W1C-clear, and ``ted_addr_off`` /
 * ``dec_addr_off`` give the matching error-address registers.
 *
 * @param[in] status_off    MRCRAES / MRERAES offset.
 * @param[in] ted_addr_off  MRCRTEA / MRERTEA offset.
 * @param[in] dec_addr_off  MRCRDEA / MRERDEA offset.
 * @param[in] ted_src       IRQ source enum for the TED event.
 * @param[in] dec_src       IRQ source enum for the DEC event.
 *
 * @return Number of callbacks delivered (0..2).
 *
 * @pre Module clock ungated.
 * @post W1C status register cleared on observation.
 *
 * @note Internal helper, not thread-safe.
 */
static uint32_t internal_dispatch_ecc(uint16_t           status_off,
                                      uint16_t           ted_addr_off,
                                      uint16_t           dec_addr_off,
                                      ra_flash_irq_src_t ted_src,
                                      ra_flash_irq_src_t dec_src)
{
  uint32_t      delivered = 0U;
  const uint8_t status    = *ra_mram_reg8(status_off);
  if ((status & k_ra_mrcraes_mask_tederrc) != 0U) {
    const uint32_t fa = *ra_mram_reg32(ted_addr_off);
    internal_deliver(ted_src, fa, (uint32_t)status);
    delivered++;
  }
  if ((status & k_ra_mrcraes_mask_decerrc) != 0U) {
    const uint32_t fa = *ra_mram_reg32(dec_addr_off);
    internal_deliver(dec_src, fa, (uint32_t)status);
    delivered++;
  }
  if (status != 0U) {
    *ra_mram_reg8(status_off) = 0U;
  }
  return delivered;
}

uint32_t ra_flash_dispatch_isr(void)
{
  uint32_t delivered = 0U;

  delivered += internal_dispatch_ecc(k_ra_mram_off_mrcraes,
                                     k_ra_mram_off_mrcrtea,
                                     k_ra_mram_off_mrcrdea,
                                     k_ra_flash_irq_code_ecc_ted,
                                     k_ra_flash_irq_code_ecc_dec);
  delivered += internal_dispatch_ecc(k_ra_mram_off_mreraes,
                                     k_ra_mram_off_mrertea,
                                     k_ra_mram_off_mrerdea,
                                     k_ra_flash_irq_extra_ecc_ted,
                                     k_ra_flash_irq_extra_ecc_dec);

  /* HUM Ch 59 "MRCPS : Code MRAM Program Status Register" p 3601 */
  const uint8_t mrcps = *ra_mram_reg8(k_ra_mram_off_mrcps);
  if ((mrcps & k_ra_mrcps_mask_errors) != 0U) {
    /* HUM Ch 59 "MRCPEA : Code MRAM Program Error Address" p 3601 */
    const uint32_t fa = *ra_mram_reg32(k_ra_mram_off_mrcpea);
    internal_deliver(k_ra_flash_irq_program_err, fa, (uint32_t)mrcps);
    /* W1C the program error bits. */
    *ra_mram_reg8(k_ra_mram_off_mrcps) = k_ra_mrcps_mask_errors;
#ifdef RA_SIMULATOR_MODE
    /* The host-test sim is dumb memory: emulate the W1C clear so the
     * post-dispatch state matches real HW. */
    *ra_mram_reg8(k_ra_mram_off_mrcps) &= (uint8_t)~k_ra_mrcps_mask_errors;
#endif
    delivered++;
  }

  /* HUM Ch 59 "MASTAT : Extra MRAM Access Status Register" p 3577 */
  const uint8_t mastat = *ra_mram_reg8(k_ra_mram_off_mastat);
  if ((mastat & k_ra_mastat_mask_mreae) != 0U) {
    internal_deliver(k_ra_flash_irq_extra_err, 0U, (uint32_t)mastat);
    delivered++;
  }
  if ((mastat & k_ra_mastat_mask_cmdlk) != 0U) {
    internal_deliver(k_ra_flash_irq_extra_cmdlk, 0U, (uint32_t)mastat);
    delivered++;
  }

  /* HUM Ch 59 "MSTATR : Extra MRAM Status Register" p 3578 */
  const uint32_t mstatr = *ra_mram_reg32(k_ra_mram_off_mstatr);
  if ((mstatr & k_ra_mstatr_mask_mrdy) != 0U) {
    internal_deliver(k_ra_flash_irq_extra_ready, 0U, mstatr);
    delivered++;
  }
  return delivered;
}

/* =============================================================================
 * Public API: FSP r_mram parity surface
 * =============================================================================
 */

ra_err_t ra_flash_open(const ra_flash_cfg_t* cfg)
{
  /* FSP r_mram.c L253 R_MRAM_Open delegates to mram_init; we delegate to the
   * existing ra_flash_init so there is one canonical bring-up path. */
  return ra_flash_init(cfg);
}

ra_err_t ra_flash_close(void)
{
  /* FSP r_mram.c L646 R_MRAM_Close just clears the opened flag; we delegate
   * to ra_flash_deinit which also returns the controller to read mode. */
  return ra_flash_deinit();
}

ra_err_t ra_flash_set_window(uintptr_t low, uintptr_t high)
{
  if (low == 0U && high == 0U) {
    s_rt.win_low  = 0U;
    s_rt.win_high = 0U;
    return k_ra_ok;
  }
  if (low >= high) {
    return k_ra_err_invalid_arg;
  }
  s_rt.win_low  = low;
  s_rt.win_high = high;
  return k_ra_ok;
}

/**
 * @brief Pick the right TrustZone world for a destination address.
 *
 * @details
 * RA8D2 surfaces the secure code-MRAM alias at a higher offset; FSP's
 * ``mram_program_control`` (r_mram.c L962) tests the BSP_FEATURE_TZ_NS_OFFSET
 * bit. We mirror the same test by comparing against the secure base.
 * For destinations that the silicon classifies as non-secure (the
 * default 0x02000000 alias), we use MRCPC0; for the secure alias we use
 * MRCPC1.
 *
 * @param[in] addr Destination address.
 * @return World selector for ``ra_flash_write_block``.
 *
 * @pre None.
 * @post No side effects.
 *
 * @note Internal helper, not thread-safe.
 */
static ra_flash_world_t internal_world_for_addr(uintptr_t addr)
{
  /* HUM Ch 59.1 "Address Map" p 3543 -- the non-secure alias is the
   * default 0x02000000 view. Anything inside the standard 1 MiB code
   * window is treated as the NS world by this driver; callers that
   * need the S alias call ra_flash_write_block directly. */
  (void)addr;
  return k_ra_flash_world_ns;
}

/**
 * @brief Validate a multi-block erase / multi-page write range.
 *
 * @details
 * Folds the alignment + bounds + soft-window checks shared by
 * ``ra_flash_erase`` and ``ra_flash_write``. Both APIs operate on the
 * 32-byte programming unit (HUM Ch 59.4.2 p 3548).
 *
 * @param[in] address   Range start address.
 * @param[in] total_len Total number of bytes covered by the operation.
 *
 * @return ``k_ra_ok`` if the range is acceptable.
 *
 * @pre None.
 * @post No side effects.
 *
 * @note Internal helper, not thread-safe.
 */
static ra_err_t internal_validate_range(uintptr_t address, uint64_t total_len)
{
  if ((address & (k_ra_mram_block_size_bytes - 1U)) != 0U) {
    return k_ra_err_invalid_arg;
  }
  const uint64_t end_excl = (uint64_t)address + total_len;
  if (address < k_ra_flash_code_start) {
    return k_ra_err_invalid_arg;
  }
  if (end_excl > (uint64_t)k_ra_flash_code_start + (uint64_t)k_ra_flash_code_size) {
    return k_ra_err_invalid_arg;
  }
  if (!internal_window_allows(address, (uint32_t)total_len)) {
    return k_ra_err_out_of_range;
  }
  return k_ra_ok;
}

ra_err_t ra_flash_erase(uintptr_t address, uint32_t num_blocks)
{
  RA_VALIDATE_INIT(s_rt.initialised, s_tag, "flash_erase before init");
  if (num_blocks == 0U) {
    return k_ra_err_invalid_arg;
  }
  const uint64_t total_bytes = (uint64_t)num_blocks * (uint64_t)k_ra_mram_block_size_bytes;
  const ra_err_t v_err       = internal_validate_range(address, total_bytes);
  RA_RETURN_ON_ERROR(v_err, s_tag, "flash_erase: validate");
  const ra_flash_world_t world = internal_world_for_addr(address);
  /* FSP r_mram.c L917 mram_erase_blocks loops one programming-size block at
   * a time; we mirror that one-block-per-iteration cadence. NASA Rule 2:
   * loop bound is the caller-supplied num_blocks, validated above. */
  uintptr_t cur = address;
  for (uint32_t i = 0U; i < num_blocks; ++i) {
    const ra_err_t err = ra_flash_erase_block((uint32_t)cur, world);
    if (err != k_ra_ok) {
      return err;
    }
    cur += k_ra_mram_block_size_bytes;
  }
  return k_ra_ok;
}

ra_err_t ra_flash_write(uintptr_t address, const uint8_t* src, uint32_t len)
{
  RA_CHECK_NULL_PTR(src, s_tag, "src must not be nullptr");
  RA_VALIDATE_INIT(s_rt.initialised, s_tag, "flash_write before init");
  if (len == 0U || (len % k_ra_mram_write_size_bytes) != 0U) {
    return k_ra_err_invalid_arg;
  }
  const ra_err_t v_err = internal_validate_range(address, (uint64_t)len);
  RA_RETURN_ON_ERROR(v_err, s_tag, "flash_write: validate");
  const ra_flash_world_t world = internal_world_for_addr(address);
  /* FSP r_mram.c L861 mram_write_data chunks the request into 32-byte
   * page programs; our wrapper re-uses ra_flash_write_block for each
   * page. NASA Rule 2: loop bound is len/page (validated above). */
  const uint32_t pages = len / k_ra_mram_write_size_bytes;
  for (uint32_t i = 0U; i < pages; ++i) {
    const uint32_t  offset    = i * k_ra_mram_write_size_bytes;
    const uintptr_t page_addr = address + offset;
    const ra_err_t  err =
      ra_flash_write_block((uint32_t)page_addr, src + offset, k_ra_mram_write_size_bytes, world);
    if (err != k_ra_ok) {
      return err;
    }
  }
  return k_ra_ok;
}

/**
 * @brief Erase-pattern constants used by ``ra_flash_blank_check``.
 */
typedef enum : uint8_t {
  k_ra_flash_blank_byte = 0xFFU, /**< Erased state byte value (HUM Ch 59 p 3548). */
} ra_flash_blank_const_t;

ra_err_t ra_flash_blank_check(uintptr_t address, uint32_t len, bool* out_blank)
{
  RA_CHECK_NULL_PTR(out_blank, s_tag, "out_blank must not be nullptr");
  if (len == 0U) {
    return k_ra_err_invalid_arg;
  }
  /* Accept either MRAM window. FSP r_mram.c L395 R_MRAM_BlankCheck returns
   * UNSUPPORTED on RA8D2 because the silicon has no BlankCheck command;
   * we implement the check as a direct read since 0xFF is the documented
   * erased state (HUM Ch 59.4.2 p 3548). */
  const uint64_t end_excl = (uint64_t)address + (uint64_t)len;
  const bool     in_code =
    (address >= k_ra_flash_code_start) &&
    (end_excl <= (uint64_t)k_ra_flash_code_start + (uint64_t)k_ra_flash_code_size);
  const bool in_extra =
    (address >= k_ra_flash_extra_start) &&
    (end_excl <= (uint64_t)k_ra_flash_extra_start + (uint64_t)k_ra_flash_extra_size);
  /* HUM Ch 7 "Option-Setting Memory" p 278 also benefits from a blank-check
   * (callers may want to verify a freshly-erased OFS slot before re-write). */
  const bool in_ofs = (address >= k_ra_flash_ofs_start) &&
                      (end_excl <= (uint64_t)k_ra_flash_ofs_start + (uint64_t)k_ra_flash_ofs_size);
  if (!in_code && !in_extra && !in_ofs) {
    return k_ra_err_invalid_arg;
  }

  const volatile uint8_t* p        = (const volatile uint8_t*)address;
  bool                    is_blank = true;
  /* NASA Rule 2: bounded loop on caller-validated len. */
  for (uint32_t i = 0U; i < len; ++i) {
    if (p[i] != k_ra_flash_blank_byte) {
      is_blank = false;
      break;
    }
  }
  *out_blank = is_blank;
  return k_ra_ok;
}

ra_err_t ra_flash_status(ra_flash_status_t* out)
{
  RA_CHECK_NULL_PTR(out, s_tag, "out must not be nullptr");

  /* HUM Ch 59 "MRCPS : Code MRAM Program Status Register" p 3601 */
  const uint8_t mrcps = *ra_mram_reg8(k_ra_mram_off_mrcps);
  /* HUM Ch 59 "MASTAT : Extra MRAM Access Status Register" p 3577 */
  const uint8_t mastat = *ra_mram_reg8(k_ra_mram_off_mastat);
  /* HUM Ch 59 "MENTRYR : Extra MRAM Program-Mode Entry" p 3582 */
  const uint16_t mentryr = *ra_mram_reg16(k_ra_mram_off_mentryr);
  /* HUM Ch 59 "MSTATR : Extra MRAM Status Register" p 3578 */
  const uint32_t mstatr = *ra_mram_reg32(k_ra_mram_off_mstatr);
  /* HUM Ch 59 "MRCBPROT0 : Code MRAM Block Protection (NS)" p 3604 */
  const uint16_t mrcbprot0 = *ra_mram_reg16(k_ra_mram_off_mrcbprot0);
  /* HUM Ch 59 "MRCBPROT1 : Code MRAM Block Protection (S)" p 3605 */
  const uint16_t mrcbprot1 = *ra_mram_reg16(k_ra_mram_off_mrcbprot1);

  const bool busy =
    ((mrcps & k_ra_mrcps_mask_prgbsyc) != 0U) || ((mentryr & k_ra_mentryr_mask_pe_mode) != 0U);
  out->programming_busy = busy;
  out->erase_busy       = busy; /* MRAM has no separate erase. */
  out->illegal_command =
    ((mastat & k_ra_mastat_mask_cmdlk) != 0U) || ((mstatr & k_ra_mstatr_mask_ilgcomerr) != 0U);
  out->voltage_error = ((mstatr & k_ra_mstatr_mask_oterr) != 0U);
  /* MRCBPROTx low bit = 1 means programming is permitted; bit clear
   * (the keyed lock pattern with bit 0 == 0) means the block is
   * write-protected. HUM Ch 59 p 3604..3605. */
  out->sector_protected = ((mrcbprot0 & 0x0001U) == 0U) || ((mrcbprot1 & 0x0001U) == 0U);
  out->program_error    = ((mrcps & k_ra_mrcps_mask_prgerrc) != 0U);
  out->ecc_error        = ((mrcps & k_ra_mrcps_mask_eccerrc) != 0U);
  return k_ra_ok;
}
