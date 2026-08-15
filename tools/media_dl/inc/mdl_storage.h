/**
 * @file mdl_storage.h
 * @brief Injected portable storage resources for downloader domain code.
 *
 * @details
 * Binds the existing ::fw_fs_t contract to the caller-owned workspaces used by
 * media-downloader file operations. The binding contains no path translation,
 * POSIX handle, device selection, or allocation. A host composition root may
 * supply `fw_if_fs_posix`; firmware may supply `fw_if_fs_ra8_vfs`; the domain
 * code below this seam is identical.
 *
 * @par Tag
 * [Ring 5 / Middleware] {World: Any}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "fw_if_fs.h"
#include "ra8_err.h"

/** @brief Recommended portable streaming scratch extent. */
typedef enum : uint16_t {
  k_mdl_storage_io_bytes = 8192U, /**< One bounded read/write chunk. */
} mdl_storage_limit_t;

/**
 * @struct mdl_storage_t
 * @brief One non-reentrant downloader filesystem dependency bundle.
 *
 * @details File and transaction storage remains owned by the composition root.
 * The capacities and alignments are checked once by ::mdl_storage_init. One
 * instance supports one operation at a time; independent workers require
 * independent workspaces and bindings.
 */
typedef struct {
  const fw_fs_t* fs;                          /**< Injected portable filesystem. */
  void*          file_workspace;              /**< Open-file backend state.      */
  void*          transaction_workspace;       /**< Transaction backend state.    */
  uint8_t*       io_buffer;                   /**< Caller-owned stream scratch.  */
  uint32_t       file_workspace_bytes;        /**< File workspace extent.        */
  uint32_t       transaction_workspace_bytes; /**< Transaction workspace extent. */
  uint32_t       io_buffer_bytes;             /**< Stream scratch extent.        */
} mdl_storage_t;

/**
 * @brief Validate and retain one filesystem plus caller-owned workspaces.
 * @details Requires namespace, stream, and transaction capabilities, verifies
 * the backend-advertised workspace extents and alignments, then publishes one
 * non-reentrant dependency bundle. No backend operation or allocation occurs.
 * @param[out] storage Downloader binding to initialize.
 * @param[in] fs Complete filesystem selected by the composition root.
 * @param[in,out] file_workspace Workspace used by one open source file.
 * @param[in] file_workspace_bytes Extent of @p file_workspace.
 * @param[in,out] transaction_workspace Workspace used by one staged publish.
 * @param[in] transaction_workspace_bytes Extent of @p transaction_workspace.
 * @param[out] io_buffer Caller-owned streaming scratch.
 * @param[in] io_buffer_bytes Nonzero extent of @p io_buffer.
 * @return Canonical status; success leaves @p storage ready for use.
 * @retval k_ra8_ok The complete binding was published.
 * @retval k_ra8_err_invalid_arg A pointer, extent, or alignment is invalid.
 * @retval k_ra8_err_not_supported A required filesystem facade is absent.
 * @retval k_ra8_err_no_mem A caller workspace is smaller than its backend cap.
 * @retval other A capability query failure propagated from @p fs.
 * @pre @p storage is non-null and writable for one complete object.
 * @pre Workspace extents describe distinct caller-owned storage spans that do
 *      not overlap @p storage or each other.
 * @post On failure @p storage and every caller workspace retain their entry
 *       values.
 * @post On success the binding retains only caller-provided pointers and caps.
 * @note Not thread-safe; each concurrent operation needs its own binding.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t mdl_storage_init(mdl_storage_t* storage,
                                         const fw_fs_t* fs,
                                         void*          file_workspace,
                                         uint32_t       file_workspace_bytes,
                                         void*          transaction_workspace,
                                         uint32_t       transaction_workspace_bytes,
                                         uint8_t*       io_buffer,
                                         uint32_t       io_buffer_bytes);

/**
 * @brief Copy a regular file through a validated atomic transaction.
 *
 * @details The source extent is snapshotted, streamed into a private sibling,
 * hashed while read, then independently size/hash validated through the
 * transaction's read-only staged handle before commit. An absent destination
 * uses create-new publication. An existing regular destination requires the
 * backend's truthful atomic-replace capability; a VFS/FAT binding therefore
 * returns ::k_ra8_err_not_supported without changing the destination.
 *
 * @param[in,out] storage Initialized filesystem binding and workspaces.
 * @param[in] source Canonical portable source path.
 * @param[in] destination Canonical portable destination path.
 * @return Canonical filesystem/validation status.
 * @retval k_ra8_ok The validated stage was published atomically.
 * @retval k_ra8_err_invalid_arg A binding/path/type contract is invalid.
 * @retval k_ra8_err_not_found The source does not exist.
 * @retval k_ra8_err_not_supported Existing-file replacement is not atomic on
 *                                 the selected backend.
 * @retval k_ra8_err_invalid_size The source/scratch/call bound was exceeded.
 * @retval k_ra8_err_protocol_error The independently read stage hash differed.
 * @retval other A namespace, stream, transaction, or cleanup error propagated.
 * @pre @p storage was initialized successfully and is exclusively owned.
 * @pre Both paths are canonical, distinct, and confined by the same binding.
 * @post Success means @p destination contains exactly the snapshotted source.
 * @post Failure before publication leaves an existing destination untouched.
 * @post A successful abort removes the private stage; an abort failure is
 *       returned directly because recovery may still be required.
 * @note Not thread-safe against concurrent mutation of either named file.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
mdl_storage_copy_atomic(mdl_storage_t* storage, const char* source, const char* destination);
