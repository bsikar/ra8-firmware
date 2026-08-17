/**
 * @file fw_if_fs_posix.c
 * @brief Secure hosted POSIX backend for the portable filesystem interface.
 * @ingroup grp_io
 *
 * @par Tag
 * [Ring 4 / Host Port] {World: Host}
 *
 * @details
 * Implements portable filesystem operations relative to one opened root
 * descriptor. Component-wise no-follow traversal rejects parent escapes and
 * symlinks, while caller-owned file and transaction workspaces provide bounded
 * streaming, listing, metadata, and staged publication on hosted systems.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#ifndef _GNU_SOURCE
/** @brief Request GNU descriptor-relative syscall declarations on Linux. */
#define _GNU_SOURCE
#endif

#include "fw_if_fs_posix.h"

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

#include "ra8_attributes.h"

#if defined(__linux__) || defined(__APPLE__)
#include <sys/syscall.h>
#endif

#include "fw_if_fs.h"
#include "fw_if_fs_backend.h"
#include "fw_if_fs_posix_contracts_internal.h"
#include "fw_if_fs_posix_internal.h"
#include "ra8_err.h"

#ifndef O_CLOEXEC
/** @brief Zero fallback when the host lacks close-on-exec open flags. */
#define O_CLOEXEC (0)
#endif

#ifndef O_NOFOLLOW
/** @brief Zero fallback paired with explicit no-follow metadata validation. */
#define O_NOFOLLOW (0)
#endif

#ifndef AT_SYMLINK_NOFOLLOW
/** @brief Zero fallback paired with explicit target-type rejection. */
#define AT_SYMLINK_NOFOLLOW (0)
#endif

#ifndef RENAME_NOREPLACE
/** @brief Host flag value for atomic no-replace rename probing. */
#define RENAME_NOREPLACE (1U << 0U)
#endif

/** @brief Number of dot records plus the completion look-ahead record. */
typedef enum : uint8_t {
  k_posix_directory_budget_overhead = 3U, /**< Dot entries plus EOF look-ahead. */
} posix_directory_budget_t;

/* see header for full description */
RA8_INTERNAL static ra8_err_t internal_component_copy(const char* start, uint16_t length, char* out)
{
  if (length == 0U) {
    return k_ra8_err_invalid_size;
  }
  if (length >= (uint16_t)k_posix_component_cap) {
    return k_ra8_err_invalid_size;
  }
  for (uint16_t i = 0U; i < length; ++i) {
    out[i] = start[i];
  }
  out[length] = '\0';
  return k_ra8_ok;
}

/* see header for full description */
RA8_INTERNAL static ra8_err_t internal_intermediate_check(int parent_fd, const char* component)
{
  struct stat meta = {};
  if (fstatat(parent_fd, component, &meta, AT_SYMLINK_NOFOLLOW) != 0) {
    return priv_fs_posix_errno(errno);
  }
  if (S_ISLNK(meta.st_mode)) {
    return k_ra8_err_access_denied;
  }
  if (!S_ISDIR(meta.st_mode)) {
    return k_ra8_err_not_found;
  }
  return k_ra8_ok;
}

/* see header for full description */
RA8_INTERNAL static ra8_err_t
internal_next_component(const char** cursor, char* out_name, uint16_t* out_length)
{
  uint16_t length = 0U;
  while ((*cursor)[length] != '\0') {
    if ((*cursor)[length] == '/') {
      break;
    }
    ++length;
    if (length >= (uint16_t)k_posix_component_cap) {
      return k_ra8_err_invalid_size;
    }
  }
  const ra8_err_t copied = internal_component_copy(*cursor, length, out_name);
  if (copied != k_ra8_ok) {
    return copied;
  }
  *out_length = length;
  return k_ra8_ok;
}

