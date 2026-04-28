/**
 * @file ra_flash.h
 * @brief Code MRAM + Extra MRAM + Option-Setting driver -- DANGEROUS, brick-capable
 *
 * @par Tag
 * [Ring 3 / HAL] {World: S}
 *
 * @details
 * On RA8D2 the "code flash" is **MRAM**, a non-volatile, byte-erasable
 * magnetoresistive RAM array exposed at ``0x02000000`` for 1 MiB. The
 * controller block (``R_MRMS``, secure base ``0x4013_C000``, HUM
 * Ch 59.5.1 p 3551) presents a small write-buffer + status register
 * window. Three independent write paths exist:
 *
 * - **Direct STR programming** of the code-MRAM page through the
 * ``MRCPC0`` / ``MRCPC1`` gate (HUM Ch 59.4.2 Figure 59.4 p 3548).
 * - **MACI command sequencer** for configuration-set, anti-rollback
 * counters, status-clear, and forced-stop (HUM Ch 59.4.4 p 3550 +
 * HUM Ch 7 p 278..299 for the OFS layout).
 * - **Block-protection writes** to ``MRCBPROT0`` / ``MRCBPROT1`` to
 * permanently or temporarily lock individual pages.
 *
 * This wave (Round 3) brings the driver to **full HUM Ch 7 + Ch 59
 * coverage**:
 *
 * - ``ra_flash_init`` / ``ra_flash_deinit`` -- bring-up + safe-state.
 * - ``ra_flash_get_status`` / ``ra_flash_clear_status`` -- MRCPS.
 * - ``ra_flash_get_extended_status`` -- MSTATR + MASTAT + MREZS.
 * - ``ra_flash_set_rww_disable`` -- MRCPFB prefetch park.
 * - ``ra_flash_write_block`` / ``ra_flash_erase_block`` -- 1..32 byte
 * direct programming.
 * - ``ra_flash_block_protect_set`` -- per-world MRCBPROT lock/unlock.
 * - ``ra_flash_set_startup_area`` -- temporary (MSUACR) and permanent
 * (configuration-set) boot-bank swap.
 * - ``ra_flash_get_startup_area`` -- read MSUASMON.
 * - ``ra_flash_arc_increment`` / ``ra_flash_arc_read`` --
 * anti-rollback counter MACI flow.
 * - ``ra_flash_config_set_write`` -- low-level OFS configuration-set.
 * - ``ra_flash_zeroize_huk`` -- MREZC trigger for the W-HUK zeroize.
 * - ``ra_flash_force_stop`` / ``ra_flash_reset`` -- abort + recover.
 * - ``ra_flash_set_security_attribution`` -- MSAR per-region SA.
 * - ``ra_flash_set_irq_enable`` -- per-source enable for the four
 * documented IRQ paths.
 * - ``ra_flash_callback_set`` -- single-callback IRQ dispatcher.
 * - ``ra_flash_dispatch_isr`` -- explicit dispatch entry the
 * BSP IRQ vector calls.
 * - ``ra_flash_set_ecc_encoder_enable`` /
 * ``ra_flash_set_ecc_decoder_enable`` -- MRCEECC / MRCDECC controls.
 * - ``ra_flash_get_ecc_error_addr`` -- TED / DEC fault addresses.
 * - ``ra_flash_get_program_error_addr`` -- MRCPEA.
 * - ``ra_flash_update_clock_freq`` -- re-issue MRCFREQ / MREFREQ.
 * - ``ra_flash_msuinitr_kick`` -- re-fetch OFS via MSUINITR.
 * - ``ra_flash_set_update_transfer`` / ``ra_flash_get_update_status``
 * -- MCTRCNTR / MCTRSTATR / MCTRLSR.
 * - ``ra_flash_extra_mram_write`` / ``ra_flash_extra_mram_erase`` --
 * extra-MRAM (data) program / erase via MACI.
 * - ``ra_flash_enter_pe_mode`` / ``ra_flash_exit_pe_mode`` -- exposed
 * for callers that need to batch multiple MACI commands without
 * paying the per-call P/E transition cost.
 *
 * @warning **THIS DRIVER CAN PERMANENTLY BRICK THE MCU.** A program
 * that overwrites its own reset vectors, FSBL hash, the
 * option-setting trust anchors at ``0x0100_A100``, the
 * permanent block-protect bits, or the start-up area control
 * register may be unrecoverable through SWD and require
 * vendor RMA. NEVER call any *write* / *erase* /
 * *block_protect* / *config_set* / *startup* / *zeroize*
 * API with a destination inside the running image's
 * ``.text`` section, the secure boot area, or the OFS region
 * unless you are absolutely certain. The driver does **not**
 * sanity-check the destination beyond enforcing the 1 MB
 * MRAM window -- caller is fully responsible.
 *
 * @warning ``ra_flash_zeroize_huk`` is **one-shot and permanent**. Once
 * fired, the silicon's wrapped HUK (W-HUK) is destroyed and
 * every secret that was wrapped under it becomes irretrievable.
 * The matching ``MREZS.WHUKZF`` flag latches forever. 
 * callers should keep this API behind a project-level
 * policy gate.
 *
 * @warning ``ra_flash_block_protect_set`` with the *permanent* flag
 * set writes a one-shot fuse. Once set, the block can never
 * be re-enabled without a new fuse cycle on the part. There
 * is no undo.
 *
 * @warning The driver does not protect against power loss mid-write.
 * A reset between the data store and the ``MRCFLR`` flush
 * leaves the affected 32-byte page in an indeterminate
 * state. For at-rest critical regions use a journaled
 * two-bank scheme on top of this driver.
 *
 * @warning Programming **must not** run from MRAM that is being
 * programmed. The HUM allows BGO (background read of the
 * opposite memory while the other is programmed) but the
 * code-MRAM path requires the write loop itself to live
 * in SRAM. ``ra_flash_write_block`` does **not** relocate
 * itself -- caller must ensure the call site is in SRAM
 * (e.g. ``__attribute__((section(".sram_text")))``) when
 * targeting MRAM addresses.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8d2_flash_regs.h"
#include "ra_err.h"

/**
 * @enum ra_flash_world_t
 * @brief Which secure-attribution gate to open for a write.
 *
 * @details
 * MRCPC0 controls writes to the non-secure MRAM half; MRCPC1 controls
 * the secure half. Cite HUM Ch 59 p 3601..3603 (MRCPC0/MRCPC1 register
 * descriptions in chapter range 3542..3625).
 */
