/**
 * @file test_media_dl_report_pack.c
 * @brief Host qualification of the media_dl progress presenter and packager.
 * @details Renders through ::mdl_report_progress, ::mdl_report_progress_bar and
 *          ::mdl_report_failures into a bounded RAM sink, asserting every byte,
 *          then drives the four ::mdl_pack entry points over their path guards,
 *          incomplete-run policy, and real container/directory output.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "mdl_export.h"
#include "mdl_fetch.h"
#include "mdl_pack.h"
#include "mdl_report.h"
#include "mdl_test_storage.h"
#include "ra8_attributes.h"
#include "ra8_io_stream_ram.h"
#include "support/ra8_test_output.h"
#include "tiny_jpeg_fixture.h"
#include "unity_minimal.h"

/** @brief Bounded fixture buffers and the truncating sink capacities. */
typedef enum : uint32_t {
  k_capture_bytes  = 2048U,              /**< Capture/expected-text buffer bytes. */
  k_path_bytes     = 1536U,              /**< Composed filesystem-path scratch.   */
  k_arena_bytes    = 8U * 1024U * 1024U, /**< Exporter workspace arena bytes.     */
  k_short_line_cap = 24U,                /**< Ends before the size fragment.      */
  k_short_bar_cap  = 5U,                 /**< Ends one cell into the filled bar.  */
  k_short_log_cap  = 3U,                 /**< Ends inside the summary header.     */
} mdl_rp_bound_t;

/** @brief Byte counts that straddle the size formatter's unit boundaries. */
typedef enum : uint64_t {
  k_bytes_none      = 0U,       /**< Renders through the plain-bytes arm.     */
  k_bytes_half_kib  = 512U,     /**< Sub-kibibyte transfer for the rate arm.  */
  k_bytes_sub_kib   = 1023U,    /**< Largest value still rendered as bytes.   */
  k_bytes_one_kib   = 1024U,    /**< Exactly the kibibyte threshold.          */
  k_bytes_two_kib   = 2048U,    /**< Two kibibytes for a whole KB/s rate.     */
  k_bytes_reused    = 7777U,    /**< Non-round size proving the reuse branch. */
  k_bytes_sub_mib   = 1048575U, /**< One byte below the mebibyte threshold.   */
  k_bytes_one_mib   = 1048576U, /**< Exactly the mebibyte threshold.          */
  k_bytes_1p5_mib   = 1572864U, /**< One and a half mebibytes.                */
  k_bytes_three_mib = 3145728U, /**< Three mebibytes for a fractional MB/s.   */
} mdl_rp_bytes_vector_t;

/** @brief Elapsed-time vectors selecting each arm of the rate formatter. */
typedef enum : uint32_t {
  k_ms_unmeasured = 0U,    /**< Selects the `-- KB/s` placeholder arm. */
  k_ms_reused     = 55U,   /**< Non-round interval for the reuse pair. */
  k_ms_one_second = 1000U, /**< One second of wall time.               */
  k_ms_two_second = 2000U, /**< Two seconds of wall time.              */
} mdl_rp_elapsed_vector_t;

/** @brief Run and page coordinates, plus the packaging outcomes and tallies. */
typedef enum : size_t {
  k_line_chap_index       = 1U,   /**< Chapter position of every page line.      */
  k_line_chap_total       = 2U,   /**< Chapter count of every page line.         */
  k_line_page_index       = 3U,   /**< Page position of every page line.         */
  k_line_page_total       = 9U,   /**< Page count of every page line.            */
  k_sole_position         = 1U,   /**< Single-chapter, single-page coordinate.   */
  k_bar_chapter           = 1U,   /**< Chapter index and total of every bar.     */
  k_bar_start             = 0U,   /**< Page index rendering an empty bar.        */
  k_bar_reused            = 2U,   /**< Page index of the reused bar vector.      */
  k_bar_third             = 3U,   /**< Page index rendering a 30 percent bar.    */
  k_bar_half              = 5U,   /**< Page index of the bar sink-failure case.  */
  k_bar_nine              = 9U,   /**< Page index rendering a one-digit percent. */
  k_bar_ten               = 10U,  /**< Page index rendering a two-digit percent. */
  k_bar_over              = 15U,  /**< Page index past the total (clamp case).   */
  k_bar_no_pages          = 0U,   /**< Page total exercising the divide guard.   */
  k_bar_ten_pages         = 10U,  /**< Ten-page chapter total.                   */
  k_bar_many_pages        = 100U, /**< Hundred-page chapter total.               */
  k_pack_ok               = 0U,   /**< Nothing failed in this operation.         */
  k_pack_failed           = 1U,   /**< Exactly one packaging failure counted.    */
  k_stats_none            = 0U,   /**< Nothing fetched, so nothing to package.   */
  k_stats_one_chapter     = 1U,   /**< One chapter completed in the run.         */
  k_stats_failed_chapters = 2U,   /**< Chapters left partial by a failure.       */
  k_stats_failed_pages    = 3U,   /**< Individual pages that failed.             */
} mdl_rp_coord_t;

/** @brief Failure-log shape, HTTP statuses, and packaging fixture bounds. */
typedef enum : uint16_t {
  k_item_first        = 0U,    /**< Index of the first stored failure.       */
  k_item_second       = 1U,    /**< Index of the second stored failure.      */
  k_log_stored_single = 1U,    /**< Stored count of the truncated fixture.   */
  k_log_stored_pair   = 2U,    /**< Stored count of the complete fixture.    */
  k_log_total_pair    = 2U,    /**< Observed count of the complete fixture.  */
  k_log_total_many    = 5U,    /**< Observed count of the truncated fixture. */
  k_http_absent       = 0U,    /**< No HTTP status was observed.             */
  k_http_not_found    = 404U,  /**< Observed status of the first entry.      */
  k_dir_mode          = 0755U, /**< rwxr-xr-x for every scratch directory.   */
  k_page_bytes        = 4U,    /**< Bytes in one synthetic page file.        */
  k_long_leaf_chars   = 300U,  /**< Leaf length overflowing the leaf buffer. */
  k_deep_dir_chars    = 1010U, /**< Parent that fits one join, then cannot.  */
} mdl_rp_pack_bound_t;

/** @brief One caller-owned bounded capture sink plus its backing bytes. */
typedef struct {
  uint8_t                   bytes[k_capture_bytes]; /**< Captured stream bytes. */
  ra8_io_stream_ram_state_t state;                  /**< RAM sink state.        */
  ra8_io_stream_t           stream;                 /**< Bound stream handle.   */
} mdl_rp_capture_t;

