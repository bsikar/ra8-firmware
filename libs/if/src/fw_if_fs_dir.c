/**
 * @file fw_if_fs_dir.c
 * @brief Guarded incremental-directory dispatch for portable filesystem ports.
 * @ingroup grp_io
 *
 * @par Tag
 * [Ring 2 / Interface] {World: Any}
 *
 * @details Validates caller-owned cursor storage, lifecycle, and stable copied
 * directory values before returning them to domain code.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "fw_if_fs.h"
#include "fw_if_fs_backend.h"
#include "ra8_attributes.h"
#include "ra8_err.h"

/**
 * @brief Validate one namespace facade used for cursor dispatch.
 * @details Checks only the binding needed by incremental-directory operations;
 *          it does not inspect or mutate backend-owned state.
 * @param[in] names Namespace facade to inspect.
 * @return Facade lifecycle status.
 * @retval k_ra8_ok The facade is bound.
 * @retval k_ra8_err_null_ptr @p names is NULL.
 * @retval k_ra8_err_not_initialized No namespace vtable is bound.
 * @pre The caller does not concurrently replace @p names.
 * @pre A non-NULL facade object remains readable for the call duration.
 * @post No state is modified.
 * @post The return value depends only on the observed facade binding.
 * @note Pure for a stable binding.
 * @since Version 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_cursor_names(const fw_fs_namespace_t* names)
{
  if (names == nullptr) {
    return k_ra8_err_null_ptr;
  }
  return (names->iface == nullptr) ? k_ra8_err_not_initialized : k_ra8_ok;
}

/**
 * @brief Validate cursor workspace size and alignment.
 * @details Enforces the opaque backend's published extent and power-of-two
 *          alignment contract before the backend can see caller storage.
 * @param[in,out] workspace Caller-owned opaque storage.
 * @param[in] bytes Accessible storage extent.
 * @param[in] need Backend-required storage extent.
 * @param[in] align Backend-required power-of-two alignment.
 * @return Workspace contract status.
 * @retval k_ra8_ok The workspace meets the backend contract.
 * @retval k_ra8_err_null_ptr @p workspace is NULL.
 * @retval k_ra8_err_no_mem @p bytes is less than @p need.
 * @retval k_ra8_err_invalid_state Backend alignment metadata is invalid.
 * @retval k_ra8_err_invalid_arg The workspace address is misaligned.
 * @pre Size and alignment facts came from a bound filesystem.
 * @pre @p bytes describes the accessible extent beginning at @p workspace.
 * @post The workspace is unchanged.
 * @post Success proves the full backend extent is suitably aligned.
 * @note Pure and thread-safe.
 * @since Version 0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_cursor_workspace(void* workspace, uint32_t bytes, uint32_t need, uint8_t align)
{
  if (workspace == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (bytes < need) {
    return k_ra8_err_no_mem;
  }
  if ((align == 0U) || (((uint32_t)align & ((uint32_t)align - 1U)) != 0U)) {
    return k_ra8_err_invalid_state;
  }
  return (((uintptr_t)workspace % (uintptr_t)align) == 0U) ? k_ra8_ok : k_ra8_err_invalid_arg;
}

/**
 * @brief Validate an open directory cursor before dispatch.
 * @details Rejects unopened and detached handles before dereferencing their
 *          backend callback table.
 * @param[in] directory Directory cursor to inspect.
 * @return Cursor lifecycle status.
 * @retval k_ra8_ok The cursor is open and dispatchable.
 * @retval k_ra8_err_null_ptr @p directory is NULL.
 * @retval k_ra8_err_invalid_state The cursor is not open.
 * @retval k_ra8_err_not_initialized The cursor lacks a backend interface.
 * @pre The caller does not concurrently close @p directory.
 * @pre A non-NULL cursor object remains readable for the call duration.
 * @post No state is modified.
 * @post Success proves a live backend interface is available.
 * @note Thread-safe only with external lifecycle synchronization.
 * @since Version 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_cursor_handle(const fw_fs_dir_t* directory)
{
  if (directory == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (!directory->is_open) {
    return k_ra8_err_invalid_state;
  }
  return (directory->iface == nullptr) ? k_ra8_err_not_initialized : k_ra8_ok;
}

/**
 * @brief Validate one backend-produced stable directory value.
 * @details Checks bounded NUL termination, portable leaf-path syntax, node
 *          type, and the directory-size invariant at the trust boundary.
 * @param[in] caps Immutable namespace limits.
 * @param[in] entry Candidate copied directory entry.
 * @return Portable entry-contract status.
 * @retval k_ra8_ok The value is coherent and names one safe leaf.
 * @retval k_ra8_err_invalid_state A backend violated the cursor contract.
 * @pre Both pointers are non-NULL and @p entry is fully initialized.
 * @pre @p caps contains the immutable limits of the producing backend.
 * @post Inputs are unchanged.
 * @post Success proves the copied value is safe for public publication.
 * @note Pure and thread-safe.
 * @since Version 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_cursor_entry(const fw_fs_caps_t*         caps,
                                                    const fw_fs_dirent_value_t* entry)
{
  if ((entry->name_bytes == 0U) || (entry->name_bytes > caps->name_max_bytes) ||
      (entry->name_bytes >= (uint16_t)k_fw_fs_path_cap)) {
    return k_ra8_err_invalid_state;
  }
  if ((entry->name[entry->name_bytes] != '\0') ||
      (strnlen(entry->name, (size_t)k_fw_fs_path_cap) != (size_t)entry->name_bytes)) {
    return k_ra8_err_invalid_state;
  }
  if ((entry->type == k_fw_fs_node_none) ||
      ((uint32_t)entry->type > (uint32_t)k_fw_fs_node_other) ||
      ((entry->type == k_fw_fs_node_directory) && (entry->size_bytes != 0U))) {
    return k_ra8_err_invalid_state;
  }
  char path[k_fw_fs_path_cap] = {'/'};
  (void)memcpy(&path[1], entry->name, (size_t)entry->name_bytes + 1U);
  return (fw_fs_path_validate(caps, path) == k_ra8_ok) ? k_ra8_ok : k_ra8_err_invalid_state;
}

ra8_err_t fw_fs_dir_open(const fw_fs_namespace_t* names,
                         const char*              path,
                         fw_fs_dir_t*             directory,
                         void*                    workspace,
                         uint32_t                 workspace_size)
{
  const ra8_err_t valid = internal_cursor_names(names);
  if (valid != k_ra8_ok) {
    return valid;
  }
  if (directory == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (directory->is_open) {
    return k_ra8_err_busy;
  }
  const ra8_err_t path_err = fw_fs_path_validate(&names->caps, path);
  if (path_err != k_ra8_ok) {
    return path_err;
  }
  const ra8_err_t work = internal_cursor_workspace(workspace,
                                                   workspace_size,
                                                   names->caps.directory_workspace_bytes,
                                                   names->caps.directory_workspace_align);
  if (work != k_ra8_ok) {
    return work;
  }
  const ra8_err_t opened = names->iface->dir_open(names->ctx, path, workspace, workspace_size);
  if (opened != k_ra8_ok) {
    return opened;
  }
  *directory = (fw_fs_dir_t){.iface       = names->iface,
                             .ctx         = names->ctx,
                             .state       = workspace,
                             .state_bytes = workspace_size,
                             .caps        = names->caps,
                             .is_open     = true};
  return k_ra8_ok;
}

ra8_err_t fw_fs_dir_next(fw_fs_dir_t* directory, fw_fs_dirent_value_t* out, bool* out_entry)
{
  const ra8_err_t valid = internal_cursor_handle(directory);
  if (valid != k_ra8_ok) {
    return valid;
  }
  if ((out == nullptr) || (out_entry == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  *out                           = (fw_fs_dirent_value_t){};
  *out_entry                     = false;
  fw_fs_dirent_value_t candidate = {};
  bool                 present   = false;
  const ra8_err_t      result =
    directory->iface->dir_next(directory->ctx, directory->state, &candidate, &present);
  if ((result != k_ra8_ok) || !present) {
    return result;
  }
  const ra8_err_t coherent = internal_cursor_entry(&directory->caps, &candidate);
  if (coherent != k_ra8_ok) {
    return coherent;
  }
  *out       = candidate;
  *out_entry = true;
  return k_ra8_ok;
}

ra8_err_t fw_fs_dir_close(fw_fs_dir_t* directory)
{
  const ra8_err_t valid = internal_cursor_handle(directory);
  if (valid != k_ra8_ok) {
    return valid;
  }
  const ra8_err_t closed = directory->iface->dir_close(directory->ctx, directory->state);
  *directory             = (fw_fs_dir_t){};
  return closed;
}
