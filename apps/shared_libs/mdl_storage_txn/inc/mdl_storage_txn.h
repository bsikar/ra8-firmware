/**
 * @file mdl_storage_txn.h
 * @brief Bridge the media-download storage seam onto the fw_if_fs transaction
 *        port
 * @ingroup grp_ereader
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details
 * The repository carries two complete staged-publication contracts for one
 * job. `fw_fs_transaction_begin/_write/_seek/_validate/_commit/_abort`
 * (`libs/if`) is the portable one, already bound to a live VFS mount by
 * `fw_fs_ra8_vfs_init()` and pinned by contract vectors. The C6 media
 * coordinator wants the same lifecycle behind five function pointers,
 * ::ra8_mdl_storage_iface_t, and `mdl_storage_vfs` satisfies that by staging
 * on `ra8_io_vfs` a second time: its own sibling-leaf policy, its own
 * stat-parent / sync-close / re-stat / same-mount rename dance, and two 768
 * byte path buffers.
 *
 * This adapter is the missing seam, in the same shape as
 * `ra8_io_blockdev_as_fs_backend()` and `ra8_io_spi_bus_as_ops()`: it fills
 * ::ra8_mdl_storage_iface_t with trampolines into one bound
 * ::fw_fs_transaction_port_t and owns no staging policy of its own. Nothing
 * here opens a file, builds a path, or renames anything; the bound port does
 * all of it, so a coordinator transfer inherits whatever atomicity and
 * capability that port actually advertises instead of a second opinion about
 * it.
 *
 * `create_new` versus atomic replacement is the caller's policy choice, which
 * ::ra8_mdl_storage_iface_t cannot express and `mdl_storage_vfs` hardcodes to
 * no-replace. It is a config field here.
 *
 * All state is caller-owned. No allocation, no POSIX call, no device register.
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
#include "fw_if_fs_types.h"
#include "ra8_c6link_mdl_transfer.h"
#include "ra8_err.h"

/**
 * @brief Inspect the closed staging artifact before publication.
 *
 * @param[in,out] ctx Caller-owned validation context.
 * @param[in,out] staged Read-only handle on the complete private artifact.
 * @param[in] total_bytes Byte count the transfer tracked independently.
 * @param[in] sha256 Digest the transfer verified independently.
 * @return Validation status. Any non-success prevents publication.
 * @pre `staged` is positioned at offset zero and spans `total_bytes` bytes.
 * @post The callback neither publishes nor destroys the stage.
 * @note Optional. A null callback selects the length check only, which suits
 *       the `loose` transfer format and nothing else.
 * @since 0.1.0
 */
typedef ra8_err_t (*mdl_storage_txn_validate_fn)(void*         ctx,
                                                 fw_fs_file_t* staged,
                                                 uint64_t      total_bytes,
                                                 const uint8_t sha256[k_ra8_mdl_sha256_bytes]);

/**
 * @struct mdl_storage_txn_cfg_t
 * @brief Immutable policy retained by one adapter instance.
 *
 * @details `workspace` is the backend transaction workspace the bound port
 * asks for through `fw_fs_get_caps()`; the adapter passes it straight to
 * `fw_fs_transaction_begin()` and never sizes or aligns it itself.
 *
 * @invariant `port` stays bound for the lifetime of the adapter.
 * @invariant `workspace` is at least `caps.transaction_workspace_bytes` and
 *            satisfies `caps.transaction_workspace_align`.
 * @since 0.1.0
 */
typedef struct {
  const fw_fs_transaction_port_t* port;           /**< Bound transaction port.   */
  fw_fs_transaction_policy_t      policy;         /**< Create-new or replace.    */
  void*                           workspace;      /**< Backend txn workspace.    */
  uint32_t                        workspace_size; /**< Workspace byte count.     */
  mdl_storage_txn_validate_fn     validate;       /**< Optional stage validator. */
  void*                           validate_ctx;   /**< Validator context.        */
} mdl_storage_txn_cfg_t;

/**
 * @enum mdl_storage_txn_state_t
 * @brief Observable adapter lifecycle state.
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_mdl_storage_txn_idle      = 0, /**< No staging artifact is owned.   */
  k_mdl_storage_txn_writing   = 1, /**< Stage is open and appendable.   */
  k_mdl_storage_txn_validated = 2, /**< Validated; commit is available. */
  k_mdl_storage_txn_committed = 3, /**< Destination was published.      */
} mdl_storage_txn_state_t;

/**
 * @struct mdl_storage_txn_t
 * @brief Caller-owned bridge context.
 *
 * @details Declare one instance per concurrently active transfer. It holds no
 * path scratch of its own: the destination string, the staging name, and every
 * byte of filesystem state live inside the bound port and the workspace it was
 * given. Treat all fields as private after ::mdl_storage_txn_init.
 *
 * @invariant `state` is `_writing` or `_validated` exactly while a backend
 *            transaction is live.
 * @invariant No field points into dynamically allocated storage.
 * @since 0.1.0
 */
typedef struct {
  /** Bound transaction port. */
  const fw_fs_transaction_port_t* port;
  /** Backend transaction workspace. */
  void* workspace;
  /** Optional delegated check. */
  mdl_storage_txn_validate_fn validate;
  /** Delegated check context. */
  void* validate_ctx;
  /** Live backend transaction. */
  fw_fs_transaction_t transaction;
  /** Bytes the port accepted. */
  uint64_t bytes_written;
  /** Length the transfer reported. */
  uint64_t expected_bytes;
  /** Digest the transfer verified. */
  uint8_t expected_sha256[k_ra8_mdl_sha256_bytes];
  /** Workspace byte count. */
  uint32_t workspace_size;
  /** Publication policy. */
  fw_fs_transaction_policy_t policy;
  /** Transaction lifecycle. */
  mdl_storage_txn_state_t state;
} mdl_storage_txn_t;

/**
 * @brief Bind one coordinator storage interface to a fw_if_fs transaction port
 *
 * @param[out] storage Caller-owned adapter context.
 * @param[in] config Bound port, publication policy, workspace, validator.
 * @param[out] out_iface Storage interface for ::ra8_mdl_transfer_config_t.
 * @return Canonical status.
 * @retval k_ra8_ok Adapter is idle and every callback in @p out_iface is bound.
 * @retval k_ra8_err_null_ptr A required pointer is null.
 * @retval k_ra8_err_invalid_size `workspace_size` is zero.
 * @pre The port was bound by a composition root, e.g. `fw_fs_ra8_vfs_init()`.
 * @pre `workspace` meets the port's advertised transaction requirements.
 * @post No filesystem access happens until the coordinator calls `begin`.
 * @note Not thread-safe; one context serves one transfer at a time.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t mdl_storage_txn_init(mdl_storage_txn_t*           storage,
                                             const mdl_storage_txn_cfg_t* config,
                                             ra8_mdl_storage_iface_t*     out_iface);

/**
 * @brief Report the adapter's lifecycle state.
 *
 * @param[in] storage Initialised adapter context.
 * @param[out] out_state Current state.
 * @return Canonical status.
 * @retval k_ra8_ok `out_state` holds the current state.
 * @retval k_ra8_err_null_ptr A required pointer is null.
 * @note Exists so an app can assert the transfer unwound, without reaching
 *       into the struct.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t mdl_storage_txn_state(const mdl_storage_txn_t* storage,
                                              mdl_storage_txn_state_t* out_state);

#ifdef __cplusplus
}
#endif
