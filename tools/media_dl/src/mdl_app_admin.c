/**
 * @file mdl_app_admin.c
 * @brief Discovery, descriptor generation, and verification actions.
 * @details Owns bounded CLI administration workflows: site discovery, atomic
 *          starter-descriptor publication, and read-only library verification.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#include "mdl_app_internal.h"

/** @brief Bounded titled discovery hits for the active query. */
static mdl_hit_list_t s_results;

/** @brief Latch the first administration presenter failure.
 * @details Uses bounded application and filesystem state with injected output.
 *          The first failure is latched and no later operation overwrites it.
 * @param[in] error Candidate failure value to latch.
 * @pre Every required pointer is non-null and remains valid for the call.
 * @pre Lengths and capacities describe complete referenced objects without overflow.
 * @post Documented outputs and the return value describe the same outcome.
 * @post A rejected or failed operation is never reported as successful.
 * @note Thread safety follows ownership of the supplied context; no synchronization is added.
 * @since Version 0.1.0
 */
RA8_INTERNAL static void internal_admin_latch(ra8_err_t error)
{
  if ((error != k_ra8_ok) && (priv_mdl_app_context()->io_error == k_ra8_ok)) {
    priv_mdl_app_context()->io_error = error;
  }
}

/** @brief Append three borrowed administration fragments and latch failure.
 * @details Uses bounded application and filesystem state with injected output.
 *          The first failure is latched and no later operation overwrites it.
 * @param[in,out] stream Destination stream state.
 * @param[in] first First text fragment.
 * @param[in] second Second text fragment.
 * @param[in] third Third text fragment.
 * @pre Every required pointer is non-null and remains valid for the call.
 * @pre Lengths and capacities describe complete referenced objects without overflow.
 * @post Documented outputs and the return value describe the same outcome.
 * @post A rejected or failed operation is never reported as successful.
 * @note Thread safety follows ownership of the supplied context; no synchronization is added.
 * @since Version 0.1.0
 */
RA8_INTERNAL static void internal_admin_text3(ra8_io_stream_t* stream,
                                              const char*      first,
                                              const char*      second,
                                              const char*      third)
{
  ra8_err_t error = priv_mdl_stream_text(k_ra8_ok, stream, first);
  error           = priv_mdl_stream_text(error, stream, second);
  internal_admin_latch(priv_mdl_stream_text(error, stream, third));
}

/**
 * @brief Run search or browse discovery and optionally download one result.
 * @details Loads the descriptor, establishes policy/governor state, lists
 *          bounded hits, and feeds an explicitly selected URL to series mode.
 * @param[in] a Parsed command-line arguments.
 * @param[in] opts Validated run policy.
 * @param[in] n Validated numeric arguments.
 * @param[in] base Template series-run parameters.
 * @return Process-style status from discovery or the selected series run.
 * @retval 0 Results were listed or the selected series succeeded.
 * @retval nonzero Descriptor, network, discovery, selection, or series work
 * failed.
 * @pre All pointer arguments are non-NULL.
 * @pre Search/browse mode and descriptor requirements passed CLI validation.
 * @post The discovery network interface is destroyed before return.
 * @post A picked URL is downloaded only when selection produced a complete URL.
 * @note Not thread-safe because it uses shared discovery buffers.
 * @since 0.1.0
 */
