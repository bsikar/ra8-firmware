/**
 * @file mdl_library.c
 * @brief Portable bounded tracked-library enumeration and tree removal.
 * @details Uses only the injected `fw_fs` namespace/stream contracts and fixed
 *          caller-owned state; no host directory stream or recursive call exists.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#include "mdl_library.h"

#include <stdint.h>
#include <string.h>

#include "mdl_sanitize.h"
#include "ra8_attributes.h"

/** @brief Logical marker basename within one tracked series. */
static const char s_state_basename[] = ".mdl_state";

/** @brief State threaded through one immediate-library enumeration. */
typedef struct {
  mdl_storage_t* storage;      /**< Portable dependency bundle. */
  const char*    root;         /**< Canonical library root.     */
  mdl_state_t*   state;        /**< Authentication scratch.     */
  mdl_library_fn callback;     /**< Authenticated visitor.      */
  void*          callback_ctx; /**< Opaque visitor context.     */
} mdl_library_enumeration_t;

mdl_library_policy_t mdl_library_policy_default(void)
{
  return (mdl_library_policy_t){.max_entries    = (uint32_t)k_mdl_library_entry_limit,
                                .max_operations = (uint32_t)k_mdl_library_operation_limit,
                                .max_depth      = (uint16_t)k_mdl_library_depth_limit};
}

ra8_err_t mdl_library_workspace_init(mdl_library_workspace_t* workspace,
                                     void*                    directory_workspace,
                                     uint32_t                 directory_workspace_bytes)
{
  if ((workspace == nullptr) || (directory_workspace == nullptr) ||
      (directory_workspace_bytes == 0U)) {
    return k_ra8_err_invalid_arg;
  }
  *workspace = (mdl_library_workspace_t){.directory_workspace       = directory_workspace,
                                         .directory_workspace_bytes = directory_workspace_bytes};
  return k_ra8_ok;
}

/**
 * @brief Clear traversal state while preserving caller cursor storage.
 * @param[in,out] workspace Initialized reusable library workspace.
 * @pre @p workspace is non-NULL and has a valid cursor binding.
 * @pre No traversal currently borrows the workspace.
 * @post All counters and retained DFS paths are reset.
 * @post Cursor storage pointer and extent are unchanged.
 * @note Backend cursor bytes are initialized by each successful open.
 * @since 0.1.0

 * @details Advances bounded filesystem cursors without retaining backend locks.
 *          Callbacks and namespace mutations run only after cursor close.
 */
RA8_INTERNAL static void internal_library_workspace_reset(mdl_library_workspace_t* workspace)
{
  workspace->entries          = 0U;
  workspace->required_entries = 0U;
  workspace->entry_limit      = 0U;
  workspace->operations       = 0U;
  workspace->depth            = 0U;
  memset(workspace->paths, 0, sizeof(workspace->paths));
}

