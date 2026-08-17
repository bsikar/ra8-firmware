/**
 * @file fw_if_fs_posix_stream.c
 * @brief Hosted POSIX byte-stream operations for the portable filesystem port.
 * @ingroup grp_io
 *
 * @par Tag
 * [Ring 4 / Host Port] {World: Host}
 *
 * @details
 * Owns the adapter's complete ::fw_fs_stream_iface_t implementation: portable
 * open-mode to no-follow `openat` flag mapping, interrupt-safe read and write
 * loops, absolute seek and position queries, length queries, durability, and
 * close. Every operation drives a caller-owned ::posix_file_state_t, so this
 * unit holds no mutable state and resolves no path beyond the confined parent
 * the shared no-follow resolver hands it. Namespace policy and staged
 * publication live in fw_if_fs_posix.c, which drives these same operations for
 * its transaction stage through the widened ::RA8_PRIV entry points.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#ifndef _GNU_SOURCE
/** @brief Request GNU descriptor-relative syscall declarations on Linux. */
#define _GNU_SOURCE
#endif

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <sys/stat.h>
#include <unistd.h>

#include "fw_if_fs.h"
#include "fw_if_fs_backend.h"
#include "fw_if_fs_posix_stream_contracts_internal.h"
#include "ra8_attributes.h"
#include "ra8_err.h"

#ifndef O_CLOEXEC
/** @brief Zero fallback when the host lacks close-on-exec open flags. */
#define O_CLOEXEC (0)
#endif

#ifndef O_NOFOLLOW
/** @brief Zero fallback paired with explicit no-follow metadata validation. */
#define O_NOFOLLOW (0)
#endif

/* see header for full description */
RA8_INTERNAL static ra8_err_t internal_open_flags(fw_fs_open_mode_t mode, int* out_flags)
{
  if (mode == k_fw_fs_open_read) {
    *out_flags = O_RDONLY | O_CLOEXEC | O_NOFOLLOW;
    return k_ra8_ok;
  }
  if (mode == k_fw_fs_open_write_truncate) {
    *out_flags = O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW;
    return k_ra8_ok;
  }
  if (mode == k_fw_fs_open_append) {
    *out_flags = O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC | O_NOFOLLOW;
    return k_ra8_ok;
  }
  if (mode == k_fw_fs_open_create_new) {
    *out_flags = O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW;
    return k_ra8_ok;
  }
  return k_ra8_err_invalid_arg;
}

/* see header for full description */
RA8_PRIV ra8_err_t priv_fs_posix_open(void*             ctx,
                                      const char*       path,
                                      fw_fs_open_mode_t mode,
                                      void*             file_state,
                                      uint32_t          state_bytes)
{
  if (state_bytes < sizeof(posix_file_state_t)) {
    return k_ra8_err_no_mem;
  }
  int             flags  = 0;
  const ra8_err_t mapped = internal_open_flags(mode, &flags);
  if (mapped != k_ra8_ok) {
    return mapped;
  }
  fw_fs_posix_state_t* state     = (fw_fs_posix_state_t*)ctx;
  int                  parent_fd = -1;
  char                 leaf[k_posix_component_cap];
  const ra8_err_t      parent = priv_fs_posix_parent_open(state, path, &parent_fd, leaf);
  if (parent != k_ra8_ok) {
    return parent;
  }
  const int       opened      = openat(parent_fd, leaf, flags, (mode_t)k_posix_file_mode);
  const int       saved_errno = errno;
  const ra8_err_t closed      = priv_fs_posix_close_fd(&parent_fd);
  if (opened < 0) {
    return priv_fs_posix_errno(saved_errno);
  }
  if (closed != k_ra8_ok) {
    int owned = opened;
    (void)priv_fs_posix_close_fd(&owned);
    return closed;
  }
  struct stat meta = {};
  if (fstat(opened, &meta) != 0) {
    int owned = opened;
    (void)priv_fs_posix_close_fd(&owned);
    return k_ra8_err_invalid_arg;
  }
  if (!S_ISREG(meta.st_mode)) {
    int owned = opened;
    (void)priv_fs_posix_close_fd(&owned);
    return k_ra8_err_invalid_arg;
  }
  ((posix_file_state_t*)file_state)->fd = opened;
  return k_ra8_ok;
}

