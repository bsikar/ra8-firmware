// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

/**
 * @file firmware_pipeline_io.c
 * @brief Bounded image acquisition and hosted provider implementation.
 * @details Implements explicit single-owner cleanup transitions for every failure.
 */

#include "firmware_pipeline_io.h"

#include <stdio.h>
#include <stdlib.h>

typedef enum : uint32_t {
  k_firmware_pipeline_max_image = 16U * 1024U * 1024U, /**< Maximum input bytes. */
} firmware_pipeline_limit_t;

/**
 * @brief Adapt binary fopen to the injectable provider contract.
 * @details Opens one path for synchronous binary input and retains no path pointer.
 * @param[in] context Unused provider context.
 * @param[in] path Non-empty terminated input path.
 * @return Opaque open stream or null on failure.
 * @pre `path` is readable and terminated.
 * @post Success transfers one stream handle to the caller.
 * @note Thread-compatible under hosted stream rules.
 * @since 0.1.0
 */
static void* host_open(void* context, const char* path)
{
  (void)context;
  return fopen(path, "rb");
}

/**
 * @brief Adapt fseek to the injectable provider contract.
 * @details Repositions one hosted file without retaining arguments.
 * @param[in] context Unused provider context.
 * @param[in] file Non-null hosted file handle.
 * @param[in] offset Requested byte offset.
 * @param[in] origin Hosted seek origin.
 * @return Hosted seek result.
 * @retval 0 Reposition succeeded.
 * @retval -1 Reposition failed.
 * @pre `file` identifies an open hosted stream.
 * @pre `origin` is accepted by fseek.
 * @post Success changes the stream position.
 * @post Failure leaves ownership unchanged.
 * @note Thread-compatible under the hosted stream rules.
 * @since 0.1.0
 */
static int host_seek(void* context, void* file, long offset, int origin)
{
  (void)context;
  return fseek(file, offset, origin);
}

/**
 * @brief Adapt ftell to the injectable provider contract.
 * @details Reports the current hosted stream offset without retaining the handle.
 * @param[in] context Unused provider context.
 * @param[in] file Non-null hosted file handle.
 * @return Non-negative offset or negative failure value.
 * @retval -1 Position reporting failed.
 * @pre `file` identifies an open hosted stream.
 * @pre Its position is representable as long on success.
 * @post Stream ownership is unchanged.
 * @post No allocation is created.
 * @note Thread-compatible under the hosted stream rules.
 * @since 0.1.0
 */
static long host_tell(void* context, void* file)
{
  (void)context;
  return ftell(file);
}

/**
 * @brief Adapt malloc to the injectable provider contract.
 * @details Allocates one exact-size block and retains no caller state.
 * @param[in] context Unused provider context.
 * @param[in] size Nonzero requested byte count.
 * @return New allocation or null on failure.
 * @pre `size` is nonzero and representable by the hosted allocator.
 * @post Success transfers one allocation to the caller.
 * @note Thread-compatible under hosted allocator rules.
 * @since 0.1.0
 */
static void* host_allocate(void* context, size_t size)
{
  (void)context;
  return malloc(size);
}

/**
 * @brief Adapt fread to the injectable provider contract.
 * @details Attempts one exact byte-oriented read into caller storage.
 * @param[in] context Unused provider context.
 * @param[out] bytes Writable destination.
 * @param[in] size Requested byte count.
 * @param[in] file Non-null hosted file handle.
 * @return Number of bytes read.
 * @retval 0 No byte was read.
 * @pre `bytes` names `size` writable bytes.
 * @pre `file` identifies an open hosted stream.
 * @post At most `size` bytes are initialized.
 * @post Stream ownership is unchanged.
 * @note Thread-compatible under the hosted stream rules.
 * @since 0.1.0
 */
static size_t host_read(void* context, void* bytes, size_t size, void* file)
{
  (void)context;
  return fread(bytes, 1U, size, file);
}

/**
 * @brief Adapt fclose to the injectable provider contract.
 * @details Ends ownership of one hosted stream even when close reports failure.
 * @param[in] context Unused provider context.
 * @param[in] file Non-null hosted file handle.
 * @return Hosted close result.
 * @retval 0 Close completed without a reported error.
 * @retval -1 Close reported failure.
 * @pre `file` identifies an open hosted stream.
 * @pre The stream is owned exactly once.
 * @post The stream handle must not be used again.
 * @post No allocation ownership changes.
 * @note Thread-compatible under the hosted stream rules.
 * @since 0.1.0
 */