/**
 * @brief Validate a caller-selected policy against hard ceilings.
 * @details Rejects zero work budgets and values exceeding fixed workspace bounds.
 * @param[in] policy Candidate traversal policy.
 * @return Canonical policy status.
 * @retval k_ra8_ok Every bound is usable.
 * @retval k_ra8_err_invalid_arg The pointer or one lower bound is invalid.
 * @retval k_ra8_err_invalid_size One field exceeds its hard ceiling.
 * @pre @p policy may be NULL.
 * @pre No filesystem operation is active.
 * @post The policy is unchanged.
 * @post No dependency state is accessed.
 * @note Thread-safe and side-effect free.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_library_policy(const mdl_library_policy_t* policy)
{
  if ((policy == nullptr) || (policy->max_entries == 0U) || (policy->max_operations == 0U)) {
    return k_ra8_err_invalid_arg;
  }
  if ((policy->max_entries > (uint32_t)k_mdl_library_entry_limit) ||
      (policy->max_operations > (uint32_t)k_mdl_library_operation_limit) ||
      (policy->max_depth > (uint16_t)k_mdl_library_depth_limit)) {
    return k_ra8_err_invalid_size;
  }
  return k_ra8_ok;
}

/**
 * @brief Validate and measure one borrowed backend leaf name.
 * @details Cross-checks the reported byte count, binding component cap, NUL,
 *          and lexical single-segment rules before any path composition.
 * @param[in] storage Initialized filesystem binding.
 * @param[in] entry Borrowed directory entry.
 * @param[out] out_length Verified leaf length.
 * @return Canonical backend-contract status.
 * @retval k_ra8_ok The complete leaf is safe to compose.
 * @retval k_ra8_err_invalid_state The backend supplied inconsistent metadata.
 * @retval k_ra8_err_invalid_arg The leaf is not one safe path segment.
 * @pre All pointers are non-NULL.
 * @pre @p entry is valid for the active list callback.
 * @post Success initializes @p out_length.
 * @post No input or filesystem state is modified.
 * @note The hard path cap bounds `strnlen` even for a faulty backend.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_library_leaf(const mdl_storage_t*        storage,
                                                    const fw_fs_dirent_value_t* entry,
                                                    size_t*                     out_length)
{
  if (entry->name_bytes == 0U) {
    return k_ra8_err_invalid_state;
  }
  const size_t length = strnlen(entry->name, (size_t)k_fw_fs_path_cap);
  if ((length >= (size_t)k_fw_fs_path_cap) || (length != (size_t)entry->name_bytes) ||
      (length > (size_t)storage->fs->caps.name_max_bytes)) {
    return k_ra8_err_invalid_state;
  }
  if ((strcmp(entry->name, ".") == 0) || (strcmp(entry->name, "..") == 0) ||
      (strpbrk(entry->name, "/\\") != nullptr)) {
    return k_ra8_err_invalid_arg;
  }
  *out_length = length;
  return k_ra8_ok;
}

/**
 * @brief Authenticate and deliver one stable cursor entry.
 * @details Ignores non-directories and untracked directories; every existing
 *          marker must authenticate before the user callback can observe it.
 * @param[in,out] walk Active post-list enumeration state.
 * @param[in] entry Stable copied directory entry.
 * @param[out] out_continue User callback continuation decision.
 * @return Canonical stat, authentication, or callback status.
 * @retval k_ra8_ok The entry was ignored, visited, or stopped cleanly.
 * @pre All pointers are non-NULL and no backend lock is held.
 * @pre @p entry was validated by guarded cursor dispatch.
 * @post The callback runs only after authenticated state recovery.
 * @post @p out_continue is initialized on success.
 * @note Filesystem calls occur strictly between cursor-next operations.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_library_visit(mdl_library_enumeration_t*  walk,
                                                     const fw_fs_dirent_value_t* entry,
                                                     bool*                       out_continue)
{
  *out_continue = true;
  if (entry->type != k_fw_fs_node_directory) {
    return k_ra8_ok;
  }
  char series[k_fw_fs_path_cap];
  char state[k_fw_fs_path_cap];
  if (!mdl_path_join(walk->root, entry->name, series, sizeof(series)) ||
      !mdl_path_join(series, s_state_basename, state, sizeof(state))) {
    return k_ra8_err_invalid_size;
  }
  ra8_err_t    err  = k_ra8_ok;
  fw_fs_stat_t node = {};
  err               = fw_fs_stat(&walk->storage->fs->names, series, &node);
  if (err != k_ra8_ok) {
    return err;
  }
  if (!node.exists || (node.type != entry->type)) {
    return k_ra8_err_invalid_state;
  }
  bool tracked = false;
  err          = mdl_state_probe(walk->storage, state, &tracked);
  if ((err != k_ra8_ok) || !tracked) {
    *out_continue = !tracked && (err == k_ra8_ok);
    return err;
  }
  err = mdl_state_load_authenticated(walk->storage, state, walk->state);
  if (err != k_ra8_ok) {
    return err;
  }
  bool user_continue = false;
  err = walk->callback(series, state, walk->state, walk->callback_ctx, &user_continue);
  if (err != k_ra8_ok) {
    return err;
  }
  *out_continue = user_continue;
  return k_ra8_ok;
}

/** @brief Preserve the first traversal error while always consuming a cursor.
 * @details Advances bounded filesystem cursors without retaining backend locks.
 *          Callbacks and namespace mutations run only after cursor close.
 * @param[in,out] directory Directory handle owned by the caller.
 * @param[in] first First text fragment.
 * @return Operation status.
 * @retval k_ra8_ok The operation completed successfully.
 * @retval other The originating validation, storage, stream, or network error.
 * @pre Every required pointer is non-null and remains valid for the call.
 * @pre Lengths and capacities describe complete referenced objects without overflow.
 * @post Documented outputs and the return value describe the same outcome.
 * @post A rejected or failed operation is never reported as successful.
 * @note Thread safety follows ownership of the supplied context; no synchronization is added.
 * @since Version 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_library_close(fw_fs_dir_t* directory, ra8_err_t first)
{
  if (!directory->is_open) {
    return first;
  }
  const ra8_err_t closed = fw_fs_dir_close(directory);
  return (first == k_ra8_ok) ? closed : first;
}

/**
 * @brief Validate one library root before cursor open.
 * @param[in,out] storage Portable dependency binding.
 * @param[in] root_path Canonical library root.
 * @param[out] out_exists Whether the validated root exists.
 * @return Root stat and type status; an absent root is an empty library.
 * @pre Required pointers are non-NULL.
 * @post No cursor or stream remains open.
 * @note Symlink roots fail closed.
 * @since 0.1.0

 * @details Advances bounded filesystem cursors without retaining backend locks.
 *          Callbacks and namespace mutations run only after cursor close.
 * @retval k_ra8_ok The operation completed successfully.
 * @retval other The originating validation, storage, stream, or network error.
 * @pre Every required pointer is non-null and remains valid for the call.
 * @post Documented outputs and the return value describe the same outcome.
 */
