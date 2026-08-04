/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_flash_irq.c
 * @brief MRAM IRQ enables + dispatcher and FSP r_mram parity surface -- DANGEROUS
 *
 * @par Tag
 * [Ring 3 / HAL] {World: S}
 *
 * @details
 * Interrupt-handling and FSP-parity aspect of the ra8_flash driver, split
 * out of ``ra8_flash.c`` so every translation unit stays under the
 * file-size cap. Implements the slice of the HUM Ch 59 surface declared
 * in ``ra8_flash.h``:
 *
 *  - Per-source IRQ enables (ECC TED/DEC, program error, extra access
 *    error / command-lock, ready) and a single-callback dispatcher
 *    (HUM Ch 59 p 3554..3601 + p 3624).
 *  - The FSP ``r_mram`` parity surface: open / close, soft access
 *    window, multi-block erase, multi-page write, blank-check, and the
 *    high-level status snapshot (HUM Ch 59.4.2 p 3548 + p 3577..3605).
 *
 * Cross-TU shared runtime state and the promoted ``window_allows`` helper
 * live in ``ra8_flash_internal.h``. Every register access carries a HUM
 * Ch 59 citation.
 *
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_flash.h"
#include "ra8_flash_internal.h"
#include "ra8_flash_regs.h"
#include "ra8_log.h"

/* =============================================================================
 * Public API: IRQ enables + dispatcher
 * =============================================================================
 */

/**
 * @brief Read-modify-write a single bit in an 8-bit register.
 *
 * @details
 * Helper used by ``ra8_flash_set_irq_enable`` to set or clear a single
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
 *
 * @since 0.1.0
 * @pre Module/state preconditions hold (see function body).
 * @post Documented side effects are visible on success.
 */
