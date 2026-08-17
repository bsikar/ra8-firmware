/**
 * @file mdl_app_series.c
 * @brief Series selection, download lifecycle, and export actions.
 * @details Reconciles validated persistent state with scraped metadata, selects
 *          chapters by identity, fetches them, checkpoints, and exports
 * results.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#include "mdl_app_internal.h"
#include "mdl_fetch_body_internal.h"

/** @brief Selected chapter window for non-update downloads. */
static mdl_url_list_t s_selected;

/** @brief Bounded failure log for the active series run. */
static mdl_fetch_faillog_t s_faillog;

/** @brief Append three borrowed series-report fragments to one stream.
 * @details Coordinates caller-owned bounded network, state, and storage objects.
 *          Preserves the first failure and publishes only finished cover transactions.
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
RA8_INTERNAL static ra8_err_t internal_series_text3(ra8_io_stream_t* stream,
                                                    const char*      first,
                                                    const char*      second,
                                                    const char*      third)
{
  ra8_err_t error = priv_mdl_stream_text(k_ra8_ok, stream, first);
  error           = priv_mdl_stream_text(error, stream, second);
  return priv_mdl_stream_text(error, stream, third);
}

/**
 * @brief Determine the inclusive numeric range of a chapter list.
 * @details Parses every URL before updating the caller's minimum and maximum.
 * @param[in] l Chapter URL list.
 * @param[out] lo Minimum parsed chapter number.
 * @param[out] hi Maximum parsed chapter number.
 * @return Whether a complete nonempty numeric range was available.
 * @retval true @p lo and @p hi hold the complete range.
 * @retval false The list was empty or a URL lacked a number.
 * @pre @p l, @p lo, and @p hi are non-NULL.
 * @pre Every populated URL row is NUL-terminated.
 * @post Outputs are initialized even when the list is empty.
 * @post The input list is never modified.
 * @note Thread-safe for distinct caller-owned inputs and outputs.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_list_range(const mdl_url_list_t* l, double* lo, double* hi)
{
  *lo = 0.0;
  *hi = 0.0;
  if (l->count == 0U) {
    return false;
  }
  if (!mdl_urlname_chapter_parse(l->urls[0], lo)) {
    return false;
  }
  *hi = *lo;
  for (size_t i = 1U; i < l->count; ++i) {
    double num = 0.0;
    if (!mdl_urlname_chapter_parse(l->urls[i], &num)) {
      return false;
    }
    *lo = (num < *lo) ? num : *lo;
    *hi = (num > *hi) ? num : *hi;
  }
  return true;
}

/**
 * @brief Select a bounded chapter window from the prepared list.
 * @details Finds the first requested numeric chapter when present and copies
 *          at most @p count complete URL rows into caller output.
 * @param[in] from_present Whether @p from_num constrains the first row.
 * @param[in] from_num Minimum numeric chapter when enabled.
 * @param[in] count Maximum rows to select.
 * @param[out] out Selected URL list.
 * @pre @p out is non-NULL.
 * @pre ::priv_mdl_app_context()->chapters contains validated bounded rows.
 * @post `out->count` never exceeds @p count or the list capacity.
 * @post ::priv_mdl_app_context()->chapters remains unchanged.
 * @note Not thread-safe because it reads shared
 * ::priv_mdl_app_context()->chapters.
 * @since 0.1.0
 */
RA8_INTERNAL static void
internal_select_window(bool from_present, double from_num, size_t count, mdl_url_list_t* out)
{
  size_t start = 0U;
  if (from_present) {
    while (start < priv_mdl_app_context()->chapters.count) {
      double value = 0.0;
      if (mdl_urlname_chapter_parse(priv_mdl_app_context()->chapters.urls[start], &value) &&
          (value >= from_num)) {
        break;
      }
      ++start;
    }
  }
  out->count = 0U;
  for (size_t i = start; (i < priv_mdl_app_context()->chapters.count) && (out->count < count);
       ++i) {
    memcpy(out->urls[out->count], priv_mdl_app_context()->chapters.urls[i], k_mdl_url_max);
    out->count += 1U;
  }
}

/**
 * @brief Compose the absolute `.mdl_state` path for a series.
 * @details Uses checked formatting and rejects every truncated result.
 * @param[in] abs_dir Canonical series directory.
 * @param[out] out Destination path buffer.
 * @param[in] cap Writable capacity of @p out.
 * @return Whether the complete state path fit.
 * @retval true @p out contains the NUL-terminated path.
 * @retval false Formatting failed or required at least @p cap bytes.
 * @pre @p abs_dir and @p out are non-NULL.
 * @pre @p cap is the true capacity of @p out.
 * @post Success appends exactly `/.mdl_state`.
 * @post No filesystem object is created or modified.
 * @note Thread-safe for distinct output buffers.
 * @since 0.1.0
 */