/* see header for full description */
RA8_INTERNAL static ra8_err_t internal_parent_open_step(int* current, const char* name)
{
  const ra8_err_t checked = internal_intermediate_check(*current, name);
  if (checked != k_ra8_ok) {
    (void)priv_fs_posix_close_fd(current);
    return checked;
  }
  const int next = openat(*current, name, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  if (next < 0) {
    const ra8_err_t failed = priv_fs_posix_errno(errno);
    (void)priv_fs_posix_close_fd(current);
    return failed;
  }
  const ra8_err_t closed = priv_fs_posix_close_fd(current);
  if (closed != k_ra8_ok) {
    int next_owned = next;
    (void)priv_fs_posix_close_fd(&next_owned);
    return closed;
  }
  *current = next;
  return k_ra8_ok;
}

/* see header for full description */
RA8_PRIV ra8_err_t priv_fs_posix_parent_open(fw_fs_posix_state_t* state,
                                             const char*          path,
                                             int*                 out_parent_fd,
                                             char*                out_leaf)
{
  int current = dup(state->root_fd);
  if (current < 0) {
    return priv_fs_posix_errno(errno);
  }
  const char* cursor = &path[1];
  for (uint16_t component = 0U; component < (uint16_t)k_fw_fs_path_cap; ++component) {
    char            name[k_posix_component_cap];
    uint16_t        length  = 0U;
    const ra8_err_t scanned = internal_next_component(&cursor, name, &length);
    if (scanned != k_ra8_ok) {
      (void)priv_fs_posix_close_fd(&current);
      return scanned;
    }
    if (cursor[length] == '\0') {
      (void)memcpy(out_leaf, name, (size_t)length + 1U);
      *out_parent_fd = current;
      return k_ra8_ok;
    }
    const ra8_err_t stepped = internal_parent_open_step(&current, name);
    if (stepped != k_ra8_ok) {
      return stepped;
    }
    cursor = &cursor[(uint16_t)(length + 1U)];
  }
  (void)priv_fs_posix_close_fd(&current);
  return k_ra8_err_invalid_size;
}

/* see header for full description */
RA8_INTERNAL static fw_fs_node_type_t internal_node_type(mode_t mode)
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

/* see header for full description */
RA8_INTERNAL static ra8_err_t internal_native_stat(fw_fs_posix_state_t* state,
                                                   const char*          path,
                                                   struct stat*         out,
                                                   bool*                out_exists)
{
  if (path[1] == '\0') {
    if (fstat(state->root_fd, out) != 0) {
      return priv_fs_posix_errno(errno);
    }
    *out_exists = true;
    return k_ra8_ok;
  }
  int             parent_fd = -1;
  char            leaf[k_posix_component_cap];
  const ra8_err_t parent = priv_fs_posix_parent_open(state, path, &parent_fd, leaf);
  if (parent != k_ra8_ok) {
    if (parent == k_ra8_err_not_found) {
      *out_exists = false;
      return k_ra8_ok;
    }
    return parent;
  }
  const int       stat_result = fstatat(parent_fd, leaf, out, AT_SYMLINK_NOFOLLOW);
  const int       saved_errno = errno;
  const ra8_err_t closed      = priv_fs_posix_close_fd(&parent_fd);
  if (stat_result != 0) {
    if (saved_errno == ENOENT) {
      *out_exists = false;
      return k_ra8_ok;
    }
    return priv_fs_posix_errno(saved_errno);
  }
  if (closed != k_ra8_ok) {
    return closed;
  }
  *out_exists = true;
  return k_ra8_ok;
}

/* see header for full description */
RA8_INTERNAL static ra8_err_t internal_stat(void* ctx, const char* path, fw_fs_stat_t* out)
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
      priv_fs_posix_timestamp(native.st_birthtimespec.tv_sec, native.st_birthtimespec.tv_nsec);
    out->modified =
      priv_fs_posix_timestamp(native.st_mtimespec.tv_sec, native.st_mtimespec.tv_nsec);
    out->accessed =
      priv_fs_posix_timestamp(native.st_atimespec.tv_sec, native.st_atimespec.tv_nsec);
#else
    out->modified = priv_fs_posix_timestamp(native.st_mtim.tv_sec, native.st_mtim.tv_nsec);
    out->accessed = priv_fs_posix_timestamp(native.st_atim.tv_sec, native.st_atim.tv_nsec);
#endif
  }
  if (out->type == k_fw_fs_node_directory) {
    out->size_bytes = 0U;
  }
  return k_ra8_ok;
}