RA8_PRIV int priv_mdl_app_run_discover(const mdl_args_t*     a,
                                       const mdl_run_opts_t* opts,
                                       const mdl_nums_t*     n,
                                       const series_run_t*   base)
{
  if (a->cfg == nullptr) {
    (void)priv_mdl_stream_text(k_ra8_ok,
                               priv_mdl_app_context()->diagnostic,
                               "media_dl: --search/--browse requires --config SITE.conf\n");
    return 2;
  }
  mdl_site_t site;
  if (mdl_config_load(&priv_mdl_app_context()->storage, a->cfg, &site) != k_ra8_ok) {
    return 1;
  }
  if (opts->polite) {
    mdl_config_apply_polite(&site);
  }
  mdl_net_iface_t        net     = {};
  mdl_net_curl_storage_t storage = {};
  if (mdl_net_curl_init(&net, &storage, &opts->policy) != k_ra8_ok) {
    (void)priv_mdl_stream_text(k_ra8_ok,
                               priv_mdl_app_context()->diagnostic,
                               "media_dl: network init failed\n");
    return 1;
  }
  char ua[k_mdl_ua_max];
  if (priv_mdl_app_start_session(&net, opts, site.contact, ua, sizeof(ua)) != k_ra8_ok) {
    mdl_net_destroy(&net);
    return 1;
  }

  mdl_governor_t      gov;
  const mdl_gov_cfg_t cfg = mdl_config_gov_cfg(&site);
  mdl_governor_init(&gov, &cfg, n->seed);

  const mdl_discover_req_t req = {
    .session    = &priv_mdl_app_context()->session,
    .gov        = &gov,
    .site       = &site,
    .mode       = a->browse ? k_mdl_discover_browse : k_mdl_discover_search,
    .term       = a->search,
    .timeout_ms = n->timeout,
    .page_buf   = priv_mdl_app_context()->page,
    .page_cap   = sizeof(priv_mdl_app_context()->page),
    .hits       = &s_results,
    .output     = priv_mdl_app_context()->output,
    .diagnostic = priv_mdl_app_context()->diagnostic,
    .io_error   = &priv_mdl_app_context()->io_error,
  };
  char      picked[k_mdl_url_max] = {};
  const int rc                    = mdl_discover_run(&req, n->pick, picked, sizeof(picked));
  mdl_net_destroy(&net);
  if ((rc != 0) || (picked[0] == '\0')) {
    return rc; /* listed results, or an error, with nothing to download */
  }
  series_run_t run = *base;
  run.series_url   = picked; /* combined search-and-select: feed the pick in */
  return priv_mdl_app_run_series(&run);
}

/**
 * @brief Derive bounded descriptor identity and destination fields.
 * @details Coordinates init site identity with fixed application workspaces and
 * propagates validation, storage, or network failure to the selected command
 * runner.
 * @param[in] url Absolute site URL.
 * @param[out] host Complete normalized URL host.
 * @param[out] slug First non-www host label, or `site` when empty.
 * @param[out] name Display name derived from @p slug.
 * @param[in] descriptor_dir Canonical composition-selected descriptor
 * directory.
 * @param[out] out_path Descriptor destination beneath @p descriptor_dir.
 * @return Whether all required identity fields were derived.
 * @retval true Every output contains a complete NUL-terminated value.
 * @retval false @p url was absent or its host was invalid.
 * @pre Output arrays use their corresponding compile-time capacities.
 * @pre @p url is NULL or NUL-terminated.
 * @post Failure emits the original usage or host diagnostic.
 * @post No filesystem content is created or modified.
 * @note Thread-safe for distinct caller-owned outputs.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_init_site_identity(const char* url,
                                                     const char* descriptor_dir,
                                                     char*       host,
                                                     char*       slug,
                                                     char*       name,
                                                     char*       out_path)
{
  if ((url == nullptr) || (url[0] == '\0') || (descriptor_dir == nullptr) ||
      (descriptor_dir[0] != '/')) {
    (void)priv_mdl_stream_text(k_ra8_ok,
                               priv_mdl_app_context()->diagnostic,
                               "media_dl: --init-site requires a URL\n");
    return false;
  }
  if (!mdl_url_host(url, host, k_mdl_host_max)) {
    internal_admin_text3(priv_mdl_app_context()->diagnostic,
                         "media_dl: --init-site could not extract host from '",
                         url,
                         "'\n");
    return false;
  }
  const char* hptr = (strncmp(host, "www.", 4) == 0) ? host + 4 : host;
  size_t      i    = 0U;
  for (; (hptr[i] != '\0') && (hptr[i] != '.') && (i + 1U < k_mdl_name_max); ++i) {
    slug[i] = hptr[i];
  }
  slug[i] = '\0';
  if (slug[0] == '\0') {
    (void)snprintf(slug, k_mdl_name_max, "site");
  }
  (void)snprintf(name, k_mdl_name_max, "%s", slug);
  if ((name[0] >= 'a') && (name[0] <= 'z')) {
    name[0] = (char)(name[0] - 'a' + 'A');
  }
  char      leaf[k_leaf_name_bytes];
  const int leaf_bytes = snprintf(leaf, sizeof(leaf), "%s.conf", slug);
  return (leaf_bytes > 0) && ((size_t)leaf_bytes < sizeof(leaf)) &&
         mdl_path_join(descriptor_dir, leaf, out_path, (size_t)k_fw_fs_path_cap);
}

/**
 * @brief Generate a starter site descriptor from one URL.
 * @details Extracts and normalizes the host into bounded name/path fields,
 *          writes a commented conservative descriptor to a sibling temp, and
 *          publishes it with a no-replace hard link.
 * @param[in] url Absolute site URL.
 * @param[in] descriptor_dir Canonical directory that receives the descriptor.
 * @return Process-style status.
 * @retval 0 The descriptor template was created.
 * @retval 1 The destination file could not be created.
 * @retval 2 The URL was absent or its host could not be extracted.
 * @pre @p url is either NULL or points to a NUL-terminated string.
 * @pre CLI validation selected init-site mode.
 * @post Success creates one template without dynamic allocation.
 * @post Failure reports the rejected URL or destination and preserves any
 *       existing descriptor byte-for-byte.
 * @note Not safe for concurrent creation of the same descriptor path.
 * @since 0.1.0
 */