/** @brief Capture sink standing in for the CLI's successful-output stream. */
static mdl_rp_capture_t s_out;

/** @brief Capture sink standing in for the CLI's diagnostic stream. */
static mdl_rp_capture_t s_diag;

/** @brief Bounded failure log driven through the end-of-run summary. */
static mdl_fetch_faillog_t s_faillog;

/** @brief Caller-owned arena backing every export in this process. */
static uint8_t s_arena[k_arena_bytes];

/** @brief Exporter workspace descriptor bound to ::s_arena. */
static mdl_export_workspace_t s_ws;

/** @brief Default metadata every explicit-metadata packaging vector embeds. */
static mdl_export_meta_t s_meta;

/** @brief Series directory every packaging fixture composes paths beneath. */
static const char* const s_series_dir = "/tmp/mdl_rp_series";

/**
 * @brief Rebind one capture sink over a chosen slice of its backing bytes.
 * @details Binding fewer bytes than a rendering needs makes the sink fail there.
 * @param[in,out] capture Caller-owned capture fixture to rebind.
 * @param[in] limit Sink capacity in bytes, at most ::k_capture_bytes.
 * @return Nothing; a binding failure terminates the test process.
 * @pre @p capture points to one writable ::mdl_rp_capture_t.
 * @pre @p limit is nonzero and no larger than ::k_capture_bytes.
 * @post The capture reports zero used bytes.
 * @post The bound stream appends into @p capture's own bytes only.
 * @note Test-only helper; this process is single-threaded and serial.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_bind(mdl_rp_capture_t* capture, uint32_t limit)
{
  TEST_ASSERT(limit <= (uint32_t)k_capture_bytes);
  (void)memset(capture->bytes, 0, sizeof(capture->bytes));
  capture->state  = (ra8_io_stream_ram_state_t){};
  capture->stream = (ra8_io_stream_t){};
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_io_stream_ram_init(&capture->stream, &capture->state, capture->bytes, limit));
}

/**
 * @brief Rebind both capture sinks at full capacity before one packaging call.
 * @details Both streams are asserted by every vector, so a leak between them fails.
 * @return Nothing; a binding failure terminates the test process.
 * @pre ::s_out and ::s_diag are file-scope capture fixtures.
 * @pre No packaging call is in flight.
 * @post Both sinks report zero used bytes.
 * @post Both sinks accept ::k_capture_bytes before reporting exhaustion.
 * @note Test-only helper; this process is single-threaded and serial.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_reset_streams(void)
{
  internal_bind(&s_out, (uint32_t)k_capture_bytes);
  internal_bind(&s_diag, (uint32_t)k_capture_bytes);
}

/**
 * @brief Require that a capture holds exactly @p expected and nothing more.
 * @details Compares length first, so a merely-correct prefix still fails.
 * @param[in] capture Bound capture fixture holding the rendered bytes.
 * @param[in] expected NUL-terminated expected rendering (may be empty).
 * @return Nothing; a mismatch terminates the test process.
 * @pre @p capture was bound by ::internal_bind.
 * @pre @p expected is NUL-terminated and fits the sink capacity.
 * @post Normal return means the capture equals @p expected byte for byte.
 * @post No fixture state is modified.
 * @note Test-only helper; an assertion failure terminates the process.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_expect_text(const mdl_rp_capture_t* capture, const char* expected)
{
  uint32_t used = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_stream_ram_used(&capture->state, &used));
  TEST_ASSERT_EQ((int64_t)strlen(expected), (int64_t)used);
  TEST_ASSERT(memcmp(capture->bytes, expected, (size_t)used) == 0);
}

/**
 * @brief Render one event as a per-page line and assert its exact text.
 * @details Calls the presenter through the public ::mdl_progress_fn seam.
 * @param[in] event Progress event to render.
 * @param[in] expected Exact expected rendering.
 * @return Nothing; a mismatch terminates the test process.
 * @pre @p event is non-NULL and fully initialized.
 * @pre @p expected is NUL-terminated.
 * @post ::s_out holds exactly @p expected.
 * @post The presenter reported ::k_ra8_ok.
 * @note Test-only helper; an assertion failure terminates the process.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_line(const mdl_fetch_progress_t* event, const char* expected)
{
  internal_bind(&s_out, (uint32_t)k_capture_bytes);
  TEST_ASSERT_EQ(k_ra8_ok, mdl_report_progress(&s_out.stream, event));
  internal_expect_text(&s_out, expected);
}

/**
 * @brief Render one event as a progress bar and assert its exact text.
 * @details The asserted status also covers the bar's terminal flush.
 * @param[in] event Progress event to render.
 * @param[in] expected Exact expected rendering.
 * @return Nothing; a mismatch terminates the test process.
 * @pre @p event is non-NULL and fully initialized.
 * @pre @p expected is NUL-terminated.
 * @post ::s_out holds exactly @p expected.
 * @post The presenter reported ::k_ra8_ok.
 * @note Test-only helper; an assertion failure terminates the process.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_bar(const mdl_fetch_progress_t* event, const char* expected)
{
  internal_bind(&s_out, (uint32_t)k_capture_bytes);
  TEST_ASSERT_EQ(k_ra8_ok, mdl_report_progress_bar(&s_out.stream, event));
  internal_expect_text(&s_out, expected);
}

/**
 * @brief Recreate @p dir as an empty scratch directory.
 * @details Unlinks the bounded set of leaf names this suite ever writes, so an
 * "empty chapter" really is empty without enumerating the directory.
 * @param[in] dir Absolute scratch directory path.
 * @return Nothing; a creation failure terminates the test process.
 * @pre @p dir is NUL-terminated and shorter than ::k_path_bytes.
 * @pre Any file in @p dir was written by this suite under a known leaf name.
 * @post @p dir exists and holds no entries.
 * @post No path outside @p dir is touched.
 * @note Test-only POSIX fixture helper.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_fresh_dir(const char* dir)
{
  static const char* const k_known_leaves[] = {"page_001.jpg", "page_002.jpg", "page_001.jof"};
  for (size_t idx = 0U; idx < (sizeof(k_known_leaves) / sizeof(k_known_leaves[0])); ++idx) {
    char      victim[k_path_bytes];
    const int composed = snprintf(victim, sizeof(victim), "%s/%s", dir, k_known_leaves[idx]);
    if ((composed > 0) && ((size_t)composed < sizeof(victim))) {
      (void)unlink(victim);
    }
  }
  (void)rmdir(dir);
  TEST_ASSERT_EQ(0, mkdir(dir, (mode_t)k_dir_mode));
}

/**
 * @brief Write one page file with exactly @p length bytes into @p dir.
 * @details Asserts the whole byte count landed, so no export sees a partial fixture.
 * @param[in] dir Existing scratch directory.
 * @param[in] leaf Page file name within @p dir.
 * @param[in] data Bytes to write.
 * @param[in] length Number of bytes at @p data.
 * @return Nothing; any write failure terminates the test process.
 * @pre @p dir exists and @p leaf contains no separator.
 * @pre @p data spans @p length readable bytes.
 * @post `dir/leaf` holds exactly @p length bytes.
 * @post No descriptor is leaked out of the call.
 * @note Test-only POSIX fixture helper.
 * @since 0.1.0
 */
