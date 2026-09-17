/**
 * @file ra8_flash_core.h
 * @brief Code MRAM + Extra MRAM + Option-Setting driver -- core API
 * @ingroup grp_hal_memory
 *
 * @par Tag
 * [Ring 3 / HAL] {World: S}
 *
 * @details
 * Core driver prototypes for the RA8D2 MRAM controller (``R_MRMS``).
 * This sub-header is split out of ``ra8_flash.h`` (the thin umbrella) and
 * holds the primary register-level API: bring-up / tear-down, status
 * snapshots, direct STR programming, block-protection, start-up-area
 * swap, anti-rollback counters, OFS configuration-set, W-HUK zeroize,
 * IRQ dispatch, ECC controls, clock-frequency notification, update
 * transfer, extra-MRAM program / erase, and P/E-mode batching.
 *
 * The full DANGEROUS / brick-capable warnings for these APIs live on the
 * ``ra8_flash.h`` umbrella ``@file`` banner; consumers should read that
 * banner before calling any *write* / *erase* / *block_protect* /
 * *config_set* / *startup* / *zeroize* entry point.
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

/**
 * @brief Initialise the MRAM controller for safe read access.
 *
 * @details
 * Performs the bring-up dance described in HUM Ch 59.4.3 Figure 59.6
 * p 3550 (frequency-down procedure) in its safe direction:
 *
 * 1. Disable the prefetch buffer (``MRCPFB`` <- 0).
 * 2. Write the keyed ``MRCFREQ`` and ``MREFREQ`` notifications so
 * the controller knows the wait-state count to apply.
 * 3. Apply ECC encoder / decoder enables from the cfg.
 * 4. Optionally re-enable prefetch.
 * 5. Lock both program-control gates (``MRCPC0`` / ``MRCPC1`` <-
 * KEY+disable) so a stray store cannot trigger an accidental
 * program.
 * 6. Clear sticky ECC + program error flags so the new run starts
 * from a known state.
 *
 * @param[in] cfg Non-NULL configuration descriptor.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Controller initialized, in read mode.
 * @retval k_ra8_err_null_ptr ``cfg`` was NULL.
 * @retval k_ra8_err_invalid_arg ``cfg->mrcfreq_mhz`` > 0x0FA or
 * ``cfg->mrefreq_mhz`` > 0x07D.
 *
 * @pre IRQs masked or single-threaded init context.
 * @pre Caller is not currently executing out of MRAM that will be
 * programmed (SRAM-resident init code is the safe pattern).
 * @post ``MRCPC0`` and ``MRCPC1`` are both in their write-disabled
 * (KEY-only) state.
 * @post ``MRPSC.MHSPEN`` is 0 (high-speed program disabled).
 *
 * @note Thread-safe: no, single-threaded init only.
 * @warning Calling this function while another bus initiator is
 * actively reading MRAM may produce one wait-state of
 * read corruption -- do it during boot only.
 *
 * @see ra8_flash_deinit
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_flash_init(const ra8_flash_cfg_t* cfg);

/**
 * @brief Deinitialise: lock all program gates and re-enable prefetch.
 *
 * @details
 * Inverse of ``ra8_flash_init``: the controller is left in the safest
 * possible state -- prefetch on, both ``MRCPC*`` registers locked,
 * high-speed program disabled. Status sticky bits are cleared. Also
 * exits P/E mode if the controller is currently in it.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Always (no failure path).
 *
 * @pre No write/erase operation in progress (caller waits).
 * @post Controller is in pure read mode.
 * @post All program-status error bits are cleared.
 *
 * @note Thread-safe: no.
 * @see ra8_flash_init
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_flash_deinit(void);

/**
 * @brief Snapshot the program-status register.
 *
 * @details
 * Wraps ``MRCPS`` (HUM Ch 59 register layout p 3601). The returned
 * value is a copy of the 8-bit register; callers should test against
 * ``k_ra8_mrcps_mask_*`` from ``ra8_flash_regs.h``.
 *
 * @param[out] out_status Non-NULL destination for the status byte.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Status copied.
 * @retval k_ra8_err_null_ptr ``out_status`` was NULL.
 *
 * @pre ``out_status`` non-null.
 * @pre MRAM controller is powered (always true after reset).
 * @post ``*out_status`` reflects the last-read value of ``MRCPS``.
 * @post No state change in the controller.
 *
 * @note Thread-safe: read-only, single-register access.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_flash_get_status(uint8_t* out_status);

/**
 * @brief Snapshot every status register the HUM exposes.
 *
 * @details
 * Reads MRCPS, MASTAT, MREZS, MCMDR, MSTATR in one call so the caller
 * sees a coherent picture of the controller. The fields cite the
 * source register in their docstrings.
 *
 * @param[out] out Non-NULL destination structure.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Status snapshot copied.
 * @retval k_ra8_err_null_ptr ``out`` was NULL.
 *
 * @pre ``out`` non-null.
 * @pre Controller is powered (always true after reset).
 * @post ``*out`` reflects the registers at the moment of the call.
 *
 * @note Thread-safe: pure reads; not atomic across registers.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_flash_get_extended_status(ra8_flash_status_ext_t* out);

/**
 * @brief Clear sticky program-error bits in ``MRCPS``.
 *
 * @details
 * Writes the W1C mask to ``MRCPS`` so that ``PRGERRC`` and
 * ``ECCERRC`` are cleared. The flow-control bits (busy / buffer
 * empty / buffer full) are not affected -- they are read-only.
 *
 * @param[in] mask Bits to clear; typically
 * ``k_ra8_mrcps_mask_errors``.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Bits cleared (or no-op if mask=0).
 * @retval k_ra8_err_invalid_arg ``mask`` had bits set outside the
 * clearable region.
 *
 * @pre ``mask`` & ``~k_ra8_mrcps_mask_errors`` == 0.
 * @pre No program operation in progress.
 * @post Bits identified by ``mask`` are 0 in ``MRCPS``.
 * @post Other ``MRCPS`` bits unchanged.
 *
 * @note Thread-safe: no.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_flash_clear_status(uint8_t mask);

/**
 * @brief Program 1..32 contiguous bytes into one MRAM page.
 *
 * @details
 * Implements the HUM Ch 59.4.2 Figure 59.4 procedure (page 3548):
 *
 * 1. Wait for ``PRGBSYC`` == 0 and ``ABUFFULL`` == 0.
 * 2. Open the appropriate program gate (MRCPC0 for NS, MRCPC1 for S).
 * 3. Set ``MRPSC.MHSPEN`` = 1 (high-speed program mode).
 * 4. Issue STR instructions to write ``len`` bytes to ``mram_addr``.
 * 5. Memory-barrier, then write the keyed flush to ``MRCFLR`` to
 * commit the partial-page buffer.
 * 6. Wait for ``ABUFEMP`` == 1 and ``PRGBSYC`` == 0.
 * 7. Close the program gate and clear ``MHSPEN``.
 * 8. Check ``PRGERRC`` / ``ECCERRC`` for errors.
 *
 * Writes that span a 32-byte boundary are rejected with
 * ``k_ra8_err_invalid_arg`` -- the caller must split such writes
 * into per-page calls. This matches the FSP ``mram_write_data``
 * loop which works one page at a time.
 *
 * @param[in] mram_addr Destination address inside the MRAM window
 * (``k_ra8_flash_code_start``..
 * ``k_ra8_flash_code_start + k_ra8_flash_code_size``).
 * @param[in] src Non-NULL source buffer of at least ``len``
 * bytes.
 * @param[in] len Number of bytes to write, 1..32.
 * @param[in] world ``k_ra8_flash_world_ns`` for the non-secure
 * half, ``k_ra8_flash_world_s`` for the secure
 * half.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Write completed and committed.
 * @retval k_ra8_err_null_ptr ``src`` was NULL.
 * @retval k_ra8_err_invalid_arg ``len`` was 0 or > 32, ``mram_addr``
 * outside the MRAM window, or the write
 * spans a 32-byte page boundary.
 * @retval k_ra8_err_hw_error Controller reported PRGERRC / ECCERRC
 * after the flush.
 *
 * @pre ``src`` non-null and ``len`` in [1, 32].
 * @pre ``mram_addr`` and ``mram_addr + len - 1`` are both inside the
 * MRAM window and the same 32-byte page.
 * @pre ``ra8_flash_init`` has been called.
 * @pre Caller's program counter is **not** in MRAM, or at least not in
 * the same memory the write targets.
 * @post On success, the destination bytes hold the source data and the
 * program-control gate is re-locked.
 * @post On error, the program-control gate is re-locked even on the
 * failure path.
 *
 * @note Thread-safe: no, must run with IRQs masked or with cooperative
 * guarantee that no other writer exists.
 *
 * @warning The driver does NOT verify that ``mram_addr`` is outside
 * the running image's ``.text``. Caller bears full
 * responsibility for not bricking the part.
 *
 * @see ra8_flash_erase_block
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_flash_write_block(uint32_t          mram_addr,
                                              const uint8_t*    src,
                                              uint32_t          len,
                                              ra8_flash_world_t world);

/**
 * @brief Erase (= program to all 0xFF) one 32-byte MRAM block.
 *
 * @details
 * MRAM does not have a distinct erase command -- the natural
 * "erased" state is all-ones, and "erase" is implemented as a
 * page-aligned write of 32 ``0xFF`` bytes. This wraps
 * ``ra8_flash_write_block`` with that fixed payload to keep callers
 * out of having to construct the buffer themselves.
 *
 * @param[in] mram_addr 32-byte aligned destination inside the MRAM
 * window.
 * @param[in] world Same semantics as ``ra8_flash_write_block``.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Block erased.
 * @retval k_ra8_err_invalid_arg ``mram_addr`` not 32-byte aligned or
 * outside the MRAM window.
 * @retval k_ra8_err_hw_error Controller reported a program error.
 *
 * @pre ``mram_addr`` is 32-byte aligned.
 * @pre ``mram_addr + 32`` <= ``k_ra8_flash_code_start +
 * k_ra8_flash_code_size``.
 * @post Block reads as all 0xFF.
 * @post Program-control gate left locked.
 *
 * @note Thread-safe: no.
 * @warning Same brick warning as ``ra8_flash_write_block``.
 *
 * @see ra8_flash_write_block
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_flash_erase_block(uint32_t mram_addr, ra8_flash_world_t world);

/**
 * @brief Disable the read-while-write prefetch buffer.
 *
 * @details
 * The HUM (Ch 59.5.1 MRCPFB p 3551) requires the prefetch buffer to
 * be cleared before any code-MRAM frequency change and is the safe
 * setting while a programming sequence is in flight. This wrapper
 * exposes the bit to callers that need to coordinate with their own
 * write loops.
 *
 * @param[in] disable ``true`` -> MRCPFB.MPFBEN:= 0 (prefetch off).
 * ``false`` -> MRCPFB.MPFBEN:= 1 (prefetch on).
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Always.
 *
 * @pre None (the register is always accessible).
 * @post ``MRCPFB.MPFBEN`` matches the inverse of ``disable``.
 *
 * @note Thread-safe: no.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_flash_set_rww_disable(bool disable);

/**
 * @brief Set or clear the per-world block-protection lock.
 *
 * @details
 * Wraps the keyed write to ``MRCBPROT0`` (NS) / ``MRCBPROT1`` (S)
 * documented at HUM Ch 59 p 3604..3605. The protection bit, once set,
 * blocks all subsequent ``MRCPC*``-gated stores to the matching half
 * of MRAM. ``permanent`` requests a fuse-style write: the bit cannot
 * be cleared after the next reset.
 *
 * @param[in] world Which half to lock (NS or S).
 * @param[in] lock true => block writes; false => unlock.
 * @param[in] permanent true => one-shot fuse; false => RW-lockable.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Bit applied.
 * @retval k_ra8_err_invalid_arg Cannot pass permanent + unlock.
 *
 * @pre Caller has run ``ra8_flash_init``.
 * @pre Caller understands that ``permanent=true`` is irreversible.
 * @post On success, MRCBPROTx reflects the requested state.
 * @post Other MRAM controller registers untouched.
 *
 * @note Thread-safe: no.
 * @warning ``permanent=true`` is irreversible. Re-flashing the part
 * will not clear the fuse.
 *
 * @see ra8_flash_write_block
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_flash_block_protect_set(ra8_flash_world_t world, bool lock, bool permanent);

/**
 * @brief Switch the start-up area between default and alternate banks.
 *
 * @details
 * HUM Ch 7 "Option-Setting Memory" p 278..299 documents the BTFLG
 * boot-area swap. ``temporary=true`` writes ``MSUACR`` (KEY=0x66) and
 * the swap takes effect immediately but is forgotten on reset.
 * ``temporary=false`` issues a configuration-set MACI command to update
 * BTFLG in extra-MRAM so the swap survives reset.
 *
 * @param[in] target ``k_ra8_flash_startup_default`` or ``_alternate``.
 * @param[in] temporary true => MSUACR-only; false => BTFLG persistent.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Swap applied.
 * @retval k_ra8_err_invalid_arg ``target`` out of range.
 * @retval k_ra8_err_hw_error Controller reported MSTATR error.
 * @retval k_ra8_err_hw_timeout MACI did not return MRDY in time.
 *
 * @pre ``target`` is a valid ``ra8_flash_startup_t`` value.
 * @pre Permanent boot-swap protection (FSPR) is not set.
 * @post On success, the next reset (or this reset, if temporary)
 * boots from the requested half.
 * @post Controller is back in read mode.
 *
 * @note Thread-safe: no.
 * @warning A failed configuration-set leaves BTFLG in an indeterminate
 * state. Reflash via SWD if the device cannot boot.
 *
 * @see ra8_flash_get_startup_area
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_flash_set_startup_area(ra8_flash_startup_t target, bool temporary);

/**
 * @brief Read the current start-up area selection.
 *
 * @details
 * Returns the bit-shifted MSUASMON snapshot so callers can decide
 * whether the part is currently booting from block 0 or 1, and whether
 * the swap is permanent.
 *
 * @param[out] out_btflg Non-NULL destination for MSUASMON.BTFLG (0/1).
 * @param[out] out_fspr Non-NULL destination for MSUASMON.FSPR (0/1).
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Status copied.
 * @retval k_ra8_err_null_ptr Either output pointer was NULL.
 *
 * @pre Both output pointers non-null.
 * @pre Controller is powered (always true after reset).
 * @post ``*out_btflg`` and ``*out_fspr`` populated from MSUASMON.
 *
 * @note Thread-safe: read-only.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_flash_get_startup_area(uint8_t* out_btflg, uint8_t* out_fspr);

/**
 * @brief Increment the selected anti-rollback counter.
 *
 * @details
 * Issues the MACI ``increment`` command (HUM Ch 7.2.21..23 p 296..297
 * + HUM Ch 59.4.4 p 3550). The driver enters P/E mode, programs
 * MCNTSELR, fires the two-byte command sequence, and waits for the
 * MSTATR.MRDY flag. The current counter value is read first to detect
 * overflow before the destructive write.
 *
 * @param[in] counter Counter to increment.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Counter incremented.
 * @retval k_ra8_err_invalid_arg ``counter`` out of range.
 * @retval k_ra8_err_out_of_range Counter already at its max value.
 * @retval k_ra8_err_hw_error MSTATR reported an error after the cmd.
 * @retval k_ra8_err_hw_timeout MACI never returned MRDY.
 *
 * @pre ``counter`` < ``k_ra8_flash_arc_count``.
 * @pre ``ra8_flash_init`` has been called.
 * @post On success, the counter advances by exactly 1.
 * @post Controller is back in read mode.
 *
 * @note Thread-safe: no.
 * @warning Counter increments are non-volatile and irreversible.
 *
 * @see ra8_flash_arc_read
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_flash_arc_increment(ra8_flash_arc_id_t counter);

/**
 * @brief Read the current value of an anti-rollback counter.
 *
 * @details
 * Returns the population count of the ARC bit-vector. ARC_OEMBL goes
 * through the MACI ``read counter`` command (HUM Ch 59 p 3589); the
 * other counters are memory-mapped reads of MCNTDTR0/1 or the
 * extra-MRAM ARC region (HUM Ch 7.2.22..23 p 296..297).
 *
 * @param[in] counter Counter to read.
 * @param[out] out_count Non-NULL destination for the count value.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Counter read.
 * @retval k_ra8_err_null_ptr ``out_count`` was NULL.
 * @retval k_ra8_err_invalid_arg ``counter`` out of range.
 * @retval k_ra8_err_hw_timeout MACI never returned MRDY (OEMBL only).
 *
 * @pre ``counter`` < ``k_ra8_flash_arc_count`` and ``out_count`` non-null.
 * @pre ``ra8_flash_init`` has been called.
 * @post On success, ``*out_count`` holds the population count.
 * @post Controller back in read mode (OEMBL path).
 *
 * @note Thread-safe: no.
 * @see ra8_flash_arc_increment
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_flash_arc_read(ra8_flash_arc_id_t counter, uint32_t* out_count);

/**
 * @brief Issue an 8-halfword MACI program command to the OFS or extra-MRAM area.
 *
 * @details
 * Low-level primitive that streams ``<opener>, N, 8 halfwords, 0xD0`` through
 * the MACI command-issuing area at ``0x4012_0000`` and waits for MSTATR.MRDY.
 * The opener opcode is chosen from @p target_addr's region so the HARDWARE-
 * correct command is issued:
 *  - OFS configuration area (HUM Ch 7.2.x p 280..299): the Configuration Set
 *    command (``0x40``, HUM Ch 59.7.4.8 p 3594). This is the escape hatch
 *    ``ra8_flash_set_startup_area`` builds on.
 *  - Extra-MRAM option-setting / OTP area (0x02E07600, HUM Ch 59.7.4.5
 *    Table 59.15 p 3592): the Program command (``0xE8``, HUM Ch 59.7.4.5
 *    "Program Command" Fig 59.13 p 3591).
 *    This is the primitive ``ra8_flash_extra_mram_write`` builds on. Config-Set
 *    is NOT interchangeable here -- against the data area it raises
 *    MSTATR.CFGSETERR and leaves the target blank.
 *
 * @param[in] target_addr OFS-window or extra-MRAM-window address to program.
 * @param[in] words Pointer to 8 halfwords.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Write completed.
 * @retval k_ra8_err_null_ptr ``words`` was NULL.
 * @retval k_ra8_err_invalid_arg ``target_addr`` outside both windows.
 * @retval k_ra8_err_hw_error MSTATR reported an error.
 * @retval k_ra8_err_hw_timeout MACI never returned MRDY.
 *
 * @pre ``words`` non-null and points to 8 valid halfwords.
 * @pre ``target_addr`` lies inside the OFS window
 *      ``[k_ra8_flash_ofs_start, +k_ra8_flash_ofs_size)`` (HUM Ch 7 p 278) OR the
 *      extra-MRAM window ``[k_ra8_flash_extra_start, +k_ra8_flash_extra_size)``
 *      (HUM Ch 59.1 "Address Map" p 3543).
 * @post On success, the addressed region holds the new values.
 * @post Controller back in read mode.
 *
 * @note Thread-safe: no.
 * @warning OFS overwrites are persistent and may brick the part.
 *
 * @see ra8_flash_set_startup_area
 * @see ra8_flash_extra_mram_write
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_flash_config_set_write(uint32_t target_addr, const uint16_t* words);

/**
 * @brief Trigger the W-HUK zeroize via ``MREZC``.
 *
 * @details
 * Permanently destroys the wrapped HUK (HUM Ch 59 p 3565). The driver
 * writes the keyed value ``0x5501`` to MREZC and waits for
 * MREZS.WHUKEXE to fall.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Zeroization complete.
 * @retval k_ra8_err_hw_timeout WHUKEXE never cleared.
 *
 * @pre ``ra8_flash_init`` has been called.
 * @pre Caller knows this is a one-shot, irreversible operation.
 * @post ``MREZS.WHUKZF`` reads 1 (latched).
 * @post ``MREZS.WHUKEXE`` reads 0 (idle).
 *
 * @note Thread-safe: no.
 * @warning Permanent destruction of the W-HUK. Do not call unless
 * policy explicitly requires it.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_flash_zeroize_huk(void);

/**
 * @brief Issue the MACI ``forced stop`` command.
 *
 * @details
 * Aborts any in-flight MACI sequence (HUM Ch 59 p 3589 + FSP
 * ``mram_stop``). After the call, the controller is left in P/E mode
 * but with the command queue idle. Callers usually pair this with
 * ``ra8_flash_exit_pe_mode``.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Command accepted, MRDY observed.
 * @retval k_ra8_err_hw_timeout MRDY never came back.
 * @retval k_ra8_err_hw_error MASTAT.CMDLK still set.
 *
 * @pre Controller is powered.
 * @pre Caller is prepared for the in-flight operation to be aborted.
 * @post MACI command queue is empty.
 * @post MASTAT.CMDLK == 0 on success.
 *
 * @note Thread-safe: no.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_flash_force_stop(void);

/**
 * @brief Reset the MRAM peripheral and clear status.
 *
 * @details
 * Mirrors ``R_MRAM_Reset`` (FSP ``mram_reset``): enter P/E, issue
 * forced-stop, status-clear, exit to read. Clears every sticky error
 * flag the MRCPS, MRCRAES, MRERAES, MASTAT, MSTATR registers carry.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Controller reset.
 * @retval k_ra8_err_hw_timeout MACI never returned MRDY.
 * @retval k_ra8_err_hw_error MASTAT.CMDLK still set after the reset.
 *
 * @pre ``ra8_flash_init`` has been called.
 * @post All sticky error bits cleared.
 * @post Controller back in read mode.
 *
 * @note Thread-safe: no.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_flash_reset(void);

/**
 * @brief Update MSAR (MRAM Security Attribution).
 *
 * @details
 * HUM Ch 59.5.13 p 3559. Each bit selects whether the matching
 * register subset is reachable from the secure (1) or non-secure (0)
 * world. ``new_msar`` is written verbatim; the caller is responsible
 * for understanding the per-bit semantics in
 * ``ra8_msar_mask_t``.
 *
 * @param[in] new_msar Value to store.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Always.
 *
 * @pre Caller is in the secure world (SAU permits the access).
 * @pre Caller has reviewed every bit they intend to flip.
 * @post ``MSAR`` reads back ``new_msar`` (subject to read-only bits).
 *
 * @note Thread-safe: no.
 * @warning Demoting a register set to non-secure exposes it to NS code.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_flash_set_security_attribution(uint16_t new_msar);

/**
 * @brief Enable or disable MRAM-controller IRQs by source.
 *
 * @details
 * Routes per-source enable bits to the matching register:
 * - ``code_ecc_*`` -> ``MRCRAEINT`` (HUM Ch 59 p 3554).
 * - ``extra_ecc_*`` -> ``MRERAINT`` (HUM Ch 59 p 3557).
 * - ``program_err`` -> ``MRCPAEINT`` (HUM Ch 59 p 3601).
 * - ``extra_err`` / ``extra_cmdlk`` -> ``MPAEINT`` (HUM Ch 59 p 3577).
 * - ``extra_ready`` -> ``MRDYIE`` (HUM Ch 59 p 3577).
 *
 * @param[in] src Source to gate.
 * @param[in] enable true => enable, false => disable.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Enable bit applied.
 * @retval k_ra8_err_invalid_arg ``src`` out of range.
 *
 * @pre ``src`` < ``k_ra8_flash_irq_count``.
 * @pre ``ra8_flash_init`` has been called.
 * @post Matching enable bit set / cleared.
 *
 * @note Thread-safe: no.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_flash_set_irq_enable(ra8_flash_irq_src_t src, bool enable);

/**
 * @brief Register the unified IRQ callback.
 *
 * @details
 * The dispatcher (``ra8_flash_dispatch_isr``) walks every documented
 * status register and calls this callback once per pending source.
 * Pass NULL to deregister.
 *
 * @param[in] cb Callback function or NULL.
 * @param[in] user_ctx Opaque pointer passed back via ``ev->user_ctx``.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Always.
 *
 * @pre None.
 * @post Subsequent ``ra8_flash_dispatch_isr`` calls invoke ``cb``.
 * @post Pre-existing pending events are NOT replayed.
 *
 * @note Thread-safe: no -- caller must serialise vs ``dispatch_isr``.
 * @see ra8_flash_dispatch_isr
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_flash_callback_set(ra8_flash_callback_t cb, void* user_ctx);

/**
 * @brief Run the MRAM IRQ dispatcher (call from the BSP vector).
 *
 * @details
 * Walks MRCRAES (code-ECC), MRERAES (extra-ECC), MRCPS (program-err),
 * MASTAT (extra-err / cmdlk), MSTATR (extra-ready); for each pending
 * bit, builds a ``ra8_flash_isr_event_t`` and calls the registered
 * callback. After the callback returns, the dispatcher clears the
 * matching status flag (W1C bits) so the next call sees only fresh
 * events.
 *
 * @return Number of events delivered.
 *
 * @pre ``ra8_flash_callback_set`` registered a callback (otherwise a
 * ``no-op`` walk runs and 0 is returned).
 * @post Every W1C status bit observed at entry is cleared.
 * @post The registered callback was invoked once per pending source.
 *
 * @note Thread-safe: no, IRQ-context only.
 * @see ra8_flash_callback_set
 * @since 0.1.0
 *
 * @retval k_ra8_ok Success path.
 * @retval k_ra8_err_invalid_arg Caller violated a precondition.
 *
 * @pre Module has been initialized.
 */
