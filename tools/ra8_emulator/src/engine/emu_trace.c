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
#include "emu_host_io_internal.h"

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
void eth_seam_hook(uc_engine* uc, const emu_elf_source_t* elf, const char* name, void* cb)
{
  const uint32_t addr = elf_sym_addr(elf, name, nullptr);
  if (addr == 0U) {
    return;
  }
  enum : uint8_t {
    k_seam_hook_slots = 24U, /**< Max symbols the seam can hook across one run. */
  };
  static uc_hook  s_handles[k_seam_hook_slots];
  static uint32_t s_handle_n;
  if (s_handle_n < (uint32_t)(sizeof(s_handles) / sizeof(s_handles[0]))) {
    (void)uc_hook_add(uc, &s_handles[s_handle_n], UC_HOOK_CODE, cb, nullptr, addr, addr);
    s_handle_n++;
  }
}

/* cppcheck-suppress constParameterCallback ; UC_HOOK_CODE callback ABI is void*. */
/**
 * @brief Perform on sym trace for the emu trace model.
 * @details Perform on sym trace for the emu trace model; this step is contained within the emu trace model and uses bounded caller or module-owned storage.
 * @param[in,out] uc Unicorn engine whose emulated state is read or updated.
 * @param[in] address Guest address involved in the operation.
 * @param[in] size Size of the requested region or access in bytes.
 * @param[in,out] user Hook context supplied when the callback was registered.
 * @pre Arguments satisfy the ranges documented for on sym trace. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the emu trace model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static void
internal_on_sym_trace(uc_engine* uc, uint64_t address, uint32_t size, void* user)
{
  (void)size;
  uint32_t lr = 0U;
  (void)uc_reg_read(uc, UC_ARM_REG_LR, &lr);
  (void)priv_emu_io_errf("  [symtrace] %s @ 0x%08X  (LR 0x%08X)\n",
                         (const char*)user,
                         (unsigned)address,
                         lr);
}

/**
 * @brief Install a `--trace-sym` entry hook for every requested symbol present.
 *
 * @param[in,out] uc    Active Unicorn engine.
 * @param[in]     elf   Loaded ELF image (for symbol resolution).
 * @param[in]     names Symbol names from the CLI (stable for the run).
 * @param[in]     count Number of names in @p names.
 *
 * @pre @p uc is initialised and @p elf is a validated loaded image.
 * @post A UC_HOOK_CODE fires ::internal_on_sym_trace at each resolved symbol's entry.
 *
 * @note A name that does not resolve is reported once and skipped.
 * @since 0.1.0
 */
void sym_trace_install(uc_engine*              uc,
                       const emu_elf_source_t* elf,
                       const char* const*      names,
                       uint32_t                count)
{
  static uc_hook s_th[k_trace_sym_max];
  for (uint32_t i = 0U; (i < count) && (i < (uint32_t)k_trace_sym_max); i++) {
    const uint32_t addr = elf_sym_addr(elf, names[i], nullptr);
    if (addr == 0U) {
      (void)priv_emu_io_errf("  [symtrace] %s: symbol not found -- skipped\n", names[i]);
      continue;
    }
    (void)uc_hook_add(uc,
                      &s_th[i],
                      UC_HOOK_CODE,
                      (void*)internal_on_sym_trace,
                      (void*)names[i],
                      addr,
                      addr);
    (void)priv_emu_io_errf("  [symtrace] %s armed @ 0x%08X\n", names[i], (unsigned)addr);
  }
}