RA8_PRIV int priv_mdl_app_run_init_site(const char* url, const char* descriptor_dir)
{
  char host[k_mdl_host_max] = {};
  char slug[k_mdl_name_max] = {};
  char name[k_mdl_name_max] = {};
  char out_path[k_fw_fs_path_cap];
  if (!internal_init_site_identity(url, descriptor_dir, host, slug, name, out_path)) {
    return 2;
  }
  if (priv_mdl_app_storage_ensure_directory(&priv_mdl_app_context()->storage, descriptor_dir) !=
      k_ra8_ok) {
    internal_admin_text3(priv_mdl_app_context()->diagnostic,
                         "media_dl: descriptor directory is unsafe '",
                         descriptor_dir,
                         "'\n");
    return 1;
  }
  const ra8_err_t published =
    priv_mdl_app_storage_publish_site(&priv_mdl_app_context()->storage, out_path, url, host, name);
  if (published == k_ra8_err_exists) {
    internal_admin_text3(priv_mdl_app_context()->diagnostic,
                         "media_dl: refusing to overwrite existing site descriptor '",
                         out_path,
                         "'\n");
    return 1;
  }
  if (published != k_ra8_ok) {
    internal_admin_text3(priv_mdl_app_context()->diagnostic,
                         "media_dl: failed to publish site descriptor '",
                         out_path,
                         "'\n");
    return 1;
  }

  ra8_io_stream_t* output = priv_mdl_app_context()->output;
  ra8_err_t        error =
    priv_mdl_stream_text(k_ra8_ok, output, "generated starter site descriptor template: ");
  error = priv_mdl_stream_text(error, output, out_path);
  error = priv_mdl_stream_text(error, output, "\nedit ");
  error = priv_mdl_stream_text(error, output, out_path);
  error =
    priv_mdl_stream_text(error,
                         output,
                         " to tune scraping selectors, then test with:\n  media_dl --config ");
  error = priv_mdl_stream_text(error, output, out_path);
  error = priv_mdl_stream_text(error, output, " --series ");
  error = priv_mdl_stream_text(error, output, url);
  error = priv_mdl_stream_text(error, output, "\n");
  return (error == k_ra8_ok) ? 0 : 1;
}

/** @brief Stats accumulated during a library/series verification run. */
typedef struct {
  size_t series_checked;   /**< Tracked series verified.               */
  size_t pages_valid;      /**< Pages matching content hash on disk.   */
  size_t pages_missing;    /**< Page files missing from disk.          */
  size_t pages_corrupt;    /**< Page files failing hash match.         */
  size_t archives_checked; /**< Archive files checked.                 */
  size_t archives_valid;   /**< Structurally valid artifacts.          */
  size_t archives_corrupt; /**< Archive files empty/corrupt.           */
  size_t state_errors;     /**< State files that could not be parsed.  */
  size_t fs_errors;        /**< Directory/path operations that failed. */
} verify_stats_t;

