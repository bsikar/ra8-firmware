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
 * name, and no code outside the media_dl tool ever does.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "mdl_fetch.h"
#include "ra8_attributes.h"
#include "ra8_err.h"

/** @brief Buffer size the ::mdl_fetch_reason renderer never overruns. */
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
 * classified status (::mdl_net_curl_classify), never on a collapsed generic
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
 * k_ra8_fail)` (3 conditions). Cited as tools/media_dl/src/mdl_fetch.c@mdl_fetch_is_retryable.
 * - Vector 1: rc=k_ra8_err_not_found -> false (control: all three false)
 * - Vector 2: rc=k_ra8_err_busy      -> true  (varies condition 1)
 * - Vector 3: rc=k_ra8_err_timeout   -> true  (varies condition 2)
 * - Vector 4: rc=k_ra8_fail          -> true  (varies condition 3)
 * Vectors 1+2 prove condition 1's independent influence, 1+3 condition 2's,
 * 1+4 condition 3's. N+1 = 4 vectors for N=3 conditions: minimal MC/DC.
 *
 * @since 0.1.0
 */
RA8_PRIV bool mdl_fetch_is_retryable(ra8_err_t rc);

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
 * conditions). Cited as tools/media_dl/src/mdl_fetch.c@mdl_fetch_run_incomplete.
 * - Vector 1: chapters_failed=0, pages_failed=0 -> false (both false)
 * - Vector 2: chapters_failed=1, pages_failed=0 -> true  (varies chapters)
 * - Vector 3: chapters_failed=0, pages_failed=1 -> true  (varies pages)
 * Vectors 1+2 prove the chapter condition's influence, 1+3 the page
 * condition's. N+1 = 3 vectors for N=2 conditions: minimal MC/DC.
 *
 * @since 0.1.0
 */
RA8_PRIV bool mdl_fetch_run_incomplete(const mdl_fetch_stats_t* stats);

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
RA8_PRIV void mdl_fetch_reason(ra8_err_t err, long status, char* buf, size_t cap);
