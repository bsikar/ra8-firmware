/**
 * @file fw_if_fs_posix_bind.c
 * @brief Lifecycle and capability binding for the hosted POSIX filesystem.
 * @ingroup grp_io
 *
 * @par Tag
 * [Ring 4 / Host Port] {World: Host}
 *
 * @details
 * Opens and owns the confined root descriptor, denies symlink roots on Linux,
 * and verifies Darwin's exact filesystem-root aliases before opening their
 * canonical components no-follow. It also probes the host's atomic no-replace
 * primitive, assembles truthful workspace and durability capabilities, and
 * binds those properties to the POSIX operation tables. Keeping lifecycle
 * composition separate makes descriptor ownership and initialization failure
 * handling explicit.
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
#include <string.h>
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
  if (result != -1L) {
    return false;
  }
  return errno == EBADF;
#elif defined(__APPLE__)
  errno            = 0;
  const int result = renameatx_np(-1, "x", -1, "y", RENAME_EXCL);
  if (result != -1) {
    return false;
  }
  return errno == EBADF;
#else
  return false;
#endif
}

/**
 * @brief Open the descriptor anchor for an absolute or relative root path.
 * @details Uses actual `/` for an absolute path and the process working
 *          directory for a relative path. Neither anchor operation traverses
 *          any caller-provided component.
 * @param[in] path Caller-selected terminated root path.
 * @param[out] out_cursor Receives the first byte to scan.
 * @param[out] out_fd Receives the owned anchor descriptor.
 * @return Anchor-open status.
 * @retval k_ra8_ok @p out_fd owns the selected anchor.
 * @retval k_ra8_err_invalid_arg @p path is empty.
 * @retval k_ra8_err_* Mapped anchor-open failure.
 * @pre @p path, @p out_cursor, and @p out_fd are non-NULL.
 * @pre @p path addresses a NUL-terminated string.
 * @post Success publishes exactly one owned descriptor and the original cursor.
 * @post Failure publishes no descriptor.
 * @note Relative paths intentionally use the working directory at call time.
 * @since Version 0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_root_base_open(const char* path, const char** out_cursor, int* out_fd)
{
  ra8_err_t status = k_ra8_err_invalid_arg;
  *out_cursor      = path;
  *out_fd          = -1;
  if (path[0] != '\0') {
    const char* anchor = (path[0] == '/') ? "/" : ".";
    const int   opened = open(anchor, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (opened < 0) {
      status = priv_fs_posix_errno(errno);
    } else {
      *out_fd = opened;
      status  = k_ra8_ok;
    }
  }
  return status;
}

/**
 * @brief Skip repeated path separators under the complete-path bound.
 * @details Advances across each leading slash while counting it against the
 *          complete path capacity. Stops before advancing when the next byte
 *          would consume the capacity reserved for the terminating NUL.
 * @param[in,out] cursor Current path byte, advanced past copied content.
 * @param[in,out] consumed Number of path bytes consumed before this call.
 * @return Separator-skip status.
 * @retval k_ra8_ok Every leading separator was consumed.
 * @retval k_ra8_err_invalid_size The complete path exceeds its bound.
 * @pre @p cursor and @p consumed are non-NULL and `*cursor` is terminated.
 * @pre `*consumed` truthfully counts bytes preceding `*cursor`.
 * @post Success advances to a non-separator byte or the terminator.
 * @post Failure advances no farther than the final permitted path byte.
 * @note Pure apart from caller-owned cursor state.
 * @since Version 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_root_skip_slashes(const char** cursor, uint16_t* consumed)
{
  ra8_err_t status = k_ra8_ok;
  RA8_LOOP_BOUND(k_fw_fs_path_cap);
  while (**cursor == '/') {
    if (*consumed >= (uint16_t)(k_fw_fs_path_cap - 1U)) {
      status = k_ra8_err_invalid_size;
      break;
    }
    *cursor = &(*cursor)[1];
    ++(*consumed);
  }
  return status;
}

/**
 * @brief Copy one bounded root component and advance its path cursor.
 * @details Copies bytes through the next slash or NUL while enforcing both
 *          the per-component capacity and the complete-path capacity. The
 *          output remains NUL-terminated on success and bounded failure.
 * @param[in,out] cursor First component byte, advanced to slash or NUL.
 * @param[in,out] consumed Number of path bytes consumed before this call.
 * @param[out] out_component Terminated component destination.
 * @return Bounded component-copy status.
 * @retval k_ra8_ok One non-empty terminated component was produced.
 * @retval k_ra8_err_invalid_size The path or a component exceeds its bound.
 * @pre All pointer parameters are non-NULL and `**cursor` starts a component.
 * @pre @p out_component has ::k_posix_component_cap writable bytes.
 * @post Success advances to the delimiter following the copied component.
 * @post Failure never writes beyond @p out_component.
 * @note Pure apart from caller-owned cursor and output storage.
 * @since Version 0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_root_name_copy(const char** cursor, uint16_t* consumed, char* out_component)
{
  uint16_t  length = 0U;
  ra8_err_t status = k_ra8_ok;
  RA8_LOOP_BOUND(k_posix_component_cap);
  for (uint16_t index = 0U; index < (uint16_t)k_posix_component_cap; ++index) {
    const char value = **cursor;
    bool       stop  = false;
    if (value == '/') {
      stop = true;
    }
    if (!stop) {
      if (value == '\0') {
        stop = true;
      }
    }
    if (!stop) {
      if (length >= (uint16_t)(k_posix_component_cap - 1U)) {
        status = k_ra8_err_invalid_size;
        stop   = true;
      }
    }
    if (!stop) {
      if (*consumed >= (uint16_t)(k_fw_fs_path_cap - 1U)) {
        status = k_ra8_err_invalid_size;
        stop   = true;
      }
    }
    if (stop) {
      break;
    }
    out_component[length] = value;
    ++length;
    *cursor = &(*cursor)[1];
    ++(*consumed);
  }
  out_component[length] = '\0';
  return status;
}

/**
 * @brief Scan one bounded component while normalizing repeated slashes.
 * @details Delegates separator skipping and component copying to independently
 *          bounded helpers, then reports an end condition for trailing slashes.
 * @param[in,out] cursor Current path byte, advanced past copied content.
 * @param[in,out] consumed Number of path bytes consumed before this call.
 * @param[out] out_component Terminated component destination.
 * @param[out] out_end Receives true when no component remains.
 * @return Bounded scan status.
 * @retval k_ra8_ok One component or the end condition was produced.
 * @retval k_ra8_err_invalid_size The path or a component exceeds its bound.
 * @pre All pointer parameters are non-NULL and `*cursor` is terminated.
 * @pre @p out_component has ::k_posix_component_cap writable bytes.
 * @post Success consumes leading slashes and one component if present.
 * @post Failure never writes beyond @p out_component.
 * @note Pure apart from caller-owned cursor and output storage.
 * @since Version 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_root_component_scan(const char** cursor,
                                                           uint16_t*    consumed,
                                                           char*        out_component,
                                                           bool*        out_end)
{
  *out_end         = false;
  out_component[0] = '\0';
  ra8_err_t status = internal_root_skip_slashes(cursor, consumed);
  if (status == k_ra8_ok) {
    if (**cursor == '\0') {
      *out_end = true;
    } else {
      status = internal_root_name_copy(cursor, consumed, out_component);
    }
  }
  return status;
}

/**
 * @brief Descend into one root component and retire the previous descriptor.
 * @details Opens through ::priv_fs_posix_component_open, then closes the owned
 *          parent. A parent-close failure consumes the newly opened descriptor
 *          before returning the close status.
 * @param[in,out] current Owned parent descriptor replaced on success.
 * @param[in] component Validated non-dot component.
 * @return Descriptor replacement status.
 * @retval k_ra8_ok @p current owns the opened child.
 * @retval k_ra8_err_* Component-open or descriptor-close failure.
 * @pre @p current owns an open directory descriptor.
 * @pre @p component is non-empty and is not `.`.
 * @post Success consumes the parent and publishes one child descriptor.
 * @post An open failure retains the parent for caller cleanup; a parent-close
 *       failure consumes both the parent and newly opened child.
 * @note Thread-safe for independent descriptors.
 * @since Version 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_root_open_step(int* current, const char* component)
{
  int       next   = -1;
  ra8_err_t status = priv_fs_posix_component_open(*current, component, &next);
  if (status == k_ra8_ok) {
    const ra8_err_t closed = priv_fs_posix_close_fd(current);
    if (closed != k_ra8_ok) {
      status = priv_fs_posix_close_fd_preserve(&next, closed);
    } else {
      *current = next;
      next     = -1;
    }
  }
  if (next >= 0) {
    status = priv_fs_posix_close_fd_preserve(&next, status);
  }
  return status;
}

/**
 * @brief Apply one scanned component to the current root descriptor.
 * @details Preserves the current descriptor for the normalized `.` component;
 *          every other component descends through the shared no-follow opener.
 * @param[in,out] current Owned directory descriptor advanced when needed.
 * @param[in] component Non-empty terminated component from the bounded scanner.
 * @return Component traversal status.
 * @retval k_ra8_ok The dot component was ignored or the child was opened.
 * @retval k_ra8_err_* Component-open or descriptor-close failure.
 * @pre @p current owns an open directory descriptor.
 * @pre @p component is non-NULL and names one scanned component.
 * @post Success leaves @p current owning the selected directory.
 * @post Failure leaves at most @p current for caller cleanup.
 * @note Thread-safe for independent descriptors.
 * @since Version 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_root_walk_step(int* current, const char* component)
{
  ra8_err_t status = k_ra8_ok;
  if (strcmp(component, ".") != 0) {
    status = internal_root_open_step(current, component);
  }
  return status;
}

/**
 * @brief Walk every remaining root component beneath an owned anchor.
 * @details Repeatedly scans a bounded name, ignores `.`, and replaces the
 *          current descriptor through the shared no-follow component opener.
 * @param[in,out] cursor Current path remainder.
 * @param[in,out] consumed Number of path bytes already consumed.
 * @param[in,out] current Owned directory descriptor advanced by each component.
 * @param[out] out_complete Receives true only after reaching path termination.
 * @return Root-walk status.
 * @retval k_ra8_ok The terminated root path was fully traversed.
 * @retval k_ra8_err_* Bounded scanning or component traversal failed.
 * @pre All pointers are non-NULL and @p current owns an anchor descriptor.
 * @pre `*cursor` addresses a terminated root-path remainder.
 * @post Success leaves @p current owning the selected root directory.
 * @post Failure leaves at most @p current for caller cleanup.
 * @note Thread-safe for independent descriptors and path storage.
 * @since Version 0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_root_walk(const char** cursor, uint16_t* consumed, int* current, bool* out_complete)
{
  ra8_err_t status = k_ra8_ok;
  *out_complete    = false;
  RA8_LOOP_BOUND(k_fw_fs_path_cap);
  for (uint16_t component = 0U; component < (uint16_t)k_fw_fs_path_cap; ++component) {
    char name[k_posix_component_cap];
    bool end  = false;
    bool stop = false;
    status    = internal_root_component_scan(cursor, consumed, name, &end);
    if (status != k_ra8_ok) {
      stop = true;
    }
    if (!stop) {
      if (end) {
        *out_complete = true;
        stop          = true;
      }
    }
    if (!stop) {
      status = internal_root_walk_step(current, name);
      if (status != k_ra8_ok) {
        stop = true;
      }
    }
    if (stop) {
      break;
    }
  }
  return status;
}

/**
 * @brief Open one caller-selected confinement root component by component.
 * @details Anchors absolute paths at `/` and relative paths at `.`, normalizes
 *          repeated and trailing slashes, and accepts native `.` and `..`
 *          semantics. Each non-dot component is opened no-follow.
 *          Linux rejects every link; Darwin alone permits verified actual-root
 *          `tmp` and `var` aliases and opens only their canonical components.
 * @param[in] path Caller-selected existing host directory.
 * @param[out] out_fd Receives the owned confinement-root descriptor.
 * @return Root-open status.
 * @retval k_ra8_ok @p out_fd owns the selected directory.
 * @retval k_ra8_err_access_denied A symbolic-link component was rejected.
 * @retval k_ra8_err_invalid_size The complete path or one component is too long.
 * @retval k_ra8_err_* Mapped open, verification, or descriptor-close failure.
 * @pre @p path and @p out_fd are non-NULL.
 * @pre @p path is a terminated path naming a caller-selected root.
 * @post Success publishes exactly one owned directory descriptor.
 * @post Failure publishes no descriptor and follows no symbolic-link pathname.
 * @note Thread-safe for independent paths and descriptors.
 * @since Version 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_root_open(const char* path, int* out_fd)
{
  const char* cursor   = path;
  uint16_t    consumed = 0U;
  int         current  = -1;
  bool        complete = false;
  ra8_err_t   status   = internal_root_base_open(path, &cursor, &current);
  *out_fd              = -1;
  if (status == k_ra8_ok) {
    status = internal_root_walk(&cursor, &consumed, &current, &complete);
  }
  if (status == k_ra8_ok) {
    if (!complete) {
      status = k_ra8_err_invalid_size;
    } else {
      *out_fd = current;
      current = -1;
    }
  }
  if (current >= 0) {
    const ra8_err_t closed = priv_fs_posix_close_fd(&current);
    if (status == k_ra8_ok) {
      status = closed;
    }
  }
  return status;
}

/**
 * @brief Restore one inactive POSIX adapter state to its public sentinel.
 * @details Clears all capability inputs and counters in addition to publishing
 *          descriptor -1 and an inactive lifecycle flag.
 * @param[out] state Caller-owned adapter state to normalize.
 * @pre @p state is non-NULL and owns no open descriptor.
 * @pre No interface operation is active against @p state.
 * @post @p state is inactive with root descriptor -1.
 * @post Counters and cached capability inputs are zero or false.
 * @note This helper does not close descriptors.
 * @since Version 0.1.0
 */
RA8_INTERNAL static void internal_state_reset(fw_fs_posix_state_t* state)
{
  state->root_fd          = -1;
  state->transaction_id   = 0U;
  state->removable_media  = false;
  state->atomic_noreplace = false;
  state->initialized      = false;
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
#ifdef __APPLE__
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
  if (out == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (state == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (cfg == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (cfg->root_path == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (state->initialized) {
    return k_ra8_err_exists;
  }
  internal_state_reset(state);
  int             root        = -1;
  const ra8_err_t root_status = internal_root_open(cfg->root_path, &root);
  if (root_status != k_ra8_ok) {
    return root_status;
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
  const ra8_err_t status = priv_fs_posix_close_fd(&state->root_fd);
  internal_state_reset(state);
  return status;
}
