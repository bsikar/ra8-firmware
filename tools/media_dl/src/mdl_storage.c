/**
 * @file mdl_storage.c
 * @brief Portable downloader storage binding and validated atomic copy.
 *
 * @details Implements the downloader's reusable filesystem dependency bundle
 * and the first transaction-backed publication operation. All filesystem
 * effects route through `fw_if_fs`; workspaces and buffers are bounded and
 * caller-owned.
 *
 * @par Tag
 * [Ring 5 / Middleware] {World: Any}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#include "mdl_storage.h"

#include <stdint.h>
#include <string.h>

#include "mdl_hash.h"
#include "ra8_attributes.h"

/** @brief Bounded I/O progress limits for a complete copy. */
typedef enum : uint32_t {
  k_storage_io_calls   = 2000001U, /**< Short-I/O and EOF call ceiling.   */
  k_storage_span_count = 4U,       /**< Output plus workspace span count. */
} mdl_storage_limits_t;

/** @brief One overflow-checked half-open caller-storage interval. */
typedef struct internal_storage_span_t {
  uintptr_t begin; /**< First byte address.         */
  uintptr_t end;   /**< One-past-last byte address. */
} internal_storage_span_t;

/** @brief Expected staged identity retained during validation. */
typedef struct {
  uint64_t size_bytes;   /**< Exact staged length.  */
  uint64_t hash;         /**< FNV identity.         */
  uint8_t* buffer;       /**< Caller-owned scratch. */
  uint32_t buffer_bytes; /**< Scratch extent.       */
} staged_identity_t;

