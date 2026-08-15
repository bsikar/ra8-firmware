/**
 * @file fw_if_fs_posix_bind.c
 * @brief Lifecycle and capability binding for the hosted POSIX filesystem.
 * @ingroup grp_io
 *
 * @par Tag
 * [Ring 4 / Host Port] {World: Host}
 *
 * @details
 * Opens and owns the confined root descriptor, probes the host's atomic
 * no-replace primitive, assembles truthful workspace and durability
 * capabilities, and binds those properties to the POSIX operation tables.
 * Keeping lifecycle composition separate from operation implementations makes
 * descriptor ownership and public initialization failure handling explicit.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#ifndef _GNU_SOURCE
/** @brief Request GNU descriptor-relative syscall declarations on Linux. */
#define _GNU_SOURCE
#endif

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

#include "fw_if_fs_posix.h"
#include "ra8_attributes.h"

#if defined(__linux__) || defined(__APPLE__)
#include <sys/syscall.h>
#endif

#include "fw_if_fs.h"
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

#ifndef RENAME_NOREPLACE
/** @brief Host flag value for atomic no-replace rename probing. */
#define RENAME_NOREPLACE (1U << 0U)
#endif

/**
 * @brief Probe whether the host provides an atomic no-replace rename.
 * @details Uses deliberately invalid descriptors so support can be detected
 *          without touching the filesystem namespace.
 * @return True only when the host recognizes the requested operation.
 * @retval true The host recognizes an atomic no-replace rename primitive.
 * @retval false The host does not provide the required primitive.
 * @pre No filesystem state is required.
 * @pre The host syscall ABI matches the platform selected at compile time.
 * @post No descriptor or filesystem object is created or consumed.
 * @post The result reflects syscall recognition rather than path existence.
 * @note The invalid-descriptor probe has no namespace side effects.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_atomic_noreplace_available(void)
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

/**
 * @brief Assemble capabilities for one initialized POSIX adapter.
 * @details Fills fixed workspace sizes, path and handle bounds, and only the
 *          optional flags established by configuration or the runtime probe.
 * @param[in] state Initialized adapter state and probe results.
 * @param[out] out Complete capability description.
 * @pre `state` and `out` designate valid objects.
 * @pre `state` contains the selected removable-media and atomic probe values.
 * @post `out` describes only guarantees supplied by this adapter and host.
 * @post Every workspace size and alignment matches the corresponding state type.
 * @note Pure apart from writing the caller-owned output object.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_caps(const fw_fs_posix_state_t* state, fw_fs_caps_t* out)
{
  *out = (fw_fs_caps_t){
    .max_file_bytes = (uint64_t)INT64_MAX,
    .flags = (uint32_t)k_fw_fs_cap_namespace | (uint32_t)k_fw_fs_cap_stream |
             (uint32_t)k_fw_fs_cap_space_query | (uint32_t)k_fw_fs_cap_same_volume_rename |
             (uint32_t)k_fw_fs_cap_atomic_replace | (uint32_t)k_fw_fs_cap_create_exclusive |
             (uint32_t)k_fw_fs_cap_file_sync | (uint32_t)k_fw_fs_cap_durable_file_sync |
             (uint32_t)k_fw_fs_cap_durable_directory_sync | (uint32_t)k_fw_fs_cap_transactions |
             (uint32_t)k_fw_fs_cap_symlinks | (uint32_t)k_fw_fs_cap_rejects_symlink_walk |
             (uint32_t)k_fw_fs_cap_modified_time | (uint32_t)k_fw_fs_cap_accessed_time,
    .file_workspace_bytes        = sizeof(posix_file_state_t),
    .directory_workspace_bytes   = sizeof(posix_directory_state_t),
    .transaction_workspace_bytes = sizeof(posix_transaction_state_t),
    .path_max_bytes              = (uint16_t)k_fw_fs_path_cap,
    .name_max_bytes              = (uint16_t)(k_posix_component_cap - 1U),
    .max_open_files              = (uint16_t)k_posix_max_open_files,
    .max_open_directories        = (uint16_t)k_posix_max_open_files,
    .file_workspace_align        = (uint8_t)_Alignof(posix_file_state_t),
    .directory_workspace_align   = (uint8_t)_Alignof(posix_directory_state_t),
    .transaction_workspace_align = (uint8_t)_Alignof(posix_transaction_state_t),
  };
#if defined(__APPLE__)
  out->flags |= (uint32_t)k_fw_fs_cap_created_time;
#endif
  if (state->atomic_noreplace) {
    out->flags |= (uint32_t)k_fw_fs_cap_atomic_noreplace;
  }
  if (state->removable_media) {
    out->flags |= (uint32_t)k_fw_fs_cap_removable_media;
  }
}

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
    return priv_fs_posix_errno(errno);
  }
  state->root_fd          = root;
  state->transaction_id   = 0U;
  state->removable_media  = cfg->removable_media;
  state->atomic_noreplace = internal_atomic_noreplace_available();
  state->initialized      = true;
  fw_fs_caps_t caps       = {};
  internal_caps(state, &caps);
  const ra8_err_t bound = priv_fs_posix_bind_interfaces(out, state, &caps);
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
  return priv_fs_posix_close_fd(&state->root_fd);
}
