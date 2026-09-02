/**
 * @file mdl_fetch_internal.h
 * @brief Module-private fetch-loop decisions promoted for host unit tests and
 *        the CLI's end-of-run reporting.
 *
 * @details
 * These are pure decisions with no network dependency, factored out of the
 * download loop so the host tests can drive each branch directly (per the "Test
 * access to internal symbols" rule in `CLAUDE.md`) and so `main.c` can reuse the
 * same retryability / incompleteness / reason logic the loop uses -- rather than
 * re-deriving it and drifting. Nothing here is part of the orchestrator's
 * public API in `mdl_fetch.h`; production callers inside the tool reach them by
 * name, and no code outside the mdl tool ever does.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "mdl_fetch.h"
#include "mdl_net.h"
#include "ra8_attributes.h"
#include "ra8_err.h"

/** @brief Caller-owned governor parameters for one cached buffer fetch. */
typedef struct mdl_fetch_cache_request mdl_fetch_cache_request_t;

/**
 * @struct mdl_fetch_cache_request
 * @brief Governed retry context for one cache buffer callback.
 * @details Holds only the active fetch dependency, host key, and delay bounds
 *          needed to adapt ::mdl_net_get_buf to ::mdl_cache_fetch_fn.
 * @since 0.1.0
 */
struct mdl_fetch_cache_request {
  mdl_fetch_ctx_t* ctx;                      /**< Active fetch dependencies. */
  char             host[k_mdl_gov_host_max]; /**< Governor host key.         */
  uint32_t         jmin;                     /**< Minimum request delay.     */
  uint32_t         jmax;                     /**< Maximum request delay.     */
};

/**
 * @brief Fetch one cache buffer through governor and bounded retry policy.
 * @details Acquires and observes the host governor around each network attempt,
 *          retrying only results accepted by ::priv_mdl_fetch_is_retryable.
 * @param[in,out] context ::mdl_fetch_cache_request_t state.
 * @param[in] url Exact request URL.
 * @param[in] request Conditional request metadata.
 * @param[out] buffer Bounded body destination.
 * @param[in] capacity Writable destination capacity.
 * @param[out] out_length Exact received bytes.
 * @param[out] response Finished response metadata.
 * @return Canonical governed network status.
 * @retval k_ra8_ok A complete response is available.
 * @retval other Governor acquisition or every bounded network attempt failed.
 * @pre Every pointer is non-NULL and @p context is initialized.
 * @pre @p buffer spans @p capacity writable bytes.
 * @post Every attempted request is observed and released exactly once.
 * @post At most the configured retry count is attempted.
 * @note Conforms directly to ::mdl_cache_fetch_fn.
 * @since 0.1.0
 */
RA8_PRIV ra8_err_t priv_mdl_fetch_cache_get_buf(void*                context,
                                                const char*          url,
                                                const mdl_net_req_t* request,
                                                char*                buffer,
                                                size_t               capacity,
                                                size_t*              out_length,
                                                mdl_net_resp_t*      response);

/**
 * @struct mdl_run_pos_t
 * @brief Stable chapter position carried into per-page progress events.
 * @since 0.1.0
 */
typedef struct {
  size_t      chapter_index; /**< 1-based chapter position in the run. */
  size_t      chapter_total; /**< Total chapters in the run.           */
  const char* chapter_id;    /**< Stable chapter identifier.           */
} mdl_run_pos_t;

/** @brief Buffer size the ::priv_mdl_fetch_reason renderer never overruns. */
typedef enum : uint16_t {
  k_mdl_reason_max = 96, /**< Failure-reason string buffer bytes. */
} mdl_fetch_reason_size_t;

