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
 * @struct mdl_storage_txn_t
 * @brief Caller-owned streaming publication transaction with running identity.
 * @details Wraps one injected filesystem transaction without hiding its
 *          workspace or allocating; independent writers require independent
 *          ::mdl_storage_t bindings.
 * @since 0.1.0
 */
typedef struct {
  mdl_storage_t*      storage;     /**< Exclusively borrowed storage binding. */
  fw_fs_transaction_t transaction; /**< Active private filesystem stage.      */
  uint64_t            size_bytes;  /**< Exact bytes accepted into the stage.  */
  uint64_t            hash;        /**< Running FNV-1a identity.              */
  uint32_t            write_calls; /**< Bounded backend write-call tally.     */
} mdl_storage_txn_t;

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

/**
 * @brief Begin one streamed create or truthful atomic replacement.
 * @param[out] writer Caller-owned writer state to initialize.
 * @param[in,out] storage Initialized exclusive storage binding.
 * @param[in] destination Canonical destination path.
 * @return Canonical namespace or transaction status.
 * @retval k_ra8_ok A private empty stage is active.
 * @retval k_ra8_err_invalid_arg A pointer/path/type contract is invalid.
 * @retval k_ra8_err_not_supported Existing replacement is not atomic.
 * @retval other Namespace or transaction-begin failure propagated.
 * @pre @p writer is inactive and @p storage is exclusively owned.
 * @pre @p destination is NUL-terminated and confined by the bound filesystem.
 * @post Success initializes an empty FNV identity and active stage.
 * @post Failure publishes no destination and leaves @p writer inactive.
 * @note Not thread-safe for shared storage workspaces.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
mdl_storage_txn_begin(mdl_storage_txn_t* writer, mdl_storage_t* storage, const char* destination);

/**
 * @brief Begin one streamed create-new publication without replacement.
 * @details Uses the same caller-owned transaction workspace and running
 *          identity as ::mdl_storage_txn_begin, but fixes the backend policy to
 *          ::k_fw_fs_txn_create_new. An existing destination is therefore
 *          refused by the transaction contract and remains byte-for-byte
 *          unchanged on every return path.
 * @param[out] writer Caller-owned writer state to initialize.
 * @param[in,out] storage Initialized exclusive storage binding.
 * @param[in] destination Canonical absent destination path.
 * @return Canonical transaction-begin status.
 * @retval k_ra8_ok A private empty create-new stage is active.
 * @retval k_ra8_err_exists The destination already exists.
 * @retval k_ra8_err_invalid_arg A pointer/path/lifecycle contract is invalid.
 * @retval other Transaction-begin failure propagated from the backend.
 * @pre @p writer is inactive and @p storage is exclusively owned.
 * @pre @p destination is NUL-terminated and confined by the bound filesystem.
 * @post Success initializes an empty FNV identity and active private stage.
 * @post Failure publishes no destination and leaves @p writer inactive.
 * @note Not thread-safe against concurrent creation of @p destination.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t mdl_storage_txn_begin_new(mdl_storage_txn_t* writer,
                                                  mdl_storage_t*     storage,
                                                  const char*        destination);

/**
 * @brief Append one complete caller chunk, tolerating bounded short writes.
 * @param[in,out] writer Active streamed transaction.
 * @param[in] bytes Readable source bytes.
 * @param[in] length Source extent.
 * @return Canonical write/progress status.
 * @retval k_ra8_ok Every byte was staged and folded into the identity.
 * @retval k_ra8_err_invalid_state The writer is inactive or made no progress.
 * @retval k_ra8_err_invalid_size Size or write-call bounds were exceeded.
 * @retval other Backend transaction-write failure propagated.
 * @pre @p writer owns one active stage and @p bytes covers @p length bytes.
 * @pre No caller mutates the transaction workspace concurrently.
 * @post Success advances size/hash by exactly @p length.
 * @post Failure leaves the transaction active for explicit abort.
 * @note An empty chunk succeeds without calling the backend.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
mdl_storage_txn_write(mdl_storage_txn_t* writer, const uint8_t* bytes, uint32_t length);

/**
 * @brief Independently validate and publish a completed streamed transaction.
 * @param[in,out] writer Active streamed transaction.
 * @return Canonical validation, commit, or cleanup status.
 * @retval k_ra8_ok Exact size/hash were independently verified and published.
 * @retval k_ra8_err_protocol_error The staged identity differed.
 * @retval other Validation, commit, or abort failure propagated.
 * @pre @p writer owns one active transaction and initialized storage binding.
 * @pre The destination namespace is not concurrently mutated.
 * @post Success consumes and clears @p writer.
 * @post Failure attempts abort; an abort failure remains visible.
 * @note Validation rereads through the binding's caller-owned I/O buffer.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t mdl_storage_txn_commit(mdl_storage_txn_t* writer);

/**
 * @brief Abort and clear one streamed transaction.
 * @param[in,out] writer Writer to abort; an inactive writer is accepted.
 * @return Canonical cleanup status.
 * @retval k_ra8_ok No private stage remains and @p writer is zeroed.
 * @retval other Backend abort failure; writer state remains for diagnosis.
 * @pre @p writer is non-NULL and exclusively owned.
 * @post Success clears all retained storage and identity state.
 * @post No destination is published.
 * @note Safe to call after any write/network failure.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t mdl_storage_txn_abort(mdl_storage_txn_t* writer);