RA8_PRIV bool priv_mdl_app_state_path_of(const char* abs_dir, char* out, size_t cap)
{
  const int n = snprintf(out, cap, "%s/.mdl_state", abs_dir);
  if (n < 0) {
    return false;
  }
  return (size_t)n < cap;
}

/**
 * @brief Copy one complete string into a fixed metadata field.
 * @details Uses a checked bounded write and clears the destination on overflow,
 *          so callers can never consume a plausible-looking truncated field.
 *
 * @param[out] dst Destination field.
 * @param[in]  cap Writable byte capacity of @p dst.
 * @param[in]  src NUL-terminated source.
 *
 * @return Whether the complete source fit.
 * @retval true  @p dst contains the complete source.
 * @retval false An argument was invalid or truncation would occur.
 *
 * @pre @p dst and @p src are non-NULL.
 * @pre @p cap is the true writable capacity of @p dst.
 * @post On true, @p dst is NUL-terminated and equals @p src.
 * @post On false with nonzero @p cap, @p dst is an empty string.
 *
 * @note Thread-safe: writes only caller-owned storage.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_copy_metadata_text(char* dst, size_t cap, const char* src)
{
  if ((dst == nullptr) || (cap == 0U) || (src == nullptr)) {
    return false;
  }
  const int n = snprintf(dst, cap, "%s", src);
  if ((n < 0) || ((size_t)n >= cap)) {
    dst[0] = '\0';
    return false;
  }
  return true;
}

/**
 * @brief Convert persisted series/chapter state into exporter metadata.
 *
 * @details The series source URL is copied without truncation and the local
 * cover leaf is resolved beneath the already-canonical series directory. A
 * deterministic identifier folds the series URL and, for separate output, the
 * chapter URL; the exporter derives modified time from verified page files.
 *
 * @param[in]  abs_dir Canonical absolute series directory.
 * @param[in]  rec     Chapter record, or NULL for a combined series container.
 * @param[out] meta    Exporter metadata to initialise and populate.
 *
 * @return Whether every persisted field and composed path fit exactly.
 * @retval true  @p meta is ready for a bounded exporter call.
 * @retval false A field/path exceeded the corresponding exporter contract.
 *
 * @pre @p abs_dir and @p meta are non-NULL.
 * @pre ::priv_mdl_app_context()->state has passed state validation and @p rec
 * belongs to it or is NULL.
 * @post On true, no exporter field contains a silently truncated value.
 * @post No filesystem object or persistent state is modified.
 *
 * @note Not thread-safe: reads process-global ::priv_mdl_app_context()->state.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_build_export_metadata(const char*              abs_dir,
                                                        const mdl_chapter_rec_t* rec,
                                                        mdl_export_meta_t*       meta)
{
  mdl_meta_init(meta);
  if (!internal_copy_metadata_text(meta->series_title,
                                   sizeof(meta->series_title),
                                   priv_mdl_app_context()->state.series_title) ||
      !internal_copy_metadata_text(meta->summary,
                                   sizeof(meta->summary),
                                   priv_mdl_app_context()->state.summary) ||
      !internal_copy_metadata_text(meta->writer,
                                   sizeof(meta->writer),
                                   priv_mdl_app_context()->state.writer) ||
      !internal_copy_metadata_text(meta->artist,
                                   sizeof(meta->artist),
                                   priv_mdl_app_context()->state.artist) ||
      !internal_copy_metadata_text(meta->source_url,
                                   sizeof(meta->source_url),
                                   priv_mdl_app_context()->state.series_url) ||
      !internal_copy_metadata_text(meta->language,
                                   sizeof(meta->language),
                                   priv_mdl_app_context()->state.language)) {
    return false;
  }
  meta->reading_direction =
    (priv_mdl_app_context()->state.reading_direction == k_mdl_state_read_rtl) ? k_mdl_read_rtl
                                                                              : k_mdl_read_ltr;
  if (priv_mdl_app_context()->state.cover_path[0] != '\0') {
    if (!mdl_path_join(abs_dir,
                       priv_mdl_app_context()->state.cover_path,
                       meta->cover_path,
                       sizeof(meta->cover_path))) {
      return false;
    }
  }
  uint64_t identity = mdl_hash_str(priv_mdl_app_context()->state.series_url);
  if (rec != nullptr) {
    if (!internal_copy_metadata_text(meta->chapter_title,
                                     sizeof(meta->chapter_title),
                                     rec->title)) {
      return false;
    }
    meta->chapter_number = rec->number_known ? rec->number : 0.0;
    identity             = mdl_hash_bytes_seed(rec->source_url, strlen(rec->source_url), identity);
  }
  const int id_len =
    snprintf(meta->identifier, sizeof(meta->identifier), "urn:media-dl:%016" PRIx64, identity);
  return (id_len >= 0) && ((size_t)id_len < sizeof(meta->identifier));
}

/**
 * @brief Export every newly completed selected chapter separately.
 * @details Resolves each selected URL to persisted state and exports only
 *          complete records fetched during the current run.
 * @param[in] format Requested supported export format.
 * @param[in] abs_dir Canonical series directory.
 * @param[in] sel Selected chapter URL list.
 * @param[in] run_start Current-run timestamp boundary.
 * @return Number of metadata or exporter failures.
 * @retval 0 Every eligible chapter exported successfully.
 * @retval positive One or more eligible chapters failed.
 * @pre @p abs_dir and @p sel are non-NULL.
 * @pre ::priv_mdl_app_context()->state and ::priv_mdl_app_context()->export_ws
 * are initialized.
 * @post Ineligible or older chapters are not exported.
 * @post Persistent chapter state is unchanged.
 * @note Not thread-safe because it uses shared state and workspace.
 * @since 0.1.0
 */
