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
#include "ra8_attributes.h"

/** @brief Human-readable size/rate scaling and buffer sizes. */
typedef enum : uint32_t {
  k_bytes_per_kib      = 1024U,         /**< Bytes in a kibibyte.            */
  k_bytes_per_mib      = 1024U * 1024U, /**< Bytes in a mebibyte.            */
  k_ms_per_sec         = 1000U,         /**< Milliseconds per second (rate). */
  k_human_bytes        = 32U,           /**< Size/rate string buffer bytes.  */
  k_progress_bar_width = 20U,           /**< Rendered progress-bar cells.    */
  k_percent_scale      = 100U,          /**< Whole-percent conversion scale. */
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
RA8_INTERNAL static void fmt_size(uint64_t bytes, char* buf, size_t cap)
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
RA8_INTERNAL static void fmt_rate(uint64_t bytes, uint32_t elapsed_ms, char* buf, size_t cap)
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

void mdl_report_progress(void* ctx, const mdl_fetch_progress_t* ev)
{
  (void)ctx;
  if (ev == nullptr) {
    return;
  }
  if (ev->reused) {
    (void)printf("  [ch %zu/%zu %s] page %zu/%zu  reused\n",
                 ev->chapter_index,
                 ev->chapter_total,
                 ev->chapter_id,
                 ev->page_index,
                 ev->page_total);
    return;
  }
  char size[k_human_bytes];
  char rate[k_human_bytes];
  fmt_size(ev->page_bytes, size, sizeof(size));
  fmt_rate(ev->page_bytes, ev->elapsed_ms, rate, sizeof(rate));
  (void)printf("  [ch %zu/%zu %s] page %zu/%zu  %s @ %s\n",
               ev->chapter_index,
               ev->chapter_total,
               ev->chapter_id,
               ev->page_index,
               ev->page_total,
               size,
               rate);
}

void mdl_report_progress_bar(void* ctx, const mdl_fetch_progress_t* ev)
{
  (void)ctx;
  if (ev == nullptr) {
    return;
  }
  const size_t bar_width = (size_t)k_progress_bar_width;
  size_t       filled    = 0U;
  if (ev->page_total > 0U) {
    filled = (ev->page_index * bar_width) / ev->page_total;
    if (filled > bar_width) {
      filled = bar_width;
    }
  }
  char bar[32];
  for (size_t i = 0U; i < bar_width; ++i) {
    bar[i] = (i < filled) ? '=' : ' ';
  }
  bar[bar_width]     = '\0';
  const uint32_t pct = (ev->page_total > 0U)
                         ? (uint32_t)((ev->page_index * (size_t)k_percent_scale) / ev->page_total)
                         : 0U;

  if (ev->reused) {
    (void)printf("\r  [%s] %3u%% [ch %zu/%zu %s] page %zu/%zu (reused)",
                 bar,
                 (unsigned)pct,
                 ev->chapter_index,
                 ev->chapter_total,
                 ev->chapter_id,
                 ev->page_index,
                 ev->page_total);
  } else {
    char size[k_human_bytes];
    char rate[k_human_bytes];
    fmt_size(ev->page_bytes, size, sizeof(size));
    fmt_rate(ev->page_bytes, ev->elapsed_ms, rate, sizeof(rate));
    (void)printf("\r  [%s] %3u%% [ch %zu/%zu %s] page %zu/%zu (%s @ %s)",
                 bar,
                 (unsigned)pct,
                 ev->chapter_index,
                 ev->chapter_total,
                 ev->chapter_id,
                 ev->page_index,
                 ev->page_total,
                 size,
                 rate);
  }
  if (ev->page_index >= ev->page_total) {
    (void)printf("\n");
  }
  (void)fflush(stdout);
}

void mdl_report_failures(const mdl_fetch_faillog_t* log)
{
  if (log->total == 0U) {
    return;
  }
  (void)fprintf(stderr, "%zu failure(s) this run:\n", log->total);
  for (size_t i = 0U; i < log->count; ++i) {
    char reason[k_mdl_reason_max];
    mdl_fetch_reason(log->items[i].err, log->items[i].status, reason, sizeof(reason));
    (void)fprintf(stderr, "  FAILED %s -- %s\n", log->items[i].url, reason);
  }
  if (log->total > log->count) {
    (void)fprintf(stderr, "  ... and %zu more (log truncated)\n", log->total - log->count);
  }
}
