/**
 * @file mdl_fetch_page.c
 * @brief Cached page validation, streamed transfer, and publication.
 * @details Implements one-page reuse and conditional-fetch state transitions
 *          over injected network and atomic storage contracts without allocation.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "mdl_fetch.h"
#include "mdl_fetch_body_internal.h"
#include "mdl_fetch_internal.h"
#include "mdl_hash.h"
#include "mdl_net.h"
#include "mdl_storage.h"
#include "mdl_url_guard.h"
#include "mdl_urlname.h"
#include "ra8_attributes.h"
#include "ra8_err.h"

/** @brief Local page-path buffer size. */
typedef enum : uint16_t {
  k_fetch_leaf_bytes = 32, /**< `page_NNNN.ext` leaf-name bytes. */
} mdl_fetch_page_size_t;

/** @brief Status and time conversions used by page accounting. */
typedef enum : uint32_t {
  k_http_not_modified = 304U,     /**< Conditional GET reused the held entity. */
  k_http_status_min   = 100U,     /**< Smallest valid HTTP response status.    */
  k_http_status_max   = 599U,     /**< Largest valid HTTP response status.     */
  k_ms_per_sec        = 1000U,    /**< Milliseconds per second.                */
  k_ns_per_ms         = 1000000U, /**< Nanoseconds per millisecond.            */
} mdl_fetch_page_bound_t;

/**
 * @struct mdl_page_outcome_t
 * @brief Transfer outcome used to produce one progress event.
 * @since 0.1.0
 */
typedef struct {
  uint64_t bytes;      /**< Bytes transferred, zero when reused. */
  uint32_t elapsed_ms; /**< Monotonic elapsed milliseconds.      */
  bool     reused;     /**< Whether verified local bytes won.    */
} mdl_page_outcome_t;

/**
 * @struct mdl_page_transfer_t
 * @brief Mutable state for one conditional page transfer.
 * @since 0.1.0
 */
typedef struct {
  const mdl_page_rec_t* recorded;                 /**< URL-keyed record, including refetch runs. */
  const mdl_page_rec_t* held;                     /**< Record eligible for conditional reuse.    */
  char                  host[k_mdl_gov_host_max]; /**< Parsed governor host key.                 */
  mdl_net_req_t         req;                      /**< Request metadata and validators.          */
  mdl_fetch_body_t      body;                     /**< Single portable transaction body sink.    */
  size_t                got;                      /**< Body bytes accepted by the sink.          */
  mdl_net_resp_t        resp;                     /**< Most recent response metadata.            */
  uint32_t              jmin;                     /**< Governed minimum image delay.             */
  uint32_t              jmax;                     /**< Governed maximum image delay.             */
} mdl_page_transfer_t;

/**
 * @brief Larger of two unsigned delay values.
 * @details Performs page max u32 under the injected network, governor, and storage contracts; dependency failures are propagated before incomplete bytes are published.
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
RA8_INTERNAL static uint32_t internal_mdl_fetch_page_max_u32(uint32_t a, uint32_t b)
{
  return (a > b) ? a : b;
}

/** @brief Governor host key for one page URL. */
RA8_INTERNAL static const char* internal_mdl_fetch_page_host(const char* url, char* buf, size_t cap)
{
  return mdl_url_host(url, buf, cap) ? buf : nullptr;
}

/**
 * @brief Perform the mono ms step.
 * @details Performs mono ms under the injected network, governor, and storage contracts; dependency failures are propagated before incomplete bytes are published.
 * @param[in] ctx Borrowed operation context.
 * @return The bounded result computed from the supplied input.
 * @retval other The computed result in the function's declared domain.
 * @pre Required pointer arguments remain valid for the call duration.
 * @pre Supplied capacities cover their referenced bounded buffers.
 * @post No ownership of caller-provided storage is transferred.
 * @post Result status and outputs describe one completed synchronous attempt.
 * @note The function performs no dynamic allocation and retains no caller pointer.
 * @since 0.1.0
 */