RA8_INTERNAL static size_t internal_export_fresh_separate(ra8_mdl_format_t      format,
                                                          const char*           abs_dir,
                                                          const mdl_url_list_t* sel,
                                                          int64_t               run_start)
{
  size_t fails = 0U;
  for (size_t i = 0U; i < sel->count; ++i) {
    char id[k_mdl_chapter_id_max];
    mdl_urlname_last_segment(sel->urls[i], id, sizeof(id));
    const mdl_chapter_rec_t* rec = mdl_state_find_chapter(&priv_mdl_app_context()->state, id);
    if ((rec != nullptr) && rec->complete && (rec->fetched_at >= run_start)) {
      mdl_export_meta_t meta;
      if (!internal_build_export_metadata(abs_dir, rec, &meta)) {
        (void)internal_series_text3(priv_mdl_app_context()->diagnostic,
                                    "  export metadata for ",
                                    id,
                                    " exceeded a bounded field\n");
        fails += 1U;
      } else {
        fails += mdl_pack_one_meta(&priv_mdl_app_context()->storage,
                                   format,
                                   abs_dir,
                                   id,
                                   &meta,
                                   &priv_mdl_app_context()->export_ws,
                                   priv_mdl_app_context()->output,
                                   priv_mdl_app_context()->diagnostic);
      }
    }
  }
  return fails;
}

/**
 * @brief Print the per-run download tally.
 * @details Emits chapter and page outcome counts with their output directory.
 * @param[in] abs_dir Canonical series output directory.
 * @param[in] st Completed fetch statistics.
 * @pre @p abs_dir and @p st are non-NULL.
 * @pre Standard output is available.
 * @post Exactly one summary line is attempted.
 * @post @p st and filesystem state remain unchanged.
 * @note Output from concurrent callers may interleave.
 * @since 0.1.0

 * @return Operation status.
 * @retval k_ra8_ok The operation completed successfully.
 * @retval other The originating validation, storage, stream, or network error.
 */
RA8_INTERNAL static ra8_err_t internal_report_stats(const char*              abs_dir,
                                                    const mdl_fetch_stats_t* st)
{
  ra8_io_stream_t* output = priv_mdl_app_context()->output;
  ra8_err_t        error  = internal_series_text3(output, "done into ", abs_dir, "/: ");
  error                   = priv_mdl_stream_u64(error, output, st->chapters_completed);
  error                   = priv_mdl_stream_text(error, output, " chapter(s) fetched, ");
  error                   = priv_mdl_stream_u64(error, output, st->chapters_skipped);
  error                   = priv_mdl_stream_text(error, output, " skipped, ");
  error                   = priv_mdl_stream_u64(error, output, st->chapters_failed);
  error                   = priv_mdl_stream_text(error, output, " failed; pages ");
  error                   = priv_mdl_stream_u64(error, output, st->pages_fetched);
  error                   = priv_mdl_stream_text(error, output, " new / ");
  error                   = priv_mdl_stream_u64(error, output, st->pages_reused);
  error                   = priv_mdl_stream_text(error, output, " reused / ");
  error                   = priv_mdl_stream_u64(error, output, st->pages_failed);
  return priv_mdl_stream_text(error, output, " failed\n");
}

/**
 * @brief Choose output layout and its combined directory name.
 * @details Forces separate layout for updates, loose output, or explicit
 *          separation; otherwise names the combined range deterministically.
 * @param[in] r Validated series-run parameters.
 * @param[in] sel Selected chapter URLs.
 * @param[out] combined_rel Combined relative directory leaf.
 * @param[in] cap Writable capacity of @p combined_rel.
 * @param[in] slug Sanitized series slug.
 * @return Selected ::mdl_fetch_layout_t value.
 * @retval k_mdl_layout_separate Chapters use individual directories.
 * @retval k_mdl_layout_combined Chapters share the named directory.
 * @pre All pointer arguments are non-NULL.
 * @pre @p cap is the true output capacity and inputs are bounded.
 * @post Separate layout clears @p combined_rel.
 * @post Combined layout writes a deterministic NUL-terminated leaf.
 * @note Thread-safe for distinct output buffers.
 * @since 0.1.0
 */
