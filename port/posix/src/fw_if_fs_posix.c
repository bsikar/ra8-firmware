/**
 * @file fw_if_fs_posix.c
 * @brief Secure hosted POSIX backend for the portable filesystem interface.
 * @ingroup grp_io
 *
 * @par Tag
 * [Ring 4 / Host Port] {World: Host}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#define _GNU_SOURCE

#include "fw_if_fs_posix.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#if defined(__linux__)
#include <sys/syscall.h>
#endif

#include "fw_if_fs.h"
#include "fw_if_fs_backend.h"
#include "fw_if_fs_posix_internal.h"
#include "ra8_err.h"

#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif

#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0
#endif

#ifndef AT_SYMLINK_NOFOLLOW
#define AT_SYMLINK_NOFOLLOW 0
#endif

#ifndef RENAME_NOREPLACE
#define RENAME_NOREPLACE (1U << 0U)
#endif

static const fw_fs_stream_iface_t s_stream_iface;

/** @brief Copy one bounded component into a fixed local buffer. */
static ra8_err_t internal_component_copy(const char* start, uint16_t length, char* out)
{
  if (length == 0U || length >= (uint16_t)k_posix_component_cap) {
    return k_ra8_err_invalid_size;
  }
  for (uint16_t i = 0U; i < length; ++i) {
    out[i] = start[i];
  }
  out[length] = '\0';
  return k_ra8_ok;
}

/** @brief Reject an intermediate symlink before attempting directory open. */
static ra8_err_t internal_intermediate_check(int parent_fd, const char* component)
{
  struct stat meta = {};
  if (fstatat(parent_fd, component, &meta, AT_SYMLINK_NOFOLLOW) != 0) {
    return fw_fs_posix_errno(errno);
  }
  if (S_ISLNK(meta.st_mode)) {
    return k_ra8_err_access_denied;
  }
  if (!S_ISDIR(meta.st_mode)) {
    return k_ra8_err_not_found;
  }
  return k_ra8_ok;
}

/**
 * @brief Resolve a canonical path's parent without following any symlink.
 * @post On success caller owns `*out_parent_fd` and `out_leaf` is populated.
 */
