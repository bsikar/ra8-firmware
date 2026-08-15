/**
 * @file mdl_library.h
 * @brief Portable bounded operations over a tracked media library.
 *
 * @details
 * A library is one canonical directory in an injected ::mdl_storage_t binding.
 * Immediate child directories are tracked only when `.mdl_state` or its `.alt`
 * peer contains an authenticated state generation. Enumeration and recursive
 * removal use only `fw_fs` namespace and stream operations; no host path or
 * allocator-backed directory object crosses this domain seam.
 *
 * Removal is an iterative post-order walk over caller-owned path storage. The
 * caller also supplies explicit entry, depth, and operation limits, making
 * worst-case work visible at every composition root.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "mdl_state.h"
#include "mdl_storage.h"
#include "ra8_err.h"

/** @brief Hard ceilings accepted by the portable library algorithms. */
typedef enum : uint32_t {
  k_mdl_library_entry_limit     = 100000U, /**< Maximum discovered entries. */
  k_mdl_library_operation_limit = 500000U, /**< Maximum namespace calls.    */
  k_mdl_library_depth_limit     = 32U,     /**< Maximum child-dir nesting.  */
} mdl_library_limit_t;

/**
 * @struct mdl_library_policy_t
 * @brief Caller-selected limits within the compile-time hard ceilings.
 */
typedef struct {
  uint32_t max_entries;    /**< Entries accepted before fail-closed stop. */
  uint32_t max_operations; /**< Namespace calls permitted for removal.    */
  uint16_t max_depth;      /**< Child-directory nesting below the root.   */
} mdl_library_policy_t;

/**
 * @struct mdl_library_workspace_t
 * @brief Caller-owned directory cursor storage and iterative traversal stack.
 * @details Enumeration advances one backend cursor entry at a time. Each next
 *          call returns with backend locks released, so authentication and user
 *          callbacks occur safely between calls without retaining a directory
 *          snapshot. Removal retains one canonical path per permitted depth.
 */
typedef struct {
  /** @brief DFS path stack, one canonical path at each permitted depth. */
  char     paths[(size_t)k_mdl_library_depth_limit + 1U][k_fw_fs_path_cap];
  void*    directory_workspace;       /**< Backend cursor workspace.          */
  uint32_t directory_workspace_bytes; /**< Cursor workspace extent.           */
  uint32_t entries;                   /**< Entries accepted by the traversal. */
  uint32_t required_entries;          /**< Entries observed through cap+1.    */
  uint32_t entry_limit;               /**< Active explicit traversal limit.   */
  uint32_t operations;                /**< Namespace calls attempted.         */
  uint16_t depth;                     /**< Current stack depth, root is zero. */
} mdl_library_workspace_t;

/**
 * @brief Bind caller-owned directory storage to a reusable library workspace.
 * @details The storage must satisfy the selected filesystem's advertised
 *          directory-workspace size and alignment. Its size is independent of
 *          the number of library entries; enumeration retains only one value.
 * @param[out] workspace Library workspace to initialize.
 * @param[in,out] directory_workspace Caller-owned backend cursor state.
 * @param[in] directory_workspace_bytes Accessible workspace extent.
 * @return Workspace binding status.
 * @retval k_ra8_ok The workspace is initialized and idle.
 * @retval k_ra8_err_invalid_arg A pointer or zero capacity is invalid.
 * @pre @p directory_workspace addresses the reported writable extent.
 * @pre Neither storage region is in use by another traversal.
 * @post Success retains the workspace pointer and extent and clears counters.
 * @post Failure does not initialize a usable workspace.
 * @note No memory is allocated or ownership transferred.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t mdl_library_workspace_init(mdl_library_workspace_t* workspace,
                                                   void*                    directory_workspace,
                                                   uint32_t directory_workspace_bytes);

/**
 * @brief Return the production library traversal policy.
 * @details Selects the complete fixed entry, operation, and depth ceilings;
 *          tests may pass a stricter policy to exercise every boundary.
 * @return Policy containing all production hard ceilings.
 * @retval mdl_library_policy_t Complete bounded production policy.
 * @pre No initialization or filesystem binding is required.
 * @pre The returned value is copied by the caller before optional narrowing.
 * @post Every field is nonzero and within its corresponding hard ceiling.
 * @post No global or caller state is modified.
 * @note Thread-safe and side-effect free.
 * @since 0.1.0
 */
[[nodiscard]] mdl_library_policy_t mdl_library_policy_default(void);

