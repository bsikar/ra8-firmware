/**
 * @file ra8_io_vfs_namespace.c
 * @brief Path-namespace operations over the ra8_io VFS named-mount table.
 * @ingroup grp_io
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details
 * Owns every request that names an entry rather than holds one open: removal,
 * same-mount rename, metadata, directory listing, bounded directory cursors,
 * and directory creation and removal. Each call resolves `"name:sub"` to a
 * mount slot through ::priv_ra8_io_vfs_resolve, checks the mount format's
 * advertised capability explicitly, then dispatches to that format's
 * ::ra8_io_fsfmt_ops_t entry. A rename whose two paths name different mounts is
 * refused rather than emulated, because this facade performs no cross-format
 * copy. All storage is caller-owned or lives in the mount table this unit never
 * touches directly.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_fs.h"
#include "ra8_io_fsfmt.h"
#include "ra8_io_vfs.h"
#include "ra8_io_vfs_internal.h"

/** @brief Module log tag. */
static const char* const s_tag = "ra8_io_vfs_namespace";

ra8_err_t ra8_io_vfs_unlink(const char* path)
{
  RA8_CHECK_NULL_PTR(path, s_tag, "path must not be nullptr");
  vfs_slot_t* slot = nullptr;
  const char* sub  = nullptr;
  RA8_RETURN_ON_ERROR(priv_ra8_io_vfs_resolve(path, &slot, nullptr, &sub), s_tag, "resolve");
  if (slot->format->caps.read_only) {
    return k_ra8_err_not_supported;
  }
  if (slot->format->ops->unlink == nullptr) {
    return k_ra8_err_not_supported;
  }
  return slot->format->ops->unlink(slot->mount_ctx, sub);
}

