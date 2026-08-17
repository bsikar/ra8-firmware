/**
 * @file fw_if_fs_posix_stream_contracts_internal.h
 * @brief File-local contracts for the hosted POSIX byte-stream operations.
 * @ingroup grp_io
 *
 * @details
 * Declares the open-mode mapping, descriptor I/O, and vtable-borrow helpers
 * implemented by fw_if_fs_posix_stream.c, so their complete contracts stay
 * readable without pushing that unit past the repository size ceiling. This
 * header is private to fw_if_fs_posix_stream.c; the widened ::RA8_PRIV members
 * are the ones the adapter's transaction stage drives, and they are also
 * declared in fw_if_fs_posix_internal.h for their cross-unit callers.
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
 * @brief Open one confined regular file into caller workspace.
 * @details Maps safe flags, resolves the no-follow parent, opens relative to it,
 *          closes the parent, then rejects any resulting non-regular object.
 * @param[in,out] ctx Initialized confined-root adapter context.
 * @param[in] path Validated portable file path.
 * @param[in] mode Portable open mode.
 * @param[out] file_state Caller workspace receiving ::posix_file_state_t.
 * @param[in] state_bytes Writable workspace size.
 * @return Workspace, mode, resolution, open, or type-check status.
 * @retval k_ra8_ok @p file_state owns one open regular-file descriptor.
 * @retval k_ra8_err_no_mem The workspace is undersized.
 * @retval k_ra8_err_invalid_arg The mode or opened object type is invalid.
 * @retval k_ra8_err_* Mapped resolution, open, stat, or close failure.
 * @pre Pointer arguments and alignment satisfy the bound stream contract.
 * @pre @p ctx owns a live confined root descriptor.
 * @post Success transfers exactly one descriptor into @p file_state.
 * @post Failure closes every descriptor acquired internally.
 * @note Thread-safe for independent file states subject to namespace races.
 * @since Version 0.1.0
 */
RA8_PRIV ra8_err_t priv_fs_posix_open(void*             ctx,
                                      const char*       path,
                                      fw_fs_open_mode_t mode,
                                      void*             file_state,
                                      uint32_t          state_bytes);

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
 * @brief Complete POSIX short writes while reporting any accepted prefix.
 * @details Retries interrupts, advances over positive short writes, maps host
 *          errors, and treats a zero-byte write before completion as failure.
 * @param[in] ctx Unused confined-root context.
 * @param[in,out] file_state Open ::posix_file_state_t.
 * @param[in] src Source bytes.
 * @param[in] len Exact requested byte count.
 * @param[in,out] out_written Running accepted count initialized by public dispatch.
 * @return Complete-write status.
 * @retval k_ra8_ok Exactly @p len bytes were accepted.
 * @retval k_ra8_fail The host returned zero before completion.
 * @retval k_ra8_err_* Mapped non-interrupt write failure.
 * @pre @p file_state owns an open writable descriptor.
 * @pre @p src addresses @p len readable bytes when non-zero.
 * @post Success sets @p out_written to @p len.
 * @post Failure preserves the exact accepted prefix count.
 * @note Not thread-safe for concurrent use of one descriptor offset.
 * @since Version 0.1.0
 */
RA8_PRIV ra8_err_t priv_fs_posix_write(void*          ctx,
                                       void*          file_state,
                                       const uint8_t* src,
                                       uint32_t       len,
                                       uint32_t*      out_written);

/**
 * @brief Seek a POSIX descriptor to an unsigned absolute offset.
 * @details Rejects values not representable by signed 64-bit host offsets before
 *          issuing one `lseek(SEEK_SET)`.
 * @param[in] ctx Unused confined-root context.
 * @param[in,out] file_state Open ::posix_file_state_t.
 * @param[in] offset Absolute byte offset.
 * @return Host seek status.
 * @retval k_ra8_ok The descriptor position is @p offset.
 * @retval k_ra8_err_invalid_size @p offset exceeds INT64_MAX.
 * @retval k_ra8_err_* Mapped `lseek` failure.
 * @pre @p file_state owns an open seekable descriptor.
 * @pre No concurrent operation changes the same descriptor offset.
 * @post Success sets the next I/O position to @p offset.
 * @post File contents and length are unchanged.
 * @note Not thread-safe for concurrent use of one descriptor offset.
 * @since Version 0.1.0
 */
