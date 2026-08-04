/**
 * @file emu_prof.h
 * @brief Firmware profiler (RA8_EMU_PROFILE): sampling, hooks, reports
 *
 * @details
 * Two profiling modes, bucketed by ELF FUNC symbol:
 *
 *  - `RA8_EMU_PROFILE=1` -- wall-time sample: charge each emulation chunk's
 *    wall time to the function its execution started in. Cheap (no
 *    per-instruction cost); a flat percentage list of the dominant cost.
 *  - `RA8_EMU_PROFILE=full` (or `insn`) -- per-instruction: a code hook
 *    tallies every instruction + call entry AND reconstructs the live call
 *    chain, so the run end emits an Ozone-style breakdown: a boot timeline,
 *    an inclusive/self table, a speedscope JSON export, and a self-contained
 *    HTML flamechart. Accurate but ~10x slower; off by default.
 *
 * The run loop reads the mode / instruction totals / RA8_EMU_STOP_PC state
 * through the accessors here; everything else stays private to emu_prof.c.
 *
 * Split out of the ra8_emulator main translation unit; behaviour unchanged.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
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
 * @enum prof_mode_t
 * @brief Profiler mode parsed from RA8_EMU_PROFILE.
 *
 * @details Selected once by prof_load() from the environment; the run loop
 * branches its wall-sampling and idle-stop logic on the mode.
 *
 * @invariant The mode is fixed for the whole run after prof_load().
 * @see prof_load()  Parses the mode from the environment.
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_prof_off  = 0U, /**< Disabled (no env, zero cost).               */
  k_prof_wall = 1U, /**< =1: cheap chunk-start wall-time sampler.    */
  k_prof_insn = 2U, /**< =full/=insn: exact per-instruction + calls. */
} prof_mode_t;

/**
 * @brief Monotonic wall-clock seconds.
 *
 * @details CLOCK_MONOTONIC as a double, the time base of the wall-sample
 * profiler mode (and any other host-side wall measurement).
 *
 * @return Monotonic seconds since an arbitrary epoch.
 * @retval 0.0 Only if the clock read fails (it does not on supported hosts).
 * @pre None (safe at any time).
 * @pre The host provides CLOCK_MONOTONIC.
 * @post No state is modified.
 * @note Thread-safe (pure system-clock read).
 * @since 0.1.0
 */
RA8_PRIV double board_now_s(void);

/**
 * @brief Collect + sort FUNC symbols (RA8_EMU_PROFILE only) for PC bucketing.
 *
 * @details Reads RA8_EMU_PROFILE to pick the mode; when enabled, walks the
 * ELF .symtab for sized FUNC symbols, records each one's [entry, end) range
 * and name, and sorts by entry address so per-PC lookup can binary-search.
 * With the variable unset the profiler stays off at zero cost.
 *
 * @param[in] elf In-memory ELF image (name pointers alias into it).
 * @param[in] len Length of @p elf in bytes.
 * @return Nothing.
 * @pre @p elf is a validated ELF32 image of @p len bytes.
 * @pre The image buffer outlives the run (names are borrowed, not copied).
 * @post The symbol table is loaded and the mode is latched.
 * @post One `[profile]` stderr line is printed when profiling is enabled.
 * @note Not thread-safe; call once during setup.
 * @see emu_prof_install()  Arms the per-instruction hook afterwards.
 * @since 0.1.0
 */
RA8_PRIV void prof_load(const uint8_t* elf, long len);

/**
 * @brief Attribute wall seconds to the function owning @p pc (wall mode).
 *
 * @details A no-op outside wall-sample mode. The run loop calls this once per
 * chunk with the previous chunk's start PC and elapsed wall time.
 *
 * @param[in] pc Chunk-start program counter (bucketing key).
 * @param[in] dt Elapsed wall seconds to charge.
 * @return Nothing.
 * @pre prof_load() ran (the mode and symbol table are latched).
 * @pre @p dt is a non-negative wall-time delta.
 * @post In wall mode the owning function's total (and the run total) grew by
 *       @p dt; otherwise nothing changed.
 * @note Not thread-safe; the run loop is single-threaded.
 * @since 0.1.0
 */