RA8_INTERNAL static ra8_err_t
internal_library_root(mdl_storage_t* storage, const char* root_path, bool* out_exists)
{
  *out_exists          = false;
  fw_fs_stat_t    root = {};
  const ra8_err_t err  = fw_fs_stat(&storage->fs->names, root_path, &root);
  if ((err != k_ra8_ok) || !root.exists) {
    return err;
  }
  if (root.type == k_fw_fs_node_symlink) {
    return k_ra8_err_access_denied;
  }
  *out_exists = root.type == k_fw_fs_node_directory;
  return *out_exists ? k_ra8_ok : k_ra8_err_invalid_arg;
}

/**
 * @brief Advance one cursor while authenticating entries between next calls.
 * @param[in,out] walk Active enumeration dependencies and callback.
 * @param[in] policy Explicit entry limit.
 * @param[in,out] workspace Cursor storage and exact counters.
 * @return Cursor, authentication, callback, or close status.
 * @pre Required pointers are non-NULL and the root is a real directory.
 * @post The cursor is consumed on every return path.
 * @note No backend lock is held during stat, state I/O, or user callback work.
 * @since 0.1.0

 * @details Advances bounded filesystem cursors without retaining backend locks.
 *          Callbacks and namespace mutations run only after cursor close.
 * @retval k_ra8_ok The operation completed successfully.
 * @retval other The originating validation, storage, stream, or network error.
 * @pre Every required pointer is non-null and remains valid for the call.
 * @post Documented outputs and the return value describe the same outcome.
 */
RA8_INTERNAL static ra8_err_t internal_library_enumerate(mdl_library_enumeration_t*  walk,
                                                         const mdl_library_policy_t* policy,
                                                         mdl_library_workspace_t*    workspace)
{
  fw_fs_dir_t directory = {};
  ra8_err_t   err       = fw_fs_dir_open(&walk->storage->fs->names,
                                         walk->root,
                                         &directory,
                                         workspace->directory_workspace,
                                         workspace->directory_workspace_bytes);
  while (err == k_ra8_ok) {
    fw_fs_dirent_value_t entry   = {};
    bool                 present = false;
    err                          = fw_fs_dir_next(&directory, &entry, &present);
    if ((err != k_ra8_ok) || !present) {
      break;
    }
    ++workspace->required_entries;
    if (workspace->required_entries > policy->max_entries) {
      err = k_ra8_err_invalid_size;
      break;
    }
    ++workspace->entries;
    bool keep_going = false;
    err             = internal_library_visit(walk, &entry, &keep_going);
    if ((err != k_ra8_ok) || !keep_going) {
      break;
    }
  }
  return internal_library_close(&directory, err);
}

