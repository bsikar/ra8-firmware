/**
 * @file fw_if_fs.h
 * @brief Architecture-neutral filesystem namespace, stream, and transaction
 * ports.
 * @ingroup grp_io
 *
 * @par Tag
 * [Ring 2 / Interface] {World: Any}
 *
 * @details
 * Portable libraries consume only the smallest facade they need. Paths are
 * rooted in the binding selected by the composition root: `/book/a.cbz` can be
 * a POSIX sandbox, an SD-card VFS mount, a RAM disk, or a future filesystem.
 * It is never a host absolute path and never contains a device name.
 *
 * All paths are canonical, NUL-terminated UTF-8 byte strings beginning with
 * `/`. `/` is the bound root. Empty components, trailing separators,
 * backslashes, colons, control bytes, `.` and `..` are rejected before a
 * backend runs. This lexical rule plus each adapter's symlink policy prevents
 * a portable caller from escaping the bound root.
 *
 * Handles and backend workspaces are caller-owned. This interface performs no
 * allocation and contains no operating-system or device header.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>

#include "fw_if_fs_types.h"
#include "ra8_err.h"

/** @brief Validate a canonical portable path against a binding's limits. */
[[nodiscard]] ra8_err_t fw_fs_path_validate(const fw_fs_caps_t* caps, const char* path);

/** @brief Copy the immutable capability snapshot from a complete binding. */
[[nodiscard]] ra8_err_t fw_fs_get_caps(const fw_fs_t* fs, fw_fs_caps_t* out);

/**
 * @brief Query a path; a miss is success with `out->exists == false`.
 * @details Backend contract violations are rejected as
 * ::k_ra8_err_invalid_state and leave @p out zeroed.
 */
[[nodiscard]] ra8_err_t
fw_fs_stat(const fw_fs_namespace_t* names, const char* path, fw_fs_stat_t* out);

/**
 * @brief Enumerate at most `max_entries` callback entries.
 * @details A backend-reported count above the bound is rejected as
 *          ::k_ra8_err_invalid_state and resets both outputs.
 */
[[nodiscard]] ra8_err_t fw_fs_listdir(const fw_fs_namespace_t* names,
                                      const char*              path,
                                      uint32_t                 max_entries,
                                      fw_fs_list_fn_t          callback,
                                      void*                    callback_ctx,
                                      uint32_t*                out_count,
                                      bool*                    out_complete);

/** @brief Create exactly one directory; parents must already exist. */
[[nodiscard]] ra8_err_t fw_fs_mkdir(const fw_fs_namespace_t* names, const char* path);

/** @brief Remove one regular file; directories require ::fw_fs_rmdir. */
[[nodiscard]] ra8_err_t fw_fs_unlink(const fw_fs_namespace_t* names, const char* path);

/** @brief Remove one empty directory; recursive deletion is deliberately
 * absent. */
[[nodiscard]] ra8_err_t fw_fs_rmdir(const fw_fs_namespace_t* names, const char* path);

/** @brief Rename inside one bound root/volume with optional atomic replacement.
 */
[[nodiscard]] ra8_err_t fw_fs_rename(const fw_fs_namespace_t* names,
                                     const char*              old_path,
                                     const char*              new_path,
                                     bool                     replace);

/**
 * @brief Report total/free/used bytes when space-query capability is present.
 * @details Impossible successful values are rejected as
 *          ::k_ra8_err_invalid_state and leave @p out zeroed.
 */
[[nodiscard]] ra8_err_t fw_fs_space(const fw_fs_namespace_t* names, fw_fs_space_t* out);

/** @brief Open a file into a caller-owned handle and backend workspace. */
[[nodiscard]] ra8_err_t fw_fs_open(const fw_fs_stream_port_t* streams,
                                   const char*                path,
                                   fw_fs_open_mode_t          mode,
                                   fw_fs_file_t*              file,
                                   void*                      workspace,
                                   uint32_t                   workspace_size);

