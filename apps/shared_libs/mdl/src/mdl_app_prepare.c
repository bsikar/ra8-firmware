/**
 * @file mdl_app_prepare.c
 * @brief Descriptor-driven series preparation and safe path setup.
 * @details Fetches and extracts configured series metadata into fixed buffers,
 *          sanitizes identities, and prepares paths contained by the library.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#include "mdl_app_internal.h"

/**
 * @brief Dispatch one cache request directly through the active network seam.
 * @details Adapts the cache callback signature to the already initialized
 *          downloader network interface without retaining any argument.
 * @param[in,out] context Borrowed ::mdl_net_iface_t pointer.
 * @param[in] url Exact absolute request URL.
 * @param[in] request Completed request metadata.
 * @param[out] buffer Bounded body destination.
 * @param[in] capacity Writable body capacity.
 * @param[out] out_length Exact accepted bytes.
 * @param[out] response Finished response metadata.
 * @return Canonical network status.
 * @retval k_ra8_ok The response and exact body length were published.
 * @retval other The network backend rejected or failed the request.
 * @pre All pointers are non-NULL and the network handle is initialized.
 * @pre @p buffer spans @p capacity writable bytes.
 * @post Success initializes both output objects.
 * @post No argument pointer is retained.
 * @note Series indexes currently require no separate governor.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_prepare_cache_fetch(void*                context,
                                                           const char*          url,
                                                           const mdl_net_req_t* request,
                                                           char*                buffer,
                                                           size_t               capacity,
                                                           size_t*              out_length,
                                                           mdl_net_resp_t*      response)
{
  return mdl_net_get_buf((mdl_net_iface_t*)context,
                         url,
                         request,
                         buffer,
                         capacity,
                         out_length,
                         response);
}

/**
 * @brief Report verified cache reuse with its pre-request staleness.
 * @param[in] url Exact cached URL.
 * @param[in] result Completed cache outcome.
 * @return First diagnostic-stream status.
 * @pre Both pointers are non-NULL.
 * @pre Shared diagnostic stream is bound.
 * @post Reuse emits one line containing age and status.
 * @post Non-reuse emits only a corruption-rebuild warning when applicable.
 * @note Output failure is propagated to the caller.
 * @since 0.1.0
 */
RA8_PRIV ra8_err_t priv_mdl_app_report_cache(const char* url, const mdl_cache_result_t* result)
{
  ra8_io_stream_t* stream = priv_mdl_app_context()->diagnostic;
  ra8_err_t        error  = k_ra8_ok;
  if (result->index_rebuilt) {
    error = priv_mdl_stream_text(error, stream, "mdl: discarded corrupt cache index for ");
    error = priv_mdl_stream_text(error, stream, url);
    error = priv_mdl_stream_text(error, stream, "\n");
  }
  if ((error == k_ra8_ok) && result->body_reused) {
    error = priv_mdl_stream_text(error, stream, "mdl: cache reuse age=");
    if (result->age_seconds >= 0) {
      error = priv_mdl_stream_u64(error, stream, (uint64_t)result->age_seconds);
      error = priv_mdl_stream_text(error, stream, "s");
    } else {
      error = priv_mdl_stream_text(error, stream, "unknown");
    }
    error = priv_mdl_stream_text(error, stream, " status=");
    error = priv_mdl_stream_u64(error, stream, result->observed_status);
    error = priv_mdl_stream_text(error, stream, " url=");
    error = priv_mdl_stream_text(error, stream, url);
    error = priv_mdl_stream_text(error, stream, "\n");
  }
  return error;
}

/** @brief Scratch row used while reordering the prepared chapter list. */
static char s_rowtmp[k_mdl_url_max];