RA8_INTERNAL static void
internal_add_page(const char* dir, const char* leaf, const uint8_t* data, size_t length)
{
  char      path[k_path_bytes];
  const int composed = snprintf(path, sizeof(path), "%s/%s", dir, leaf);
  TEST_ASSERT((composed > 0) && ((size_t)composed < sizeof(path)));
  const int descriptor = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, S_IRUSR | S_IWUSR);
  TEST_ASSERT(descriptor >= 0);
  const ssize_t written = write(descriptor, data, length);
  TEST_ASSERT_EQ(0, close(descriptor));
  TEST_ASSERT_EQ((int64_t)length, (int64_t)written);
}

/**
 * @brief Report whether @p path names a regular file with content.
 * @details Outputs are unlinked first, so a stale artifact cannot pass this check.
 * @param[in] path Absolute artifact path.
 * @return Whether a non-empty regular file exists at @p path.
 * @retval true @p path is a regular file of nonzero size.
 * @retval false @p path is absent, not regular, or empty.
 * @pre @p path is NUL-terminated.
 * @pre The caller owns @p path for the duration of the test.
 * @post No filesystem state is modified.
 * @post The result reflects the filesystem at call time only.
 * @note Test-only POSIX fixture helper.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_file_exists(const char* path)
{
  struct stat info = {};
  return (stat(path, &info) == 0) && S_ISREG(info.st_mode) && (info.st_size > 0);
}

/**
 * @brief Populate one bounded failure-log entry.
 * @details Clears the entry first, then stores URL, status and classified error.
 * @param[out] item Failure entry to populate.
 * @param[in] url Failing URL, shorter than the entry's URL field.
 * @param[in] status Observed HTTP status, or zero when none applies.
 * @param[in] err Classified transfer failure.
 * @return Nothing; an over-long URL terminates the test process.
 * @pre @p item points to one writable ::mdl_fetch_fail_t.
 * @pre @p url is NUL-terminated.
 * @post @p item carries a NUL-terminated copy of @p url.
 * @post @p item carries @p status and @p err unchanged.
 * @note Test-only fixture helper.
 * @since 0.1.0
 */
RA8_INTERNAL static void
internal_fill_failure(mdl_fetch_fail_t* item, const char* url, long status, ra8_err_t err)
{
  (void)memset(item, 0, sizeof(*item));
  const int composed = snprintf(item->url, sizeof(item->url), "%s", url);
  TEST_ASSERT((composed > 0) && ((size_t)composed < sizeof(item->url)));
  item->status = status;
  item->err    = err;
}

/**
 * @brief Package one chapter with ::s_meta into the two capture sinks.
 * @details Rebinds both sinks, then forwards to ::mdl_pack_one_meta.
 * @param[in] format Output format to write.
 * @param[in] series Series directory the chapter leaf is joined beneath.
 * @param[in] chapter Chapter leaf name.
 * @return Count of failures the packaging operation reported.
 * @retval 0U Packaging succeeded.
 * @retval 1U A path was rejected or the export failed.
 * @pre The process-local storage binding is initialized.
 * @pre ::s_ws was bound to ::s_arena and ::s_meta was initialized.
 * @post ::s_out and ::s_diag hold this call's complete output.
 * @post No earlier vector's text survives into either sink.
 * @note Test-only helper; not thread-safe (shared sinks and workspace).
 * @since 0.1.0
 */
RA8_INTERNAL static size_t
internal_pack_chapter(ra8_mdl_format_t format, const char* series, const char* chapter)
{
  internal_reset_streams();
  return mdl_pack_one_meta(mdl_test_storage_get(),
                           format,
                           series,
                           chapter,
                           &s_meta,
                           &s_ws,
                           &s_out.stream,
                           &s_diag.stream);
}

/**
 * @brief Package one combined folder with ::s_meta into the capture sinks.
 * @details Rebinds both sinks, then forwards to ::mdl_pack_combined_meta.
 * @param[in] format Output format to write.
 * @param[in] allow Whether an incomplete run may still be packaged.
 * @param[in] series Series directory the folder leaf is joined beneath.
 * @param[in] folder Combined folder leaf name.
 * @param[in] stats Run tallies driving the completion policy.
 * @return Count of failures the packaging operation reported.
 * @retval 0U Nothing needed packaging, policy skipped it, or it succeeded.
 * @retval 1U A path was rejected or the export failed.
 * @pre The process-local storage binding is initialized.
 * @pre ::s_ws was bound to ::s_arena and ::s_meta was initialized.
 * @post ::s_out and ::s_diag hold this call's complete output.
 * @post No earlier vector's text survives into either sink.
 * @note Test-only helper; not thread-safe (shared sinks and workspace).
 * @since 0.1.0
 */
RA8_INTERNAL static size_t internal_pack_folder(ra8_mdl_format_t         format,
                                                bool                     allow,
                                                const char*              series,
                                                const char*              folder,
                                                const mdl_fetch_stats_t* stats)
{
  internal_reset_streams();
  return mdl_pack_combined_meta(mdl_test_storage_get(),
                                format,
                                allow,
                                series,
                                folder,
                                stats,
                                &s_meta,
                                &s_ws,
                                &s_out.stream,
                                &s_diag.stream);
}

