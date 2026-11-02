/**
 * @file ra8_fs_fat_resolve.c
 * @brief FAT path resolution: parent and directory lookup.
 *
 * @details
 * Splits a caller path into a parent directory location plus a leaf name
 * (`priv_resolve_parent`), and resolves a path to an existing directory
 * (`priv_resolve_dir`). Split out of `ra8_fs_fat_file.c` so the open/close
 * path can port to Zig on its own; this unit gets its own port slice next.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_fs.h"
#include "ra8_fs_fat_internal.h"

/**
 * @brief Descend into the named directory component within @p cur.
 *
 * @details Packs the `len`-byte component to an 8.3 name, looks it up in @p cur,
 *          requires the matched entry to carry the directory attribute, and
 *          returns its first cluster as a subdirectory location.
 *
 * @param[in]  m    Mount providing geometry and backend.
 * @param[in]  cur  Directory the component is looked up in.
 * @param[in]  comp Pointer to the component characters (not NUL-terminated).
 * @param[in]  len  Number of component characters.
 * @param[out] out  Receives the subdirectory location on success.
 *
 * @return Error code.
 * @retval k_ra8_ok                 Component resolved to a subdirectory.
 * @retval k_ra8_err_invalid_arg    Component is not 8.3 or is not a directory.
 * @retval k_ra8_err_not_found      No such entry in @p cur.
 * @retval k_ra8_err_protocol_error Directory entry has no data cluster.
 * @retval k_ra8_err_*              Backend error.
 *
 * @pre `m`, `cur`, `comp`, and `out` are non-NULL.
 * @pre `len` is the exact component length (no trailing slash).
 * @post On success `out` locates the subdirectory.
 * @post On failure `out` is unmodified.
 *
 * @note Not thread-safe; callers serialise.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_enter_subdir(const ra8_fs_mount_t* m,
                                       const dir_loc_t*      cur,
                                       const char*           comp,
                                       uint32_t              len,
                                       dir_loc_t*            out)
{
  /* A BYTE bound, because the component is UTF-8 here: the unit-count limit is
   * ::k_lfn_write_max and lands in priv_name_classify(). Bounding the bytes by
   * the unit cap would refuse a perfectly storable name three characters into
   * a Cyrillic directory. */
  if (len >= (uint32_t)k_lfn_utf8_cap) {
    return k_ra8_err_invalid_arg;
  }
  /* Sized for a long name, not an 8.3 one: once `mkdir` can create
   * `/Reading List`, every path THROUGH it has to resolve as well, and a
   * 13-byte buffer would have refused the component before the lookup. */
  char namebuf[k_lfn_utf8_cap] = {};
  for (uint32_t i = 0; i < len; i++) {
    namebuf[i] = comp[i];
  }
  namebuf[len]                              = '\0';
  uint64_t  lba                             = 0;
  uint32_t  off                             = 0;
  uint8_t   entry[k_ra8_fs_dir_entry_bytes] = {};
  ra8_err_t err = priv_dir_lookup_any(m, cur, namebuf, &lba, &off, entry);
  if (err != k_ra8_ok) {
    return err;
  }
  if ((entry[k_dir_off_attr] & k_ra8_fs_attr_directory) == 0U) {
    return k_ra8_err_invalid_arg;
  }
  const uint32_t cl = priv_entry_first_cluster(entry);
  if (cl < (uint32_t)k_cluster_first_data) {
    return k_ra8_err_protocol_error;
  }
  out->is_root = 0U;
  out->cluster = cl;
  return k_ra8_ok;
}

/* `priv_resolve_parent()`: see header for the documented contract. */
ra8_err_t priv_resolve_parent(const ra8_fs_mount_t* m,
                              const char*           path,
                              dir_loc_t*            out_parent,
                              const char**          out_leaf)
{
  const char* p = path;
  while (*p == '/') {
    p++;
  }
  dir_loc_t cur = {.is_root = 1U, .cluster = 0U};
  for (uint32_t depth = 0; depth < (uint32_t)k_path_max_depth; depth++) {
    const char* end = p;
    while (*end != '\0') {
      if (*end == '/') {
        break;
      }
      end++;
    }
    if (*end == '\0') {
      *out_parent = cur;
      *out_leaf   = p;
      return k_ra8_ok;
    }
    dir_loc_t       next = {};
    const ra8_err_t err  = internal_enter_subdir(m, &cur, p, (uint32_t)(end - p), &next);
    if (err != k_ra8_ok) {
      return err;
    }
    cur = next;
    p   = end;
    while (*p == '/') {
      p++;
    }
  }
  return k_ra8_err_invalid_arg; /* deeper than k_path_max_depth */
}

/* `priv_resolve_dir()`: see header for the documented contract. */
ra8_err_t priv_resolve_dir(const ra8_fs_mount_t* m, const char* path, dir_loc_t* out)
{
  const char* p = path;
  while (*p == '/') {
    p++;
  }
  if (*p == '\0') {
    out->is_root = 1U;
    out->cluster = 0U;
    return k_ra8_ok;
  }
  dir_loc_t       parent = {};
  const char*     leaf   = nullptr;
  const ra8_err_t err    = priv_resolve_parent(m, path, &parent, &leaf);
  if (err != k_ra8_ok) {
    return err;
  }
  uint32_t len = 0;
  while (leaf[len] != '\0') {
    len++;
  }
  if (len == 0U) {
    *out = parent; /* trailing slash: the parent is the directory */
    return k_ra8_ok;
  }
  return internal_enter_subdir(m, &parent, leaf, len, out);
}

