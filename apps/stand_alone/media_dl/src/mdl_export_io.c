/**
 * @file mdl_export_io.c
 * @brief Portable source streams and validated exporter publication stages.
 * @details Adapts injected storage readers and transactions to bounded exporter
 *          callbacks while preserving exact publication and abort semantics.
 *
 * [Ring 4 / Domain] {World: NS}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */
#include <limits.h>
#include <stdio.h>
#include <string.h>

#include "mdl_export_io_internal.h"
#include "mdl_hash.h"
#include "mdl_verify.h"

/** @brief Bounded callback call count shared by archive stream adapters. */
typedef enum : uint32_t {
  k_export_io_calls = 2000001U, /**< Short-I/O progress ceiling. */
} mdl_export_io_limit_t;

/** @brief Context retained while a staged artifact is structurally checked. */
typedef struct {
  mdl_storage_t*          storage;           /**< Borrowed storage scratch.  */
  mdl_export_workspace_t* workspace;         /**< Borrowed verifier arena.   */
  ra8_mdl_format_t        format;            /**< Exact artifact format.     */
  uint64_t                extent;            /**< Exact staged extent.       */
  size_t                  export_high_water; /**< Pre-validation high-water. */
} internal_export_validation_t;

/** @brief Sequential view over one caller-owned ZIP memory member. */
typedef struct {
  const uint8_t* bytes;  /**< Borrowed immutable payload. */
  size_t         length; /**< Complete payload extent.    */
  size_t         offset; /**< Sequential read cursor.     */
} internal_export_memory_t;