RA8_INTERNAL static mdl_fetch_layout_t internal_choose_layout(const series_run_t*   r,
                                                              const mdl_url_list_t* sel,
                                                              char*                 combined_rel,
                                                              size_t                cap,
                                                              const char*           slug)
{
  combined_rel[0] = '\0';
  if (r->update || !r->combine || (r->format == k_ra8_mdl_format_loose)) {
    return k_mdl_layout_separate;
  }
  double lo = 0.0;
  double hi = 0.0;
  if (internal_list_range(sel, &lo, &hi)) {
    (void)snprintf(combined_rel, cap, "%s-%.10g-%.10g", slug, lo, hi);
  } else {
    (void)snprintf(combined_rel, cap, "%s-%zu-chapters", slug, sel->count);
  }
  return k_mdl_layout_combined;
}

/**
 * @brief Assemble a fetch context over the shared bounded buffers.
 * @details Connects validated run/site inputs to session, state, governor,
 *          scratch tables, failure log, and the requested progress callback.
 * @param[in] r Validated series-run parameters.
 * @param[in] site Validated site descriptor.
 * @param[in] abs_dir Canonical series directory.
 * @param[in] state_path Complete state-file path.
 * @param[in,out] gov Initialized request governor.
 * @param[in,out] cache Bound persistent HTTP cache and index workspace.
 * @return Fully populated fetch context by value.
 * @retval mdl_fetch_ctx_t Context referencing the supplied and shared storage.
 * @pre All pointer arguments are non-NULL and outlive the fetch run.
 * @pre ::priv_mdl_app_context()->session and shared bounded buffers are
 * initialized.
 * @post The returned context owns no dynamically allocated memory.
 * @post No request or filesystem mutation has yet occurred.
 * @note Not thread-safe because the context references process-global storage.
 * @since 0.1.0
 */
RA8_INTERNAL static mdl_fetch_ctx_t internal_make_ctx(const series_run_t* r,
                                                      const mdl_site_t*   site,
                                                      const char*         abs_dir,
                                                      const char*         state_path,
                                                      mdl_governor_t*     gov,
                                                      mdl_cache_t*        cache)
{
  return (mdl_fetch_ctx_t){.session        = &priv_mdl_app_context()->session,
                           .storage        = &priv_mdl_app_context()->storage,
                           .state          = &priv_mdl_app_context()->state,
                           .cache          = cache,
                           .state_path     = state_path,
                           .series_abs_dir = abs_dir,
                           .series_url     = r->series_url,
                           .site           = site,
                           .gov            = gov,
                           .timeout_ms     = r->timeout,
                           .page_buf       = priv_mdl_app_context()->page,
                           .page_cap       = sizeof(priv_mdl_app_context()->page),
                           .images         = &priv_mdl_app_context()->images,
                           .update_only    = r->update,
                           .refetch        = r->opts->refetch,
                           .faillog        = &s_faillog,
                           .diagnostic     = priv_mdl_app_context()->diagnostic,
                           .progress_fn =
                             r->opts->progress ? mdl_report_progress_bar : mdl_report_progress,
                           .progress_ctx   = priv_mdl_app_context()->output,
                           .progress_error = k_ra8_ok};
}

/**
 * @brief Package freshly downloaded output according to the selected layout.
 * @details Skips loose output, exports one combined container when requested,
 *          or delegates current-run chapters to separate packaging.
 * @param[in] r Validated series-run parameters.
 * @param[in] abs_dir Canonical series directory.
 * @param[in] layout Completed fetch layout.
 * @param[in] combined_rel Combined directory leaf when applicable.
 * @param[in] sel Selected chapter URLs.
 * @param[in] stats Completed fetch statistics.
 * @param[in] run_start Current-run timestamp boundary.
 * @return Number of packaging failures.
 * @retval 0 Packaging succeeded or loose output required none.
 * @retval positive Metadata or exporter work failed.
 * @pre All pointer arguments are non-NULL.
 * @pre ::priv_mdl_app_context()->state and ::priv_mdl_app_context()->export_ws
 * are initialized.
 * @post Successful container outputs are atomically published.
 * @post A metadata overflow is reported as an export failure.
 * @note Not thread-safe because it uses shared exporter storage.
 * @since 0.1.0
 */
RA8_INTERNAL static size_t internal_export_after(const series_run_t*      r,
                                                 const char*              abs_dir,
                                                 mdl_fetch_layout_t       layout,
                                                 const char*              combined_rel,
                                                 const mdl_url_list_t*    sel,
                                                 const mdl_fetch_stats_t* stats,
                                                 int64_t                  run_start)
{
  if (r->format == k_ra8_mdl_format_loose) {
    return 0U;
  }
  if (layout == k_mdl_layout_combined) {
    mdl_export_meta_t meta;
    if (!internal_build_export_metadata(abs_dir, nullptr, &meta)) {
      (void)priv_mdl_stream_text(k_ra8_ok,
                                 priv_mdl_app_context()->diagnostic,
                                 "  combined export metadata exceeded a bounded field\n");
      return 1U;
    }
    return mdl_pack_combined_meta(&priv_mdl_app_context()->storage,
                                  r->format,
                                  r->opts->allow_incomplete,
                                  abs_dir,
                                  combined_rel,
                                  stats,
                                  &meta,
                                  &priv_mdl_app_context()->export_ws,
                                  priv_mdl_app_context()->output,
                                  priv_mdl_app_context()->diagnostic);
  }
  return internal_export_fresh_separate(r->format, abs_dir, sel, run_start);
}

