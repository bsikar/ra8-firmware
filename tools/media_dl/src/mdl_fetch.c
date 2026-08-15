/**
 * @file mdl_fetch.c
 * @brief State-aware chapter, asset, and run download orchestration.
 * @details Coordinates injected network, storage, verification, checkpointing,
 *          and progress contracts while preserving the first failure.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#include "mdl_fetch.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "mdl_fetch_body_internal.h"
#include "mdl_fetch_internal.h"
#include "mdl_net.h"
#include "mdl_pathfs.h"
#include "mdl_storage.h"
#include "mdl_stream_internal.h"
#include "mdl_url_guard.h"
#include "mdl_urlname.h"
#include "ra8_attributes.h"
#include "ra8_err.h"

/** @brief HTTP-status boundary used when rendering a failure reason. */
typedef enum : uint16_t {
  k_http_server_err = 500, /**< First status that is a server error. */
} mdl_fetch_http_t;

/** @brief Bounded attempts per request: the initial try plus backoff retries. */
typedef enum : uint8_t {
  k_fetch_max_attempts = 4U, /**< One initial request plus at most three retries. */
} mdl_fetch_retry_t;

/** @brief Per-chapter outcome reported to the run loop. */
typedef enum : uint8_t {
  k_ch_completed = 0, /**< Every page present and verified.  */
  k_ch_skipped   = 1, /**< Already complete (--update skip). */
  k_ch_failed    = 2, /**< Left partial by a page failure.   */
} mdl_chap_status_t;

/**
 * @struct mdl_chapter_work_t
 * @brief Bounded metadata and state accumulated while processing one chapter.
 * @since 0.1.0
 */
typedef struct {
  char               id[k_mdl_chapter_id_max];         /**< Stable chapter key.        */
  char               extracted_title[k_mdl_title_max]; /**< Selector title scratch.    */
  double             number;                           /**< Parsed chapter number.     */
  bool               number_known;                     /**< Explicit number presence.  */
  size_t             html_len;                         /**< Retained chapter HTML.     */
  const char*        title;                            /**< Selected persistent title. */
  mdl_chapter_rec_t* rec;                              /**< Persistent chapter record. */
} mdl_chapter_work_t;

/** @brief Emit three fetch diagnostics and latch a sink failure.
 * @details Writes fragments in order through the injected diagnostic stream.
 *          The first sink error is latched in the fetch context and returned.
 * @param[in,out] ctx Caller-owned operation context.
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
RA8_INTERNAL static ra8_err_t internal_mdl_fetch_diag3(mdl_fetch_ctx_t* ctx,
                                                       const char*      first,
                                                       const char*      second,
                                                       const char*      third)
{
  ra8_err_t error = priv_mdl_stream_text(k_ra8_ok, ctx->diagnostic, first);
  error           = priv_mdl_stream_text(error, ctx->diagnostic, second);
  error           = priv_mdl_stream_text(error, ctx->diagnostic, third);
  if (error != k_ra8_ok) {
    ctx->progress_error = error;
  }
  return error;
}

/**
 * @brief Perform the max u32 step.
 * @details Performs max u32 under the injected network, governor, and storage contracts; dependency failures are propagated before incomplete bytes are published.
 * @param[in] a First unsigned delay operand.
 * @param[in] b Second unsigned delay operand.
 * @return The bounded result computed from the supplied input.
 * @retval other The computed result in the function's declared domain.
 * @pre Required pointer arguments remain valid for the call duration.
 * @pre Supplied capacities cover their referenced bounded buffers.
 * @post No ownership of caller-provided storage is transferred.
 * @post Result status and outputs describe one completed synchronous attempt.
 * @note The function performs no dynamic allocation and retains no caller pointer.
 * @since 0.1.0
 */
RA8_INTERNAL static uint32_t internal_mdl_fetch_max_u32(uint32_t a, uint32_t b)
{
  return (a > b) ? a : b;
}

RA8_PRIV bool priv_mdl_fetch_is_retryable(ra8_err_t rc)
{
  return (rc == k_ra8_err_busy) || (rc == k_ra8_err_timeout) || (rc == k_ra8_fail);
}

RA8_PRIV bool priv_mdl_fetch_run_incomplete(const mdl_fetch_stats_t* stats)
{
  if (stats == nullptr) {
    return false;
  }
  return (stats->chapters_failed > 0U) || (stats->pages_failed > 0U);
}

/** @brief Prose for a k_ra8_fail: a 5xx server error versus a transport error.
 */
RA8_INTERNAL static const char* internal_mdl_fetch_fail_reason(long status)
{
  return (status >= (long)k_http_server_err) ? "server error" : "transport error";
}