ra8_err_t mdl_library_for_each(mdl_storage_t*              storage,
                               const char*                 out_dir,
                               mdl_state_t*                state_scratch,
                               mdl_library_workspace_t*    workspace,
                               const mdl_library_policy_t* policy,
                               mdl_library_fn              callback,
                               void*                       callback_ctx)
{
  if ((storage == nullptr) || (storage->fs == nullptr) || (out_dir == nullptr) ||
      (state_scratch == nullptr) || (workspace == nullptr) || (callback == nullptr) ||
      (workspace->directory_workspace == nullptr) || (workspace->directory_workspace_bytes == 0U)) {
    return k_ra8_err_invalid_arg;
  }
  ra8_err_t err = internal_library_policy(policy);
  internal_library_workspace_reset(workspace);
  workspace->entry_limit = (policy != nullptr) ? policy->max_entries : 0U;
  bool root_exists       = false;
  if (err == k_ra8_ok) {
    err = internal_library_root(storage, out_dir, &root_exists);
  }
  if ((err != k_ra8_ok) || !root_exists) {
    return err;
  }
  mdl_library_enumeration_t walk = {.storage      = storage,
                                    .root         = out_dir,
                                    .state        = state_scratch,
                                    .callback     = callback,
                                    .callback_ctx = callback_ctx};
  return internal_library_enumerate(&walk, policy, workspace);
}

/**
 * @brief Consume one removal namespace-operation budget unit.
 * @details Increments before a dependency call and rejects cap+1 without calling it.
 * @param[in] policy Active explicit traversal policy.
 * @param[in,out] workspace Active traversal counters.
 * @return Operation-budget status.
 * @retval k_ra8_ok One operation is reserved.
 * @retval k_ra8_err_invalid_size The exact budget was already consumed.
 * @pre Both pointers are non-NULL.
 * @pre `workspace->operations` never exceeds the policy bound.
 * @post Success increments the counter exactly once.
 * @post Failure leaves the counter unchanged.
 * @note Call immediately before each namespace operation.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_library_take_operation(const mdl_library_policy_t* policy,
                                                              mdl_library_workspace_t*    workspace)
{
  if (workspace->operations >= policy->max_operations) {
    return k_ra8_err_invalid_size;
  }
  ++workspace->operations;
  return k_ra8_ok;
}

/**
 * @brief Copy at most one child then close before any namespace mutation.
 * @param[in,out] storage Portable namespace binding.
 * @param[in] path Current canonical directory path.
 * @param[in] policy Active operation budget.
 * @param[in,out] workspace Cursor storage and operation counter.
 * @param[out] out Stable copied child value.
 * @param[out] out_present True when a child was copied.
 * @return Budget, cursor, next, or close status.
 * @pre Required pointers are non-NULL and no cursor is open.
 * @post No cursor remains open and @p out_present is initialized on success.
 * @note Three operations are reserved atomically so close can never be skipped.
 * @since 0.1.0

 * @details Advances bounded filesystem cursors without retaining backend locks.
 *          Callbacks and namespace mutations run only after cursor close.
 * @retval k_ra8_ok The operation completed successfully.
 * @retval other The originating validation, storage, stream, or network error.
 * @pre Every required pointer is non-null and remains valid for the call.
 * @post Documented outputs and the return value describe the same outcome.
 */
RA8_INTERNAL static ra8_err_t internal_library_first_child(mdl_storage_t*              storage,
                                                           const char*                 path,
                                                           const mdl_library_policy_t* policy,
                                                           mdl_library_workspace_t*    workspace,
                                                           fw_fs_dirent_value_t*       out,
                                                           bool*                       out_present)
{
  const uint32_t remaining = policy->max_operations - workspace->operations;
  if (remaining < 3U) {
    return k_ra8_err_invalid_size;
  }
  workspace->operations += 3U;
  fw_fs_dir_t directory = {};
  ra8_err_t   err       = fw_fs_dir_open(&storage->fs->names,
                                         path,
                                         &directory,
                                         workspace->directory_workspace,
                                         workspace->directory_workspace_bytes);
  if (err != k_ra8_ok) {
    workspace->operations -= 2U;
    return err;
  }
  err = fw_fs_dir_next(&directory, out, out_present);
  return internal_library_close(&directory, err);
}