RA8_PRIV ra8_err_t priv_fs_posix_seek(void* ctx, void* file_state, uint64_t offset);

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

/**
 * @brief Report a POSIX descriptor's current file length.
 * @details Queries descriptor metadata with `fstat` and widens the non-negative
 *          regular-file size to the portable uint64_t result.
 * @param[in] ctx Unused confined-root context.
 * @param[in] file_state Open ::posix_file_state_t.
 * @param[out] out_size Receives current file length in bytes.
 * @return Host metadata-query status.
 * @retval k_ra8_ok @p out_size contains the current length.
 * @retval k_ra8_err_* Mapped `fstat` failure.
 * @pre @p file_state owns an open regular-file descriptor.
 * @pre @p out_size addresses one writable uint64_t object.
 * @post Success writes the length without changing descriptor position.
 * @post File contents are unchanged.
 * @note Thread-safe subject to file mutation and descriptor lifecycle synchronization.
 * @since Version 0.1.0
 */
RA8_PRIV ra8_err_t priv_fs_posix_size(void* ctx, void* file_state, uint64_t* out_size);

/**
 * @brief Flush file contents and metadata through POSIX `fsync`.
 * @details Delegates durability to the host descriptor and maps any failure.
 * @param[in] ctx Unused confined-root context.
 * @param[in,out] file_state Open ::posix_file_state_t.
 * @return Host synchronization status.
 * @retval k_ra8_ok The host accepted the durability request.
 * @retval k_ra8_err_* Mapped `fsync` failure.
 * @pre @p file_state owns an open descriptor valid for synchronization.
 * @pre No concurrent close consumes the descriptor.
 * @post Success makes prior writes durable according to host filesystem guarantees.
 * @post Descriptor ownership and offset are unchanged.
 * @note Thread-safe only with external descriptor lifecycle synchronization.
 * @since Version 0.1.0
 */
RA8_PRIV ra8_err_t priv_fs_posix_sync(void* ctx, void* file_state);

/**
 * @brief Close and consume one caller-owned POSIX file state.
 * @details Delegates to the shared EINTR-safe descriptor close helper, which
 *          invalidates the stored descriptor to prevent unsafe retries.
 * @param[in] ctx Unused confined-root context.
 * @param[in,out] file_state Open ::posix_file_state_t to consume.
 * @return Host close status.
 * @retval k_ra8_ok The descriptor closed successfully.
 * @retval k_ra8_err_* Mapped close failure.
 * @pre @p file_state owns one open descriptor.
 * @pre No concurrent I/O or close uses the descriptor.
 * @post The stored descriptor is invalidated on every return path.
 * @post The state cannot be used for I/O without reopening.
 * @note Not thread-safe for concurrent access to one file state.
 * @since Version 0.1.0
 */
RA8_PRIV ra8_err_t priv_fs_posix_close(void* ctx, void* file_state);

/**
 * @brief Borrow the immutable POSIX byte-stream operation table.
 * @details Hands back the single translation-unit-scoped vtable so the adapter's
 *          namespace and transaction units can bind or borrow the same stream
 *          implementation without a second copy of the table.
 * @return Address of the immutable stream operation table.
 * @retval non-NULL The one stream vtable, valid for the program lifetime.
 * @pre The caller only reads through the returned table.
 * @pre No caller attempts to modify the referenced operations.
 * @post The returned table outlives every caller and is never reassigned.
 * @post No adapter state is read or written by the call itself.
 * @note Thread-safe; the table is immutable and statically initialized.
 * @since Version 0.1.0
 */
RA8_PRIV const fw_fs_stream_iface_t* priv_fs_posix_stream_iface(void);
