/**
 * @file fw_if_fs_posix.h
 * @brief Root-confined hosted POSIX adapter for `fw_if_fs`.
 * @ingroup grp_io
 *
 * @par Tag
 * [Ring 4 / Host Port] {World: Host}
 *
 * @details
 * The adapter opens one caller-selected directory and resolves every portable
 * path relative to that descriptor. Intermediate components and final file
 * opens use no-follow semantics, so neither `..` nor a symlink can escape the
 * bound root. The adapter itself calls no allocator. File streams use raw
 * descriptors and Linux/macOS directory enumeration uses a bounded local
 * kernel-record buffer rather than an opaque C-runtime directory stream. On
 * any other hosted kernel, enumeration fails with `k_ra8_err_not_supported`;
 * it never falls back to an allocator-backed `DIR` implementation.
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

/** @brief POSIX composition-root settings. */
typedef struct {
  const char* root_path;       /**< Existing host directory used as `/`.    */
  bool        removable_media; /**< Truthful property of the selected root. */
} fw_fs_posix_cfg_t;

/** @brief Caller-owned POSIX adapter state. */
typedef struct {
  int      root_fd;          /**< Open descriptor for the confined root. */
  uint32_t transaction_id;   /**< Per-binding stage-name counter.        */
  bool     removable_media;  /**< Capability input.                      */
  bool     atomic_noreplace; /**< Runtime-probed rename guarantee.       */
  bool     initialized;      /**< Lifecycle guard.                       */
} fw_fs_posix_state_t;

/** @brief Open/configure a root-confined POSIX binding. */
[[nodiscard]] ra8_err_t
fw_fs_posix_init(fw_fs_t* out, fw_fs_posix_state_t* state, const fw_fs_posix_cfg_t* cfg);

/** @brief Close the root descriptor; no bound operation is valid afterward. */
[[nodiscard]] ra8_err_t fw_fs_posix_deinit(fw_fs_posix_state_t* state);

#ifdef __cplusplus
}
#endif
