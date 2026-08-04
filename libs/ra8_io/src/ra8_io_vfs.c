/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_io_vfs.c
 * @brief VFS mount table + `"name:/path"` router over `ra8_fs`.
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details
 * A fixed table maps short names to `ra8_fs_mount_t` volumes. Path operations
 * split a `"name:/sub"` string, look the mount up, and delegate the `sub` path
 * to the matching `ra8_fs` operation. All string scans are bounded and use only
 * single-condition guards (no compound decisions, so no MC/DC vectors are due).
 */

#include "ra8_io_vfs.h"

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_fs.h"

/** @brief Module log tag. */
static const char* const s_tag = "ra8_io_vfs";

/**
 * @struct vfs_slot_t
 * @brief One mount-table entry.
 * @since 0.1.0
 */
typedef struct {
  char            name[(uint32_t)k_ra8_io_vfs_name_max]; /**< NUL-terminated mount name. */
  ra8_fs_mount_t* mount;                                 /**< Bound volume.              */
  bool            in_use;                                /**< Slot occupied flag.        */
} vfs_slot_t;

/** @brief The mount table. */
static vfs_slot_t s_table[(uint32_t)k_ra8_io_vfs_max_mounts];

/**
 * @brief Bounded equality of two mount names.
 *
 * @details Compares up to ::k_ra8_io_vfs_name_max bytes; equal names share the
 *          same terminator position.
 *
 * @param[in] a First name.
 * @param[in] b Second name.
 *
 * @return bool true when the names match.
 * @retval true  Names are equal.
 * @retval false Names differ within the bound.
 *
 * @pre `a` and `b` are NUL-terminated within the name bound.
 * @pre Neither pointer is NULL.
 * @post No state is mutated.
 * @post The return reflects only the byte comparison.
 *
 * @note Thread-safe (pure comparison).
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static bool internal_streq(const char* a, const char* b)
{
  for (uint32_t i = 0; i < (uint32_t)k_ra8_io_vfs_name_max; ++i) {
    if (a[i] != b[i]) {
      return false;
    }
    if (a[i] == '\0') {
      return true;
    }
  }
  return true;
}

/**
 * @brief Validate a mount name (non-empty, bounded, no `:` or `/`).
 *
 * @details
 * Scans the name for the terminator while rejecting reserved characters.
 *
 * @param[in] name Candidate mount name.
 *
 * @return bool true when the name is acceptable.
 * @retval true  Name is 1..15 chars with no reserved character.
 * @retval false Name is empty, too long, or contains `:` / `/`.
 *
 * @pre `name` is non-NULL.
 * @pre `name` is NUL-terminated.
 * @post No state is mutated.
 * @post The return reflects only the name contents.
 *
 * @note Thread-safe (pure scan).
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static bool internal_name_ok(const char* name)
{
  uint32_t i = 0;
  for (i = 0; i < (uint32_t)k_ra8_io_vfs_name_max; ++i) {
    const char c = name[i];
    if (c == '\0') {
      break;
    }
    if (c == ':') {
      return false;
    }
    if (c == '/') {
      return false;
    }
  }
  if (i == 0U) {
    return false;
  }
  if (i >= (uint32_t)k_ra8_io_vfs_name_max) {
    return false;
  }
  return true;
}

/**
 * @brief Copy a validated mount name into a slot.
 *
 * @details
 * Bounded copy: stops at the source NUL or the name capacity, NUL-terminating.
 *
 * @param[out] dst Destination buffer of ::k_ra8_io_vfs_name_max bytes.
 * @param[in]  src Source name (already validated).
 *
 * @return void
 *
 * @pre `src` fits in ::k_ra8_io_vfs_name_max bytes including the NUL.
 * @pre `dst` is writable for ::k_ra8_io_vfs_name_max bytes.
 * @post `dst` holds a NUL-terminated copy of `src`.
 * @post No more than the name bound is written.
 *
 * @note Thread-safe with respect to distinct buffers.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_copy_name(char* dst, const char* src)
{
  uint32_t i = 0;
  for (i = 0; i < (uint32_t)k_ra8_io_vfs_name_max - 1U; ++i) {
    dst[i] = src[i];
    if (src[i] == '\0') {
      return;
    }
  }
  dst[i] = '\0';
}

/**
 * @brief Find the mount registered under `name`.
 *
 * @param[in] name Mount name to look up.
 *
 * @return ra8_fs_mount_t* The bound volume, or NULL if not mounted.
 * @retval NULL No slot matches `name`.
 *
 * @pre `name` is NUL-terminated.
 * @pre `name` is non-NULL.
 * @post No state is mutated.
 *
 * @note Not thread-safe with respect to mount/unmount.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_fs_mount_t* internal_find(const char* name)
{
  for (uint32_t i = 0; i < (uint32_t)k_ra8_io_vfs_max_mounts; ++i) {
    if (!s_table[i].in_use) {
      continue;
    }
    if (internal_streq(s_table[i].name, name)) {
      return s_table[i].mount;
    }
  }
  return nullptr;
}

/**
 * @brief Split `"name:sub"` into the mount name and the sub-path.
 *
 * @details
 * Scans up to the name bound for the `:` separator, copying the name out.
 *
 * @param[in]  path     `"name:sub"` string.
 * @param[out] out_name Buffer of ::k_ra8_io_vfs_name_max bytes for the name.
 * @param[out] out_sub  Set to the sub-path (after the `:`).
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok              Split succeeded.
 * @retval k_ra8_err_invalid_arg No `:` within the name bound.
 *
 * @pre `path` is NUL-terminated.
 * @pre `out_name` and `out_sub` are writable.
 * @post On success `out_name` holds the name and `out_sub` points past the `:`.
 * @post On failure the outputs are unspecified.
 *
 * @note Thread-safe with respect to distinct buffers.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_split(const char* path, char* out_name, const char** out_sub)
{
  uint32_t i = 0;
  for (i = 0; i < (uint32_t)k_ra8_io_vfs_name_max; ++i) {
    const char c = path[i];
    if (c == '\0') {
      return k_ra8_err_invalid_arg;
    }
    if (c == ':') {
      break;
    }
    out_name[i] = c;
  }
  if (i >= (uint32_t)k_ra8_io_vfs_name_max) {
    return k_ra8_err_invalid_arg;
  }
  out_name[i] = '\0';
  *out_sub    = &path[i + 1U];
  return k_ra8_ok;
}

/**
 * @brief Resolve `"name:sub"` to its mount and sub-path.
 *
 * @details
 * Splits the path then looks the name up in the mount table.
 *
 * @param[in]  path      `"name:sub"` string.
 * @param[out] out_mount Set to the resolved volume.
 * @param[out] out_sub   Set to the sub-path.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok              Resolved.
 * @retval k_ra8_err_invalid_arg `path` has no `name:` prefix.
 * @retval k_ra8_err_not_found   The name is not mounted.
 *
 * @pre `path` is NUL-terminated.
 * @pre `out_mount` and `out_sub` are writable.
 * @post On success both outputs are set.
 * @post On failure the outputs are unspecified.
 *
 * @note Not thread-safe with respect to mount/unmount.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t
internal_resolve(const char* path, ra8_fs_mount_t** out_mount, const char** out_sub)
{
  char            name[(uint32_t)k_ra8_io_vfs_name_max];
  const ra8_err_t e = internal_split(path, name, out_sub);
  if (e != k_ra8_ok) {
    return e;
  }
  ra8_fs_mount_t* m = internal_find(name);
  if (m == nullptr) {
    return k_ra8_err_not_found;
  }
  *out_mount = m;
  return k_ra8_ok;
}

ra8_err_t ra8_io_vfs_init(void)
{
  for (uint32_t i = 0; i < (uint32_t)k_ra8_io_vfs_max_mounts; ++i) {
    s_table[i].in_use = false;
    s_table[i].mount  = nullptr;
  }
  return k_ra8_ok;
}

ra8_err_t ra8_io_vfs_mount(const char* name, ra8_fs_mount_t* mount)
{
  RA8_CHECK_NULL_PTR(name, s_tag, "name must not be nullptr");
  RA8_CHECK_NULL_PTR(mount, s_tag, "mount must not be nullptr");
  if (!internal_name_ok(name)) {
    return k_ra8_err_invalid_arg;
  }
  if (internal_find(name) != nullptr) {
    return k_ra8_err_exists;
  }
  for (uint32_t i = 0; i < (uint32_t)k_ra8_io_vfs_max_mounts; ++i) {
    if (s_table[i].in_use) {
      continue;
    }
    internal_copy_name(s_table[i].name, name);
    s_table[i].mount  = mount;
    s_table[i].in_use = true;
    return k_ra8_ok;
  }
  return k_ra8_err_no_mem;
}

ra8_err_t ra8_io_vfs_unmount(const char* name)
{
  RA8_CHECK_NULL_PTR(name, s_tag, "name must not be nullptr");
  for (uint32_t i = 0; i < (uint32_t)k_ra8_io_vfs_max_mounts; ++i) {
    if (!s_table[i].in_use) {
      continue;
    }
    if (internal_streq(s_table[i].name, name)) {
      s_table[i].in_use = false;
      s_table[i].mount  = nullptr;
      return k_ra8_ok;
    }
  }
  return k_ra8_err_not_found;
}

ra8_err_t ra8_io_vfs_open(const char* path, ra8_fs_mode_t mode, ra8_fs_file_t** out_file)
{
  RA8_CHECK_NULL_PTR(path, s_tag, "path must not be nullptr");
  RA8_CHECK_NULL_PTR(out_file, s_tag, "out_file must not be nullptr");
  ra8_fs_mount_t* m   = nullptr;
  const char*     sub = nullptr;
  const ra8_err_t e   = internal_resolve(path, &m, &sub);
  if (e != k_ra8_ok) {
    return e;
  }
  return ra8_fs_open(m, sub, mode, out_file);
}

ra8_err_t ra8_io_vfs_unlink(const char* path)
{
  RA8_CHECK_NULL_PTR(path, s_tag, "path must not be nullptr");
  ra8_fs_mount_t* m   = nullptr;
  const char*     sub = nullptr;
  const ra8_err_t e   = internal_resolve(path, &m, &sub);
  if (e != k_ra8_ok) {
    return e;
  }
  return ra8_fs_unlink(m, sub);
}

ra8_err_t ra8_io_vfs_rename(const char* old_path, const char* new_path)
{
  RA8_CHECK_NULL_PTR(old_path, s_tag, "old_path must not be nullptr");
  RA8_CHECK_NULL_PTR(new_path, s_tag, "new_path must not be nullptr");
  char            oname[(uint32_t)k_ra8_io_vfs_name_max];
  char            nname[(uint32_t)k_ra8_io_vfs_name_max];
  const char*     osub = nullptr;
  const char*     nsub = nullptr;
  const ra8_err_t e1   = internal_split(old_path, oname, &osub);
  if (e1 != k_ra8_ok) {
    return e1;
  }
  const ra8_err_t e2 = internal_split(new_path, nname, &nsub);
  if (e2 != k_ra8_ok) {
    return e2;
  }
  if (!internal_streq(oname, nname)) {
    return k_ra8_err_invalid_arg;
  }
  ra8_fs_mount_t* m = internal_find(oname);
  if (m == nullptr) {
    return k_ra8_err_not_found;
  }
  return ra8_fs_rename(m, osub, nsub);
}

ra8_err_t ra8_io_vfs_stat(const char* path, ra8_io_vfs_stat_t* out)
{
  RA8_CHECK_NULL_PTR(path, s_tag, "path must not be nullptr");
  RA8_CHECK_NULL_PTR(out, s_tag, "out must not be nullptr");
  *out                = (ra8_io_vfs_stat_t){};
  ra8_fs_mount_t* m   = nullptr;
  const char*     sub = nullptr;
  const ra8_err_t e   = internal_resolve(path, &m, &sub);
  if (e != k_ra8_ok) {
    return e;
  }
  ra8_fs_file_t*  f  = nullptr;
  const ra8_err_t oe = ra8_fs_open(m, sub, k_ra8_fs_mode_read, &f);
  if (oe == k_ra8_err_not_found) {
    out->exists = false;
    return k_ra8_ok;
  }
  if (oe != k_ra8_ok) {
    return oe;
  }
  uint32_t        sz = 0;
  const ra8_err_t se = ra8_fs_size(f, &sz);
  (void)ra8_fs_close(f);
  if (se != k_ra8_ok) {
    return se;
  }
  out->exists       = true;
  out->size_bytes   = sz;
  out->attr         = (uint8_t)k_ra8_fs_attr_archive;
  out->is_directory = false;
  return k_ra8_ok;
}

ra8_err_t ra8_io_vfs_listdir(const char* path, ra8_fs_listdir_cb_t cb, void* ctx)
{
  RA8_CHECK_NULL_PTR(path, s_tag, "path must not be nullptr");
  RA8_CHECK_NULL_PTR(cb, s_tag, "cb must not be nullptr");
  ra8_fs_mount_t* m   = nullptr;
  const char*     sub = nullptr;
  const ra8_err_t e   = internal_resolve(path, &m, &sub);
  if (e != k_ra8_ok) {
    return e;
  }
  return ra8_fs_listdir(m, sub, cb, ctx);
}

ra8_err_t ra8_io_vfs_mkdir(const char* path)
{
  RA8_CHECK_NULL_PTR(path, s_tag, "path must not be nullptr");
  ra8_fs_mount_t* m   = nullptr;
  const char*     sub = nullptr;
  const ra8_err_t e   = internal_resolve(path, &m, &sub);
  if (e != k_ra8_ok) {
    return e;
  }
  return ra8_fs_mkdir(m, sub);
}