/** @brief Append three borrowed preparation diagnostics.
 * @details Uses caller-owned application and storage state with injected diagnostics.
 *          Returns the first failure without publishing partial preparation.
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
RA8_INTERNAL static ra8_err_t
internal_prepare_diag3(const char* first, const char* second, const char* third)
{
  ra8_io_stream_t* diagnostic = priv_mdl_app_context()->diagnostic;
  ra8_err_t        error      = priv_mdl_stream_text(k_ra8_ok, diagnostic, first);
  error                       = priv_mdl_stream_text(error, diagnostic, second);
  return priv_mdl_stream_text(error, diagnostic, third);
}

/**
 * @brief Swap two rows of a URL list.
 * @details Uses the fixed global row scratch so the bounded rows move intact.
 * @param[in,out] l URL list to mutate.
 * @param[in] a First row index.
 * @param[in] b Second row index.
 * @pre @p l is non-NULL and owns complete URL rows.
 * @pre @p a and @p b are smaller than `l->count`.
 * @post The selected rows exchange positions.
 * @post All other rows and `l->count` are unchanged.
 * @note Not thread-safe because it uses ::s_rowtmp.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_swap_rows(mdl_url_list_t* l, size_t a, size_t b)
{
  if (a == b) {
    return;
  }
  memcpy(s_rowtmp, l->urls[a], k_mdl_url_max);
  memcpy(l->urls[a], l->urls[b], k_mdl_url_max);
  memcpy(l->urls[b], s_rowtmp, k_mdl_url_max);
}

/**
 * @brief Reverse a URL list in place.
 * @details Exchanges mirrored rows through ::internal_swap_rows without
 * allocating.
 * @param[in,out] l URL list to reverse.
 * @pre @p l is non-NULL.
 * @pre `l->count` does not exceed its fixed row capacity.
 * @post Row order is exactly reversed.
 * @post `l->count` and row contents remain otherwise unchanged.
 * @note Not thread-safe because ::internal_swap_rows uses shared scratch.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_reverse_list(mdl_url_list_t* l)
{
  for (size_t i = 0U; i < (l->count / 2U); ++i) {
    internal_swap_rows(l, i, l->count - 1U - i);
  }
}

/**
 * @brief Sort a URL list by parsed chapter number.
 * @details Validates every numeric identifier before performing the in-place
 *          selection sort, preserving input order when validation fails.
 * @param[in,out] l URL list to validate and sort.
 * @return Whether every URL carried a numeric chapter identifier.
 * @retval true The list is sorted in ascending numeric order.
 * @retval false At least one identifier was missing and the list is unchanged.
 * @pre @p l is non-NULL.
 * @pre `l->count` fits its fixed row table.
 * @post Success orders every existing row without changing the count.
 * @post Failure preserves every row and its original position.
 * @note Not thread-safe because row swaps use shared scratch.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_sort_by_chapter_num(mdl_url_list_t* l)
{
  for (size_t i = 0U; i < l->count; ++i) {
    double ignored = 0.0;
    if (!mdl_urlname_chapter_parse(l->urls[i], &ignored)) {
      return false;
    }
  }
  for (size_t i = 0U; i + 1U < l->count; ++i) {
    size_t min = i;
    for (size_t j = i + 1U; j < l->count; ++j) {
      if (mdl_urlname_chapter_value(l->urls[j]) < mdl_urlname_chapter_value(l->urls[min])) {
        min = j;
      }
    }
    internal_swap_rows(l, i, min);
  }
  return true;
}

/**
 * @brief Apply the configured chapter ordering in place.
 * @details Selects document order, reverse order, or validated numeric order.
 * @param[in,out] l URL list to reorder.
 * @param[in] order Validated descriptor ordering policy.
 * @pre @p l is non-NULL.
 * @pre @p order is a valid ::mdl_chapter_order_t value.
 * @post The requested supported ordering is applied.
 * @post Numeric parse failure leaves document order intact and emits a warning.
 * @note Not thread-safe because sorting uses shared row scratch.
 * @since 0.1.0

 * @return Operation status.
 * @retval k_ra8_ok The operation completed successfully.
 * @retval other The originating validation, storage, stream, or network error.
 */