RA8_INTERNAL static int64_t internal_mdl_fetch_mono_ms(const mdl_fetch_ctx_t* ctx)
{
  if (ctx->progress_fn == nullptr) {
    return 0;
  }
  struct timespec ts = {};
  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
    return 0;
  }
  return ((int64_t)ts.tv_sec * (int64_t)k_ms_per_sec) +
         ((int64_t)ts.tv_nsec / (int64_t)k_ns_per_ms);
}

/**
 * @brief Emit one per-page progress event through the injected sink, if any.
 * @details Snapshots chapter position, page position, bytes, duration, and reuse
 *          into a stack event and synchronously invokes the optional callback.
 * @param[in] ctx Fetch context containing the optional callback and page count.
 * @param[in] pos Current chapter position and stable identifier.
 * @param[in] page_index Zero-based page index.
 * @param[in] out Completed page outcome to report.
 * @pre @p ctx, @p pos, and @p out are non-NULL.
 * @pre @p page_index is less than the current extracted page count.
 * @post A configured callback observes exactly one complete event.
 * @post No callback or context pointer is retained or transferred.
 * @note A missing callback is an intentional no-op.
 * @since 0.1.0

 * @return Operation status.
 * @retval k_ra8_ok The operation completed successfully.
 * @retval other The originating validation, storage, stream, or network error.
 */
RA8_INTERNAL static ra8_err_t internal_mdl_fetch_emit_progress(const mdl_fetch_ctx_t*    ctx,
                                                               const mdl_run_pos_t*      pos,
                                                               size_t                    page_index,
                                                               const mdl_page_outcome_t* out)
{
  if (ctx->progress_fn == nullptr) {
    return k_ra8_ok;
  }
  const mdl_fetch_progress_t ev = {.chapter_index = pos->chapter_index,
                                   .chapter_total = pos->chapter_total,
                                   .chapter_id    = pos->chapter_id,
                                   .page_index    = page_index,
                                   .page_total    = ctx->images->count,
                                   .page_bytes    = out->bytes,
                                   .elapsed_ms    = out->elapsed_ms,
                                   .reused        = out->reused};
  return ctx->progress_fn(ctx->progress_ctx, &ev);
}

/**
 * @brief Copy one file through the validated portable storage transaction
 * @details Adapts the canonical storage status to the fetcher's reuse boolean;
 * the underlying operation independently hashes the staged bytes before an
 * atomic create or truthful atomic replacement.
 * @param[in,out] storage Initialized storage binding and workspaces.
 * @param[in] src Canonical regular-file source path.
 * @param[in] dst Canonical distinct destination path.
 * @return Whether the complete validated copy was published.
 * @retval true The destination now contains the exact source snapshot.
 * @retval false Validation, I/O, capability, or publication failed.
 * @pre Every pointer is non-null and both paths share one bound filesystem.
 * @pre @p storage is exclusively owned for the duration of the copy.
 * @post Success publishes no partial destination.
 * @post Failure leaves any existing destination untouched.
 * @note Not thread-safe against concurrent mutation of either named object.
 * @since 0.1.0
 */
RA8_INTERNAL static bool
internal_mdl_fetch_copy_file(mdl_storage_t* storage, const char* src, const char* dst)
{
  return mdl_storage_copy_atomic(storage, src, dst) == k_ra8_ok;
}

/**
 * @brief Compose the `page_NNNN.ext` leaf for one page; false if it overran.
 * @details Derives the bounded extension from @p url and renders the one-based
 *          page number into caller-owned storage without filesystem access.
 * @param[in] page_no One-based page number.
 * @param[in] url Canonical page URL used to derive the extension.
 * @param[out] out Receives a NUL-terminated leaf on success.
 * @param[in] cap Capacity of @p out in bytes.
 * @return Whether the complete leaf fit.
 * @retval true @p out contains the complete page leaf.
 * @retval false Formatting failed or @p cap was insufficient.
 * @pre @p url and @p out are non-NULL.
 * @pre @p cap is nonzero and describes writable storage at @p out.
 * @post Success leaves a NUL-terminated filename within @p cap.
 * @post No pointer ownership is retained or transferred.
 * @note Performs no filesystem I/O.
 * @since 0.1.0
 */
