/**
 * @file test_emu_memmap_stub.c
 * @brief Unbound central-memory fallback for ELF parser unit tests
 * @details The ELF parser unit target owns no memory-map workspace; this exact
 * fallback preserves its isolated Unicorn-write link seam.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include "emu_memmap.h"

uc_err emu_mem_write(uc_engine* uc, uint64_t address, const void* bytes, size_t count)
{
  return uc_mem_write(uc, address, bytes, count);
}