/**
 * @brief Act on one revalidated child node according to its type.
 * @details Pushes a directory onto the DFS stack, rejects a symlink, or
 * unlinks a regular file.
 * @param[in,out] storage Portable namespace binding.
 * @param[in] policy Active traversal limits.
 * @param[in,out] workspace Active DFS stack and counters.
 * @param[in] path Canonical path of the revalidated node.
 * @param[in] node Freshly stat'd node value for @p path.
 * @return Canonical child-processing status.
 * @retval k_ra8_ok The directory was pushed or the file was removed.
 * @retval k_ra8_err_access_denied The node is a symlink.
 * @retval k_ra8_err_invalid_size The DFS depth budget was exhausted.
 * @retval k_ra8_err_invalid_arg The node is neither a directory, symlink,
 * nor a regular file.
 * @pre @p storage, @p policy, @p workspace, and @p path are non-NULL.
 * @pre @p node was produced by a fresh `fw_fs_stat` on @p path.
 * @post Success either increments @p workspace->depth exactly once or
 * removes exactly one file.
 * @post A symlink is rejected without being followed, opened, or unlinked.
 * @note Not thread-safe with respect to concurrent mutation of @p storage.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_library_act_on_node(mdl_storage_t*              storage,
                                                           const mdl_library_policy_t* policy,
                                                           mdl_library_workspace_t*    workspace,
                                                           const char*                 path,
                                                           fw_fs_stat_t                node)
{
  if (node.type == k_fw_fs_node_directory) {
    if (workspace->depth >= policy->max_depth) {
      return k_ra8_err_invalid_size;
    }
    ++workspace->depth;
    memcpy(workspace->paths[workspace->depth], path, strlen(path) + 1U);
    return k_ra8_ok;
  }
  if (node.type == k_fw_fs_node_symlink) {
    return k_ra8_err_access_denied;
  }
  if (node.type != k_fw_fs_node_file) {
    return k_ra8_err_invalid_arg;
  }
  const ra8_err_t err = internal_library_take_operation(policy, workspace);
  return (err == k_ra8_ok) ? fw_fs_unlink(&storage->fs->names, path) : err;
}

/**
 * @brief Remove or descend into one captured child.
 * @details Revalidates the stable leaf and node type after listdir closes,
 *          then delegates to ::internal_library_act_on_node.
 * @param[in,out] storage Portable namespace binding.
 * @param[in] policy Active traversal limits.
 * @param[in,out] workspace Active DFS stack and counters.
 * @param[in] child Stable copied child.
 * @return Canonical child-processing status.
 * @retval k_ra8_ok The file was removed or directory was pushed.
 * @retval k_ra8_err_access_denied A symlink was encountered.
 * @pre All pointers are non-NULL and the current stack path is canonical.
 * @pre @p child was produced by ::internal_library_first_child.
 * @post Success removes one file or increments depth exactly once.
 * @post Failure never follows or operates through a symbolic link.
 * @note Stat/list type disagreement is treated as a concurrent-mutation error.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_library_remove_child(mdl_storage_t*              storage,
                                                            const mdl_library_policy_t* policy,
                                                            mdl_library_workspace_t*    workspace,
                                                            const fw_fs_dirent_value_t* child)
{
  size_t    length = 0U;
  ra8_err_t err    = internal_library_leaf(storage, child, &length);
  (void)length;
  if (err != k_ra8_ok) {
    return err;
  }
  ++workspace->required_entries;
  if (workspace->required_entries > policy->max_entries) {
    return k_ra8_err_invalid_size;
  }
  ++workspace->entries;
  char path[k_fw_fs_path_cap];
  if (!mdl_path_join(workspace->paths[workspace->depth], child->name, path, sizeof(path))) {
    return k_ra8_err_invalid_size;
  }
  err               = internal_library_take_operation(policy, workspace);
  fw_fs_stat_t node = {};
  if (err == k_ra8_ok) {
    err = fw_fs_stat(&storage->fs->names, path, &node);
  }
  if (err != k_ra8_ok) {
    return err;
  }
  if (!node.exists || (node.type != child->type)) {
    return k_ra8_err_invalid_state;
  }
  return internal_library_act_on_node(storage, policy, workspace, path, node);
}

/**
 * @brief Validate and retain the root of one removal traversal.
 * @param[in,out] storage Portable namespace binding.
 * @param[in] dir Canonical candidate root.
 * @param[in] policy Validated removal policy.
 * @param[in,out] workspace Initialized idle traversal workspace.
 * @return Canonical root validation or stat status.
 * @pre Required pointers are non-NULL and policy bounds are valid.
 * @pre @p workspace has a live caller-owned entry binding.
 * @post Success retains @p dir at stack depth zero.
 * @post An absent root succeeds without retaining a path.
 * @note Symlinks and non-directory nodes fail closed.
 * @since 0.1.0

 * @details Advances bounded filesystem cursors without retaining backend locks.
 *          Callbacks and namespace mutations run only after cursor close.
 * @retval k_ra8_ok The operation completed successfully.
 * @retval other The originating validation, storage, stream, or network error.
 */