/**
 * @brief Classify one recognized artifact directory entry.
 * @param[in] dir Canonical directory containing @p entry.
 * @param[in] entry Complete cursor value to inspect without following links.
 * @param[in,out] st Verification counters to update.
 * @pre All pointer arguments are non-NULL.
 * @post Unrecognized suffixes leave @p st unchanged; recognized entries
 * increment `archives_checked` and exactly one outcome counter.
 * @note Not thread-safe because validation uses the shared exporter workspace.
 * @since 0.1.0

 * @details Uses bounded application and filesystem state with injected output.
 *          The first failure is latched and no later operation overwrites it.
 * @pre Every required pointer is non-null and remains valid for the call.
 * @post Documented outputs and the return value describe the same outcome.
 */
RA8_INTERNAL static void internal_verify_artifact_entry(const char*                 dir,
                                                        const fw_fs_dirent_value_t* entry,
                                                        verify_stats_t*             st)
{
  ra8_mdl_format_t format = k_ra8_mdl_format_invalid;
  if (mdl_format_from_path(entry->name, &format) != k_ra8_ok) {
    return;
  }
  st->archives_checked += 1U;
  char         path[k_fw_fs_path_cap];
  fw_fs_stat_t node = {};
  if ((entry->type != k_fw_fs_node_file) || !mdl_path_join(dir, entry->name, path, sizeof(path)) ||
      (fw_fs_stat(&priv_mdl_app_context()->storage.fs->names, path, &node) != k_ra8_ok) ||
      !node.exists || (node.type != entry->type)) {
    internal_admin_text3(priv_mdl_app_context()->output,
                         "  [CORRUPT ARTIFACT] ",
                         entry->name,
                         " (not a regular file)\n");
    st->archives_corrupt += 1U;
    return;
  }
  mdl_verify_report_t report = {};
  const ra8_err_t     rc     = mdl_verify_file(&priv_mdl_app_context()->storage,
                                               format,
                                               path,
                                               &priv_mdl_app_context()->export_ws,
                                               &report);
  if (rc != k_ra8_ok) {
    ra8_io_stream_t* output = priv_mdl_app_context()->output;
    ra8_err_t        error  = priv_mdl_stream_text(k_ra8_ok, output, "  [");
    error = priv_mdl_stream_text(error,
                                 output,
                                 mdl_format_is_verifiable(format) ? "CORRUPT" : "UNSUPPORTED");
    error = priv_mdl_stream_text(error, output, " ARTIFACT] ");
    error = priv_mdl_stream_text(error, output, entry->name);
    internal_admin_latch(priv_mdl_stream_text(error, output, "\n"));
    st->archives_corrupt += 1U;
    return;
  }
  ra8_io_stream_t* output = priv_mdl_app_context()->output;
  ra8_err_t        error  = priv_mdl_stream_text(k_ra8_ok, output, "  [VALID ARTIFACT] ");
  error                   = priv_mdl_stream_text(error, output, entry->name);
  error                   = priv_mdl_stream_text(error, output, " (");
  error                   = priv_mdl_stream_u64(error, output, report.page_count);
  error                   = priv_mdl_stream_text(error, output, " page(s), ");
  error                   = priv_mdl_stream_u64(error, output, report.member_count);
  internal_admin_latch(priv_mdl_stream_text(error, output, " member(s))\n"));
  st->archives_valid += 1U;
}

