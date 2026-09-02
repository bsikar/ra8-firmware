/**
 * @file mdl_app_library.c
 * @brief Tracked-library list, remove, and update-all actions.
 * @details Enumerates only verified state markers through injected storage and
 *          dispatches the selected bounded management action synchronously.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#include "mdl_app_internal.h"

typedef struct {
  size_t found; /**< Authenticated tracked series encountered. */
} list_ctx_t;

/** @brief Write up to three borrowed fragments through one selected sink.
 * @details Authenticates bounded paths through the injected storage facade.
 *          Enumeration or removal failures are reported without claiming success.
 * @param[in,out] stream Destination stream state.
 * @param[in] first First text fragment.
 * @param[in] second Second text fragment.
 * @param[in] third Third text fragment.
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
RA8_INTERNAL static ra8_err_t internal_library_text3(ra8_io_stream_t* stream,
                                                     const char*      first,
                                                     const char*      second,
                                                     const char*      third)
{
  ra8_err_t error = priv_mdl_stream_text(k_ra8_ok, stream, first);
  error           = priv_mdl_stream_text(error, stream, second);
  return priv_mdl_stream_text(error, stream, third);
}

/**
 * @brief Report a portable library-enumeration failure in user-facing terms.
 * @param[in] action Human-readable action that failed.
 * @param[in] error Canonical library error.
 * @pre @p action is non-NULL and NUL-terminated.
 * @pre @p error is not ::k_ra8_ok.
 * @post One actionable diagnostic is emitted to standard error.
 * @post No library or callback state is modified.
 * @note Capacity diagnostics use exact observations retained by the workspace.
 * @since 0.1.0

 * @details Authenticates bounded paths through the injected storage facade.
 *          Enumeration or removal failures are reported without claiming success.
 * @return Operation status.
 * @retval k_ra8_ok The operation completed successfully.
 * @retval other The originating validation, storage, stream, or network error.
 */
RA8_INTERNAL static ra8_err_t internal_library_report(const char* action, ra8_err_t error)
{
  if (error == k_ra8_err_invalid_state) {
    return internal_library_text3(priv_mdl_app_context()->diagnostic,
                                  "mdl: state file unreadable or corrupt while ",
                                  action,
                                  "\n");
  }
  if (error == k_ra8_err_invalid_size) {
    const mdl_library_workspace_t* workspace = &priv_mdl_app_context()->library_workspace;
    ra8_err_t output_error = internal_library_text3(priv_mdl_app_context()->diagnostic,
                                                    "mdl: library capacity exceeded while ",
                                                    action,
                                                    " (observed ");
    output_error           = priv_mdl_stream_u64(output_error,
                                                 priv_mdl_app_context()->diagnostic,
                                                 workspace->required_entries);
    output_error =
      priv_mdl_stream_text(output_error, priv_mdl_app_context()->diagnostic, ", limit ");
    output_error =
      priv_mdl_stream_u64(output_error, priv_mdl_app_context()->diagnostic, workspace->entry_limit);
    return priv_mdl_stream_text(output_error, priv_mdl_app_context()->diagnostic, ")\n");
  }
  if (error == k_ra8_err_hw_not_ready) {
    return internal_library_text3(priv_mdl_app_context()->diagnostic,
                                  "mdl: library storage unavailable while ",
                                  action,
                                  "\n");
  }
  ra8_err_t output_error = internal_library_text3(priv_mdl_app_context()->diagnostic,
                                                  "mdl: library operation failed while ",
                                                  action,
                                                  " (");
  output_error =
    priv_mdl_stream_u64(output_error, priv_mdl_app_context()->diagnostic, (uint32_t)error);
  return priv_mdl_stream_text(output_error, priv_mdl_app_context()->diagnostic, ")\n");
}