RA8_INTERNAL static ra8_err_t internal_apply_order(mdl_url_list_t* l, mdl_chapter_order_t order)
{
  if (order == k_mdl_order_reverse) {
    internal_reverse_list(l);
  } else if (order == k_mdl_order_asc) {
    if (!internal_sort_by_chapter_num(l)) {
      return priv_mdl_stream_text(k_ra8_ok,
                                  priv_mdl_app_context()->diagnostic,
                                  "mdl: WARNING: at least one chapter has no numeric "
                                  "identifier; preserving site document order\n");
    }
  }
  return k_ra8_ok;
}

/**
 * @brief Keep only list entries whose URL starts with a prefix.
 * @details Compacts matching bounded rows in place while retaining their order.
 * @param[in,out] l URL list to filter.
 * @param[in] prefix NUL-terminated absolute URL prefix.
 * @pre @p l and @p prefix are non-NULL.
 * @pre Every populated row is NUL-terminated.
 * @post `l->count` equals the number of matching rows.
 * @post Surviving rows preserve their relative order.
 * @note Not thread-safe when callers concurrently mutate @p l.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_filter_prefix(mdl_url_list_t* l, const char* prefix)
{
  const size_t plen = strlen(prefix);
  size_t       w    = 0U;
  for (size_t r = 0U; r < l->count; ++r) {
    if (strncmp(l->urls[r], prefix, plen) == 0) {
      if (w != r) {
        memcpy(l->urls[w], l->urls[r], k_mdl_url_max);
      }
      ++w;
    }
  }
  l->count = w;
}

/**
 * @brief Print the missing-operator-contact warning once.
 * @details Suppresses duplicate warnings across sessions within one process.
 * @pre Standard error is available for diagnostic output.
 * @pre The process uses the command-line single-threaded composition root.
 * @post The first call marks the warning as emitted.
 * @post Later calls produce no output.
 * @note Not thread-safe because the one-time flag is function-static.
 * @since 0.1.0

 * @return Operation status.
 * @retval k_ra8_ok The operation completed successfully.
 * @retval other The originating validation, storage, stream, or network error.
 */
RA8_INTERNAL static ra8_err_t internal_warn_no_contact(void)
{
  static bool s_warned = false;
  if (!s_warned) {
    const ra8_err_t error =
      priv_mdl_stream_text(k_ra8_ok,
                           priv_mdl_app_context()->diagnostic,
                           "mdl: WARNING: no contact configured; pass --contact "
                           "<email|url> so a site operator can reach you before banning\n");
    if (error == k_ra8_ok) {
      s_warned = true;
    }
    return error;
  }
  return k_ra8_ok;
}

/**
 * @brief Extract the optional summary and creator fields.
 * @details Coordinates extract optional metadata with fixed application
 * workspaces and propagates validation, storage, or network failure to the
 * selected command runner.
 * @param[in] site Validated descriptor containing optional selectors.
 * @param[in] html Complete fetched series HTML.
 * @param[in] len Number of readable bytes at @p html.
 * @return Canonical extraction status.
 * @retval k_ra8_ok Every configured field fit; misses were cleared and warned.
 * @retval k_ra8_err_invalid_size A selected value exceeded its destination.
 * @pre All pointer arguments are non-NULL and @p html spans @p len bytes.
 * @pre Shared series metadata has been cleared for this series.
 * @post Successful matches populate their corresponding bounded fields.
 * @post Missing optional values remain empty and visible in diagnostics.
 * @note Not thread-safe because it writes shared metadata.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_extract_optional_metadata(const mdl_site_t* site, const char* html, size_t len)
{
  const struct {
    const char* selector; /**< Validated descriptor selector. */
    char*       out;      /**< Bounded destination field.     */
    size_t      cap;      /**< Destination byte capacity.     */
    const char* label;    /**< Diagnostic field name.         */
  } optional[] = {{site->series_summary_selector,
                   priv_mdl_app_context()->series_metadata.summary,
                   sizeof(priv_mdl_app_context()->series_metadata.summary),
                   "summary"},
                  {site->series_author_selector,
                   priv_mdl_app_context()->series_metadata.writer,
                   sizeof(priv_mdl_app_context()->series_metadata.writer),
                   "author"},
                  {site->series_artist_selector,
                   priv_mdl_app_context()->series_metadata.artist,
                   sizeof(priv_mdl_app_context()->series_metadata.artist),
                   "artist"}};
  for (size_t i = 0U; i < (sizeof(optional) / sizeof(optional[0])); ++i) {
    if (optional[i].selector[0] == '\0') {
      continue;
    }
    const ra8_err_t rc =
      mdl_extract_selector(html, len, optional[i].selector, optional[i].out, optional[i].cap);
    if (rc == k_ra8_err_invalid_size) {
      const ra8_err_t output_error =
        internal_prepare_diag3("mdl: series ",
                               optional[i].label,
                               " exceeds the bounded metadata field\n");
      return (output_error == k_ra8_ok) ? rc : output_error;
    }
    if (rc != k_ra8_ok) {
      optional[i].out[0] = '\0';
      ra8_err_t output_error =
        internal_prepare_diag3("mdl: WARNING: series ", optional[i].label, " selector '");
      output_error = priv_mdl_stream_text(output_error,
                                          priv_mdl_app_context()->diagnostic,
                                          optional[i].selector);
      output_error = priv_mdl_stream_text(output_error,
                                          priv_mdl_app_context()->diagnostic,
                                          "' matched no bounded value\n");
      if (output_error != k_ra8_ok) {
        return output_error;
      }
    }
  }
  return k_ra8_ok;
}