RA8_INTERNAL static ra8_err_t internal_library_remove_root(mdl_storage_t*              storage,
                                                           const char*                 dir,
                                                           const mdl_library_policy_t* policy,
                                                           mdl_library_workspace_t*    workspace)
{
  internal_library_workspace_reset(workspace);
  workspace->entry_limit = policy->max_entries;
  if ((dir[0] == '/') && (dir[1] == '\0')) {
    return k_ra8_err_access_denied;
  }
  ra8_err_t    err  = internal_library_take_operation(policy, workspace);
  fw_fs_stat_t root = {};
  if (err == k_ra8_ok) {
    err = fw_fs_stat(&storage->fs->names, dir, &root);
  }
  if ((err != k_ra8_ok) || !root.exists) {
    return err;
  }
  if (root.type == k_fw_fs_node_symlink) {
    return k_ra8_err_access_denied;
  }
  if (root.type != k_fw_fs_node_directory) {
    return k_ra8_err_invalid_arg;
  }
  const size_t root_length = strnlen(dir, sizeof(workspace->paths[0]));
  if (root_length >= sizeof(workspace->paths[0])) {
    return k_ra8_err_invalid_size;
  }
  memcpy(workspace->paths[0], dir, root_length + 1U);
  return k_ra8_ok;
}

/**
 * @brief Execute iterative post-order removal from a retained valid root.
 * @param[in,out] storage Portable namespace binding.
 * @param[in] policy Validated removal policy.
 * @param[in,out] workspace Active root path, stack, and counters.
 * @return Canonical traversal or namespace status.
 * @pre Required pointers are non-NULL and stack depth zero holds a directory.
 * @pre No directory listing handle is live on entry.
 * @post Success removes the complete root tree.
 * @post Failure reports the first fault after any already completed deletion.
 * @note Each list handle closes before stat, descent, unlink, or rmdir.
 * @since 0.1.0

 * @details Advances bounded filesystem cursors without retaining backend locks.
 *          Callbacks and namespace mutations run only after cursor close.
 * @retval k_ra8_ok The operation completed successfully.
 * @retval other The originating validation, storage, stream, or network error.
 */
RA8_INTERNAL static ra8_err_t internal_library_remove_walk(mdl_storage_t*              storage,
                                                           const mdl_library_policy_t* policy,
                                                           mdl_library_workspace_t*    workspace)
{
  for (;;) {
    fw_fs_dirent_value_t child   = {};
    bool                 present = false;
    ra8_err_t            err     = internal_library_first_child(storage,
                                                                workspace->paths[workspace->depth],
                                                                policy,
                                                                workspace,
                                                                &child,
                                                                &present);
    if (err != k_ra8_ok) {
      return err;
    }
    if (present) {
      err = internal_library_remove_child(storage, policy, workspace, &child);
      if (err != k_ra8_ok) {
        return err;
      }
      continue;
    }
    err = internal_library_take_operation(policy, workspace);
    if (err == k_ra8_ok) {
      err = fw_fs_rmdir(&storage->fs->names, workspace->paths[workspace->depth]);
    }
    if ((err != k_ra8_ok) || (workspace->depth == 0U)) {
      return err;
    }
    --workspace->depth;
  }
}

ra8_err_t mdl_library_remove_tree(mdl_storage_t*              storage,
                                  const char*                 dir,
                                  const mdl_library_policy_t* policy,
                                  mdl_library_workspace_t*    workspace)
{
  if ((storage == nullptr) || (storage->fs == nullptr) || (dir == nullptr) ||
      (workspace == nullptr) || (workspace->directory_workspace == nullptr) ||
      (workspace->directory_workspace_bytes == 0U)) {
    return k_ra8_err_invalid_arg;
  }
  ra8_err_t err = internal_library_policy(policy);
  if (err == k_ra8_ok) {
    err = internal_library_remove_root(storage, dir, policy, workspace);
  }
  return ((err == k_ra8_ok) && (workspace->paths[0][0] != '\0'))
           ? internal_library_remove_walk(storage, policy, workspace)
           : err;
}