/**
 * @brief Print one tracked series for library-list mode.
 * @details Prints the canonical directory, source URL, and coverage from the
 *          state generation authenticated by the library enumerator.
 * @param[in] series_dir Canonical tracked-series directory.
 * @param[in] state_path Complete `.mdl_state` path.
 * @param[in] state Already authenticated state generation.
 * @param[in,out] ctx Pointer to a ::list_ctx_t accumulator.
 * @param[out] out_continue Set true to visit the remaining series.
 * @return Iterator continuation result.
 * @retval k_ra8_ok Continue enumeration, including after unreadable state.
 * @pre All pointer arguments are non-NULL.
 * @pre @p ctx points to writable ::list_ctx_t storage.
 * @post `found` increases exactly once.
 * @post The callback requests continued enumeration.
 * @note Not thread-safe because standard output is shared.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_list_cb(const char*        series_dir,
                                               const char*        state_path,
                                               const mdl_state_t* state,
                                               void*              ctx,
                                               bool*              out_continue)
{
  list_ctx_t* list = (list_ctx_t*)ctx;
  (void)state_path;
  list->found += 1U;
  char cov[k_cov_bytes];
  mdl_state_coverage(state, cov, sizeof(cov));
  ra8_err_t output_error =
    internal_library_text3(priv_mdl_app_context()->output, "  ", series_dir, "\n    url: ");
  output_error =
    priv_mdl_stream_text(output_error,
                         priv_mdl_app_context()->output,
                         (state->series_url[0] != '\0') ? state->series_url : "(unknown)");
  output_error  = priv_mdl_stream_text(output_error, priv_mdl_app_context()->output, "\n    ");
  output_error  = priv_mdl_stream_text(output_error, priv_mdl_app_context()->output, cov);
  output_error  = priv_mdl_stream_text(output_error, priv_mdl_app_context()->output, "\n");
  *out_continue = output_error == k_ra8_ok;
  return output_error;
}

int mdl_app_run_list(const char* out_dir)
{
  list_ctx_t                 list   = {};
  const mdl_library_policy_t policy = mdl_library_policy_default();
  const ra8_err_t            rc = mdl_library_for_each(&priv_mdl_app_context()->storage,
                                                       out_dir,
                                                       &priv_mdl_app_context()->state,
                                                       &priv_mdl_app_context()->library_workspace,
                                                       &policy,
                                                       internal_list_cb,
                                                       &list);
  if (rc != k_ra8_ok) {
    (void)internal_library_report("listing tracked series", rc);
  } else if (list.found == 0U) {
    if (internal_library_text3(priv_mdl_app_context()->output,
                               "no tracked series under ",
                               out_dir,
                               "\n") != k_ra8_ok) {
      return 1;
    }
  }
  return (rc == k_ra8_ok) ? 0 : 1;
}

/**
 * @brief Resolve and authenticate one tracked library-removal target.
 * @param[in] out_dir Library root.
 * @param[in] url_or_slug User-selected series URL or slug.
 * @param[out] dir Complete confined target directory.
 * @return Whether @p dir names the authenticated tracked series.
 * @retval true The target is a real directory with matching authenticated state.
 * @retval false The path, marker, state, or recorded identity was rejected.
 * @pre All pointers are non-NULL; @p dir covers `k_dir_path_bytes` bytes.
 * @post Success authorizes only the later guarded removal call.
 * @post Failure does not mutate the library.
 * @note Not safe for concurrent mutation of the target tree.
 * @since 0.1.0

 * @details Authenticates bounded paths through the injected storage facade.
 *          Enumeration or removal failures are reported without claiming success.
 * @pre Every required pointer is non-null and remains valid for the call.
 */
RA8_INTERNAL static bool
internal_resolve_removal_target(const char* out_dir, const char* url_or_slug, char* dir)
{
  char slug[k_slug_bytes];
  mdl_urlname_last_segment(url_or_slug, slug, sizeof(slug));
  if (!mdl_path_join(out_dir, slug, dir, k_dir_path_bytes)) {
    (void)internal_library_text3(priv_mdl_app_context()->diagnostic,
                                 "mdl: cannot resolve series '",
                                 url_or_slug,
                                 "'\n");
    return false;
  }
  fw_fs_stat_t dir_stat = {};
  if ((fw_fs_stat(&priv_mdl_app_context()->storage.fs->names, dir, &dir_stat) != k_ra8_ok) ||
      !dir_stat.exists || (dir_stat.type != k_fw_fs_node_directory)) {
    (void)internal_library_text3(priv_mdl_app_context()->diagnostic,
                                 "mdl: refusing to remove unsafe series path '",
                                 dir,
                                 "'\n");
    return false;
  }
  char state_path[PATH_MAX];
  bool state_exists = false;
  if (!priv_mdl_app_state_path_of(dir, state_path, sizeof(state_path)) ||
      (mdl_state_probe(&priv_mdl_app_context()->storage, state_path, &state_exists) != k_ra8_ok) ||
      !state_exists) {
    (void)internal_library_text3(priv_mdl_app_context()->diagnostic,
                                 "mdl: refusing to remove untracked directory '",
                                 dir,
                                 "' (missing or unsafe .mdl_state)\n");
    return false;
  }
  if (mdl_state_load_authenticated(&priv_mdl_app_context()->storage,
                                   state_path,
                                   &priv_mdl_app_context()->state) != k_ra8_ok) {
    (void)internal_library_text3(priv_mdl_app_context()->diagnostic,
                                 "mdl: refusing to remove '",
                                 dir,
                                 "' (state is unreadable or corrupt)\n");
    return false;
  }
  char recorded_slug[k_slug_bytes];
  mdl_urlname_last_segment(priv_mdl_app_context()->state.series_url,
                           recorded_slug,
                           sizeof(recorded_slug));
  if ((priv_mdl_app_context()->state.series_url[0] != '\0') && (strcmp(recorded_slug, slug) == 0)) {
    return true;
  }
  (void)internal_library_text3(priv_mdl_app_context()->diagnostic,
                               "mdl: refusing to remove '",
                               dir,
                               "' (state identity does not match target)\n");
  return false;
}

