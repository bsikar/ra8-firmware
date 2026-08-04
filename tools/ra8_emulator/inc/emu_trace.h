/**
 * @file emu_trace.h
 * @brief Function-entry seam glue + the --trace-sym instrument
 *
 * @details
 * The shared "hook one ELF symbol's entry to a C callback" helpers every
 * function-level seam builds on (emulate "return r0 to LR", install a
 * UC_HOOK_CODE at a named symbol), plus the `--trace-sym` debugging
 * instrument that logs each entry to a named function with its caller. The
 * Ethernet frame seam that first introduced the helpers was retired once
 * board_periph_eth modelled the R-Switch registers; the USB host-mode and
 * fast-SD seams still ride them.
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
 * @enum emu_trace_cfg_t
 * @brief Sizing for the --trace-sym instrument.
 *
 * @details Bounds both the CLI collection array and the installed hook
 * handles, so a run can trace at most this many functions.
 *
 * @invariant The CLI parser and the installer share this bound.
 * @see sym_trace_install()  Consumes up to this many names.
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_trace_sym_max = 16U, /**< Max --trace-sym functions per run. */
} emu_trace_cfg_t;

/**
 * @brief Emulate "return r0;" from a hooked function: set R0, branch to LR.
 *
 * @details Loads @p r0 into R0, moves PC to LR (Thumb bit dropped -- the
 * M-class core stays Thumb) and stops the engine so the chunked run loop
 * relaunches from the returned PC (editing PC and continuing in-place
 * corrupts Unicorn's block/Thumb state).
 *
 * @param[in,out] uc Unicorn engine stopped at a hooked function entry.
 * @param[in]     r0 The value the shadowed function "returns".
 * @return Nothing.
 * @pre @p uc sits at a function entry (LR holds the return address).
 * @pre The caller is a UC_HOOK_CODE seam about to skip the real body.
 * @post R0/PC are set and the engine is stopped for relaunch.
 * @note Not thread-safe; the emulator is single-threaded host-side.
 * @see eth_seam_hook()  Installs the seams that call this.
 * @since 0.1.0
 */
RA8_PRIV void eth_hook_return(uc_engine* uc, uint32_t r0);

/**
 * @brief Hook one symbol (if present) to @p cb; record it for the report.
 *
 * @details Resolves @p name from the ELF symbol table and installs a
 * UC_HOOK_CODE at its entry; a missing symbol is silently skipped so a seam
 * family can offer hooks for APIs the firmware may not link. Bounded by a
 * fixed handle pool shared across all callers.
 *
 * @param[in,out] uc   Unicorn engine.
 * @param[in]     elf  Loaded ELF image (symbol resolution).
 * @param[in]     len  ELF image length in bytes.
 * @param[in]     name Symbol to hook.
 * @param[in]     cb   UC_HOOK_CODE callback to fire at the entry.
 * @return Nothing.
 * @pre @p elf is resident and valid for symbol resolution.
 * @pre @p cb matches the UC_HOOK_CODE ABI.
 * @post On a resolved symbol one more entry hook is armed (pool permitting).
 * @note Not thread-safe; call during single-threaded setup.
 * @since 0.1.0
 */
RA8_PRIV void
eth_seam_hook(uc_engine* uc, const uint8_t* elf, long len, const char* name, void* cb);

/**
 * @brief Install a `--trace-sym` entry hook for every requested symbol present.
 *
 * @param[in,out] uc    Active Unicorn engine.
 * @param[in]     elf   Loaded ELF image (for symbol resolution).
 * @param[in]     len   ELF image length in bytes.
 * @param[in]     names Symbol names from the CLI (stable for the run).
 * @param[in]     count Number of names in @p names.
 * @return Nothing.
 * @pre @p uc is initialised and @p elf holds @p len valid bytes.
 * @pre @p names entries outlive the run (argv pointers).
 * @post A UC_HOOK_CODE logs each resolved symbol's entries (+LR).
 * @note A name that does not resolve is reported once and skipped.
 * @since 0.1.0
 */
RA8_PRIV void sym_trace_install(uc_engine*         uc,
                                const uint8_t*     elf,
                                long               len,
                                const char* const* names,
                                uint32_t           count);

#ifdef __cplusplus
}
#endif