/**
 * @test Every arm of the size and rate formatters renders its exact text.
 * @brief Cross the byte, kibibyte and mebibyte edges in both formatters.
 * @details Varies only bytes, interval and the reuse flag, so a changed line can
 *          only come from the arm the formatter chose. The reuse pair carries a
 *          nonzero size and interval both ways, so it cannot pass by accident.
 * @return Nothing; a mismatch terminates the test process.
 * @pre The report presenter and RAM stream backend are linked.
 * @pre ::s_out is a writable file-scope capture fixture.
 * @post Every vector rendered its exact documented line.
 * @post No filesystem or network activity occurred.
 * @note Host-only rendering test; runs serially in one process.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_report_units(void)
{
  TEST_BEGIN("report progress size and rate units");
  mdl_fetch_progress_t ev = {.chapter_index = k_line_chap_index,
                             .chapter_total = k_line_chap_total,
                             .chapter_id    = "ch7",
                             .page_index    = k_line_page_index,
                             .page_total    = k_line_page_total,
                             .page_bytes    = k_bytes_none,
                             .elapsed_ms    = k_ms_unmeasured,
                             .reused        = false};
  internal_line(&ev, "  [ch 1/2 ch7] page 3/9  0 B @ -- KB/s\n");
  ev.page_bytes = k_bytes_sub_kib;
  internal_line(&ev, "  [ch 1/2 ch7] page 3/9  1023 B @ -- KB/s\n");
  ev.page_bytes = k_bytes_one_kib;
  internal_line(&ev, "  [ch 1/2 ch7] page 3/9  1.0 KB @ -- KB/s\n");
  ev.page_bytes = k_bytes_sub_mib;
  internal_line(&ev, "  [ch 1/2 ch7] page 3/9  1024.0 KB @ -- KB/s\n");
  ev.page_bytes = k_bytes_one_mib;
  internal_line(&ev, "  [ch 1/2 ch7] page 3/9  1.0 MB @ -- KB/s\n");
  ev.page_bytes = k_bytes_1p5_mib;
  internal_line(&ev, "  [ch 1/2 ch7] page 3/9  1.5 MB @ -- KB/s\n");
  ev.elapsed_ms = k_ms_one_second;
  ev.page_bytes = k_bytes_half_kib;
  internal_line(&ev, "  [ch 1/2 ch7] page 3/9  512 B @ 0.5 KB/s\n");
  ev.page_bytes = k_bytes_two_kib;
  internal_line(&ev, "  [ch 1/2 ch7] page 3/9  2.0 KB @ 2.0 KB/s\n");
  ev.page_bytes = k_bytes_sub_mib;
  internal_line(&ev, "  [ch 1/2 ch7] page 3/9  1024.0 KB @ 1024.0 KB/s\n");
  ev.page_bytes = k_bytes_one_mib;
  internal_line(&ev, "  [ch 1/2 ch7] page 3/9  1.0 MB @ 1.0 MB/s\n");
  ev.page_bytes = k_bytes_three_mib;
  ev.elapsed_ms = k_ms_two_second;
  internal_line(&ev, "  [ch 1/2 ch7] page 3/9  3.0 MB @ 1.5 MB/s\n");
  ev.page_bytes = k_bytes_reused;
  ev.elapsed_ms = k_ms_reused;
  internal_line(&ev, "  [ch 1/2 ch7] page 3/9  7.6 KB @ 138.1 KB/s\n");
  ev.reused = true;
  internal_line(&ev, "  [ch 1/2 ch7] page 3/9  reused\n");
  TEST_END("report progress size and rate units");
}

/**
 * @test An absent event writes nothing and a full sink latches its error.
 * @brief Prove the NULL guard is a no-op and a mid-line failure propagates.
 * @details A NULL event must leave the sink untouched, asserted as a
 *          zero-length capture rather than a bare success code. The second
 *          vector ends the sink where the size fragment would start.
 * @return Nothing; a mismatch terminates the test process.
 * @pre The report presenter and RAM stream backend are linked.
 * @pre ::s_out is a writable file-scope capture fixture.
 * @post The NULL vector leaves the sink empty.
 * @post The truncated vector reports ::k_ra8_err_no_mem after 24 bytes.
 * @note Host-only rendering test; runs serially in one process.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_report_line_guards(void)
{
  TEST_BEGIN("report progress guards and sink failure");
  internal_bind(&s_out, (uint32_t)k_capture_bytes);
  TEST_ASSERT_EQ(k_ra8_ok, mdl_report_progress(&s_out.stream, nullptr));
  internal_expect_text(&s_out, "");
  const mdl_fetch_progress_t ev = {.chapter_index = k_sole_position,
                                   .chapter_total = k_sole_position,
                                   .chapter_id    = "ch",
                                   .page_index    = k_sole_position,
                                   .page_total    = k_sole_position,
                                   .page_bytes    = k_bytes_none,
                                   .elapsed_ms    = k_ms_unmeasured,
                                   .reused        = false};
  internal_bind(&s_out, (uint32_t)k_short_line_cap);
  TEST_ASSERT_EQ(k_ra8_err_no_mem, mdl_report_progress(&s_out.stream, &ev));
  internal_expect_text(&s_out, "  [ch 1/1 ch] page 1/1  ");
  TEST_END("report progress guards and sink failure");
}

/**
 * @test The percent column is right-aligned to three digits at every width.
 * @brief Drive one, two and three digit percentages through the bar renderer.
 * @details The padding is only observable in the rendered string, so each vector
 *          asserts the whole bar; 9/100 and 10/100 sit on the digit-width edge.
 * @return Nothing; a mismatch terminates the test process.
 * @pre The report presenter and RAM stream backend are linked.
 * @pre ::s_out is a writable file-scope capture fixture.
 * @post Every vector rendered its exact documented bar.
 * @post Bars below the final page carry no trailing newline.
 * @note Host-only rendering test; runs serially in one process.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_bar_padding(void)
{
  TEST_BEGIN("report progress bar percent padding");
  mdl_fetch_progress_t ev = {.chapter_index = k_bar_chapter,
                             .chapter_total = k_bar_chapter,
                             .chapter_id    = "x",
                             .page_index    = k_bar_start,
                             .page_total    = k_bar_ten_pages,
                             .page_bytes    = k_bytes_none,
                             .elapsed_ms    = k_ms_unmeasured,
                             .reused        = false};
  internal_bar(&ev, "\r  [                    ]   0% [ch 1/1 x] page 0/10 (0 B @ -- KB/s)");
  ev.page_index = k_bar_nine;
  ev.page_total = k_bar_many_pages;
  internal_bar(&ev, "\r  [=                   ]   9% [ch 1/1 x] page 9/100 (0 B @ -- KB/s)");
  ev.page_index = k_bar_ten;
  internal_bar(&ev, "\r  [==                  ]  10% [ch 1/1 x] page 10/100 (0 B @ -- KB/s)");
  ev.page_index = k_bar_third;
  ev.page_total = k_bar_ten_pages;
  internal_bar(&ev, "\r  [======              ]  30% [ch 1/1 x] page 3/10 (0 B @ -- KB/s)");
  TEST_END("report progress bar percent padding");
}

/**
 * @test The bar clamps, guards a zero total, and closes the final line.
 * @brief Drive the completion, over-run, zero-total, reuse and guard branches.
 * @details Without the clamp the unfilled run is `20 - 30`, an enormous `size_t`
 *          the bounded repeat helper refuses with ::k_ra8_err_invalid_size, so a
 *          successful status plus a twenty-cell bar proves the clamp is present.
 * @return Nothing; a mismatch terminates the test process.
 * @pre The report presenter and RAM stream backend are linked.
 * @pre ::s_out is a writable file-scope capture fixture.
 * @post Every terminal vector ends in a newline.
 * @post A NULL event writes nothing and a five-byte sink latches no-mem.
 * @note Host-only rendering test; runs serially in one process.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_bar_clamp(void)
{
  TEST_BEGIN("report progress bar clamp and terminal newline");
  mdl_fetch_progress_t ev = {.chapter_index = k_bar_chapter,
                             .chapter_total = k_bar_chapter,
                             .chapter_id    = "x",
                             .page_index    = k_bar_ten,
                             .page_total    = k_bar_ten_pages,
                             .page_bytes    = k_bytes_none,
                             .elapsed_ms    = k_ms_unmeasured,
                             .reused        = false};
  internal_bar(&ev, "\r  [====================] 100% [ch 1/1 x] page 10/10 (0 B @ -- KB/s)\n");
  ev.page_index = k_bar_over;
  internal_bar(&ev, "\r  [====================] 150% [ch 1/1 x] page 15/10 (0 B @ -- KB/s)\n");
  ev.page_index = k_bar_start;
  ev.page_total = k_bar_no_pages;
  internal_bar(&ev, "\r  [                    ]   0% [ch 1/1 x] page 0/0 (0 B @ -- KB/s)\n");
  ev.page_index = k_bar_reused;
  ev.page_total = k_bar_ten_pages;
  ev.page_bytes = k_bytes_reused;
  ev.elapsed_ms = k_ms_reused;
  ev.reused     = true;
  internal_bar(&ev, "\r  [====                ]  20% [ch 1/1 x] page 2/10 (reused)");
  internal_bind(&s_out, (uint32_t)k_capture_bytes);
  TEST_ASSERT_EQ(k_ra8_ok, mdl_report_progress_bar(&s_out.stream, nullptr));
  internal_expect_text(&s_out, "");
  ev.page_index = k_bar_half;
  ev.page_bytes = k_bytes_none;
  ev.elapsed_ms = k_ms_unmeasured;
  ev.reused     = false;
  internal_bind(&s_out, (uint32_t)k_short_bar_cap);
  TEST_ASSERT_EQ(k_ra8_err_no_mem, mdl_report_progress_bar(&s_out.stream, &ev));
  internal_expect_text(&s_out, "\r  [=");
  TEST_END("report progress bar clamp and terminal newline");
}

/**
 * @test The end-of-run summary names every failure and its true total.
 * @brief Render a clean run, a listed pair, a saturated log, and a full sink.
 * @details Nothing to report must print nothing, asserted as a zero-length capture.
 *          The fixture mixes a status-bearing failure with a status-free one so
 *          both reason arms appear; the last vector proves the latch survives.
 * @return Nothing; a mismatch terminates the test process.
 * @pre The report presenter and RAM stream backend are linked.
 * @pre ::s_faillog is a writable file-scope failure log.
 * @post A zero-total log leaves the sink empty.
 * @post The truncated sink reports ::k_ra8_err_no_mem after three bytes.
 * @note Host-only rendering test; runs serially in one process.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_failure_summary(void)
{
  TEST_BEGIN("report failures summary");
  (void)memset(&s_faillog, 0, sizeof(s_faillog));
  internal_bind(&s_diag, (uint32_t)k_capture_bytes);
  TEST_ASSERT_EQ(k_ra8_ok, mdl_report_failures(&s_diag.stream, &s_faillog));
  internal_expect_text(&s_diag, "");
  internal_fill_failure(&s_faillog.items[k_item_first],
                        "https://a/1.jpg",
                        (long)k_http_not_found,
                        k_ra8_err_not_found);
  internal_fill_failure(&s_faillog.items[k_item_second],
                        "https://a/2.jpg",
                        (long)k_http_absent,
                        k_ra8_err_timeout);
  s_faillog.count = (size_t)k_log_stored_pair;
  s_faillog.total = (size_t)k_log_total_pair;
  internal_bind(&s_diag, (uint32_t)k_capture_bytes);
  TEST_ASSERT_EQ(k_ra8_ok, mdl_report_failures(&s_diag.stream, &s_faillog));
  internal_expect_text(&s_diag,
                       "2 failure(s) this run:\n"
                       "  FAILED https://a/1.jpg -- not found (HTTP 404)\n"
                       "  FAILED https://a/2.jpg -- request timed out\n");
  s_faillog.count = (size_t)k_log_stored_single;
  s_faillog.total = (size_t)k_log_total_many;
  internal_bind(&s_diag, (uint32_t)k_capture_bytes);
  TEST_ASSERT_EQ(k_ra8_ok, mdl_report_failures(&s_diag.stream, &s_faillog));
  internal_expect_text(&s_diag,
                       "5 failure(s) this run:\n"
                       "  FAILED https://a/1.jpg -- not found (HTTP 404)\n"
                       "  ... and 4 more (log truncated)\n");
  internal_bind(&s_diag, (uint32_t)k_short_log_cap);
  TEST_ASSERT_EQ(k_ra8_err_no_mem, mdl_report_failures(&s_diag.stream, &s_faillog));
  internal_expect_text(&s_diag, "5 f");
  TEST_END("report failures summary");
}

/**
 * @test Chapter packaging refuses every path it cannot compose safely.
 * @brief Reject a separator-bearing leaf, an over-long leaf, and a long parent.
 * @details All three count one failure, report only on the diagnostic stream,
 *          and leave the success stream untouched -- asserted every time.
 * @return Nothing; a mismatch terminates the test process.
 * @pre The packaging module and process-local storage binding are linked.
 * @pre ::s_ws was bound to ::s_arena.
 * @post Each vector returned exactly one counted failure.
 * @post The success stream stayed empty for every vector.
 * @note Host-only test; the rejected paths are never created on disk.
 * @par MC/DC:
 * Decision: `!internal_pack_snprintf_fit(ln, sizeof(leaf)) ||
 * !mdl_path_join(series_dir, leaf, out, sizeof(out))` (2 conditions). Cited as
 * tools/media_dl/src/mdl_pack.c@mdl_pack_one_meta.
 * - Vector 1: leaf fits, join succeeds -> false (control; driven by
 *   ::internal_test_pack_one_container, which reaches the export call)
 * - Vector 2: a 300-character leaf does not fit -> true (varies condition 1;
 *   the short circuit leaves condition 2 unevaluated)
 * - Vector 3: a 12-character leaf fits, but under a 1010-character parent the
 *   join overflows the 1024-byte output -> true (varies condition 2)
 * Vectors 1+2 prove condition 1's independent influence, 1+3 condition 2's.
 * N+1 = 3 vectors for N=2 conditions: minimal MC/DC.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_pack_one_path_guards(void)
{
  TEST_BEGIN("pack one path guards");
  size_t rc = internal_pack_chapter(k_ra8_mdl_format_cbz, s_series_dir, "a/b");
  TEST_ASSERT_EQ((int64_t)k_pack_failed, (int64_t)rc);
  internal_expect_text(&s_out, "");
  internal_expect_text(&s_diag, "  export a/b.cbz path rejected, skipped\n");
  char long_id[k_long_leaf_chars + 1U];
  (void)memset(long_id, 'x', (size_t)k_long_leaf_chars);
  long_id[k_long_leaf_chars] = '\0';
  char      expect[k_capture_bytes];
  const int composed =
    snprintf(expect, sizeof(expect), "  export %s.cbz path rejected, skipped\n", long_id);
  TEST_ASSERT((composed > 0) && ((size_t)composed < sizeof(expect)));
  rc = internal_pack_chapter(k_ra8_mdl_format_cbz, s_series_dir, long_id);
  TEST_ASSERT_EQ((int64_t)k_pack_failed, (int64_t)rc);
  internal_expect_text(&s_out, "");
  internal_expect_text(&s_diag, expect);
  char deep[k_deep_dir_chars + 1U];
  (void)memset(deep, 'd', (size_t)k_deep_dir_chars);
  deep[k_deep_dir_chars] = '\0';
  rc                     = internal_pack_chapter(k_ra8_mdl_format_cbz, deep, "abcdefghijkl");
  TEST_ASSERT_EQ((int64_t)k_pack_failed, (int64_t)rc);
  internal_expect_text(&s_out, "");
  internal_expect_text(&s_diag, "  export abcdefghijkl.cbz path rejected, skipped\n");
  TEST_END("pack one path guards");
}

/**
 * @test A chapter packages into a real container, and a failure is diagnosed.
 * @brief Package two synthetic pages, then fail on an empty chapter directory.
 * @details ::mdl_pack_one delegates with absent metadata, so the on-disk loader
 *          runs; the archive is asserted absent first. An empty directory is the
 *          exporter's ::k_ra8_err_empty case, so the diagnostic uses that symbol.
 * @return Nothing; a mismatch terminates the test process.
 * @pre The exporter, packaging module and storage binding are linked.
 * @pre `/tmp` is writable by the test process.
 * @post The chapter archive exists beside its page directory.
 * @post The failing vector counts one failure and writes no success text.
 * @note Host-only test; leaves its scratch fixtures in `/tmp`.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_pack_one_container(void)
{
  TEST_BEGIN("pack one container output");
  const char*   chapter            = "/tmp/mdl_rp_series/chap01";
  const char*   archive            = "/tmp/mdl_rp_series/chap01.cbz";
  const uint8_t page[k_page_bytes] = {'p', 'a', 'g', 'e'};
  internal_fresh_dir(chapter);
  internal_add_page(chapter, "page_001.jpg", page, sizeof(page));
  internal_add_page(chapter, "page_002.jpg", page, sizeof(page));
  (void)unlink(archive);
  TEST_ASSERT(!internal_file_exists(archive));
  internal_reset_streams();
  const size_t direct = mdl_pack_one(mdl_test_storage_get(),
                                     k_ra8_mdl_format_cbz,
                                     s_series_dir,
                                     "chap01",
                                     &s_ws,
                                     &s_out.stream,
                                     &s_diag.stream);
  TEST_ASSERT_EQ((int64_t)k_pack_ok, (int64_t)direct);
  internal_expect_text(&s_out, "  packaged chap01.cbz\n");
  internal_expect_text(&s_diag, "");
  TEST_ASSERT(internal_file_exists(archive));
  internal_fresh_dir("/tmp/mdl_rp_series/chapempty");
  char      expect[k_capture_bytes];
  const int composed = snprintf(expect,
                                sizeof(expect),
                                "  export chapempty.cbz FAILED (err 0x%x)\n",
                                (unsigned)k_ra8_err_empty);
  TEST_ASSERT((composed > 0) && ((size_t)composed < sizeof(expect)));
  const size_t rc = internal_pack_chapter(k_ra8_mdl_format_cbz, s_series_dir, "chapempty");
  TEST_ASSERT_EQ((int64_t)k_pack_failed, (int64_t)rc);
  internal_expect_text(&s_out, "");
  internal_expect_text(&s_diag, expect);
  TEST_END("pack one container output");
}

/**
 * @test A directory-output format reports the directory, never an archive.
 * @brief Package one real JPEG into JOF siblings, then fail on an empty folder.
 * @details JOF writes one `.jof` beside each page instead of a container, so the
 *          success text names the directory glob and the sibling must exist.
 * @return Nothing; a mismatch terminates the test process.
 * @pre The JOF producer, exporter and storage binding are linked.
 * @pre `/tmp` is writable by the test process.
 * @post The successful vector leaves a `.jof` sibling inside the chapter.
 * @post The failing vector counts one failure and writes no success text.
 * @note Host-only test; leaves its scratch fixtures in `/tmp`.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_pack_one_dir_output(void)
{
  TEST_BEGIN("pack one directory output");
  const char* chapter = "/tmp/mdl_rp_series/chapjof";
  const char* sibling = "/tmp/mdl_rp_series/chapjof/page_001.jof";
  internal_fresh_dir(chapter);
  internal_add_page(chapter, "page_001.jpg", s_tiny_jpeg, (size_t)s_tiny_jpeg_len);
  size_t rc = internal_pack_chapter(k_ra8_mdl_format_jof, s_series_dir, "chapjof");
  TEST_ASSERT_EQ((int64_t)k_pack_ok, (int64_t)rc);
  internal_expect_text(&s_out, "  converted chapjof -> /tmp/mdl_rp_series/chapjof/*.jof\n");
  internal_expect_text(&s_diag, "");
  TEST_ASSERT(internal_file_exists(sibling));
  internal_fresh_dir("/tmp/mdl_rp_series/chapjofempty");
  char      expect[k_capture_bytes];
  const int composed = snprintf(expect,
                                sizeof(expect),
                                "  export chapjofempty.jof FAILED (err 0x%x)\n",
                                (unsigned)k_ra8_err_empty);
  TEST_ASSERT((composed > 0) && ((size_t)composed < sizeof(expect)));
  rc = internal_pack_chapter(k_ra8_mdl_format_jof, s_series_dir, "chapjofempty");
  TEST_ASSERT_EQ((int64_t)k_pack_failed, (int64_t)rc);
  internal_expect_text(&s_out, "");
  internal_expect_text(&s_diag, expect);
  TEST_END("pack one directory output");
}

/**
 * @test Combined packaging refuses to publish a complete-looking archive.
 * @brief Skip an empty run, refuse an unallowed partial run, then mark one.
 * @details The empty-run short circuit must write nothing at all, on BOTH streams.
 *          The refusal names the exact failed counts, and the permitted partial
 *          run must carry `INCOMPLETE` in its text and in its filename.
 * @return Nothing; a mismatch terminates the test process.
 * @pre The exporter, packaging module and storage binding are linked.
 * @pre `/tmp` is writable by the test process.
 * @post No vector counted a packaging failure.
 * @post Only the permitted partial run created an archive.
 * @note Host-only test; leaves its scratch fixtures in `/tmp`.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_pack_combined_policy(void)
{
  TEST_BEGIN("pack combined completion policy");
  const char*   folder             = "/tmp/mdl_rp_series/combined";
  const char*   marked             = "/tmp/mdl_rp_series/combined.INCOMPLETE.cbz";
  const uint8_t page[k_page_bytes] = {'c', 'o', 'm', 'b'};
  internal_fresh_dir(folder);
  internal_add_page(folder, "page_001.jpg", page, sizeof(page));
  internal_add_page(folder, "page_002.jpg", page, sizeof(page));
  mdl_fetch_stats_t stats  = {};
  stats.chapters_completed = (size_t)k_stats_none;
  size_t rc = internal_pack_folder(k_ra8_mdl_format_cbz, false, s_series_dir, "combined", &stats);
  TEST_ASSERT_EQ((int64_t)k_pack_ok, (int64_t)rc);
  internal_expect_text(&s_out, "");
  internal_expect_text(&s_diag, "");
  stats.chapters_completed = (size_t)k_stats_one_chapter;
  stats.chapters_failed    = (size_t)k_stats_failed_chapters;
  stats.pages_failed       = (size_t)k_stats_failed_pages;
  rc = internal_pack_folder(k_ra8_mdl_format_cbz, false, s_series_dir, "combined", &stats);
  TEST_ASSERT_EQ((int64_t)k_pack_ok, (int64_t)rc);
  internal_expect_text(&s_out, "");
  internal_expect_text(&s_diag,
                       "  combine: NOT packaged -- run is incomplete (2 chapter(s) / 3 page(s) "
                       "failed); pass --allow-incomplete to force a marked archive\n");
  (void)unlink(marked);
  TEST_ASSERT(!internal_file_exists(marked));
  rc = internal_pack_folder(k_ra8_mdl_format_cbz, true, s_series_dir, "combined", &stats);
  TEST_ASSERT_EQ((int64_t)k_pack_ok, (int64_t)rc);
  internal_expect_text(&s_out,
                       "  combined (INCOMPLETE) -> "
                       "/tmp/mdl_rp_series/combined.INCOMPLETE.cbz\n");
  internal_expect_text(&s_diag, "");
  TEST_ASSERT(internal_file_exists(marked));
  TEST_END("pack combined completion policy");
}

/**
 * @test A clean run packages the combined folder under its unmarked name.
 * @brief Drive the delegating combined entry point to a real container.
 * @details ::mdl_pack_combined only delegates with absent metadata, so this also
 *          drives the on-disk loader; the archive is asserted absent first.
 * @return Nothing; a mismatch terminates the test process.
 * @pre The exporter, packaging module and storage binding are linked.
 * @pre `/tmp` is writable by the test process.
 * @post The unmarked combined archive exists beside its page folder.
 * @post The success line names the composed output path exactly.
 * @note Host-only test; leaves its scratch fixtures in `/tmp`.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_pack_combined_container(void)
{
  TEST_BEGIN("pack combined container output");
  const char*   folder             = "/tmp/mdl_rp_series/combined";
  const char*   archive            = "/tmp/mdl_rp_series/combined.cbz";
  const uint8_t page[k_page_bytes] = {'c', 'o', 'm', 'b'};
  internal_fresh_dir(folder);
  internal_add_page(folder, "page_001.jpg", page, sizeof(page));
  internal_add_page(folder, "page_002.jpg", page, sizeof(page));
  (void)unlink(archive);
  TEST_ASSERT(!internal_file_exists(archive));
  mdl_fetch_stats_t stats  = {};
  stats.chapters_completed = (size_t)k_stats_one_chapter;
  internal_reset_streams();
  const size_t rc = mdl_pack_combined(mdl_test_storage_get(),
                                      k_ra8_mdl_format_cbz,
                                      false,
                                      s_series_dir,
                                      "combined",
                                      &stats,
                                      &s_ws,
                                      &s_out.stream,
                                      &s_diag.stream);
  TEST_ASSERT_EQ((int64_t)k_pack_ok, (int64_t)rc);
  internal_expect_text(&s_out, "  combined -> /tmp/mdl_rp_series/combined.cbz\n");
  internal_expect_text(&s_diag, "");
  TEST_ASSERT(internal_file_exists(archive));
  TEST_END("pack combined container output");
}

/**
 * @test Combined packaging into a directory-output format names the folder.
 * @brief Convert one real JPEG in place, then fail on an empty combined folder.
 * @details Success reports the folder glob and leaves a `.jof` sibling; an empty
 *          folder is diagnosed through the combine failure renderer, in both the
 *          directory-output and single-container arms of the combine path.
 * @return Nothing; a mismatch terminates the test process.
 * @pre The JOF producer, exporter and storage binding are linked.
 * @pre `/tmp` is writable by the test process.
 * @post The successful vector leaves a `.jof` sibling inside the folder.
 * @post Both failing vectors count one failure and write no success text.
 * @note Host-only test; leaves its scratch fixtures in `/tmp`.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_pack_combined_dir_output(void)
{
  TEST_BEGIN("pack combined directory output");
  const char* folder  = "/tmp/mdl_rp_series/combjof";
  const char* sibling = "/tmp/mdl_rp_series/combjof/page_001.jof";
  internal_fresh_dir(folder);
  internal_add_page(folder, "page_001.jpg", s_tiny_jpeg, (size_t)s_tiny_jpeg_len);
  mdl_fetch_stats_t stats  = {};
  stats.chapters_completed = (size_t)k_stats_one_chapter;
  size_t rc = internal_pack_folder(k_ra8_mdl_format_jof, false, s_series_dir, "combjof", &stats);
  TEST_ASSERT_EQ((int64_t)k_pack_ok, (int64_t)rc);
  internal_expect_text(&s_out, "  combined -> /tmp/mdl_rp_series/combjof/*.jof\n");
  internal_expect_text(&s_diag, "");
  TEST_ASSERT(internal_file_exists(sibling));
  internal_fresh_dir("/tmp/mdl_rp_series/combjofempty");
  char      expect[k_capture_bytes];
  const int composed = snprintf(expect,
                                sizeof(expect),
                                "  combine export FAILED (err 0x%x)\n",
                                (unsigned)k_ra8_err_empty);
  TEST_ASSERT((composed > 0) && ((size_t)composed < sizeof(expect)));
  rc = internal_pack_folder(k_ra8_mdl_format_jof, false, s_series_dir, "combjofempty", &stats);
  TEST_ASSERT_EQ((int64_t)k_pack_failed, (int64_t)rc);
  internal_expect_text(&s_out, "");
  internal_expect_text(&s_diag, expect);
  rc = internal_pack_folder(k_ra8_mdl_format_cbz, false, s_series_dir, "combjofempty", &stats);
  TEST_ASSERT_EQ((int64_t)k_pack_failed, (int64_t)rc);
  internal_expect_text(&s_out, "");
  internal_expect_text(&s_diag, expect);
  TEST_END("pack combined directory output");
}

/**
 * @test Combined packaging refuses every path it cannot compose safely.
 * @brief Reject a separator-bearing leaf, an over-long leaf, and a long parent.
 * @details The combine path reports the series directory rather than the leaf, so
 *          the 1010-character parent proves the whole parent is carried.
 * @return Nothing; a mismatch terminates the test process.
 * @pre The packaging module and process-local storage binding are linked.
 * @pre ::s_ws was bound to ::s_arena.
 * @post Each vector returned exactly one counted failure.
 * @post The success stream stayed empty for every vector.
 * @note Host-only test; the rejected paths are never created on disk.
 *
 * @par MC/DC:
 * The same two-condition `!fit || !join` decision documented on
 * ::internal_test_pack_one_path_guards, here in
 * tools/media_dl/src/mdl_pack.c@internal_pack_combined_dir: the control comes
 * from ::internal_test_pack_combined_container, the 300-character leaf varies
 * condition 1, and the 1010-character parent varies condition 2.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_pack_combined_path_guards(void)
{
  TEST_BEGIN("pack combined path guards");
  mdl_fetch_stats_t stats  = {};
  stats.chapters_completed = (size_t)k_stats_one_chapter;
  size_t rc = internal_pack_folder(k_ra8_mdl_format_cbz, false, s_series_dir, "x/y", &stats);
  TEST_ASSERT_EQ((int64_t)k_pack_failed, (int64_t)rc);
  internal_expect_text(&s_out, "");
  internal_expect_text(&s_diag, "  combine export path rejected under /tmp/mdl_rp_series\n");
  char long_rel[k_long_leaf_chars + 1U];
  (void)memset(long_rel, 'y', (size_t)k_long_leaf_chars);
  long_rel[k_long_leaf_chars] = '\0';
  rc = internal_pack_folder(k_ra8_mdl_format_cbz, false, s_series_dir, long_rel, &stats);
  TEST_ASSERT_EQ((int64_t)k_pack_failed, (int64_t)rc);
  internal_expect_text(&s_out, "");
  internal_expect_text(&s_diag, "  combine export path rejected under /tmp/mdl_rp_series\n");
  char deep[k_deep_dir_chars + 1U];
  (void)memset(deep, 'd', (size_t)k_deep_dir_chars);
  deep[k_deep_dir_chars] = '\0';
  char      expect[k_capture_bytes];
  const int composed =
    snprintf(expect, sizeof(expect), "  combine export path rejected under %s\n", deep);
  TEST_ASSERT((composed > 0) && ((size_t)composed < sizeof(expect)));
  rc = internal_pack_folder(k_ra8_mdl_format_cbz, false, deep, "abcdefghijkl", &stats);
  TEST_ASSERT_EQ((int64_t)k_pack_failed, (int64_t)rc);
  internal_expect_text(&s_out, "");
  internal_expect_text(&s_diag, expect);
  TEST_END("pack combined path guards");
}

/**
 * @brief Run the progress-presenter and packaging qualification suite.
 * @details Binds the process-local POSIX storage, the exporter arena and the
 *          shared metadata once, ensures the series directory exists, then runs
 *          every vector serially; the harness exits on the first failure.
 * @return Process exit status.
 * @retval 0 Every registered test passed.
 * @pre `/tmp` is writable by the test process.
 * @pre The unity-minimal assertion harness is available.
 * @post The storage binding is released before returning.
 * @post Every registered test completed.
 * @note Runs serially in one single-threaded process.
 * @since 0.1.0
 */
int main(void)
{
  TEST_ASSERT_EQ(k_ra8_ok, mdl_test_storage_init());
  mdl_export_workspace_init(&s_ws, s_arena, sizeof(s_arena));
  mdl_meta_init(&s_meta);
  (void)mkdir(s_series_dir, (mode_t)k_dir_mode);
  internal_test_report_units();
  internal_test_report_line_guards();
  internal_test_bar_padding();
  internal_test_bar_clamp();
  internal_test_failure_summary();
  internal_test_pack_one_path_guards();
  internal_test_pack_one_container();
  internal_test_pack_one_dir_output();
  internal_test_pack_combined_policy();
  internal_test_pack_combined_container();
  internal_test_pack_combined_dir_output();
  internal_test_pack_combined_path_guards();
  TEST_ASSERT_EQ(k_ra8_ok, mdl_test_storage_deinit());
  (void)internal_test_output_fd_text(STDERR_FILENO, "[OK  ] test_media_dl_report_pack.c\n");
  return 0;
}
