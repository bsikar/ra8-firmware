/**
 * @file emu_trace.c
 * @brief Seam glue + --trace-sym implementation (see emu_trace.h)
 *
 * @details
 * The return-to-LR helper, the named-symbol entry hook installer, and the
 * `--trace-sym` logger -- moved verbatim out of the ra8_emulator main
 * translation unit.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include "emu_trace.h"

#include <stdio.h>

#include "emu_elf.h"

/* =============================================================================
 * Function-entry seam helpers -- emulate "return <r0> to LR" and hook one ELF
 * symbol's entry to a C callback. Shared by the USB host-mode and virtual-
 * keyboard seams below. The Ethernet frame seam that first introduced them was
 * retired once board_periph_eth modelled the R-Switch (ESWM/ETHA/RMAC/GWCA)
 * registers, so the genuine ra8_eth driver now runs the descriptor DMA path.
 * =============================================================================
 */

/** @brief Emulate "return r0;" from a hooked function: set R0, branch to LR. */
void eth_hook_return(uc_engine* uc, uint32_t r0)
{
  uint32_t lr = 0U;
  (void)uc_reg_read(uc, UC_ARM_REG_LR, &lr);
  (void)uc_reg_write(uc, UC_ARM_REG_R0, &r0);
  uint32_t pc = lr & ~1U; /* drop the Thumb bit; the M-class core stays Thumb. */
  (void)uc_reg_write(uc, UC_ARM_REG_PC, &pc);
  (void)uc_emu_stop(uc); /* relaunch from the returned PC (chunk contract). */
}

/** @brief Hook one symbol (if present) to @p cb; record it for the report. */
void eth_seam_hook(uc_engine* uc, const uint8_t* elf, long len, const char* name, void* cb)
{
  const uint32_t addr = elf_sym_addr(elf, len, name, nullptr);
  if (addr == 0U) {
    return;
  }
  enum : uint8_t {
    k_seam_hook_slots = 24U, /**< Max symbols the seam can hook across one run. */
  };
  static uc_hook  s_handles[k_seam_hook_slots];
  static uint32_t s_n;
  if (s_n < (uint32_t)(sizeof(s_handles) / sizeof(s_handles[0]))) {
    (void)uc_hook_add(uc, &s_handles[s_n], UC_HOOK_CODE, cb, nullptr, addr, addr);
    s_n++;
  }
}

/**
 * @brief Generic `--trace-sym` hook: log the first time control reaches a named
 *        symbol, plus the calling LR, so a stuck bring-up sequence is visible.
 *
 * @details Installed by ::sym_trace_install over each `--trace-sym <name>`. The
 * UC_HOOK_CODE fires on every execution of the symbol's entry address; we print
 * the entry address and the link register (return address) so a poll loop that
 * re-enters, or a thread that reaches step N but never N+1, is obvious in the
 * log without a full instruction trace.
 *
 * @param[in] uc      Active Unicorn engine (read for LR).
 * @param[in] address Entry address that fired the hook.
 * @param[in] size    Decoded instruction size (unused).
 * @param[in] user    The symbol name passed at install (stable argv pointer).
 *
 * @pre @p user names the hooked symbol.
 * @post One stderr line is emitted per hit.
 *
 * @note Not thread-safe; the run loop is single-threaded host-side.
 * @since 0.1.0
 */
/* cppcheck-suppress constParameterCallback ; UC_HOOK_CODE callback ABI is void*. */
static void on_sym_trace(uc_engine* uc, uint64_t address, uint32_t size, void* user)
{
  (void)size;
  uint32_t lr = 0U;
  (void)uc_reg_read(uc, UC_ARM_REG_LR, &lr);
  (void)fprintf(stderr,
                "  [symtrace] %s @ 0x%08X  (LR 0x%08X)\n",
                (const char*)user,
                (unsigned)address,
                lr);
}

/**
 * @brief Install a `--trace-sym` entry hook for every requested symbol present.
 *
 * @param[in,out] uc    Active Unicorn engine.
 * @param[in]     elf   Loaded ELF image (for symbol resolution).
 * @param[in]     len   ELF image length in bytes.
 * @param[in]     names Symbol names from the CLI (stable for the run).
 * @param[in]     count Number of names in @p names.
 *
 * @pre @p uc is initialised and @p elf holds @p len valid bytes.
 * @post A UC_HOOK_CODE fires ::on_sym_trace at each resolved symbol's entry.
 *
 * @note A name that does not resolve is reported once and skipped.
 * @since 0.1.0
 */
void sym_trace_install(uc_engine*         uc,
                       const uint8_t*     elf,
                       long               len,
                       const char* const* names,
                       uint32_t           count)
{
  static uc_hook s_th[k_trace_sym_max];
  for (uint32_t i = 0U; (i < count) && (i < (uint32_t)k_trace_sym_max); i++) {
    const uint32_t addr = elf_sym_addr(elf, len, names[i], nullptr);
    if (addr == 0U) {
      (void)fprintf(stderr, "  [symtrace] %s: symbol not found -- skipped\n", names[i]);
      continue;
    }
    (void)uc_hook_add(uc, &s_th[i], UC_HOOK_CODE, (void*)on_sym_trace, (void*)names[i], addr, addr);
    (void)fprintf(stderr, "  [symtrace] %s armed @ 0x%08X\n", names[i], (unsigned)addr);
  }
}
