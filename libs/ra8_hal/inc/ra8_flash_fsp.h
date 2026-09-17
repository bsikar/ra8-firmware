/**
 * @file ra8_flash_fsp.h
 * @brief Code MRAM driver -- FSP r_mram parity surface
 * @ingroup grp_hal_memory
 *
 * @par Tag
 * [Ring 3 / HAL] {World: S}
 *
 * @details
 * FSP-parity prototypes for the RA8D2 MRAM controller driver. This
 * sub-header is split out of ``ra8_flash.h`` (the thin umbrella) and holds
 * the ``R_MRAM_*``-mirroring surface: open / close, multi-block erase /
 * write, blank-check, decoded status, suspend / resume, block-lock, and
 * the soft access window. These entry points compose the core API in
 * ``ra8_flash_core.h`` and share its DANGEROUS / brick-capable warnings,
 * documented on the ``ra8_flash.h`` umbrella ``@file`` banner.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_flash_regs.h"
#include "ra8_flash_types.h"

/* =============================================================================
 * FSP r_mram parity surface
 * =============================================================================
 */

/**
 * @brief FSP-parity bring-up: equivalent to ``ra8_flash_init``.
 *
 * @details
 * Mirrors ``R_MRAM_Open`` (FSP ``r_mram.c`` line 253). Registers the
 * controller for use, programs MRCFREQ / MREFREQ, and locks both
 * program-control gates. After ``ra8_flash_open`` callers may invoke
 * ``ra8_flash_write`` / ``ra8_flash_erase`` / ``ra8_flash_blank_check`` /
 * ``ra8_flash_status`` / ``ra8_flash_set_window``.
 *
 * @param[in] cfg Non-NULL configuration descriptor.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Controller opened.
 * @retval k_ra8_err_null_ptr ``cfg`` was NULL.
 * @retval k_ra8_err_invalid_arg ``cfg`` field out of range.
 *
 * @pre IRQs masked or single-threaded init context.
 * @pre Caller is not currently executing out of MRAM that will be programmed.
 * @post Soft access window (``ra8_flash_set_window``) state is preserved
 *       across re-open; defaults to "all-allowed" on first boot.
 * @post ``MRCPC0`` / ``MRCPC1`` are locked.
 *
 * @note Thread-safe: no, single-threaded init only.
 * @see ra8_flash_close
 * @see ra8_flash_init
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_flash_open(const ra8_flash_cfg_t* cfg);

/**
 * @brief FSP-parity tear-down: equivalent to ``ra8_flash_deinit``.
 *
 * @details
 * Mirrors ``R_MRAM_Close`` (FSP ``r_mram.c`` line 646). Locks all
 * program gates, exits P/E mode, clears sticky errors, re-enables
 * prefetch.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Always.
 *
 * @pre No write/erase operation in progress.
 * @post Controller is in pure read mode.
 *
 * @note Thread-safe: no.
 * @see ra8_flash_open
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_flash_close(void);

/**
 * @brief Erase ``num_blocks`` consecutive 32-byte MRAM blocks.
 *
 * @details
 * Mirrors ``R_MRAM_Erase`` (FSP ``r_mram.c`` line 365). Loops over
 * ``ra8_flash_erase_block`` once per block. The world (NS / S) is
 * inferred from the destination address: addresses inside the secure
 * code-MRAM alias use MRCPC1, all others use MRCPC0.
 *
 * @param[in] address    32-byte aligned destination inside code-MRAM.
 * @param[in] num_blocks Number of consecutive 32-byte blocks to erase.
 *                       Must be > 0 and inside the code-MRAM window.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok All blocks erased.
 * @retval k_ra8_err_invalid_arg Address misaligned, ``num_blocks`` is 0,
 *         or the range exceeds the code-MRAM window.
 * @retval k_ra8_err_out_of_range Address blocked by the soft access
 *         window set via ``ra8_flash_set_window``.
 * @retval k_ra8_err_hw_error Controller reported a program error.
 * @retval k_ra8_err_hw_timeout Controller never observed OPDONE.
 *
 * @pre ``address`` is 32-byte aligned and inside code-MRAM.
 * @pre ``ra8_flash_open`` (or ``ra8_flash_init``) has been called.
 * @post On success, every byte in [address, address + 32*num_blocks) reads 0xFF.
 * @post Program-control gate is locked on every exit path.
 *
 * @note Thread-safe: no.
 * @warning Same brick warnings as ``ra8_flash_write_block``.
 * @see ra8_flash_erase_block
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_flash_erase(uintptr_t address, uint32_t num_blocks);

/**
 * @brief Program ``len`` bytes into code-MRAM starting at ``address``.
 *
 * @details
 * Mirrors ``R_MRAM_Write`` (FSP ``r_mram.c`` line 323 + ``mram_write_data``
 * line 861). The driver chunks the request into per-page writes of up to
 * 32 bytes (``k_ra8_mram_write_size_bytes``), each one going through
 * ``ra8_flash_write_block``. ``len`` must be a non-zero multiple of the
 * 32-byte page size; arbitrary lengths are rejected to match FSP's
 * ``BSP_FEATURE_MRAM_PROGRAMMING_SIZE_BYTES`` boundary requirement.
 *
 * @param[in] address Destination start address (32-byte aligned, inside code-MRAM).
 * @param[in] src     Non-NULL source buffer of at least ``len`` bytes.
 * @param[in] len     Length in bytes; must be non-zero and a multiple of 32.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok All bytes written.
 * @retval k_ra8_err_null_ptr ``src`` was NULL.
 * @retval k_ra8_err_invalid_arg ``address`` misaligned, ``len`` not a
 *         multiple of 32, or range outside the code-MRAM window.
 * @retval k_ra8_err_out_of_range Range blocked by the soft access window.
 * @retval k_ra8_err_hw_error Controller reported a program error.
 * @retval k_ra8_err_hw_timeout Controller never observed OPDONE.
 *
 * @pre ``src`` non-null; ``address`` 32-byte aligned; ``len`` multiple of 32.
 * @pre ``ra8_flash_open`` (or ``ra8_flash_init``) has been called.
 * @post On success, the destination range holds the source bytes.
 * @post Program-control gate is locked on every exit path.
 *
 * @note Thread-safe: no.
 * @warning Same brick warnings as ``ra8_flash_write_block``.
 * @see ra8_flash_write_block
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_flash_write(uintptr_t address, const uint8_t* src, uint32_t len);

/**
 * @brief Check whether a region holds the erase pattern (all 0xFF).
 *
 * @details
 * FSP's ``R_MRAM_BlankCheck`` is a stub (returns FSP_ERR_UNSUPPORTED on
 * RA8D2 -- see ``r_mram.c`` line 395). The HUM Ch 59 layout uses 0xFF as
 * the natural erased state, so this driver implements the check as a
 * direct read of the region in 16-byte chunks. ``*out_blank`` is true
 * iff every byte in [address, address + len) equals 0xFF.
 *
 * @param[in]  address   Start address inside code-MRAM, extra-MRAM, or OFS.
 * @param[in]  len       Length in bytes; must be > 0 and inside one window.
 * @param[out] out_blank Non-NULL destination for the result.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Check completed; ``*out_blank`` set.
 * @retval k_ra8_err_null_ptr ``out_blank`` was NULL.
 * @retval k_ra8_err_invalid_arg ``len`` is 0 or range outside MRAM windows.
 *
 * @pre ``out_blank`` non-null and ``len`` > 0.
 * @pre ``ra8_flash_open`` (or ``ra8_flash_init``) has been called.
 * @post ``*out_blank`` reflects whether every byte equals 0xFF.
 * @post No state change in the controller.
 *
 * @note Thread-safe: pure reads; not atomic if concurrent writers exist.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_flash_blank_check(uintptr_t address, uint32_t len, bool* out_blank);

/**
 * @brief Decode the controller status into a flat ``ra8_flash_status_t``.
 *
 * @details
 * Reads MRCPS / MASTAT / MENTRYR / MSTATR / MRCBPROT0 / MRCBPROT1 and
 * collapses the bits down to the FSP-parity ``ra8_flash_status_t``
 * boolean fields. HUM Ch 59 p 3577..3605.
 *
 * @param[out] out Non-NULL destination structure.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Status decoded.
 * @retval k_ra8_err_null_ptr ``out`` was NULL.
 *
 * @pre ``out`` non-null.
 * @pre Controller is powered (always true after reset).
 * @post Every ``out->`` field reflects the registers at call time.
 *
 * @note Thread-safe: pure reads; not atomic across registers.
 * @see ra8_flash_get_extended_status
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_flash_status(ra8_flash_status_t* out);

/**
 * @brief Pause an in-flight MRAM program/erase operation.
 *
 * @details
 * Drives the MENTRYR pause-key (KEY=0xAA, MENTRY=1, plus the project-
 * internal ``PCKA`` "Pause-Code MRAM Access" bit, see HUM Ch 59
 * "MENTRYR : Extra MRAM Program-Mode Entry" pp 3582+).  The
 * controller halts the currently-running MACI command after the next
 * 32-byte page boundary.  Resume with ::ra8_flash_resume.
 *
 * @return ::ra8_err_t outcome.
 * @retval k_ra8_ok            Suspend latched.
 * @retval k_ra8_err_hw_timeout MENTRYR.PCKA never went to 1.
 *
 * @pre  ::ra8_flash_init has been called.
 * @pre  Caller is in IRQ-masked or single-threaded context.
 * @post Programming halts at the next page boundary.
 *
 * @note Not thread-safe.
 * @see ra8_flash_resume
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_flash_suspend(void);

/**
 * @brief Resume a previously-paused MRAM operation.
 *
 * @details
 * Drives MENTRYR with the resume key (KEY=0xAA, MENTRY=1, PCKA=0).
 * See HUM Ch 59 "MENTRYR" pp 3582+.
 *
 * @return ::ra8_err_t outcome.
 * @retval k_ra8_ok            Operation resumed (or no-op).
 * @retval k_ra8_err_hw_timeout MENTRYR.PCKA never went to 0.
 *
 * @pre  ::ra8_flash_init has been called.
 * @pre  Caller is in IRQ-masked or single-threaded context.
 * @post Programming continues on the next clock.
 *
 * @note Not thread-safe.
 * @see ra8_flash_suspend
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_flash_resume(void);

/**
 * @brief Programme MRCBPROT0/1 lock bits at @p addr.
 *
 * @details
 * Writes the keyed value @p lock_bits into MRCBPROT0 (when @p addr
 * falls in the non-secure code-MRAM half) or MRCBPROT1 (secure
 * half).  See HUM Ch 59 "MRCBPROT0" p 3604 and "MRCBPROT1" p 3605.
 *
 * @param[in] addr      Address inside the code-MRAM window.  Bit 19
 *                      selects secure (MRCBPROT1) vs non-secure
 *                      (MRCBPROT0).
 * @param[in] lock_bits Keyed 16-bit value to programme.
 *
 * @return ::ra8_err_t outcome.
 * @retval k_ra8_ok               Lock register updated.
 * @retval k_ra8_err_invalid_arg  @p addr outside code-MRAM, or
 *                               @p lock_bits has an invalid key byte.
 *
 * @pre  ::ra8_flash_init has been called.
 * @pre  Caller is in single-threaded init context.
 * @post Selected MRCBPROTx register reflects @p lock_bits.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_flash_lock_set(uintptr_t addr, uint16_t lock_bits);

/**
 * @brief Configure the soft access window enforced by write/erase.
 *
 * @details
 * FSP's ``R_MRAM_AccessWindowSet`` is a stub on RA8D2 (returns
 * FSP_ERR_UNSUPPORTED -- see ``r_mram.c`` line 469); the silicon has no
 * FAWMON / FAWMR registers because MRAM uses block-protect bits in the
 * MRCBPROT0 / MRCBPROT1 registers instead. To preserve a useful FSP-style
 * surface, this driver maintains a *software* window: ``ra8_flash_write``,
 * ``ra8_flash_erase`` and ``ra8_flash_write_block`` reject any request that
 * touches an address outside [low, high). Pass ``low == high == 0`` to
 * disable the window (allow all addresses, the default after open).
 *
 * @param[in] low   Inclusive lower bound (or 0 to disable).
 * @param[in] high  Exclusive upper bound (or 0 to disable).
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Window applied.
 * @retval k_ra8_err_invalid_arg ``low`` >= ``high`` and not both zero.
 *
 * @pre ``low`` < ``high`` or both are zero.
 * @post Subsequent writes/erases enforce the window.
 *
 * @note Thread-safe: no -- caller must serialise vs writes/erases.
 * @see ra8_flash_block_protect_set
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_flash_set_window(uintptr_t low, uintptr_t high);

#ifdef __cplusplus
}
#endif
