/**
 * @file mdl_pathfs.c
 * @brief Implementation of the guarded filesystem directory join.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#include "mdl_pathfs.h"

#include <stdint.h>

#include "mdl_sanitize.h"

bool mdl_join_dir_under(mdl_storage_t* storage,
                        const char*    parent_abs,
                        const char*    seg,
                        char*          out,
                        size_t         cap)
{
  if ((storage == nullptr) || (storage->fs == nullptr)) {
    return false;
  }
  if (!mdl_path_join(parent_abs, seg, out, cap)) {
    return false;
  }
  fw_fs_stat_t parent = {};
  if ((fw_fs_stat(&storage->fs->names, parent_abs, &parent) != k_ra8_ok) || !parent.exists ||
      (parent.type != k_fw_fs_node_directory)) {
    return false;
  }
  const ra8_err_t made = fw_fs_mkdir(&storage->fs->names, out);
  if ((made != k_ra8_ok) && (made != k_ra8_err_exists)) {
    return false;
  }
  fw_fs_stat_t child = {};
  return (fw_fs_stat(&storage->fs->names, out, &child) == k_ra8_ok) && child.exists &&
         (child.type == k_fw_fs_node_directory);
}