RA8_PRIV void priv_mdl_fetch_reason(ra8_err_t err, long status, char* buf, size_t cap)
{
  if (buf == nullptr) {
    return;
  }
  if (cap == 0U) {
    return;
  }
  const char* base = nullptr;
  switch (err) {
    case k_ra8_ok:
      base = "ok";
      break;
    case k_ra8_err_timeout:
      base = "request timed out";
      break;
    case k_ra8_err_busy:
      base = "rate limited";
      break;
    case k_ra8_err_not_found:
      base = "not found";
      break;
    case k_ra8_err_no_mem:
      base = "response exceeded size cap";
      break;
    case k_ra8_err_retry_limit:
      base = "still failing after retries";
      break;
    case k_ra8_fail:
      base = internal_mdl_fetch_fail_reason(status);
      break;
    default:
      base = "error";
      break;
  }
  if (status > 0) {
    (void)snprintf(buf, cap, "%s (HTTP %ld)", base, status);
  } else {
    (void)snprintf(buf, cap, "%s", base);
  }
}

/** @brief Append one failure to the run's log (when set); always tally total.
 */
RA8_PRIV void
priv_mdl_fetch_record_fail(const mdl_fetch_ctx_t* ctx, const char* url, long status, ra8_err_t err)
{
  if (ctx->faillog == nullptr) {
    return;
  }
  mdl_fetch_faillog_t* log = ctx->faillog;
  log->total += 1U;
  if (log->count >= (size_t)k_mdl_fetch_fail_max) {
    return;
  }
  mdl_fetch_fail_t* item = &log->items[log->count];
  (void)snprintf(item->url, sizeof(item->url), "%s", (url != nullptr) ? url : "");
  item->status = status;
  item->err    = err;
  log->count += 1U;
}

/** @brief Persist state atomically when a checkpoint target is configured. */
RA8_PRIV ra8_err_t priv_mdl_fetch_checkpoint(const mdl_fetch_ctx_t* ctx)
{
  if (ctx->state_path == nullptr) {
    return k_ra8_ok;
  }
  bool            published = false;
  const ra8_err_t rc        = mdl_state_save(ctx->storage, ctx->state_path, ctx->state, &published);
  if (rc != k_ra8_ok) {
    priv_mdl_fetch_record_fail(ctx, ctx->state_path, 0L, rc);
  }
  return rc;
}

/** @brief Governor host key for `url`, or NULL when it cannot be parsed. */
RA8_INTERNAL static const char* internal_mdl_fetch_page_host(const char* url, char* buf, size_t cap)
{
  return mdl_url_host(url, buf, cap) ? buf : nullptr;
}

/**
 * @brief One governed body transfer: pace, fetch, feed the outcome back.
 * @details Dispatches through an injected caller-owned sink and always releases
 *          the governor slot after the synchronous attempt.
 * @param[in,out] ctx Borrowed operation context.
 * @param[in] host Origin host governing pacing and robots policy.
 * @param[in] url Canonical input URL.
 * @param[in] req Validated discovery or request description.
 * @param[in,out] sink Reset/write destination for this attempt.
 * @param[in] jmin Minimum retry-jitter delay in milliseconds.
 * @param[in] jmax Maximum retry-jitter delay in milliseconds.
 * @param[out] out_resp Receives normalized response metadata.
 * @param[out] out_bytes Receives the transferred byte count.
 * @return Canonical downloader status.
 * @retval k_ra8_ok The operation completed.
 * @retval other Validation, capacity, network, or storage failed.
 * @pre Required pointer arguments remain valid for the call duration.
 * @pre Supplied capacities cover their referenced bounded buffers.
 * @post No ownership of caller-provided storage is transferred.
 * @post Result status and outputs describe one completed synchronous attempt.
 * @note The function performs no dynamic allocation and retains no caller pointer.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_mdl_fetch_governed_get_body(mdl_fetch_ctx_t*     ctx,
                                                                   const char*          host,
                                                                   const char*          url,
                                                                   const mdl_net_req_t* req,
                                                                   mdl_net_body_sink_t* sink,
                                                                   uint32_t             jmin,
                                                                   uint32_t             jmax,
                                                                   mdl_net_resp_t*      out_resp,
                                                                   size_t*              out_bytes)
{
  if (mdl_governor_acquire(ctx->gov, host, jmin, jmax) == k_ra8_err_would_block) {
    return k_ra8_err_would_block;
  }
  size_t          got  = 0U;
  mdl_net_resp_t  resp = {};
  const ra8_err_t rc   = mdl_net_get_body(ctx->session->net, url, req, sink, &got, &resp);
  mdl_governor_observe(ctx->gov, host, resp.status, resp.retry_after);
  mdl_governor_release(ctx->gov, host);
  if (out_resp != nullptr) {
    *out_resp = resp;
  }
  if (out_bytes != nullptr) {
    *out_bytes = got;
  }
  return rc;
}

/** @brief Bounded, governed retry of one image transfer; last status latched.
 */
