/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file emu_seams.h
 * @brief Armv8.1-M instruction-emulation seams (M85 ops on Unicorn's M33)
 *
 * @details
 * The RA8D2's Cortex-M85 executes Armv8.1-M instructions that Unicorn's
 * nearest core (Cortex-M33, Armv8-M) either traps as invalid or silently
 * mis-executes. Each seam closes one gap:
 *
 *  - MVE (Helium): VMOV.I32 traps invalid and is emulated from the
 *    invalid-instruction hook; the contiguous load/store family reuses the
 *    legacy coprocessor encodings, so the M33 raises a NoCP UsageFault
 *    instead of trapping and those are serviced from the interrupt hook.
 *  - Long shifts (LSLL/LSRL/ASRL): overlap ORR.W and mis-execute silently;
 *    likewise found by image scan and emulated per site.
 *  - Divide-by-zero trap (CCR.DIV_0_TRP): Unicorn never raises the
 *    UsageFault; every UDIV/SDIV site is tracked and, once the firmware
 *    opts in, overwritten with UDF so the divide traps through the
 *    invalid-instruction hook and is either faulted (zero divisor) or
 *    emulated in software.
 *  - The invalid-instruction dispatcher itself (conditional-select family,
 *    barriers on old Unicorn builds, security register scrubs, and the
 *    low-overhead-branch DLS/LE loops).
 *
 * Split out of the ra8_emulator main translation unit; behaviour unchanged.
 *
 *
 * @since 0.1.0
 */

#pragma once

#include <stdint.h>
#include <unicorn/unicorn.h>

#include "ra8_attributes.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Emulate a run of consecutive auto-vectoriser MVE instructions.
 *
 * @details
 * Helium ops come in tight back-to-back runs (e.g. one VMOV.I32 then several
 * VSTRW.32 to zero a struct). Emulating only the first and relaunching would
 * land the next launch on another invalid instruction, which Unicorn faults
 * on -- so this consumes every consecutive handled MVE op in one trap and
 * sets PC to the first instruction it does NOT handle, exactly mirroring the
 * cond-select seam's "stop, then relaunch on valid code" contract. Bounded.
 *
 * @param[in,out] uc    Unicorn engine.
 * @param[in]     pc0   Address of the trapped instruction.
 * @param[in]     code0 The 4 instruction bytes already read at @p pc0.
 * @return true iff at least one MVE instruction was emulated.
 * @retval false @p code0 is not a handled MVE form (state untouched).
 * @pre @p code0 holds the 4 bytes at @p pc0.
 * @pre The invalid-instruction hook trapped at @p pc0.
 * @post On true, PC points at the first unhandled instruction.
 * @post On false, no engine state changed.
 * @note Not thread-safe; the emulator is single-threaded host-side.
 * @see emu_mve_nocp_emulate()  Handles the MVE loads/stores that raise NoCP.
 * @since 0.1.0
 */
RA8_PRIV bool emulate_mve(uc_engine* uc, uint32_t pc0, const uint8_t code0[4]);

/**
 * @brief Emulate an MVE contiguous load/store from the NoCP UsageFault.
 *
 * @details
 * Armv8.1-M reallocates coprocessor space 0b1110 / 0b1111 to MVE, so
 * VLDRB/VLDRH/VLDRW and VSTRB/VSTRH/VSTRW (immediate offset) reuse the legacy
 * STC/LDC encodings byte for byte -- `stc p15, c7, [r0, #196]` and
 * `vstrw.32 q3, [r0, #196]` are both ED80 7F31. Unicorn's M33 implements
 * neither MVE nor coprocessor 14/15, so it does not trap them as invalid
 * instructions: it raises a NoCP UsageFault with PC still at the faulting
 * word. This decodes that word straight from its two halfwords (capstone
 * renders the family as a legacy `stc`, so it cannot be used), moves the
 * 16-byte vector, applies write-back, and advances PC past the instruction.
 * Anything outside the family -- including the neighbouring FP stores, which
 * differ only in hw2[12:9] and which Unicorn executes correctly -- is
 * rejected so it still faults honestly.
 *
 * @param[in,out] uc Unicorn engine.
 * @param[in]     pc Address of the faulting instruction.
 * @return true iff an MVE contiguous load/store was decoded and performed.
 * @retval true  The access happened and PC advanced by four bytes.
 * @retval false Not this family; no engine state changed.
 * @pre @p uc is stopped inside the UC_HOOK_INTR NoCP fault.
 * @pre @p pc is the faulting instruction address, not an EXC_RETURN magic.
 * @post On true, PC points at the next instruction.
 * @post On false, no engine or memory state changed.
 * @note Not thread-safe; the emulator is single-threaded host-side.
 * @see emulate_mve()  Handles the MVE forms that do trap as invalid.
 * @since 0.1.0
 */
RA8_PRIV bool emu_mve_nocp_emulate(uc_engine* uc, uint32_t pc);

/**
 * @brief Test and clear the "NoCP fault serviced by the MVE seam" latch.
 *
 * @details
 * `uc_emu_start` returns UC_ERR_EXCEPTION for a NoCP UsageFault even when the
 * interrupt hook handled it completely, so the run loop cannot tell a serviced
 * MVE access from a genuine unhandled exception by status alone. The seam sets
 * this latch on every access it performs; the run loop consumes it at the chunk
 * boundary and relaunches rather than ending the run as a fault.
 *
 * @return true iff a NoCP fault was serviced since the last call.
 * @retval true  Relaunch from the advanced PC; the access already happened.
 * @retval false Nothing was serviced; treat the chunk status at face value.
 * @pre Called once per chunk boundary, from the run loop only.
 * @pre The engine is stopped at a chunk boundary.
 * @post The latch is clear on return.
 * @note Not thread-safe; the emulator is single-threaded host-side.
 * @see emu_mve_nocp_emulate()  Sets the latch.
 * @since 0.1.0
 */
RA8_PRIV bool emu_mve_nocp_take(void);

/**
 * @brief Report whether an invalid-instruction trap at @p pc is the bogus one
 *        Unicorn raises just after the MVE seam serviced a NoCP fault.
 *
 * @details
 * Servicing the NoCP fault means writing PC and calling `uc_emu_stop` from
 * inside the interrupt hook. Unicorn then re-decodes at the advanced PC and
 * reports UC_ERR_INSN_INVALID there even though the instruction is perfectly
 * valid -- in `ereader_cover` the address is a plain `strd r3, r3, [r7, #8]`
 * four bytes past a `vstrw.32`. A relaunch from that same PC executes it
 * correctly, so the report is noise, but it is indistinguishable from a real
 * one by decode alone: this predicate distinguishes it by address instead.
 *
 * Exactly one report is absorbed per serviced fault, and only at the address
 * the seam advanced to. A genuinely invalid instruction sitting at that
 * address is therefore delayed by one relaunch, never suppressed: the arming
 * is cleared on the first call, so the second trap reports normally.
 *
 * @param[in] pc Address the invalid-instruction hook trapped at.
 * @return true iff this trap is the expected post-NoCP artefact.
 * @retval true  Absorb it silently and relaunch; nothing is wrong.
 * @retval false Report it; the arming (if any) is now cleared.
 * @pre Called only after every real seam in the dispatch chain declined.
 * @pre Called at most once per invalid-instruction trap.
 * @post The one-shot arming is clear on return.
 * @note Not thread-safe; the emulator is single-threaded host-side.
 * @see emu_mve_nocp_emulate()  Arms this.
 * @since 0.1.0
 */
RA8_PRIV bool emu_mve_nocp_spurious(uint32_t pc);

/**
 * @brief Count of MVE instructions emulated this run (run-end telemetry).
 *
 * @return Emulated MVE instruction count.
 * @retval 0 No MVE instruction was emulated (e.g. an M33 image).
 * @pre None.
 * @pre None.
 * @post No state is modified.
 * @note Not thread-safe; the emulator is single-threaded host-side.
 * @since 0.1.0
 */
RA8_PRIV uint64_t emu_mve_emulated_count(void);

/**
 * @brief Scan the loaded image and install a hook at every immediate long-shift.
 *
 * @details
 * Walks the ELF32 PT_LOAD program headers, and for each executable segment
 * scans its bytes on 2-byte boundaries for the LSLL/LSRL/ASRL encoding. A
 * targeted UC_HOOK_CODE is installed at each site's VMA so the shift is
 * applied on the host -- where 64-bit arithmetic is correct -- before the
 * core silently mis-executes the overlapping ORR.W encoding. Matches use the
 * segment's VMA (p_vaddr), so a `.sram_text` ramfunc region is hooked at its
 * execution address even though it is not yet copied at install time. A scan
 * false-positive is harmless: the core never starts execution there, so the
 * hook never fires. Zero hooks -- hence zero steady-state cost -- for
 * firmware that contains no long shifts.
 *
 * @param[in,out] uc  Unicorn engine to install the hooks on.
 * @param[in]     elf In-memory ELF image (still alive at call time).
 * @param[in]     len Length of @p elf in bytes.
 * @return Nothing.
 * @pre @p elf is a 32-bit ARM ELF (already validated by load_elf).
 * @pre The M85 profile is selected (the caller gates on the primary core).
 * @post One UC_HOOK_CODE per long-shift site is armed (up to the site cap).
 * @post One stderr summary line is printed when any site was hooked.
 * @note Not thread-safe; call once during setup before the run loop.
 * @see div0_seam_install()  The companion image-scan seam.
 * @since 0.1.0
 */
RA8_PRIV void long_shift_seam_install(uc_engine* uc, const uint8_t* elf, long len);

/**
 * @brief Emulate a register-form Armv8.1-M long shift (LSLL/ASRL) that trapped.
 *
 * @details
 * The register form (\`lsll r0, r1, ip\`) aliases to an ORR.W whose Rm field is
 * SP, which the core refuses outright, so unlike the immediate form it arrives
 * here as a genuine undefined-instruction trap rather than silently
 * mis-executing. Decodes the site, applies the shift to the {RdaHi:RdaLo} pair
 * with correct 64-bit host arithmetic -- the amount is the SIGNED low byte of
 * Rm, so a negative value shifts the other way -- writes the pair back and
 * advances PC past the 4-byte instruction. Flags are untouched because the
 * aliased ORRS never executed.
 *
 * @param[in,out] uc   Unicorn engine.
 * @param[in]     pc   Address of the trapping instruction.
 * @param[in]     code The 4 instruction bytes at @p pc.
 * @return true when @p code was a register-form long shift and was emulated.
 * @retval true  Registers written and PC advanced; caller should stop+relaunch.
 * @retval false Not a register-form long shift; try the next handler.
 * @pre @p code holds the 4 bytes the core failed to decode at @p pc.
 * @pre @p uc is stopped inside the invalid-instruction hook.
 * @post On true, PC is @p pc + 4 and the register pair holds the result.
 * @post On false, no engine state is modified.
 * @note Not thread-safe; called from the single-threaded run loop.
 * @see long_shift_seam_install()  Handles the immediate form, which never traps.
 * @since 0.1.0
 */
RA8_PRIV bool emulate_long_shift_reg(uc_engine* uc, uint32_t pc, const uint8_t code[4]);

/** @brief UDIV/SDIV decode masks + fault field bits for the div-0 seam. */
typedef enum : uint32_t {
  k_div0_hw1_mask     = 0xFFF0U,     /**< hw1[15:4] selects the divide opcode.    */
  k_div0_hw1_udiv     = 0xFBB0U,     /**< UDIV T1: hw1[15:4] == 0xFBB.            */
  k_div0_hw1_sdiv     = 0xFB90U,     /**< SDIV T1: hw1[15:4] == 0xFB9.            */
  k_div0_hw2_mask     = 0xF0F0U,     /**< hw2[15:12] and hw2[7:4] must be 1111.   */
  k_div0_hw2_fixed    = 0xF0F0U,     /**< Their required value for a real divide. */
  k_div0_reg_mask     = 0x000FU,     /**< 4-bit register field (Rn / Rm / Rd).    */
  k_div0_rd_shift     = 8U,          /**< hw2[11:8] = Rd (destination register).  */
  k_div0_reg_sp       = 13U,         /**< r13 (SP): UNPREDICTABLE as UDIV d/n/m.  */
  k_div0_reg_pc       = 15U,         /**< r15 (PC): UNPREDICTABLE as UDIV d/n/m.  */
  k_div0_insn_len     = 4U,          /**< UDIV/SDIV are 32-bit Thumb-2.           */
  k_div0_cfsr_divzero = 1U << 25U,   /**< CFSR.UFSR.DIVBYZERO (0x02000000).       */
  k_div0_int32_min    = 0x80000000U, /**< INT32_MIN: the SDIV / -1 overflow edge. */
  k_div0_udf_b0       = 0xF0U,       /**< UDF.W #0 little-endian byte 0.          */
  k_div0_udf_b1       = 0xF7U,       /**< UDF.W #0 little-endian byte 1.          */
  k_div0_udf_b3       = 0xA0U,       /**< UDF.W #0 little-endian byte 3.          */
} div0_field_t;

/**
 * @brief Scan the image for UDIV/SDIV sites so the div-0 trap can arm later.
 *
 * @details
 * Walks the ELF32 PT_LOAD executable segments on 2-byte boundaries for the
 * UDIV/SDIV encoding, recording each site's VMA (p_vaddr based, so a ramfunc
 * is tracked at its execution address) and its original halfwords. Nothing is
 * patched here; the SCB control-write watcher overwrites the sites with UDF
 * via div0_patch_sites() only if the firmware sets CCR.DIV_0_TRP. Tracked for
 * every core (UDIV/SDIV exist on the M85 and the M33 alike). A scan
 * false-positive is harmless: the site is only ever patched after opt-in, and
 * emulate_div0_patched() re-decodes before acting.
 *
 * @param[in] elf In-memory ELF image (still alive at call time).
 * @param[in] len Length of @p elf in bytes.
 * @return Nothing.
 * @pre @p elf is a 32-bit ARM ELF (already validated by load_elf).
 * @pre @p len is the true byte length of @p elf.
 * @post Up to the site cap of divide sites are tracked, none patched yet.
 * @post The armed flag is cleared (a fresh scan starts un-armed).
 * @note Not thread-safe; call once during setup before the run loop.
 * @see div0_patch_sites()  Arms the tracked sites on firmware opt-in.
 * @since 0.1.0
 */
RA8_PRIV void div0_seam_install(const uint8_t* elf, long len);

/**
 * @brief Overwrite every tracked divide with UDF so divide-by-zero can trap.
 *
 * @details
 * Called from the SCB control-register write hook the first time the firmware
 * sets CCR.DIV_0_TRP. The patch is deferred to opt-in so a firmware that
 * never arms the trap keeps its original divides (native, quotient-0
 * semantics) and pays nothing. Patching to UDF -- rather than installing a
 * UC_HOOK_CODE at each site -- is deliberate: a code hook disables Unicorn's
 * engine-wide block chaining and would roughly quarter the throughput of any
 * busy loop in a DIV_0_TRP firmware, whereas the already-armed
 * undefined-instruction hook has no such cost. Idempotent; re-applied after
 * a warm reboot re-loads the image (the reboot path calls emu_div0_disarm()).
 *
 * @param[in,out] uc Unicorn engine whose memory is patched.
 * @return Nothing.
 * @pre The tracked divide sites hold valid addresses.
 * @pre @p uc permits uc_mem_write to the code image.
 * @post Every tracked site holds the UDF encoding and the seam is armed.
 * @post A no-op when already armed.
 * @note Not thread-safe (single engine).
 * @since 0.1.0
 */
RA8_PRIV void div0_patch_sites(uc_engine* uc);

/**
 * @brief Service an undefined-instruction trap that landed on an armed divide.
 *
 * @details
 * Called from the invalid-instruction dispatcher. When @p pc is a patched
 * divide site this recovers the original encoding, reads the operands, and
 * either (a) latches the pending div-0 fault when the divisor is zero and
 * DIV_0_TRP is set -- so the run loop synthesises the decoded UsageFault with
 * @p pc stacked -- or (b) computes the quotient in software, writes Rd and
 * steps PC past the 4-byte instruction as a zero-time seam relaunch. A trap
 * at any other address is not ours.
 *
 * @param[in,out] uc   Unicorn engine.
 * @param[in]     pc   Address of the trapping instruction.
 * @param[in]     code The 4 bytes at @p pc (the UDF, unused -- the original
 *                     encoding comes from the tracked sites).
 * @return Whether the trap was an armed divide this handler serviced.
 * @retval true  @p pc was a patched divide; state (or the fault latch) was
 *               updated and the caller should stop + resume.
 * @retval false @p pc is not a patched divide; try the next handler.
 * @pre The tracked sites hold the armed divide addresses.
 * @pre The PPB CCR word is mapped as RAM.
 * @post On a trapping div-0 the pending fault is latched with @p pc; else Rd
 *       and PC are advanced.
 * @note Not thread-safe (single engine).
 * @since 0.1.0
 */
RA8_PRIV bool emulate_div0_patched(uc_engine* uc, uint32_t pc, const uint8_t code[4]);

/**
 * @brief Whether a trapping divide-by-zero is latched for the run loop.
 *
 * @return true while a div-0 UsageFault awaits synthesis.
 * @retval false No divide-by-zero is pending.
 * @pre None.
 * @pre None.
 * @post No state is modified.
 * @note Not thread-safe; the emulator is single-threaded host-side.
 * @since 0.1.0
 */
RA8_PRIV bool emu_div0_fault_pending(void);

/**
 * @brief Clear the latched divide-by-zero fault.
 *
 * @details The run loop clears the latch right before synthesising the
 * UsageFault; the warm-reboot path clears it so a rebooted image starts
 * clean.
 *
 * @return Nothing.
 * @pre A fault was latched (or the call is a harmless reset).
 * @pre None otherwise.
 * @post No divide-by-zero is pending.
 * @note Not thread-safe; the emulator is single-threaded host-side.
 * @since 0.1.0
 */
RA8_PRIV void emu_div0_clear_fault(void);

/**
 * @brief PC of the divide that latched the pending fault.
 *
 * @return The trapping divide's address (stacked by the synthesised fault).
 * @retval 0 No fault has ever been latched.
 * @pre A fault was latched this run (else the value is stale/zero).
 * @pre None otherwise.
 * @post No state is modified.
 * @note Not thread-safe; the emulator is single-threaded host-side.
 * @since 0.1.0
 */
RA8_PRIV uint32_t emu_div0_fault_pc(void);

/**
 * @brief Count one synthesised divide-by-zero UsageFault (telemetry).
 *
 * @return Nothing.
 * @pre A UsageFault is being synthesised for a latched div-0.
 * @pre None otherwise.
 * @post The run's div-0 trap count grew by one.
 * @note Not thread-safe; the emulator is single-threaded host-side.
 * @since 0.1.0
 */
RA8_PRIV void emu_div0_count_trap(void);

/**
 * @brief Drop the armed state after a warm reboot re-loads the image.
 *
 * @details Re-loading the ELF restores the original divide encodings, so the
 * seam must re-arm on the next CCR.DIV_0_TRP write; this clears the
 * idempotence latch.
 *
 * @return Nothing.
 * @pre A warm reboot just re-wrote the PT_LOAD segments.
 * @pre None otherwise.
 * @post The next div0_patch_sites() call patches again.
 * @note Not thread-safe; the emulator is single-threaded host-side.
 * @since 0.1.0
 */
RA8_PRIV void emu_div0_disarm(void);

/**
 * @brief UC_HOOK_INSN_INVALID dispatcher: service or report a trapped opcode.
 *
 * @details Tries each trap-path seam in a fixed order -- the armed div-0 UDF
 * service first (it matches only exact patched addresses), then the Armv8-M
 * security scrubs, the conditional-select family, the barrier NOPs, the MVE
 * run consumer and the low-overhead-branch loops. A handled seam edits PC and
 * stops the engine (the run loop relaunches on valid code); anything else is
 * disassembled and reported, then the engine stops with the invalid-opcode
 * error.
 *
 * @param[in,out] uc   Unicorn engine at the trapped instruction.
 * @param[in]     user Hook user pointer (unused; Unicorn ABI).
 * @return true when a seam handled the instruction, false to fault.
 * @retval true  PC advanced past the emulated instruction; engine stopped.
 * @retval false Genuinely invalid: report printed, emulation errors out.
 * @pre The engine trapped an instruction its core cannot decode.
 * @pre The seams were installed for this run.
 * @post On true, engine state reflects the emulated instruction.
 * @note Not thread-safe; the emulator is single-threaded host-side.
 * @see emu_insn_seams_install()  Arms this dispatcher.
 * @since 0.1.0
 */
RA8_PRIV bool on_invalid_insn(uc_engine* uc, void* user);

/**
 * @brief Arm the invalid-instruction dispatcher on the engine.
 *
 * @return Nothing.
 * @pre @p uc is initialised (no code has run yet).
 * @pre Called once during setup, before any other hook that relies on the
 *      dispatcher's seams.
 * @post The UC_HOOK_INSN_INVALID hook is installed for the whole run.
 * @note Not thread-safe; call once during single-threaded setup.
 * @since 0.1.0
 */
RA8_PRIV void emu_insn_seams_install(uc_engine* uc);

/**
 * @brief Count of LOB (DLS/LE) instructions emulated this run (telemetry).
 *
 * @return Emulated low-overhead-branch instruction count.
 * @retval 0 No LOB instruction was emulated.
 * @pre None.
 * @pre None.
 * @post No state is modified.
 * @note Not thread-safe; the emulator is single-threaded host-side.
 * @since 0.1.0
 */
RA8_PRIV uint64_t emu_lob_emulated_count(void);

/**
 * @brief Synthesise a UsageFault (#6) for a trapped divide-by-zero.
 *
 * @details
 * Called by the run loop after emulate_div0_patched() latched a trapping
 * div-0. Latches CFSR.UFSR.DIVBYZERO (so a fault handler -- and the HIL alive
 * probe -- see the architectural status), forces PC back to the faulting
 * divide so the exception entry stacks *that* address, and vectors into the
 * application's UsageFault_Handler. If no handler is installed the trap is
 * dropped (no HardFault escalation is modelled).
 *
 * @param[in,out] uc        Unicorn engine.
 * @param[in]     vtor_base Fallback vector base if VTOR reads as 0.
 * @return Nothing.
 * @pre A divide-by-zero fault is latched (PC captured at the divide).
 * @pre The PPB CFSR word and the vector table are mapped as RAM.
 * @post On a valid vector, the core is in the UsageFault handler with the
 *       basic frame stacked and IPSR == 6.
 * @note Faithful to Armv8-M CCR.DIV_0_TRP semantics; no time advances.
 * @since 0.1.0
 */
RA8_PRIV void div0_synth_usagefault(uc_engine* uc, uint32_t vtor_base);

/**
 * @brief Opt in to the --fast-sd block-serving seam for this run.
 *
 * @return Nothing.
 * @pre Called from the CLI parser before the seams are installed.
 * @pre None otherwise.
 * @post fast_sd_seam_install() will arm the block hook when possible.
 * @note Not thread-safe; single-threaded setup only.
 * @since 0.1.0
 */
RA8_PRIV void emu_fast_sd_enable(void);

/**
 * @brief Install the `--fast-sd` block-read hook if opted-in and the symbol exists.
 *
 * @param[in,out] uc  Active Unicorn engine.
 * @param[in]     elf Loaded ELF image (for symbol resolution).
 * @param[in]     len ELF image length in bytes.
 * @return Nothing.
 * @pre @p uc is initialised and @p elf holds @p len valid bytes.
 * @pre The SD card model is attached when the fast path should serve blocks.
 * @post With the opt-in and the symbol present, a UC_HOOK_CODE serves whole
 *       512-byte blocks at ra8_sdmmc_spi_read_block's entry; otherwise
 *       nothing is armed.
 * @note A firmware without the symbol (no SD path) is reported once and left
 *       on the default per-byte MMIO path.
 * @since 0.1.0
 */
RA8_PRIV void fast_sd_seam_install(uc_engine* uc, const uint8_t* elf, long len);

#ifdef __cplusplus
}
#endif
