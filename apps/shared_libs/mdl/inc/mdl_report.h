/**
 * @file mdl_report.h
 * @brief Human-facing progress and failure reporting for the mdl CLI.
 *
 * @details
 * The download loop (`mdl_fetch`) is deliberately silent and testable: it emits
 * progress through an injected ::mdl_progress_fn and records failures into a
 * ::mdl_fetch_faillog_t, never printing on its own. This module is the concrete
 * presenter the CLI wires in -- a redirect-safe per-page progress line and an
 * end-of-run summary that names every lost page with a human-readable reason,
 * so a long run is legible live and its failures survive the scrollback. Kept
 * out of `main.c` so the entry point stays a thin dispatcher.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include "mdl_fetch.h"

/**
 * @brief Per-page progress sink: print one redirect-safe line per page.
 *
 * @details
 * Signature-compatible with ::mdl_progress_fn so it can be wired straight into
 * ::mdl_fetch_ctx_t::progress_fn. Prints the run/chapter/page position and,
 * for a transferred page, its size and rate; a reused page is marked as such.
 * Uses no terminal control sequences, so the output stays readable when
 * redirected to a file or a pipe.
 *
 * @param[in] ctx Unused progress context (kept for the ::mdl_progress_fn ABI).
 * @param[in] ev  The just-completed page's progress event, or NULL (no-op).
 *
 * @return Stream output status.
 * @retval k_ra8_ok The event was absent or its complete line was accepted.
 * @retval other The output stream rejected a fragment.
 *
 * @pre @p ev, when non-NULL, was populated by ::mdl_fetch_run.
 * @pre @p ctx points to a bound ::ra8_io_stream_t when @p ev is non-NULL.
 * @post One line is written to the injected stream for a non-NULL @p ev.
 * @post No state is modified.
 *
 * @note Thread safety follows the injected stream.
 * @see mdl_progress_fn
 * @since 0.1.0
 */
ra8_err_t mdl_report_progress(void* ctx, const mdl_fetch_progress_t* ev);

/**
 * @brief Per-page progress bar sink: render an in-place terminal progress bar.
 *
 * @details Rewrites one fixed-width bar with carriage returns and appends a
 *          newline when the final page completes. Reused pages omit rate data.
 *
 * @param[in] ctx Unused progress context (kept for the ::mdl_progress_fn ABI).
 * @param[in] ev  The just-completed page's progress event, or NULL (no-op).
 *
 * @return Stream output status.
 * @retval k_ra8_ok The event was absent or its complete update was accepted.
 * @retval other The output stream rejected a fragment or flush.
 * @pre @p ev, when non-NULL, contains a coherent page index and total.
 * @pre @p ctx points to a bound ::ra8_io_stream_t when @p ev is non-NULL.
 * @post A non-NULL event updates the injected progress stream.
 * @post A terminal event leaves that stream positioned on a new line.
 * @note Thread safety follows the injected stream.
 * @since 0.1.0
 */
ra8_err_t mdl_report_progress_bar(void* ctx, const mdl_fetch_progress_t* ev);

/**
 * @brief End-of-run summary: list every failure with its URL and reason.
 *
 * @details
 * Prints one line per recorded failure (URL plus a human-readable reason
 * derived from the classified error and HTTP status), preceded by the total
 * count, so the information survives after the per-page diagnostics have
 * scrolled away. A run with no failures prints nothing.
 *
 * @param[in,out] diagnostic Borrowed stream receiving the failure summary.
 * @param[in] log The run's failure log (never NULL).
 *
 * @return Stream output status.
 * @retval k_ra8_ok No failures existed or the complete summary was accepted.
 * @retval other The diagnostic stream rejected a fragment.
 *
 * @pre @p log is non-NULL and was cleared before the run it summarises.
 * @pre @p diagnostic is bound when @p log contains failures.
 * @post Nothing is written when `log->total == 0`.
 * @post No state is modified.
 *
 * @note Thread safety follows the injected stream.
 * @see mdl_fetch_faillog_t
 * @since 0.1.0
 */
ra8_err_t mdl_report_failures(ra8_io_stream_t* diagnostic, const mdl_fetch_faillog_t* log);