/**
 * @brief Save the shared series state and report a contextual failure.
 * @param[in] state_path Complete state-file path.
 * @param[in] failure Diagnostic prefix for a failed save.
 * @return State-save result.
 * @retval k_ra8_ok A new authenticated generation was published.
 * @retval other Persistence failed and the diagnostic was attempted.
 * @pre Both strings are non-NULL and NUL-terminated.
 * @post Success publishes one complete state generation.
 * @post Failure leaves the preceding generation authoritative.
 * @note Not thread-safe because it uses shared state and storage.
 * @since 0.1.0

 * @details Coordinates caller-owned bounded network, state, and storage objects.
 *          Preserves the first failure and publishes only finished cover transactions.
 * @pre Every required pointer is non-null and remains valid for the call.
 */
RA8_INTERNAL static ra8_err_t internal_save_series_state(const char* state_path,
                                                         const char* failure)
{
  bool            published = false;
  const ra8_err_t error     = mdl_state_save(&priv_mdl_app_context()->storage,
                                             state_path,
                                             &priv_mdl_app_context()->state,
                                             &published);
  if (error != k_ra8_ok) {
    (void)internal_series_text3(priv_mdl_app_context()->diagnostic, failure, state_path, "'\n");
  }
  return error;
}

/** @brief Report a failed series-cover acquisition with its exact error code.
 * @details Coordinates caller-owned bounded network, state, and storage objects.
 *          Preserves the first failure and publishes only finished cover transactions.
 * @param[in] error Exact failure value to report.
 * @pre Every required pointer is non-null and remains valid for the call.
 * @pre Lengths and capacities describe complete referenced objects without overflow.
 * @post Documented outputs and the return value describe the same outcome.
 * @post A rejected or failed operation is never reported as successful.
 * @note Thread safety follows ownership of the supplied context; no synchronization is added.
 * @since Version 0.1.0
 */
RA8_INTERNAL static void internal_report_cover_failure(ra8_err_t error)
{
  ra8_err_t output_error = priv_mdl_stream_text(k_ra8_ok,
                                                priv_mdl_app_context()->diagnostic,
                                                "media_dl: series cover acquisition failed "
                                                "(err 0x");
  output_error =
    priv_mdl_stream_hex(output_error, priv_mdl_app_context()->diagnostic, (uint32_t)error);
  (void)priv_mdl_stream_text(output_error, priv_mdl_app_context()->diagnostic, ")\n");
}

/**
 * @brief Resolve the chapter-selection window for one prepared run.
 * @details Uses the complete prepared chapter list on an update run;
 *          otherwise selects the requested present/from-number window into
 *          the shared selection scratch and requires it be non-empty.
 * @param[in] r Validated series-run parameters.
 * @param[out] out_sel Selected chapter-URL list on success.
 * @return Selection status.
 * @retval k_ra8_ok @p out_sel names a non-empty selection.
 * @retval k_ra8_err_invalid_arg The requested window selected zero chapters.
 * @pre @p r and @p out_sel are non-NULL.
 * @pre The prepared chapter list and app diagnostic sink are available.
 * @post On failure a diagnostic explaining the empty selection was emitted.
 * @post An update run selects the prepared chapter list itself, unmodified.
 * @note Not thread-safe; writes the shared selection-window scratch buffer.
 * @since Version 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_select_run_window(const series_run_t*    r,
                                                         const mdl_url_list_t** out_sel)
{
  *out_sel = &priv_mdl_app_context()->chapters;
  if (!r->update) {
    internal_select_window(r->from_present, r->from_num, r->chapters, &s_selected);
    *out_sel = &s_selected;
    if ((*out_sel)->count == 0U) {
      (void)priv_mdl_stream_text(k_ra8_ok,
                                 priv_mdl_app_context()->diagnostic,
                                 "media_dl: no chapters match the requested range\n");
      return k_ra8_err_invalid_arg;
    }
  }
  return k_ra8_ok;
}

/**
 * @brief Drive one prepared series through fetch, checkpoint, and export.
 * @details Selects the requested window, creates the fetch context, acquires a
 *          verified cover, saves state around the fetch, and packages results.
 * @param[in] r Validated series-run parameters.
 * @param[in] site Validated site descriptor.
 * @param[in] abs_dir Canonical series directory.
 * @param[in] state_path Complete state-file path.
 * @param[in,out] cache Bound persistent HTTP cache and index workspace.
 * @return Process-style status.
 * @retval 0 Fetch, state saves, and eligible exports all succeeded.
 * @retval 1 Selection, cover, fetch, state, or export work failed.
 * @pre All pointer arguments are non-NULL.
 * @pre Chapters and series state have been prepared.
 * @post A successful run durably checkpoints current state.
 * @post Failures are reported and never presented as successful export.
 * @note Not thread-safe because it owns the shared run composition state.
 * @since 0.1.0
 */