RA8_PRIV ra8_err_t priv_mdl_fetch_with_retry(mdl_fetch_ctx_t*     ctx,
                                             const char*          host,
                                             const char*          url,
                                             const mdl_net_req_t* req,
                                             mdl_net_body_sink_t* sink,
                                             uint32_t             jmin,
                                             uint32_t             jmax,
                                             mdl_net_resp_t*      out_resp,
                                             size_t*              out_bytes)
{
  ra8_err_t rc = k_ra8_fail;
  for (uint8_t attempt = 0U; attempt < (uint8_t)k_fetch_max_attempts; ++attempt) {
    rc = internal_mdl_fetch_governed_get_body(ctx,
                                              host,
                                              url,
                                              req,
                                              sink,
                                              jmin,
                                              jmax,
                                              out_resp,
                                              out_bytes);
    if (!priv_mdl_fetch_is_retryable(rc)) {
      break; /* success, or a permanent failure not worth retrying */
    }
  }
  return rc;
}

ra8_err_t mdl_fetch_asset(mdl_fetch_ctx_t* ctx,
                          const char*      url,
                          const char*      target_abs,
                          const char*      referer,
                          mdl_net_resp_t*  out_resp,
                          size_t*          out_bytes)
{
  if (out_resp != nullptr) {
    *out_resp = (mdl_net_resp_t){};
  }
  if (out_bytes != nullptr) {
    *out_bytes = 0U;
  }
  if ((ctx == nullptr) || (ctx->session == nullptr) || (ctx->session->net == nullptr) ||
      (ctx->site == nullptr) || (url == nullptr) || (url[0] == '\0') || (target_abs == nullptr) ||
      (target_abs[0] != '/')) {
    return k_ra8_err_invalid_arg;
  }

  uint32_t crawl = 0U;
  if (!mdl_session_url_allowed(ctx->session, url, &crawl)) {
    priv_mdl_fetch_record_fail(ctx, url, 0, k_ra8_fail);
    return k_ra8_fail;
  }
  char        hostbuf[k_mdl_gov_host_max];
  const char* host = internal_mdl_fetch_page_host(url, hostbuf, sizeof(hostbuf));
  if (host == nullptr) {
    return k_ra8_err_invalid_arg;
  }

  const uint32_t      jmin = internal_mdl_fetch_max_u32(ctx->site->img_delay_min, crawl);
  const uint32_t      jmax = internal_mdl_fetch_max_u32(ctx->site->img_delay_max, crawl);
  const mdl_net_req_t req  = {.user_agent        = ctx->session->user_agent,
                              .referer           = referer,
                              .if_none_match     = nullptr,
                              .if_modified_since = nullptr,
                              .timeout_ms        = ctx->timeout_ms};
  size_t              got  = 0U;
  mdl_net_resp_t      resp = {};
  mdl_fetch_body_t    body = {};
  ra8_err_t           rc   = priv_mdl_fetch_body_init_exact(&body, ctx->storage, target_abs);
  mdl_net_body_sink_t sink = priv_mdl_fetch_body_sink(&body);
  if (rc == k_ra8_ok) {
    rc = priv_mdl_fetch_with_retry(ctx, host, url, &req, &sink, jmin, jmax, &resp, &got);
  }
  if (out_resp != nullptr) {
    *out_resp = resp;
  }
  if (rc == k_ra8_ok) {
    rc = priv_mdl_fetch_body_prepare(&body);
  }
  if (rc == k_ra8_ok) {
    rc = priv_mdl_fetch_body_commit(&body);
  }
  if (rc != k_ra8_ok) {
    const ra8_err_t aborted = priv_mdl_fetch_body_abort(&body);
    const ra8_err_t result  = (aborted == k_ra8_ok) ? rc : aborted;
    priv_mdl_fetch_record_fail(ctx, url, resp.status, result);
    return result;
  }
  if (out_bytes != nullptr) {
    *out_bytes = got;
  }
  return k_ra8_ok;
}

/**
 * @brief Fetch one cache attempt through governor and bounded retry policy.
 * @param[in,out] context ::mdl_fetch_cache_request_t state.
 * @param[in] url Exact chapter URL.
 * @param[in] request Conditional request metadata.
 * @param[out] buffer Bounded HTML destination.
 * @param[in] capacity Writable destination capacity.
 * @param[out] out_length Exact received bytes.
 * @param[out] response Finished response metadata.
 * @return Canonical governed network status.
 * @pre Every pointer is non-NULL and the context is fully initialized.
 * @pre @p buffer spans @p capacity writable bytes.
 * @post Every attempt is acquired, observed, and released exactly once.
 * @post Retryable results make at most ::k_fetch_max_attempts attempts.
 * @note No callback argument is retained.
 * @since 0.1.0
 */