/**
 * @brief Convert one non-empty caller region into an address interval
 * @details Uses integer endpoints solely to reject wrapping and overlapping
 * workspace contracts before any caller byte is changed.
 * @param[in] pointer Region start.
 * @param[in] bytes Nonzero region extent.
 * @param[out] out Receives the half-open interval.
 * @return Canonical span-validation status.
 * @retval k_ra8_ok The interval is non-wrapping.
 * @retval k_ra8_err_invalid_arg A pointer or extent is zero.
 * @retval k_ra8_err_invalid_size The one-past-last address would wrap.
 * @pre @p out is non-null and writable.
 * @pre @p pointer is either null or denotes the claimed caller allocation.
 * @post Success initializes @p out without dereferencing @p pointer.
 * @post Failure leaves caller storage unchanged.
 * @note Thread-safe and allocation-free.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_make_span(const void* pointer, uint32_t bytes, internal_storage_span_t* out)
{
  if ((pointer == nullptr) || (bytes == 0U)) {
    return k_ra8_err_invalid_arg;
  }
  const uintptr_t begin = (uintptr_t)pointer;
  if ((uintptr_t)bytes > (UINTPTR_MAX - begin)) {
    return k_ra8_err_invalid_size;
  }
  *out = (internal_storage_span_t){.begin = begin, .end = begin + (uintptr_t)bytes};
  return k_ra8_ok;
}

/**
 * @brief Return whether two validated half-open intervals share a byte
 * @details Strict endpoint comparisons treat adjacent workspaces as disjoint.
 * @param[in] left First interval.
 * @param[in] right Second interval.
 * @return Overlap result.
 * @retval true At least one byte is shared.
 * @retval false The intervals are disjoint.
 * @pre Both inputs are non-null and came from ::internal_make_span.
 * @pre Both interval endpoints are ordered and non-wrapping.
 * @post Neither interval is modified.
 * @post The result depends only on the four endpoints.
 * @note Thread-safe and allocation-free.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_overlap(const internal_storage_span_t* left,
                                          const internal_storage_span_t* right)
{
  return (left->begin < right->end) && (right->begin < left->end);
}

/**
 * @brief Validate that output and operational workspaces are pairwise disjoint
 * @details Builds four guarded spans and performs a fixed pairwise comparison;
 * source-file and transaction state must coexist during atomic copy.
 * @param[in] storage Output object storage.
 * @param[in] file_workspace File-backend workspace.
 * @param[in] file_workspace_bytes File-workspace extent.
 * @param[in] transaction_workspace Transaction-backend workspace.
 * @param[in] transaction_workspace_bytes Transaction-workspace extent.
 * @param[in] io_buffer Streaming I/O buffer.
 * @param[in] io_buffer_bytes I/O-buffer extent.
 * @return Canonical separation status.
 * @retval k_ra8_ok All four intervals are valid and disjoint.
 * @retval k_ra8_err_invalid_arg A region is empty or overlaps another.
 * @retval k_ra8_err_invalid_size An interval endpoint would wrap.
 * @pre Pointer/extent pairs describe the caller's actual allocations.
 * @pre No caller concurrently mutates the four span descriptions.
 * @post No byte in any described region is modified.
 * @post Success proves every pair is disjoint.
 * @note Thread-safe and allocation-free.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_validate_spans(mdl_storage_t* storage,
                                                      void*          file_workspace,
                                                      uint32_t       file_workspace_bytes,
                                                      void*          transaction_workspace,
                                                      uint32_t       transaction_workspace_bytes,
                                                      uint8_t*       io_buffer,
                                                      uint32_t       io_buffer_bytes)
{
  internal_storage_span_t spans[k_storage_span_count] = {};
  ra8_err_t               err = internal_make_span(storage, (uint32_t)sizeof(*storage), &spans[0]);
  if (err == k_ra8_ok) {
    err = internal_make_span(file_workspace, file_workspace_bytes, &spans[1]);
  }
  if (err == k_ra8_ok) {
    err = internal_make_span(transaction_workspace, transaction_workspace_bytes, &spans[2]);
  }
  if (err == k_ra8_ok) {
    err = internal_make_span(io_buffer, io_buffer_bytes, &spans[3]);
  }
  if (err != k_ra8_ok) {
    return err;
  }
  for (uint32_t left = 0U; left < (uint32_t)k_storage_span_count; ++left) {
    for (uint32_t right = left + 1U; right < (uint32_t)k_storage_span_count; ++right) {
      if (internal_overlap(&spans[left], &spans[right])) {
        return k_ra8_err_invalid_arg;
      }
    }
  }
  return k_ra8_ok;
}

/**
 * @brief Return whether one pointer satisfies a backend alignment contract
 * @details Applies the standard power-of-two mask after ::fw_fs_get_caps has
 * validated the advertised nonzero alignment.
 * @param[in] pointer Candidate workspace address.
 * @param[in] alignment Required power-of-two byte alignment.
 * @return Workspace alignment result.
 * @retval true The pointer is non-null and correctly aligned.
 * @retval false The pointer is null, the alignment is zero, or the address is
 *               misaligned.
 * @pre @p alignment originated from a successfully bound filesystem facade.
 * @pre No dereference of @p pointer is required.
 * @post No caller or backend state is modified.
 * @post The result depends only on the pointer value and alignment mask.
 * @note Thread-safe and allocation-free.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_aligned(const void* pointer, uint8_t alignment)
{
  if ((pointer == nullptr) || (alignment == 0U)) {
    return false;
  }
  return ((uintptr_t)pointer & ((uintptr_t)alignment - 1U)) == 0U;
}

ra8_err_t mdl_storage_init(mdl_storage_t* storage,
                           const fw_fs_t* fs,
                           void*          file_workspace,
                           uint32_t       file_workspace_bytes,
                           void*          transaction_workspace,
                           uint32_t       transaction_workspace_bytes,
                           uint8_t*       io_buffer,
                           uint32_t       io_buffer_bytes)
{
  if (storage == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  if ((fs == nullptr) || (file_workspace == nullptr) || (transaction_workspace == nullptr) ||
      (io_buffer == nullptr) || (io_buffer_bytes == 0U)) {
    return k_ra8_err_invalid_arg;
  }
  ra8_err_t err = internal_validate_spans(storage,
                                          file_workspace,
                                          file_workspace_bytes,
                                          transaction_workspace,
                                          transaction_workspace_bytes,
                                          io_buffer,
                                          io_buffer_bytes);
  if (err != k_ra8_ok) {
    return err;
  }
  fw_fs_caps_t caps = {};
  err               = fw_fs_get_caps(fs, &caps);
  if (err != k_ra8_ok) {
    return err;
  }
  const uint32_t required = (uint32_t)k_fw_fs_cap_namespace | (uint32_t)k_fw_fs_cap_stream |
                            (uint32_t)k_fw_fs_cap_transactions;
  if ((caps.flags & required) != required) {
    return k_ra8_err_not_supported;
  }
  if ((file_workspace_bytes < caps.file_workspace_bytes) ||
      (transaction_workspace_bytes < caps.transaction_workspace_bytes)) {
    return k_ra8_err_no_mem;
  }
  if (!internal_aligned(file_workspace, caps.file_workspace_align) ||
      !internal_aligned(transaction_workspace, caps.transaction_workspace_align)) {
    return k_ra8_err_invalid_arg;
  }
  const mdl_storage_t candidate = {.fs                          = fs,
                                   .file_workspace              = file_workspace,
                                   .transaction_workspace       = transaction_workspace,
                                   .io_buffer                   = io_buffer,
                                   .file_workspace_bytes        = file_workspace_bytes,
                                   .transaction_workspace_bytes = transaction_workspace_bytes,
                                   .io_buffer_bytes             = io_buffer_bytes};
  *storage                      = candidate;
  return k_ra8_ok;
}

/**
 * @brief Abort an active transaction and preserve cleanup failure visibility
 * @details A successful abort returns the original operation status. If abort
 * itself fails, that cleanup error takes precedence because a private stage may
 * remain and the caller must not mistake the failure for a fully cleaned path.
 * @param[in,out] transaction Transaction to abort when still active.
 * @param[in] primary Status from the operation that triggered cleanup.
 * @return The primary status after successful cleanup, otherwise abort status.
 * @retval k_ra8_ok The transaction was already inactive or cleanly aborted and
 *                  no earlier operation failed.
 * @retval other The primary operation or transaction abort failed.
 * @pre @p transaction is non-null and initialized or all-zero.
 * @pre @p primary is a canonical ::ra8_err_t status.
 * @post Success leaves no active transaction.
 * @post An abort failure remains visible to the caller.
 * @note Not thread-safe; the transaction is exclusively owned.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_abort(fw_fs_transaction_t* transaction, ra8_err_t primary)
{
  if (transaction->active) {
    const ra8_err_t aborted = fw_fs_transaction_abort(transaction);
    if (aborted != k_ra8_ok) {
      return aborted;
    }
  }
  return primary;
}

/**
 * @brief Write all of one buffer while rejecting zero-progress success
 * @details Repeats bounded transaction writes until every byte is accepted,
 * making short writes visible and enforcing the module-wide call ceiling.
 * @param[in,out] transaction Active destination transaction.
 * @param[in] source Readable source bytes.
 * @param[in] length Exact byte count to publish.
 * @param[in,out] calls Shared write-call tally.
 * @return Canonical write/progress status.
 * @retval k_ra8_ok Exactly @p length bytes were consumed.
 * @retval k_ra8_err_invalid_state A successful backend write made no progress.
 * @retval k_ra8_err_invalid_size The call ceiling was exhausted.
 * @retval other A transaction write failure propagated.
 * @pre @p transaction is active and exclusively owned.
 * @pre @p source covers @p length readable bytes and @p calls is writable.
 * @post Success advances the staged extent by exactly @p length bytes.
 * @post Every attempted write increments @p calls exactly once.
 * @note Not thread-safe; transaction state is mutable.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_write_all(fw_fs_transaction_t* transaction,
                                                 const uint8_t*       source,
                                                 uint32_t             length,
                                                 uint32_t*            calls)
{
  uint32_t offset = 0U;
  while (offset < length) {
    if (*calls >= (uint32_t)k_storage_io_calls) {
      return k_ra8_err_invalid_size;
    }
    uint32_t        written = 0U;
    const ra8_err_t err =
      fw_fs_transaction_write(transaction, source + offset, length - offset, &written);
    ++(*calls);
    if (err != k_ra8_ok) {
      return err;
    }
    if (written == 0U) {
      return k_ra8_err_invalid_state;
    }
    offset += written;
  }
  return k_ra8_ok;
}

/**
 * @brief Validate staged size and identity through its generic read handle
 * @details Independently reads the complete stage using the caller's shared
 * scratch, then compares exact extent and FNV identity before commit.
 * @param[in] ctx Read-only ::staged_identity_t expectation.
 * @param[in,out] staged Validation handle positioned at byte zero.
 * @return Canonical size/read/identity status.
 * @retval k_ra8_ok Size and hash both match.
 * @retval k_ra8_err_invalid_arg A required pointer is null.
 * @retval k_ra8_err_invalid_size The staged extent differs.
 * @retval k_ra8_err_protocol_error The staged hash differs.
 * @retval other A file query/read failure propagated.
 * @pre @p ctx describes live caller-owned scratch for the complete callback.
 * @pre @p staged is open for validation and exclusively owned by the facade.
 * @post Success consumes the stage through EOF without changing its bytes.
 * @post No publication occurs in this callback.
 * @note Not thread-safe; it borrows the binding's single I/O buffer.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_validate_stage(void* ctx, fw_fs_file_t* staged)
{
  if ((ctx == nullptr) || (staged == nullptr)) {
    return k_ra8_err_invalid_arg;
  }
  const staged_identity_t* const expected = (const staged_identity_t*)ctx;
  uint64_t                       size     = 0U;
  ra8_err_t                      err      = fw_fs_file_size(staged, &size);
  if (err != k_ra8_ok) {
    return err;
  }
  if (size != expected->size_bytes) {
    return k_ra8_err_invalid_size;
  }
  uint64_t hash = 0U;
  err =
    mdl_hash_stream(staged, expected->size_bytes, expected->buffer, expected->buffer_bytes, &hash);
  if (err != k_ra8_ok) {
    return err;
  }
  return (hash == expected->hash) ? k_ra8_ok : k_ra8_err_protocol_error;
}

/**
 * @brief Select create-new or truthful atomic replacement policy
 * @details Stats the destination without mutation. Missing paths select
 * create-new; regular files select atomic replacement and let the transaction
 * facade reject a backend that cannot provide that guarantee.
 * @param[in] storage Initialized storage binding.
 * @param[in] destination Canonical destination path.
 * @param[out] out Receives the selected transaction policy.
 * @return Canonical namespace/type status.
 * @retval k_ra8_ok One policy was published.
 * @retval k_ra8_err_invalid_arg An existing destination is not a file.
 * @retval other A namespace stat failure propagated.
 * @pre @p storage and its filesystem are initialized.
 * @pre @p destination is canonical and @p out is writable.
 * @post Success writes exactly one supported policy to @p out.
 * @post No filesystem object is created, removed, or renamed.
 * @note Not thread-safe against destination namespace mutation.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_policy(const mdl_storage_t*        storage,
                                              const char*                 destination,
                                              fw_fs_transaction_policy_t* out)
{
  fw_fs_stat_t    stat = {};
  const ra8_err_t err  = fw_fs_stat(&storage->fs->names, destination, &stat);
  if (err != k_ra8_ok) {
    return err;
  }
  if (!stat.exists) {
    *out = k_fw_fs_txn_create_new;
    return k_ra8_ok;
  }
  if (stat.type != k_fw_fs_node_file) {
    return k_ra8_err_invalid_arg;
  }
  *out = k_fw_fs_txn_replace_atomic;
  return k_ra8_ok;
}

ra8_err_t
mdl_storage_copy_atomic(mdl_storage_t* storage, const char* source, const char* destination)
{
  if ((storage == nullptr) || (storage->fs == nullptr) || (source == nullptr) ||
      (destination == nullptr)) {
    return k_ra8_err_invalid_arg;
  }
  if (strcmp(source, destination) == 0) {
    return k_ra8_err_invalid_arg;
  }
  fw_fs_stat_t source_stat = {};
  ra8_err_t    err         = fw_fs_stat(&storage->fs->names, source, &source_stat);
  if (err != k_ra8_ok) {
    return err;
  }
  if (!source_stat.exists) {
    return k_ra8_err_not_found;
  }
  if (source_stat.type != k_fw_fs_node_file) {
    return k_ra8_err_invalid_arg;
  }
  if (source_stat.size_bytes > (uint64_t)k_mdl_hash_max_file_bytes) {
    return k_ra8_err_invalid_size;
  }

  fw_fs_transaction_policy_t policy = k_fw_fs_txn_create_new;
  err                               = internal_policy(storage, destination, &policy);
  if (err != k_ra8_ok) {
    return err;
  }

  fw_fs_file_t source_file = {};
  err                      = fw_fs_open(&storage->fs->streams,
                                        source,
                                        k_fw_fs_open_read,
                                        &source_file,
                                        storage->file_workspace,
                                        storage->file_workspace_bytes);
  if (err != k_ra8_ok) {
    return err;
  }
  fw_fs_transaction_t transaction = {};
  err                             = fw_fs_transaction_begin(&storage->fs->transactions,
                                                            destination,
                                                            policy,
                                                            &transaction,
                                                            storage->transaction_workspace,
                                                            storage->transaction_workspace_bytes);
  if (err != k_ra8_ok) {
    (void)fw_fs_close(&source_file);
    return err;
  }

  uint64_t remaining   = source_stat.size_bytes;
  uint64_t hash        = (uint64_t)k_mdl_fnv_offset;
  uint32_t read_calls  = 0U;
  uint32_t write_calls = 0U;
  while ((remaining > 0U) && (read_calls < (uint32_t)k_storage_io_calls)) {
    const uint32_t wanted = (remaining < (uint64_t)storage->io_buffer_bytes)
                              ? (uint32_t)remaining
                              : storage->io_buffer_bytes;
    uint32_t       read   = 0U;
    err                   = fw_fs_read(&source_file, storage->io_buffer, wanted, &read);
    ++read_calls;
    if (err != k_ra8_ok) {
      break;
    }
    if ((read == 0U) || ((uint64_t)read > remaining)) {
      err = k_ra8_fail;
      break;
    }
    hash = mdl_hash_bytes_seed(storage->io_buffer, read, hash);
    err  = internal_write_all(&transaction, storage->io_buffer, read, &write_calls);
    if (err != k_ra8_ok) {
      break;
    }
    remaining -= read;
  }
  if ((err == k_ra8_ok) && (remaining != 0U)) {
    err = k_ra8_err_invalid_size;
  }
  if (err == k_ra8_ok) {
    uint32_t trailing = 0U;
    err               = fw_fs_read(&source_file, storage->io_buffer, 1U, &trailing);
    if ((err == k_ra8_ok) && (trailing != 0U)) {
      err = k_ra8_fail;
    }
  }
  const ra8_err_t closed = fw_fs_close(&source_file);
  if (err == k_ra8_ok) {
    err = closed;
  }
  if (err != k_ra8_ok) {
    return internal_abort(&transaction, err);
  }

  const staged_identity_t identity = {.size_bytes   = source_stat.size_bytes,
                                      .hash         = hash,
                                      .buffer       = storage->io_buffer,
                                      .buffer_bytes = storage->io_buffer_bytes};
  err = fw_fs_transaction_validate(&transaction, internal_validate_stage, (void*)&identity);
  if (err != k_ra8_ok) {
    return internal_abort(&transaction, err);
  }
  bool published = false;
  err            = fw_fs_transaction_commit(&transaction, &published);
  if ((err == k_ra8_ok) && !published) {
    err = k_ra8_err_invalid_state;
  }
  return internal_abort(&transaction, err);
}