RA8_INTERNAL static int internal_run_prepared(const series_run_t* r,
                                              const mdl_site_t*   site,
                                              const char*         abs_dir,
                                              const char*         state_path,
                                              mdl_cache_t*        cache)
{
  const mdl_url_list_t* sel = nullptr;
  if (internal_select_run_window(r, &sel) != k_ra8_ok) {
    return 1;
  }
  char display_slug[k_slug_bytes];
  (void)mdl_sanitize_segment(priv_mdl_app_context()->state.series_title,
                             display_slug,
                             sizeof(display_slug));
  char               combined_rel[k_leaf_name_bytes];
  mdl_fetch_layout_t layout =
    internal_choose_layout(r, sel, combined_rel, sizeof(combined_rel), display_slug);

  mdl_governor_t      gov;
  const mdl_gov_cfg_t cfg = mdl_config_gov_cfg(site);
  mdl_governor_init(&gov, &cfg, r->seed);
  memset(&s_faillog, 0, sizeof(s_faillog));
  mdl_fetch_ctx_t ctx = internal_make_ctx(r, site, abs_dir, state_path, &gov, cache);
  const ra8_err_t crc = priv_mdl_app_ensure_series_cover(&ctx, abs_dir);
  if (crc != k_ra8_ok) {
    internal_report_cover_failure(crc);
    return 1;
  }
  if (internal_save_series_state(state_path, "media_dl: failed to checkpoint series metadata '") !=
      k_ra8_ok) {
    return 1;
  }
  const int64_t     run_start = (int64_t)time(nullptr);
  mdl_fetch_stats_t stats;
  const ra8_err_t   frc = mdl_fetch_run(&ctx,
                                        sel,
                                        layout,
                                        (layout == k_mdl_layout_combined) ? combined_rel : nullptr,
                                        &stats);
  if (ctx.progress_error != k_ra8_ok) {
    return 1;
  }
  const ra8_err_t src =
    internal_save_series_state(state_path, "media_dl: failed to save library state '");
  if (internal_report_stats(abs_dir, &stats) != k_ra8_ok) {
    return 1;
  }
  if (mdl_report_failures(priv_mdl_app_context()->diagnostic, &s_faillog) != k_ra8_ok) {
    return 1;
  }
  const size_t efail =
    internal_export_after(r, abs_dir, layout, combined_rel, sel, &stats, run_start);
  return ((frc == k_ra8_ok) && (src == k_ra8_ok) && (efail == 0U)) ? 0 : 1;
}

/** @brief Persisted descriptive fields retained across selector misses. */
typedef struct {
  char title[k_mdl_title_max];        /**< Previously recorded title.    */
  char summary[k_mdl_summary_max];    /**< Previously recorded synopsis. */
  char writer[k_mdl_person_max];      /**< Previously recorded writer.   */
  char artist[k_mdl_person_max];      /**< Previously recorded artist.   */
  char cover_path[k_mdl_relpath_max]; /**< Reusable verified cover leaf. */
} series_prior_metadata_t;