/**
 * @brief Structurally validate recognized artifacts in one directory.
 * @details Infers supported formats from complete suffixes, rejects nonregular
 *          candidates, and validates contents through format reader paths.
 * @param[in] dir Canonical directory to scan.
 * @param[in,out] st Verification counters to update.
 * @pre @p dir and @p st are non-NULL.
 * @pre ::priv_mdl_app_context()->export_ws is initialized for bounded
 * validation scratch.
 * @post Every recognized directory entry increments `archives_checked` once.
 * @post Each checked artifact increments exactly one outcome counter.
 * @note Not thread-safe because it uses the shared exporter workspace.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_verify_artifacts(const char* dir, verify_stats_t* st)
{
  fw_fs_dir_t directory = {};
  ra8_err_t   error =
    fw_fs_dir_open(&priv_mdl_app_context()->storage.fs->names,
                   dir,
                   &directory,
                   priv_mdl_app_context()->admin_directory_workspace.bytes,
                   (uint32_t)sizeof(priv_mdl_app_context()->admin_directory_workspace.bytes));
  if (error != k_ra8_ok) {
    internal_admin_text3(priv_mdl_app_context()->output, "  [VERIFY FAIL] cannot open ", dir, "\n");
    st->fs_errors += 1U;
    return;
  }
  while (error == k_ra8_ok) {
    fw_fs_dirent_value_t entry   = {};
    bool                 present = false;
    error                        = fw_fs_dir_next(&directory, &entry, &present);
    if ((error != k_ra8_ok) || !present) {
      break;
    }
    if (entry.name[0] == '.') {
      continue;
    }
    internal_verify_artifact_entry(dir, &entry, st);
    if (priv_mdl_app_context()->io_error != k_ra8_ok) {
      break;
    }
  }
  if (error != k_ra8_ok) {
    internal_admin_text3(priv_mdl_app_context()->output,
                         "  [VERIFY FAIL] cannot finish reading ",
                         dir,
                         "\n");
    st->fs_errors += 1U;
  }
  const ra8_err_t closed = fw_fs_dir_close(&directory);
  if (closed != k_ra8_ok) {
    st->fs_errors += 1U;
  }
}

/**
 * @brief Verify one tracked page's presence and content hash.
 * @details Resolves the page's canonical path beneath the series root,
 *          confirms it exists as a regular file, then compares its
 *          content hash against the recorded value.
 * @param[in] series_dir Canonical tracked-series directory.
 * @param[in] rec Recorded page whose file and hash are checked.
 * @param[in,out] st Verification counters to update.
 * @return Whether the caller's page loop should keep going.
 * @retval true One of `pages_missing`, `pages_corrupt`, or `pages_valid`
 *         increased; continue with the next page.
 * @retval false A diagnostic write hit the shared I/O error latch; the
 *         caller must abort the whole series verification.
 * @pre All pointer arguments are non-NULL.
 * @post Exactly one counter increases when this returns true.
 * @note Not thread-safe; shares the application's I/O error latch.
 * @since 0.1.0
 */
RA8_INTERNAL static bool
internal_verify_page_rec(const char* series_dir, const mdl_page_rec_t* rec, verify_stats_t* st)
{
  char         page_path[k_fw_fs_path_cap];
  const int    pn   = snprintf(page_path, sizeof(page_path), "%s/%s", series_dir, rec->rel_path);
  fw_fs_stat_t page = {};
  if ((pn < 0) || ((size_t)pn >= sizeof(page_path)) ||
      (fw_fs_stat(&priv_mdl_app_context()->storage.fs->names, page_path, &page) != k_ra8_ok) ||
      !page.exists || (page.type != k_fw_fs_node_file)) {
    internal_admin_text3(priv_mdl_app_context()->output,
                         "  [MISSING/UNSAFE] ",
                         rec->rel_path,
                         "\n");
    if (priv_mdl_app_context()->io_error != k_ra8_ok) {
      return false;
    }
    st->pages_missing += 1U;
    return true;
  }
  uint64_t hash = 0U;
  if ((mdl_hash_file(&priv_mdl_app_context()->storage, page_path, &hash) != k_ra8_ok) ||
      (hash != rec->content_hash)) {
    internal_admin_text3(priv_mdl_app_context()->output,
                         "  [CORRUPT] ",
                         rec->rel_path,
                         " (hash mismatch)\n");
    if (priv_mdl_app_context()->io_error != k_ra8_ok) {
      return false;
    }
    st->pages_corrupt += 1U;
    return true;
  }
  st->pages_valid += 1U;
  return true;
}

/**
 * @brief Verify persisted pages and artifacts for one tracked series.
 * @details Loads validated state, resolves every page beneath the canonical
 *          series root, compares content hashes, then checks local artifacts.
 * @param[in] series_dir Canonical tracked-series directory.
 * @param[in] state_path Complete `.mdl_state` path.
 * @param[in,out] st Verification counters to update.
 * @pre All pointer arguments are non-NULL.
 * @pre @p series_dir is canonical and @p st is initialized.
 * @post `series_checked` increases exactly once.
 * @post Every recorded page is classified as valid, missing/unsafe, or corrupt.
 * @note Not thread-safe because state and validator workspace are shared.
 * @since 0.1.0
 */