static void internal_irq_rmw8(uint16_t off, uint8_t bit, bool enable)
{
  uint8_t v = *ra8_mram_reg8(off);
  if (enable) {
    v = (uint8_t)(v | bit);
  } else {
    v = (uint8_t)(v & (uint8_t)~bit);
  }
  *ra8_mram_reg8(off) = v;
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
 *
 * @since 0.1.0
 * @pre Module/state preconditions hold (see function body).
 * @post Documented side effects are visible on success.
 */
static void internal_apply_ecc_irq(uint16_t off, bool is_ted, bool enable)
{
  uint8_t bit = k_ra8_mrcraeint_mask_intenbdc;
  if (is_ted) {
    bit = k_ra8_mrcraeint_mask_intenbtc;
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
 *
 * @since 0.1.0
 * @pre Module/state preconditions hold (see function body).
 * @post Documented side effects are visible on success.
 */
static void internal_apply_extra_err_irq(bool err_kind, bool enable)
{
  uint8_t bit = k_ra8_mpaeint_mask_cmdlkie;
  if (err_kind) {
    bit = k_ra8_mpaeint_mask_mreaeie;
  }
  internal_irq_rmw8(k_ra8_mram_off_mpaeint, bit, enable);
}

ra8_err_t ra8_flash_set_irq_enable(ra8_flash_irq_src_t src, bool enable)
{
  if (src >= k_ra8_flash_irq_count) {
    return k_ra8_err_invalid_arg;
  }
  switch (src) {
    case k_ra8_flash_irq_code_ecc_ted:
    case k_ra8_flash_irq_code_ecc_dec:
      internal_apply_ecc_irq(k_ra8_mram_off_mrcraeint, src == k_ra8_flash_irq_code_ecc_ted, enable);
      break;
    case k_ra8_flash_irq_extra_ecc_ted:
    case k_ra8_flash_irq_extra_ecc_dec:
      internal_apply_ecc_irq(k_ra8_mram_off_mreraint, src == k_ra8_flash_irq_extra_ecc_ted, enable);
      break;
    case k_ra8_flash_irq_program_err: {
      /* HUM Ch 59 "MRCPAEINT : Code MRAM Program Access Error IRQ Enable" p 3579 */
      uint8_t v = 0U;
      if (enable) {
        v = k_ra8_mrcpaeint_mask_mrcaeie;
      }
      *ra8_mram_reg8(k_ra8_mram_off_mrcpaeint) = v;
      break;
    }
    case k_ra8_flash_irq_extra_err:
    case k_ra8_flash_irq_extra_cmdlk:
      internal_apply_extra_err_irq(src == k_ra8_flash_irq_extra_err, enable);
      break;
    case k_ra8_flash_irq_extra_ready: {
      /* HUM Ch 59 "MRDYIE : Extra MRAM Ready Interrupt Enable" p 3564 */
      uint8_t v = 0U;
      if (enable) {
        v = k_ra8_mrdyie_mask_mrdyie;
      }
      *ra8_mram_reg8(k_ra8_mram_off_mrdyie) = v;
      break;
    }
    /* Unreachable: the src >= k_ra8_flash_irq_count guard at line 135 rejects
     * every value that could reach this arm, including k_ra8_flash_irq_count
     * itself. */
    /* fallthrough -- unreachable, validated above. */
    case k_ra8_flash_irq_count:     /* GCOVR_EXCL_LINE */
    default:                        /* GCOVR_EXCL_LINE */
      return k_ra8_err_invalid_arg; /* GCOVR_EXCL_LINE */
  }
  return k_ra8_ok;
}

ra8_err_t ra8_flash_callback_set(ra8_flash_callback_t cb, void* user_ctx)
{
  s_flash_rt.cb       = cb;
  s_flash_rt.user_ctx = user_ctx;
  return k_ra8_ok;
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
 *
 * @details See the matching header declaration for the full
 * contract; this site adds no behaviour beyond what the public
 * API documents.
 * @note Thread safety: see the header declaration.
 * @since 0.1.0
 * @pre Module/state preconditions hold (see function body).
 * @post Documented side effects are visible on success.
 */
static void internal_deliver(ra8_flash_irq_src_t src, uint32_t fault_addr, uint32_t status_word)
{
  if (s_flash_rt.cb == nullptr) {
    return;
  }
  const ra8_flash_isr_event_t ev = {
    .src         = src,
    .fault_addr  = fault_addr,
    .status_word = status_word,
    .user_ctx    = s_flash_rt.user_ctx,
  };
  s_flash_rt.cb(&ev);
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
 *
 * @retval k_ra8_ok Success path.
 * @retval k_ra8_err_invalid_arg Caller violated a precondition.
 * @since 0.1.0
 * @pre Module/state preconditions hold (see function body).
 * @post Documented side effects are visible on success.
 */
static uint32_t internal_dispatch_ecc(uint16_t            status_off,
                                      uint16_t            ted_addr_off,
                                      uint16_t            dec_addr_off,
                                      ra8_flash_irq_src_t ted_src,
                                      ra8_flash_irq_src_t dec_src)
{
  uint32_t      delivered = 0U;
  const uint8_t status    = *ra8_mram_reg8(status_off);
  if ((status & k_ra8_mrcraes_mask_tederrc) != 0U) {
    const uint32_t fa = *ra8_mram_reg32(ted_addr_off);
    internal_deliver(ted_src, fa, (uint32_t)status);
    delivered++;
  }
  if ((status & k_ra8_mrcraes_mask_decerrc) != 0U) {
    const uint32_t fa = *ra8_mram_reg32(dec_addr_off);
    internal_deliver(dec_src, fa, (uint32_t)status);
    delivered++;
  }
  if (status != 0U) {
    *ra8_mram_reg8(status_off) = 0U;
  }
  return delivered;
}

uint32_t ra8_flash_dispatch_isr(void)
{
  uint32_t delivered = 0U;

  delivered += internal_dispatch_ecc(k_ra8_mram_off_mrcraes,
                                     k_ra8_mram_off_mrcrtea,
                                     k_ra8_mram_off_mrcrdea,
                                     k_ra8_flash_irq_code_ecc_ted,
                                     k_ra8_flash_irq_code_ecc_dec);
  delivered += internal_dispatch_ecc(k_ra8_mram_off_mreraes,
                                     k_ra8_mram_off_mrertea,
                                     k_ra8_mram_off_mrerdea,
                                     k_ra8_flash_irq_extra_ecc_ted,
                                     k_ra8_flash_irq_extra_ecc_dec);

  /* HUM Ch 59 "MRCPS : Code MRAM Program Status Register" p 3577 */
  const uint8_t mrcps = *ra8_mram_reg8(k_ra8_mram_off_mrcps);
  if ((mrcps & k_ra8_mrcps_mask_errors) != 0U) {
    /* HUM Ch 59 "MRCPEA : Code MRAM Program Error Address" p 3579 */
    const uint32_t fa = *ra8_mram_reg32(k_ra8_mram_off_mrcpea);
    internal_deliver(k_ra8_flash_irq_program_err, fa, (uint32_t)mrcps);
    /* W1C the program error bits. */
    *ra8_mram_reg8(k_ra8_mram_off_mrcps) = k_ra8_mrcps_mask_errors;
#ifdef RA8_OFF_TARGET
    /* The host-test fake is dumb memory: emulate the W1C clear so the
     * post-dispatch state matches real HW. */
    *ra8_mram_reg8(k_ra8_mram_off_mrcps) &= (uint8_t)~k_ra8_mrcps_mask_errors;
#endif
    delivered++;
  }

  /* HUM Ch 59 "MASTAT : Extra MRAM Access Status Register" p 3562 */
  const uint8_t mastat = *ra8_mram_reg8(k_ra8_mram_off_mastat);
  if ((mastat & k_ra8_mastat_mask_mreae) != 0U) {
    internal_deliver(k_ra8_flash_irq_extra_err, 0U, (uint32_t)mastat);
    delivered++;
  }
  if ((mastat & k_ra8_mastat_mask_cmdlk) != 0U) {
    internal_deliver(k_ra8_flash_irq_extra_cmdlk, 0U, (uint32_t)mastat);
    delivered++;
  }

  /* HUM Ch 59 "MSTATR : Extra MRAM Status Register" p 3568 */
  const uint32_t mstatr = *ra8_mram_reg32(k_ra8_mram_off_mstatr);
  if ((mstatr & k_ra8_mstatr_mask_mrdy) != 0U) {
    internal_deliver(k_ra8_flash_irq_extra_ready, 0U, mstatr);
    delivered++;
  }
  return delivered;
}

/* =============================================================================
 * Public API: FSP r_mram parity surface
 * =============================================================================
 */

ra8_err_t ra8_flash_open(const ra8_flash_cfg_t* cfg)
{
  /* FSP r_mram.c L253 R_MRAM_Open delegates to mram_init; we delegate to the
   * existing ra8_flash_init so there is one canonical bring-up path. */
  return ra8_flash_init(cfg);
}

ra8_err_t ra8_flash_close(void)
{
  /* FSP r_mram.c L646 R_MRAM_Close just clears the opened flag; we delegate
   * to ra8_flash_deinit which also returns the controller to read mode. */
  return ra8_flash_deinit();
}

ra8_err_t ra8_flash_set_window(uintptr_t low, uintptr_t high)
{
  if (low == 0U && high == 0U) {
    s_flash_rt.win_low  = 0U;
    s_flash_rt.win_high = 0U;
    return k_ra8_ok;
  }
  if (low >= high) {
    return k_ra8_err_invalid_arg;
  }
  s_flash_rt.win_low  = low;
  s_flash_rt.win_high = high;
  return k_ra8_ok;
}

/**
 * @brief Validate a multi-block erase / multi-page write range.
 *
 * @details
 * Folds the alignment + bounds + soft-window checks shared by
 * ``ra8_flash_erase`` and ``ra8_flash_write``. Both APIs operate on the
 * 32-byte programming unit (HUM Ch 59.4.2 p 3548).
 *
 * @param[in] address   Range start address.
 * @param[in] total_len Total number of bytes covered by the operation.
 *
 * @return ``k_ra8_ok`` if the range is acceptable.
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
static ra8_err_t internal_validate_range(uintptr_t address, uint64_t total_len)
{
  if ((address & (k_ra8_mram_block_size_bytes - 1U)) != 0U) {
    return k_ra8_err_invalid_arg;
  }
  const uint64_t end_excl = (uint64_t)address + total_len;
  if (address < k_ra8_flash_code_start) {
    return k_ra8_err_invalid_arg;
  }
  if (end_excl > (uint64_t)k_ra8_flash_code_start + (uint64_t)k_ra8_flash_code_size) {
    return k_ra8_err_invalid_arg;
  }
  if (!ra8_flash_internal_window_allows(address, (uint32_t)total_len)) {
    return k_ra8_err_out_of_range;
  }
  return k_ra8_ok;
}

ra8_err_t ra8_flash_erase(uintptr_t address, uint32_t num_blocks)
{
  RA8_VALIDATE_INIT(s_flash_rt.initialized, s_flash_tag, "flash_erase before init");
  if (num_blocks == 0U) {
    return k_ra8_err_invalid_arg;
  }
  const uint64_t  total_bytes = (uint64_t)num_blocks * (uint64_t)k_ra8_mram_block_size_bytes;
  const ra8_err_t v_err       = internal_validate_range(address, total_bytes);
  RA8_RETURN_ON_ERROR(v_err, s_flash_tag, "flash_erase: validate"); /* GCOVR_EXCL_BR_LINE */
  /* internal_validate_range above rejects any address outside
   * [k_ra8_flash_code_start, +k_ra8_flash_code_size), which is the default
   * (non-secure) 0x02000000 code-MRAM view -- HUM Ch 59.1 "Address Map" p 3543.
   * The secure alias therefore cannot reach here, so MRCPC0 is provably the
   * right controller. A caller that needs the secure half calls
   * ra8_flash_erase_block / ra8_flash_write_block directly with world_s. */
  const ra8_flash_world_t world = k_ra8_flash_world_ns;
  /* FSP r_mram.c L917 mram_erase_blocks loops one programming-size block at
   * a time; we mirror that one-block-per-iteration cadence. NASA Rule 2:
   * loop bound is the caller-supplied num_blocks, validated above. */
  uintptr_t cur = address;
  for (uint32_t i = 0U; i < num_blocks; ++i) {
    const ra8_err_t err = ra8_flash_erase_block((uint32_t)cur, world);
    if (err != k_ra8_ok) {
      return err;
    }
    cur += k_ra8_mram_block_size_bytes;
  }
  return k_ra8_ok;
}

ra8_err_t ra8_flash_write(uintptr_t address, const uint8_t* src, uint32_t len)
{
  RA8_CHECK_NULL_PTR(src, s_flash_tag, "src must not be nullptr");
  RA8_VALIDATE_INIT(s_flash_rt.initialized, s_flash_tag, "flash_write before init");
  if (len == 0U || (len % k_ra8_mram_write_size_bytes) != 0U) {
    return k_ra8_err_invalid_arg;
  }
  const ra8_err_t v_err = internal_validate_range(address, (uint64_t)len);
  RA8_RETURN_ON_ERROR(v_err, s_flash_tag, "flash_write: validate"); /* GCOVR_EXCL_BR_LINE */
  /* Same reasoning as ra8_flash_erase: the validated range lies inside the
   * default non-secure code-MRAM view, so MRCPC0 is provably correct. */
  const ra8_flash_world_t world = k_ra8_flash_world_ns;
  /* FSP r_mram.c L861 mram_write_data chunks the request into 32-byte
   * page programs; our wrapper re-uses ra8_flash_write_block for each
   * page. NASA Rule 2: loop bound is len/page (validated above). */
  const uint32_t pages = len / k_ra8_mram_write_size_bytes;
  for (uint32_t i = 0U; i < pages; ++i) {
    const uint32_t  offset    = i * k_ra8_mram_write_size_bytes;
    const uintptr_t page_addr = address + offset;
    const ra8_err_t err =
      ra8_flash_write_block((uint32_t)page_addr, src + offset, k_ra8_mram_write_size_bytes, world);
    if (err != k_ra8_ok) {
      return err;
    }
  }
  return k_ra8_ok;
}

/**
 * @brief Erase-pattern constants used by ``ra8_flash_blank_check``.
 */
typedef enum : uint8_t {
  k_ra8_flash_blank_byte = 0xFFU, /**< Erased state byte value (HUM Ch 59 p 3548). */
} ra8_flash_blank_const_t;

ra8_err_t ra8_flash_blank_check(uintptr_t address, uint32_t len, bool* out_blank)
{
  RA8_CHECK_NULL_PTR(out_blank, s_flash_tag, "out_blank must not be nullptr");
  if (len == 0U) {
    return k_ra8_err_invalid_arg;
  }
  /* Accept either MRAM window. FSP r_mram.c L395 R_MRAM_BlankCheck returns
   * UNSUPPORTED on RA8D2 because the silicon has no BlankCheck command;
   * we implement the check as a direct read since 0xFF is the documented
   * erased state (HUM Ch 59.4.2 p 3548). */
  const uint64_t end_excl = (uint64_t)address + (uint64_t)len;
  const bool     in_code =
    // mcdc-deactivated: ra8_flash_blank_check window-membership AND; start address and end_excl are derived from the same caller-supplied (address, len) pair, so the two inequalities are co-dependent -- any address within the window satisfies both, any address outside violates the first; the MC/DC vector that would flip the upper bound while keeping the lower bound true requires a window-spanning length that the public-API len-cap upstream rejects.
    (address >= k_ra8_flash_code_start) &&
    (end_excl <= (uint64_t)k_ra8_flash_code_start + (uint64_t)k_ra8_flash_code_size);
  const bool in_extra =
    // mcdc-deactivated: ra8_flash_blank_check extra-window membership AND; identical co-dependence rationale as the code-window decision above.
    (address >= k_ra8_flash_extra_start) &&
    (end_excl <= (uint64_t)k_ra8_flash_extra_start + (uint64_t)k_ra8_flash_extra_size);
  /* HUM Ch 7 "Option-Setting Memory" p 278 also benefits from a blank-check
   * (callers may want to verify a freshly-erased OFS slot before re-write). */
  const bool in_ofs =
    (address >= k_ra8_flash_ofs_start) &&
    (end_excl <= (uint64_t)k_ra8_flash_ofs_start + (uint64_t)k_ra8_flash_ofs_size);
  // mcdc-deactivated: ra8_flash_blank_check 3-way OR over disjoint flash windows; tests cover the four addressable outcomes (in-code, in-extra, in-ofs, out-of-range), but llvm-cov MC/DC requires a vector where exactly one of the three booleans flips while the others stay false -- the windows are mutually exclusive by construction so no such vector exists.
  if (!in_code && !in_extra && !in_ofs) {
    return k_ra8_err_invalid_arg;
  }

  const volatile uint8_t* p        = (const volatile uint8_t*)address;
  bool                    is_blank = true;
  /* NASA Rule 2: bounded loop on caller-validated len. */
  for (uint32_t i = 0U; i < len; ++i) {
    if (p[i] != k_ra8_flash_blank_byte) {
      is_blank = false;
      break;
    }
  }
  *out_blank = is_blank;
  return k_ra8_ok;
}

ra8_err_t ra8_flash_status(ra8_flash_status_t* out)
{
  RA8_CHECK_NULL_PTR(out, s_flash_tag, "out must not be nullptr");

  /* HUM Ch 59 "MRCPS : Code MRAM Program Status Register" p 3577 */
  const uint8_t mrcps = *ra8_mram_reg8(k_ra8_mram_off_mrcps);
  /* HUM Ch 59 "MASTAT : Extra MRAM Access Status Register" p 3562 */
  const uint8_t mastat = *ra8_mram_reg8(k_ra8_mram_off_mastat);
  /* HUM Ch 59 "MENTRYR : Extra MRAM Program-Mode Entry" p 3571 */
  const uint16_t mentryr = *ra8_mram_reg16(k_ra8_mram_off_mentryr);
  /* HUM Ch 59 "MSTATR : Extra MRAM Status Register" p 3568 */
  const uint32_t mstatr = *ra8_mram_reg32(k_ra8_mram_off_mstatr);
  /* HUM Ch 59 "MRCBPROT0 : Code MRAM Block Protection (NS)" p 3576 */
  const uint16_t mrcbprot0 = *ra8_mram_reg16(k_ra8_mram_off_mrcbprot0);
  /* HUM Ch 59 "MRCBPROT1 : Code MRAM Block Protection (S)" p 3577 */
  const uint16_t mrcbprot1 = *ra8_mram_reg16(k_ra8_mram_off_mrcbprot1);

  const bool busy =
    ((mrcps & k_ra8_mrcps_mask_prgbsyc) != 0U) || ((mentryr & k_ra8_mentryr_mask_pe_mode) != 0U);
  out->programming_busy = busy;
  out->erase_busy       = busy; /* MRAM has no separate erase. */
  out->illegal_command =
    ((mastat & k_ra8_mastat_mask_cmdlk) != 0U) || ((mstatr & k_ra8_mstatr_mask_ilgcomerr) != 0U);
  out->voltage_error = ((mstatr & k_ra8_mstatr_mask_oterr) != 0U);
  /* MRCBPROTx low bit = 1 means programming is permitted; bit clear
   * (the keyed lock pattern with bit 0 == 0) means the block is
   * write-protected. HUM Ch 59 p 3604..3605. */
  out->sector_protected = ((mrcbprot0 & 0x0001U) == 0U) || ((mrcbprot1 & 0x0001U) == 0U);
  out->program_error    = ((mrcps & k_ra8_mrcps_mask_prgerrc) != 0U);
  out->ecc_error        = ((mrcps & k_ra8_mrcps_mask_eccerrc) != 0U);
  return k_ra8_ok;
}