/**
 * @brief Snapshot metadata that may survive a temporary selector miss.
 * @details Coordinates snapshot prior metadata with fixed application
 * workspaces and propagates validation, storage, or network failure to the
 * selected command runner.
 * @param[out] prior Bounded snapshot destination.
 * @pre @p prior is non-NULL and shared state passed validation.
 * @pre Current scraped metadata contains the newly selected cover URL.
 * @post @p prior owns complete bounded copies of reusable prior fields.
 * @post Cover path is retained only while its source URL is unchanged.
 * @note Not thread-safe because it reads the shared series state.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_snapshot_prior_metadata(series_prior_metadata_t* prior)
{
  const mdl_app_context_t* app = priv_mdl_app_context();
  (void)snprintf(prior->title, sizeof(prior->title), "%s", app->state.series_title);
  (void)snprintf(prior->summary, sizeof(prior->summary), "%s", app->state.summary);
  (void)snprintf(prior->writer, sizeof(prior->writer), "%s", app->state.writer);
  (void)snprintf(prior->artist, sizeof(prior->artist), "%s", app->state.artist);
  (void)snprintf(prior->cover_path,
                 sizeof(prior->cover_path),
                 "%s",
                 (strcmp(app->state.cover_url, app->series_metadata.cover_url) == 0)
                   ? app->state.cover_path
                   : "");
}

/**
 * @brief Reconcile freshly scraped metadata with validated persisted state.
 * @details Computes reconcile series state from bounded state records without
 * allocation; persistent publication remains the responsibility of the injected
 * storage transaction.
 * @param[in] run Active series identity and descriptor path.
 * @param[in] site Loaded site descriptor.
 * @param[in] prior Snapshot of reusable prior descriptive fields.
 * @return Whether the complete metadata tuple fit persistent state.
 * @retval true State now contains the reconciled metadata tuple.
 * @retval false At least one bounded field rejected the complete value.
 * @pre All pointer arguments are non-NULL and shared state is loaded.
 * @pre @p prior was produced by ::internal_snapshot_prior_metadata.
 * @post Required identity fields always reflect the current run.
 * @post Optional selector misses preserve the corresponding prior value.
 * @note Not thread-safe because it mutates the shared series state.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_reconcile_series_state(const series_run_t*            run,
                                                         const mdl_site_t*              site,
                                                         const series_prior_metadata_t* prior)
{
  mdl_app_context_t* app   = priv_mdl_app_context();
  const char*        title = app->series_metadata.title;
  if (!app->series_metadata.title_selected && (prior->title[0] != '\0')) {
    title = prior->title;
    if (priv_mdl_stream_text(k_ra8_ok,
                             priv_mdl_app_context()->diagnostic,
                             "media_dl: WARNING: preserving recorded series title after "
                             "selector failure\n") != k_ra8_ok) {
      return false;
    }
  }
  const char* summary =
    (app->series_metadata.summary[0] != '\0') ? app->series_metadata.summary : prior->summary;
  const char* writer =
    (app->series_metadata.writer[0] != '\0') ? app->series_metadata.writer : prior->writer;
  const char* artist =
    (app->series_metadata.artist[0] != '\0') ? app->series_metadata.artist : prior->artist;
  mdl_state_set_series(&app->state, run->series_url, title, site->name, site->host, run->cfg_path);
  return mdl_state_set_series_metadata(&app->state,
                                       summary,
                                       writer,
                                       artist,
                                       app->series_metadata.cover_url,
                                       prior->cover_path,
                                       app->series_metadata.language,
                                       app->series_metadata.direction);
}

/**
 * @brief Prepare paths, load state, reconcile metadata, and run one series.
 * @details Coordinates run series paths with fixed application workspaces and
 * propagates validation, storage, or network failure to the selected command
 * runner.
 * @param[in] run Active series parameters.
 * @param[in] site Loaded site descriptor.
 * @param[in,out] cache Bound persistent HTTP cache and index workspace.
 * @return Process-style series status.
 * @retval 0 The prepared fetch/export lifecycle succeeded.
 * @retval 1 Path, state, metadata, fetch, or export work failed.
 * @pre @p run and @p site are non-NULL and chapters are prepared.
 * @pre Shared scraped metadata corresponds to @p run.
 * @post Unreadable state is never overwritten.
 * @post Successful metadata is checkpointed by ::internal_run_prepared.
 * @note Not thread-safe because it owns the shared series state.
 * @since 0.1.0
 */
RA8_INTERNAL static int
internal_run_series_paths(const series_run_t* run, const mdl_site_t* site, mdl_cache_t* cache)
{
  char slug[k_slug_bytes];
  char abs_dir[k_fw_fs_path_cap];
  char state_path[k_fw_fs_path_cap];
  if (!priv_mdl_app_prepare_series_dir(run->out_dir,
                                       run->series_url,
                                       slug,
                                       sizeof(slug),
                                       abs_dir) ||
      !priv_mdl_app_state_path_of(abs_dir, state_path, sizeof(state_path))) {
    return 1;
  }
  const ra8_err_t load_rc =
    mdl_state_load(&priv_mdl_app_context()->storage, state_path, &priv_mdl_app_context()->state);
  if (load_rc != k_ra8_ok) {
    ra8_err_t output_error = internal_series_text3(priv_mdl_app_context()->diagnostic,
                                                   "media_dl: refusing to overwrite unreadable "
                                                   "state '",
                                                   state_path,
                                                   "' (err 0x");
    output_error =
      priv_mdl_stream_hex(output_error, priv_mdl_app_context()->diagnostic, (uint32_t)load_rc);
    (void)priv_mdl_stream_text(output_error, priv_mdl_app_context()->diagnostic, ")\n");
    return 1;
  }
  series_prior_metadata_t prior;
  internal_snapshot_prior_metadata(&prior);
  if (!internal_reconcile_series_state(run, site, &prior)) {
    (void)priv_mdl_stream_text(k_ra8_ok,
                               priv_mdl_app_context()->diagnostic,
                               "media_dl: scraped series metadata exceeded state bounds\n");
    return 1;
  }
  return internal_run_prepared(run, site, abs_dir, state_path, cache);
}