RA8_PRIV ra8_err_t priv_mdl_fetch_cache_get_buf(void*                context,
                                                const char*          url,
                                                const mdl_net_req_t* request,
                                                char*                buffer,
                                                size_t               capacity,
                                                size_t*              out_length,
                                                mdl_net_resp_t*      response)
{
  mdl_fetch_cache_request_t* fetch = (mdl_fetch_cache_request_t*)context;
  ra8_err_t                  error = k_ra8_fail;
  for (uint8_t attempt = 0U; attempt < (uint8_t)k_fetch_max_attempts; ++attempt) {
    error = mdl_governor_acquire(fetch->ctx->gov, fetch->host, fetch->jmin, fetch->jmax);
    if (error == k_ra8_ok) {
      error = mdl_net_get_buf(fetch->ctx->session->net,
                              url,
                              request,
                              buffer,
                              capacity,
                              out_length,
                              response);
      mdl_governor_observe(fetch->ctx->gov, fetch->host, response->status, response->retry_after);
      mdl_governor_release(fetch->ctx->gov, fetch->host);
    }
    if (!priv_mdl_fetch_is_retryable(error)) {
      break;
    }
  }
  return error;
}

/**
 * @brief Fetch one chapter HTML document and extract its page URLs.
 * @details Applies robots policy, governed bounded retries, then retains the
 *          successful HTML bytes in `ctx->page_buf` for immediate metadata use.
 * @param[in,out] ctx         Fully configured fetch context.
 * @param[in]     chapter_url Absolute chapter URL.
 * @param[out]    out_len     Retained HTML byte count.
 * @return An ::ra8_err_t fetch/extraction result.
 * @retval k_ra8_ok          At least one page URL was extracted.
 * @retval k_ra8_err_no_data The HTML held no matching page image.
 * @retval k_ra8_fail        Robots or network policy refused/failed the fetch.
 * @pre All pointer arguments are non-NULL.
 * @pre `ctx->page_buf` and `ctx->images` are writable caller storage.
 * @post On success, @p out_len is nonzero and the HTML remains in the buffer.
 * @post On success, `ctx->images->count` is nonzero.
 * @note Not thread-safe: mutates session, governor, and scratch storage.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_mdl_fetch_chapter_html(mdl_fetch_ctx_t* ctx, const char* chapter_url, size_t* out_len)
{
  uint32_t crawl = 0U;
  if (!mdl_session_url_allowed(ctx->session, chapter_url, &crawl)) {
    priv_mdl_fetch_record_fail(ctx, chapter_url, 0, k_ra8_fail);
    return k_ra8_fail;
  }
  mdl_fetch_cache_request_t fetch = {
    .ctx  = ctx,
    .jmin = internal_mdl_fetch_max_u32(ctx->site->chapter_delay_min, crawl),
    .jmax = internal_mdl_fetch_max_u32(ctx->site->chapter_delay_max, crawl)};
  if (!mdl_url_host(chapter_url, fetch.host, sizeof(fetch.host))) {
    return k_ra8_err_invalid_arg;
  }
  const mdl_net_req_t req      = {.user_agent = ctx->session->user_agent,
                                  .referer    = ctx->series_url,
                                  .timeout_ms = ctx->timeout_ms};
  size_t              len      = 0U;
  mdl_net_resp_t      response = {};
  mdl_cache_result_t  cache_result;
  const ra8_err_t     rc = (ctx->cache != nullptr) ? mdl_cache_get_buf(ctx->cache,
                                                                       chapter_url,
                                                                       &req,
                                                                       priv_mdl_fetch_cache_get_buf,
                                                                       &fetch,
                                                                       ctx->page_buf,
                                                                       ctx->page_cap,
                                                                       &len,
                                                                       &response,
                                                                       &cache_result)
                                                   : priv_mdl_fetch_cache_get_buf(&fetch,
                                                                                  chapter_url,
                                                                                  &req,
                                                                                  ctx->page_buf,
                                                                                  ctx->page_cap,
                                                                                  &len,
                                                                                  &response);
  if (rc != k_ra8_ok) {
    priv_mdl_fetch_record_fail(ctx, chapter_url, response.status, rc);
    return k_ra8_fail;
  }
  const ra8_err_t erc = mdl_extract_images(ctx->page_buf,
                                           len,
                                           chapter_url,
                                           ctx->site->page_img_attr,
                                           ctx->site->page_img_url_contains,
                                           ctx->images);
  if (erc != k_ra8_ok) {
    priv_mdl_fetch_record_fail(ctx, chapter_url, response.status, erc);
    return erc;
  }
  if (ctx->images->count == 0U) {
    priv_mdl_fetch_record_fail(ctx, chapter_url, response.status, k_ra8_err_no_data);
    return k_ra8_err_no_data;
  }
  *out_len = len;
  return k_ra8_ok;
}

/**
 * @brief Resolve the output directory and starting page number for one chapter.
 * @details Selects the shared combined destination or creates the chapter's
 *          contained directory, then returns the matching absolute/relative
 *          directory pointers and numbering base.
 * @param[in,out] ctx Fetch context containing storage and the series root.
 * @param[in] layout Combined or per-chapter layout selection.
 * @param[in] id Sanitized chapter identifier.
 * @param[in] combined_abs Canonical combined-output directory.
 * @param[in] combined_rel Library-relative combined-output directory.
 * @param[in] global_no Next combined-layout page number.
 * @param[out] chap_abs Scratch buffer for a per-chapter absolute directory.
 * @param[in] chap_cap Capacity of @p chap_abs in bytes.
 * @param[out] dest_abs Receives the selected absolute directory pointer.
 * @param[out] dest_rel Receives the selected relative directory pointer.
 * @param[out] base Receives the selected starting page number.
 * @return Whether all selected paths were prepared within bounds.
 * @retval true Destination pointers and @p base were initialized.
 * @retval false The per-chapter directory was invalid or could not be created.
 * @pre Every pointer is non-NULL and text inputs are NUL-terminated.
 * @pre @p chap_abs references @p chap_cap writable bytes.
 * @post Success selects only a path contained by the configured series root.
 * @post Failure publishes no destination pointer or chapter completion record.
 * @note Returned path pointers remain borrowed from caller-owned storage.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_mdl_fetch_resolve_dest(mdl_fetch_ctx_t*   ctx,
                                                         mdl_fetch_layout_t layout,
                                                         const char*        id,
                                                         const char*        combined_abs,
                                                         const char*        combined_rel,
                                                         size_t             global_no,
                                                         char*              chap_abs,
                                                         size_t             chap_cap,
                                                         const char**       dest_abs,
                                                         const char**       dest_rel,
                                                         size_t*            base)
{
  if (layout == k_mdl_layout_combined) {
    *dest_abs = combined_abs;
    *dest_rel = combined_rel;
    *base     = global_no;
    return true;
  }
  if (!mdl_join_dir_under(ctx->storage, ctx->series_abs_dir, id, chap_abs, chap_cap)) {
    return false;
  }
  *dest_abs = chap_abs;
  *dest_rel = id;
  *base     = 0U;
  return true;
}

/**
 * @brief Mark a chapter complete and advance combined numbering.
 * @details Performs mark complete under the injected network, governor, and storage contracts; dependency failures are propagated before incomplete bytes are published.
 * @param[in,out] ctx Borrowed operation context.
 * @param[in] layout Selected combined or per-chapter layout.
 * @param[in,out] rec Persistent chapter record to mark complete.
 * @param[out] global_no Combined-layout page number advanced on completion.
 * @pre Required pointer arguments remain valid for the call duration.
 * @pre Supplied capacities cover their referenced bounded buffers.
 * @post No ownership of caller-provided storage is transferred.
 * @post Result status and outputs describe one completed synchronous attempt.
 * @note The function performs no dynamic allocation and retains no caller pointer.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_mdl_fetch_mark_complete(mdl_fetch_ctx_t*   ctx,
                                                          mdl_fetch_layout_t layout,
                                                          mdl_chapter_rec_t* rec,
                                                          size_t*            global_no)
{
  rec->pages_done = (uint16_t)ctx->images->count;
  rec->complete   = true;
  rec->fetched_at = (int64_t)time(nullptr);
  if (layout == k_mdl_layout_combined) {
    *global_no += ctx->images->count;
  }
}

/**
 * @brief Select one bounded persistent chapter title.
 * @details Performs select chapter title under the injected network, governor, and storage contracts; dependency failures are propagated before incomplete bytes are published.
 * @param[in,out] ctx Fetch context and retained chapter HTML.
 * @param[in] chapter_url Absolute chapter URL.
 * @param[in,out] work Chapter metadata scratch.
 * @return Whether title selection can continue.
 * @retval true The condition holds or the operation completed.
 * @retval false Input was rejected or the condition does not hold.
 * @pre Required pointer arguments remain valid for the call duration.
 * @pre Supplied capacities cover their referenced bounded buffers.
 * @post Success sets @ref mdl_chapter_work_t::title.
 * @post An over-cap matched title is logged and rejected without truncation.
 * @note The function performs no dynamic allocation and retains no caller pointer.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_mdl_fetch_select_chapter_title(mdl_fetch_ctx_t*    ctx,
                                                                 const char*         chapter_url,
                                                                 mdl_chapter_work_t* work)
{
  work->title = work->rec->title;
  if (ctx->site->chapter_title_selector[0] == '\0') {
    if (work->rec->title[0] == '\0') {
      work->title = work->id;
    }
    return true;
  }
  const ra8_err_t rc = mdl_extract_selector(ctx->page_buf,
                                            work->html_len,
                                            ctx->site->chapter_title_selector,
                                            work->extracted_title,
                                            sizeof(work->extracted_title));
  if (rc == k_ra8_ok) {
    work->title = work->extracted_title;
    return true;
  }
  if (rc == k_ra8_err_invalid_size) {
    (void)internal_mdl_fetch_diag3(ctx,
                                   "media_dl: chapter title for '",
                                   chapter_url,
                                   "' exceeds the persistent field; refusing truncation\n");
    priv_mdl_fetch_record_fail(ctx, chapter_url, 0, rc);
    return false;
  }
  const bool preserving   = work->rec->title[0] != '\0';
  ra8_err_t  output_error = internal_mdl_fetch_diag3(ctx,
                                                     "media_dl: chapter title selector '",
                                                     ctx->site->chapter_title_selector,
                                                     "' failed for '");
  output_error            = priv_mdl_stream_text(output_error, ctx->diagnostic, chapter_url);
  output_error            = priv_mdl_stream_text(output_error, ctx->diagnostic, "'; ");
  output_error =
    priv_mdl_stream_text(output_error,
                         ctx->diagnostic,
                         preserving ? "preserving recorded title\n" : "using chapter id\n");
  if (output_error != k_ra8_ok) {
    ctx->progress_error = output_error;
    return false;
  }
  if (!preserving) {
    work->title = work->id;
  }
  return true;
}

/**
 * @brief Select and strictly parse optional descriptor-driven chapter numbering.
 * @details Performs select chapter number under the injected network, governor, and storage contracts; dependency failures are propagated before incomplete bytes are published.
 * @param[in,out] ctx Fetch context and retained chapter HTML.
 * @param[in] chapter_url Absolute chapter URL.
 * @param[in,out] work Chapter metadata scratch.
 * @return Whether number selection can continue.
 * @retval true The condition holds or the operation completed.
 * @retval false Input was rejected or the condition does not hold.
 * @pre Required pointer arguments remain valid for the call duration.
 * @pre Supplied capacities cover their referenced bounded buffers.
 * @post Success preserves the recorded number or stores one complete finite match.
 * @post Malformed or over-cap matched text is logged and rejected.
 * @note The function performs no dynamic allocation and retains no caller pointer.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_mdl_fetch_select_chapter_number(mdl_fetch_ctx_t*    ctx,
                                                                  const char*         chapter_url,
                                                                  mdl_chapter_work_t* work)
{
  if (ctx->site->chapter_number_selector[0] == '\0') {
    return true;
  }
  char            extracted[k_mdl_title_max];
  const ra8_err_t rc = mdl_extract_selector(ctx->page_buf,
                                            work->html_len,
                                            ctx->site->chapter_number_selector,
                                            extracted,
                                            sizeof(extracted));
  if (rc == k_ra8_ok) {
    if (mdl_urlname_chapter_text_parse(extracted, &work->number)) {
      work->number_known = true;
      return true;
    }
    ra8_err_t output_error = internal_mdl_fetch_diag3(ctx,
                                                      "media_dl: chapter number selector '",
                                                      ctx->site->chapter_number_selector,
                                                      "' returned invalid value '");
    output_error           = priv_mdl_stream_text(output_error, ctx->diagnostic, extracted);
    output_error           = priv_mdl_stream_text(output_error, ctx->diagnostic, "' for '");
    output_error           = priv_mdl_stream_text(output_error, ctx->diagnostic, chapter_url);
    output_error           = priv_mdl_stream_text(output_error, ctx->diagnostic, "'\n");
    if (output_error != k_ra8_ok) {
      ctx->progress_error = output_error;
    }
    priv_mdl_fetch_record_fail(ctx, chapter_url, 0, k_ra8_err_validation_failed);
    return false;
  }
  if (rc == k_ra8_err_invalid_size) {
    (void)internal_mdl_fetch_diag3(ctx,
                                   "media_dl: chapter number for '",
                                   chapter_url,
                                   "' exceeds the bounded selector field\n");
    priv_mdl_fetch_record_fail(ctx, chapter_url, 0, rc);
    return false;
  }
  ra8_err_t output_error = internal_mdl_fetch_diag3(ctx,
                                                    "media_dl: chapter number selector '",
                                                    ctx->site->chapter_number_selector,
                                                    "' failed for '");
  output_error           = priv_mdl_stream_text(output_error, ctx->diagnostic, chapter_url);
  output_error           = priv_mdl_stream_text(output_error, ctx->diagnostic, "'; preserving ");
  output_error =
    priv_mdl_stream_text(output_error,
                         ctx->diagnostic,
                         work->number_known ? "recorded number\n" : "unknown number\n");
  if (output_error != k_ra8_ok) {
    ctx->progress_error = output_error;
    return false;
  }
  return true;
}

/**
 * @brief Persist metadata, fetch pages, and checkpoint one prepared chapter.
 * @details Performs chapter pages and checkpoint under the injected network, governor, and storage contracts; dependency failures are propagated before incomplete bytes are published.
 * @param[in,out] ctx Fetch dependencies and state.
 * @param[in] chapter_url Absolute chapter URL.
 * @param[in] layout Requested output layout.
 * @param[in] combined_abs Combined absolute output directory.
 * @param[in] combined_rel Combined relative output directory.
 * @param[in,out] global_no Combined page counter.
 * @param[in,out] stats Run counters.
 * @param[in] chapter_index One-based chapter position.
 * @param[in] chapter_total Total chapters in the run.
 * @param[in,out] work Prepared chapter metadata.
 * @return Completed or failed chapter status.
 * @retval other The computed result in the function's declared domain.
 * @pre Required pointer arguments remain valid for the call duration.
 * @pre Supplied capacities cover their referenced bounded buffers.
 * @post No ownership of caller-provided storage is transferred.
 * @post Result status and outputs describe one completed synchronous attempt.
 * @note The function performs no dynamic allocation and retains no caller pointer.
 * @since 0.1.0
 */