RA8_INTERNAL static void
internal_verify_series_dir(const char* series_dir, const char* state_path, verify_stats_t* st)
{
  st->series_checked += 1U;
  internal_admin_text3(priv_mdl_app_context()->output, "verifying ", series_dir, "...\n");
  if (priv_mdl_app_context()->io_error != k_ra8_ok) {
    return;
  }
  if (mdl_state_load(&priv_mdl_app_context()->storage,
                     state_path,
                     &priv_mdl_app_context()->state) != k_ra8_ok) {
    internal_admin_text3(priv_mdl_app_context()->output,
                         "  [VERIFY FAIL] ",
                         series_dir,
                         ": .mdl_state file unreadable or corrupt\n");
    st->state_errors += 1U;
  } else {
    for (uint32_t i = 0U; i < priv_mdl_app_context()->state.page_rec_count; ++i) {
      const mdl_page_rec_t* rec = &priv_mdl_app_context()->state.pages[i];
      if (!internal_verify_page_rec(series_dir, rec, st)) {
        return;
      }
    }
  }
  if (priv_mdl_app_context()->io_error == k_ra8_ok) {
    internal_verify_artifacts(series_dir, st);
  }
}

/**
 * @brief Verify root-level artifacts and every tracked child series.
 * @details Coordinates verify library root with fixed application workspaces
 * and propagates validation, storage, or network failure to the selected
 * command runner.
 * @param[in] canonical Canonical library root.
 * @param[in,out] stats Verification counters to update.
 * @pre @p canonical and @p stats are non-NULL.
 * @pre @p canonical names a real directory.
 * @post Every recognized root artifact and tracked child is classified once.
 * @post Enumeration failures increment `stats->fs_errors`.
 * @note Not thread-safe because child verification uses shared state.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_verify_library_root(const char* canonical, verify_stats_t* stats)
{
  internal_verify_artifacts(canonical, stats);
  fw_fs_dir_t root = {};
  ra8_err_t   error =
    fw_fs_dir_open(&priv_mdl_app_context()->storage.fs->names,
                   canonical,
                   &root,
                   priv_mdl_app_context()->library_workspace.directory_workspace,
                   priv_mdl_app_context()->library_workspace.directory_workspace_bytes);
  if (error != k_ra8_ok) {
    stats->fs_errors += 1U;
    return;
  }
  while (error == k_ra8_ok) {
    fw_fs_dirent_value_t entry   = {};
    bool                 present = false;
    error                        = fw_fs_dir_next(&root, &entry, &present);
    if ((error != k_ra8_ok) || !present) {
      break;
    }
    if ((entry.name[0] == '.') || (entry.type != k_fw_fs_node_directory)) {
      continue;
    }
    char         child[k_fw_fs_path_cap];
    char         child_state[k_fw_fs_path_cap];
    fw_fs_stat_t child_node    = {};
    bool         marker_exists = false;
    if (!mdl_path_join(canonical, entry.name, child, sizeof(child)) ||
        !mdl_path_join(child, ".mdl_state", child_state, sizeof(child_state)) ||
        (fw_fs_stat(&priv_mdl_app_context()->storage.fs->names, child, &child_node) != k_ra8_ok) ||
        !child_node.exists || (child_node.type != entry.type) ||
        (mdl_state_probe(&priv_mdl_app_context()->storage, child_state, &marker_exists) !=
         k_ra8_ok) ||
        !marker_exists) {
      continue;
    }
    internal_verify_series_dir(child, child_state, stats);
    if (priv_mdl_app_context()->io_error != k_ra8_ok) {
      break;
    }
  }
  if (error != k_ra8_ok) {
    stats->fs_errors += 1U;
  }
  if (fw_fs_dir_close(&root) != k_ra8_ok) {
    stats->fs_errors += 1U;
  }
}

/**
 * @brief Stream the verification totals summary line.
 * @details Composes the fixed "verify complete" line with series, page, and
 *          artifact counters in one non-branching stream chain.
 * @param[in,out] output Bound stream sink.
 * @param[in] st Final verification counters.
 * @return Stream status.
 * @retval k_ra8_ok The complete summary line was written.
 * @retval other The first stream write failure.
 * @pre @p output and @p st are non-NULL.
 * @post Success writes exactly one terminated summary line.
 * @note Not thread-safe for a shared stream.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_verify_print_summary(ra8_io_stream_t*      output,
                                                            const verify_stats_t* st)
{
  ra8_err_t error = priv_mdl_stream_text(k_ra8_ok, output, "verify complete: ");
  error           = priv_mdl_stream_u64(error, output, st->series_checked);
  error           = priv_mdl_stream_text(error, output, " series; pages ");
  error           = priv_mdl_stream_u64(error, output, st->pages_valid);
  error           = priv_mdl_stream_text(error, output, " valid, ");
  error           = priv_mdl_stream_u64(error, output, st->pages_missing);
  error           = priv_mdl_stream_text(error, output, " missing, ");
  error           = priv_mdl_stream_u64(error, output, st->pages_corrupt);
  error           = priv_mdl_stream_text(error, output, " corrupt; artifacts ");
  error           = priv_mdl_stream_u64(error, output, st->archives_valid);
  error           = priv_mdl_stream_text(error, output, " valid, ");
  error           = priv_mdl_stream_u64(error, output, st->archives_corrupt);
  return priv_mdl_stream_text(error, output, " failed\n");
}

/**
 * @brief Verify a tracked series, a library root, or recognized artifacts.
 * @details Canonicalizes the target, detects a direct tracked-series marker or
 *          enumerates child series, validates artifacts, and prints totals.
 * @param[in] target_dir Requested directory, or NULL/empty for `downloads`.
 * @return Process-style verification status.
 * @retval 0 At least one target was found and every check passed.
 * @retval 1 The root was invalid, empty of targets, or any check failed.
 * @pre @p target_dir is NULL or points to a NUL-terminated path.
 * @pre ::priv_mdl_app_context()->export_ws is initialized.
 * @post No target content is modified.
 * @post Every discovered failure contributes to status and summary counters.
 * @note Not thread-safe because it uses shared state and validator workspace.
 * @since 0.1.0
 */