/**
 * @brief Extract and resolve the configured required cover URL.
 * @details Coordinates extract cover with fixed application workspaces and
 * propagates validation, storage, or network failure to the selected command
 * runner.
 * @param[in] site Validated descriptor containing the cover selector.
 * @param[in] series_url Absolute URL used as the resolution base.
 * @param[in] html Complete fetched series HTML.
 * @param[in] len Number of readable bytes at @p html.
 * @return Canonical extraction or validation status.
 * @retval k_ra8_ok No cover was configured, or it resolved safely.
 * @retval k_ra8_err_validation_failed The selected cover was not a safe URL.
 * @retval other The configured selector did not yield a bounded value.
 * @pre All pointer arguments are non-NULL and @p html spans @p len bytes.
 * @pre Shared series metadata has been cleared for this series.
 * @post Success populates the cover URL exactly when a selector is configured.
 * @post Failure prevents the series download from starting.
 * @note Not thread-safe because it writes shared metadata.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_extract_cover(const mdl_site_t* site, const char* series_url, const char* html, size_t len)
{
  if (site->series_cover_selector[0] == '\0') {
    return k_ra8_ok;
  }
  char            cover_raw[k_mdl_url_max];
  const ra8_err_t rc =
    mdl_extract_selector(html, len, site->series_cover_selector, cover_raw, sizeof(cover_raw));
  if (rc != k_ra8_ok) {
    const ra8_err_t output_error = internal_prepare_diag3("mdl: required series cover selector '",
                                                          site->series_cover_selector,
                                                          "' failed\n");
    return (output_error == k_ra8_ok) ? rc : output_error;
  }
  if (!mdl_extract_resolve_url(series_url,
                               cover_raw,
                               priv_mdl_app_context()->series_metadata.cover_url,
                               sizeof(priv_mdl_app_context()->series_metadata.cover_url))) {
    const ra8_err_t output_error =
      priv_mdl_stream_text(k_ra8_ok,
                           priv_mdl_app_context()->diagnostic,
                           "mdl: series cover URL could not be resolved safely\n");
    return (output_error == k_ra8_ok) ? k_ra8_err_validation_failed : output_error;
  }
  return k_ra8_ok;
}

/**
 * @brief Extract descriptor-driven series metadata from one fetched page.
 *
 * @details A missing/unmatched title falls back loudly to the stable URL leaf;
 * optional descriptive fields warn and remain empty. A configured cover is a
 * required contract: it must be found and resolve to an absolute URL, because
 * silently exporting a book without its promised cover is a false success.
 *
 * @param[in] site       Validated site descriptor.
 * @param[in] series_url Absolute URL used as the cover-resolution base.
 * @param[in] html       Complete fetched series HTML.
 * @param[in] len        Number of readable bytes at @p html.
 *
 * @return An ::ra8_err_t extraction result.
 * @retval k_ra8_ok Metadata was extracted, with documented optional fallbacks.
 * @retval k_ra8_err_invalid_size A selected value exceeded its bounded field.
 * @retval k_ra8_err_validation_failed A configured cover URL was unsafe.
 * @retval k_ra8_err_no_data A configured cover selector matched no value.
 *
 * @pre All pointer arguments are non-NULL and NUL-terminated where applicable.
 * @pre @p html addresses at least @p len readable bytes.
 * @post On success, ::priv_mdl_app_context()->series_metadata contains no stale
 * prior-series values.
 * @post On failure, callers do not begin a chapter download.
 *
 * @note Not thread-safe: replaces process-global bounded metadata scratch.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_extract_series_metadata(const mdl_site_t* site,
                                                               const char*       series_url,
                                                               const char*       html,
                                                               size_t            len)
{
  memset(&priv_mdl_app_context()->series_metadata,
         0,
         sizeof(priv_mdl_app_context()->series_metadata));
  priv_mdl_app_context()->series_metadata.direction =
    (strcmp(site->reading_direction, "rtl") == 0) ? k_mdl_state_read_rtl : k_mdl_state_read_ltr;
  const int language_len = snprintf(priv_mdl_app_context()->series_metadata.language,
                                    sizeof(priv_mdl_app_context()->series_metadata.language),
                                    "%s",
                                    site->language);
  if ((language_len < 0) ||
      ((size_t)language_len >= sizeof(priv_mdl_app_context()->series_metadata.language))) {
    return k_ra8_err_invalid_size;
  }

  ra8_err_t rc = k_ra8_err_no_data;
  if (site->series_title_selector[0] != '\0') {
    rc = mdl_extract_selector(html,
                              len,
                              site->series_title_selector,
                              priv_mdl_app_context()->series_metadata.title,
                              sizeof(priv_mdl_app_context()->series_metadata.title));
    priv_mdl_app_context()->series_metadata.title_selected = rc == k_ra8_ok;
  }
  if (rc == k_ra8_err_invalid_size) {
    const ra8_err_t output_error =
      priv_mdl_stream_text(k_ra8_ok,
                           priv_mdl_app_context()->diagnostic,
                           "mdl: series title exceeds the bounded metadata field\n");
    return (output_error == k_ra8_ok) ? rc : output_error;
  }
  if (rc != k_ra8_ok) {
    mdl_urlname_last_segment(series_url,
                             priv_mdl_app_context()->series_metadata.title,
                             sizeof(priv_mdl_app_context()->series_metadata.title));
    ra8_err_t output_error = internal_prepare_diag3("mdl: WARNING: series title selector '",
                                                    site->series_title_selector,
                                                    "' failed; using URL leaf '");
    output_error           = priv_mdl_stream_text(output_error,
                                                  priv_mdl_app_context()->diagnostic,
                                                  priv_mdl_app_context()->series_metadata.title);
    output_error = priv_mdl_stream_text(output_error, priv_mdl_app_context()->diagnostic, "'\n");
    if (output_error != k_ra8_ok) {
      return output_error;
    }
  }

  rc = internal_extract_optional_metadata(site, html, len);
  if (rc != k_ra8_ok) {
    return rc;
  }
  return internal_extract_cover(site, series_url, html, len);
}

/**
 * @brief Retain only chapters belonging to the configured series.
 * @details Builds the descriptor-selected absolute prefix, removes relation
 *          links outside that prefix, and applies the configured ordering.
 * @param[in] site Validated site descriptor.
 * @param[in] series_url Absolute series page URL.
 * @return Canonical filtering or ordering status.
 * @retval k_ra8_ok The retained rows are ordered as configured.
 * @retval k_ra8_err_invalid_size The bounded prefix cannot be represented.
 * @pre Both pointers are non-NULL and the chapter list is initialized.
 * @pre Every chapter row is a canonical absolute URL.
 * @post Success removes cross-series rows and applies descriptor ordering.
 * @post Failure never reads or writes beyond the bounded prefix buffer.
 * @note A configured sibling prefix takes precedence over the series URL.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_prepare_filter_chapters(const mdl_site_t* site,
                                                               const char*       series_url)
{
  char prefix[k_dir_path_bytes];
  if (site->chapter_url_prefix[0] != '\0') {
    const int prefix_len = snprintf(prefix, sizeof(prefix), "%s", site->chapter_url_prefix);
    if ((prefix_len < 0) || ((size_t)prefix_len >= sizeof(prefix))) {
      return k_ra8_err_invalid_size;
    }
  } else {
    const int prefix_len = snprintf(prefix, sizeof(prefix), "%s", series_url);
    if ((prefix_len < 0) || ((size_t)prefix_len >= sizeof(prefix))) {
      return k_ra8_err_invalid_size;
    }
    size_t pl = strlen(prefix);
    if ((pl > 0U) && (prefix[pl - 1U] != '/') && (pl + 1U < sizeof(prefix))) {
      prefix[pl]      = '/';
      prefix[pl + 1U] = '\0';
    }
  }
  internal_filter_prefix(&priv_mdl_app_context()->chapters, prefix);
  return internal_apply_order(&priv_mdl_app_context()->chapters, site->chapter_order);
}

/**
 * @brief Fetch a series page and build its ordered chapter list.
 * @details Enforces session policy, extracts descriptor metadata and anchors,
 *          then filters cross-series links and applies configured ordering.
 * @param[in] site Validated site descriptor.
 * @param[in] series_url Absolute series page URL.
 * @param[in] timeout Per-request timeout in milliseconds.
 * @param[in,out] cache Bound persistent HTTP cache and index workspace.
 * @return An ::ra8_err_t preparation result.
 * @retval k_ra8_ok Metadata and chapter rows were prepared.
 * @retval k_ra8_fail Session policy refused the series URL.
 * @retval k_ra8_err_invalid_size A configured prefix or extracted field
 * exceeded its bound.
 * @retval other Fetch, extraction, validation, or ordering failed.
 * @pre @p site and @p series_url are non-NULL.
 * @pre ::priv_mdl_app_context()->session is initialized and @p timeout is
 * nonzero.
 * @post Success replaces ::priv_mdl_app_context()->chapters and
 * ::priv_mdl_app_context()->series_metadata.
 * @post Failure prevents the caller from starting chapter downloads.
 * @note Not thread-safe because it uses process-global bounded buffers.
 * @since 0.1.0
 */