RA8_INTERNAL static mdl_chap_status_t
internal_mdl_fetch_chapter_pages_and_checkpoint(mdl_fetch_ctx_t*    ctx,
                                                const char*         chapter_url,
                                                mdl_fetch_layout_t  layout,
                                                const char*         combined_abs,
                                                const char*         combined_rel,
                                                size_t*             global_no,
                                                mdl_fetch_stats_t*  stats,
                                                size_t              chapter_index,
                                                size_t              chapter_total,
                                                mdl_chapter_work_t* work)
{
  if (!mdl_state_set_chapter_metadata(work->rec, work->title, work->number, work->number_known)) {
    return k_ch_failed;
  }
  work->rec->page_count = (uint16_t)ctx->images->count;
  if (work->rec->pages_done > work->rec->page_count) {
    work->rec->pages_done = 0U;
  }
  char        chap_abs[PATH_MAX];
  const char* dest_abs = nullptr;
  const char* dest_rel = nullptr;
  size_t      base     = 0U;
  if (!internal_mdl_fetch_resolve_dest(ctx,
                                       layout,
                                       work->id,
                                       combined_abs,
                                       combined_rel,
                                       *global_no,
                                       chap_abs,
                                       sizeof(chap_abs),
                                       &dest_abs,
                                       &dest_rel,
                                       &base)) {
    return k_ch_failed;
  }
  const mdl_run_pos_t pos = {.chapter_index = chapter_index,
                             .chapter_total = chapter_total,
                             .chapter_id    = work->id};
  if (priv_mdl_fetch_chapter_pages(ctx,
                                   chapter_url,
                                   dest_abs,
                                   dest_rel,
                                   base,
                                   work->rec,
                                   stats,
                                   &pos) != k_ra8_ok) {
    (void)priv_mdl_fetch_checkpoint(ctx);
    return k_ch_failed;
  }
  internal_mdl_fetch_mark_complete(ctx, layout, work->rec, global_no);
  return (priv_mdl_fetch_checkpoint(ctx) == k_ra8_ok) ? k_ch_completed : k_ch_failed;
}