static int host_close(void* context, void* file)
{
  (void)context;
  return fclose(file);
}

/**
 * @brief Adapt free to the injectable provider contract.
 * @details Releases one hosted allocation and retains no pointer.
 * @param[in] context Unused provider context.
 * @param[in] bytes Allocation or null empty representation.
 * @pre `bytes` is null or was returned by host_allocate.
 * @pre The allocation has not already been released.
 * @post Any allocation is no longer owned.
 * @post No other resource changes ownership.
 * @note Thread-compatible under hosted allocator rules.
 * @since 0.1.0
 */
static void host_release(void* context, void* bytes)
{
  (void)context;
  free(bytes);
}

const firmware_pipeline_io_ops_t* firmware_pipeline_host_io(void)
{
  static const firmware_pipeline_io_ops_t ops = {
    .context  = nullptr,
    .open     = host_open,
    .seek     = host_seek,
    .tell     = host_tell,
    .allocate = host_allocate,
    .read     = host_read,
    .close    = host_close,
    .release  = host_release,
  };
  return &ops;
}

/**
 * @brief Check that every required provider callback is present.
 * @details Performs no callback and acquires no resource.
 * @param[in] ops Candidate provider table, possibly null.
 * @return Whether every required callback is non-null.
 * @retval true Provider is complete.
 * @retval false Provider is null or incomplete.
 * @pre Candidate pointer is null or readable.
 * @pre A readable table has stable callback fields for this call.
 * @post Candidate table is unchanged.
 * @post No callback is invoked.
 * @note Thread-safe; reads caller state only.
 * @since 0.1.0
 */
static bool valid_ops(const firmware_pipeline_io_ops_t* ops)
{
  return (ops != nullptr) && (ops->open != nullptr) && (ops->seek != nullptr) &&
         (ops->tell != nullptr) && (ops->allocate != nullptr) && (ops->read != nullptr) &&
         (ops->close != nullptr) && (ops->release != nullptr);
}

firmware_pipeline_io_status_t firmware_pipeline_read_image(const firmware_pipeline_io_ops_t* ops,
                                                           const char*                       path,
                                                           firmware_pipeline_image_t* out_image)
{
  if (!valid_ops(ops) || (path == nullptr) || (path[0] == '\0') || (out_image == nullptr)) {
    return k_firmware_pipeline_io_failed;
  }
  void* file = ops->open(ops->context, path);
  if (file == nullptr) {
    return k_firmware_pipeline_io_failed;
  }
  if (ops->seek(ops->context, file, 0L, SEEK_END) != 0) {
    (void)ops->close(ops->context, file);
    return k_firmware_pipeline_io_failed;
  }
  const long measured = ops->tell(ops->context, file);
  if ((measured < 0L) || ((unsigned long)measured > k_firmware_pipeline_max_image) ||
      (ops->seek(ops->context, file, 0L, SEEK_SET) != 0)) {
    (void)ops->close(ops->context, file);
    return k_firmware_pipeline_io_failed;
  }
  const size_t size  = (size_t)measured;
  void*        bytes = size == 0U ? nullptr : ops->allocate(ops->context, size);
  if ((size != 0U) && (bytes == nullptr)) {
    (void)ops->close(ops->context, file);
    return k_firmware_pipeline_io_failed;
  }
  if ((size != 0U) && (ops->read(ops->context, bytes, size, file) != size)) {
    ops->release(ops->context, bytes);
    (void)ops->close(ops->context, file);
    return k_firmware_pipeline_io_failed;
  }
  if (ops->close(ops->context, file) != 0) {
    if (bytes != nullptr) {
      ops->release(ops->context, bytes);
    }
    return k_firmware_pipeline_io_failed;
  }
  firmware_pipeline_image_t completed = {.bytes = bytes, .size = size};
  *out_image                          = completed;
  return k_firmware_pipeline_io_ok;
}

void firmware_pipeline_release_image(const firmware_pipeline_io_ops_t* ops,
                                     firmware_pipeline_image_t*        image)
{
  if ((ops == nullptr) || (image == nullptr)) {
    return;
  }
  if (image->bytes != nullptr) {
    ops->release(ops->context, image->bytes);
  }
  *image = (firmware_pipeline_image_t){};
}