RA8_INTERNAL static bool
internal_mdl_fetch_page_leaf(size_t page_no, const char* url, char* out, size_t cap)
{
  char ext[8];
  mdl_urlname_ext(url, ext, sizeof(ext));
  const int n = snprintf(out, cap, "page_%04zu.%s", page_no, ext);
  return (n > 0) && ((size_t)n < cap);
}

/**
 * @brief Reuse an already-held byte-identical page instead of fetching it.
 * @details Content-hash dedup: if a page with the same source URL is recorded
 *          and its file still verifies, put those bytes at the target position
 *          (a no-op when it is already there, else a copy) and record the new
 *          location -- no network. Returns false to fall through to a fetch.
 * @param[in,out] ctx Borrowed operation context.
 * @param[in] url_hash Stable hash of the canonical source URL.
 * @param[in] target_abs Absolute destination path.
 * @param[in] target_rel Library-relative destination path.
 * @return Whether the requested condition or operation succeeded.
 * @retval true The condition holds or the operation completed.
 * @retval false Input was rejected or the condition does not hold.
 * @pre Required pointer arguments remain valid for the call duration.
 * @pre Supplied capacities cover their referenced bounded buffers.
 * @post No ownership of caller-provided storage is transferred.
 * @post Result status and outputs describe one completed synchronous attempt.
 * @note The function performs no dynamic allocation and retains no caller pointer.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_mdl_fetch_try_reuse(mdl_fetch_ctx_t* ctx,
                                                      uint64_t         url_hash,
                                                      const char*      target_abs,
                                                      const char*      target_rel)
{
  const mdl_page_rec_t* held = mdl_state_find_page(ctx->state, url_hash);
  if (held == nullptr) {
    return false;
  }
  char      src_abs[PATH_MAX];
  const int sn = snprintf(src_abs, sizeof(src_abs), "%s/%s", ctx->series_abs_dir, held->rel_path);
  if ((sn < 0) || ((size_t)sn >= sizeof(src_abs))) {
    return false;
  }
  uint64_t have = 0U;
  if ((mdl_hash_file(ctx->storage, src_abs, &have) != k_ra8_ok) || (have != held->content_hash)) {
    return false; /* the held file is gone or torn: refetch */
  }

  char act_abs[PATH_MAX];
  char act_rel[k_mdl_relpath_max];
  (void)snprintf(act_abs, sizeof(act_abs), "%s", target_abs);
  (void)snprintf(act_rel, sizeof(act_rel), "%s", target_rel);

  const char* held_dot = strrchr(held->rel_path, '.');
  const char* tab_dot  = strrchr(act_abs, '.');
  const char* tre_dot  = strrchr(act_rel, '.');
  if ((held_dot != nullptr) && (tab_dot != nullptr) && (tre_dot != nullptr)) {
    const char* held_ext = held_dot + 1;
    if (strcmp(tab_dot + 1, held_ext) != 0) {
      (void)snprintf(act_abs,
                     sizeof(act_abs),
                     "%.*s.%s",
                     (int)(tab_dot - target_abs),
                     target_abs,
                     held_ext);
      (void)snprintf(act_rel,
                     sizeof(act_rel),
                     "%.*s.%s",
                     (int)(tre_dot - target_rel),
                     target_rel,
                     held_ext);
    }
  }

  if (strcmp(held->rel_path, act_rel) == 0) {
    return true; /* already in place (same-chapter resume) */
  }
  if (!internal_mdl_fetch_copy_file(ctx->storage, src_abs, act_abs)) {
    return false;
  }
  return mdl_state_add_page(ctx->state,
                            url_hash,
                            held->content_hash,
                            act_rel,
                            held->etag,
                            held->last_modified,
                            held->fetched_at,
                            held->response_status);
}

