/**
 * @file emu_memory_access.c
 * @brief Central Unicorn memory access seam (see emu_memory_access.h)
 *
 * @details Kept in its own translation unit so every consumer -- the engine,
 * the peripheral models and the ELF-parser unit target -- links the SAME
 * definition. It previously had a second, test-only definition; two definitions
 * of one access seam is exactly how a test can stop exercising the production
 * path without anything reporting it.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include "emu_memory_access.h"

uc_err emu_mem_read(uc_engine* uc, uint64_t address, void* bytes, size_t count)
{
  if ((uc == nullptr) || (bytes == nullptr) || (count == 0U)) {
    return UC_ERR_ARG;
  }
  return uc_mem_read(uc, address, bytes, count);
}

uc_err emu_mem_write(uc_engine* uc, uint64_t address, const void* bytes, size_t count)
{
  if ((uc == nullptr) || (bytes == nullptr) || (count == 0U)) {
    return UC_ERR_ARG;
  }
  return uc_mem_write(uc, address, bytes, count);
}
