/**
 * @file cache_bench_host.c
 * @brief EINTR-safe raw-descriptor I/O and sibling publication.
 * @details Implements the host-only descriptor adapters behind the injected
 *          source, sink, scratch, and validated output-transaction seams.
 *
 * [Ring 7 / Tooling] {World: NS}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */
#include "cache_bench_host.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "ra8_attributes.h"

typedef enum : uint8_t {
  k_cb_host_retry_limit = 32U, /**< Bounded EINTR/temp-name retry count. */
} cb_host_retry_t;

typedef enum : uint16_t {
  k_cb_host_create_mode = 0600U, /**< Owner-only scratch/output permissions. */
} cb_host_mode_t;

/**
 * @brief Write one bounded fragment to a borrowed host descriptor.
 * @details Retries interrupted writes up to ::k_cb_host_retry_limit and
 *          reports the exact accepted prefix for ::cb_sink_write_all.
 * @param[in] ctx Pointer to the borrowed descriptor integer.
 * @param[in] data Bytes to write.
 * @param[in] length Requested byte count.
 * @param[out] out_written Receives the completed prefix length.
 * @return Tool-local I/O status.
 * @retval k_cb_io_ok The host accepted a possibly short prefix.
 * @retval k_cb_io_fault A binding check or host write failed.
 * @pre @p data is readable for @p length bytes when length is non-zero.
 * @pre @p ctx and @p out_written are non-NULL for a valid operation.
 * @post On success, @p out_written does not exceed @p length.
 * @post On failure, @p out_written is zero when it is writable.
 * @note The descriptor remains borrowed and open.
 * @since 0.1.0
 */
RA8_INTERNAL
static cb_io_status_t
internal_sink_write(void* ctx, const uint8_t* data, size_t length, size_t* out_written)
{
  if ((ctx == nullptr) || (out_written == nullptr)) {
    return k_cb_io_fault;
  }
  const int fd = *(const int*)ctx;
  for (uint8_t attempt = 0U; attempt < (uint8_t)k_cb_host_retry_limit; ++attempt) {
    const ssize_t result = write(fd, data, length);
    if (result >= 0) {
      *out_written = (size_t)result;
      return k_cb_io_ok;
    }
    if (errno != EINTR) {
      break;
    }
  }
  *out_written = 0U;
  return k_cb_io_fault;
}

void cb_host_standard_sinks(cb_sink_t* output, cb_sink_t* error)
{
  static int s_output_fd = STDOUT_FILENO;
  static int s_error_fd  = STDERR_FILENO;
  if (output != nullptr) {
    *output = (cb_sink_t){.write = internal_sink_write, .ctx = &s_output_fd};
  }
  if (error != nullptr) {
    *error = (cb_sink_t){.write = internal_sink_write, .ctx = &s_error_fd};
  }
}

/**
 * @brief Read a bounded fragment from a snapshotted host source.
 * @details Revalidates file size before each positional read and retries
 *          interrupted host calls without changing the descriptor offset.
 * @param[in] ctx Bound ::cb_host_source_t.
 * @param[in] offset Byte offset in the source snapshot.
 * @param[out] dst Destination buffer.
 * @param[in] capacity Maximum bytes to read.
 * @param[out] out_read Receives the completed prefix length.
 * @return Tool-local I/O status.
 * @retval k_cb_io_ok A possibly short read completed.
 * @retval k_cb_io_mutated The source size changed after binding.
 * @retval k_cb_io_fault A binding, metadata, or host read failed.
 * @pre @p dst is writable for @p capacity bytes.
 * @pre @p ctx and @p out_read are non-NULL and @p offset is in range.
 * @post On success, @p out_read does not exceed @p capacity.
 * @post The borrowed descriptor's sequential offset is unchanged.
 * @note File content mutation without a size change is detected by trace fingerprints.
 * @since 0.1.0
 */
RA8_INTERNAL
static cb_io_status_t
internal_source_read(void* ctx, uint64_t offset, uint8_t* dst, size_t capacity, size_t* out_read)
{
  cb_host_source_t* source = (cb_host_source_t*)ctx;
  if ((source == nullptr) || (dst == nullptr) || (out_read == nullptr) || (offset > source->size)) {
    return k_cb_io_fault;
  }
  struct stat info = {};
  if ((fstat(source->fd, &info) != 0) || (info.st_size < 0) ||
      ((uint64_t)info.st_size != source->size)) {
    return k_cb_io_mutated;
  }
  for (uint8_t attempt = 0U; attempt < (uint8_t)k_cb_host_retry_limit; ++attempt) {
    const ssize_t result = pread(source->fd, dst, capacity, (off_t)offset);
    if (result >= 0) {
      *out_read = (size_t)result;
      return k_cb_io_ok;
    }
    if (errno != EINTR) {
      break;
    }
  }
  *out_read = 0U;
  return k_cb_io_fault;
}

