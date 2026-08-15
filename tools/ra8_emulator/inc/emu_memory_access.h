/**
 * @file emu_memory_access.h
 * @brief Central first-party Unicorn memory access seam
 * @details Routes host reads and writes through the per-engine authoritative
 * backing association while preserving a direct fallback for unbound tests.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <unicorn/unicorn.h>

#include "ra8_attributes.h"

/**
 * @brief Read through the authoritative descriptor for aliased memory.
 * @param[in,out] uc Bound engine, or an ordinary unbound Unicorn engine.
 * @param[in] address First guest byte address.
 * @param[out] bytes Destination spanning @p count bytes.
 * @param[in] count Exact transfer length.
 * @return Unicorn-compatible completion status.
 * @post Bound aliased reads come from the raw descriptor without changing it.
 * @post Other reads preserve the direct Unicorn API behavior.
 * @since 0.1.0
  * @details Read through the authoritative descriptor for aliased memory; this step is contained within the emu memory access model and uses bounded caller or module-owned storage.
 * @retval value The operation-specific emu mem read value.
 * @pre Arguments satisfy the ranges documented for emu mem read. @pre The call executes on the emulator's single owning thread.
 * @note The operation is synchronous and does not transfer heap ownership.
 */
uc_err emu_mem_read(uc_engine* uc, uint64_t address, void* bytes, size_t count);

/**
 * @brief Publish one host write to authoritative and aliased engine state.
 * @param[in,out] uc Bound engine, or an ordinary unbound Unicorn engine.
 * @param[in] address First guest byte address.
 * @param[in] bytes Source spanning @p count bytes.
 * @param[in] count Exact transfer length, at most 4096 for aliased memory.
 * @return Unicorn-compatible completion status.
 * @post Success updates the raw descriptor and every active Secure/NS view.
 * @post Publication failure rolls back available views and poisons run output.
 * @since 0.1.0
  * @details Publish one host write to authoritative and aliased engine state; this step is contained within the emu memory access model and uses bounded caller or module-owned storage.
 * @retval value The operation-specific emu mem write value.
 * @pre Arguments satisfy the ranges documented for emu mem write. @pre The call executes on the emulator's single owning thread.
 * @note The operation is synchronous and does not transfer heap ownership.
 */
uc_err emu_mem_write(uc_engine* uc, uint64_t address, const void* bytes, size_t count);