/**
 * @brief Download one chapter (or skip it), returning its outcome.
 * @details Performs process chapter under the injected network, governor, and storage contracts; dependency failures are propagated before incomplete bytes are published.
 * @param[in,out] ctx Borrowed operation context.
 * @param[in] chapter_url Canonical chapter URL.
 * @param[in] layout Selected combined or per-chapter layout.
 * @param[in] combined_abs Canonical combined-output directory.
 * @param[in] combined_rel Portable combined-output path.
 * @param[out] global_no Combined-layout page number advanced on completion.
 * @param[in,out] stats Run counters to update.
 * @param[in] chapter_index Zero-based chapter index.
 * @param[in] chapter_total Total selected chapter count.
 * @return The bounded result computed from the supplied input.
 * @retval other The computed result in the function's declared domain.
 * @pre Required pointer arguments remain valid for the call duration.
 * @pre Supplied capacities cover their referenced bounded buffers.
 * @post No ownership of caller-provided storage is transferred.
 * @post Result status and outputs describe one completed synchronous attempt.
 * @note The function performs no dynamic allocation and retains no caller pointer.
 * @since 0.1.0
 */
RA8_INTERNAL static mdl_chap_status_t internal_mdl_fetch_process_chapter(mdl_fetch_ctx_t* ctx,
                                                                         const char* chapter_url,
                                                                         mdl_fetch_layout_t layout,
                                                                         const char* combined_abs,
                                                                         const char* combined_rel,
                                                                         size_t*     global_no,
                                                                         mdl_fetch_stats_t* stats,
                                                                         size_t chapter_index,
                                                                         size_t chapter_total)
{
  mdl_chapter_work_t work = {};
  mdl_urlname_last_segment(chapter_url, work.id, sizeof(work.id));
  work.number_known = mdl_urlname_chapter_parse(chapter_url, &work.number);
  work.rec          = mdl_state_add_chapter_numbered(ctx->state,
                                                     work.id,
                                                     chapter_url,
                                                     work.number,
                                                     work.number_known);
  if (work.rec == nullptr) {
    return k_ch_failed;
  }
  work.number       = work.rec->number;
  work.number_known = work.rec->number_known;
  if (ctx->update_only && work.rec->complete) {
    *global_no += work.rec->page_count;
    return k_ch_skipped;
  }
  work.rec->complete   = false;
  work.rec->fetched_at = 0;
  if (internal_mdl_fetch_chapter_html(ctx, chapter_url, &work.html_len) != k_ra8_ok) {
    return k_ch_failed;
  }
  if (!internal_mdl_fetch_select_chapter_title(ctx, chapter_url, &work) ||
      !internal_mdl_fetch_select_chapter_number(ctx, chapter_url, &work)) {
    return k_ch_failed;
  }
  return internal_mdl_fetch_chapter_pages_and_checkpoint(ctx,
                                                         chapter_url,
                                                         layout,
                                                         combined_abs,
                                                         combined_rel,
                                                         global_no,
                                                         stats,
                                                         chapter_index,
                                                         chapter_total,
                                                         &work);
}