/**
 * @brief Split both rename paths and require them to name the same mount.
 * @details Renaming across mounts is not a rename this facade supports (it
 *          would need a cross-format copy), so both paths must resolve to
 *          the identical mount name.
 * @param[in] old_path Full VFS path of the existing entry.
 * @param[in] new_path Full VFS path of the desired name.
 * @param[out] old_name Mount-name buffer for @p old_path, at least
 *             ::k_ra8_io_vfs_name_max bytes.
 * @param[out] new_name Mount-name buffer for @p new_path, at least
 *             ::k_ra8_io_vfs_name_max bytes.
 * @param[out] out_old_sub Sub-path within the mount for @p old_path.
 * @param[out] out_new_sub Sub-path within the mount for @p new_path.
 * @return Split-and-match status.
 * @retval k_ra8_ok Both paths split cleanly and name the same mount.
 * @retval k_ra8_err_invalid_arg The two paths name different mounts.
 * @retval other Either path failed to split.
 * @pre All six parameters are non-NULL.
 * @pre Both name buffers hold ::k_ra8_io_vfs_name_max writable bytes.
 * @post No mount table entry is modified.
 * @post On success both sub-path outputs alias the caller's input strings.
 * @note Not thread-safe; caller serializes VFS-table access.
 * @since Version 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_vfs_rename_split(const char*  old_path,
                                                        const char*  new_path,
                                                        char*        old_name,
                                                        char*        new_name,
                                                        const char** out_old_sub,
                                                        const char** out_new_sub)
{
  RA8_RETURN_ON_ERROR(priv_ra8_io_vfs_split(old_path, old_name, out_old_sub), s_tag, "old path");
  RA8_RETURN_ON_ERROR(priv_ra8_io_vfs_split(new_path, new_name, out_new_sub), s_tag, "new path");
  if (!priv_ra8_io_vfs_streq(old_name, new_name)) {
    return k_ra8_err_invalid_arg;
  }
  return k_ra8_ok;
}

ra8_err_t ra8_io_vfs_rename(const char* old_path, const char* new_path)
{
  RA8_CHECK_NULL_PTR(old_path, s_tag, "old_path must not be nullptr");
  RA8_CHECK_NULL_PTR(new_path, s_tag, "new_path must not be nullptr");
  char            old_name[(uint32_t)k_ra8_io_vfs_name_max];
  char            new_name[(uint32_t)k_ra8_io_vfs_name_max];
  const char*     old_sub = nullptr;
  const char*     new_sub = nullptr;
  const ra8_err_t split =
    internal_vfs_rename_split(old_path, new_path, old_name, new_name, &old_sub, &new_sub);
  if (split != k_ra8_ok) {
    return split;
  }
  vfs_slot_t* slot = priv_ra8_io_vfs_find(old_name, nullptr);
  if (slot == nullptr) {
    return k_ra8_err_not_found;
  }
  if (slot->format->caps.read_only) {
    return k_ra8_err_not_supported;
  }
  if (slot->format->ops->rename == nullptr) {
    return k_ra8_err_not_supported;
  }
  return slot->format->ops->rename(slot->mount_ctx, old_sub, new_sub);
}

ra8_err_t ra8_io_vfs_stat(const char* path, ra8_io_vfs_stat_t* out)
{
  RA8_CHECK_NULL_PTR(path, s_tag, "path must not be nullptr");
  RA8_CHECK_NULL_PTR(out, s_tag, "out must not be nullptr");
  *out             = (ra8_io_vfs_stat_t){};
  vfs_slot_t* slot = nullptr;
  const char* sub  = nullptr;
  RA8_RETURN_ON_ERROR(priv_ra8_io_vfs_resolve(path, &slot, nullptr, &sub), s_tag, "resolve");
  ra8_fs_stat_t   stat = {};
  const ra8_err_t e    = slot->format->ops->stat(slot->mount_ctx, sub, &stat);
  if (e == k_ra8_err_not_found) {
    return k_ra8_ok;
  }
  if (e != k_ra8_ok) {
    return e;
  }
  out->size_bytes   = stat.size_bytes;
  out->created      = stat.created;
  out->modified     = stat.modified;
  out->accessed     = stat.accessed;
  out->attr         = stat.attr;
  out->is_directory = stat.is_directory;
  out->exists       = true;
  return k_ra8_ok;
}

ra8_err_t ra8_io_vfs_listdir(const char* path, ra8_fs_listdir_cb_t cb, void* ctx)
{
  RA8_CHECK_NULL_PTR(path, s_tag, "path must not be nullptr");
  RA8_CHECK_NULL_PTR(cb, s_tag, "cb must not be nullptr");
  vfs_slot_t* slot = nullptr;
  const char* sub  = nullptr;
  RA8_RETURN_ON_ERROR(priv_ra8_io_vfs_resolve(path, &slot, nullptr, &sub), s_tag, "resolve");
  return slot->format->ops->listdir(slot->mount_ctx, sub, cb, ctx);
}

ra8_err_t ra8_io_vfs_dir_requirements(const char* path,
                                      uint32_t*   out_bytes,
                                      uint8_t*    out_align,
                                      uint16_t*   out_max_open)
{
  if ((path == nullptr) || (out_bytes == nullptr) || (out_align == nullptr) ||
      (out_max_open == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  *out_bytes               = 0U;
  *out_align               = 0U;
  *out_max_open            = 0U;
  vfs_slot_t*     slot     = nullptr;
  const char*     sub      = nullptr;
  const ra8_err_t resolved = priv_ra8_io_vfs_resolve(path, &slot, nullptr, &sub);
  (void)sub;
  if (resolved != k_ra8_ok) {
    return resolved;
  }
  if (!slot->format->caps.supports_dir_cursor) {
    return k_ra8_err_not_supported;
  }
  *out_bytes    = slot->format->caps.directory_workspace_bytes;
  *out_align    = slot->format->caps.directory_workspace_align;
  *out_max_open = slot->format->caps.max_open_directories;
  return k_ra8_ok;
}

ra8_err_t ra8_io_vfs_dir_open(const char*       path,
                              ra8_io_vfs_dir_t* directory,
                              void*             workspace,
                              uint32_t          workspace_bytes)
{
  if ((path == nullptr) || (directory == nullptr) || (workspace == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  if (directory->is_open) {
    return k_ra8_err_busy;
  }
  vfs_slot_t*     slot     = nullptr;
  const char*     sub      = nullptr;
  const ra8_err_t resolved = priv_ra8_io_vfs_resolve(path, &slot, nullptr, &sub);
  if (resolved != k_ra8_ok) {
    return resolved;
  }
  const ra8_io_fsfmt_caps_t* caps = &slot->format->caps;
  if (!caps->supports_dir_cursor) {
    return k_ra8_err_not_supported;
  }
  if (workspace_bytes < caps->directory_workspace_bytes) {
    return k_ra8_err_no_mem;
  }
  if (((uintptr_t)workspace % (uintptr_t)caps->directory_workspace_align) != 0U) {
    return k_ra8_err_invalid_arg;
  }
  const ra8_err_t opened =
    slot->format->ops->dir_open(slot->mount_ctx, sub, workspace, workspace_bytes);
  if (opened != k_ra8_ok) {
    return opened;
  }
  *directory = (ra8_io_vfs_dir_t){.format      = slot->format,
                                  .state       = workspace,
                                  .state_bytes = workspace_bytes,
                                  .is_open     = true};
  return k_ra8_ok;
}

ra8_err_t ra8_io_vfs_dir_next(ra8_io_vfs_dir_t* directory, ra8_fs_dirent_t* out, bool* out_entry)
{
  if ((directory == nullptr) || (out == nullptr) || (out_entry == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  if (!directory->is_open || (directory->format == nullptr)) {
    return k_ra8_err_invalid_state;
  }
  *out       = (ra8_fs_dirent_t){};
  *out_entry = false;
  return directory->format->ops->dir_next(directory->state, out, out_entry);
}

ra8_err_t ra8_io_vfs_dir_close(ra8_io_vfs_dir_t* directory)
{
  if (directory == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (!directory->is_open || (directory->format == nullptr)) {
    return k_ra8_err_invalid_state;
  }
  const ra8_err_t closed = directory->format->ops->dir_close(directory->state);
  *directory             = (ra8_io_vfs_dir_t){};
  return closed;
}

ra8_err_t ra8_io_vfs_mkdir(const char* path)
{
  RA8_CHECK_NULL_PTR(path, s_tag, "path must not be nullptr");
  vfs_slot_t* slot = nullptr;
  const char* sub  = nullptr;
  RA8_RETURN_ON_ERROR(priv_ra8_io_vfs_resolve(path, &slot, nullptr, &sub), s_tag, "resolve");
  if (slot->format->caps.read_only) {
    return k_ra8_err_not_supported;
  }
  if (!slot->format->caps.supports_mkdir) {
    return k_ra8_err_not_supported;
  }
  if (slot->format->ops->mkdir == nullptr) {
    return k_ra8_err_not_supported;
  }
  return slot->format->ops->mkdir(slot->mount_ctx, sub);
}

ra8_err_t ra8_io_vfs_rmdir(const char* path)
{
  RA8_CHECK_NULL_PTR(path, s_tag, "path must not be nullptr");
  vfs_slot_t* slot = nullptr;
  const char* sub  = nullptr;
  RA8_RETURN_ON_ERROR(priv_ra8_io_vfs_resolve(path, &slot, nullptr, &sub), s_tag, "resolve");
  if (slot->format->caps.read_only) {
    return k_ra8_err_not_supported;
  }
  if (!slot->format->caps.supports_rmdir) {
    return k_ra8_err_not_supported;
  }
  if (slot->format->ops->rmdir == nullptr) {
    return k_ra8_err_not_supported;
  }
  return slot->format->ops->rmdir(slot->mount_ctx, sub);
}