/* see header for full description */
RA8_INTERNAL static ra8_err_t
internal_directory_open(fw_fs_posix_state_t* state, const char* path, int* out_fd)
{
  if (path[1] == '\0') {
    *out_fd = dup(state->root_fd);
    return (*out_fd < 0) ? priv_fs_posix_errno(errno) : k_ra8_ok;
  }
  int             parent_fd = -1;
  char            leaf[k_posix_component_cap];
  const ra8_err_t parent = priv_fs_posix_parent_open(state, path, &parent_fd, leaf);
  if (parent != k_ra8_ok) {
    return parent;
  }
  const ra8_err_t checked = internal_intermediate_check(parent_fd, leaf);
  if (checked != k_ra8_ok) {
    (void)priv_fs_posix_close_fd(&parent_fd);
    return checked;
  }
  const int       opened = openat(parent_fd, leaf, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
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
  *out_fd = opened;
  return k_ra8_ok;
}

/* see header for full description */
RA8_PRIV ra8_err_t priv_fs_posix_dir_open(void*       ctx,
                                          const char* path,
                                          void*       directory_state,
                                          uint32_t    state_bytes)
{
  if (state_bytes < (uint32_t)sizeof(posix_directory_state_t)) {
    return k_ra8_err_no_mem;
  }
  posix_directory_state_t* directory = (posix_directory_state_t*)directory_state;
  *directory                         = (posix_directory_state_t){.fd = -1};
  return internal_directory_open((fw_fs_posix_state_t*)ctx, path, &directory->fd);
}

/* see header for full description */
RA8_PRIV ra8_err_t priv_fs_posix_dir_next(void*                 ctx,
                                          void*                 directory_state,
                                          fw_fs_dirent_value_t* out,
                                          bool*                 out_entry)
{
  (void)ctx;
  posix_directory_state_t* directory = (posix_directory_state_t*)directory_state;
  for (uint16_t skipped = 0U; skipped < (uint16_t)k_posix_directory_budget_overhead; ++skipped) {
    posix_directory_record_t record = {};
    bool                     at_end = false;
    ra8_err_t                err =
      priv_fs_posix_directory_next(directory->fd, &directory->reader, &record, &at_end);
    if (err != k_ra8_ok) {
      *out_entry = false;
      return err;
    }
    if (at_end) {
      *out_entry = false;
      return err;
    }
    if (strcmp(record.name, ".") == 0) {
      continue;
    }
    if (strcmp(record.name, "..") == 0) {
      continue;
    }
    struct stat meta = {};
    if (fstatat(directory->fd, record.name, &meta, AT_SYMLINK_NOFOLLOW) != 0) {
      return priv_fs_posix_errno(errno);
    }
    (void)memcpy(out->name, record.name, (size_t)record.name_bytes + 1U);
    out->name_bytes = record.name_bytes;
    out->size_bytes = S_ISDIR(meta.st_mode) ? 0U : (uint64_t)meta.st_size;
    out->type       = internal_node_type(meta.st_mode);
    *out_entry      = true;
    return k_ra8_ok;
  }
  return k_ra8_err_invalid_state;
}

/* see header for full description */
RA8_PRIV ra8_err_t priv_fs_posix_dir_close(void* ctx, void* directory_state)
{
  (void)ctx;
  posix_directory_state_t* directory = (posix_directory_state_t*)directory_state;
  return priv_fs_posix_close_fd(&directory->fd);
}

/* see header for full description */
RA8_INTERNAL static ra8_err_t internal_mkdir(void* ctx, const char* path)
{
  fw_fs_posix_state_t* state     = (fw_fs_posix_state_t*)ctx;
  int                  parent_fd = -1;
  char                 leaf[k_posix_component_cap];
  const ra8_err_t      parent = priv_fs_posix_parent_open(state, path, &parent_fd, leaf);
  if (parent != k_ra8_ok) {
    return parent;
  }
  const int       result      = mkdirat(parent_fd, leaf, (mode_t)k_posix_directory_mode);
  const int       saved_errno = errno;
  const ra8_err_t closed      = priv_fs_posix_close_fd(&parent_fd);
  if (result != 0) {
    return priv_fs_posix_errno(saved_errno);
  }
  return closed;
}

/* see header for full description */
RA8_INTERNAL static ra8_err_t internal_unlink(void* ctx, const char* path)
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
  const ra8_err_t parent = priv_fs_posix_parent_open(state, path, &parent_fd, leaf);
  if (parent != k_ra8_ok) {
    return parent;
  }
  const int       result      = unlinkat(parent_fd, leaf, 0);
  const int       saved_errno = errno;
  const ra8_err_t closed      = priv_fs_posix_close_fd(&parent_fd);
  if (result != 0) {
    return priv_fs_posix_errno(saved_errno);
  }
  return closed;
}