/**
 * @brief Per-series visitor callback for ::mdl_library_for_each.
 * @details Receives the already authenticated state generation selected from
 *          the logical marker and `.alt`; setting @p out_continue false stops
 *          enumeration successfully, while a returned error aborts it.
 * @param[in] series_dir Canonical path of one tracked series directory.
 * @param[in] state_path Canonical logical `.mdl_state` path.
 * @param[in] state Validated state, borrowed until this callback returns.
 * @param[in,out] ctx Opaque caller context, possibly NULL.
 * @param[out] out_continue Set false to stop normally or true to continue.
 * @return Callback status propagated unchanged by the enumeration.
 * @retval k_ra8_ok The callback completed and initialized @p out_continue.
 * @pre Path/state/output pointers are non-NULL and paths are NUL-terminated.
 * @pre The callback does not retain @p state or mutate the enumerated directory.
 * @post The callback initializes @p out_continue on success.
 * @post No ownership of path, state, or context storage is transferred.
 * @note Invoked serially; not thread-safe with shared context.
 * @since 0.1.0
 */
typedef ra8_err_t (*mdl_library_fn)(const char*        series_dir,
                                    const char*        state_path,
                                    const mdl_state_t* state,
                                    void*              ctx,
                                    bool*              out_continue);

/**
 * @brief Visit every authenticated tracked series under a library root.
 * @details Validates the canonical root, enumerates at most `max_entries + 1`
 *          immediate entries to distinguish the exact cap from cap+1, and
 *          authenticates both state generations before invoking @p callback.
 *          An absent root is an empty library. Clean callback stop succeeds;
 *          callback, list, stat, state-read, and close errors propagate.
 * @param[in,out] storage Initialized portable filesystem binding.
 * @param[in] out_dir Canonical library-root path.
 * @param[in,out] state_scratch Caller-owned state model reused per callback.
 * @param[in,out] workspace Initialized caller-owned cursor storage.
 * @param[in] policy Explicit bounded traversal policy.
 * @param[in] callback Authenticated-series visitor.
 * @param[in,out] callback_ctx Opaque callback context, possibly NULL.
 * @return Canonical traversal, authentication, or callback status.
 * @retval k_ra8_ok Enumeration completed, stopped cleanly, or root was absent.
 * @retval k_ra8_err_invalid_size The immediate entry cap was exceeded.
 * @retval k_ra8_err_invalid_arg A binding, policy, path, or root type is invalid.
 * @retval other A namespace, stream, authentication, close, or callback error.
 * @pre Required pointers are non-NULL and @p out_dir is canonical.
 * @pre Storage, state scratch, and workspace are exclusively owned for the call.
 * @pre The workspace meets `storage->fs->caps.directory_workspace_*`.
 * @post Every callback receives one authenticated state and runs at most once
 *       for each accepted immediate directory entry.
 * @post Enumeration never modifies the library namespace.
 * @note Not thread-safe because storage and state scratch are reused.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t mdl_library_for_each(mdl_storage_t*              storage,
                                             const char*                 out_dir,
                                             mdl_state_t*                state_scratch,
                                             mdl_library_workspace_t*    workspace,
                                             const mdl_library_policy_t* policy,
                                             mdl_library_fn              callback,
                                             void*                       callback_ctx);

/**
 * @brief Remove one canonical directory tree through portable namespace calls.
 * @details Performs a non-recursive iterative post-order traversal using
 *          ::fw_fs_dir_open, ::fw_fs_dir_next, ::fw_fs_dir_close,
 *          ::fw_fs_stat, ::fw_fs_unlink, and ::fw_fs_rmdir.
 *          Symlinks and other special nodes are refused, never followed. Every
 *          bound and dependency failure is returned even when prior children
 *          were already removed, making partial deletion explicit.
 * @param[in,out] storage Initialized portable filesystem binding.
 * @param[in] dir Canonical non-root directory to remove.
 * @param[in] policy Explicit entry, depth, and operation limits.
 * @param[in,out] workspace Initialized iterative path stack and counters.
 * @return Canonical removal or traversal status.
 * @retval k_ra8_ok The tree was removed or was already absent.
 * @retval k_ra8_err_access_denied A symbolic link or protected root was seen.
 * @retval k_ra8_err_invalid_size A path, entry, depth, or operation cap was exceeded.
 * @retval k_ra8_err_invalid_arg A binding, policy, path, or node type is invalid.
 * @retval other A list, stat, unlink, or rmdir failure propagated unchanged.
 * @pre Required pointers are non-NULL, @p dir is canonical, and workspaces are idle.
 * @pre @p workspace was initialized by ::mdl_library_workspace_init.
 * @pre The caller has authenticated that @p dir is the intended tracked series.
 * @post Success leaves no node at @p dir.
 * @post Failure never accesses a path not lexically contained beneath @p dir.
 * @note Not thread-safe against concurrent mutation of the same tree.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t mdl_library_remove_tree(mdl_storage_t*              storage,
                                                const char*                 dir,
                                                const mdl_library_policy_t* policy,
                                                mdl_library_workspace_t*    workspace);