typedef enum : uint8_t {
  k_ra_flash_world_ns = 0U, /**< Open MRCPC0 (non-secure half). */
  k_ra_flash_world_s  = 1U, /**< Open MRCPC1 (secure half). */
} ra_flash_world_t;

/**
 * @enum ra_flash_irq_src_t
 * @brief Source identifier for unified flash IRQ dispatcher.
 */
typedef enum : uint8_t {
  k_ra_flash_irq_code_ecc_ted  = 0U, /**< Code MRAM 1-bit ECC TED. */
  k_ra_flash_irq_code_ecc_dec  = 1U, /**< Code MRAM 2-bit ECC DEC. */
  k_ra_flash_irq_extra_ecc_ted = 2U, /**< Extra MRAM 1-bit ECC TED. */
  k_ra_flash_irq_extra_ecc_dec = 3U, /**< Extra MRAM 2-bit ECC DEC. */
  k_ra_flash_irq_program_err   = 4U, /**< Code MRAM program/erase error. */
  k_ra_flash_irq_extra_err     = 5U, /**< Extra MRAM access error. */
  k_ra_flash_irq_extra_cmdlk   = 6U, /**< Extra MRAM command-lock. */
  k_ra_flash_irq_extra_ready   = 7U, /**< Extra MRAM ready. */
  k_ra_flash_irq_count         = 8U, /**< Sentinel: number of sources. */
} ra_flash_irq_src_t;

/**
 * @enum ra_flash_startup_t
 * @brief Boot-area selection for ::ra_flash_set_startup_area.
 */
typedef enum : uint8_t {
  k_ra_flash_startup_default   = 0U, /**< Default startup area (BTFLG=1). */
  k_ra_flash_startup_alternate = 1U, /**< Alternate startup area (BTFLG=0). */
  k_ra_flash_startup_btflg     = 2U, /**< Validation sentinel. */
} ra_flash_startup_t;

/**
 * @enum ra_flash_arc_id_t
 * @brief Anti-rollback counter identifier.
 */
typedef enum : uint8_t {
  k_ra_flash_arc_sec    = 0U, /**< Secure ARC. */
  k_ra_flash_arc_oembl  = 1U, /**< OEM bootloader ARC. */
  k_ra_flash_arc_nsec_0 = 2U, /**< Non-secure ARC #0. */
  k_ra_flash_arc_nsec_1 = 3U, /**< Non-secure ARC #1. */
  k_ra_flash_arc_nsec_2 = 4U, /**< Non-secure ARC #2. */
  k_ra_flash_arc_nsec_3 = 5U, /**< Non-secure ARC #3. */
  k_ra_flash_arc_count  = 6U, /**< Sentinel. */
} ra_flash_arc_id_t;

