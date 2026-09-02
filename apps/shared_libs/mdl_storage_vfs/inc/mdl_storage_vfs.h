/**
 * @file mdl_storage_vfs.h
 * @brief No-heap VFS storage binding for the media-download coordinator
 * @ingroup grp_ereader
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details
 * Binds the transactional ::ra8_mdl_storage_iface_t to a named
 * ::ra8_io_vfs mount. The destination prefix selects the medium at runtime:
 * `sd:/BOOKS/A.RBK`, `ram:/A.RBK`, and `ospi:/A.RBK` use the same adapter.
 * The adapter stages in the destination directory, closes and checks the
 * private file, then publishes it with one same-mount VFS rename.
 *
 * The current VFS rename is serialized **no-replace**, not atomic replacement.
 * On a successful return it is atomic to filesystem readers because the
 * filesystem lock spans the rename, but native FAT/exFAT explicitly does not
 * claim power-loss-atomic rename. This adapter therefore refuses an existing
 * destination before any write and checks it again immediately before rename.
 * It never moves the old file out of sight and never claims overwrite or
 * power-loss semantics the backend cannot provide.
 * `ra8_fs` writes blocks synchronously and close commits filesystem metadata,
 * but the current block-device contract has no cache-flush/durable-barrier
 * capability. Power-loss durability therefore extends only to writes the
 * selected backend has acknowledged; this adapter does not invent `fsync`.
 *
 * All buffers and state are caller-owned. No allocation, POSIX call, device
 * register, or device-specific branch exists here.
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

#include "ra8_c6link_mdl_transfer.h"
#include "ra8_io_vfs.h"

/**
 * @enum mdl_storage_vfs_limits_t
 * @brief Fixed capacities owned by one adapter instance.
 * @since 0.1.0
 */
typedef enum : uint16_t {
  k_mdl_storage_vfs_path_capacity       = 768, /**< VFS path incl NUL.   */
  k_mdl_storage_vfs_stage_leaf_capacity = 32,  /**< Stage leaf incl NUL. */
} mdl_storage_vfs_limits_t;

/**
 * @brief Validate a closed private artifact before publication.
 *
 * @param[in,out] ctx Caller-owned validation context.
 * @param[in] staging_path Named VFS path of the closed private file.
 * @param[in] total_bytes Byte count independently tracked by the transfer.
 * @param[in] sha256 Independently verified transfer digest.
 * @return Validation status. Any non-success prevents publication.
 * @pre `staging_path` names a regular file whose size equals `total_bytes`.
 * @post The callback does not rename or delete `staging_path`.
 * @note Optional in ::mdl_storage_vfs_config_t. A null callback selects
 *       size/type validation only, suitable for format-agnostic raw bytes.
 * @since 0.1.0
 */
typedef ra8_err_t (*mdl_storage_vfs_validate_fn)(void*         ctx,
                                                 const char*   staging_path,
                                                 uint64_t      total_bytes,
                                                 const uint8_t sha256[k_ra8_mdl_sha256_bytes]);

/**
 * @struct mdl_storage_vfs_config_t
 * @brief Immutable policy copied or retained by an adapter instance.
 *
 * @details `stage_leaf` is a caller-reserved simple filename such as
 * `MDLSTAGE.TMP`. The adapter owns that name in the destination directory:
 * `begin` may remove a stale regular file with that exact name after a prior
 * interrupted transaction. It never removes a directory at that path.
 *
 * @invariant `stage_leaf` is a non-empty, non-dot path component with no slash,
 *            backslash, or colon and fits the fixed staging-leaf capacity.
 * @since 0.1.0
 */
typedef struct {
  const char*                 stage_leaf;   /**< Reserved sibling staging leaf. */
  mdl_storage_vfs_validate_fn validate;     /**< Optional artifact validator.   */
  void*                       validate_ctx; /**< Validator context.             */
} mdl_storage_vfs_config_t;

/**
 * @enum mdl_storage_vfs_state_t
 * @brief Observable adapter lifecycle state.
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_mdl_storage_vfs_idle      = 0, /**< No owned private object.       */
  k_mdl_storage_vfs_writing   = 1, /**< Private writer is open.        */
  k_mdl_storage_vfs_staged    = 2, /**< Writer closed; not validated.  */
  k_mdl_storage_vfs_ready     = 3, /**< Validated and ready to rename. */
  k_mdl_storage_vfs_committed = 4, /**< Destination was published.     */
} mdl_storage_vfs_state_t;

/**
 * @struct mdl_storage_vfs_t
 * @brief Caller-owned VFS transaction context.
 *
 * @details Declare one instance for each concurrently active downloader. Its
 * fixed buffers hold the canonical final and sibling-staging paths. Treat all
 * fields as private after ::mdl_storage_vfs_init.
 *
 * @invariant `file` is non-null exactly while `state` is `_writing`.
 * @invariant `staging_path` and `destination` share one mount and parent.
 * @invariant No field points into dynamically allocated storage.
 * @since 0.1.0
 */
typedef struct {
  /** Open private writer. */
  ra8_io_vfs_file_t* file;
  /** Optional delegated check. */
  mdl_storage_vfs_validate_fn validate;
  /** Delegated check context. */
  void* validate_ctx;
  /** Successful appended bytes. */
  uint64_t bytes_written;
  /** Transaction lifecycle. */
  mdl_storage_vfs_state_t state;
  /** Owned leaf. */
  char stage_leaf[k_mdl_storage_vfs_stage_leaf_capacity];
  /** Final path. */
  char destination[k_mdl_storage_vfs_path_capacity];
  /** Private path. */
  char staging_path[k_mdl_storage_vfs_path_capacity];
} mdl_storage_vfs_t;

/**
 * @brief Initialise one VFS storage adapter and bind its coordinator interface.
 *
 * @param[out] storage Caller-owned adapter context.
 * @param[in] config Staging-name policy and optional artifact validator.
 * @param[out] out_iface Bound media-download storage interface.
 * @return Canonical status.
 * @retval k_ra8_ok Adapter initialised and interface bound.
 * @retval k_ra8_err_null_ptr A required pointer or `stage_leaf` is null.
 * @retval k_ra8_err_invalid_arg `stage_leaf` is not a simple path component.
 * @retval k_ra8_err_invalid_size `stage_leaf` exceeds the fixed capacity.
 * @pre `storage` has no active transaction.
 * @post Success leaves `storage` idle and every callback in `out_iface` bound.
 * @note Not thread-safe; one context serves one transfer at a time.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t mdl_storage_vfs_init(mdl_storage_vfs_t*              storage,
                                             const mdl_storage_vfs_config_t* config,
                                             ra8_mdl_storage_iface_t*        out_iface);

#ifdef __cplusplus
}
#endif
