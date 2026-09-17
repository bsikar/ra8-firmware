/**
 * @file fw_if_fs_ra8_vfs.h
 * @brief Firmware filesystem-port adapter over `ra8_io_vfs` and `ra8_fs`.
 * @ingroup grp_io
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details
 * The composition root binds one already-mounted VFS volume as the portable
 * root. Portable `/books/a.cbz` becomes `mount:/books/a.cbz`; portable code
 * never sees the mount name or underlying SD/RAM/flash medium.
 *
 * FAT/exFAT has no symlinks, so traversal is structurally absent after the
 * generic lexical path guard. `ra8_fs_close` commits filesystem metadata but
 * the current stack exposes neither an explicit medium flush nor crash-atomic
 * replacement. The adapter therefore does not advertise durable sync or
 * atomic replacement. Its staged transaction safely publishes a new name;
 * replacing an existing destination is rejected without changing it.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "fw_if_fs.h"
#include "ra8_fs.h"
#include "ra8_io_vfs.h"

/** @brief Fixed storage used to prefix one portable path with a mount name. */
typedef enum : uint16_t {
  k_fw_fs_ra8_vfs_full_path_cap =
    (uint16_t)k_fw_fs_path_cap + (uint16_t)k_ra8_io_vfs_name_max + 2U, /**< `name:` + path + NUL. */
} fw_fs_ra8_vfs_limits_t;

/** @brief Composition-root configuration for one firmware filesystem binding. */
typedef struct {
  const char*     mount_name;      /**< Registered VFS name without `:`.         */
  ra8_fs_mount_t* mount;           /**< Matching live mount for space/caps.      */
  bool            removable_media; /**< True for hot-removable media such as SD. */
} fw_fs_ra8_vfs_cfg_t;

/**
 * @brief Caller-owned adapter context.
 * @details Scratch makes dispatch allocation-free. One state is deliberately
 *          non-reentrant; install serialization above it when shared.
 */
typedef struct {
  char            mount_name[k_ra8_io_vfs_name_max];     /**< Bounded mount name.             */
  char            path_a[k_fw_fs_ra8_vfs_full_path_cap]; /**< First path scratch.             */
  char            path_b[k_fw_fs_ra8_vfs_full_path_cap]; /**< Rename path scratch.            */
  ra8_fs_mount_t* mount;                                 /**< Live mount.                     */
  uint32_t        directory_workspace_bytes;             /**< Native cursor workspace bytes.  */
  uint32_t        transaction_id;                        /**< Stage-name counter.             */
  uint16_t        max_open_directories;                  /**< Native concurrent cursor limit. */
  uint8_t         directory_workspace_align;             /**< Native cursor alignment.        */
  bool            removable_media;                       /**< Capability input.               */
} fw_fs_ra8_vfs_state_t;

/**
 * @brief Bind a live named VFS mount into the portable filesystem facade.
 * @param[out]    out   Complete portable facade.
 * @param[in,out] state Caller-owned adapter state, retained by `out`.
 * @param[in]     cfg   Live mount, registered name, and media property.
 */
[[nodiscard]] ra8_err_t
fw_fs_ra8_vfs_init(fw_fs_t* out, fw_fs_ra8_vfs_state_t* state, const fw_fs_ra8_vfs_cfg_t* cfg);

#ifdef __cplusplus
}
#endif