/* see header for full description */
RA8_INTERNAL static ra8_err_t internal_rmdir(void* ctx, const char* path)
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
  const ra8_err_t parent = priv_fs_posix_parent_open(state, path, &parent_fd, leaf);
  if (parent != k_ra8_ok) {
    return parent;
  }
  const int       result      = unlinkat(parent_fd, leaf, AT_REMOVEDIR);
  const int       saved_errno = errno;
  const ra8_err_t closed      = priv_fs_posix_close_fd(&parent_fd);
  if (result != 0) {
    return priv_fs_posix_errno(saved_errno);
  }
  return closed;
}

/* see header for full description */
RA8_INTERNAL static ra8_err_t
internal_rename_noreplace(int old_fd, const char* old_leaf, int new_fd, const char* new_leaf)
{
#if defined(__linux__) && defined(SYS_renameat2)
  if (syscall(SYS_renameat2, old_fd, old_leaf, new_fd, new_leaf, RENAME_NOREPLACE) == 0) {
    return k_ra8_ok;
  }
  if (errno == ENOSYS) {
    return k_ra8_err_not_supported;
  }
  return priv_fs_posix_errno(errno);
#elif defined(__APPLE__)
  if (renameatx_np(old_fd, old_leaf, new_fd, new_leaf, RENAME_EXCL) == 0) {
    return k_ra8_ok;
  }
  if (errno == ENOSYS) {
    return k_ra8_err_not_supported;
  }
  return priv_fs_posix_errno(errno);
#else
  (void)old_fd;
  (void)old_leaf;
  (void)new_fd;
  (void)new_leaf;
  return k_ra8_err_not_supported;
#endif
}