RA8_PRIV ra8_err_t priv_mdl_app_prepare_chapters(const mdl_site_t* site,
                                                 const char*       series_url,
                                                 uint32_t          timeout,
                                                 mdl_cache_t*      cache)
{
  if ((cache == nullptr) ||
      !mdl_session_url_allowed(&priv_mdl_app_context()->session, series_url, nullptr)) {
    return k_ra8_fail; /* robots refused the series page (message printed) */
  }
  const mdl_net_req_t req      = {.user_agent = priv_mdl_app_context()->session.user_agent,
                                  .referer    = nullptr,
                                  .timeout_ms = timeout};
  size_t              len      = 0U;
  mdl_net_resp_t      response = {};
  mdl_cache_result_t  cache_result;
  ra8_err_t           rc = mdl_cache_get_buf(cache,
                                             series_url,
                                             &req,
                                             internal_prepare_cache_fetch,
                                             priv_mdl_app_context()->session.net,
                                             priv_mdl_app_context()->page,
                                             sizeof(priv_mdl_app_context()->page),
                                             &len,
                                             &response,
                                             &cache_result);
  if (rc != k_ra8_ok) {
    return rc;
  }
  rc = priv_mdl_app_report_cache(series_url, &cache_result);
  if (rc != k_ra8_ok) {
    return rc;
  }
  rc = internal_extract_series_metadata(site, series_url, priv_mdl_app_context()->page, len);
  if (rc != k_ra8_ok) {
    return rc;
  }
  rc = mdl_extract_anchors(priv_mdl_app_context()->page,
                           len,
                           series_url,
                           site->chapter_url_contains,
                           &priv_mdl_app_context()->chapters);
  if (rc != k_ra8_ok) {
    return rc;
  }

  return internal_prepare_filter_chapters(site, series_url);
}