/* see header for full description */
RA8_INTERNAL static ra8_err_t
internal_read(void* ctx, void* file_state, uint8_t* dst, uint32_t cap, uint32_t* out_read)
{
  (void)ctx;
  const int fd  = ((posix_file_state_t*)file_state)->fd;
  ssize_t   got = -1;
  for (;;) {
    got = read(fd, dst, (size_t)cap);
    if (got >= 0) {
      break;
    }
    if (errno != EINTR) {
      break;
    }
  }
  if (got < 0) {
    return priv_fs_posix_errno(errno);
  }
  *out_read = (uint32_t)got;
  return k_ra8_ok;
}

/* see header for full description */
RA8_PRIV ra8_err_t priv_fs_posix_write(void*          ctx,
                                       void*          file_state,
                                       const uint8_t* src,
                                       uint32_t       len,
                                       uint32_t*      out_written)
{
  (void)ctx;
  const int fd = ((posix_file_state_t*)file_state)->fd;
  while (*out_written < len) {
    const uint32_t remaining = len - *out_written;
    ssize_t        wrote     = write(fd, &src[*out_written], (size_t)remaining);
    if (wrote < 0) {
      if (errno == EINTR) {
        continue;
      }
      return priv_fs_posix_errno(errno);
    }
    if (wrote == 0) {
      return k_ra8_fail;
    }
    *out_written += (uint32_t)wrote;
  }
  return k_ra8_ok;
}

/* see header for full description */
RA8_PRIV ra8_err_t priv_fs_posix_seek(void* ctx, void* file_state, uint64_t offset)
{
  (void)ctx;
  if (offset > (uint64_t)INT64_MAX) {
    return k_ra8_err_invalid_size;
  }
  const int fd = ((posix_file_state_t*)file_state)->fd;
  if (lseek(fd, (off_t)offset, SEEK_SET) < 0) {
    return priv_fs_posix_errno(errno);
  }
  return k_ra8_ok;
}

/* see header for full description */
RA8_INTERNAL static ra8_err_t internal_tell(void* ctx, void* file_state, uint64_t* out_offset)
{
  (void)ctx;
  const int   fd     = ((posix_file_state_t*)file_state)->fd;
  const off_t offset = lseek(fd, 0, SEEK_CUR);
  if (offset < 0) {
    return priv_fs_posix_errno(errno);
  }
  *out_offset = (uint64_t)offset;
  return k_ra8_ok;
}

/* see header for full description */
RA8_PRIV ra8_err_t priv_fs_posix_size(void* ctx, void* file_state, uint64_t* out_size)
{
  (void)ctx;
  const int   fd   = ((posix_file_state_t*)file_state)->fd;
  struct stat meta = {};
  if (fstat(fd, &meta) != 0) {
    return priv_fs_posix_errno(errno);
  }
  *out_size = (uint64_t)meta.st_size;
  return k_ra8_ok;
}

/* see header for full description */
RA8_PRIV ra8_err_t priv_fs_posix_sync(void* ctx, void* file_state)
{
  (void)ctx;
  const int fd = ((posix_file_state_t*)file_state)->fd;
  if (fsync(fd) != 0) {
    return priv_fs_posix_errno(errno);
  }
  return k_ra8_ok;
}

/* see header for full description */
RA8_PRIV ra8_err_t priv_fs_posix_close(void* ctx, void* file_state)
{
  (void)ctx;
  return priv_fs_posix_close_fd(&((posix_file_state_t*)file_state)->fd);
}

/** @brief Immutable POSIX stream vtable. */
static const fw_fs_stream_iface_t s_stream_iface = {
  .open  = priv_fs_posix_open,
  .read  = internal_read,
  .write = priv_fs_posix_write,
  .seek  = priv_fs_posix_seek,
  .tell  = internal_tell,
  .size  = priv_fs_posix_size,
  .sync  = priv_fs_posix_sync,
  .close = priv_fs_posix_close,
};

/* see header for full description */
RA8_PRIV const fw_fs_stream_iface_t* priv_fs_posix_stream_iface(void)
{
  return &s_stream_iface;
}