/**
 * @brief Whether a failed transfer result is worth retrying.
 *
 * @details
 * The retry classifier the bounded per-request loop consults. A transport
 * error (::k_ra8_fail), a timeout (::k_ra8_err_timeout) and a throttle
 * (::k_ra8_err_busy, i.e. HTTP 429/503) are transient and retried; every other
 * outcome -- success, an absent resource (::k_ra8_err_not_found / 404), an
 * over-cap body (::k_ra8_err_no_mem), a refused argument or a governor decline
 * -- is terminal and breaks the loop. Retryability is decided on the real
 * classified status (::priv_mdl_net_curl_classify), never on a collapsed generic
 * failure, so a 404 is never retried while a 503 is.
 *
 * @param[in] rc The transfer's classified ::ra8_err_t result.
 *
 * @return Whether the request should be retried.
 * @retval true  @p rc is ::k_ra8_err_busy, ::k_ra8_err_timeout or ::k_ra8_fail.
 * @retval false Any other value (success or a permanent error).
 *
 * @pre @p rc is a value from the ::ra8_err_t contract.
 * @pre The caller bounds the retry count independently (this makes no loop).
 * @post No state is modified.
 * @post ::k_ra8_ok always yields false, so a success ends the loop.
 *
 * @note Thread-safe: depends only on its argument.
 *
 * @par MC/DC:
 * Decision: `(rc == k_ra8_err_busy) || (rc == k_ra8_err_timeout) || (rc ==
 * k_ra8_fail)` (3 conditions). Cited as apps/shared_libs/mdl/src/mdl_fetch.c@priv_mdl_fetch_is_retryable.
 * - Vector 1: rc=k_ra8_err_not_found -> false (control: all three false)
 * - Vector 2: rc=k_ra8_err_busy      -> true  (varies condition 1)
 * - Vector 3: rc=k_ra8_err_timeout   -> true  (varies condition 2)
 * - Vector 4: rc=k_ra8_fail          -> true  (varies condition 3)
 * Vectors 1+2 prove condition 1's independent influence, 1+3 condition 2's,
 * 1+4 condition 3's. N+1 = 4 vectors for N=3 conditions: minimal MC/DC.
 *
 * @since 0.1.0
 */
RA8_PRIV bool priv_mdl_fetch_is_retryable(ra8_err_t rc);

/**
 * @brief Whether a finished run left anything unfetched (chapter or page).
 *
 * @details
 * The single honesty predicate the CLI's export gate reads: a run is incomplete
 * when any chapter was left partial OR any individual page failed. It exists so
 * the "do not package an incomplete archive" decision is one tested function
 * rather than a compound test re-derived at each export site.
 *
 * @param[in] stats The run tallies (may be NULL).
 *
 * @return Whether the run has an unfetched chapter or page.
 * @retval true  `chapters_failed > 0` or `pages_failed > 0`.
 * @retval false A NULL @p stats, or a run in which nothing failed.
 *
 * @pre @p stats, when non-NULL, was filled by ::mdl_fetch_run.
 * @pre The caller treats NULL as "cannot prove incomplete" (false).
 * @post No state is modified.
 * @post A fully clean run yields false.
 *
 * @note Thread-safe: reads only its argument.
 *
 * @par MC/DC:
 * Decision: `(stats->chapters_failed > 0) || (stats->pages_failed > 0)` (2
 * conditions). Cited as apps/shared_libs/mdl/src/mdl_fetch.c@priv_mdl_fetch_run_incomplete.
 * - Vector 1: chapters_failed=0, pages_failed=0 -> false (both false)
 * - Vector 2: chapters_failed=1, pages_failed=0 -> true  (varies chapters)
 * - Vector 3: chapters_failed=0, pages_failed=1 -> true  (varies pages)
 * Vectors 1+2 prove the chapter condition's influence, 1+3 the page
 * condition's. N+1 = 3 vectors for N=2 conditions: minimal MC/DC.
 *
 * @since 0.1.0
 */
RA8_PRIV bool priv_mdl_fetch_run_incomplete(const mdl_fetch_stats_t* stats);

/**
 * @brief Render a human-readable reason for a transfer result and HTTP status.
 *
 * @details
 * Maps the classified ::ra8_err_t (plus the observed HTTP status, where one
 * applies) to a short prose reason, so a failure is reported as "rate limited
 * (HTTP 503)" rather than a bare `err 0x109`. The HTTP status is appended in
 * parentheses when it is non-zero. Used by the CLI for both the per-failure
 * summary and any inline error line.
 *
 * @param[in]  err    The classified transfer result.
 * @param[in]  status HTTP status observed, or 0 when none applies.
 * @param[out] buf    Destination buffer for the NUL-terminated reason.
 * @param[in]  cap    Capacity of @p buf in bytes.
 *
 * @return Nothing.
 *
 * @pre @p buf is non-NULL and @p cap > 0 for output to be written.
 * @pre @p err comes from a finished transfer.
 * @post @p buf is NUL-terminated when @p cap > 0.
 * @post A NULL @p buf or zero @p cap is a tolerated no-op.
 *
 * @note Thread-safe: writes only the caller-provided buffer.
 * @since 0.1.0
 */
RA8_PRIV void priv_mdl_fetch_reason(ra8_err_t err, long status, char* buf, size_t cap);

