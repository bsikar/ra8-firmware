/**
 * @file mdl_report.h
 * @brief Human-facing progress and failure reporting for the media_dl CLI.
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
 * @return Nothing.
 *
 * @pre @p ev, when non-NULL, was populated by ::mdl_fetch_run.
 * @pre stdout is open.
 * @post One line is written to stdout for a non-NULL @p ev.
 * @post No state is modified.
 *
 * @note Thread-safe: writes only to stdout.
 * @see mdl_progress_fn
 * @since 0.1.0
 */
void mdl_report_progress(void* ctx, const mdl_fetch_progress_t* ev);

/**
 * @brief End-of-run summary: list every failure with its URL and reason.
 *
 * @details
 * Prints one line per recorded failure (URL plus a human-readable reason
 * derived from the classified error and HTTP status), preceded by the total
 * count, so the information survives after the per-page diagnostics have
 * scrolled away. A run with no failures prints nothing.
 *
 * @param[in] log The run's failure log (never NULL).
 *
 * @return Nothing.
 *
 * @pre @p log is non-NULL and was cleared before the run it summarises.
 * @pre stderr is open.
 * @post Nothing is written when `log->total == 0`.
 * @post No state is modified.
 *
 * @note Thread-safe: writes only to stderr.
 * @see mdl_fetch_faillog_t
 * @since 0.1.0
 */
void mdl_report_failures(const mdl_fetch_faillog_t* log);