RA8_PRIV int priv_mdl_app_run_verify(const char* target_dir)
{
  const char*    dir  = target_dir;
  verify_stats_t st   = {};
  fw_fs_stat_t   root = {};
  if ((dir == nullptr) || (dir[0] != '/') ||
      (fw_fs_stat(&priv_mdl_app_context()->storage.fs->names, dir, &root) != k_ra8_ok) ||
      !root.exists || (root.type != k_fw_fs_node_directory)) {
    internal_admin_text3(priv_mdl_app_context()->diagnostic,
                         "media_dl: verify target '",
                         (dir != nullptr) ? dir : "",
                         "' is not a readable directory\n");
    return 1;
  }
  char state_path[k_fw_fs_path_cap];
  bool state_exists = false;
  if (mdl_path_join(dir, ".mdl_state", state_path, sizeof(state_path)) &&
      (mdl_state_probe(&priv_mdl_app_context()->storage, state_path, &state_exists) == k_ra8_ok) &&
      state_exists) {
    internal_verify_series_dir(dir, state_path, &st);
  } else {
    internal_verify_library_root(dir, &st);
  }
  if (priv_mdl_app_context()->io_error != k_ra8_ok) {
    return 1;
  }
  ra8_io_stream_t* output = priv_mdl_app_context()->output;
  if (internal_verify_print_summary(output, &st) != k_ra8_ok) {
    return 1;
  }

  const bool any_target = (st.series_checked != 0U) || (st.archives_checked != 0U);
  if (!any_target) {
    internal_admin_text3(priv_mdl_app_context()->diagnostic,
                         "media_dl: no tracked series or recognized artifacts under '",
                         dir,
                         "'\n");
    if (priv_mdl_app_context()->io_error != k_ra8_ok) {
      return 1;
    }
  }
  return (any_target && (st.state_errors == 0U) && (st.fs_errors == 0U) &&
          (st.pages_missing == 0U) && (st.pages_corrupt == 0U) && (st.archives_corrupt == 0U))
           ? 0
           : 1;
}