RA8_PRIV void prof_add(uint32_t pc, double dt);

/**
 * @brief Print the top hot functions (by wall time or instruction count).
 *
 * @details The run-end report. In per-instruction mode it additionally emits
 * the speedscope JSON export, the local HTML flamechart, the boot timeline,
 * and the inclusive/self table. A no-op when profiling was off or nothing
 * was sampled.
 *
 * @return Nothing.
 * @pre The run has ended (totals are final).
 * @pre stderr is writable.
 * @post The report (if any) has been written; per-symbol counters are
 *       consumed by the top-N selection.
 * @note Not thread-safe; call once at run end.
 * @since 0.1.0
 */
RA8_PRIV void prof_report(void);

/**
 * @brief Arm the per-instruction profiling hook (insn mode only).
 *
 * @details Installs the UC_HOOK_CODE that tallies every instruction, call
 * entry, and call-chain sample. A no-op in off/wall modes so those runs pay
 * nothing.
 *
 * @param[in,out] uc Unicorn engine to hook.
 * @return Nothing.
 * @pre prof_load() ran (the mode is latched).
 * @pre @p uc is initialised.
 * @post In insn mode the code hook is armed for the whole run.
 * @post In other modes @p uc is untouched.
 * @note Not thread-safe; call once during setup.
 * @since 0.1.0
 */
RA8_PRIV void emu_prof_install(uc_engine* uc);

/**
 * @brief The latched profiler mode.
 *
 * @return The mode RA8_EMU_PROFILE selected.
 * @retval k_prof_off Profiling disabled.
 * @retval k_prof_wall Wall-time sampling.
 * @retval k_prof_insn Per-instruction profiling.
 * @pre prof_load() ran.
 * @pre None otherwise.
 * @post No state is modified.
 * @note Not thread-safe; the emulator is single-threaded host-side.
 * @since 0.1.0
 */
RA8_PRIV prof_mode_t emu_prof_mode(void);

/**
 * @brief Total instructions retired under the per-instruction hook.
 *
 * @details The run loop's compute-idle auto-stop differences this counter
 * per chunk to detect the steady idle frame loop.
 *
 * @return Instructions counted so far (0 outside insn mode).
 * @retval 0 No instructions counted yet (or not in insn mode).
 * @pre None.
 * @pre None.
 * @post No state is modified.
 * @note Not thread-safe; the emulator is single-threaded host-side.
 * @since 0.1.0
 */
RA8_PRIV uint64_t emu_prof_total_insns(void);

/**
 * @brief Set the RA8_EMU_STOP_PC early-stop address (0 disables).
 *
 * @details The per-instruction hook ends the run the first time PC reaches
 * this address, letting a profile span exactly the boot path. The caller
 * clears the Thumb bit before passing.
 *
 * @param[in] pc Stop address (Thumb bit already cleared), or 0 to disable.
 * @return Nothing.
 * @pre The address (when non-zero) is a code address of the loaded image.
 * @pre Called during setup, before the run loop.
 * @post The stop address is latched for the run.
 * @note Not thread-safe; the emulator is single-threaded host-side.
 * @since 0.1.0
 */
RA8_PRIV void emu_prof_set_stop_pc(uint32_t pc);

/**
 * @brief Whether the RA8_EMU_STOP_PC address was reached.
 *
 * @return true once the per-instruction hook saw PC hit the stop address.
 * @retval false The stop PC is disabled or not yet reached.
 * @pre None.
 * @pre None.
 * @post No state is modified.
 * @note Not thread-safe; the emulator is single-threaded host-side.
 * @since 0.1.0
 */
RA8_PRIV bool emu_prof_stop_hit(void);

#ifdef __cplusplus
}
#endif