cb_io_status_t cb_host_source_open(const char* path, cb_host_source_t* binding, cb_source_t* source)
{
  if ((path == nullptr) || (binding == nullptr) || (source == nullptr)) {
    return k_cb_io_fault;
  }
  *binding     = (cb_host_source_t){.fd = -1};
  const int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0) {
    return k_cb_io_fault;
  }
  struct stat info = {};
  if ((fstat(fd, &info) != 0) || !S_ISREG(info.st_mode) || (info.st_size < 0)) {
    (void)close(fd);
    return k_cb_io_fault;
  }
  *binding = (cb_host_source_t){.fd = fd, .size = (uint64_t)info.st_size};
  *source  = (cb_source_t){.read = internal_source_read, .ctx = binding, .size = binding->size};
  return k_cb_io_ok;
}

cb_io_status_t cb_host_source_close(cb_host_source_t* binding)
{
  if ((binding == nullptr) || (binding->fd < 0)) {
    return k_cb_io_fault;
  }
  const int result = close(binding->fd);
  binding->fd      = -1;
  return (result == 0) ? k_cb_io_ok : k_cb_io_fault;
}

/**
 * @brief Split an output path into parent bytes and a borrowed leaf pointer.
 * @details Handles a relative leaf, an absolute root child, and an ordinary
 *          parent path without allocating or normalizing either component.
 * @param[in] path NUL-terminated destination path.
 * @param[out] parent Buffer receiving the NUL-terminated parent.
 * @param[in] parent_capacity Capacity of @p parent.
 * @param[out] base Receives a pointer to the leaf within @p path.
 * @return Tool-local I/O status.
 * @retval k_cb_io_ok Both components were published.
 * @retval k_cb_io_capacity The parent does not fit or the leaf is empty.
 * @pre @p path, @p parent, and @p base are non-NULL.
 * @pre @p path is NUL-terminated and @p parent_capacity is non-zero.
 * @post On success, @p parent is NUL-terminated and @p base is non-empty.
 * @post @p path is not modified and ownership does not change.
 * @note Symlink and file-type validation is performed by the caller's opens.
 * @since 0.1.0
 */
RA8_INTERNAL
static cb_io_status_t
internal_split_path(const char* path, char* parent, size_t parent_capacity, const char** base)
{
  const char* slash          = strrchr(path, '/');
  *base                      = (slash == nullptr) ? path : slash + 1;
  const size_t parent_length = (slash == nullptr) ? 1U : (size_t)(slash - path);
  if ((**base == '\0') || (parent_length >= parent_capacity)) {
    return k_cb_io_capacity;
  }
  size_t output_length = parent_length;
  if (slash == nullptr) {
    parent[0] = '.';
  } else if (parent_length == 0U) {
    parent[0]     = '/';
    output_length = 1U;
  } else {
    memcpy(parent, path, parent_length);
  }
  parent[output_length] = '\0';
  return k_cb_io_ok;
}

/**
 * @brief Create a uniquely-named exclusive temp file beside the destination.
 * @details Retries EEXIST collisions up to ::k_cb_host_retry_limit, each time
 *          formatting a new PID- and attempt-qualified temporary name into
 *          @p binding's temporary-name buffer before an O_EXCL create.
 * @param[in,out] binding Output binding; receives the created descriptor and
 *        the accepted temporary-name bytes on success.
 * @param[in] parent NUL-terminated parent directory path.
 * @param[in] base Borrowed leaf-name pointer within the original path.
 * @return Tool-local I/O status.
 * @retval k_cb_io_ok A new exclusive temp file was created and bound.
 * @retval k_cb_io_capacity The formatted temporary name did not fit.
 * @retval k_cb_io_fault Every retry was exhausted or a non-EEXIST error hit.
 * @pre @p binding, @p parent, and @p base are non-NULL.
 * @pre @p binding's temporary-name buffer has capacity for a candidate name.
 * @post On success, @p binding->fd is open and @p binding->transactional is true.
 * @post On failure, @p binding->fd is left as this helper set it; the caller
 *       still owns cleanup of @p binding->dir_fd.
 * @note Not thread-safe against a concurrent creator of the same destination.
 * @since 0.1.0
 */