/**
 * @brief Dispatch structural validation for one staged artifact
 * @details Delegates exact format and extent validation without closing or
 *          reopening the transaction-owned staged handle.
 * @param[in] ctx Immutable ::internal_export_validation_t.
 * @param[in,out] staged Open staged read handle.
 * @return Selected format validation status.
 * @retval k_ra8_ok The staged artifact is structurally valid.
 * @retval k_ra8_err_validation_failed The canonical reader rejected the stage.
 * @pre Both pointers are non-null and @p staged is exclusively owned.
 * @pre The validation context carries a live exclusive workspace.
 * @post Success proves the canonical format reader accepts the complete stage.
 * @post The staged handle remains owned by the transaction.
 * @note No publication occurs in this callback.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_validate_stage(void* ctx, fw_fs_file_t* staged)
{
  if ((ctx == nullptr) || (staged == nullptr)) {
    return k_ra8_err_invalid_arg;
  }
  internal_export_validation_t* validation = (internal_export_validation_t*)ctx;
  mdl_verify_report_t           report     = {};
  const ra8_err_t               err        = mdl_verify_open_file(validation->storage,
                                                                  validation->format,
                                                                  staged,
                                                                  validation->extent,
                                                                  validation->workspace,
                                                                  &report);
  if (validation->workspace->high_water < validation->export_high_water) {
    validation->workspace->high_water = validation->export_high_water;
  }
  return err;
}

/**
 * @brief Write a complete byte span at one staged offset
 * @details Retries bounded short writes and captures the first injected sink
 *          failure without claiming partial completion.
 * @param[in,out] output Active output transaction.
 * @param[in] offset Absolute write offset at or below the current extent.
 * @param[in] bytes Readable source bytes.
 * @param[in] length Byte count.
 * @return Canonical seek/write/progress status.
 * @retval k_ra8_ok Every byte reached the stage.
 * @retval k_ra8_fail The sink failed or made zero progress.
 * @pre @p output owns one active stage and pointers satisfy @p length.
 * @pre @p offset does not introduce a hole or overflow the extent.
 * @post Success advances the cursor and greatest written edge exactly.
 * @post Failure retains the first output error.
 * @note ZIP backfills may overwrite but may never introduce a hole.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_output_write_at(mdl_export_output_t* output,
                                                       uint64_t             offset,
                                                       const uint8_t*       bytes,
                                                       size_t               length)
{
  if ((offset > output->extent) || (length > (UINT64_MAX - offset))) {
    return k_ra8_err_invalid_size;
  }
  ra8_err_t err = k_ra8_ok;
  if (offset != output->offset) {
    err = fw_fs_transaction_seek(&output->writer.transaction, offset);
  }
  size_t done = 0U;
  while ((err == k_ra8_ok) && (done < length)) {
    if (output->writer.write_calls >= (uint32_t)k_export_io_calls) {
      return k_ra8_err_invalid_size;
    }
    const size_t   remaining = length - done;
    const uint32_t request   = (remaining > UINT32_MAX) ? UINT32_MAX : (uint32_t)remaining;
    uint32_t       written   = 0U;
    err = fw_fs_transaction_write(&output->writer.transaction, bytes + done, request, &written);
    ++output->writer.write_calls;
    if ((err == k_ra8_ok) && (written == 0U)) {
      err = k_ra8_err_invalid_state;
    }
    done += written;
  }
  output->offset = offset + done;
  if (output->offset > output->extent) {
    output->extent = output->offset;
  }
  return err;
}

ra8_err_t priv_mdl_export_output_begin(mdl_export_output_t* output,
                                       mdl_storage_t*       storage,
                                       const char*          destination,
                                       ra8_mdl_format_t     format)
{
  if ((output == nullptr) || (storage == nullptr) || (destination == nullptr) ||
      !mdl_format_is_verifiable(format)) {
    return k_ra8_err_invalid_arg;
  }
  *output             = (mdl_export_output_t){.format = format};
  const ra8_err_t err = mdl_storage_txn_begin(&output->writer, storage, destination);
  if (err != k_ra8_ok) {
    *output = (mdl_export_output_t){};
  }
  return err;
}

RA8_PRIV ra8_err_t priv_mdl_export_output_begin_new(mdl_export_output_t* output,
                                                    mdl_storage_t*       storage,
                                                    const char*          destination,
                                                    ra8_mdl_format_t     format)
{
  if ((output == nullptr) || (storage == nullptr) || (destination == nullptr) ||
      !mdl_format_is_verifiable(format)) {
    return k_ra8_err_invalid_arg;
  }
  *output             = (mdl_export_output_t){.format = format};
  const ra8_err_t err = mdl_storage_txn_begin_new(&output->writer, storage, destination);
  if (err != k_ra8_ok) {
    *output = (mdl_export_output_t){};
  }
  return err;
}

ra8_err_t
priv_mdl_export_output_write(mdl_export_output_t* output, const uint8_t* bytes, uint32_t length)
{
  if ((output == nullptr) || ((bytes == nullptr) && (length != 0U)) ||
      !output->writer.transaction.active) {
    return k_ra8_err_invalid_arg;
  }
  const ra8_err_t err = internal_output_write_at(output, output->offset, bytes, length);
  if ((err != k_ra8_ok) && (output->error == k_ra8_ok)) {
    output->error = err;
  }
  return err;
}

RA8_PRIV ra8_err_t priv_mdl_export_output_write_at(void*          opaque,
                                                   uint64_t       offset,
                                                   const uint8_t* bytes,
                                                   uint32_t       length,
                                                   uint32_t*      out_written)
{
  mdl_export_output_t* output = (mdl_export_output_t*)opaque;
  if (out_written != nullptr) {
    *out_written = 0U;
  }
  if ((output == nullptr) || (out_written == nullptr) || ((bytes == nullptr) && (length != 0U)) ||
      !output->writer.transaction.active || (output->error != k_ra8_ok)) {
    return k_ra8_err_invalid_arg;
  }
  const ra8_err_t err = internal_output_write_at(output, offset, bytes, length);
  if (err != k_ra8_ok) {
    output->error = err;
    return err;
  }
  *out_written = length;
  return k_ra8_ok;
}

size_t
priv_mdl_export_zip_write(void* opaque, mz_uint64 file_offset, const void* bytes, size_t length)
{
  mdl_export_output_t* output = (mdl_export_output_t*)opaque;
  if ((output == nullptr) || ((bytes == nullptr) && (length != 0U)) ||
      !output->writer.transaction.active || (output->error != k_ra8_ok)) {
    return 0U;
  }
  const ra8_err_t err =
    internal_output_write_at(output, (uint64_t)file_offset, (const uint8_t*)bytes, length);
  if (err != k_ra8_ok) {
    output->error = err;
    return 0U;
  }
  return length;
}

ra8_err_t priv_mdl_export_output_abort(mdl_export_output_t* output)
{
  if (output == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  const ra8_err_t err = mdl_storage_txn_abort(&output->writer);
  if (err == k_ra8_ok) {
    *output = (mdl_export_output_t){};
  }
  return err;
}

ra8_err_t priv_mdl_export_output_commit(mdl_export_output_t*    output,
                                        mdl_export_workspace_t* workspace,
                                        bool*                   out_published)
{
  if ((output == nullptr) || (workspace == nullptr) || (out_published == nullptr) ||
      !output->writer.transaction.active) {
    return k_ra8_err_invalid_arg;
  }
  *out_published = false;
  if (output->error != k_ra8_ok) {
    const ra8_err_t primary = output->error;
    const ra8_err_t aborted = priv_mdl_export_output_abort(output);
    return (aborted == k_ra8_ok) ? primary : aborted;
  }
  internal_export_validation_t validation = {.storage           = output->writer.storage,
                                             .workspace         = workspace,
                                             .format            = output->format,
                                             .extent            = output->extent,
                                             .export_high_water = workspace->high_water};
  ra8_err_t                    err        = fw_fs_transaction_validate(&output->writer.transaction,
                                                                       internal_validate_stage,
                                                                       (void*)&validation);
  if (err == k_ra8_ok) {
    err = fw_fs_transaction_commit(&output->writer.transaction, out_published);
  }
  if ((err == k_ra8_ok) && !*out_published) {
    err = k_ra8_err_invalid_state;
  }
  if ((err != k_ra8_ok) && output->writer.transaction.active) {
    const ra8_err_t aborted = fw_fs_transaction_abort(&output->writer.transaction);
    if (aborted != k_ra8_ok) {
      return aborted;
    }
  }
  if (!output->writer.transaction.active) {
    *output = (mdl_export_output_t){};
  }
  return err;
}

ra8_err_t
priv_mdl_export_source_open(mdl_export_source_t* source, mdl_storage_t* storage, const char* path)
{
  if ((source == nullptr) || (storage == nullptr) || (storage->fs == nullptr) ||
      (path == nullptr)) {
    return k_ra8_err_invalid_arg;
  }
  fw_fs_stat_t stat = {};
  ra8_err_t    err  = fw_fs_stat(&storage->fs->names, path, &stat);
  if (err != k_ra8_ok) {
    return err;
  }
  if (!stat.exists) {
    return k_ra8_err_not_found;
  }
  if (stat.type != k_fw_fs_node_file) {
    return k_ra8_err_invalid_arg;
  }
  if ((stat.size_bytes == 0U) || (stat.size_bytes > (uint64_t)k_mdl_hash_max_file_bytes)) {
    return k_ra8_err_invalid_size;
  }
  *source            = (mdl_export_source_t){.storage = storage,
                                             .size    = stat.size_bytes,
                                             .hash    = (uint64_t)k_mdl_fnv_offset};
  err                = fw_fs_open(&storage->fs->streams,
                                  path,
                                  k_fw_fs_open_read,
                                  &source->file,
                                  storage->file_workspace,
                                  storage->file_workspace_bytes);
  uint64_t open_size = 0U;
  if (err == k_ra8_ok) {
    err = fw_fs_file_size(&source->file, &open_size);
  }
  if ((err == k_ra8_ok) && (open_size != source->size)) {
    err = k_ra8_err_protocol_error;
  }
  if ((err != k_ra8_ok) && source->file.is_open) {
    (void)fw_fs_close(&source->file);
  }
  if (err != k_ra8_ok) {
    *source = (mdl_export_source_t){};
  }
  return err;
}

ra8_err_t priv_mdl_export_source_close(mdl_export_source_t* source)
{
  if (source == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  ra8_err_t err = k_ra8_ok;
  if (source->file.is_open) {
    err = fw_fs_close(&source->file);
  }
  if (err == k_ra8_ok) {
    *source = (mdl_export_source_t){};
  }
  return err;
}

size_t
priv_mdl_export_zip_read(void* opaque, mz_uint64 file_offset, void* destination, size_t capacity)
{
  mdl_export_source_t* source = (mdl_export_source_t*)opaque;
  if ((source == nullptr) || (destination == nullptr) || !source->file.is_open ||
      (source->error != k_ra8_ok) || ((uint64_t)file_offset != source->offset) ||
      (source->offset > source->size)) {
    if ((source != nullptr) && (source->error == k_ra8_ok)) {
      source->error = k_ra8_err_invalid_state;
    }
    return 0U;
  }
  const uint64_t remaining = source->size - source->offset;
  size_t         target    = capacity;
  if ((uint64_t)target > remaining) {
    target = (size_t)remaining;
  }
  size_t done = 0U;
  while ((done < target) && (source->error == k_ra8_ok)) {
    if (source->calls >= (uint32_t)k_export_io_calls) {
      source->error = k_ra8_err_invalid_size;
      break;
    }
    const size_t   left    = target - done;
    const uint32_t request = (left > UINT32_MAX) ? UINT32_MAX : (uint32_t)left;
    uint32_t       got     = 0U;
    source->error          = fw_fs_read(&source->file, (uint8_t*)destination + done, request, &got);
    ++source->calls;
    if ((source->error == k_ra8_ok) && (got == 0U)) {
      source->error = k_ra8_err_protocol_error;
    }
    source->hash = mdl_hash_bytes_seed((uint8_t*)destination + done, got, source->hash);
    source->offset += got;
    done += got;
  }
  return (source->error == k_ra8_ok) ? done : 0U;
}

ra8_err_t priv_mdl_export_source_verify_close(mdl_export_source_t* source)
{
  if ((source == nullptr) || !source->file.is_open || (source->storage == nullptr)) {
    return k_ra8_err_invalid_arg;
  }
  ra8_err_t err = source->error;
  if ((err == k_ra8_ok) && (source->offset != source->size)) {
    err = k_ra8_err_protocol_error;
  }
  uint64_t digest = 0U;
  if (err == k_ra8_ok) {
    err = fw_fs_seek(&source->file, 0U);
  }
  if (err == k_ra8_ok) {
    err = mdl_hash_stream(&source->file,
                          source->size,
                          source->storage->io_buffer,
                          source->storage->io_buffer_bytes,
                          &digest);
  }
  if ((err == k_ra8_ok) && (digest != source->hash)) {
    err = k_ra8_err_protocol_error;
  }
  const ra8_err_t closed = fw_fs_close(&source->file);
  if ((err == k_ra8_ok) && (closed != k_ra8_ok)) {
    err = closed;
  }
  *source = (mdl_export_source_t){};
  return err;
}

ra8_err_t priv_mdl_export_zip_add_file(mz_zip_archive* zip,
                                       mdl_storage_t*  storage,
                                       const char*     member,
                                       const char*     path,
                                       mz_uint         flags)
{
  mdl_export_source_t source = {};
  ra8_err_t           err    = priv_mdl_export_source_open(&source, storage, path);
  if (err != k_ra8_ok) {
    return err;
  }
  const mz_bool added = mz_zip_writer_add_read_buf_callback(zip,
                                                            member,
                                                            priv_mdl_export_zip_read,
                                                            &source,
                                                            source.size,
                                                            nullptr,
                                                            nullptr,
                                                            0U,
                                                            flags,
                                                            nullptr,
                                                            0U,
                                                            nullptr,
                                                            0U);
  if (added == MZ_FALSE) {
    const ra8_err_t source_error = source.error;
    const ra8_err_t closed       = priv_mdl_export_source_close(&source);
    if (source_error != k_ra8_ok) {
      return source_error;
    }
    return (closed == k_ra8_ok) ? k_ra8_fail : closed;
  }
  const ra8_err_t verified = priv_mdl_export_source_verify_close(&source);
  if (verified != k_ra8_ok) {
    return verified;
  }
  return k_ra8_ok;
}

/**
 * @brief Serve a sequential ZIP read from immutable caller memory
 * @details Enforces sequential bounded reads over immutable caller-owned bytes
 *          and records no ownership outside the callback context.
 * @param[in,out] opaque Bound ::internal_export_memory_t.
 * @param[in] offset Requested sequential offset.
 * @param[out] destination Destination bytes.
 * @param[in] capacity Requested byte count.
 * @return Bytes copied, or zero on contract failure/EOF.
 * @retval 0 Exact EOF or callback contract failure.
 * @pre Context and destination remain valid for the callback.
 * @pre @p offset equals the context's sequential cursor.
 * @post Success advances the exact sequential cursor.
 * @post Input bytes remain unchanged and caller-owned.
 * @note Thread-safe across distinct contexts.
 * @since 0.1.0
 */