/**
 * @brief Record one classified failure in the caller's bounded log.
 * @details Performs record fail under the injected network, governor, and storage contracts; dependency failures are propagated before incomplete bytes are published.
 * @param[in] ctx Fetch context carrying the optional log.
 * @param[in] url Failed URL or path.
 * @param[in] status Observed HTTP status, or zero.
 * @param[in] err Classified failure.
 * @pre @p ctx is non-NULL.
 * @pre Supplied capacities cover their referenced bounded buffers.
 * @post A configured log tallies the failure without exceeding capacity.
 * @post Result status and outputs describe one completed synchronous attempt.
 * @note The function performs no dynamic allocation and retains no caller pointer.
 * @since 0.1.0
 */
RA8_PRIV void
priv_mdl_fetch_record_fail(const mdl_fetch_ctx_t* ctx, const char* url, long status, ra8_err_t err);

/**
 * @brief Persist the current fetch state when a checkpoint path is configured.
 * @details Performs checkpoint under the injected network, governor, and storage contracts; dependency failures are propagated before incomplete bytes are published.
 * @param[in] ctx Fetch context and state.
 * @return State-save result, or ::k_ra8_ok when checkpointing is disabled.
 * @retval k_ra8_ok The operation completed.
 * @retval other Validation, capacity, network, or storage failed.
 * @pre Required pointer arguments remain valid for the call duration.
 * @pre Supplied capacities cover their referenced bounded buffers.
 * @post No ownership of caller-provided storage is transferred.
 * @post Result status and outputs describe one completed synchronous attempt.
 * @note The function performs no dynamic allocation and retains no caller pointer.
 * @since 0.1.0
 */
RA8_PRIV ra8_err_t priv_mdl_fetch_checkpoint(const mdl_fetch_ctx_t* ctx);

/**
 * @brief Perform one bounded governed file transfer with retry classification.
 * @details Performs with retry under the injected network, governor, and storage contracts; dependency failures are propagated before incomplete bytes are published.
 * @param[in,out] ctx Fetch dependencies.
 * @param[in] host Governor host key.
 * @param[in] url Absolute source URL.
 * @param[in] req Request headers and timeout.
 * @param[in,out] sink Reset/write body destination.
 * @param[in] jmin Minimum delay in milliseconds.
 * @param[in] jmax Maximum delay in milliseconds.
 * @param[out] out_resp Final response metadata.
 * @param[out] out_bytes Final response byte count.
 * @return Final classified transfer result.
 * @retval k_ra8_ok The operation completed.
 * @retval other Validation, capacity, network, or storage failed.
 * @pre Required pointer arguments remain valid for the call duration.
 * @pre Supplied capacities cover their referenced bounded buffers.
 * @post No ownership of caller-provided storage is transferred.
 * @post Result status and outputs describe one completed synchronous attempt.
 * @note The function performs no dynamic allocation and retains no caller pointer.
 * @since 0.1.0
 */
RA8_PRIV ra8_err_t priv_mdl_fetch_with_retry(mdl_fetch_ctx_t*     ctx,
                                             const char*          host,
                                             const char*          url,
                                             const mdl_net_req_t* req,
                                             mdl_net_body_sink_t* sink,
                                             uint32_t             jmin,
                                             uint32_t             jmax,
                                             mdl_net_resp_t*      out_resp,
                                             size_t*              out_bytes);

/**
 * @brief Fetch and checkpoint every extracted page in one chapter.
 * @details Performs chapter pages under the injected network, governor, and storage contracts; dependency failures are propagated before incomplete bytes are published.
 * @param[in,out] ctx Fetch dependencies and extracted image list.
 * @param[in] chapter_url Chapter URL used as Referer.
 * @param[in] dest_abs Absolute output directory.
 * @param[in] dest_rel Series-relative output directory.
 * @param[in] base Page-number base for combined layout.
 * @param[in,out] rec Persistent chapter record.
 * @param[in,out] stats Run counters.
 * @param[in] pos Chapter progress coordinates.
 * @return ::k_ra8_ok after every page is durable; otherwise first failure.
 * @retval k_ra8_ok The operation completed.
 * @retval other Validation, capacity, network, or storage failed.
 * @pre Required pointer arguments remain valid for the call duration.
 * @pre Supplied capacities cover their referenced bounded buffers.
 * @post No ownership of caller-provided storage is transferred.
 * @post Result status and outputs describe one completed synchronous attempt.
 * @note The function performs no dynamic allocation and retains no caller pointer.
 * @since 0.1.0
 */
RA8_PRIV ra8_err_t priv_mdl_fetch_chapter_pages(mdl_fetch_ctx_t*     ctx,
                                                const char*          chapter_url,
                                                const char*          dest_abs,
                                                const char*          dest_rel,
                                                size_t               base,
                                                mdl_chapter_rec_t*   rec,
                                                mdl_fetch_stats_t*   stats,
                                                const mdl_run_pos_t* pos);
