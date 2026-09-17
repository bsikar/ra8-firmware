/**
 * @file mdl_app_cover.c
 * @brief Cached series-cover acquisition and validated publication.
 * @details Revalidates cover URLs through the persistent per-host cache, keeps
 *          a verified local cover untouched on reuse, and publishes changed or
 *          recovered bytes only after magic-derived typing in one transaction.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#include <limits.h>
#include <stdint.h>
#include <string.h>

#include "mdl_app_internal.h"
#include "mdl_fetch_body_internal.h"

/**
 * @brief Copy one complete cover metadata string.
 * @details Measures before copying so state never retains a truncated body
 *          leaf after an otherwise successful publication.
 * @param[out] destination Writable destination field.
 * @param[in] capacity Destination byte capacity.
 * @param[in] source NUL-terminated source.
 * @return Whether the complete source fit.
 * @retval true The destination equals the complete source.
 * @retval false An argument was invalid or the source was oversized.
 * @pre A non-NULL destination spans @p capacity bytes.
 * @pre A non-NULL source is NUL-terminated.
 * @post Success leaves a complete NUL-terminated copy.
 * @post Failure clears a nonempty destination buffer.
 * @note Allocation-free and thread-safe across distinct buffers.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_cover_copy(char* destination, size_t capacity, const char* source)
{
  if ((destination == nullptr) || (capacity == 0U) || (source == nullptr)) {
    return false;
  }
  const size_t length = strlen(source);
  if (length >= capacity) {
    destination[0] = '\0';
    return false;
  }
  memcpy(destination, source, length + 1U);
  return true;
}

/**
 * @brief Determine whether the recorded local cover still has image magic.
 * @details Resolves the retained relative path beneath the series directory
 *          and accepts it only when the portable sniffer recognizes its bytes.
 * @param[in,out] ctx Active fetch context and storage binding.
 * @param[in] series_directory Canonical absolute series directory.
 * @return Whether a verified local cover exists.
 * @retval true State names a present supported image.
 * @retval false State has no cover or its path/bytes are invalid.
 * @pre Both pointers are non-NULL and the directory is canonical.
 * @pre State has passed its persistent validation contract.
 * @post No state or file content is modified.
 * @post True proves the current cover is safe for packaging.
 * @note File inspection uses only the injected portable storage facade.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_cover_current(mdl_fetch_ctx_t* ctx, const char* series_directory)
{
  const char* relative = priv_mdl_app_context()->state.cover_path;
  if (relative[0] == '\0') {
    return false;
  }
  char existing[k_fw_fs_path_cap];
  return mdl_path_join(series_directory, relative, existing, sizeof(existing)) &&
         (mdl_urlname_sniff_file(ctx->storage, existing, nullptr, nullptr, 0U, nullptr, 0U) ==
          k_ra8_ok);
}

/**
 * @brief Validate, name, and publish one complete cover body transaction.
 * @details Flushes the retained signature, copies the magic-derived body leaf
 *          into state, and commits only after independent staged validation.
 * @param[in,out] body Completed cover body transaction.
 * @param[in] bytes Exact response-body extent for diagnostics.
 * @return Canonical classification, state, or publication status.
 * @retval k_ra8_ok A validated cover and state leaf are available.
 * @retval k_ra8_err_invalid_size The state leaf could not fit.
 * @retval other Image classification, diagnostic, commit, or abort failed.
 * @pre @p body is initialized and exclusively owned.
 * @pre Every accepted response byte has reached @p body.
 * @post Success publishes exactly one magic-typed cover.
 * @post Failure leaves no active cover transaction.
 * @note A post-publication error clears the state leaf rather than lying.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_cover_finish(mdl_fetch_body_t* body, size_t bytes)
{
  ra8_err_t error = priv_mdl_fetch_body_prepare(body);
  if (error != k_ra8_ok) {
    (void)priv_mdl_fetch_body_abort(body);
    ra8_err_t output_error = priv_mdl_stream_text(k_ra8_ok,
                                                  priv_mdl_app_context()->diagnostic,
                                                  "mdl: downloaded series cover is not a "
                                                  "supported image (");
    output_error = priv_mdl_stream_u64(output_error, priv_mdl_app_context()->diagnostic, bytes);
    output_error =
      priv_mdl_stream_text(output_error, priv_mdl_app_context()->diagnostic, " bytes)\n");
    return (output_error == k_ra8_ok) ? error : output_error;
  }
  if (!internal_cover_copy(priv_mdl_app_context()->state.cover_path,
                           sizeof(priv_mdl_app_context()->state.cover_path),
                           body->actual_rel)) {
    (void)priv_mdl_fetch_body_abort(body);
    return k_ra8_err_invalid_size;
  }
  error = priv_mdl_fetch_body_commit(body);
  if (error != k_ra8_ok) {
    priv_mdl_app_context()->state.cover_path[0] = '\0';
  }
  return error;
}

/**
 * @brief Feed one cached cover buffer into the validated image transaction.
 * @details Reuses the production body sink so cached bytes follow the same
 *          signature, naming, stage-validation, and publication path as a stream.
 * @param[in,out] ctx Active fetch and storage context.
 * @param[in] holding Canonical requested holding path.
 * @param[in] bytes Complete cached body extent.
 * @return Canonical body-sink or publication status.
 * @retval k_ra8_ok The cached body became a validated local cover.
 * @retval k_ra8_err_invalid_size The body exceeds the sink write width.
 * @retval other Sink initialization, writing, validation, or commit failed.
 * @pre @p ctx and @p holding are non-NULL.
 * @pre `ctx->page_buf` spans at least @p bytes initialized bytes.
 * @post Success publishes exactly the cached bytes once.
 * @post Failure attempts to abort every active stage.
 * @note This path performs no network operation.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_cover_publish_cached(mdl_fetch_ctx_t* ctx, const char* holding, size_t bytes)
{
  if (bytes > UINT32_MAX) {
    return k_ra8_err_invalid_size;
  }
  mdl_fetch_body_t body = {};
  ra8_err_t error = priv_mdl_fetch_body_init_image(&body, ctx->storage, holding, "cover.download");
  mdl_net_body_sink_t sink     = priv_mdl_fetch_body_sink(&body);
  uint32_t            accepted = 0U;
  if (error == k_ra8_ok) {
    error = sink.write(sink.ctx, (const uint8_t*)ctx->page_buf, (uint32_t)bytes, &accepted);
  }
  if ((error == k_ra8_ok) && (accepted != (uint32_t)bytes)) {
    error = k_ra8_err_invalid_state;
  }
  if (error != k_ra8_ok) {
    const ra8_err_t aborted = priv_mdl_fetch_body_abort(&body);
    return (aborted == k_ra8_ok) ? error : aborted;
  }
  return internal_cover_finish(&body, bytes);
}

/**
 * @brief Fetch one cover directly into a private image transaction.
 * @details Preserves the non-cache fallback for callers that intentionally do
 *          not supply a cache binding while retaining governed bounded retries.
 * @param[in,out] ctx Active fetch dependencies.
 * @param[in] holding Canonical requested holding path.
 * @param[in] host Parsed governor host key.
 * @param[in] minimum_delay Minimum request delay in milliseconds.
 * @param[in] maximum_delay Maximum request delay in milliseconds.
 * @return Canonical network or publication status.
 * @retval k_ra8_ok The streamed cover was validated and published.
 * @retval other Network, body, diagnostic, or transaction work failed.
 * @pre Every pointer is non-NULL and delay bounds are valid.
 * @pre Robots and URL policy already accepted the cover URL.
 * @post Success names one verified local cover in state.
 * @post Failure records the exact transfer failure and aborts its stage.
 * @note Used only when `ctx->cache == NULL`.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_cover_stream(mdl_fetch_ctx_t* ctx,
                                                    const char*      holding,
                                                    const char*      host,
                                                    uint32_t         minimum_delay,
                                                    uint32_t         maximum_delay)
{
  const mdl_net_req_t request = {.user_agent = ctx->session->user_agent,
                                 .referer    = priv_mdl_app_context()->state.series_url,
                                 .timeout_ms = ctx->timeout_ms};
  mdl_fetch_body_t    body    = {};
  ra8_err_t error = priv_mdl_fetch_body_init_image(&body, ctx->storage, holding, "cover.download");
  mdl_net_body_sink_t sink     = priv_mdl_fetch_body_sink(&body);
  mdl_net_resp_t      response = {};
  size_t              bytes    = 0U;
  if (error == k_ra8_ok) {
    error = priv_mdl_fetch_with_retry(ctx,
                                      host,
                                      priv_mdl_app_context()->state.cover_url,
                                      &request,
                                      &sink,
                                      minimum_delay,
                                      maximum_delay,
                                      &response,
                                      &bytes);
  }
  if (error == k_ra8_ok) {
    return internal_cover_finish(&body, bytes);
  }
  const ra8_err_t aborted = priv_mdl_fetch_body_abort(&body);
  error                   = (aborted == k_ra8_ok) ? error : aborted;
  priv_mdl_fetch_record_fail(ctx, priv_mdl_app_context()->state.cover_url, response.status, error);
  return error;
}

/**
 * @brief Fetch or revalidate one cover through the persistent host cache.
 * @details Uses the governed buffer callback, reports staleness, leaves a
 *          verified current cover untouched on reuse, and publishes otherwise.
 * @param[in,out] ctx Active fetch and cache dependencies.
 * @param[in] holding Canonical requested holding path.
 * @param[in] host Parsed governor host key.
 * @param[in] minimum_delay Minimum request delay in milliseconds.
 * @param[in] maximum_delay Maximum request delay in milliseconds.
 * @param[in] current Whether a verified local cover already exists.
 * @return Canonical cache, diagnostic, or publication status.
 * @retval k_ra8_ok A current verified cover exists after the call.
 * @retval other Cache fetching, reporting, image validation, or publication failed.
 * @pre Every pointer is non-NULL and `ctx->cache` is initialized.
 * @pre Robots and URL policy already accepted the cover URL.
 * @post A 304/current-body reuse never opens the cover destination for writing.
 * @post Changed or recovered bytes use the validated body transaction.
 * @note The shared page buffer is free after series metadata extraction.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_cover_cached(mdl_fetch_ctx_t* ctx,
                                                    const char*      holding,
                                                    const char*      host,
                                                    uint32_t         minimum_delay,
                                                    uint32_t         maximum_delay,
                                                    bool             current)
{
  mdl_fetch_cache_request_t fetch = {.ctx = ctx, .jmin = minimum_delay, .jmax = maximum_delay};
  if (!internal_cover_copy(fetch.host, sizeof(fetch.host), host)) {
    return k_ra8_err_invalid_size;
  }
  const mdl_net_req_t request  = {.user_agent = ctx->session->user_agent,
                                  .referer    = priv_mdl_app_context()->state.series_url,
                                  .timeout_ms = ctx->timeout_ms};
  mdl_net_resp_t      response = {};
  mdl_cache_result_t  result;
  size_t              bytes = 0U;
  ra8_err_t           error = mdl_cache_get_buf(ctx->cache,
                                                priv_mdl_app_context()->state.cover_url,
                                                &request,
                                                priv_mdl_fetch_cache_get_buf,
                                                &fetch,
                                                ctx->page_buf,
                                                ctx->page_cap,
                                                &bytes,
                                                &response,
                                                &result);
  if (error == k_ra8_ok) {
    error = priv_mdl_app_report_cache(priv_mdl_app_context()->state.cover_url, &result);
  }
  if ((error == k_ra8_ok) && current && result.body_reused) {
    return k_ra8_ok;
  }
  if (error == k_ra8_ok) {
    return internal_cover_publish_cached(ctx, holding, bytes);
  }
  priv_mdl_fetch_record_fail(ctx, priv_mdl_app_context()->state.cover_url, response.status, error);
  return error;
}

RA8_PRIV ra8_err_t priv_mdl_app_ensure_series_cover(mdl_fetch_ctx_t* ctx,
                                                    const char*      series_directory)
{
  if ((ctx == nullptr) || (series_directory == nullptr)) {
    return k_ra8_err_invalid_arg;
  }
  if (priv_mdl_app_context()->state.cover_url[0] == '\0') {
    return k_ra8_ok;
  }
  const bool current = internal_cover_current(ctx, series_directory);
  if (current && (ctx->cache == nullptr) && !ctx->refetch) {
    return k_ra8_ok;
  }
  char holding[k_fw_fs_path_cap];
  if (!mdl_path_join(series_directory, "cover.download", holding, sizeof(holding))) {
    return k_ra8_err_invalid_size;
  }
  uint32_t crawl = 0U;
  if (!mdl_session_url_allowed(ctx->session, priv_mdl_app_context()->state.cover_url, &crawl)) {
    return k_ra8_err_access_denied;
  }
  char host[k_mdl_gov_host_max];
  if (!mdl_url_host(priv_mdl_app_context()->state.cover_url, host, sizeof(host))) {
    return k_ra8_err_invalid_arg;
  }
  const uint32_t minimum_delay =
    (ctx->site->img_delay_min > crawl) ? ctx->site->img_delay_min : crawl;
  const uint32_t maximum_delay =
    (ctx->site->img_delay_max > crawl) ? ctx->site->img_delay_max : crawl;
  return (ctx->cache != nullptr)
           ? internal_cover_cached(ctx, holding, host, minimum_delay, maximum_delay, current)
           : internal_cover_stream(ctx, holding, host, minimum_delay, maximum_delay);
}