/**
 * @brief Run one prepared descriptor through the network-backed series flow.
 * @details Initializes the concrete network/session bindings, prepares cached
 *          chapters, reports the retained count, and runs path/state/export work.
 * @param[in] run Complete validated series-run parameters.
 * @param[in] site Loaded and policy-adjusted site descriptor.
 * @return Process-style status.
 * @retval 0 Series processing completed without a reported failure.
 * @retval 1 Network, preparation, output, state, fetch, or export work failed.
 * @pre Both pointers are non-NULL and application streams are bound.
 * @pre @p site has already received any requested polite policy overrides.
 * @post The network interface is destroyed on every initialized path.
 * @post Success leaves the cache and library in fully published states.
 * @note The cache index workspace is borrowed exclusively for this call.
 * @since 0.1.0
 */
RA8_INTERNAL static int internal_run_series_network(const series_run_t* run, mdl_site_t* site)
{
  mdl_net_iface_t        net     = {};
  mdl_net_curl_storage_t storage = {};
  if (mdl_net_curl_init(&net, &storage, &run->opts->policy) != k_ra8_ok) {
    (void)priv_mdl_stream_text(k_ra8_ok,
                               priv_mdl_app_context()->diagnostic,
                               "media_dl: network init failed\n");
    return 1;
  }
  char ua[k_mdl_ua_max];
  if (priv_mdl_app_start_session(&net, run->opts, site->contact, ua, sizeof(ua)) != k_ra8_ok) {
    mdl_net_destroy(&net);
    return 1;
  }

  mdl_cache_t cache = {.storage    = &priv_mdl_app_context()->storage,
                       .index      = &priv_mdl_app_context()->cache_index,
                       .root       = run->cache_dir,
                       .diagnostic = priv_mdl_app_context()->diagnostic,
                       .refetch    = run->opts->refetch};
  int         rc    = 1;
  if (priv_mdl_app_prepare_chapters(site, run->series_url, run->timeout, &cache) != k_ra8_ok) {
    (void)priv_mdl_stream_text(k_ra8_ok,
                               priv_mdl_app_context()->diagnostic,
                               "media_dl: series fetch failed\n");
  } else if (priv_mdl_app_context()->chapters.count == 0U) {
    (void)priv_mdl_stream_text(k_ra8_ok,
                               priv_mdl_app_context()->diagnostic,
                               "media_dl: no chapters (check chapter_url_contains)\n");
  } else {
    ra8_err_t output_error =
      priv_mdl_stream_text(k_ra8_ok, priv_mdl_app_context()->output, "chapters found: ");
    output_error = priv_mdl_stream_u64(output_error,
                                       priv_mdl_app_context()->output,
                                       priv_mdl_app_context()->chapters.count);
    output_error = priv_mdl_stream_text(output_error, priv_mdl_app_context()->output, "\n");
    if (output_error == k_ra8_ok) {
      rc = internal_run_series_paths(run, site, &cache);
    }
  }
  mdl_net_destroy(&net);
  return rc;
}

/**
 * @brief Execute series mode from descriptor through download.
 * @details Loads policy, establishes networking, prepares chapters and the
 *          tracked directory, reconciles metadata, then runs the orchestrator.
 * @param[in] r Complete validated series-run parameters.
 * @return Process-style status.
 * @retval 0 Series processing completed without a reported failure.
 * @retval 1 Configuration, network, state, fetch, or export work failed.
 * @pre @p r and all required strings/options are non-NULL.
 * @pre The CLI has validated mode-specific argument combinations.
 * @post The network interface is destroyed on every initialized path.
 * @post Unreadable state is never silently overwritten.
 * @note Not thread-safe because it uses process-global run storage.
 * @since 0.1.0
 */
RA8_PRIV int priv_mdl_app_run_series(const series_run_t* r)
{
  if ((r == nullptr) || (r->cache_dir == nullptr) ||
      (priv_mdl_app_storage_ensure_directory(&priv_mdl_app_context()->storage, r->out_dir) !=
       k_ra8_ok)) {
    return 1;
  }
  mdl_site_t site;
  if (mdl_config_load(&priv_mdl_app_context()->storage, r->cfg_path, &site) != k_ra8_ok) {
    return 1;
  }
  if (r->opts->polite) {
    mdl_config_apply_polite(&site);
  }
  ra8_err_t output_error =
    internal_series_text3(priv_mdl_app_context()->output, "site: ", site.name, " (host ");
  output_error = priv_mdl_stream_text(output_error, priv_mdl_app_context()->output, site.host);
  output_error = priv_mdl_stream_text(output_error, priv_mdl_app_context()->output, ", kind ");
  output_error = priv_mdl_stream_text(output_error, priv_mdl_app_context()->output, site.kind);
  output_error = priv_mdl_stream_text(output_error, priv_mdl_app_context()->output, ")\n");
  if (output_error != k_ra8_ok) {
    return 1;
  }

  return internal_run_series_network(r, &site);
}