/**
 * @struct ra_flash_cfg_t
 * @brief Initialisation descriptor for ``ra_flash_init``.
 *
 * @details
 * MRAM has no MSTP gate (it is the code memory and is always powered)
 * so the only thing the driver needs at init time is the operating
 * frequency the silicon should advertise to the controller via
 * ``MRCFREQ``. cppcheck cannot see callers in tests/ so it flags
 * every field as unused; the suppress comment matches what
 * ra_acmphs.h does.
 */
/* cppcheck-suppress-begin [unusedStructMember] */
typedef struct {
  uint16_t mrcfreq_mhz;        /**< Code MRAM clock in MHz, 0..0x0FA (250 MHz). */
  uint8_t  mrefreq_mhz;        /**< Extra MRAM clock in MHz, 0..0x07D (125 MHz). */
  bool     prefetch_en;        /**< true -> leave prefetch enabled at init. */
  bool     ecc_encoder_enable; /**< true -> MRCEECC.ECCEN=1 (default). */
  bool     ecc_decoder_enable; /**< true -> MRCDECC.DECECEN=1 (default). */
} ra_flash_cfg_t;
/* cppcheck-suppress-end [unusedStructMember] */

/**
 * @struct ra_flash_status_ext_t
 * @brief Extended status snapshot (MRCPS + MSTATR + MASTAT + MREZS).
 *
 * @details
 * One-shot snapshot of every status register that callers may want to
 * inspect after an operation. Populated by
 * ``ra_flash_get_extended_status``.
 */
/* cppcheck-suppress-begin [unusedStructMember] */
typedef struct {
  uint8_t  mrcps;  /**< MRCPS bits (HUM Ch 59 p 3601). */
  uint8_t  mastat; /**< MASTAT bits (HUM Ch 59 p 3577). */
  uint8_t  mrezs;  /**< MREZS bits (HUM Ch 59 p 3565). */
  uint16_t mcmdr;  /**< MCMDR bits (HUM Ch 59 p 3589). */
  uint32_t mstatr; /**< MSTATR bits (HUM Ch 59 p 3578). */
} ra_flash_status_ext_t;
/* cppcheck-suppress-end [unusedStructMember] */

/**
 * @struct ra_flash_isr_event_t
 * @brief One IRQ event delivered to ``ra_flash_callback_t``.
 *
 * @details
 * The dispatcher records which source fired and (where applicable)
 * the captured fault address from MRCRTEA / MRCRDEA / MRCPEA.
 */
/* cppcheck-suppress-begin [unusedStructMember] */
typedef struct {
  ra_flash_irq_src_t src;         /**< Which source fired. */
  uint32_t           fault_addr;  /**< MRCRTEA / MRCRDEA / MRCPEA. */
  uint32_t           status_word; /**< Snapshot of the source reg. */
  void*              user_ctx;    /**< Caller-provided context. */
} ra_flash_isr_event_t;
/* cppcheck-suppress-end [unusedStructMember] */

/**
 * @brief Callback signature for the unified flash IRQ dispatcher.
 *
 * @param[in] ev Event descriptor (source, fault address, status).
 *
 * @pre Caller registered this callback via ``ra_flash_callback_set``.
 * @post Callback returns; dispatcher continues with the next pending
 * source (if any) or returns to the caller of
 * ``ra_flash_dispatch_isr``.
 *
 * @note Runs in IRQ context: keep it short, do not block.
 */
typedef void (*ra_flash_callback_t)(const ra_flash_isr_event_t* ev);

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
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Controller initialised, in read mode.
 * @retval k_ra_err_null_ptr ``cfg`` was NULL.
 * @retval k_ra_err_invalid_arg ``cfg->mrcfreq_mhz`` > 0x0FA or
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
 * @warning Calling this function while another bus master is
 * actively reading MRAM may produce one wait-state of
 * read corruption -- do it during boot only.
 *
 * @see ra_flash_deinit
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_flash_init(const ra_flash_cfg_t* cfg);