RA8_INTERNAL
static cb_io_status_t
internal_create_temp_exclusive(cb_host_output_t* binding, const char* parent, const char* base)
{
  for (uint8_t attempt = 0U; attempt < (uint8_t)k_cb_host_retry_limit; ++attempt) {
    const int length = snprintf(binding->temporary,
                                sizeof(binding->temporary),
                                "%s/.%s.cache-bench.%ld.%u",
                                parent,
                                base,
                                (long)getpid(),
                                (unsigned int)attempt);
    if ((length < 0) || ((size_t)length >= sizeof(binding->temporary))) {
      return k_cb_io_capacity;
    }
    binding->fd = open(binding->temporary,
                       O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC,
                       (mode_t)k_cb_host_create_mode);
    if (binding->fd >= 0) {
      binding->transactional = true;
      return k_cb_io_ok;
    }
    if (errno != EEXIST) {
      break;
    }
  }
  return k_cb_io_fault;
}

cb_io_status_t cb_host_output_open(const char* path, cb_host_output_t* binding, cb_sink_t* sink)
{
  if ((binding == nullptr) || (sink == nullptr)) {
    return k_cb_io_fault;
  }
  *binding = (cb_host_output_t){.fd = -1, .dir_fd = -1};
  if (path == nullptr) {
    binding->fd = STDOUT_FILENO;
    *sink       = (cb_sink_t){.write = internal_sink_write, .ctx = &binding->fd};
    return k_cb_io_ok;
  }
  const size_t path_length = strlen(path);
  if (path_length >= sizeof(binding->destination)) {
    return k_cb_io_capacity;
  }
  memcpy(binding->destination, path, path_length + 1U);
  char           parent[k_cb_host_path_capacity];
  const char*    base   = nullptr;
  cb_io_status_t status = internal_split_path(path, parent, sizeof(parent), &base);
  if (status != k_cb_io_ok) {
    return status;
  }
  binding->dir_fd = open(parent, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (binding->dir_fd < 0) {
    return k_cb_io_fault;
  }
  status = internal_create_temp_exclusive(binding, parent, base);
  if (status == k_cb_io_ok) {
    *sink = (cb_sink_t){.write = internal_sink_write, .ctx = &binding->fd};
    return k_cb_io_ok;
  }
  cb_host_output_abort(binding);
  return status;
}

cb_io_status_t cb_host_output_commit(cb_host_output_t* binding)
{
  if ((binding == nullptr) || (binding->fd < 0)) {
    return k_cb_io_fault;
  }
  if (!binding->transactional) {
    return k_cb_io_ok;
  }
  bool ok     = fsync(binding->fd) == 0;
  ok          = (close(binding->fd) == 0) && ok;
  binding->fd = -1;
  if (ok) {
    ok = rename(binding->temporary, binding->destination) == 0;
  }
  if (ok) {
    ok = fsync(binding->dir_fd) == 0;
  }
  const bool dir_ok = close(binding->dir_fd) == 0;
  binding->dir_fd   = -1;
  if (!ok) {
    (void)unlink(binding->temporary);
  }
  binding->transactional = false;
  return (ok && dir_ok) ? k_cb_io_ok : k_cb_io_fault;
}

void cb_host_output_abort(cb_host_output_t* binding)
{
  if (binding == nullptr) {
    return;
  }
  if (binding->fd >= 0) {
    (void)close(binding->fd);
    binding->fd = -1;
  }
  if (binding->transactional) {
    (void)unlink(binding->temporary);
  }
  if (binding->dir_fd >= 0) {
    (void)close(binding->dir_fd);
    binding->dir_fd = -1;
  }
  binding->transactional = false;
}

/**
 * @brief Complete one positional scratch transfer with bounded interruption retries.
 * @details Loops over short `pread` or `pwrite` results until @p length bytes
 *          complete, resetting the interruption counter after progress.
 * @param[in] fd Open scratch descriptor.
 * @param[in] write_mode Selects write when true and read when false.
 * @param[in] offset Initial scratch offset.
 * @param[in,out] data Transfer buffer.
 * @param[in] length Exact transfer length.
 * @return Tool-local I/O status.
 * @retval k_cb_io_ok The full transfer completed.
 * @retval k_cb_io_fault Progress stopped or the retry bound was exhausted.
 * @pre @p fd is open for the selected operation.
 * @pre @p data is valid for @p length readable or writable bytes.
 * @post On success, exactly @p length bytes were transferred.
 * @post The descriptor's sequential offset is unchanged.
 * @note The scratch transaction remains open and caller-owned.
 * @since 0.1.0
 */
RA8_INTERNAL
static cb_io_status_t
internal_scratch_transfer(int fd, bool write_mode, uint64_t offset, void* data, size_t length)
{
  size_t   complete = 0U;
  uint32_t attempts = 0U;
  while (complete < length) {
    const ssize_t result =
      write_mode
        ? pwrite(fd,
                 &((const uint8_t*)data)[complete],
                 length - complete,
                 (off_t)(offset + complete))
        : pread(fd, &((uint8_t*)data)[complete], length - complete, (off_t)(offset + complete));
    if (result > 0) {
      complete += (size_t)result;
      attempts = 0U;
      continue;
    }
    if ((result < 0) && (errno == EINTR) && (attempts < (uint32_t)k_cb_host_retry_limit)) {
      attempts++;
      continue;
    }
    return k_cb_io_fault;
  }
  return k_cb_io_ok;
}

/**
 * @brief Read an exact region through a bound host scratch transaction.
 * @details Adapts the injected scratch callback to ::internal_scratch_transfer.
 * @param[in] ctx Bound ::cb_host_scratch_t.
 * @param[in] offset Scratch byte offset.
 * @param[out] data Destination buffer.
 * @param[in] length Exact byte count.
 * @return Tool-local I/O status.
 * @retval k_cb_io_ok The full region was read.
 * @retval k_cb_io_fault The transfer failed.
 * @pre @p ctx identifies an open scratch descriptor.
 * @pre @p data is writable for @p length bytes.
 * @post On success, @p data holds exactly the requested region.
 * @post The scratch binding remains open.
 * @note Distinct descriptor bindings may be read independently.
 * @since 0.1.0
 */
RA8_INTERNAL
static cb_io_status_t internal_scratch_read(void* ctx, uint64_t offset, void* data, size_t length)
{
  return internal_scratch_transfer(((cb_host_scratch_t*)ctx)->fd, false, offset, data, length);
}

/**
 * @brief Write an exact region through a bound host scratch transaction.
 * @details Adapts the injected scratch callback to ::internal_scratch_transfer.
 * @param[in] ctx Bound ::cb_host_scratch_t.
 * @param[in] offset Scratch byte offset.
 * @param[in] data Source buffer.
 * @param[in] length Exact byte count.
 * @return Tool-local I/O status.
 * @retval k_cb_io_ok The full region was written.
 * @retval k_cb_io_fault The transfer failed.
 * @pre @p ctx identifies an open scratch descriptor.
 * @pre @p data is readable for @p length bytes.
 * @post On success, the requested scratch region contains @p data.
 * @post The scratch binding remains open.
 * @note Distinct descriptor bindings may be written independently.
 * @since 0.1.0
 */
RA8_INTERNAL
static cb_io_status_t internal_scratch_write(void* ctx, uint64_t offset, void* data, size_t length)
{
  return internal_scratch_transfer(((cb_host_scratch_t*)ctx)->fd, true, offset, data, length);
}

cb_io_status_t cb_host_scratch_open(cb_host_scratch_t* binding, cb_scratch_t* scratch)
{
  if ((binding == nullptr) || (scratch == nullptr)) {
    return k_cb_io_fault;
  }
  char path[] = "/tmp/ra8-cache-bench.XXXXXX";
  binding->fd = mkstemp(path);
  if (binding->fd < 0) {
    return k_cb_io_fault;
  }
  if (unlink(path) != 0) {
    (void)close(binding->fd);
    binding->fd = -1;
    return k_cb_io_fault;
  }
  *scratch =
    (cb_scratch_t){.read = internal_scratch_read, .write = internal_scratch_write, .ctx = binding};
  return k_cb_io_ok;
}

cb_io_status_t cb_host_scratch_close(cb_host_scratch_t* binding)
{
  if ((binding == nullptr) || (binding->fd < 0)) {
    return k_cb_io_fault;
  }
  const int result = close(binding->fd);
  binding->fd      = -1;
  return (result == 0) ? k_cb_io_ok : k_cb_io_fault;
}
