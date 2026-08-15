/**
 * @file mdl_report.c
 * @brief Implementation of the media_dl progress + failure presenter.
 * @details Formats bounded status text and translates downloader progress and
 *          terminal results into the configured command-line presentation.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#include "mdl_report.h"

#include <stdint.h>
#include <stdio.h>

#include "mdl_fetch.h"
#include "mdl_fetch_internal.h"
#include "mdl_stream_internal.h"
#include "ra8_attributes.h"

/** @brief Human-readable size/rate scaling and buffer sizes. */
typedef enum : uint32_t {
  k_bytes_per_kib      = 1024U,         /**< Bytes in a kibibyte.            */
  k_bytes_per_mib      = 1024U * 1024U, /**< Bytes in a mebibyte.            */
  k_ms_per_sec         = 1000U,         /**< Milliseconds per second (rate). */
  k_human_bytes        = 32U,           /**< Size/rate string buffer bytes.  */
  k_progress_bar_width = 20U,           /**< Rendered progress-bar cells.    */
  k_percent_scale      = 100U,          /**< Whole-percent conversion scale. */
  k_percent_two_digits = 10U,           /**< First two-digit percentage.     */
} mdl_report_scale_t;

/**
 * @brief Format a byte count as a compact human-readable string
 * @details Chooses bytes, KiB-style KB, or MiB-style MB and emits one decimal
 *          place for scaled values into the caller's bounded buffer.
 * @param[in] bytes Byte count to render.
 * @param[out] buf Destination text buffer.
 * @param[in] cap Writable capacity of @p buf.
 * @pre @p buf points to @p cap writable bytes.
 * @pre @p cap is large enough for the documented compact representation.
 * @post @p buf contains a NUL-terminated size string.
 * @post No state other than @p buf is modified.
 * @note Thread-safe across distinct destination buffers.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_fmt_size(uint64_t bytes, char* buf, size_t cap)
{
  const double b = (double)bytes;
  if (bytes >= (uint64_t)k_bytes_per_mib) {
    (void)snprintf(buf, cap, "%.1f MB", b / (double)k_bytes_per_mib);
    return;
  }
  if (bytes >= (uint64_t)k_bytes_per_kib) {
    (void)snprintf(buf, cap, "%.1f KB", b / (double)k_bytes_per_kib);
    return;
  }
  (void)snprintf(buf, cap, "%llu B", (unsigned long long)bytes);
}

/**
 * @brief Format a byte transfer rate as a compact string
 * @details Converts elapsed milliseconds to bytes per second, scales to KB/s
 *          or MB/s, and emits a placeholder when elapsed time is zero.
 * @param[in] bytes Bytes transferred during the interval.
 * @param[in] elapsed_ms Interval duration in milliseconds.
 * @param[out] buf Destination text buffer.
 * @param[in] cap Writable capacity of @p buf.
 * @pre @p buf points to @p cap writable bytes.
 * @pre @p cap is large enough for the documented compact representation.
 * @post @p buf contains a NUL-terminated rate or placeholder string.
 * @post No state other than @p buf is modified.
 * @note Thread-safe across distinct destination buffers.
 * @since 0.1.0
 */
RA8_INTERNAL static void
internal_fmt_rate(uint64_t bytes, uint32_t elapsed_ms, char* buf, size_t cap)
{
  if (elapsed_ms == 0U) {
    (void)snprintf(buf, cap, "-- KB/s");
    return;
  }
  const double bps = ((double)bytes * (double)k_ms_per_sec) / (double)elapsed_ms;
  if (bps >= (double)k_bytes_per_mib) {
    (void)snprintf(buf, cap, "%.1f MB/s", bps / (double)k_bytes_per_mib);
    return;
  }
  (void)snprintf(buf, cap, "%.1f KB/s", bps / (double)k_bytes_per_kib);
}