/**
 * @brief Validate opened parents and perform the selected rename operation.
 * @details Proves both parents occupy one filesystem, then dispatches to
 *          atomic replacement or the host's atomic no-replace primitive.
 * @param[in] old_fd Open source-parent descriptor.
 * @param[in] old_leaf Source leaf relative to @p old_fd.
 * @param[in] new_fd Open destination-parent descriptor.
 * @param[in] new_leaf Destination leaf relative to @p new_fd.
 * @param[in] replace Whether an existing destination may be replaced.
 * @return Rename status or mapped host failure.
 * @retval k_ra8_ok The entry was renamed atomically.
 * @retval k_ra8_err_invalid_arg Parent descriptors name different filesystems.
 * @retval k_ra8_err_* Metadata or rename failures are mapped unchanged.
 * @pre Both descriptors are live, owned by the caller, and name directories.
 * @pre Both leaf strings are bounded validated path components.
 * @post Success moves exactly the source entry to the destination name.
 * @post Descriptor ownership remains with the caller on every result.
 * @note No-replace mode never falls back to a racy check-then-rename sequence.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_rename_opened(int         old_fd,
                                                     const char* old_leaf,
                                                     int         new_fd,
                                                     const char* new_leaf,
                                                     bool        replace)
{
  struct stat old_parent = {};
  if (fstat(old_fd, &old_parent) != 0) {
    return priv_fs_posix_errno(errno);
  }
  struct stat new_parent = {};
  if (fstat(new_fd, &new_parent) != 0) {
    return priv_fs_posix_errno(errno);
  }
  if (old_parent.st_dev != new_parent.st_dev) {
    return k_ra8_err_invalid_arg;
  }
  if (!replace) {
    return internal_rename_noreplace(old_fd, old_leaf, new_fd, new_leaf);
  }
  if (renameat(old_fd, old_leaf, new_fd, new_leaf) != 0) {
    return priv_fs_posix_errno(errno);
  }
  return k_ra8_ok;
}

/* see header for full description */
RA8_INTERNAL static ra8_err_t internal_rename_validate_endpoints(fw_fs_posix_state_t* state,
                                                                 const char*          old_path,
                                                                 const char*          new_path)
{
  struct stat     source        = {};
  bool            source_exists = false;
  const ra8_err_t stated        = internal_native_stat(state, old_path, &source, &source_exists);
  if (stated != k_ra8_ok) {
    return stated;
  }
  if (!source_exists) {
    return k_ra8_err_not_found;
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
  if (destination_exists) {
    if (S_ISLNK(destination.st_mode)) {
      return k_ra8_err_access_denied;
    }
  }
  return k_ra8_ok;
}

/* see header for full description */
RA8_INTERNAL static ra8_err_t
internal_rename(void* ctx, const char* old_path, const char* new_path, bool replace)
{
  fw_fs_posix_state_t* state     = (fw_fs_posix_state_t*)ctx;
  const ra8_err_t      validated = internal_rename_validate_endpoints(state, old_path, new_path);
  if (validated != k_ra8_ok) {
    return validated;
  }
  int       old_fd = -1;
  int       new_fd = -1;
  char      old_leaf[k_posix_component_cap];
  char      new_leaf[k_posix_component_cap];
  ra8_err_t result = priv_fs_posix_parent_open(state, old_path, &old_fd, old_leaf);
  if (result == k_ra8_ok) {
    result = priv_fs_posix_parent_open(state, new_path, &new_fd, new_leaf);
  }
  if (result == k_ra8_ok) {
    result = internal_rename_opened(old_fd, old_leaf, new_fd, new_leaf, replace);
  }
  if (old_fd >= 0) {
    const ra8_err_t closed = priv_fs_posix_close_fd(&old_fd);
    if (result == k_ra8_ok) {
      result = closed;
    }
  }
  if (new_fd >= 0) {
    const ra8_err_t closed = priv_fs_posix_close_fd(&new_fd);
    if (result == k_ra8_ok) {
      result = closed;
    }
  }
  return result;
}

/* see header for full description */
RA8_INTERNAL static ra8_err_t internal_space(void* ctx, fw_fs_space_t* out)
{
  fw_fs_posix_state_t* state = (fw_fs_posix_state_t*)ctx;
  struct statvfs       space = {};
  if (fstatvfs(state->root_fd, &space) != 0) {
    return priv_fs_posix_errno(errno);
  }
  out->total_bytes = (uint64_t)space.f_blocks * (uint64_t)space.f_frsize;
  out->free_bytes  = (uint64_t)space.f_bavail * (uint64_t)space.f_frsize;
  out->used_bytes  = out->total_bytes - ((uint64_t)space.f_bfree * (uint64_t)space.f_frsize);
  return k_ra8_ok;
}

/* see header for full description */
RA8_INTERNAL static ra8_err_t internal_stage_open(fw_fs_posix_state_t*       state,
                                                  posix_transaction_state_t* txn)
{
  for (uint16_t attempt = 0U; attempt < (uint16_t)k_posix_stage_attempts; ++attempt) {
    ++state->transaction_id;
    const ra8_err_t named =
      priv_fs_posix_stage_path(txn->destination, state->transaction_id, txn->stage);
    if (named != k_ra8_ok) {
      return named;
    }
    const ra8_err_t opened = priv_fs_posix_open(state,
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

/* see header for full description */
RA8_INTERNAL static ra8_err_t internal_txn_begin(void*                      ctx,
                                                 void*                      transaction_state,
                                                 uint32_t                   state_bytes,
                                                 const char*                destination,
                                                 fw_fs_transaction_policy_t policy)
{
  if (state_bytes < sizeof(posix_transaction_state_t)) {
    return k_ra8_err_no_mem;
  }
  fw_fs_posix_state_t* state = (fw_fs_posix_state_t*)ctx;
  if (policy == k_fw_fs_txn_create_new) {
    if (!state->atomic_noreplace) {
      return k_ra8_err_not_supported;
    }
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
  if (policy == k_fw_fs_txn_create_new) {
    if (destination_stat.exists) {
      return k_ra8_err_exists;
    }
  }
  posix_transaction_state_t* txn = (posix_transaction_state_t*)transaction_state;
  (void)memset(txn, 0, sizeof(*txn));
  txn->file_state.fd     = -1;
  txn->policy            = policy;
  const ra8_err_t copied = priv_fs_posix_copy_path(txn->destination, destination);
  if (copied != k_ra8_ok) {
    return copied;
  }
  return internal_stage_open(state, txn);
}

/* see header for full description */
RA8_INTERNAL static ra8_err_t internal_txn_write(void*          ctx,
                                                 void*          transaction_state,
                                                 const uint8_t* src,
                                                 uint32_t       len,
                                                 uint32_t*      out_written)
{
  posix_transaction_state_t* txn = (posix_transaction_state_t*)transaction_state;
  if (!txn->writer_open) {
    return k_ra8_err_invalid_state;
  }
  return priv_fs_posix_write(ctx, &txn->file_state, src, len, out_written);
}

/* see header for full description */
RA8_INTERNAL static ra8_err_t internal_txn_seek(void* ctx, void* transaction_state, uint64_t offset)
{
  posix_transaction_state_t* txn = (posix_transaction_state_t*)transaction_state;
  if (!txn->writer_open) {
    return k_ra8_err_invalid_state;
  }
  uint64_t        size  = 0U;
  const ra8_err_t sized = priv_fs_posix_size(ctx, &txn->file_state, &size);
  if (sized != k_ra8_ok) {
    return sized;
  }
  if (offset > size) {
    return k_ra8_err_invalid_size;
  }
  return priv_fs_posix_seek(ctx, &txn->file_state, offset);
}

/* see header for full description */
RA8_INTERNAL static ra8_err_t internal_txn_validate(void*               ctx,
                                                    void*               transaction_state,
                                                    fw_fs_validate_fn_t validator,
                                                    void*               validator_ctx)
{
  posix_transaction_state_t* txn = (posix_transaction_state_t*)transaction_state;
  if (!txn->writer_open) {
    return k_ra8_err_invalid_state;
  }
  const ra8_err_t synced = priv_fs_posix_sync(ctx, &txn->file_state);
  if (synced != k_ra8_ok) {
    return synced;
  }
  const ra8_err_t closed = priv_fs_posix_close(ctx, &txn->file_state);
  txn->writer_open       = false;
  if (closed != k_ra8_ok) {
    return closed;
  }
  const ra8_err_t opened = priv_fs_posix_open(ctx,
                                              txn->stage,
                                              k_fw_fs_open_read,
                                              &txn->file_state,
                                              sizeof(txn->file_state));
  if (opened != k_ra8_ok) {
    return opened;
  }
  fw_fs_file_t staged = {
    .iface       = priv_fs_posix_stream_iface(),
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

/* see header for full description */
RA8_INTERNAL static ra8_err_t internal_parent_sync(fw_fs_posix_state_t* state, const char* path)
{
  int             parent_fd = -1;
  char            leaf[k_posix_component_cap];
  const ra8_err_t parent = priv_fs_posix_parent_open(state, path, &parent_fd, leaf);
  if (parent != k_ra8_ok) {
    return parent;
  }
  (void)leaf;
  ra8_err_t result = k_ra8_ok;
  if (fsync(parent_fd) != 0) {
    result = priv_fs_posix_errno(errno);
  }
  const ra8_err_t closed = priv_fs_posix_close_fd(&parent_fd);
  return (result == k_ra8_ok) ? closed : result;
}

/* see header for full description */
RA8_INTERNAL static ra8_err_t
internal_txn_commit(void* ctx, void* transaction_state, bool* out_published)
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

/* see header for full description */
RA8_INTERNAL static ra8_err_t internal_txn_abort(void* ctx, void* transaction_state)
{
  posix_transaction_state_t* txn   = (posix_transaction_state_t*)transaction_state;
  ra8_err_t                  first = k_ra8_ok;
  if (txn->writer_open) {
    first            = priv_fs_posix_close(ctx, &txn->file_state);
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
  .stat      = internal_stat,
  .listdir   = priv_fs_posix_listdir,
  .dir_open  = priv_fs_posix_dir_open,
  .dir_next  = priv_fs_posix_dir_next,
  .dir_close = priv_fs_posix_dir_close,
  .mkdir     = internal_mkdir,
  .unlink    = internal_unlink,
  .rmdir     = internal_rmdir,
  .rename    = internal_rename,
  .space     = internal_space,
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

RA8_PRIV ra8_err_t priv_fs_posix_bind_interfaces(fw_fs_t*             out,
                                                 fw_fs_posix_state_t* state,
                                                 const fw_fs_caps_t*  caps)
{
  return fw_fs_bind(out,
                    &s_namespace_iface,
                    priv_fs_posix_stream_iface(),
                    &s_transaction_iface,
                    state,
                    caps);
}