/**
 * @brief Initialize the shared network session identity for one run.
 * @details Applies CLI-over-descriptor contact precedence and builds the
 *          bounded user agent before configuring robots enforcement.
 * @param[in,out] net Initialized network interface.
 * @param[in] opts Validated run policy.
 * @param[in] cfg_contact Optional descriptor contact string.
 * @param[out] ua User-agent buffer.
 * @param[in] ua_cap Writable capacity of @p ua.
 * @pre @p net, @p opts, and @p ua are non-NULL.
 * @pre @p ua_cap is the true writable user-agent capacity.
 * @post ::priv_mdl_app_context()->session references @p net and the completed
 * identity.
 * @post Missing contact emits at most one process warning.
 * @note Not thread-safe because it replaces ::priv_mdl_app_context()->session.
 * @since 0.1.0
 */
RA8_PRIV ra8_err_t priv_mdl_app_start_session(mdl_net_iface_t*      net,
                                              const mdl_run_opts_t* opts,
                                              const char*           cfg_contact,
                                              char*                 ua,
                                              size_t                ua_cap)
{
  const char* contact = opts->contact;
  if ((contact == nullptr) && (cfg_contact != nullptr) && (cfg_contact[0] != '\0')) {
    contact = cfg_contact; /* CLI --contact overrides the site descriptor's key */
  }
  if (!mdl_session_build_ua(contact, ua, ua_cap)) {
    const ra8_err_t error = internal_warn_no_contact();
    if (error != k_ra8_ok) {
      return error;
    }
  }
  mdl_session_init(&priv_mdl_app_context()->session,
                   net,
                   ua,
                   priv_mdl_app_context()->diagnostic,
                   opts->honor_robots);
  return k_ra8_ok;
}