RA8_INTERNAL static size_t
internal_memory_read(void* opaque, mz_uint64 offset, void* destination, size_t capacity)
{
  internal_export_memory_t* memory = (internal_export_memory_t*)opaque;
  if ((memory == nullptr) || (destination == nullptr) || (offset != memory->offset) ||
      (memory->offset > memory->length)) {
    return 0U;
  }
  size_t amount = memory->length - memory->offset;
  if (amount > capacity) {
    amount = capacity;
  }
  memcpy(destination, &memory->bytes[memory->offset], amount);
  memory->offset += amount;
  return amount;
}

ra8_err_t priv_mdl_export_zip_add_memory(mz_zip_archive* zip,
                                         const char*     member,
                                         const uint8_t*  bytes,
                                         size_t          length,
                                         mz_uint         flags)
{
  if ((zip == nullptr) || (member == nullptr) || ((bytes == nullptr) && (length != 0U))) {
    return k_ra8_err_invalid_arg;
  }
  internal_export_memory_t memory = {.bytes = bytes, .length = length};
  const mz_bool            added  = mz_zip_writer_add_read_buf_callback(zip,
                                                                        member,
                                                                        internal_memory_read,
                                                                        &memory,
                                                                        length,
                                                                        nullptr,
                                                                        nullptr,
                                                                        0U,
                                                                        flags,
                                                                        nullptr,
                                                                        0U,
                                                                        nullptr,
                                                                        0U);
  return ((added != MZ_FALSE) && (memory.offset == memory.length)) ? k_ra8_ok : k_ra8_fail;
}

