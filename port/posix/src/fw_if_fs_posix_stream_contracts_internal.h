/**
 * @file fw_if_fs_posix_stream_contracts_internal.h
 * @brief File-local contracts for the hosted POSIX byte-stream operations.
 * @ingroup grp_io
 *
 * @details
 * Declares the file-local open-mode mapping, descriptor-read, and offset-query
 * helpers implemented by fw_if_fs_posix_stream.c. Their complete contracts
 * stay readable without pushing that unit past the repository size ceiling;
 * the cross-unit ::RA8_PRIV operation contracts instead live in
 * fw_if_fs_posix_internal.h, their single declaration authority.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since Version 0.1.0
 */
#pragma once

#include "fw_if_fs_posix_internal.h"
#include "ra8_attributes.h"

/**
 * @brief Map a portable open mode to confined no-follow `openat` flags.
 * @details Adds close-on-exec and no-follow to read, truncate, append, and
 *          exclusive-create modes; rejects every unrecognized value.
 * @param[in] mode Portable open mode.
 * @param[out] out_flags Receives POSIX open flags.
 * @return Mode mapping status.
 * @retval k_ra8_ok @p out_flags contains the complete safe host flag set.
 * @retval k_ra8_err_invalid_arg @p mode is unsupported or invalid.
 * @pre @p out_flags addresses one writable integer.
 * @pre Required fallback macros preserve fail-closed behavior on this host.
 * @post Success writes flags corresponding exactly to @p mode.
 * @post Failure leaves @p out_flags unchanged.
 * @note Pure apart from caller output and thread-safe.
 * @since Version 0.1.0
 */
static ra8_err_t internal_open_flags(fw_fs_open_mode_t mode, int* out_flags);

/**
 * @brief Retry an interrupted POSIX read and expose a legitimate short read.
 * @details Repeats only `EINTR`, maps other host errors, and preserves short-read
 *          and zero-byte EOF semantics in the portable count.
 * @param[in] ctx Unused confined-root context.
 * @param[in,out] file_state Open ::posix_file_state_t.
 * @param[out] dst Destination for at most @p cap bytes.
 * @param[in] cap Writable destination capacity.
 * @param[out] out_read Accepted byte count.
 * @return Host read status.
 * @retval k_ra8_ok A bounded prefix or EOF was reported.
 * @retval k_ra8_err_* Mapped non-interrupt read failure.
 * @pre @p file_state owns an open readable descriptor.
 * @pre @p dst addresses @p cap writable bytes when non-zero.
 * @post Success reports no more than @p cap bytes.
 * @post The file offset advances by the reported count.
 * @note Not thread-safe for concurrent use of one descriptor offset.
 * @since Version 0.1.0
 */
static ra8_err_t
internal_read(void* ctx, void* file_state, uint8_t* dst, uint32_t cap, uint32_t* out_read);

/**
 * @brief Report the current POSIX descriptor offset.
 * @details Uses `lseek(SEEK_CUR)` without moving the offset and maps host errors.
 * @param[in] ctx Unused confined-root context.
 * @param[in] file_state Open ::posix_file_state_t.
 * @param[out] out_offset Receives the absolute byte offset.
 * @return Host position-query status.
 * @retval k_ra8_ok @p out_offset contains the current position.
 * @retval k_ra8_err_* Mapped `lseek` failure.
 * @pre @p file_state owns an open seekable descriptor.
 * @pre @p out_offset addresses one writable uint64_t object.
 * @post Success writes the position without changing it.
 * @post File contents and length are unchanged.
 * @note Thread-safe only with external descriptor-offset synchronization.
 * @since Version 0.1.0
 */
static ra8_err_t internal_tell(void* ctx, void* file_state, uint64_t* out_offset);
