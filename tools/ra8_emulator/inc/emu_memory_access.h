/**
 * @file emu_memory_access.h
 * @brief Central first-party Unicorn memory access seam
 * @details The one place first-party emulator code reads or writes guest
 * memory. The shared apertures (SRAM, SDRAM, OSPI) are bound into every engine
 * as views onto one host mapping per aperture, so an access through any Secure
 * or Non-secure alias already reaches the same bytes every other view and every
 * other engine sees; this seam therefore validates its arguments and performs
 * the access directly, with no mirroring step and no per-access host I/O.
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
 * @brief Read guest memory through the central access seam.
 * @details Reads @p count bytes starting at @p address. For an aliased
 * aperture the bytes come from the shared host mapping, so a Secure read
 * observes what was written through the Non-secure alias or by the other CPU
 * engine without any explicit synchronisation.
 * @param[in,out] uc Engine whose guest memory is read.
 * @param[in] address First guest byte address.
 * @param[out] bytes Destination spanning @p count bytes.
 * @param[in] count Exact transfer length.
 * @return Unicorn-compatible completion status.
 * @retval UC_ERR_OK Every requested byte was read.
 * @retval UC_ERR_ARG A null argument or a zero-length transfer was supplied.
 * @retval UC_ERR_READ_UNMAPPED The range is not mapped in @p uc.
 * @pre @p uc, @p bytes are non-null and @p count is non-zero.
 * @pre The call executes on the emulator's single owning thread.
 * @post No guest memory or engine state changes.
 * @post @p bytes is written only when the status is UC_ERR_OK.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
uc_err emu_mem_read(uc_engine* uc, uint64_t address, void* bytes, size_t count);

/**
 * @brief Write guest memory through the central access seam.
 * @details Writes @p count bytes at @p address. For an aliased aperture the
 * bytes land in the shared host mapping, so the write is immediately visible
 * through the matching Secure/Non-secure alias and in every other engine bound
 * to the same workspace.
 * @param[in,out] uc Engine whose guest memory is written.
 * @param[in] address First guest byte address.
 * @param[in] bytes Source spanning @p count bytes.
 * @param[in] count Exact transfer length.
 * @return Unicorn-compatible completion status.
 * @retval UC_ERR_OK Every byte was written.
 * @retval UC_ERR_ARG A null argument or a zero-length transfer was supplied.
 * @retval UC_ERR_WRITE_UNMAPPED The range is not mapped in @p uc.
 * @pre @p uc, @p bytes are non-null and @p count is non-zero.
 * @pre The call executes on the emulator's single owning thread.
 * @post Success updates every view of the addressed bytes.
 * @post A rejected call changes no guest memory.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
uc_err emu_mem_write(uc_engine* uc, uint64_t address, const void* bytes, size_t count);