int mdl_app_run_remove(const char* out_dir, const char* url_or_slug)
{
  char dir[k_dir_path_bytes];
  if (!internal_resolve_removal_target(out_dir, url_or_slug, dir)) {
    return 1;
  }
  const mdl_library_policy_t policy = mdl_library_policy_default();
  if (mdl_library_remove_tree(&priv_mdl_app_context()->storage,
                              dir,
                              &policy,
                              &priv_mdl_app_context()->library_workspace) != k_ra8_ok) {
    (void)internal_library_text3(priv_mdl_app_context()->diagnostic,
                                 "mdl: failed to remove ",
                                 dir,
                                 "\n");
    return 1;
  }
  return (internal_library_text3(priv_mdl_app_context()->output, "removed ", dir, "\n") == k_ra8_ok)
           ? 0
           : 1;
}

/**
 * @struct update_all_ctx_t
 * @brief Context threaded through ::internal_update_all_cb for `--update-all`.
 * @since 0.1.0
 */
typedef struct {
  const mdl_series_run_t* base;    /**< Template run (format/knobs); url filled per series. */
  size_t                  updated; /**< Count of series updated.                            */
  size_t                  failed;  /**< Count of series skipped or unsuccessfully updated.  */
} update_all_ctx_t;

/**
 * @brief Incrementally update one visited tracked series.
 * @details Uses authenticated URL/config identity, derives a run from the
 *          caller's template, and records success or a nonfatal series failure.
 * @param[in] series_dir Canonical tracked-series directory.
 * @param[in] state_path Complete `.mdl_state` path.
 * @param[in] state Already authenticated state generation.
 * @param[in,out] ctx Pointer to an ::update_all_ctx_t accumulator.
 * @param[out] out_continue Set true to visit the remaining series.
 * @return Iterator continuation result.
 * @retval k_ra8_ok Continue enumeration after either outcome.
 * @pre All pointer arguments are non-NULL.
 * @pre @p ctx points to a valid template and writable counters.
 * @post Exactly one of `updated` or `failed` increases.
 * @post Unreadable or incomplete identity is never overwritten.
 * @note Not thread-safe because it invokes ::mdl_app_run_series with shared state.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_update_all_cb(const char*        series_dir,
                                                     const char*        state_path,
                                                     const mdl_state_t* state,
                                                     void*              ctx,
                                                     bool*              out_continue)
{
  update_all_ctx_t* p = (update_all_ctx_t*)ctx;
  (void)state_path;
  char url[k_mdl_url_max];
  char cfg[k_mdl_cfgpath_max];
  (void)snprintf(url, sizeof(url), "%s", state->series_url);
  (void)snprintf(cfg,
                 sizeof(cfg),
                 "%s",
                 (state->config_path[0] != '\0') ? state->config_path : p->base->cfg_path);
  if ((url[0] == '\0') || (cfg[0] == '\0')) {
    const ra8_err_t output_error = internal_library_text3(priv_mdl_app_context()->output,
                                                          "skip ",
                                                          series_dir,
                                                          " (no series URL / descriptor "
                                                          "recorded)\n");
    p->failed += 1U;
    *out_continue = output_error == k_ra8_ok;
    return output_error;
  }
  const ra8_err_t output_error =
    internal_library_text3(priv_mdl_app_context()->output, "updating ", url, "\n");
  if (output_error != k_ra8_ok) {
    *out_continue = false;
    return output_error;
  }
  mdl_series_run_t run = *p->base;
  run.series_url       = url;
  run.cfg_path         = cfg;
  run.update           = true;
  if (mdl_app_run_series(&run) == 0) {
    p->updated += 1U;
  } else {
    p->failed += 1U;
  }
  *out_continue = true;
  return k_ra8_ok;
}

int mdl_app_run_update_all(const mdl_series_run_t* base)
{
  update_all_ctx_t           c      = {.base = base, .updated = 0U, .failed = 0U};
  const mdl_library_policy_t policy = mdl_library_policy_default();
  const ra8_err_t            rc = mdl_library_for_each(&priv_mdl_app_context()->storage,
                                                       base->out_dir,
                                                       &priv_mdl_app_context()->state,
                                                       &priv_mdl_app_context()->library_workspace,
                                                       &policy,
                                                       internal_update_all_cb,
                                                       &c);
  if (c.updated == 0U) {
    const ra8_err_t output_error = internal_library_text3(priv_mdl_app_context()->output,
                                                          "no tracked series to update under ",
                                                          base->out_dir,
                                                          "\n");
    if (output_error != k_ra8_ok) {
      return 1;
    }
  }
  if (c.failed > 0U) {
    ra8_err_t output_error =
      priv_mdl_stream_text(k_ra8_ok, priv_mdl_app_context()->diagnostic, "mdl: ");
    output_error = priv_mdl_stream_u64(output_error, priv_mdl_app_context()->diagnostic, c.failed);
    output_error = priv_mdl_stream_text(output_error,
                                        priv_mdl_app_context()->diagnostic,
                                        " series failed to update\n");
    if (output_error != k_ra8_ok) {
      return 1;
    }
  }
  if (rc != k_ra8_ok) {
    (void)internal_library_report("updating tracked series", rc);
  }
  return ((rc == k_ra8_ok) && (c.failed == 0U)) ? 0 : 1;
}