uint32_t ra8_flash_dispatch_isr(void);

/**
 * @brief Toggle ``MRCEECC.ECCEN`` (program-side ECC encoder).
 *
 * @param[in] enable true => MRCEECC.ECCEN:= 1.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Always.
 *
 * @pre None.
 * @post MRCEECC.ECCEN matches ``enable``.
 *
 * @note Thread-safe: no.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_flash_set_ecc_encoder_enable(bool enable);

/**
 * @brief Toggle ``MRCDECC.DECECEN`` (read-side ECC decoder).
 *
 * @param[in] enable true => MRCDECC.DECECEN:= 1.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Always.
 *
 * @pre None.
 * @post MRCDECC.DECECEN matches ``enable``.
 *
 * @note Thread-safe: no.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_flash_set_ecc_decoder_enable(bool enable);

/**
 * @brief Snapshot the latched ECC fault addresses.
 *
 * @details
 * Reads MRCRTEA / MRCRDEA / MRERTEA / MRERDEA (HUM Ch 59 p 3555..3558).
 * Each output gets its register value or 0 if no fault was latched
 * since the last read-clear.
 *
 * @param[out] out_code_ted Non-NULL destination for MRCRTEA.
 * @param[out] out_code_dec Non-NULL destination for MRCRDEA.
 * @param[out] out_extra_ted Non-NULL destination for MRERTEA.
 * @param[out] out_extra_dec Non-NULL destination for MRERDEA.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Addresses copied.
 * @retval k_ra8_err_null_ptr Any destination pointer was NULL.
 *
 * @pre All four output pointers non-null.
 * @pre Controller is powered.
 * @post All four ``*out_*`` locations updated.
 *
 * @note Thread-safe: read-only.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_flash_get_ecc_error_addr(uint32_t* out_code_ted,
                                                     uint32_t* out_code_dec,
                                                     uint32_t* out_extra_ted,
                                                     uint32_t* out_extra_dec);

/**
 * @brief Snapshot the program-error address (MRCPEA).
 *
 * @param[out] out_addr Non-NULL destination for MRCPEA.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Address copied.
 * @retval k_ra8_err_null_ptr ``out_addr`` was NULL.
 *
 * @pre ``out_addr`` non-null.
 * @pre Controller is powered.
 * @post ``*out_addr`` populated from MRCPEA.
 *
 * @note Thread-safe: read-only.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_flash_get_program_error_addr(uint32_t* out_addr);

/**
 * @brief Re-issue the keyed MRCFREQ / MREFREQ to track a clock change.
 *
 * @details
 * Disables prefetch, writes the new frequency notifications, then
 * restores prefetch. Mirrors FSP ``R_MRAM_UpdateFlashClockFreq``.
 *
 * @param[in] mrcfreq_mhz New code-MRAM clock in MHz, 0..0x0FA.
 * @param[in] mrefreq_mhz New extra-MRAM clock in MHz, 0..0x07D.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Frequencies applied.
 * @retval k_ra8_err_invalid_arg Either value out of range.
 *
 * @pre Both inputs in range.
 * @pre No write/erase operation in flight.
 * @post MRCFREQ/MREFREQ reflect the new values.
 * @post MRCPFB restored to its prior state.
 *
 * @note Thread-safe: no.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_flash_update_clock_freq(uint16_t mrcfreq_mhz, uint8_t mrefreq_mhz);

/**
 * @brief Kick MSUINITR to re-load the OFS sequencer.
 *
 * @details
 * Writes the keyed value ``0xA501`` to MSUINITR (HUM Ch 59 p 3585) and
 * waits for the SUINIT bit to fall. Used after a configuration-set
 * write to make the controller pick up the new option-setting bytes
 * without requiring a chip reset.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Re-init complete.
 * @retval k_ra8_err_hw_timeout SUINIT never cleared.
 *
 * @pre ``ra8_flash_init`` has been called.
 * @post MSUINITR.SUINIT reads back 0.
 *
 * @note Thread-safe: no.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_flash_msuinitr_kick(void);

/**
 * @brief Trigger the MRAM update transfer (MCTRCNTR).
 *
 * @details
 * Selects an MCTRLSR list, writes the keyed start to MCTRCNTR (HUM
 * Ch 59 p 3580), and returns immediately. Use
 * ``ra8_flash_get_update_status`` to poll for completion.
 *
 * @param[in] list_select Which list (0..15) to run.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Transfer kicked.
 * @retval k_ra8_err_invalid_arg ``list_select`` > 15.
 *
 * @pre ``list_select`` <= 15.
 * @pre ``ra8_flash_init`` has been called.
 * @post MCTRSTATR.BUSY likely 1 immediately after the call.
 *
 * @note Thread-safe: no.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_flash_set_update_transfer(uint8_t list_select);

/**
 * @brief Poll the MRAM update-transfer status.
 *
 * @param[out] out_busy Non-NULL destination for MCTRSTATR.BUSY.
 * @param[out] out_done Non-NULL destination for MCTRSTATR.DONE.
 * @param[out] out_err Non-NULL destination for MCTRSTATR.ERR.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Status copied.
 * @retval k_ra8_err_null_ptr Any destination pointer was NULL.
 *
 * @pre Output pointers non-null.
 * @post All three ``*out_*`` locations updated.
 *
 * @note Thread-safe: read-only.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_flash_get_update_status(uint8_t* out_busy, uint8_t* out_done, uint8_t* out_err);

/**
 * @brief Program 1..32 contiguous bytes into the general-purpose extra-MRAM window.
 *
 * @details
 * The extra-MRAM option-setting / OTP window
 * (``[k_ra8_flash_extra_start, +k_ra8_flash_extra_size)``, HUM Ch 59.7.4.5
 * Table 59.15 p 3592) is programmed through the MACI Program command rather than
 * the direct STR gate. This API mirrors ``ra8_flash_write_block`` semantics:
 * 1..32 bytes inside one page.
 *
 * **OTP-misuse guard (#397):** this is the *general-purpose* write path, so it
 * refuses any target at or above ``k_ra8_flash_extra_locked_start`` -- the
 * permanent, irreversible structures (PBPS, POFSPS, REVOKE, HUK-zeroize enable,
 * anti-rollback). Programming those can brick the part or destroy the wrapped
 * HUK, so they require the deliberate, separately-named
 * ``ra8_flash_config_set_write``. The general-purpose OTP sub-range
 * (``k_ra8_flash_gpotp_start``, HUM Ch 7.2.25 p 299) is the intended target for
 * ordinary callers. Note that the whole window is one-time-programmable on this
 * silicon -- there is no rewritable data-flash to erase and re-use.
 *
 * @param[in] mram_addr Destination inside the extra-MRAM window, below
 *                      ``k_ra8_flash_extra_locked_start``.
 * @param[in] src Non-NULL source buffer of at least ``len`` bytes.
 * @param[in] len 1..32.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Bytes written.
 * @retval k_ra8_err_null_ptr ``src`` was NULL.
 * @retval k_ra8_err_invalid_arg Range / alignment violation, or the target is a
 *                               permanent structure at/above the guard boundary.
 * @retval k_ra8_err_hw_error MSTATR error after the command.
 * @retval k_ra8_err_hw_timeout MACI never returned MRDY.
 *
 * @pre ``src`` non-null and ``len`` in [1, 32].
 * @pre ``mram_addr`` lies inside extra-MRAM below the guard boundary and
 * ``mram_addr+len-1`` lies on the same 32-byte page.
 * @pre ``ra8_flash_init`` has been called.
 * @post Data committed; controller back in read mode.
 *
 * @note Thread-safe: no.
 * @warning Same brick warnings as ``ra8_flash_write_block``.
 * @see ra8_flash_extra_mram_erase
 * @see ra8_flash_config_set_write
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_flash_extra_mram_write(uint32_t mram_addr, const uint8_t* src, uint32_t len);

/**
 * @brief Erase one 32-byte block of extra-MRAM via MACI.
 *
 * @details
 * Equivalent to ``ra8_flash_extra_mram_write`` with a 32-byte payload of
 * 0xFF.
 *
 * @param[in] mram_addr 32-byte aligned destination inside the
 * extra-MRAM window.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Block erased.
 * @retval k_ra8_err_invalid_arg Misaligned or out-of-range address.
 * @retval k_ra8_err_hw_error MSTATR error after the command.
 * @retval k_ra8_err_hw_timeout MACI never returned MRDY.
 *
 * @pre ``mram_addr`` is 32-byte aligned and inside the extra-MRAM
 * window.
 * @post Block reads as all 0xFF.
 *
 * @note Thread-safe: no.
 * @warning Brick warnings apply.
 * @see ra8_flash_extra_mram_write
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_flash_extra_mram_erase(uint32_t mram_addr);

/**
 * @brief Enter MRAM P/E mode.
 *
 * @details
 * Writes ``MENTRYR:= 0xAA80`` and waits for MENTRYR.MENTRY to go to 1
 * (HUM Ch 59 p 3582). Exposed so callers can batch multiple MACI
 * commands without paying the per-command transition cost.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Entered P/E.
 * @retval k_ra8_err_hw_timeout MENTRY never went to 1.
 *
 * @pre Controller is powered.
 * @post Controller is in P/E mode.
 *
 * @note Thread-safe: no.
 * @see ra8_flash_exit_pe_mode
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_flash_enter_pe_mode(void);

/**
 * @brief Exit MRAM P/E mode (return to read mode).
 *
 * @details
 * Writes ``MENTRYR:= 0xAA00`` and waits for the register to fall to
 * zero. Restores the prefetch buffer to its previous state.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Returned to read mode.
 * @retval k_ra8_err_hw_timeout MENTRYR never went to 0.
 *
 * @pre Controller was in P/E mode (no harm if it wasn't -- the write
 * is idempotent).
 * @post Controller back in read mode.
 *
 * @note Thread-safe: no.
 * @see ra8_flash_enter_pe_mode
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_flash_exit_pe_mode(void);

#ifdef __cplusplus
}
#endif