/**
 * @brief Remove a superseded URL-derived page only after its replacement
 * committed.
 * @details Performs discard stale page under the injected network, governor, and storage contracts; dependency failures are propagated before incomplete bytes are published.
 * @param[in] storage Portable storage binding.
 * @param[in] guessed_abs Original URL-derived destination.
 * @param[in] actual_abs Magic-derived committed destination.
 * @return Whether no conflicting stale page remains.
 * @retval true Paths match, the stale path was absent, or a regular stale file
 * was removed.
 * @retval false The stale path names a non-regular object or could not be
 * inspected/removed.
 * @pre Both paths are absolute, NUL-terminated, and share a parent directory.
 * @pre Supplied capacities cover their referenced bounded buffers.
 * @post @p actual_abs is never modified or removed.
 * @post Result status and outputs describe one completed synchronous attempt.
 * @note Symbolic links and directories are rejected rather than followed or
 * removed.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_mdl_fetch_discard_stale_page(mdl_storage_t* storage,
                                                               const char*    guessed_abs,
                                                               const char*    actual_abs)
{
  if ((storage == nullptr) || (guessed_abs == nullptr) || (actual_abs == nullptr)) {
    return false;
  }
  if (strcmp(guessed_abs, actual_abs) == 0) {
    return true;
  }
  fw_fs_stat_t    state = {};
  const ra8_err_t error = fw_fs_stat(&storage->fs->names, guessed_abs, &state);
  if (error != k_ra8_ok) {
    return false;
  }
  if (!state.exists) {
    return true;
  }
  if (state.type != k_fw_fs_node_file) {
    return false;
  }
  return fw_fs_unlink(&storage->fs->names, guessed_abs) == k_ra8_ok;
}

/**
 * @brief Prepare and execute the initial conditional page request.
 * @details Performs prepare page under the injected network, governor, and storage contracts; dependency failures are propagated before incomplete bytes are published.
 * @param[in,out] ctx Fetch dependencies and state.
 * @param[in] chapter_url Chapter URL used as the request Referer.
 * @param[in] url Absolute page URL.
 * @param[in] target_abs Final absolute page path.
 * @param[in] target_rel Final relative page path.
 * @param[out] tx Caller-owned transfer state.
 * @return ::k_ra8_ok after a response, otherwise the classified failure.
 * @retval k_ra8_ok The operation completed.
 * @retval other Validation, capacity, network, or storage failed.
 * @pre Every pointer is non-NULL and @p tx is writable.
 * @pre Supplied capacities cover their referenced bounded buffers.
 * @post Success leaves a response and caller-owned transactional body in @p tx.
 * @post Failure is recorded once and aborts any private transaction stage.
 * @note The function performs no dynamic allocation and retains no caller pointer.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_mdl_fetch_prepare_page(mdl_fetch_ctx_t*     ctx,
                                                              const char*          chapter_url,
                                                              const char*          url,
                                                              const char*          target_abs,
                                                              const char*          target_rel,
                                                              mdl_page_transfer_t* tx)
{
  uint32_t crawl = 0U;
  if (!mdl_session_url_allowed(ctx->session, url, &crawl)) {
    priv_mdl_fetch_record_fail(ctx, url, 0, k_ra8_fail);
    return k_ra8_fail;
  }
  tx->recorded     = mdl_state_find_page(ctx->state, mdl_hash_str(url));
  tx->held         = ctx->refetch ? nullptr : tx->recorded;
  const char* host = internal_mdl_fetch_page_host(url, tx->host, sizeof(tx->host));
  tx->jmin         = internal_mdl_fetch_page_max_u32(ctx->site->img_delay_min, crawl);
  tx->jmax         = internal_mdl_fetch_page_max_u32(ctx->site->img_delay_max, crawl);
  tx->req          = (mdl_net_req_t){
    .user_agent    = ctx->session->user_agent,
    .referer       = chapter_url,
    .if_none_match = (tx->held != nullptr && tx->held->etag[0] != '\0') ? tx->held->etag : nullptr,
    .if_modified_since = (tx->held != nullptr && tx->held->last_modified[0] != '\0')
                           ? tx->held->last_modified
                           : nullptr,
    .timeout_ms        = ctx->timeout_ms,
  };
  ra8_err_t rc = priv_mdl_fetch_body_init_image(&tx->body, ctx->storage, target_abs, target_rel);
  mdl_net_body_sink_t sink = priv_mdl_fetch_body_sink(&tx->body);
  if (rc == k_ra8_ok) {
    rc = priv_mdl_fetch_with_retry(ctx,
                                   host,
                                   url,
                                   &tx->req,
                                   &sink,
                                   tx->jmin,
                                   tx->jmax,
                                   &tx->resp,
                                   &tx->got);
  }
  if (rc != k_ra8_ok) {
    const ra8_err_t aborted = priv_mdl_fetch_body_abort(&tx->body);
    const ra8_err_t failure = (aborted == k_ra8_ok) ? rc : aborted;
    priv_mdl_fetch_record_fail(ctx, url, tx->resp.status, failure);
    return failure;
  }
  return k_ra8_ok;
}

/**
 * @brief Resolve HTTP 304 by verified reuse or one unconditional retry.
 * @details Performs resolve not modified under the injected network, governor, and storage contracts; dependency failures are propagated before incomplete bytes are published.
 * @param[in,out] ctx Fetch dependencies and state.
 * @param[in] url Absolute page URL.
 * @param[in] target_abs URL-derived absolute destination.
 * @param[in] target_rel URL-derived relative destination.
 * @param[in,out] tx Current transfer state.
 * @param[out] out_done Whether verified reuse completed the page.
 * @return ::k_ra8_ok when reusable or followed by a 200 response; otherwise failure.
 * @retval k_ra8_ok The operation completed.
 * @retval other Validation, capacity, network, or storage failed.
 * @pre Every pointer is non-NULL.
 * @pre Supplied capacities cover their referenced bounded buffers.
 * @post A 304 creates no filesystem transaction.
 * @post Any failed unconditional retry is recorded exactly once.
 * @note The function performs no dynamic allocation and retains no caller pointer.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_mdl_fetch_resolve_not_modified(mdl_fetch_ctx_t* ctx,
                                                                      const char*      url,
                                                                      const char*      target_abs,
                                                                      const char*      target_rel,
                                                                      mdl_page_transfer_t* tx,
                                                                      bool*                out_done)
{
  *out_done = false;
  if (tx->resp.status != (long)k_http_not_modified) {
    return k_ra8_ok;
  }
  ra8_err_t cleanup = priv_mdl_fetch_body_abort(&tx->body);
  if (cleanup != k_ra8_ok) {
    return cleanup;
  }
  if ((tx->held != nullptr) &&
      internal_mdl_fetch_try_reuse(ctx, mdl_hash_str(url), target_abs, target_rel)) {
    const time_t observed = time(nullptr);
    if (!mdl_state_note_page_response(ctx->state,
                                      mdl_hash_str(url),
                                      (observed < (time_t)0) ? 0 : (int64_t)observed,
                                      (uint16_t)k_http_not_modified)) {
      return k_ra8_err_invalid_state;
    }
    *out_done = true;
    return k_ra8_ok;
  }
  tx->req.if_none_match     = nullptr;
  tx->req.if_modified_since = nullptr;
  tx->resp                  = (mdl_net_resp_t){};
  tx->got                   = 0U;
  mdl_net_body_sink_t sink  = priv_mdl_fetch_body_sink(&tx->body);
  const ra8_err_t     rc    = priv_mdl_fetch_with_retry(ctx,
                                                        tx->host,
                                                        url,
                                                        &tx->req,
                                                        &sink,
                                                        tx->jmin,
                                                        tx->jmax,
                                                        &tx->resp,
                                                        &tx->got);
  if ((rc == k_ra8_ok) && (tx->resp.status != (long)k_http_not_modified)) {
    return k_ra8_ok;
  }
  cleanup = priv_mdl_fetch_body_abort(&tx->body);
  const ra8_err_t failure =
    (cleanup != k_ra8_ok) ? cleanup : ((rc != k_ra8_ok) ? rc : k_ra8_err_validation_failed);
  priv_mdl_fetch_record_fail(ctx, url, tx->resp.status, failure);
  return failure;
}

/**
 * @brief Validate and atomically publish a nonempty staged page.
 * @details Performs publish page under the injected network, governor, and storage contracts; dependency failures are propagated before incomplete bytes are published.
 * @param[in,out] ctx Fetch dependencies and persistent state.
 * @param[in] url Absolute page URL.
 * @param[in] target_abs URL-derived absolute destination.
 * @param[in,out] tx Completed non-304 transfer state.
 * @param[out] out_bytes Optional published byte count.
 * @return Publication or validation result.
 * @retval k_ra8_ok The operation completed.
 * @retval other Validation, capacity, network, or storage failed.
 * @pre Every required pointer is non-NULL.
 * @pre Supplied capacities cover their referenced bounded buffers.
 * @post Success records the magic-derived path and content hash.
 * @post Pre-commit failures leave any existing destination untouched.
 * @note The function performs no dynamic allocation and retains no caller pointer.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_mdl_fetch_publish_page(mdl_fetch_ctx_t*     ctx,
                                                              const char*          url,
                                                              const char*          target_abs,
                                                              mdl_page_transfer_t* tx,
                                                              size_t*              out_bytes)
{
  ra8_err_t error = priv_mdl_fetch_body_prepare(&tx->body);
  if (error != k_ra8_ok) {
    const ra8_err_t aborted = priv_mdl_fetch_body_abort(&tx->body);
    error                   = (aborted == k_ra8_ok) ? error : aborted;
    priv_mdl_fetch_record_fail(ctx, url, tx->resp.status, error);
    return error;
  }
  const char* etag =
    (tx->resp.etag[0] != '\0') ? tx->resp.etag : ((tx->held != nullptr) ? tx->held->etag : "");
  const char* modified = (tx->resp.last_modified[0] != '\0')
                           ? tx->resp.last_modified
                           : ((tx->held != nullptr) ? tx->held->last_modified : "");
  if ((ctx->state->page_rec_count >= (uint32_t)k_mdl_max_page_recs) && (tx->recorded == nullptr)) {
    error = priv_mdl_fetch_body_abort(&tx->body);
    error = (error == k_ra8_ok) ? k_ra8_err_no_mem : error;
    priv_mdl_fetch_record_fail(ctx, url, tx->resp.status, error);
    return error;
  }
  if ((tx->resp.status < (long)k_http_status_min) || (tx->resp.status > (long)k_http_status_max)) {
    error = priv_mdl_fetch_body_abort(&tx->body);
    error = (error == k_ra8_ok) ? k_ra8_err_protocol_error : error;
    priv_mdl_fetch_record_fail(ctx, url, tx->resp.status, error);
    return error;
  }
  const uint64_t content  = tx->body.writer.hash;
  const time_t   observed = time(nullptr);
  error                   = priv_mdl_fetch_body_commit(&tx->body);
  if (error != k_ra8_ok) {
    priv_mdl_fetch_record_fail(ctx, url, tx->resp.status, error);
    return error;
  }
  if (!internal_mdl_fetch_discard_stale_page(ctx->storage, target_abs, tx->body.actual_abs) ||
      !mdl_state_add_page(ctx->state,
                          mdl_hash_str(url),
                          content,
                          tx->body.actual_rel,
                          etag,
                          modified,
                          (observed < (time_t)0) ? 0 : (int64_t)observed,
                          (uint16_t)tx->resp.status)) {
    priv_mdl_fetch_record_fail(ctx, url, tx->resp.status, k_ra8_fail);
    return k_ra8_fail;
  }
  if (out_bytes != nullptr) {
    *out_bytes = tx->got;
  }
  return k_ra8_ok;
}

/**
 * @brief Perform the do fetch page step.
 * @details Performs do fetch page under the injected network, governor, and storage contracts; dependency failures are propagated before incomplete bytes are published.
 * @param[in,out] ctx Borrowed operation context.
 * @param[in] chapter_url Canonical chapter URL.
 * @param[in] url Canonical input URL.
 * @param[in] target_abs Absolute destination path.
 * @param[in] target_rel Library-relative destination path.
 * @param[out] out_bytes Receives the transferred byte count.
 * @param[out] out_resp Receives normalized response metadata.
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
RA8_INTERNAL static ra8_err_t internal_mdl_fetch_do_fetch_page(mdl_fetch_ctx_t* ctx,
                                                               const char*      chapter_url,
                                                               const char*      url,
                                                               const char*      target_abs,
                                                               const char*      target_rel,
                                                               size_t*          out_bytes,
                                                               mdl_net_resp_t*  out_resp)
{
  if (out_bytes != nullptr) {
    *out_bytes = 0U;
  }
  if (out_resp != nullptr) {
    *out_resp = (mdl_net_resp_t){};
  }
  mdl_page_transfer_t tx = {};
  ra8_err_t           rc =
    internal_mdl_fetch_prepare_page(ctx, chapter_url, url, target_abs, target_rel, &tx);
  if (rc != k_ra8_ok) {
    if (out_resp != nullptr) {
      *out_resp = tx.resp;
    }
    return rc;
  }
  bool reused = false;
  rc = internal_mdl_fetch_resolve_not_modified(ctx, url, target_abs, target_rel, &tx, &reused);
  if (out_resp != nullptr) {
    *out_resp = tx.resp;
  }
  if ((rc != k_ra8_ok) || reused) {
    return rc;
  }
  return internal_mdl_fetch_publish_page(ctx, url, target_abs, &tx, out_bytes);
}

/**
 * @brief Reuse or fetch page @p idx and checkpoint its outcome.
 * @details Composes absolute and relative paths, tries verified hash reuse,
 *          otherwise performs a governed conditional fetch, records the page,
 *          updates statistics, and checkpoints only the completed outcome.
 * @param[in,out] ctx Fetch dependencies, persistent state, and page list.
 * @param[in] chapter_url Canonical chapter URL used for diagnostics.
 * @param[in] dest_abs Canonical destination directory.
 * @param[in] dest_rel Library-relative destination directory.
 * @param[in] page_no One-based page number.
 * @param[in] idx Zero-based index in the extracted page URL list.
 * @param[in,out] rec Chapter record receiving completed-page progress.
 * @param[in,out] stats Run counters receiving bytes, reuse, or failure.
 * @param[out] out Receives the bounded page outcome.
 * @return Fetch, validation, or checkpoint status.
 * @retval k_ra8_ok The page was verified in place, reused, or published and checkpointed.
 * @retval other Path construction, transfer, validation, or checkpointing failed.
 * @pre Every pointer is non-NULL and @p idx is less than the extracted page count.
 * @pre The storage and network dependencies in @p ctx are exclusively owned.
 * @post Success records one verified page and one successful checkpoint.
 * @post Failure never records an incomplete page as complete.
 * @note Existing destinations are protected by the injected transaction contract.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_mdl_fetch_one_page(mdl_fetch_ctx_t*    ctx,
                                                          const char*         chapter_url,
                                                          const char*         dest_abs,
                                                          const char*         dest_rel,
                                                          size_t              page_no,
                                                          size_t              idx,
                                                          mdl_chapter_rec_t*  rec,
                                                          mdl_fetch_stats_t*  stats,
                                                          mdl_page_outcome_t* out)
{
  const char* url = ctx->images->urls[idx];
  char        leaf[k_fetch_leaf_bytes];
  if (!internal_mdl_fetch_page_leaf(page_no, url, leaf, sizeof(leaf))) {
    return k_ra8_fail;
  }
  char      target_abs[PATH_MAX];
  char      target_rel[k_mdl_relpath_max];
  const int an = snprintf(target_abs, sizeof(target_abs), "%s/%s", dest_abs, leaf);
  const int rn = snprintf(target_rel, sizeof(target_rel), "%s/%s", dest_rel, leaf);
  if ((an < 0) || ((size_t)an >= sizeof(target_abs)) || (rn < 0) ||
      ((size_t)rn >= sizeof(target_rel))) {
    return k_ra8_fail;
  }
  const uint64_t        url_hash = mdl_hash_str(url);
  const mdl_page_rec_t* held     = mdl_state_find_page(ctx->state, url_hash);
  const bool            has_validator =
    (held != nullptr) && ((held->etag[0] != '\0') || (held->last_modified[0] != '\0'));
  if (!ctx->refetch && !has_validator &&
      internal_mdl_fetch_try_reuse(ctx, url_hash, target_abs, target_rel)) {
    stats->pages_reused += 1U;
    *out = (mdl_page_outcome_t){.bytes = 0U, .elapsed_ms = 0U, .reused = true};
  } else {
    const int64_t   t0   = internal_mdl_fetch_mono_ms(ctx);
    size_t          got  = 0U;
    mdl_net_resp_t  resp = {};
    const ra8_err_t rc =
      internal_mdl_fetch_do_fetch_page(ctx, chapter_url, url, target_abs, target_rel, &got, &resp);
    if (rc != k_ra8_ok) {
      stats->pages_failed += 1U;
      return rc;
    }
    if (resp.status == (long)k_http_not_modified) {
      stats->pages_reused += 1U;
      *out = (mdl_page_outcome_t){.bytes      = 0U,
                                  .elapsed_ms = (uint32_t)(internal_mdl_fetch_mono_ms(ctx) - t0),
                                  .reused     = true};
    } else {
      stats->pages_fetched += 1U;
      *out = (mdl_page_outcome_t){.bytes      = (uint64_t)got,
                                  .elapsed_ms = (uint32_t)(internal_mdl_fetch_mono_ms(ctx) - t0),
                                  .reused     = false};
    }
  }
  rec->pages_done = (uint16_t)(idx + 1U);
  return priv_mdl_fetch_checkpoint(ctx);
}

/** @brief Fetch every page of one chapter; fail (partial) on the first bad
 * page. */