/** @brief Append the common chapter and page coordinates for one event.
 * @details Writes coordinates in stable field order through the injected stream.
 *          The first sink error is returned unchanged and prevents later fields.
 * @param[in,out] error Error accumulator or error value.
 * @param[out] output Destination report stream.
 * @param[in] ev Structured report event.
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
internal_report_position(ra8_err_t error, ra8_io_stream_t* output, const mdl_fetch_progress_t* ev)
{
  error = priv_mdl_stream_text(error, output, "[ch ");
  error = priv_mdl_stream_u64(error, output, ev->chapter_index);
  error = priv_mdl_stream_text(error, output, "/");
  error = priv_mdl_stream_u64(error, output, ev->chapter_total);
  error = priv_mdl_stream_text(error, output, " ");
  error = priv_mdl_stream_text(error, output, ev->chapter_id);
  error = priv_mdl_stream_text(error, output, "] page ");
  error = priv_mdl_stream_u64(error, output, ev->page_index);
  error = priv_mdl_stream_text(error, output, "/");
  return priv_mdl_stream_u64(error, output, ev->page_total);
}

ra8_err_t mdl_report_progress(void* ctx, const mdl_fetch_progress_t* ev)
{
  if (ev == nullptr) {
    return k_ra8_ok;
  }
  ra8_io_stream_t* output = (ra8_io_stream_t*)ctx;
  ra8_err_t        error  = priv_mdl_stream_text(k_ra8_ok, output, "  ");
  error                   = internal_report_position(error, output, ev);
  if (ev->reused) {
    return priv_mdl_stream_text(error, output, "  reused\n");
  }
  char size[k_human_bytes];
  char rate[k_human_bytes];
  internal_fmt_size(ev->page_bytes, size, sizeof(size));
  internal_fmt_rate(ev->page_bytes, ev->elapsed_ms, rate, sizeof(rate));
  error = priv_mdl_stream_text(error, output, "  ");
  error = priv_mdl_stream_text(error, output, size);
  error = priv_mdl_stream_text(error, output, " @ ");
  error = priv_mdl_stream_text(error, output, rate);
  return priv_mdl_stream_text(error, output, "\n");
}

ra8_err_t mdl_report_progress_bar(void* ctx, const mdl_fetch_progress_t* ev)
{
  if (ev == nullptr) {
    return k_ra8_ok;
  }
  ra8_io_stream_t* output    = (ra8_io_stream_t*)ctx;
  const size_t     bar_width = (size_t)k_progress_bar_width;
  size_t           filled    = 0U;
  if (ev->page_total > 0U) {
    filled = (ev->page_index * bar_width) / ev->page_total;
    if (filled > bar_width) {
      filled = bar_width;
    }
  }
  const uint32_t pct = (ev->page_total > 0U)
                         ? (uint32_t)((ev->page_index * (size_t)k_percent_scale) / ev->page_total)
                         : 0U;

  ra8_err_t error = priv_mdl_stream_text(k_ra8_ok, output, "\r  [");
  error           = priv_mdl_stream_repeat(error, output, '=', filled);
  error           = priv_mdl_stream_repeat(error, output, ' ', bar_width - filled);
  error           = priv_mdl_stream_text(error, output, "] ");
  error =
    priv_mdl_stream_repeat(error,
                           output,
                           ' ',
                           (pct < k_percent_two_digits) ? 2U : ((pct < k_percent_scale) ? 1U : 0U));
  error = priv_mdl_stream_u64(error, output, pct);
  error = priv_mdl_stream_text(error, output, "% ");
  error = internal_report_position(error, output, ev);
  if (ev->reused) {
    error = priv_mdl_stream_text(error, output, " (reused)");
  } else {
    char size[k_human_bytes];
    char rate[k_human_bytes];
    internal_fmt_size(ev->page_bytes, size, sizeof(size));
    internal_fmt_rate(ev->page_bytes, ev->elapsed_ms, rate, sizeof(rate));
    error = priv_mdl_stream_text(error, output, " (");
    error = priv_mdl_stream_text(error, output, size);
    error = priv_mdl_stream_text(error, output, " @ ");
    error = priv_mdl_stream_text(error, output, rate);
    error = priv_mdl_stream_text(error, output, ")");
  }
  if (ev->page_index >= ev->page_total) {
    error = priv_mdl_stream_text(error, output, "\n");
  }
  return priv_mdl_stream_flush(error, output);
}

ra8_err_t mdl_report_failures(ra8_io_stream_t* diagnostic, const mdl_fetch_faillog_t* log)
{
  if (log->total == 0U) {
    return k_ra8_ok;
  }
  ra8_err_t error = priv_mdl_stream_u64(k_ra8_ok, diagnostic, log->total);
  error           = priv_mdl_stream_text(error, diagnostic, " failure(s) this run:\n");
  for (size_t i = 0U; i < log->count; ++i) {
    char reason[k_mdl_reason_max];
    priv_mdl_fetch_reason(log->items[i].err, log->items[i].status, reason, sizeof(reason));
    error = priv_mdl_stream_text(error, diagnostic, "  FAILED ");
    error = priv_mdl_stream_text(error, diagnostic, log->items[i].url);
    error = priv_mdl_stream_text(error, diagnostic, " -- ");
    error = priv_mdl_stream_text(error, diagnostic, reason);
    error = priv_mdl_stream_text(error, diagnostic, "\n");
  }
  if (log->total > log->count) {
    error = priv_mdl_stream_text(error, diagnostic, "  ... and ");
    error = priv_mdl_stream_u64(error, diagnostic, log->total - log->count);
    error = priv_mdl_stream_text(error, diagnostic, " more (log truncated)\n");
  }
  return error;
}