static ra8_err_t internal_parent_open(fw_fs_posix_state_t* state,
                                      const char*          path,
                                      int*                 out_parent_fd,
                                      char*                out_leaf)
{
  int current = dup(state->root_fd);
  if (current < 0) {
    return fw_fs_posix_errno(errno);
  }
  const char* cursor = &path[1];
  for (uint16_t component = 0U; component < (uint16_t)k_fw_fs_path_cap; ++component) {
    uint16_t length = 0U;
    while (cursor[length] != '\0' && cursor[length] != '/') {
      ++length;
      if (length >= (uint16_t)k_posix_component_cap) {
        (void)fw_fs_posix_close_fd(&current);
        return k_ra8_err_invalid_size;
      }
    }
    char            name[k_posix_component_cap];
    const ra8_err_t copied = internal_component_copy(cursor, length, name);
    if (copied != k_ra8_ok) {
      (void)fw_fs_posix_close_fd(&current);
      return copied;
    }
    if (cursor[length] == '\0') {
      (void)memcpy(out_leaf, name, (size_t)length + 1U);
      *out_parent_fd = current;
      return k_ra8_ok;
    }
    const ra8_err_t checked = internal_intermediate_check(current, name);
    if (checked != k_ra8_ok) {
      (void)fw_fs_posix_close_fd(&current);
      return checked;
    }
    const int next = openat(current, name, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (next < 0) {
      const ra8_err_t failed = fw_fs_posix_errno(errno);
      (void)fw_fs_posix_close_fd(&current);
      return failed;
    }
    const ra8_err_t closed = fw_fs_posix_close_fd(&current);
    if (closed != k_ra8_ok) {
      int next_owned = next;
      (void)fw_fs_posix_close_fd(&next_owned);
      return closed;
    }
    current = next;
    cursor  = &cursor[(uint16_t)(length + 1U)];
  }
  (void)fw_fs_posix_close_fd(&current);
  return k_ra8_err_invalid_size;
}

/** @brief Convert a POSIX mode into a portable node kind. */
static fw_fs_node_type_t internal_node_type(mode_t mode)
{
  if (S_ISREG(mode)) {
    return k_fw_fs_node_file;
  }
  if (S_ISDIR(mode)) {
    return k_fw_fs_node_directory;
  }
  if (S_ISLNK(mode)) {
    return k_fw_fs_node_symlink;
  }
  return k_fw_fs_node_other;
}

/** @brief Stat without following the final component. */
static ra8_err_t internal_native_stat(fw_fs_posix_state_t* state,
                                      const char*          path,
                                      struct stat*         out,
                                      bool*                out_exists)
{
  if (path[1] == '\0') {
    if (fstat(state->root_fd, out) != 0) {
      return fw_fs_posix_errno(errno);
    }
    *out_exists = true;
    return k_ra8_ok;
  }
  int             parent_fd = -1;
  char            leaf[k_posix_component_cap];
  const ra8_err_t parent = internal_parent_open(state, path, &parent_fd, leaf);
  if (parent != k_ra8_ok) {
    if (parent == k_ra8_err_not_found) {
      *out_exists = false;
      return k_ra8_ok;
    }
    return parent;
  }
  const int       stat_result = fstatat(parent_fd, leaf, out, AT_SYMLINK_NOFOLLOW);
  const int       saved_errno = errno;
  const ra8_err_t closed      = fw_fs_posix_close_fd(&parent_fd);
  if (stat_result != 0) {
    if (saved_errno == ENOENT) {
      *out_exists = false;
      return k_ra8_ok;
    }
    return fw_fs_posix_errno(saved_errno);
  }
  if (closed != k_ra8_ok) {
    return closed;
  }
  *out_exists = true;
  return k_ra8_ok;
}

/** @brief Portable stat over the confined root. */
static ra8_err_t internal_stat(void* ctx, const char* path, fw_fs_stat_t* out)
{
  fw_fs_posix_state_t* state  = (fw_fs_posix_state_t*)ctx;
  struct stat          native = {};
  bool                 exists = false;
  const ra8_err_t      result = internal_native_stat(state, path, &native, &exists);
  if (result != k_ra8_ok) {
    return result;
  }
  out->exists     = exists;
  out->size_bytes = exists ? (uint64_t)native.st_size : 0U;
  out->type       = exists ? internal_node_type(native.st_mode) : k_fw_fs_node_none;
  if (exists) {
#if defined(__APPLE__)
    out->created =
      fw_fs_posix_timestamp(native.st_birthtimespec.tv_sec, native.st_birthtimespec.tv_nsec);
    out->modified = fw_fs_posix_timestamp(native.st_mtimespec.tv_sec, native.st_mtimespec.tv_nsec);
    out->accessed = fw_fs_posix_timestamp(native.st_atimespec.tv_sec, native.st_atimespec.tv_nsec);
#else
    out->modified = fw_fs_posix_timestamp(native.st_mtim.tv_sec, native.st_mtim.tv_nsec);
    out->accessed = fw_fs_posix_timestamp(native.st_atim.tv_sec, native.st_atim.tv_nsec);
#endif
  }
  if (out->type == k_fw_fs_node_directory) {
    out->size_bytes = 0U;
  }
  return k_ra8_ok;
}

/** @brief Open a directory without following its final component. */
static ra8_err_t internal_directory_open(fw_fs_posix_state_t* state, const char* path, int* out_fd)
{
  if (path[1] == '\0') {
    *out_fd = dup(state->root_fd);
    return (*out_fd < 0) ? fw_fs_posix_errno(errno) : k_ra8_ok;
  }
  int             parent_fd = -1;
  char            leaf[k_posix_component_cap];
  const ra8_err_t parent = internal_parent_open(state, path, &parent_fd, leaf);
  if (parent != k_ra8_ok) {
    return parent;
  }
  const ra8_err_t checked = internal_intermediate_check(parent_fd, leaf);
  if (checked != k_ra8_ok) {
    (void)fw_fs_posix_close_fd(&parent_fd);
    return checked;
  }
  const int       opened = openat(parent_fd, leaf, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  const int       saved_errno = errno;
  const ra8_err_t closed      = fw_fs_posix_close_fd(&parent_fd);
  if (opened < 0) {
    return fw_fs_posix_errno(saved_errno);
  }
  if (closed != k_ra8_ok) {
    int owned = opened;
    (void)fw_fs_posix_close_fd(&owned);
    return closed;
  }
  *out_fd = opened;
  return k_ra8_ok;
}

/** @brief Bounded POSIX directory enumeration. */
static ra8_err_t internal_listdir(void*           ctx,
                                  const char*     path,
                                  uint32_t        max_entries,
                                  fw_fs_list_fn_t callback,
                                  void*           callback_ctx,
                                  uint32_t*       out_count,
                                  bool*           out_complete)
{
  fw_fs_posix_state_t* state        = (fw_fs_posix_state_t*)ctx;
  int                  directory_fd = -1;
  const ra8_err_t      opened       = internal_directory_open(state, path, &directory_fd);
  if (opened != k_ra8_ok) {
    return opened;
  }
  DIR* directory = fdopendir(directory_fd);
  if (directory == nullptr) {
    const ra8_err_t failed = fw_fs_posix_errno(errno);
    (void)fw_fs_posix_close_fd(&directory_fd);
    return failed;
  }
  ra8_err_t result = k_ra8_ok;
  uint64_t  budget = (uint64_t)max_entries + 3U;
  *out_complete    = false;
  while (budget > 0U) {
    --budget;
    errno                 = 0;
    struct dirent* native = readdir(directory);
    if (native == nullptr) {
      result        = fw_fs_posix_errno(errno);
      *out_complete = (result == k_ra8_ok);
      break;
    }
    if (strcmp(native->d_name, ".") == 0 || strcmp(native->d_name, "..") == 0) {
      continue;
    }
    if (*out_count >= max_entries) {
      break;
    }
    struct stat meta = {};
    if (fstatat(dirfd(directory), native->d_name, &meta, AT_SYMLINK_NOFOLLOW) != 0) {
      result = fw_fs_posix_errno(errno);
      break;
    }
    const size_t name_len = strnlen(native->d_name, (size_t)k_posix_component_cap);
    if (name_len >= (size_t)k_posix_component_cap) {
      result = k_ra8_err_invalid_size;
      break;
    }
    const fw_fs_dirent_t entry = {
      .name       = native->d_name,
      .size_bytes = S_ISDIR(meta.st_mode) ? 0U : (uint64_t)meta.st_size,
      .name_bytes = (uint16_t)name_len,
      .type       = internal_node_type(meta.st_mode),
    };
    bool keep_going = true;
    result          = callback(callback_ctx, &entry, &keep_going);
    ++(*out_count);
    if (result != k_ra8_ok || !keep_going) {
      break;
    }
  }
  if (closedir(directory) != 0 && result == k_ra8_ok) {
    result = fw_fs_posix_errno(errno);
  }
  return result;
}

/** @brief Create one directory with no implicit parent creation. */
static ra8_err_t internal_mkdir(void* ctx, const char* path)
{
  fw_fs_posix_state_t* state     = (fw_fs_posix_state_t*)ctx;
  int                  parent_fd = -1;
  char                 leaf[k_posix_component_cap];
  const ra8_err_t      parent = internal_parent_open(state, path, &parent_fd, leaf);
  if (parent != k_ra8_ok) {
    return parent;
  }
  const int       result      = mkdirat(parent_fd, leaf, 0700);
  const int       saved_errno = errno;
  const ra8_err_t closed      = fw_fs_posix_close_fd(&parent_fd);
  if (result != 0) {
    return fw_fs_posix_errno(saved_errno);
  }
  return closed;
}

/** @brief Remove a file while refusing directories and symbolic links. */
static ra8_err_t internal_unlink(void* ctx, const char* path)
{
  fw_fs_posix_state_t* state  = (fw_fs_posix_state_t*)ctx;
  struct stat          meta   = {};
  bool                 exists = false;
  const ra8_err_t      stated = internal_native_stat(state, path, &meta, &exists);
  if (stated != k_ra8_ok) {
    return stated;
  }
  if (!exists) {
    return k_ra8_err_not_found;
  }
  if (S_ISLNK(meta.st_mode)) {
    return k_ra8_err_access_denied;
  }
  if (!S_ISREG(meta.st_mode)) {
    return k_ra8_err_invalid_arg;
  }
  int             parent_fd = -1;
  char            leaf[k_posix_component_cap];
  const ra8_err_t parent = internal_parent_open(state, path, &parent_fd, leaf);
  if (parent != k_ra8_ok) {
    return parent;
  }
  const int       result      = unlinkat(parent_fd, leaf, 0);
  const int       saved_errno = errno;
  const ra8_err_t closed      = fw_fs_posix_close_fd(&parent_fd);
  if (result != 0) {
    return fw_fs_posix_errno(saved_errno);
  }
  return closed;
}

/** @brief Remove one empty real directory. */
static ra8_err_t internal_rmdir(void* ctx, const char* path)
{
  fw_fs_posix_state_t* state  = (fw_fs_posix_state_t*)ctx;
  struct stat          meta   = {};
  bool                 exists = false;
  const ra8_err_t      stated = internal_native_stat(state, path, &meta, &exists);
  if (stated != k_ra8_ok) {
    return stated;
  }
  if (!exists) {
    return k_ra8_err_not_found;
  }
  if (S_ISLNK(meta.st_mode)) {
    return k_ra8_err_access_denied;
  }
  if (!S_ISDIR(meta.st_mode)) {
    return k_ra8_err_invalid_arg;
  }
  int             parent_fd = -1;
  char            leaf[k_posix_component_cap];
  const ra8_err_t parent = internal_parent_open(state, path, &parent_fd, leaf);
  if (parent != k_ra8_ok) {
    return parent;
  }
  const int       result      = unlinkat(parent_fd, leaf, AT_REMOVEDIR);
  const int       saved_errno = errno;
  const ra8_err_t closed      = fw_fs_posix_close_fd(&parent_fd);
  if (result != 0) {
    return fw_fs_posix_errno(saved_errno);
  }
  return closed;
}

/** @brief Linux atomic no-replace rename; never substitute a TOCTOU fallback. */
static ra8_err_t
internal_rename_noreplace(int old_fd, const char* old_leaf, int new_fd, const char* new_leaf)
{
#if defined(__linux__) && defined(SYS_renameat2)
  if (syscall(SYS_renameat2, old_fd, old_leaf, new_fd, new_leaf, RENAME_NOREPLACE) == 0) {
    return k_ra8_ok;
  }
  if (errno == ENOSYS) {
    return k_ra8_err_not_supported;
  }
  return fw_fs_posix_errno(errno);
#elif defined(__APPLE__)
  if (renameatx_np(old_fd, old_leaf, new_fd, new_leaf, RENAME_EXCL) == 0) {
    return k_ra8_ok;
  }
  if (errno == ENOSYS) {
    return k_ra8_err_not_supported;
  }
  return fw_fs_posix_errno(errno);
#else
  (void)old_fd;
  (void)old_leaf;
  (void)new_fd;
  (void)new_leaf;
  return k_ra8_err_not_supported;
#endif
}

/** @brief Probe the host atomic no-replace primitive without touching a name. */
static bool internal_atomic_noreplace_available(void)
{
#if defined(__linux__) && defined(SYS_renameat2)
  errno             = 0;
  const long result = syscall(SYS_renameat2, -1, "x", -1, "y", RENAME_NOREPLACE);
  return result == -1L && errno == EBADF;
#elif defined(__APPLE__)
  errno            = 0;
  const int result = renameatx_np(-1, "x", -1, "y", RENAME_EXCL);
  return result == -1 && errno == EBADF;
#else
  return false;
#endif
}

/** @brief Rename within the selected root, never following either leaf. */
static ra8_err_t
internal_rename(void* ctx, const char* old_path, const char* new_path, bool replace)
{
  fw_fs_posix_state_t* state         = (fw_fs_posix_state_t*)ctx;
  struct stat          source        = {};
  bool                 source_exists = false;
  const ra8_err_t      stated = internal_native_stat(state, old_path, &source, &source_exists);
  if (stated != k_ra8_ok || !source_exists) {
    return (stated == k_ra8_ok) ? k_ra8_err_not_found : stated;
  }
  if (S_ISLNK(source.st_mode)) {
    return k_ra8_err_access_denied;
  }
  struct stat     destination        = {};
  bool            destination_exists = false;
  const ra8_err_t destination_stat =
    internal_native_stat(state, new_path, &destination, &destination_exists);
  if (destination_stat != k_ra8_ok) {
    return destination_stat;
  }
  if (destination_exists && S_ISLNK(destination.st_mode)) {
    return k_ra8_err_access_denied;
  }
  int       old_fd = -1;
  int       new_fd = -1;
  char      old_leaf[k_posix_component_cap];
  char      new_leaf[k_posix_component_cap];
  ra8_err_t result = internal_parent_open(state, old_path, &old_fd, old_leaf);
  if (result == k_ra8_ok) {
    result = internal_parent_open(state, new_path, &new_fd, new_leaf);
  }
  struct stat old_parent = {};
  struct stat new_parent = {};
  if (result == k_ra8_ok && (fstat(old_fd, &old_parent) != 0 || fstat(new_fd, &new_parent) != 0)) {
    result = fw_fs_posix_errno(errno);
  }
  if (result == k_ra8_ok && old_parent.st_dev != new_parent.st_dev) {
    result = k_ra8_err_invalid_arg;
  }
  if (result == k_ra8_ok && replace && renameat(old_fd, old_leaf, new_fd, new_leaf) != 0) {
    result = fw_fs_posix_errno(errno);
  } else if (result == k_ra8_ok && !replace) {
    result = internal_rename_noreplace(old_fd, old_leaf, new_fd, new_leaf);
  }
  if (old_fd >= 0) {
    const ra8_err_t closed = fw_fs_posix_close_fd(&old_fd);
    if (result == k_ra8_ok) {
      result = closed;
    }
  }
  if (new_fd >= 0) {
    const ra8_err_t closed = fw_fs_posix_close_fd(&new_fd);
    if (result == k_ra8_ok) {
      result = closed;
    }
  }
  return result;
}

/** @brief Query volume byte totals with `fstatvfs`. */
static ra8_err_t internal_space(void* ctx, fw_fs_space_t* out)
{
  fw_fs_posix_state_t* state = (fw_fs_posix_state_t*)ctx;
  struct statvfs       space = {};
  if (fstatvfs(state->root_fd, &space) != 0) {
    return fw_fs_posix_errno(errno);
  }
  out->total_bytes = (uint64_t)space.f_blocks * (uint64_t)space.f_frsize;
  out->free_bytes  = (uint64_t)space.f_bavail * (uint64_t)space.f_frsize;
  out->used_bytes  = out->total_bytes - ((uint64_t)space.f_bfree * (uint64_t)space.f_frsize);
  return k_ra8_ok;
}

/** @brief Map a portable mode to no-follow `openat` flags. */
static ra8_err_t internal_open_flags(fw_fs_open_mode_t mode, int* out_flags)
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

/** @brief Open one regular file into caller workspace. */
static ra8_err_t internal_open(void*             ctx,
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
  const ra8_err_t      parent = internal_parent_open(state, path, &parent_fd, leaf);
  if (parent != k_ra8_ok) {
    return parent;
  }
  const int       opened      = openat(parent_fd, leaf, flags, 0600);
  const int       saved_errno = errno;
  const ra8_err_t closed      = fw_fs_posix_close_fd(&parent_fd);
  if (opened < 0) {
    return fw_fs_posix_errno(saved_errno);
  }
  if (closed != k_ra8_ok) {
    int owned = opened;
    (void)fw_fs_posix_close_fd(&owned);
    return closed;
  }
  struct stat meta = {};
  if (fstat(opened, &meta) != 0 || !S_ISREG(meta.st_mode)) {
    int owned = opened;
    (void)fw_fs_posix_close_fd(&owned);
    return k_ra8_err_invalid_arg;
  }
  ((posix_file_state_t*)file_state)->fd = opened;
  return k_ra8_ok;
}

/** @brief Retry an interrupted POSIX read and expose a legitimate short read. */
static ra8_err_t
internal_read(void* ctx, void* file_state, uint8_t* dst, uint32_t cap, uint32_t* out_read)
{
  (void)ctx;
  const int fd  = ((posix_file_state_t*)file_state)->fd;
  ssize_t   got = -1;
  do {
    got = read(fd, dst, (size_t)cap);
  } while (got < 0 && errno == EINTR);
  if (got < 0) {
    return fw_fs_posix_errno(errno);
  }
  *out_read = (uint32_t)got;
  return k_ra8_ok;
}

/** @brief Loop over POSIX short writes while reporting any accepted prefix. */
static ra8_err_t
internal_write(void* ctx, void* file_state, const uint8_t* src, uint32_t len, uint32_t* out_written)
{
  (void)ctx;
  const int fd = ((posix_file_state_t*)file_state)->fd;
  while (*out_written < len) {
    const uint32_t remaining = len - *out_written;
    ssize_t        wrote     = write(fd, &src[*out_written], (size_t)remaining);
    if (wrote < 0 && errno == EINTR) {
      continue;
    }
    if (wrote < 0) {
      return fw_fs_posix_errno(errno);
    }
    if (wrote == 0) {
      return k_ra8_fail;
    }
    *out_written += (uint32_t)wrote;
  }
  return k_ra8_ok;
}

/** @brief Seek a POSIX descriptor to an unsigned absolute offset. */
static ra8_err_t internal_seek(void* ctx, void* file_state, uint64_t offset)
{
  (void)ctx;
  if (offset > (uint64_t)INT64_MAX) {
    return k_ra8_err_invalid_size;
  }
  const int fd = ((posix_file_state_t*)file_state)->fd;
  if (lseek(fd, (off_t)offset, SEEK_SET) < 0) {
    return fw_fs_posix_errno(errno);
  }
  return k_ra8_ok;
}

/** @brief Tell the current POSIX descriptor offset. */
static ra8_err_t internal_tell(void* ctx, void* file_state, uint64_t* out_offset)
{
  (void)ctx;
  const int   fd     = ((posix_file_state_t*)file_state)->fd;
  const off_t offset = lseek(fd, 0, SEEK_CUR);
  if (offset < 0) {
    return fw_fs_posix_errno(errno);
  }
  *out_offset = (uint64_t)offset;
  return k_ra8_ok;
}

/** @brief Read a POSIX descriptor's current length. */
static ra8_err_t internal_size(void* ctx, void* file_state, uint64_t* out_size)
{
  (void)ctx;
  const int   fd   = ((posix_file_state_t*)file_state)->fd;
  struct stat meta = {};
  if (fstat(fd, &meta) != 0) {
    return fw_fs_posix_errno(errno);
  }
  *out_size = (uint64_t)meta.st_size;
  return k_ra8_ok;
}

/** @brief Flush file contents and metadata through POSIX `fsync`. */
static ra8_err_t internal_sync(void* ctx, void* file_state)
{
  (void)ctx;
  const int fd = ((posix_file_state_t*)file_state)->fd;
  if (fsync(fd) != 0) {
    return fw_fs_posix_errno(errno);
  }
  return k_ra8_ok;
}

/** @brief Close one caller-owned POSIX file state. */
static ra8_err_t internal_close(void* ctx, void* file_state)
{
  (void)ctx;
  return fw_fs_posix_close_fd(&((posix_file_state_t*)file_state)->fd);
}

/** @brief Create an exclusive sibling stage after bounded collision retries. */
static ra8_err_t internal_stage_open(fw_fs_posix_state_t* state, posix_transaction_state_t* txn)
{
  for (uint16_t attempt = 0U; attempt < (uint16_t)k_posix_stage_attempts; ++attempt) {
    ++state->transaction_id;
    const ra8_err_t named =
      fw_fs_posix_stage_path(txn->destination, state->transaction_id, txn->stage);
    if (named != k_ra8_ok) {
      return named;
    }
    const ra8_err_t opened = internal_open(state,
                                           txn->stage,
                                           k_fw_fs_open_create_new,
                                           &txn->file_state,
                                           sizeof(txn->file_state));
    if (opened == k_ra8_ok) {
      txn->writer_open  = true;
      txn->stage_exists = true;
      return k_ra8_ok;
    }
    if (opened != k_ra8_err_exists) {
      return opened;
    }
  }
  return k_ra8_err_no_mem;
}

/** @brief Begin a staged POSIX create or atomic replacement. */
static ra8_err_t internal_txn_begin(void*                      ctx,
                                    void*                      transaction_state,
                                    uint32_t                   state_bytes,
                                    const char*                destination,
                                    fw_fs_transaction_policy_t policy)
{
  if (state_bytes < sizeof(posix_transaction_state_t)) {
    return k_ra8_err_no_mem;
  }
  fw_fs_posix_state_t* state = (fw_fs_posix_state_t*)ctx;
  if (policy == k_fw_fs_txn_create_new && !state->atomic_noreplace) {
    return k_ra8_err_not_supported;
  }
  fw_fs_stat_t    destination_stat = {};
  const ra8_err_t stated           = internal_stat(state, destination, &destination_stat);
  if (stated != k_ra8_ok) {
    return stated;
  }
  if (destination_stat.type == k_fw_fs_node_symlink) {
    return k_ra8_err_access_denied;
  }
  if (destination_stat.type == k_fw_fs_node_directory) {
    return k_ra8_err_invalid_arg;
  }
  if (policy == k_fw_fs_txn_create_new && destination_stat.exists) {
    return k_ra8_err_exists;
  }
  posix_transaction_state_t* txn = (posix_transaction_state_t*)transaction_state;
  (void)memset(txn, 0, sizeof(*txn));
  txn->file_state.fd     = -1;
  txn->policy            = policy;
  const ra8_err_t copied = fw_fs_posix_copy_path(txn->destination, destination);
  if (copied != k_ra8_ok) {
    return copied;
  }
  return internal_stage_open(state, txn);
}

/** @brief Append bytes to an open POSIX stage. */
static ra8_err_t internal_txn_write(void*          ctx,
                                    void*          transaction_state,
                                    const uint8_t* src,
                                    uint32_t       len,
                                    uint32_t*      out_written)
{
  posix_transaction_state_t* txn = (posix_transaction_state_t*)transaction_state;
  if (!txn->writer_open) {
    return k_ra8_err_invalid_state;
  }
  return internal_write(ctx, &txn->file_state, src, len, out_written);
}

/** @brief Seek within the open POSIX stage for header/table backfill. */
static ra8_err_t internal_txn_seek(void* ctx, void* transaction_state, uint64_t offset)
{
  posix_transaction_state_t* txn = (posix_transaction_state_t*)transaction_state;
  if (!txn->writer_open) {
    return k_ra8_err_invalid_state;
  }
  uint64_t        size  = 0U;
  const ra8_err_t sized = internal_size(ctx, &txn->file_state, &size);
  if (sized != k_ra8_ok) {
    return sized;
  }
  if (offset > size) {
    return k_ra8_err_invalid_size;
  }
  return internal_seek(ctx, &txn->file_state, offset);
}

/** @brief Durably sync, reopen read-only, and validate a POSIX stage. */
static ra8_err_t internal_txn_validate(void*               ctx,
                                       void*               transaction_state,
                                       fw_fs_validate_fn_t validator,
                                       void*               validator_ctx)
{
  posix_transaction_state_t* txn = (posix_transaction_state_t*)transaction_state;
  if (!txn->writer_open) {
    return k_ra8_err_invalid_state;
  }
  const ra8_err_t synced = internal_sync(ctx, &txn->file_state);
  if (synced != k_ra8_ok) {
    return synced;
  }
  const ra8_err_t closed = internal_close(ctx, &txn->file_state);
  txn->writer_open       = false;
  if (closed != k_ra8_ok) {
    return closed;
  }
  const ra8_err_t opened =
    internal_open(ctx, txn->stage, k_fw_fs_open_read, &txn->file_state, sizeof(txn->file_state));
  if (opened != k_ra8_ok) {
    return opened;
  }
  fw_fs_file_t staged = {
    .iface       = &s_stream_iface,
    .ctx         = ctx,
    .state       = &txn->file_state,
    .state_bytes = sizeof(txn->file_state),
    .is_open     = true,
  };
  const ra8_err_t checked = validator(validator_ctx, &staged);
  const ra8_err_t shut    = fw_fs_close(&staged);
  if (checked != k_ra8_ok) {
    return checked;
  }
  return shut;
}

/** @brief Sync the destination's containing directory after publication. */
static ra8_err_t internal_parent_sync(fw_fs_posix_state_t* state, const char* path)
{
  int             parent_fd = -1;
  char            leaf[k_posix_component_cap];
  const ra8_err_t parent = internal_parent_open(state, path, &parent_fd, leaf);
  if (parent != k_ra8_ok) {
    return parent;
  }
  (void)leaf;
  ra8_err_t result = k_ra8_ok;
  if (fsync(parent_fd) != 0) {
    result = fw_fs_posix_errno(errno);
  }
  const ra8_err_t closed = fw_fs_posix_close_fd(&parent_fd);
  return (result == k_ra8_ok) ? closed : result;
}

/** @brief Atomically publish then durably sync the destination directory. */
static ra8_err_t internal_txn_commit(void* ctx, void* transaction_state, bool* out_published)
{
  fw_fs_posix_state_t*       state = (fw_fs_posix_state_t*)ctx;
  posix_transaction_state_t* txn   = (posix_transaction_state_t*)transaction_state;
  if (txn->writer_open) {
    return k_ra8_err_invalid_state;
  }
  const bool      replace = txn->policy == k_fw_fs_txn_replace_atomic;
  const ra8_err_t renamed = internal_rename(state, txn->stage, txn->destination, replace);
  if (renamed != k_ra8_ok) {
    return renamed;
  }
  txn->stage_exists = false;
  *out_published    = true;
  return internal_parent_sync(state, txn->destination);
}

/** @brief Close and remove a POSIX stage while preserving the destination. */
static ra8_err_t internal_txn_abort(void* ctx, void* transaction_state)
{
  posix_transaction_state_t* txn   = (posix_transaction_state_t*)transaction_state;
  ra8_err_t                  first = k_ra8_ok;
  if (txn->writer_open) {
    first            = internal_close(ctx, &txn->file_state);
    txn->writer_open = false;
  }
  if (txn->stage_exists) {
    const ra8_err_t removed = internal_unlink(ctx, txn->stage);
    if (first == k_ra8_ok) {
      first = removed;
    }
    if (removed == k_ra8_ok) {
      txn->stage_exists = false;
    }
  }
  return first;
}

/** @brief Immutable POSIX namespace vtable. */
static const fw_fs_namespace_iface_t s_namespace_iface = {
  .stat    = internal_stat,
  .listdir = internal_listdir,
  .mkdir   = internal_mkdir,
  .unlink  = internal_unlink,
  .rmdir   = internal_rmdir,
  .rename  = internal_rename,
  .space   = internal_space,
};

/** @brief Immutable POSIX stream vtable. */
static const fw_fs_stream_iface_t s_stream_iface = {
  .open  = internal_open,
  .read  = internal_read,
  .write = internal_write,
  .seek  = internal_seek,
  .tell  = internal_tell,
  .size  = internal_size,
  .sync  = internal_sync,
  .close = internal_close,
};

/** @brief Immutable POSIX transaction vtable. */
static const fw_fs_transaction_iface_t s_transaction_iface = {
  .begin    = internal_txn_begin,
  .write    = internal_txn_write,
  .seek     = internal_txn_seek,
  .validate = internal_txn_validate,
  .commit   = internal_txn_commit,
  .abort    = internal_txn_abort,
};

ra8_err_t fw_fs_posix_init(fw_fs_t* out, fw_fs_posix_state_t* state, const fw_fs_posix_cfg_t* cfg)
{
  if (out == nullptr || state == nullptr || cfg == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (cfg->root_path == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (state->initialized) {
    return k_ra8_err_exists;
  }
  const int root = open(cfg->root_path, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  if (root < 0) {
    return fw_fs_posix_errno(errno);
  }
  state->root_fd          = root;
  state->transaction_id   = 0U;
  state->removable_media  = cfg->removable_media;
  state->atomic_noreplace = internal_atomic_noreplace_available();
  state->initialized      = true;
  fw_fs_caps_t caps       = {
    .max_file_bytes = (uint64_t)INT64_MAX,
    .flags = (uint32_t)k_fw_fs_cap_namespace | (uint32_t)k_fw_fs_cap_stream |
             (uint32_t)k_fw_fs_cap_space_query | (uint32_t)k_fw_fs_cap_same_volume_rename |
             (uint32_t)k_fw_fs_cap_atomic_replace | (uint32_t)k_fw_fs_cap_create_exclusive |
             (uint32_t)k_fw_fs_cap_file_sync | (uint32_t)k_fw_fs_cap_durable_file_sync |
             (uint32_t)k_fw_fs_cap_durable_directory_sync | (uint32_t)k_fw_fs_cap_transactions |
             (uint32_t)k_fw_fs_cap_symlinks | (uint32_t)k_fw_fs_cap_rejects_symlink_walk |
             (uint32_t)k_fw_fs_cap_modified_time | (uint32_t)k_fw_fs_cap_accessed_time,
    .file_workspace_bytes        = sizeof(posix_file_state_t),
    .transaction_workspace_bytes = sizeof(posix_transaction_state_t),
    .path_max_bytes              = (uint16_t)k_fw_fs_path_cap,
    .name_max_bytes              = (uint16_t)(k_posix_component_cap - 1U),
    .max_open_files              = 64U,
    .file_workspace_align        = (uint8_t)_Alignof(posix_file_state_t),
    .transaction_workspace_align = (uint8_t)_Alignof(posix_transaction_state_t),
  };
#if defined(__APPLE__)
  caps.flags |= (uint32_t)k_fw_fs_cap_created_time;
#endif
  if (state->atomic_noreplace) {
    caps.flags |= (uint32_t)k_fw_fs_cap_atomic_noreplace;
  }
  if (cfg->removable_media) {
    caps.flags |= (uint32_t)k_fw_fs_cap_removable_media;
  }
  const ra8_err_t bound =
    fw_fs_bind(out, &s_namespace_iface, &s_stream_iface, &s_transaction_iface, state, &caps);
  if (bound != k_ra8_ok) {
    (void)fw_fs_posix_deinit(state);
  }
  return bound;
}

ra8_err_t fw_fs_posix_deinit(fw_fs_posix_state_t* state)
{
  if (state == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (!state->initialized) {
    return k_ra8_err_not_initialized;
  }
  state->initialized = false;
  return fw_fs_posix_close_fd(&state->root_fd);
}