/**
 * @brief Deinitialise: lock all program gates and re-enable prefetch.
 *
 * @details
 * Inverse of ``ra_flash_init``: the controller is left in the safest
 * possible state -- prefetch on, both ``MRCPC*`` registers locked,
 * high-speed program disabled. Status sticky bits are cleared. Also
 * exits P/E mode if the controller is currently in it.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Always (no failure path).
 *
 * @pre No write/erase operation in progress (caller waits).
 * @post Controller is in pure read mode.
 * @post All program-status error bits are cleared.
 *
 * @note Thread-safe: no.
 * @see ra_flash_init
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_flash_deinit(void);

/**
 * @brief Snapshot the program-status register.
 *
 * @details
 * Wraps ``MRCPS`` (HUM Ch 59 register layout p 3601). The returned
 * value is a copy of the 8-bit register; callers should test against
 * ``k_ra_mrcps_mask_*`` from ``ra8d2_flash_regs.h``.
 *
 * @param[out] out_status Non-NULL destination for the status byte.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Status copied.
 * @retval k_ra_err_null_ptr ``out_status`` was NULL.
 *
 * @pre ``out_status`` non-null.
 * @pre MRAM controller is powered (always true after reset).
 * @post ``*out_status`` reflects the last-read value of ``MRCPS``.
 * @post No state change in the controller.
 *
 * @note Thread-safe: read-only, single-register access.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_flash_get_status(uint8_t* out_status);

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
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Status snapshot copied.
 * @retval k_ra_err_null_ptr ``out`` was NULL.
 *
 * @pre ``out`` non-null.
 * @pre Controller is powered (always true after reset).
 * @post ``*out`` reflects the registers at the moment of the call.
 *
 * @note Thread-safe: pure reads; not atomic across registers.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_flash_get_extended_status(ra_flash_status_ext_t* out);

/**
 * @brief Clear sticky program-error bits in ``MRCPS``.
 *
 * @details
 * Writes the W1C mask to ``MRCPS`` so that ``PRGERRC`` and
 * ``ECCERRC`` are cleared. The flow-control bits (busy / buffer
 * empty / buffer full) are not affected -- they are read-only.
 *
 * @param[in] mask Bits to clear; typically
 * ``k_ra_mrcps_mask_errors``.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Bits cleared (or no-op if mask=0).
 * @retval k_ra_err_invalid_arg ``mask`` had bits set outside the
 * clearable region.
 *
 * @pre ``mask`` & ``~k_ra_mrcps_mask_errors`` == 0.
 * @pre No program operation in progress.
 * @post Bits identified by ``mask`` are 0 in ``MRCPS``.
 * @post Other ``MRCPS`` bits unchanged.
 *
 * @note Thread-safe: no.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_flash_clear_status(uint8_t mask);

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
 * ``k_ra_err_invalid_arg`` -- the caller must split such writes
 * into per-page calls. This matches the FSP ``mram_write_data``
 * loop which works one page at a time.
 *
 * @param[in] mram_addr Destination address inside the MRAM window
 * (``k_ra_flash_code_start``..
 * ``k_ra_flash_code_start + k_ra_flash_code_size``).
 * @param[in] src Non-NULL source buffer of at least ``len``
 * bytes.
 * @param[in] len Number of bytes to write, 1..32.
 * @param[in] world ``k_ra_flash_world_ns`` for the non-secure
 * half, ``k_ra_flash_world_s`` for the secure
 * half.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Write completed and committed.
 * @retval k_ra_err_null_ptr ``src`` was NULL.
 * @retval k_ra_err_invalid_arg ``len`` was 0 or > 32, ``mram_addr``
 * outside the MRAM window, or the write
 * spans a 32-byte page boundary.
 * @retval k_ra_err_hw_error Controller reported PRGERRC / ECCERRC
 * after the flush.
 *
 * @pre ``src`` non-null and ``len`` in [1, 32].
 * @pre ``mram_addr`` and ``mram_addr + len - 1`` are both inside the
 * MRAM window and the same 32-byte page.
 * @pre ``ra_flash_init`` has been called.
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
 * @see ra_flash_erase_block
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t
ra_flash_write_block(uint32_t mram_addr, const uint8_t* src, uint32_t len, ra_flash_world_t world);

/**
 * @brief Erase (= program to all 0xFF) one 32-byte MRAM block.
 *
 * @details
 * MRAM does not have a distinct erase command -- the natural
 * "erased" state is all-ones, and "erase" is implemented as a
 * page-aligned write of 32 ``0xFF`` bytes. This wraps
 * ``ra_flash_write_block`` with that fixed payload to keep callers
 * out of having to construct the buffer themselves.
 *
 * @param[in] mram_addr 32-byte aligned destination inside the MRAM
 * window.
 * @param[in] world Same semantics as ``ra_flash_write_block``.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Block erased.
 * @retval k_ra_err_invalid_arg ``mram_addr`` not 32-byte aligned or
 * outside the MRAM window.
 * @retval k_ra_err_hw_error Controller reported a program error.
 *
 * @pre ``mram_addr`` is 32-byte aligned.
 * @pre ``mram_addr + 32`` <= ``k_ra_flash_code_start +
 * k_ra_flash_code_size``.
 * @post Block reads as all 0xFF.
 * @post Program-control gate left locked.
 *
 * @note Thread-safe: no.
 * @warning Same brick warning as ``ra_flash_write_block``.
 *
 * @see ra_flash_write_block
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_flash_erase_block(uint32_t mram_addr, ra_flash_world_t world);

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
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Always.
 *
 * @pre None (the register is always accessible).
 * @post ``MRCPFB.MPFBEN`` matches the inverse of ``disable``.
 *
 * @note Thread-safe: no.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_flash_set_rww_disable(bool disable);

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
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Bit applied.
 * @retval k_ra_err_invalid_arg Cannot pass permanent + unlock.
 *
 * @pre Caller has run ``ra_flash_init``.
 * @pre Caller understands that ``permanent=true`` is irreversible.
 * @post On success, MRCBPROTx reflects the requested state.
 * @post Other MRAM controller registers untouched.
 *
 * @note Thread-safe: no.
 * @warning ``permanent=true`` is irreversible. Re-flashing the part
 * will not clear the fuse.
 *
 * @see ra_flash_write_block
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t
ra_flash_block_protect_set(ra_flash_world_t world, bool lock, bool permanent);

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
 * @param[in] target ``k_ra_flash_startup_default`` or ``_alternate``.
 * @param[in] temporary true => MSUACR-only; false => BTFLG persistent.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Swap applied.
 * @retval k_ra_err_invalid_arg ``target`` out of range.
 * @retval k_ra_err_hw_error Controller reported MSTATR error.
 * @retval k_ra_err_hw_timeout MACI did not return MRDY in time.
 *
 * @pre ``target`` is a valid ``ra_flash_startup_t`` value.
 * @pre Permanent boot-swap protection (FSPR) is not set.
 * @post On success, the next reset (or this reset, if temporary)
 * boots from the requested half.
 * @post Controller is back in read mode.
 *
 * @note Thread-safe: no.
 * @warning A failed configuration-set leaves BTFLG in an indeterminate
 * state. Reflash via SWD if the device cannot boot.
 *
 * @see ra_flash_get_startup_area
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_flash_set_startup_area(ra_flash_startup_t target, bool temporary);

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
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Status copied.
 * @retval k_ra_err_null_ptr Either output pointer was NULL.
 *
 * @pre Both output pointers non-null.
 * @pre Controller is powered (always true after reset).
 * @post ``*out_btflg`` and ``*out_fspr`` populated from MSUASMON.
 *
 * @note Thread-safe: read-only.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_flash_get_startup_area(uint8_t* out_btflg, uint8_t* out_fspr);

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
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Counter incremented.
 * @retval k_ra_err_invalid_arg ``counter`` out of range.
 * @retval k_ra_err_out_of_range Counter already at its max value.
 * @retval k_ra_err_hw_error MSTATR reported an error after the cmd.
 * @retval k_ra_err_hw_timeout MACI never returned MRDY.
 *
 * @pre ``counter`` < ``k_ra_flash_arc_count``.
 * @pre ``ra_flash_init`` has been called.
 * @post On success, the counter advances by exactly 1.
 * @post Controller is back in read mode.
 *
 * @note Thread-safe: no.
 * @warning Counter increments are non-volatile and irreversible.
 *
 * @see ra_flash_arc_read
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_flash_arc_increment(ra_flash_arc_id_t counter);

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
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Counter read.
 * @retval k_ra_err_null_ptr ``out_count`` was NULL.
 * @retval k_ra_err_invalid_arg ``counter`` out of range.
 * @retval k_ra_err_hw_timeout MACI never returned MRDY (OEMBL only).
 *
 * @pre ``counter`` < ``k_ra_flash_arc_count`` and ``out_count`` non-null.
 * @pre ``ra_flash_init`` has been called.
 * @post On success, ``*out_count`` holds the population count.
 * @post Controller back in read mode (OEMBL path).
 *
 * @note Thread-safe: no.
 * @see ra_flash_arc_increment
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_flash_arc_read(ra_flash_arc_id_t counter, uint32_t* out_count);

/**
 * @brief Issue the configuration-set MACI command.
 *
 * @details
 * Low-level escape hatch for callers that need to write the OFS
 * region (HUM Ch 7.2.x p 280..299) without going through the
 * ``ra_flash_set_startup_area`` wrapper. Writes 8 halfwords through
 * the MACI command-issuing area at ``0x4012_0000`` and waits for
 * MSTATR.MRDY.
 *
 * @param[in] target_addr OFS-region address to write (HUM Ch 7).
 * @param[in] words Pointer to 8 halfwords.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Write completed.
 * @retval k_ra_err_null_ptr ``words`` was NULL.
 * @retval k_ra_err_invalid_arg ``target_addr`` outside the OFS window.
 * @retval k_ra_err_hw_error MSTATR reported an error.
 * @retval k_ra_err_hw_timeout MACI never returned MRDY.
 *
 * @pre ``words`` non-null and points to 8 valid halfwords.
 * @pre ``target_addr`` lies inside the OFS window
 *      ``[k_ra_flash_ofs_start, k_ra_flash_ofs_start + k_ra_flash_ofs_size)``
 *      (HUM Ch 7 "Option-Setting Memory" p 278).
 * @post On success, the OFS region holds the new values.
 * @post Controller back in read mode.
 *
 * @note Thread-safe: no.
 * @warning OFS overwrites are persistent and may brick the part.
 *
 * @see ra_flash_set_startup_area
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_flash_config_set_write(uint32_t target_addr, const uint16_t* words);

/**
 * @brief Trigger the W-HUK zeroize via ``MREZC``.
 *
 * @details
 * Permanently destroys the wrapped HUK (HUM Ch 59 p 3565). The driver
 * writes the keyed value ``0x5501`` to MREZC and waits for
 * MREZS.WHUKEXE to fall.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Zeroization complete.
 * @retval k_ra_err_hw_timeout WHUKEXE never cleared.
 *
 * @pre ``ra_flash_init`` has been called.
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
[[nodiscard]] ra_err_t ra_flash_zeroize_huk(void);

/**
 * @brief Issue the MACI ``forced stop`` command.
 *
 * @details
 * Aborts any in-flight MACI sequence (HUM Ch 59 p 3589 + FSP
 * ``mram_stop``). After the call, the controller is left in P/E mode
 * but with the command queue idle. Callers usually pair this with
 * ``ra_flash_exit_pe_mode``.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Command accepted, MRDY observed.
 * @retval k_ra_err_hw_timeout MRDY never came back.
 * @retval k_ra_err_hw_error MASTAT.CMDLK still set.
 *
 * @pre Controller is powered.
 * @pre Caller is prepared for the in-flight operation to be aborted.
 * @post MACI command queue is empty.
 * @post MASTAT.CMDLK == 0 on success.
 *
 * @note Thread-safe: no.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_flash_force_stop(void);

/**
 * @brief Reset the MRAM peripheral and clear status.
 *
 * @details
 * Mirrors ``R_MRAM_Reset`` (FSP ``mram_reset``): enter P/E, issue
 * forced-stop, status-clear, exit to read. Clears every sticky error
 * flag the MRCPS, MRCRAES, MRERAES, MASTAT, MSTATR registers carry.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Controller reset.
 * @retval k_ra_err_hw_timeout MACI never returned MRDY.
 * @retval k_ra_err_hw_error MASTAT.CMDLK still set after the reset.
 *
 * @pre ``ra_flash_init`` has been called.
 * @post All sticky error bits cleared.
 * @post Controller back in read mode.
 *
 * @note Thread-safe: no.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_flash_reset(void);

/**
 * @brief Update MSAR (MRAM Security Attribution).
 *
 * @details
 * HUM Ch 59.5.13 p 3559. Each bit selects whether the matching
 * register subset is reachable from the secure (1) or non-secure (0)
 * world. ``new_msar`` is written verbatim; the caller is responsible
 * for understanding the per-bit semantics in
 * ``ra_msar_mask_t``.
 *
 * @param[in] new_msar Value to store.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Always.
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
[[nodiscard]] ra_err_t ra_flash_set_security_attribution(uint16_t new_msar);

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
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Enable bit applied.
 * @retval k_ra_err_invalid_arg ``src`` out of range.
 *
 * @pre ``src`` < ``k_ra_flash_irq_count``.
 * @pre ``ra_flash_init`` has been called.
 * @post Matching enable bit set / cleared.
 *
 * @note Thread-safe: no.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_flash_set_irq_enable(ra_flash_irq_src_t src, bool enable);

/**
 * @brief Register the unified IRQ callback.
 *
 * @details
 * The dispatcher (``ra_flash_dispatch_isr``) walks every documented
 * status register and calls this callback once per pending source.
 * Pass NULL to deregister.
 *
 * @param[in] cb Callback function or NULL.
 * @param[in] user_ctx Opaque pointer passed back via ``ev->user_ctx``.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Always.
 *
 * @pre None.
 * @post Subsequent ``ra_flash_dispatch_isr`` calls invoke ``cb``.
 * @post Pre-existing pending events are NOT replayed.
 *
 * @note Thread-safe: no -- caller must serialise vs ``dispatch_isr``.
 * @see ra_flash_dispatch_isr
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_flash_callback_set(ra_flash_callback_t cb, void* user_ctx);

/**
 * @brief Run the MRAM IRQ dispatcher (call from the BSP vector).
 *
 * @details
 * Walks MRCRAES (code-ECC), MRERAES (extra-ECC), MRCPS (program-err),
 * MASTAT (extra-err / cmdlk), MSTATR (extra-ready); for each pending
 * bit, builds a ``ra_flash_isr_event_t`` and calls the registered
 * callback. After the callback returns, the dispatcher clears the
 * matching status flag (W1C bits) so the next call sees only fresh
 * events.
 *
 * @return Number of events delivered.
 *
 * @pre ``ra_flash_callback_set`` registered a callback (otherwise a
 * ``no-op`` walk runs and 0 is returned).
 * @post Every W1C status bit observed at entry is cleared.
 * @post The registered callback was invoked once per pending source.
 *
 * @note Thread-safe: no, IRQ-context only.
 * @see ra_flash_callback_set
 * @since 0.1.0
 */