ra8_err_t
priv_mdl_export_source_copy(mdl_export_source_t* source, mdl_export_sink_fn_t sink, void* sink_ctx)
{
  if ((source == nullptr) || (sink == nullptr) || !source->file.is_open ||
      (source->storage == nullptr)) {
    return k_ra8_err_invalid_arg;
  }
  while ((source->offset < source->size) && (source->error == k_ra8_ok)) {
    const uint64_t remaining = source->size - source->offset;
    uint32_t       request   = source->storage->io_buffer_bytes;
    if ((uint64_t)request > remaining) {
      request = (uint32_t)remaining;
    }
    const size_t got =
      priv_mdl_export_zip_read(source, source->offset, source->storage->io_buffer, request);
    if (got > 0U) {
      source->error = sink(sink_ctx, source->storage->io_buffer, (uint32_t)got);
    }
  }
  return priv_mdl_export_source_verify_close(source);
}

ra8_err_t priv_mdl_export_source_slurp(mdl_storage_t* storage,
                                       const char*    path,
                                       uint8_t*       destination,
                                       size_t         capacity,
                                       size_t*        out_length)
{
  if ((destination == nullptr) || (out_length == nullptr)) {
    return k_ra8_err_invalid_arg;
  }
  mdl_export_source_t source = {};
  ra8_err_t           err    = priv_mdl_export_source_open(&source, storage, path);
  if (err != k_ra8_ok) {
    return err;
  }
  if ((source.size > capacity) || (source.size > SIZE_MAX)) {
    (void)priv_mdl_export_source_close(&source);
    return k_ra8_err_invalid_size;
  }
  const uint64_t expected = source.size;
  const size_t   got      = priv_mdl_export_zip_read(&source, 0U, destination, (size_t)expected);
  err                     = priv_mdl_export_source_verify_close(&source);
  if ((err == k_ra8_ok) && (got != (size_t)expected)) {
    err = k_ra8_err_protocol_error;
  }
  if (err == k_ra8_ok) {
    *out_length = got;
  }
  return err;
}

ra8_err_t
priv_mdl_export_path_join(char* out, size_t capacity, const char* directory, const char* leaf)
{
  if ((out == nullptr) || (capacity == 0U) || (directory == nullptr) || (leaf == nullptr)) {
    return k_ra8_err_invalid_arg;
  }
  const char* separator = (strcmp(directory, "/") == 0) ? "" : "/";
  const int   written   = snprintf(out, capacity, "%s%s%s", directory, separator, leaf);
  return ((written >= 0) && ((size_t)written < capacity)) ? k_ra8_ok : k_ra8_err_invalid_size;
}