/**
 * @brief Fold one chapter outcome into run statistics.
 * @details Increments exactly one completed, skipped, or failed counter and
 *          reports whether the outcome should fail the enclosing run.
 * @param[in] s Completed, skipped, or failed chapter outcome.
 * @param[in,out] stats Run counters to update.
 * @return Whether @p s represents failure.
 * @retval true The failed counter was incremented.
 * @retval false The completed or skipped counter was incremented.
 * @pre @p stats is non-NULL.
 * @pre The selected counter can be incremented without `size_t` overflow.
 * @post Exactly one chapter counter is incremented once.
 * @post Page and byte counters remain unchanged.
 * @note Unknown enum values are conservatively counted as failures.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_mdl_fetch_tally(mdl_chap_status_t s, mdl_fetch_stats_t* stats)
{
  if (s == k_ch_completed) {
    stats->chapters_completed += 1U;
  } else if (s == k_ch_skipped) {
    stats->chapters_skipped += 1U;
  } else {
    stats->chapters_failed += 1U;
  }
  return s == k_ch_failed;
}

/**
 * @brief Validate that every required fetch dependency is present
 * @details Checks the network session, portable storage binding, persistent
 * state, series/site metadata, and caller-owned extraction buffers before the
 * orchestrator performs any I/O.
 * @param[in] ctx Candidate fetch dependency bundle.
 * @return Dependency validation result.
 * @retval true Every mandatory pointer is non-null.
 * @retval false The context or at least one mandatory dependency is absent.
 * @pre @p ctx is either null or points to a readable context object.
 * @pre No concurrent thread mutates a non-null context during this check.
 * @post No context, backend, or caller buffer is modified.
 * @post Success authorizes only pointer presence; callee operations still
 *       validate their detailed lifecycle and capacity contracts.
 * @note Thread-safe for an immutable context.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_mdl_fetch_ctx_ready(const mdl_fetch_ctx_t* ctx)
{
  if (ctx == nullptr) {
    return false;
  }
  return (ctx->session != nullptr) && (ctx->storage != nullptr) && (ctx->state != nullptr) &&
         (ctx->series_abs_dir != nullptr) && (ctx->series_url != nullptr) &&
         (ctx->site != nullptr) && (ctx->page_buf != nullptr) && (ctx->images != nullptr);
}

ra8_err_t mdl_fetch_run(mdl_fetch_ctx_t*      ctx,
                        const mdl_url_list_t* chapters,
                        mdl_fetch_layout_t    layout,
                        const char*           combined_dir_rel,
                        mdl_fetch_stats_t*    stats)
{
  if ((ctx == nullptr) || (chapters == nullptr) || (stats == nullptr)) {
    return k_ra8_err_invalid_arg;
  }
  if (!internal_mdl_fetch_ctx_ready(ctx)) {
    return k_ra8_err_invalid_arg;
  }
  memset(stats, 0, sizeof(*stats));
  ctx->progress_error = k_ra8_ok;

  char combined_abs[PATH_MAX];
  if (layout == k_mdl_layout_combined) {
    if ((combined_dir_rel == nullptr) || !mdl_join_dir_under(ctx->storage,
                                                             ctx->series_abs_dir,
                                                             combined_dir_rel,
                                                             combined_abs,
                                                             sizeof(combined_abs))) {
      return k_ra8_err_invalid_arg;
    }
  } else {
    combined_abs[0] = '\0';
  }

  size_t global_no = 0U;
  bool   any_fail  = false;
  for (size_t i = 0U; i < chapters->count; ++i) {
    const mdl_chap_status_t s = internal_mdl_fetch_process_chapter(ctx,
                                                                   chapters->urls[i],
                                                                   layout,
                                                                   combined_abs,
                                                                   combined_dir_rel,
                                                                   &global_no,
                                                                   stats,
                                                                   i + 1U,
                                                                   chapters->count);
    if (internal_mdl_fetch_tally(s, stats)) {
      any_fail = true;
      if ((ctx->progress_error != k_ra8_ok) || (layout == k_mdl_layout_combined)) {
        break; /* the combined archive cannot be completed past a gap */
      }
    }
  }
  if (ctx->progress_error != k_ra8_ok) {
    return ctx->progress_error;
  }
  return any_fail ? k_ra8_fail : k_ra8_ok;
}