RA8_PRIV ra8_err_t priv_mdl_fetch_chapter_pages(mdl_fetch_ctx_t*     ctx,
                                                const char*          chapter_url,
                                                const char*          dest_abs,
                                                const char*          dest_rel,
                                                size_t               base,
                                                mdl_chapter_rec_t*   rec,
                                                mdl_fetch_stats_t*   stats,
                                                const mdl_run_pos_t* pos)
{
  for (size_t i = 0U; i < ctx->images->count; ++i) {
    const size_t       page_no = base + i + 1U;
    mdl_page_outcome_t out     = {};
    const ra8_err_t    rc      = internal_mdl_fetch_one_page(ctx,
                                                             chapter_url,
                                                             dest_abs,
                                                             dest_rel,
                                                             page_no,
                                                             i,
                                                             rec,
                                                             stats,
                                                             &out);
    if (rc != k_ra8_ok) {
      return rc;
    }
    const ra8_err_t progress_error = internal_mdl_fetch_emit_progress(ctx, pos, i + 1U, &out);
    if (progress_error != k_ra8_ok) {
      ctx->progress_error = progress_error;
      return progress_error;
    }
  }
  return k_ra8_ok;
}

/** @brief One governed chapter-HTML fetch: pace, GET, feed the outcome back. */