/**
 * @brief Read up to `capacity` bytes; zero bytes is EOF.
 * @details A backend count above @p capacity is rejected as
 *          ::k_ra8_err_invalid_state and reset to zero.
 */
[[nodiscard]] ra8_err_t
fw_fs_read(fw_fs_file_t* file, uint8_t* destination, uint32_t capacity, uint32_t* out_read);

/**
 * @brief Attempt to write all bytes, reporting any accepted prefix.
 * @details A backend count above @p length is rejected as
 *          ::k_ra8_err_invalid_state and reset to zero.
 */
[[nodiscard]] ra8_err_t
fw_fs_write(fw_fs_file_t* file, const uint8_t* source, uint32_t length, uint32_t* out_written);

/** @brief Seek to an absolute byte offset from the beginning. */
[[nodiscard]] ra8_err_t fw_fs_seek(fw_fs_file_t* file, uint64_t absolute_offset);

/** @brief Report the current absolute offset. */
[[nodiscard]] ra8_err_t fw_fs_tell(fw_fs_file_t* file, uint64_t* out_offset);

/** @brief Report the open file's current length. */
[[nodiscard]] ra8_err_t fw_fs_file_size(fw_fs_file_t* file, uint64_t* out_size);

/** @brief Request file synchronization or return ::k_ra8_err_not_supported. */
[[nodiscard]] ra8_err_t fw_fs_sync(fw_fs_file_t* file);

/**
 * @brief Close and consume an open handle.
 * @details The handle is invalidated even when close reports an error, because
 *          retrying close is unsafe on some backends.
 */
[[nodiscard]] ra8_err_t fw_fs_close(fw_fs_file_t* file);

/**
 * @brief Create a hidden sibling staging file for one destination.
 * @details Returns ::k_ra8_err_not_supported when the bound port does not
 *          advertise ::k_fw_fs_cap_transactions.
 */
[[nodiscard]] ra8_err_t fw_fs_transaction_begin(const fw_fs_transaction_port_t* transactions,
                                                const char*                     destination,
                                                fw_fs_transaction_policy_t      policy,
                                                fw_fs_transaction_t*            transaction,
                                                void*                           workspace,
                                                uint32_t                        workspace_size);

/**
 * @brief Append bytes to the private staging artifact.
 * @details A backend count above @p length is rejected as
 *          ::k_ra8_err_invalid_state and reset to zero.
 */
[[nodiscard]] ra8_err_t fw_fs_transaction_write(fw_fs_transaction_t* transaction,
                                                const uint8_t*       source,
                                                uint32_t             length,
                                                uint32_t*            out_written);

/**
 * @brief Seek the staging writer to an absolute byte offset for bounded
 * backfill.
 * @details Seeking never extends or publishes the stage. Writes and seeks are
 *          refused after successful validation.
 */
[[nodiscard]] ra8_err_t fw_fs_transaction_seek(fw_fs_transaction_t* transaction,
                                               uint64_t             absolute_offset);

/**
 * @brief Flush/reopen the stage and ask `validator` to inspect it read-only.
 * @details Commit is unavailable until this succeeds; later writes are refused.
 */
[[nodiscard]] ra8_err_t fw_fs_transaction_validate(fw_fs_transaction_t* transaction,
                                                   fw_fs_validate_fn_t  validator,
                                                   void*                validator_ctx);

/**
 * @brief Publish a validated stage.
 * @param[out] out_published True when the destination changed, even if a later
 *                           durability operation failed.
 * @details A backend returning success without publication violates the
 *          contract and is reported as ::k_ra8_err_invalid_state; the
 *          transaction remains active so it can be aborted.
 */
[[nodiscard]] ra8_err_t fw_fs_transaction_commit(fw_fs_transaction_t* transaction,
                                                 bool*                out_published);

/** @brief Close and remove an unpublished staging artifact. */
[[nodiscard]] ra8_err_t fw_fs_transaction_abort(fw_fs_transaction_t* transaction);

#ifdef __cplusplus
}
#endif
