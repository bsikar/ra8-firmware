// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

/**
 * @file firmware_pipeline_io.c
 * @brief Bounded image acquisition and hosted provider implementation.
 * @details Implements explicit single-owner cleanup transitions for every failure.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>

#include "firmware_pipeline_io_internal.h"

typedef enum : uint32_t {
  k_firmware_pipeline_max_image = 16U * 1024U * 1024U, /**< Maximum input bytes. */
} firmware_pipeline_limit_t;

/**
 * @brief Adapt POSIX open to the injectable provider contract.
 * @details Opens one path for synchronous binary input and retains no path pointer.
 * @param[in] context Unused provider context.
 * @param[in] path Non-empty terminated input path.
 * @return Encoded raw descriptor or null on failure.
 * @pre `path` is readable and terminated.
 * @post Success transfers one descriptor handle to the caller.
 * @note Thread-compatible under hosted descriptor rules.
 * @since 0.1.0
 */
static void* host_open(void* context, const char* path)
{
  (void)context;
  const int descriptor = open(path, O_RDONLY);
  return descriptor < 0 ? nullptr : (void*)((intptr_t)descriptor + 1);
}

/**
 * @brief Decode the non-null opaque representation of a raw descriptor.
 * @details Reverses the offset encoding used by host_open without changing ownership.
 * @param[in] file Non-null encoded descriptor handle.
 * @return Raw non-negative descriptor.
 * @retval 0 Standard-input descriptor when encoded as one.
 * @pre `file` was returned by host_open.
 * @pre The encoded value is representable as an integer descriptor.
 * @post The handle and descriptor state are unchanged.
 * @post No resource is acquired or released.
 * @note Thread-safe; reads only its argument.
 * @since 0.1.0
 */
static int host_descriptor(void* file)
{
  return (int)((intptr_t)file - 1);
}

/**
 * @brief Adapt POSIX lseek to the injectable provider contract.
 * @details Repositions one raw descriptor without retaining arguments.
 * @param[in] context Unused provider context.
 * @param[in] file Non-null hosted file handle.
 * @param[in] offset Requested byte offset.
 * @param[in] origin Hosted seek origin.
 * @return Hosted seek result.
 * @retval 0 Reposition succeeded.
 * @retval -1 Reposition failed.
 * @pre `file` identifies an open raw descriptor.
 * @pre `origin` is accepted by lseek.
 * @post Success changes the descriptor position.
 * @post Failure leaves ownership unchanged.
 * @note Thread-compatible under hosted descriptor rules.
 * @since 0.1.0
 */
static int host_seek(void* context, void* file, long offset, int origin)
{
  (void)context;
  return lseek(host_descriptor(file), (off_t)offset, origin) < 0 ? -1 : 0;
}

/**
 * @brief Report a raw descriptor's current offset.
 * @details Uses POSIX lseek without retaining the handle.
 * @param[in] context Unused provider context.
 * @param[in] file Non-null hosted file handle.
 * @return Non-negative offset or negative failure value.
 * @retval -1 Position reporting failed.
 * @pre `file` identifies an open raw descriptor.
 * @pre Its position is representable as long on success.
 * @post Descriptor ownership is unchanged.
 * @post No allocation is created.
 * @note Thread-compatible under hosted descriptor rules.
 * @since 0.1.0
 */
static long host_tell(void* context, void* file)
{
  (void)context;
  return (long)lseek(host_descriptor(file), 0, SEEK_CUR);
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
  void* allocation = malloc(size); /* alloc-allow: bounded host file image owned by provider */
  return allocation;
}

/**
 * @brief Read an exact byte count from a raw descriptor.
 * @details Retries interrupted calls and accumulates legal short reads.
 * @param[in] context Unused provider context.
 * @param[out] bytes Writable destination.
 * @param[in] size Requested byte count.
 * @param[in] file Non-null hosted file handle.
 * @return Number of bytes read.
 * @retval 0 No byte was read.
 * @pre `bytes` names `size` writable bytes.
 * @pre `file` identifies an open raw descriptor.
 * @post At most `size` bytes are initialized.
 * @post Descriptor ownership is unchanged.
 * @note Thread-compatible under hosted descriptor rules.
 * @since 0.1.0
 */
static size_t host_read(void* context, void* bytes, size_t size, void* file)
{
  (void)context;
  size_t complete = 0U;
  while (complete < size) {
    const ssize_t count = read(host_descriptor(file), (uint8_t*)bytes + complete, size - complete);
    if (count > 0) {
      complete += (size_t)count;
    } else if ((count == 0) || (errno != EINTR)) {
      break;
    }
  }
  return complete;
}

/**
 * @brief Adapt POSIX close to the injectable provider contract.
 * @details Ends ownership of one raw descriptor even when close reports failure.
 * @param[in] context Unused provider context.
 * @param[in] file Non-null hosted file handle.
 * @return Hosted close result.
 * @retval 0 Close completed without a reported error.
 * @retval -1 Close reported failure.
 * @pre `file` identifies an open raw descriptor.
 * @pre The descriptor is owned exactly once.
 * @post The descriptor handle must not be used again.
 * @post No allocation ownership changes.
 * @note Thread-compatible under hosted descriptor rules.
 * @since 0.1.0
 */
static int host_close(void* context, void* file)
{
  (void)context;
  return close(host_descriptor(file));
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
  free(bytes); /* alloc-allow: releases the matching host-only provider allocation */
}

RA8_PRIV const firmware_pipeline_io_ops_t* priv_firmware_pipeline_host_io(void)
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

RA8_PRIV firmware_pipeline_io_status_t
priv_firmware_pipeline_read_image(const firmware_pipeline_io_ops_t* ops,
                                  const char*                       path,
                                  firmware_pipeline_image_t*        out_image)
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

RA8_PRIV void priv_firmware_pipeline_release_image(const firmware_pipeline_io_ops_t* ops,
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