uint32_t ra_flash_dispatch_isr(void);

/**
 * @brief Toggle ``MRCEECC.ECCEN`` (program-side ECC encoder).
 *
 * @param[in] enable true => MRCEECC.ECCEN:= 1.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Always.
 *
 * @pre None.
 * @post MRCEECC.ECCEN matches ``enable``.
 *
 * @note Thread-safe: no.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_flash_set_ecc_encoder_enable(bool enable);

/**
 * @brief Toggle ``MRCDECC.DECECEN`` (read-side ECC decoder).
 *
 * @param[in] enable true => MRCDECC.DECECEN:= 1.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Always.
 *
 * @pre None.
 * @post MRCDECC.DECECEN matches ``enable``.
 *
 * @note Thread-safe: no.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_flash_set_ecc_decoder_enable(bool enable);

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
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Addresses copied.
 * @retval k_ra_err_null_ptr Any destination pointer was NULL.
 *
 * @pre All four output pointers non-null.
 * @pre Controller is powered.
 * @post All four ``*out_*`` locations updated.
 *
 * @note Thread-safe: read-only.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_flash_get_ecc_error_addr(uint32_t* out_code_ted,
                                                   uint32_t* out_code_dec,
                                                   uint32_t* out_extra_ted,
                                                   uint32_t* out_extra_dec);

/**
 * @brief Snapshot the program-error address (MRCPEA).
 *
 * @param[out] out_addr Non-NULL destination for MRCPEA.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Address copied.
 * @retval k_ra_err_null_ptr ``out_addr`` was NULL.
 *
 * @pre ``out_addr`` non-null.
 * @pre Controller is powered.
 * @post ``*out_addr`` populated from MRCPEA.
 *
 * @note Thread-safe: read-only.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_flash_get_program_error_addr(uint32_t* out_addr);

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
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Frequencies applied.
 * @retval k_ra_err_invalid_arg Either value out of range.
 *
 * @pre Both inputs in range.
 * @pre No write/erase operation in flight.
 * @post MRCFREQ/MREFREQ reflect the new values.
 * @post MRCPFB restored to its prior state.
 *
 * @note Thread-safe: no.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_flash_update_clock_freq(uint16_t mrcfreq_mhz, uint8_t mrefreq_mhz);

/**
 * @brief Kick MSUINITR to re-load the OFS sequencer.
 *
 * @details
 * Writes the keyed value ``0xA501`` to MSUINITR (HUM Ch 59 p 3585) and
 * waits for the SUINIT bit to fall. Used after a configuration-set
 * write to make the controller pick up the new option-setting bytes
 * without requiring a chip reset.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Re-init complete.
 * @retval k_ra_err_hw_timeout SUINIT never cleared.
 *
 * @pre ``ra_flash_init`` has been called.
 * @post MSUINITR.SUINIT reads back 0.
 *
 * @note Thread-safe: no.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_flash_msuinitr_kick(void);

/**
 * @brief Trigger the MRAM update transfer (MCTRCNTR).
 *
 * @details
 * Selects an MCTRLSR list, writes the keyed start to MCTRCNTR (HUM
 * Ch 59 p 3580), and returns immediately. Use
 * ``ra_flash_get_update_status`` to poll for completion.
 *
 * @param[in] list_select Which list (0..15) to run.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Transfer kicked.
 * @retval k_ra_err_invalid_arg ``list_select`` > 15.
 *
 * @pre ``list_select`` <= 15.
 * @pre ``ra_flash_init`` has been called.
 * @post MCTRSTATR.BUSY likely 1 immediately after the call.
 *
 * @note Thread-safe: no.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_flash_set_update_transfer(uint8_t list_select);

/**
 * @brief Poll the MRAM update-transfer status.
 *
 * @param[out] out_busy Non-NULL destination for MCTRSTATR.BUSY.
 * @param[out] out_done Non-NULL destination for MCTRSTATR.DONE.
 * @param[out] out_err Non-NULL destination for MCTRSTATR.ERR.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Status copied.
 * @retval k_ra_err_null_ptr Any destination pointer was NULL.
 *
 * @pre Output pointers non-null.
 * @post All three ``*out_*`` locations updated.
 *
 * @note Thread-safe: read-only.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t
ra_flash_get_update_status(uint8_t* out_busy, uint8_t* out_done, uint8_t* out_err);

/**
 * @brief Program 1..32 contiguous bytes into extra-MRAM via MACI.
 *
 * @details
 * Extra-MRAM (data flash) lives at ``k_ra_flash_extra_start`` and is
 * programmed through the MACI sequencer rather than the direct STR
 * gate (HUM Ch 7 p 278..299 + HUM Ch 59 p 3550..3624). This API mirrors
 * ``ra_flash_write_block`` semantics: 1..32 bytes inside one page.
 *
 * @param[in] mram_addr Destination inside the extra-MRAM window.
 * @param[in] src Non-NULL source buffer of at least ``len`` bytes.
 * @param[in] len 1..32.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Bytes written.
 * @retval k_ra_err_null_ptr ``src`` was NULL.
 * @retval k_ra_err_invalid_arg Range / alignment violation.
 * @retval k_ra_err_hw_error MSTATR error after the command.
 * @retval k_ra_err_hw_timeout MACI never returned MRDY.
 *
 * @pre ``src`` non-null and ``len`` in [1, 32].
 * @pre ``mram_addr`` lies inside extra-MRAM and ``mram_addr+len-1``
 * lies on the same 32-byte page.
 * @pre ``ra_flash_init`` has been called.
 * @post Data committed; controller back in read mode.
 *
 * @note Thread-safe: no.
 * @warning Same brick warnings as ``ra_flash_write_block``.
 * @see ra_flash_extra_mram_erase
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t
ra_flash_extra_mram_write(uint32_t mram_addr, const uint8_t* src, uint32_t len);

/**
 * @brief Erase one 32-byte block of extra-MRAM via MACI.
 *
 * @details
 * Equivalent to ``ra_flash_extra_mram_write`` with a 32-byte payload of
 * 0xFF.
 *
 * @param[in] mram_addr 32-byte aligned destination inside the
 * extra-MRAM window.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Block erased.
 * @retval k_ra_err_invalid_arg Misaligned or out-of-range address.
 * @retval k_ra_err_hw_error MSTATR error after the command.
 * @retval k_ra_err_hw_timeout MACI never returned MRDY.
 *
 * @pre ``mram_addr`` is 32-byte aligned and inside the extra-MRAM
 * window.
 * @post Block reads as all 0xFF.
 *
 * @note Thread-safe: no.
 * @warning Brick warnings apply.
 * @see ra_flash_extra_mram_write
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_flash_extra_mram_erase(uint32_t mram_addr);

/**
 * @brief Enter MRAM P/E mode.
 *
 * @details
 * Writes ``MENTRYR:= 0xAA80`` and waits for MENTRYR.MENTRY to go to 1
 * (HUM Ch 59 p 3582). Exposed so callers can batch multiple MACI
 * commands without paying the per-command transition cost.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Entered P/E.
 * @retval k_ra_err_hw_timeout MENTRY never went to 1.
 *
 * @pre Controller is powered.
 * @post Controller is in P/E mode.
 *
 * @note Thread-safe: no.
 * @see ra_flash_exit_pe_mode
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_flash_enter_pe_mode(void);

/**
 * @brief Exit MRAM P/E mode (return to read mode).
 *
 * @details
 * Writes ``MENTRYR:= 0xAA00`` and waits for the register to fall to
 * zero. Restores the prefetch buffer to its previous state.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Returned to read mode.
 * @retval k_ra_err_hw_timeout MENTRYR never went to 0.
 *
 * @pre Controller was in P/E mode (no harm if it wasn't -- the write
 * is idempotent).
 * @post Controller back in read mode.
 *
 * @note Thread-safe: no.
 * @see ra_flash_enter_pe_mode
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_flash_exit_pe_mode(void);

#ifdef __cplusplus
}
#endif
