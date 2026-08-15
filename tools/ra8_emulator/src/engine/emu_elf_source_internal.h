/**
 * @file emu_elf_source_internal.h
 * @brief Private raw-descriptor ELF source operations.
 * @details Shares the bounded source seam only among emulator engine and
 * composition translation units; public ELF types remain in `emu_elf.h`.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "emu_elf.h"
#include "ra8_attributes.h"

/**
 * @brief Open one regular ELF source without reading or allocating its bytes.
 * @param[in] path NUL-terminated source path.
 * @param[out] source Receives an independently owned descriptor on success.
 * @return Exact open result including the full source length.
 * @retval k_emu_elf_io_ok A regular source was published with its exact length.
 * @retval k_emu_elf_io_error The source could not be opened or stated.
 * @pre @p path and @p source are non-null.
 * @pre @p source is not currently open.
 * @post Failure leaves @p source untouched and owns no descriptor.
 * @post Success leaves @p source open until ::priv_emu_elf_source_close.
 * @note Distinct source objects have independent lifetimes.
 * @since 0.1.0
 */
RA8_PRIV [[nodiscard]] emu_elf_io_result_t priv_emu_elf_source_open(const char*       path,
                                                                    emu_elf_source_t* source);

/**
 * @brief Close and invalidate one independently owned ELF source.
 * @param[in,out] source Open source to close.
 * @return Exact close status.
 * @retval k_emu_elf_io_ok The descriptor was closed and invalidated.
 * @retval k_emu_elf_io_invalid @p source was null or already closed.
 * @pre @p source is null, closed, or owned by the caller.
 * @pre No concurrent read is active through @p source.
 * @post A valid descriptor is invalidated even when close reports error.
 * @post Other source instances remain usable.
 * @note Not safe during a concurrent read through the same source.
 * @since 0.1.0
 */
RA8_PRIV [[nodiscard]] emu_elf_io_result_t priv_emu_elf_source_close(emu_elf_source_t* source);

/**
 * @brief Read one exact source range into caller-owned bounded scratch.
 * @param[in] source Open immutable ELF source.
 * @param[in] offset First source byte to read.
 * @param[in] required_bytes Exact requested byte count.
 * @param[out] scratch Caller-owned destination.
 * @param[in] supplied_bytes Available destination bytes.
 * @param[out] view Receives the complete transient view on success.
 * @return Exact range, capacity, or transfer status.
 * @retval k_emu_elf_io_ok The view was published completely.
 * @retval k_emu_elf_io_capacity Required bytes exceed supplied bytes.
 * @pre @p scratch spans @p supplied_bytes writable bytes when nonzero.
 * @pre @p source remains open through the operation.
 * @post Failure leaves @p view untouched.
 * @post Success publishes exactly @p required_bytes at @p offset.
 * @note Failure may alter scratch bytes but never publishes a partial view.
 * @since 0.1.0
 */
RA8_PRIV [[nodiscard]] emu_elf_io_result_t priv_emu_elf_read(const emu_elf_source_t* source,
                                                             uint64_t                offset,
                                                             size_t                  required_bytes,
                                                             void*                   scratch,
                                                             size_t                  supplied_bytes,
                                                             emu_elf_view_t*         view);
