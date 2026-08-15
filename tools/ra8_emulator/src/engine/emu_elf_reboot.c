/**
 * @file emu_elf_reboot.c
 * @brief Warm-reboot orchestration over an open streaming ELF source
 * @details Restores PT_LOAD bytes through the bounded loader, resets models,
 * and restarts from the vector table without changing source ownership.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include "board_console.h"
#include "board_net.h"
#include "board_periph.h"
#include "emu_console.h"
#include "emu_elf.h"
#include "emu_exc.h"
#include "emu_host_io_internal.h"
#include "emu_memmap.h"
#include "emu_mpu.h"
#include "emu_seams.h"
#include "emu_view.h"

uint32_t warm_reboot(uc_engine* uc, const emu_elf_source_t* elf, bool trace)
{
  if (load_elf(uc, elf) != 0) {
    (void)priv_emu_io_errf("ra8_emulator: warm_reboot: load_elf failed -- ending run\n");
    return 0U;
  }
  board_periph_init(trace);
  board_net_init(trace);
  emu_exc_reset();
  emu_mpu_clear_fault();
  emu_div0_clear_fault();
  emu_div0_disarm();
  board_console_reset();
  emu_console_reset();
  emu_view_reset_console();
  uint32_t sp = 0U;
  uint32_t pc = 0U;
  (void)emu_mem_read(uc, emu_memmap_mram_base(), &sp, 4U);
  (void)emu_mem_read(uc, emu_memmap_mram_base() + 4U, &pc, 4U);
  pc |= 1U;
  uint32_t xpsr = (uint32_t)k_xpsr_t_bit;
  (void)uc_reg_write(uc, UC_ARM_REG_SP, &sp);
  (void)uc_reg_write(uc, UC_ARM_REG_PC, &pc);
  (void)uc_reg_write(uc, UC_ARM_REG_XPSR, &xpsr);
  (void)priv_emu_io_errf("ra8_emulator: warm reboot -- reset SP=0x%08X PC=0x%08X\n", sp, pc);
  return pc;
}
