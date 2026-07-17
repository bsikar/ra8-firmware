/**
 * @file sim_seams.h
 * @brief Armv8.1-M instruction-emulation seams (M85 ops on Unicorn's M33)
 *
 * @details
 * The RA8D2's Cortex-M85 executes Armv8.1-M instructions that Unicorn's
 * nearest core (Cortex-M33, Armv8-M) either traps as invalid or silently
 * mis-executes. Each seam closes one gap:
 *
 *  - MVE (Helium): VMOV.I32 traps invalid and is emulated from the
 *    invalid-instruction hook; VSTRW.32 decodes as a *valid* legacy
 *    coprocessor store, so a one-time image scan hooks every site instead.
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
 * Split out of the board_sim main translation unit; behaviour unchanged.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
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
 * @see mve_seam_install()  Handles the VSTRW.32 sites the core would not trap.
 * @since 0.1.0
 */
RA8_PRIV bool emulate_mve(uc_engine* uc, uint32_t pc0, const uint8_t code0[4]);

/**
 * @brief Scan the loaded image for VSTRW.32 sites and hook each one.
 *
 * @details
 * Unlike VMOV.I32 (which traps as invalid), the MVE vector store decodes as a
 * valid legacy coprocessor store on the M33 core, so Unicorn would EXECUTE it
 * -- faulting on the absent coprocessor -- rather than trap it. This walks
 * the ELF32 executable PT_LOAD segments on 2-byte boundaries for the VSTRW.32
 * encoding and installs a targeted UC_HOOK_CODE at each site's VMA so the
 * store is performed by hand before the core reaches the bad instruction.
 * Scan false-positives are harmless (never executed, hook never fires).
 *
 * @param[in,out] uc  Unicorn engine to install the hooks on.
 * @param[in]     elf In-memory ELF image (still alive at call time).
 * @param[in]     len Length of @p elf in bytes.
 * @return Nothing.
 * @pre @p elf is a 32-bit ARM ELF (already validated by load_elf).
 * @pre The M85 profile is selected (the caller gates on the primary core).
 * @post One UC_HOOK_CODE per VSTRW.32 site is armed (up to the site cap).
 * @post One stderr summary line is printed when any site was hooked.
 * @note Not thread-safe; call once during setup before the run loop.
 * @see emulate_mve()  Handles the MVE forms that do trap.
 * @since 0.1.0
 */
RA8_PRIV void mve_seam_install(uc_engine* uc, const uint8_t* elf, long len);

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
RA8_PRIV uint64_t sim_mve_emulated_count(void);

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
 * @see mve_seam_install()  The companion image-scan seam.
 * @since 0.1.0
 */
RA8_PRIV void long_shift_seam_install(uc_engine* uc, const uint8_t* elf, long len);

#ifdef __cplusplus
}
#endif