/**
 * @brief Create and resolve a series directory beneath an output root.
 * @details Derives the bounded URL slug, joins it under the canonical output
 *          root, creates both directories as needed, rejects a symbolic-link
 *          leaf, and proves the resolved series path remains contained.
 * @param[in] out_dir Output library root.
 * @param[in] series_url Absolute series URL used to derive the slug.
 * @param[out] slug Destination for the series slug.
 * @param[in] slug_cap Writable capacity of @p slug.
 * @param[out] abs_dir Destination for the canonical directory path.
 * @return Whether the directory was safely prepared.
 * @retval true @p slug and @p abs_dir contain complete results.
 * @retval false Joining, creation, or canonicalization failed.
 * @pre All pointer arguments are non-NULL.
 * @pre Output buffers satisfy their documented bounded capacities.
 * @post Success leaves a real series directory strictly beneath the output
 * root.
 * @post Failure is reported and callers do not use @p abs_dir.
 * @note Not safe for concurrent mutation of the same filesystem path.
 * @since 0.1.0
 */
RA8_PRIV bool priv_mdl_app_prepare_series_dir(const char* out_dir,
                                              const char* series_url,
                                              char*       slug,
                                              size_t      slug_cap,
                                              char*       abs_dir)
{
  mdl_urlname_last_segment(series_url, slug, slug_cap);
  mdl_storage_t* storage = &priv_mdl_app_context()->storage;
  if (priv_mdl_app_storage_ensure_directory(storage, out_dir) != k_ra8_ok) {
    (void)internal_prepare_diag3("mdl: library root is not a directory: ", out_dir, "\n");
    return false;
  }
  if (!mdl_join_dir_under(storage, out_dir, slug, abs_dir, (size_t)k_fw_fs_path_cap)) {
    (void)internal_prepare_diag3("mdl: refusing unsafe series path for slug '", slug, "'\n");
    return false;
  }
  return true;
}
