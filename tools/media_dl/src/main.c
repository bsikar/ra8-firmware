/**
 * @file main.c
 * @brief media_dl -- host CLI for the e-reader media downloader.
 *
 * @details
 * Native host binary that links the firmware error contract (`ra8_err_t`) and
 * drives the downloader logic that will later run on the RA8 (only the injected
 * libcurl backend is host-specific). Modes:
 *
 *  - **series** (`--config S.conf --series URL`): read a site descriptor, list a
 *    series' chapters, and download them into a reader-openable file. Selection
 *    is by chapter identity, not list position: `--from CHAP` starts at the
 *    chapter NUMBERED CHAP, and `--update` fetches only chapters not already
 *    recorded complete in the per-series library state. Downloads resume after
 *    interruption and never re-fetch a page whose bytes are already held.
 *  - **library** (over `--out`): `--list` shows tracked series with coverage and
 *    gaps, `--update-all` incrementally updates every tracked series, `--remove`
 *    drops one series.
 *  - **pack** (`--pack DIR --format FMT`): package an existing image folder with
 *    no network.
 *  - **page** (bare `URL`): fetch one page and download its `<img>` URLs.
 *
 * The tool identifies itself honestly, honours robots.txt by default, and
 * sanitises every untrusted name before it reaches the filesystem.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "fw_if_fs_posix.h"
#include "mdl_atomic.h"
#include "mdl_cli.h"
#include "mdl_config.h"
#include "mdl_discover.h"
#include "mdl_export.h"
#include "mdl_extract.h"
#include "mdl_fetch.h"
#include "mdl_fetch_internal.h"
#include "mdl_hash.h"
#include "mdl_library.h"
#include "mdl_net.h"
#include "mdl_net_curl.h"
#include "mdl_pack.h"
#include "mdl_pathfs.h"
#include "mdl_politeness.h"
#include "mdl_report.h"
#include "mdl_sanitize.h"
#include "mdl_session.h"
#include "mdl_state.h"
#include "mdl_storage.h"
#include "mdl_url_guard.h"
#include "mdl_urlname.h"
#include "mdl_verify.h"
#include "ra8_attributes.h"
#include "ra8_err.h"

/** @brief Fixed sizing for the CLI (large buffers live in .bss). */
typedef enum : uint32_t {
  k_page_buf_bytes     = 8U * 1024U * 1024U,  /**< Max HTML page size.          */
  k_export_arena_bytes = 96U * 1024U * 1024U, /**< Host export scratch ceiling. */
  k_storage_work_bytes = 2048U,               /**< Per-handle FS backend state. */
} mdl_cli_limits_t;

/** @brief On-stack string buffer sizes and page-mode delays. */
typedef enum : uint16_t {
  k_slug_bytes         = 128,  /**< Series slug buffer.              */
  k_leaf_name_bytes    = 256,  /**< Composed archive/dir leaf name.  */
  k_dir_path_bytes     = 1024, /**< Directory-path buffer.           */
  k_file_path_bytes    = 1200, /**< File-path buffer.                */
  k_cov_bytes          = 256,  /**< Coverage summary line buffer.    */
  k_ext_bytes          = 8,    /**< Image-extension buffer.          */
  k_page_img_delay_min = 400,  /**< page-mode per-image floor, ms.   */
  k_page_img_delay_max = 800,  /**< page-mode per-image ceiling, ms. */
} mdl_bufsize_t;

/** @brief `--polite` per-image delay floors for page mode (milliseconds). */
typedef enum : uint16_t {
  k_polite_img_min_ms = 2000, /**< Polite per-image floor.   */
  k_polite_img_max_ms = 4000, /**< Polite per-image ceiling. */
} mdl_polite_floor_t;

/** @brief Filesystem constants. */
typedef enum : uint16_t {
  k_dir_mode = 0755, /**< mkdir() permission bits. */
} mdl_misc_t;

/**
 * @struct mdl_series_metadata_t
 * @brief Bounded metadata scraped while the series HTML is resident.
 * @details Separates remote cover identity from the exporter cover path: the
 *          former is persisted in state, while the latter is resolved only
 *          after a verified local cover exists under the series directory.
 * @since 0.1.0
 */
typedef struct {
  char                          title[k_mdl_title_max];       /**< Display title.            */
  char                          summary[k_mdl_summary_max];   /**< Synopsis/description.     */
  char                          writer[k_mdl_person_max];     /**< Writer/author.            */
  char                          artist[k_mdl_person_max];     /**< Artist/illustrator.       */
  char                          cover_url[k_mdl_url_max];     /**< Resolved remote cover.    */
  char                          language[k_mdl_language_max]; /**< BCP-47 language tag.      */
  mdl_state_reading_direction_t direction;                    /**< Page progression.         */
  bool                          title_selected;               /**< Title selector succeeded. */
} mdl_series_metadata_t;

/**
 * @var s_session
 * @brief The one download session (identity + robots cache) for this run.
 * @details Embeds a large per-host cache and fetch scratch, so it lives in
 *          `.bss` rather than on the stack.
 * @warning Single-threaded; reused across every fetch in one run.
 * @since 0.1.0
 */
static mdl_session_t s_session;

/** @brief Naturally aligned, caller-owned exporter workspace storage. */
typedef union {
  max_align_t align;                       /**< Force maximum C alignment. */
  uint8_t     bytes[k_export_arena_bytes]; /**< Bounded scratch bytes.     */
} export_arena_storage_t;

/** @brief Process-lifetime exporter workspace storage (zero heap). */
static export_arena_storage_t s_export_arena;

/** @brief Arena cursor reset by each exporter call; single-threaded CLI only. */
static mdl_export_workspace_t s_export_ws;

/** @brief Maximally aligned storage for one filesystem backend handle. */
typedef union {
  max_align_t align;                       /**< Force maximum C alignment. */
  uint8_t     bytes[k_storage_work_bytes]; /**< Opaque backend state.      */
} storage_workspace_t;

/** @brief Host-selected filesystem facade and adapter state. */
static fw_fs_t             s_fs;
static fw_fs_posix_state_t s_fs_posix = {.root_fd = -1};
/** @brief One file and one transaction workspace for single-threaded storage. */
static storage_workspace_t s_fs_file_work;
static storage_workspace_t s_fs_transaction_work;
/** @brief Caller-owned streaming buffer shared by serial storage operations. */
static uint8_t s_fs_io_buffer[k_mdl_storage_io_bytes];
/** @brief Portable storage dependency injected into the fetch core. */
static mdl_storage_t s_storage;

/**
 * @brief Bind the host root through the POSIX composition adapter
 * @details Opens `/` as the confined host adapter root, validates the static
 * file/transaction/I/O workspaces, and publishes the dependency injected into
 * downloader operations. If the second stage fails, the POSIX adapter is
 * deinitialized before the error is returned.
 * @return Canonical storage-composition status.
 * @retval k_ra8_ok The POSIX facade and downloader binding are ready.
 * @retval other POSIX adapter or workspace binding initialization failed.
 * @pre The process-lifetime storage objects are not currently initialized.
 * @pre The static workspaces remain exclusively owned by the CLI thread.
 * @post Success leaves ::s_storage ready until the matching deinit in `main`.
 * @post Failure leaves no initialized POSIX adapter resource.
 * @note Host composition only; firmware supplies a VFS facade instead.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_storage_init(void)
{
  const fw_fs_posix_cfg_t cfg = {.root_path = "/", .removable_media = false};
  ra8_err_t               err = fw_fs_posix_init(&s_fs, &s_fs_posix, &cfg);
  if (err != k_ra8_ok) {
    return err;
  }
  err = mdl_storage_init(&s_storage,
                         &s_fs,
                         s_fs_file_work.bytes,
                         sizeof(s_fs_file_work.bytes),
                         s_fs_transaction_work.bytes,
                         sizeof(s_fs_transaction_work.bytes),
                         s_fs_io_buffer,
                         sizeof(s_fs_io_buffer));
  if (err != k_ra8_ok) {
    (void)fw_fs_posix_deinit(&s_fs_posix);
  }
  return err;
}

/**
 * @var s_page
 * @brief Whole-page HTML scratch buffer for the current fetch.
 * @details Sized by ::k_page_buf_bytes and placed in `.bss` rather than on the
 *          stack, keeping the CLI's zero-dynamic-allocation shape.
 * @warning Single-threaded: each fetch overwrites the previous page.
 * @since 0.1.0
 */
static char s_page[k_page_buf_bytes];

/**
 * @var s_chapters
 * @brief Chapter URLs extracted from the current series page.
 * @details Filled by mdl_extract_anchors(), then filtered and ordered in place.
 * @warning Single-threaded; reused across series within one run.
 * @since 0.1.0
 */
static mdl_url_list_t s_chapters;

/**
 * @var s_selected
 * @brief The `--from`/`--chapters` window copied out of ::s_chapters.
 * @warning Single-threaded; rebuilt per series.
 * @since 0.1.0
 */
static mdl_url_list_t s_selected;

/**
 * @var s_images
 * @brief Page-image URLs extracted from the current chapter page.
 * @warning Single-threaded; overwritten per chapter.
 * @since 0.1.0
 */
static mdl_url_list_t s_images;

/**
 * @var s_state
 * @brief The persistent library state of the series being processed.
 * @details About 2 MiB (chapter table + page pool), so it lives in `.bss`.
 * @warning Single-threaded; reused across series within one run.
 * @since 0.1.0
 */
static mdl_state_t s_state;

/**
 * @var s_faillog
 * @brief The per-run failure log surfaced in the end-of-run summary.
 * @details About 135 KiB (a fixed failure table), so it lives in `.bss`. Cleared
 *          before each series run; the fetch loop only appends.
 * @warning Single-threaded; reused across series within one run.
 * @since 0.1.0
 */
static mdl_fetch_faillog_t s_faillog;

/**
 * @var s_series_metadata
 * @brief Metadata extracted from the current series page.
 * @details Rebuilt before chapter extraction and copied transactionally into
 *          ::s_state after the existing state has been loaded.
 * @warning Single-threaded; reused across series within one run.
 * @since 0.1.0
 */
static mdl_series_metadata_t s_series_metadata;

/**
 * @var s_rowtmp
 * @brief Scratch row used while reordering ::s_chapters in place.
 * @warning Single-threaded; holds no meaningful value between sorts.
 * @since 0.1.0
 */
static char s_rowtmp[k_mdl_url_max];

/**
 * @var s_results
 * @brief Titled search/browse hits parsed from a discovery results page.
 * @details About 98 KiB (a fixed title+URL table), so it lives in `.bss`. Only
 *          used before a download begins, so it never overlaps ::s_chapters.
 * @warning Single-threaded; rebuilt per discovery run.
 * @since 0.1.0
 */
static mdl_hit_list_t s_results;

/**
 * @brief Return the larger of two unsigned values.
 * @details Performs the comparison without arithmetic or narrowing.
 * @param[in] a First value.
 * @param[in] b Second value.
 * @return The larger input value.
 * @retval a @p a is greater than @p b.
 * @retval b @p b is greater than or equal to @p a.
 * @pre Both inputs are valid uint32_t values.
 * @pre No ordering relationship is required.
 * @post The result equals one input.
 * @post Neither input nor shared state is modified.
 * @note Thread-safe and side-effect free.
 * @since 0.1.0
 */
RA8_INTERNAL static uint32_t max_u32(uint32_t a, uint32_t b)
{
  return (a > b) ? a : b;
}

/**
 * @brief Swap two rows of a URL list.
 * @details Uses the fixed global row scratch so the bounded rows move intact.
 * @param[in,out] l URL list to mutate.
 * @param[in] a First row index.
 * @param[in] b Second row index.
 * @pre @p l is non-NULL and owns complete URL rows.
 * @pre @p a and @p b are smaller than `l->count`.
 * @post The selected rows exchange positions.
 * @post All other rows and `l->count` are unchanged.
 * @note Not thread-safe because it uses ::s_rowtmp.
 * @since 0.1.0
 */
RA8_INTERNAL static void swap_rows(mdl_url_list_t* l, size_t a, size_t b)
{
  if (a == b) {
    return;
  }
  memcpy(s_rowtmp, l->urls[a], k_mdl_url_max);
  memcpy(l->urls[a], l->urls[b], k_mdl_url_max);
  memcpy(l->urls[b], s_rowtmp, k_mdl_url_max);
}

/**
 * @brief Reverse a URL list in place.
 * @details Exchanges mirrored rows through ::swap_rows without allocating.
 * @param[in,out] l URL list to reverse.
 * @pre @p l is non-NULL.
 * @pre `l->count` does not exceed its fixed row capacity.
 * @post Row order is exactly reversed.
 * @post `l->count` and row contents remain otherwise unchanged.
 * @note Not thread-safe because ::swap_rows uses shared scratch.
 * @since 0.1.0
 */
RA8_INTERNAL static void reverse_list(mdl_url_list_t* l)
{
  for (size_t i = 0U; i < (l->count / 2U); ++i) {
    swap_rows(l, i, l->count - 1U - i);
  }
}

/**
 * @brief Sort a URL list by parsed chapter number.
 * @details Validates every numeric identifier before performing the in-place
 *          selection sort, preserving input order when validation fails.
 * @param[in,out] l URL list to validate and sort.
 * @return Whether every URL carried a numeric chapter identifier.
 * @retval true The list is sorted in ascending numeric order.
 * @retval false At least one identifier was missing and the list is unchanged.
 * @pre @p l is non-NULL.
 * @pre `l->count` fits its fixed row table.
 * @post Success orders every existing row without changing the count.
 * @post Failure preserves every row and its original position.
 * @note Not thread-safe because row swaps use shared scratch.
 * @since 0.1.0
 */
RA8_INTERNAL static bool sort_by_chapter_num(mdl_url_list_t* l)
{
  for (size_t i = 0U; i < l->count; ++i) {
    double ignored = 0.0;
    if (!mdl_urlname_chapter_parse(l->urls[i], &ignored)) {
      return false;
    }
  }
  for (size_t i = 0U; i + 1U < l->count; ++i) {
    size_t min = i;
    for (size_t j = i + 1U; j < l->count; ++j) {
      if (mdl_urlname_chapter_value(l->urls[j]) < mdl_urlname_chapter_value(l->urls[min])) {
        min = j;
      }
    }
    swap_rows(l, i, min);
  }
  return true;
}

/**
 * @brief Apply the configured chapter ordering in place.
 * @details Selects document order, reverse order, or validated numeric order.
 * @param[in,out] l URL list to reorder.
 * @param[in] order Validated descriptor ordering policy.
 * @pre @p l is non-NULL.
 * @pre @p order is a valid ::mdl_chapter_order_t value.
 * @post The requested supported ordering is applied.
 * @post Numeric parse failure leaves document order intact and emits a warning.
 * @note Not thread-safe because sorting uses shared row scratch.
 * @since 0.1.0
 */
RA8_INTERNAL static void apply_order(mdl_url_list_t* l, mdl_chapter_order_t order)
{
  if (order == k_mdl_order_reverse) {
    reverse_list(l);
  } else if (order == k_mdl_order_asc) {
    if (!sort_by_chapter_num(l)) {
      (void)fprintf(stderr,
                    "media_dl: WARNING: at least one chapter has no numeric identifier; "
                    "preserving site document order\n");
    }
  }
}

/**
 * @brief Keep only list entries whose URL starts with a prefix.
 * @details Compacts matching bounded rows in place while retaining their order.
 * @param[in,out] l URL list to filter.
 * @param[in] prefix NUL-terminated absolute URL prefix.
 * @pre @p l and @p prefix are non-NULL.
 * @pre Every populated row is NUL-terminated.
 * @post `l->count` equals the number of matching rows.
 * @post Surviving rows preserve their relative order.
 * @note Not thread-safe when callers concurrently mutate @p l.
 * @since 0.1.0
 */
RA8_INTERNAL static void filter_prefix(mdl_url_list_t* l, const char* prefix)
{
  const size_t plen = strlen(prefix);
  size_t       w    = 0U;
  for (size_t r = 0U; r < l->count; ++r) {
    if (strncmp(l->urls[r], prefix, plen) == 0) {
      if (w != r) {
        memcpy(l->urls[w], l->urls[r], k_mdl_url_max);
      }
      ++w;
    }
  }
  l->count = w;
}

/**
 * @brief Print the missing-operator-contact warning once.
 * @details Suppresses duplicate warnings across sessions within one process.
 * @pre Standard error is available for diagnostic output.
 * @pre The process uses the command-line single-threaded composition root.
 * @post The first call marks the warning as emitted.
 * @post Later calls produce no output.
 * @note Not thread-safe because the one-time flag is function-static.
 * @since 0.1.0
 */
RA8_INTERNAL static void warn_no_contact(void)
{
  static bool s_warned = false;
  if (!s_warned) {
    (void)fprintf(stderr,
                  "media_dl: WARNING: no contact configured; pass --contact "
                  "<email|url> so a site operator can reach you before banning\n");
    s_warned = true;
  }
}

/**
 * @brief Extract descriptor-driven series metadata from one fetched page.
 *
 * @details A missing/unmatched title falls back loudly to the stable URL leaf;
 * optional descriptive fields warn and remain empty. A configured cover is a
 * required contract: it must be found and resolve to an absolute URL, because
 * silently exporting a book without its promised cover is a false success.
 *
 * @param[in] site       Validated site descriptor.
 * @param[in] series_url Absolute URL used as the cover-resolution base.
 * @param[in] html       Complete fetched series HTML.
 * @param[in] len        Number of readable bytes at @p html.
 *
 * @return An ::ra8_err_t extraction result.
 * @retval k_ra8_ok Metadata was extracted, with documented optional fallbacks.
 * @retval k_ra8_err_invalid_size A selected value exceeded its bounded field.
 * @retval k_ra8_err_validation_failed A configured cover URL was unsafe.
 * @retval k_ra8_err_no_data A configured cover selector matched no value.
 *
 * @pre All pointer arguments are non-NULL and NUL-terminated where applicable.
 * @pre @p html addresses at least @p len readable bytes.
 * @post On success, ::s_series_metadata contains no stale prior-series values.
 * @post On failure, callers do not begin a chapter download.
 *
 * @note Not thread-safe: replaces process-global bounded metadata scratch.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t extract_series_metadata(const mdl_site_t* site,
                                                      const char*       series_url,
                                                      const char*       html,
                                                      size_t            len)
{
  memset(&s_series_metadata, 0, sizeof(s_series_metadata));
  s_series_metadata.direction =
    (strcmp(site->reading_direction, "rtl") == 0) ? k_mdl_state_read_rtl : k_mdl_state_read_ltr;
  const int language_len =
    snprintf(s_series_metadata.language, sizeof(s_series_metadata.language), "%s", site->language);
  if ((language_len < 0) || ((size_t)language_len >= sizeof(s_series_metadata.language))) {
    return k_ra8_err_invalid_size;
  }

  ra8_err_t rc = k_ra8_err_no_data;
  if (site->series_title_selector[0] != '\0') {
    rc                               = mdl_extract_selector(html,
                                                            len,
                                                            site->series_title_selector,
                                                            s_series_metadata.title,
                                                            sizeof(s_series_metadata.title));
    s_series_metadata.title_selected = rc == k_ra8_ok;
  }
  if (rc == k_ra8_err_invalid_size) {
    (void)fprintf(stderr, "media_dl: series title exceeds the bounded metadata field\n");
    return rc;
  }
  if (rc != k_ra8_ok) {
    mdl_urlname_last_segment(series_url, s_series_metadata.title, sizeof(s_series_metadata.title));
    (void)fprintf(stderr,
                  "media_dl: WARNING: series title selector '%s' failed; using URL leaf '%s'\n",
                  site->series_title_selector,
                  s_series_metadata.title);
  }

  const struct {
    const char* selector; /**< Validated descriptor selector. */
    char*       out;      /**< Bounded destination field.     */
    size_t      cap;      /**< Destination byte capacity.     */
    const char* label;    /**< Diagnostic field name.         */
  } optional[] = {{site->series_summary_selector,
                   s_series_metadata.summary,
                   sizeof(s_series_metadata.summary),
                   "summary"},
                  {site->series_author_selector,
                   s_series_metadata.writer,
                   sizeof(s_series_metadata.writer),
                   "author"},
                  {site->series_artist_selector,
                   s_series_metadata.artist,
                   sizeof(s_series_metadata.artist),
                   "artist"}};
  for (size_t i = 0U; i < (sizeof(optional) / sizeof(optional[0])); ++i) {
    if (optional[i].selector[0] == '\0') {
      continue;
    }
    rc = mdl_extract_selector(html, len, optional[i].selector, optional[i].out, optional[i].cap);
    if (rc == k_ra8_err_invalid_size) {
      (void)fprintf(stderr,
                    "media_dl: series %s exceeds the bounded metadata field\n",
                    optional[i].label);
      return rc;
    }
    if (rc != k_ra8_ok) {
      optional[i].out[0] = '\0';
      (void)fprintf(stderr,
                    "media_dl: WARNING: series %s selector '%s' matched no bounded value\n",
                    optional[i].label,
                    optional[i].selector);
    }
  }

  if (site->series_cover_selector[0] != '\0') {
    char cover_raw[k_mdl_url_max];
    rc = mdl_extract_selector(html, len, site->series_cover_selector, cover_raw, sizeof(cover_raw));
    if (rc != k_ra8_ok) {
      (void)fprintf(stderr,
                    "media_dl: required series cover selector '%s' failed\n",
                    site->series_cover_selector);
      return rc;
    }
    if (!mdl_extract_resolve_url(series_url,
                                 cover_raw,
                                 s_series_metadata.cover_url,
                                 sizeof(s_series_metadata.cover_url))) {
      (void)fprintf(stderr, "media_dl: series cover URL could not be resolved safely\n");
      return k_ra8_err_validation_failed;
    }
  }
  return k_ra8_ok;
}

/**
 * @brief Fetch a series page and build its ordered chapter list.
 * @details Enforces session policy, extracts descriptor metadata and anchors,
 *          then filters cross-series links and applies configured ordering.
 * @param[in] site Validated site descriptor.
 * @param[in] series_url Absolute series page URL.
 * @param[in] timeout Per-request timeout in milliseconds.
 * @return An ::ra8_err_t preparation result.
 * @retval k_ra8_ok Metadata and chapter rows were prepared.
 * @retval k_ra8_fail Session policy refused the series URL.
 * @retval k_ra8_err_invalid_size A configured prefix or extracted field exceeded its bound.
 * @retval other Fetch, extraction, validation, or ordering failed.
 * @pre @p site and @p series_url are non-NULL.
 * @pre ::s_session is initialized and @p timeout is nonzero.
 * @post Success replaces ::s_chapters and ::s_series_metadata.
 * @post Failure prevents the caller from starting chapter downloads.
 * @note Not thread-safe because it uses process-global bounded buffers.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t
prepare_chapters(const mdl_site_t* site, const char* series_url, uint32_t timeout)
{
  if (!mdl_session_url_allowed(&s_session, series_url, nullptr)) {
    return k_ra8_fail; /* robots refused the series page (message printed) */
  }
  const mdl_net_req_t req = {.user_agent = s_session.user_agent,
                             .referer    = nullptr,
                             .timeout_ms = timeout};
  size_t              len = 0U;
  ra8_err_t           rc =
    mdl_net_get_buf(s_session.net, series_url, &req, s_page, sizeof(s_page), &len, nullptr);
  if (rc != k_ra8_ok) {
    return rc;
  }
  rc = extract_series_metadata(site, series_url, s_page, len);
  if (rc != k_ra8_ok) {
    return rc;
  }
  rc = mdl_extract_anchors(s_page, len, series_url, site->chapter_url_contains, &s_chapters);
  if (rc != k_ra8_ok) {
    return rc;
  }

  /* Keep only this series' chapters (drop cross-series relation links). A
   * descriptor may supply a stable absolute chapter prefix when its episode
   * pages are siblings of, rather than descendants of, the index URL. */
  char prefix[k_dir_path_bytes];
  if (site->chapter_url_prefix[0] != '\0') {
    const int prefix_len = snprintf(prefix, sizeof(prefix), "%s", site->chapter_url_prefix);
    if ((prefix_len < 0) || ((size_t)prefix_len >= sizeof(prefix))) {
      return k_ra8_err_invalid_size;
    }
  } else {
    const int prefix_len = snprintf(prefix, sizeof(prefix), "%s", series_url);
    if ((prefix_len < 0) || ((size_t)prefix_len >= sizeof(prefix))) {
      return k_ra8_err_invalid_size;
    }
    size_t pl = strlen(prefix);
    if ((pl > 0U) && (prefix[pl - 1U] != '/') && (pl + 1U < sizeof(prefix))) {
      prefix[pl]      = '/';
      prefix[pl + 1U] = '\0';
    }
  }
  filter_prefix(&s_chapters, prefix);
  apply_order(&s_chapters, site->chapter_order);
  return k_ra8_ok;
}

/**
 * @brief Initialize the shared network session identity for one run.
 * @details Applies CLI-over-descriptor contact precedence and builds the
 *          bounded user agent before configuring robots enforcement.
 * @param[in,out] net Initialized network interface.
 * @param[in] opts Validated run policy.
 * @param[in] cfg_contact Optional descriptor contact string.
 * @param[out] ua User-agent buffer.
 * @param[in] ua_cap Writable capacity of @p ua.
 * @pre @p net, @p opts, and @p ua are non-NULL.
 * @pre @p ua_cap is the true writable user-agent capacity.
 * @post ::s_session references @p net and the completed identity.
 * @post Missing contact emits at most one process warning.
 * @note Not thread-safe because it replaces ::s_session.
 * @since 0.1.0
 */
RA8_INTERNAL static void start_session(mdl_net_iface_t*      net,
                                       const mdl_run_opts_t* opts,
                                       const char*           cfg_contact,
                                       char*                 ua,
                                       size_t                ua_cap)
{
  const char* contact = opts->contact;
  if ((contact == nullptr) && (cfg_contact != nullptr) && (cfg_contact[0] != '\0')) {
    contact = cfg_contact; /* CLI --contact overrides the site descriptor's key */
  }
  if (!mdl_session_build_ua(contact, ua, ua_cap)) {
    warn_no_contact();
  }
  mdl_session_init(&s_session, net, ua, opts->honor_robots);
}

/**
 * @brief Create and resolve a series directory beneath an output root.
 * @details Derives the bounded URL slug, joins it under the canonical output
 *          root, creates both directories as needed, rejects a symbolic-link
 *          leaf, and proves the resolved series path remains contained.
 * @param[in] out_dir Output library root.
 * @param[in] series_url Absolute series URL used to derive the slug.
 * @param[out] slug Destination for the series slug.
 * @param[in] slug_cap Writable capacity of @p slug.
 * @param[out] abs_dir Destination for the canonical directory path.
 * @return Whether the directory was safely prepared.
 * @retval true @p slug and @p abs_dir contain complete results.
 * @retval false Joining, creation, or canonicalization failed.
 * @pre All pointer arguments are non-NULL.
 * @pre Output buffers satisfy their documented bounded capacities.
 * @post Success leaves a real series directory strictly beneath the output root.
 * @post Failure is reported and callers do not use @p abs_dir.
 * @note Not safe for concurrent mutation of the same filesystem path.
 * @since 0.1.0
 */
RA8_INTERNAL static bool prepare_series_dir(const char* out_dir,
                                            const char* series_url,
                                            char*       slug,
                                            size_t      slug_cap,
                                            char*       abs_dir)
{
  mdl_urlname_last_segment(series_url, slug, slug_cap);
  char series_dir[k_dir_path_bytes];
  if (!mdl_path_join(out_dir, slug, series_dir, sizeof(series_dir))) {
    (void)fprintf(stderr, "media_dl: cannot build series dir for slug '%s'\n", slug);
    return false;
  }
  if ((mkdir(out_dir, (mode_t)k_dir_mode) != 0) && (errno != EEXIST)) {
    (void)
      fprintf(stderr, "media_dl: cannot create library root %s: %s\n", out_dir, strerror(errno));
    return false;
  }
  char        root_abs[PATH_MAX];
  struct stat root_state;
  if ((realpath(out_dir, root_abs) == nullptr) || (stat(root_abs, &root_state) != 0) ||
      !S_ISDIR(root_state.st_mode)) {
    (void)fprintf(stderr, "media_dl: library root is not a directory: %s\n", out_dir);
    return false;
  }
  if ((mkdir(series_dir, (mode_t)k_dir_mode) != 0) && (errno != EEXIST)) {
    (void)
      fprintf(stderr, "media_dl: cannot create series dir %s: %s\n", series_dir, strerror(errno));
    return false;
  }
  struct stat leaf_state;
  if ((lstat(series_dir, &leaf_state) != 0) || !S_ISDIR(leaf_state.st_mode)) {
    (void)fprintf(stderr, "media_dl: refusing unsafe series path: %s\n", series_dir);
    return false;
  }
  if (realpath(series_dir, abs_dir) == nullptr) {
    (void)fprintf(stderr, "media_dl: cannot resolve %s\n", series_dir);
    return false;
  }
  struct stat dir_state;
  if ((stat(abs_dir, &dir_state) != 0) || !S_ISDIR(dir_state.st_mode) ||
      (strcmp(root_abs, abs_dir) == 0) || !mdl_path_contained(root_abs, abs_dir)) {
    (void)fprintf(stderr, "media_dl: series path escapes library root: %s\n", abs_dir);
    return false;
  }
  return true;
}

/**
 * @brief Determine the inclusive numeric range of a chapter list.
 * @details Parses every URL before updating the caller's minimum and maximum.
 * @param[in] l Chapter URL list.
 * @param[out] lo Minimum parsed chapter number.
 * @param[out] hi Maximum parsed chapter number.
 * @return Whether a complete nonempty numeric range was available.
 * @retval true @p lo and @p hi hold the complete range.
 * @retval false The list was empty or a URL lacked a number.
 * @pre @p l, @p lo, and @p hi are non-NULL.
 * @pre Every populated URL row is NUL-terminated.
 * @post Outputs are initialized even when the list is empty.
 * @post The input list is never modified.
 * @note Thread-safe for distinct caller-owned inputs and outputs.
 * @since 0.1.0
 */
RA8_INTERNAL static bool list_range(const mdl_url_list_t* l, double* lo, double* hi)
{
  *lo = 0.0;
  *hi = 0.0;
  if (l->count == 0U) {
    return false;
  }
  if (!mdl_urlname_chapter_parse(l->urls[0], lo)) {
    return false;
  }
  *hi = *lo;
  for (size_t i = 1U; i < l->count; ++i) {
    double num = 0.0;
    if (!mdl_urlname_chapter_parse(l->urls[i], &num)) {
      return false;
    }
    *lo = (num < *lo) ? num : *lo;
    *hi = (num > *hi) ? num : *hi;
  }
  return true;
}

/**
 * @brief Select a bounded chapter window from the prepared list.
 * @details Finds the first requested numeric chapter when present and copies
 *          at most @p count complete URL rows into caller output.
 * @param[in] from_present Whether @p from_num constrains the first row.
 * @param[in] from_num Minimum numeric chapter when enabled.
 * @param[in] count Maximum rows to select.
 * @param[out] out Selected URL list.
 * @pre @p out is non-NULL.
 * @pre ::s_chapters contains validated bounded rows.
 * @post `out->count` never exceeds @p count or the list capacity.
 * @post ::s_chapters remains unchanged.
 * @note Not thread-safe because it reads shared ::s_chapters.
 * @since 0.1.0
 */
RA8_INTERNAL static void
select_window(bool from_present, double from_num, size_t count, mdl_url_list_t* out)
{
  size_t start = 0U;
  if (from_present) {
    while (start < s_chapters.count) {
      double value = 0.0;
      if (mdl_urlname_chapter_parse(s_chapters.urls[start], &value) && (value >= from_num)) {
        break;
      }
      ++start;
    }
  }
  out->count = 0U;
  for (size_t i = start; (i < s_chapters.count) && (out->count < count); ++i) {
    memcpy(out->urls[out->count], s_chapters.urls[i], k_mdl_url_max);
    out->count += 1U;
  }
}

/**
 * @brief Compose the absolute `.mdl_state` path for a series.
 * @details Uses checked formatting and rejects every truncated result.
 * @param[in] abs_dir Canonical series directory.
 * @param[out] out Destination path buffer.
 * @param[in] cap Writable capacity of @p out.
 * @return Whether the complete state path fit.
 * @retval true @p out contains the NUL-terminated path.
 * @retval false Formatting failed or required at least @p cap bytes.
 * @pre @p abs_dir and @p out are non-NULL.
 * @pre @p cap is the true capacity of @p out.
 * @post Success appends exactly `/.mdl_state`.
 * @post No filesystem object is created or modified.
 * @note Thread-safe for distinct output buffers.
 * @since 0.1.0
 */
RA8_INTERNAL static bool state_path_of(const char* abs_dir, char* out, size_t cap)
{
  const int n = snprintf(out, cap, "%s/.mdl_state", abs_dir);
  if (n < 0) {
    return false;
  }
  return (size_t)n < cap;
}

/**
 * @brief Copy one complete string into a fixed metadata field.
 * @details Uses a checked bounded write and clears the destination on overflow,
 *          so callers can never consume a plausible-looking truncated field.
 *
 * @param[out] dst Destination field.
 * @param[in]  cap Writable byte capacity of @p dst.
 * @param[in]  src NUL-terminated source.
 *
 * @return Whether the complete source fit.
 * @retval true  @p dst contains the complete source.
 * @retval false An argument was invalid or truncation would occur.
 *
 * @pre @p dst and @p src are non-NULL.
 * @pre @p cap is the true writable capacity of @p dst.
 * @post On true, @p dst is NUL-terminated and equals @p src.
 * @post On false with nonzero @p cap, @p dst is an empty string.
 *
 * @note Thread-safe: writes only caller-owned storage.
 * @since 0.1.0
 */
RA8_INTERNAL static bool copy_metadata_text(char* dst, size_t cap, const char* src)
{
  if ((dst == nullptr) || (cap == 0U) || (src == nullptr)) {
    return false;
  }
  const int n = snprintf(dst, cap, "%s", src);
  if ((n < 0) || ((size_t)n >= cap)) {
    dst[0] = '\0';
    return false;
  }
  return true;
}

/**
 * @brief Convert persisted series/chapter state into exporter metadata.
 *
 * @details The series source URL is copied without truncation and the local
 * cover leaf is resolved beneath the already-canonical series directory. A
 * deterministic identifier folds the series URL and, for separate output, the
 * chapter URL; the exporter derives modified time from verified page files.
 *
 * @param[in]  abs_dir Canonical absolute series directory.
 * @param[in]  rec     Chapter record, or NULL for a combined series container.
 * @param[out] meta    Exporter metadata to initialise and populate.
 *
 * @return Whether every persisted field and composed path fit exactly.
 * @retval true  @p meta is ready for a bounded exporter call.
 * @retval false A field/path exceeded the corresponding exporter contract.
 *
 * @pre @p abs_dir and @p meta are non-NULL.
 * @pre ::s_state has passed state validation and @p rec belongs to it or is NULL.
 * @post On true, no exporter field contains a silently truncated value.
 * @post No filesystem object or persistent state is modified.
 *
 * @note Not thread-safe: reads process-global ::s_state.
 * @since 0.1.0
 */
RA8_INTERNAL static bool
build_export_metadata(const char* abs_dir, const mdl_chapter_rec_t* rec, mdl_export_meta_t* meta)
{
  mdl_meta_init(meta);
  if (!copy_metadata_text(meta->series_title, sizeof(meta->series_title), s_state.series_title) ||
      !copy_metadata_text(meta->summary, sizeof(meta->summary), s_state.summary) ||
      !copy_metadata_text(meta->writer, sizeof(meta->writer), s_state.writer) ||
      !copy_metadata_text(meta->artist, sizeof(meta->artist), s_state.artist) ||
      !copy_metadata_text(meta->source_url, sizeof(meta->source_url), s_state.series_url) ||
      !copy_metadata_text(meta->language, sizeof(meta->language), s_state.language)) {
    return false;
  }
  meta->reading_direction =
    (s_state.reading_direction == k_mdl_state_read_rtl) ? k_mdl_read_rtl : k_mdl_read_ltr;
  if (s_state.cover_path[0] != '\0') {
    if (!mdl_path_join(abs_dir, s_state.cover_path, meta->cover_path, sizeof(meta->cover_path))) {
      return false;
    }
  }
  uint64_t identity = mdl_hash_str(s_state.series_url);
  if (rec != nullptr) {
    if (!copy_metadata_text(meta->chapter_title, sizeof(meta->chapter_title), rec->title)) {
      return false;
    }
    meta->chapter_number = rec->number_known ? rec->number : 0.0;
    identity             = mdl_hash_bytes_seed(rec->source_url, strlen(rec->source_url), identity);
  }
  const int id_len =
    snprintf(meta->identifier, sizeof(meta->identifier), "urn:media-dl:%016" PRIx64, identity);
  return (id_len >= 0) && ((size_t)id_len < sizeof(meta->identifier));
}

/**
 * @brief Export every newly completed selected chapter separately.
 * @details Resolves each selected URL to persisted state and exports only
 *          complete records fetched during the current run.
 * @param[in] format Requested supported export format.
 * @param[in] abs_dir Canonical series directory.
 * @param[in] sel Selected chapter URL list.
 * @param[in] run_start Current-run timestamp boundary.
 * @return Number of metadata or exporter failures.
 * @retval 0 Every eligible chapter exported successfully.
 * @retval positive One or more eligible chapters failed.
 * @pre @p abs_dir and @p sel are non-NULL.
 * @pre ::s_state and ::s_export_ws are initialized.
 * @post Ineligible or older chapters are not exported.
 * @post Persistent chapter state is unchanged.
 * @note Not thread-safe because it uses shared state and workspace.
 * @since 0.1.0
 */
RA8_INTERNAL static size_t export_fresh_separate(mdl_format_t          format,
                                                 const char*           abs_dir,
                                                 const mdl_url_list_t* sel,
                                                 int64_t               run_start)
{
  size_t fails = 0U;
  for (size_t i = 0U; i < sel->count; ++i) {
    char id[k_mdl_chapter_id_max];
    mdl_urlname_last_segment(sel->urls[i], id, sizeof(id));
    const mdl_chapter_rec_t* rec = mdl_state_find_chapter(&s_state, id);
    if ((rec != nullptr) && rec->complete && (rec->fetched_at >= run_start)) {
      mdl_export_meta_t meta;
      if (!build_export_metadata(abs_dir, rec, &meta)) {
        (void)fprintf(stderr, "  export metadata for %s exceeded a bounded field\n", id);
        fails += 1U;
      } else {
        fails += mdl_pack_one_meta(format, abs_dir, id, &meta, &s_export_ws);
      }
    }
  }
  return fails;
}

/**
 * @brief Print the per-run download tally.
 * @details Emits chapter and page outcome counts with their output directory.
 * @param[in] abs_dir Canonical series output directory.
 * @param[in] st Completed fetch statistics.
 * @pre @p abs_dir and @p st are non-NULL.
 * @pre Standard output is available.
 * @post Exactly one summary line is attempted.
 * @post @p st and filesystem state remain unchanged.
 * @note Output from concurrent callers may interleave.
 * @since 0.1.0
 */
RA8_INTERNAL static void report_stats(const char* abs_dir, const mdl_fetch_stats_t* st)
{
  (void)printf("done into %s/: %zu chapter(s) fetched, %zu skipped, %zu failed; "
               "pages %zu new / %zu reused / %zu failed\n",
               abs_dir,
               st->chapters_completed,
               st->chapters_skipped,
               st->chapters_failed,
               st->pages_fetched,
               st->pages_reused,
               st->pages_failed);
}

/**
 * @struct series_run_t
 * @brief The parameters of one series download, bundled for the run entry point.
 * @details Keeps ::run_series to a single options pointer rather than a long,
 *          error-prone positional parameter list (the library `--update-all`
 *          path reuses the same struct).
 * @since 0.1.0
 */
typedef struct {
  const char*           cfg_path;     /**< Site descriptor path.          */
  const char*           series_url;   /**< Series page URL.               */
  const char*           out_dir;      /**< Output library root.           */
  mdl_format_t          format;       /**< Output container.              */
  bool                  combine;      /**< Combine into one archive.      */
  bool                  update;       /**< Incremental (skip complete).   */
  bool                  from_present; /**< Whether @ref from_num applies. */
  double                from_num;     /**< First chapter number to fetch. */
  size_t                chapters;     /**< Max chapters (window mode).    */
  uint64_t              seed;         /**< Politeness jitter seed.        */
  uint32_t              timeout;      /**< Per-request budget, ms.        */
  const mdl_run_opts_t* opts;         /**< Identity/security knobs.       */
} series_run_t;

/**
 * @brief Choose output layout and its combined directory name.
 * @details Forces separate layout for updates, loose output, or explicit
 *          separation; otherwise names the combined range deterministically.
 * @param[in] r Validated series-run parameters.
 * @param[in] sel Selected chapter URLs.
 * @param[out] combined_rel Combined relative directory leaf.
 * @param[in] cap Writable capacity of @p combined_rel.
 * @param[in] slug Sanitized series slug.
 * @return Selected ::mdl_fetch_layout_t value.
 * @retval k_mdl_layout_separate Chapters use individual directories.
 * @retval k_mdl_layout_combined Chapters share the named directory.
 * @pre All pointer arguments are non-NULL.
 * @pre @p cap is the true output capacity and inputs are bounded.
 * @post Separate layout clears @p combined_rel.
 * @post Combined layout writes a deterministic NUL-terminated leaf.
 * @note Thread-safe for distinct output buffers.
 * @since 0.1.0
 */
RA8_INTERNAL static mdl_fetch_layout_t choose_layout(const series_run_t*   r,
                                                     const mdl_url_list_t* sel,
                                                     char*                 combined_rel,
                                                     size_t                cap,
                                                     const char*           slug)
{
  combined_rel[0] = '\0';
  if (r->update || !r->combine || (r->format == k_mdl_fmt_loose)) {
    return k_mdl_layout_separate;
  }
  double lo = 0.0;
  double hi = 0.0;
  if (list_range(sel, &lo, &hi)) {
    (void)snprintf(combined_rel, cap, "%s-%.10g-%.10g", slug, lo, hi);
  } else {
    (void)snprintf(combined_rel, cap, "%s-%zu-chapters", slug, sel->count);
  }
  return k_mdl_layout_combined;
}

/**
 * @brief Assemble a fetch context over the shared bounded buffers.
 * @details Connects validated run/site inputs to session, state, governor,
 *          scratch tables, failure log, and the requested progress callback.
 * @param[in] r Validated series-run parameters.
 * @param[in] site Validated site descriptor.
 * @param[in] abs_dir Canonical series directory.
 * @param[in] state_path Complete state-file path.
 * @param[in,out] gov Initialized request governor.
 * @return Fully populated fetch context by value.
 * @retval mdl_fetch_ctx_t Context referencing the supplied and shared storage.
 * @pre All pointer arguments are non-NULL and outlive the fetch run.
 * @pre ::s_session and shared bounded buffers are initialized.
 * @post The returned context owns no dynamically allocated memory.
 * @post No request or filesystem mutation has yet occurred.
 * @note Not thread-safe because the context references process-global storage.
 * @since 0.1.0
 */
RA8_INTERNAL static mdl_fetch_ctx_t make_ctx(const series_run_t* r,
                                             const mdl_site_t*   site,
                                             const char*         abs_dir,
                                             const char*         state_path,
                                             mdl_governor_t*     gov)
{
  return (mdl_fetch_ctx_t){.session        = &s_session,
                           .storage        = &s_storage,
                           .state          = &s_state,
                           .state_path     = state_path,
                           .series_abs_dir = abs_dir,
                           .series_url     = r->series_url,
                           .site           = site,
                           .gov            = gov,
                           .timeout_ms     = r->timeout,
                           .page_buf       = s_page,
                           .page_cap       = sizeof(s_page),
                           .images         = &s_images,
                           .update_only    = r->update,
                           .refetch        = r->opts->refetch,
                           .faillog        = &s_faillog,
                           .progress_fn =
                             r->opts->progress ? mdl_report_progress_bar : mdl_report_progress,
                           .progress_ctx = nullptr};
}

/**
 * @brief Ensure state names a verified, magic-typed local series cover.
 *
 * @details Reuses a previously recorded cover only when its bytes still have a
 * supported image signature. Otherwise the descriptor URL is fetched through
 * the normal robots/SSRF/governor/retry path to an atomic holding leaf, typed
 * from bytes (never a URL suffix), and durably renamed to `cover.<true-ext>`.
 *
 * @param[in,out] ctx     Fully configured fetch context.
 * @param[in]     abs_dir Canonical absolute series directory.
 *
 * @return An ::ra8_err_t cover result.
 * @retval k_ra8_ok No cover is configured, or a verified local cover exists.
 * @retval k_ra8_err_validation_failed Downloaded bytes are not a supported image.
 * @retval k_ra8_err_invalid_size A bounded path or state field could not fit.
 * @retval k_ra8_fail Fetching, publication, or state update failed.
 *
 * @pre @p ctx and @p abs_dir are non-NULL and @p abs_dir is canonical.
 * @pre ::s_state contains the current descriptor metadata tuple.
 * @post On success with a cover URL, `s_state.cover_path` names verified bytes.
 * @post On failure, no partial cover file remains and chapter fetching has not begun.
 *
 * @note Not thread-safe: mutates ::s_state and uses the shared session/governor.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t ensure_series_cover(mdl_fetch_ctx_t* ctx, const char* abs_dir)
{
  if (s_state.cover_url[0] == '\0') {
    return k_ra8_ok;
  }
  if (s_state.cover_path[0] != '\0') {
    char existing[PATH_MAX];
    if (mdl_path_join(abs_dir, s_state.cover_path, existing, sizeof(existing)) &&
        mdl_urlname_sniff_file(existing, nullptr, nullptr, 0U, nullptr, 0U)) {
      return k_ra8_ok;
    }
  }

  char holding[PATH_MAX];
  if (!mdl_path_join(abs_dir, "cover.download", holding, sizeof(holding))) {
    return k_ra8_err_invalid_size;
  }
  mdl_net_resp_t response = {};
  size_t         bytes    = 0U;
  ra8_err_t      rc =
    mdl_fetch_asset(ctx, s_state.cover_url, holding, s_state.series_url, &response, &bytes);
  if (rc != k_ra8_ok) {
    return rc;
  }
  char true_ext[k_ext_bytes];
  if (!mdl_urlname_sniff_file(holding, nullptr, true_ext, sizeof(true_ext), nullptr, 0U)) {
    mdl_atomic_abort(holding);
    (void)fprintf(stderr,
                  "media_dl: downloaded series cover is not a supported image (%zu bytes)\n",
                  bytes);
    return k_ra8_err_validation_failed;
  }
  char      leaf[k_leaf_name_bytes];
  const int leaf_len = snprintf(leaf, sizeof(leaf), "cover.%s", true_ext);
  char      final_path[PATH_MAX];
  if ((leaf_len < 0) || ((size_t)leaf_len >= sizeof(leaf)) ||
      !mdl_path_join(abs_dir, leaf, final_path, sizeof(final_path))) {
    mdl_atomic_abort(holding);
    return k_ra8_err_invalid_size;
  }
  if (!mdl_atomic_commit(holding, final_path)) {
    return k_ra8_fail;
  }
  if (!copy_metadata_text(s_state.cover_path, sizeof(s_state.cover_path), leaf)) {
    return k_ra8_fail;
  }
  return k_ra8_ok;
}

/**
 * @brief Package freshly downloaded output according to the selected layout.
 * @details Skips loose output, exports one combined container when requested,
 *          or delegates current-run chapters to separate packaging.
 * @param[in] r Validated series-run parameters.
 * @param[in] abs_dir Canonical series directory.
 * @param[in] layout Completed fetch layout.
 * @param[in] combined_rel Combined directory leaf when applicable.
 * @param[in] sel Selected chapter URLs.
 * @param[in] stats Completed fetch statistics.
 * @param[in] run_start Current-run timestamp boundary.
 * @return Number of packaging failures.
 * @retval 0 Packaging succeeded or loose output required none.
 * @retval positive Metadata or exporter work failed.
 * @pre All pointer arguments are non-NULL.
 * @pre ::s_state and ::s_export_ws are initialized.
 * @post Successful container outputs are atomically published.
 * @post A metadata overflow is reported as an export failure.
 * @note Not thread-safe because it uses shared exporter storage.
 * @since 0.1.0
 */
RA8_INTERNAL static size_t export_after(const series_run_t*      r,
                                        const char*              abs_dir,
                                        mdl_fetch_layout_t       layout,
                                        const char*              combined_rel,
                                        const mdl_url_list_t*    sel,
                                        const mdl_fetch_stats_t* stats,
                                        int64_t                  run_start)
{
  if (r->format == k_mdl_fmt_loose) {
    return 0U;
  }
  if (layout == k_mdl_layout_combined) {
    mdl_export_meta_t meta;
    if (!build_export_metadata(abs_dir, nullptr, &meta)) {
      (void)fprintf(stderr, "  combined export metadata exceeded a bounded field\n");
      return 1U;
    }
    return mdl_pack_combined_meta(r->format,
                                  r->opts->allow_incomplete,
                                  abs_dir,
                                  combined_rel,
                                  stats,
                                  &meta,
                                  &s_export_ws);
  }
  return export_fresh_separate(r->format, abs_dir, sel, run_start);
}

/**
 * @brief Drive one prepared series through fetch, checkpoint, and export.
 * @details Selects the requested window, creates the fetch context, acquires a
 *          verified cover, saves state around the fetch, and packages results.
 * @param[in] r Validated series-run parameters.
 * @param[in] site Validated site descriptor.
 * @param[in] abs_dir Canonical series directory.
 * @param[in] state_path Complete state-file path.
 * @return Process-style status.
 * @retval 0 Fetch, state saves, and eligible exports all succeeded.
 * @retval 1 Selection, cover, fetch, state, or export work failed.
 * @pre All pointer arguments are non-NULL.
 * @pre Chapters and series state have been prepared.
 * @post A successful run durably checkpoints current state.
 * @post Failures are reported and never presented as successful export.
 * @note Not thread-safe because it owns the shared run composition state.
 * @since 0.1.0
 */
RA8_INTERNAL static int run_prepared(const series_run_t* r,
                                     const mdl_site_t*   site,
                                     const char*         abs_dir,
                                     const char*         state_path)
{
  const mdl_url_list_t* sel = &s_chapters;
  if (!r->update) {
    select_window(r->from_present, r->from_num, r->chapters, &s_selected);
    sel = &s_selected;
    if (sel->count == 0U) {
      (void)fprintf(stderr, "media_dl: no chapters match the requested range\n");
      return 1;
    }
  }
  char display_slug[k_slug_bytes];
  (void)mdl_sanitize_segment(s_state.series_title, display_slug, sizeof(display_slug));
  char               combined_rel[k_leaf_name_bytes];
  mdl_fetch_layout_t layout =
    choose_layout(r, sel, combined_rel, sizeof(combined_rel), display_slug);

  mdl_governor_t      gov;
  const mdl_gov_cfg_t cfg = mdl_config_gov_cfg(site);
  mdl_governor_init(&gov, &cfg, r->seed);
  memset(&s_faillog, 0, sizeof(s_faillog));
  mdl_fetch_ctx_t ctx = make_ctx(r, site, abs_dir, state_path, &gov);
  const ra8_err_t crc = ensure_series_cover(&ctx, abs_dir);
  if (crc != k_ra8_ok) {
    (void)fprintf(stderr, "media_dl: series cover acquisition failed (err 0x%X)\n", (unsigned)crc);
    return 1;
  }
  if (mdl_state_save(state_path, &s_state) != k_ra8_ok) {
    (void)fprintf(stderr, "media_dl: failed to checkpoint series metadata '%s'\n", state_path);
    return 1;
  }
  const int64_t     run_start = (int64_t)time(nullptr);
  mdl_fetch_stats_t stats;
  const ra8_err_t   frc = mdl_fetch_run(&ctx,
                                        sel,
                                        layout,
                                        (layout == k_mdl_layout_combined) ? combined_rel : nullptr,
                                        &stats);
  const ra8_err_t   src = mdl_state_save(state_path, &s_state);
  if (src != k_ra8_ok) {
    (void)fprintf(stderr, "media_dl: failed to save library state '%s'\n", state_path);
  }
  report_stats(abs_dir, &stats);
  mdl_report_failures(&s_faillog);
  const size_t efail = export_after(r, abs_dir, layout, combined_rel, sel, &stats, run_start);
  return ((frc == k_ra8_ok) && (src == k_ra8_ok) && (efail == 0U)) ? 0 : 1;
}

/**
 * @brief Execute series mode from descriptor through download.
 * @details Loads policy, establishes networking, prepares chapters and the
 *          tracked directory, reconciles metadata, then runs the orchestrator.
 * @param[in] r Complete validated series-run parameters.
 * @return Process-style status.
 * @retval 0 Series processing completed without a reported failure.
 * @retval 1 Configuration, network, state, fetch, or export work failed.
 * @pre @p r and all required strings/options are non-NULL.
 * @pre The CLI has validated mode-specific argument combinations.
 * @post The network interface is destroyed on every initialized path.
 * @post Unreadable state is never silently overwritten.
 * @note Not thread-safe because it uses process-global run storage.
 * @since 0.1.0
 */
RA8_INTERNAL static int run_series(const series_run_t* r)
{
  mdl_site_t site;
  if (mdl_config_load(r->cfg_path, &site) != k_ra8_ok) {
    return 1;
  }
  if (r->opts->polite) {
    mdl_config_apply_polite(&site);
  }
  (void)printf("site: %s (host %s, kind %s)\n", site.name, site.host, site.kind);

  mdl_net_iface_t        net     = {};
  mdl_net_curl_storage_t storage = {};
  if (mdl_net_curl_init(&net, &storage, &r->opts->policy) != k_ra8_ok) {
    (void)fprintf(stderr, "media_dl: network init failed\n");
    return 1;
  }
  char ua[k_mdl_ua_max];
  start_session(&net, r->opts, site.contact, ua, sizeof(ua));

  int rc = 1;
  if (prepare_chapters(&site, r->series_url, r->timeout) != k_ra8_ok) {
    (void)fprintf(stderr, "media_dl: series fetch failed\n");
  } else if (s_chapters.count == 0U) {
    (void)fprintf(stderr, "media_dl: no chapters (check chapter_url_contains)\n");
  } else {
    (void)printf("chapters found: %zu\n", s_chapters.count);
    char slug[k_slug_bytes];
    char abs_dir[PATH_MAX];
    char state_path[PATH_MAX];
    if (prepare_series_dir(r->out_dir, r->series_url, slug, sizeof(slug), abs_dir) &&
        state_path_of(abs_dir, state_path, sizeof(state_path))) {
      const ra8_err_t load_rc = mdl_state_load(state_path, &s_state);
      if (load_rc != k_ra8_ok) {
        (void)fprintf(stderr,
                      "media_dl: refusing to overwrite unreadable state '%s' (err 0x%X)\n",
                      state_path,
                      (unsigned)load_rc);
      } else {
        char prior_title[k_mdl_title_max];
        char prior_summary[k_mdl_summary_max];
        char prior_writer[k_mdl_person_max];
        char prior_artist[k_mdl_person_max];
        char prior_cover_path[k_mdl_relpath_max];
        (void)snprintf(prior_title, sizeof(prior_title), "%s", s_state.series_title);
        (void)snprintf(prior_summary, sizeof(prior_summary), "%s", s_state.summary);
        (void)snprintf(prior_writer, sizeof(prior_writer), "%s", s_state.writer);
        (void)snprintf(prior_artist, sizeof(prior_artist), "%s", s_state.artist);
        (void)snprintf(
          prior_cover_path,
          sizeof(prior_cover_path),
          "%s",
          (strcmp(s_state.cover_url, s_series_metadata.cover_url) == 0) ? s_state.cover_path : "");
        const char* title = s_series_metadata.title;
        if (!s_series_metadata.title_selected && (prior_title[0] != '\0')) {
          title = prior_title;
          (void)fprintf(
            stderr,
            "media_dl: WARNING: preserving recorded series title after selector failure\n");
        }
        const char* summary =
          (s_series_metadata.summary[0] != '\0') ? s_series_metadata.summary : prior_summary;
        const char* writer =
          (s_series_metadata.writer[0] != '\0') ? s_series_metadata.writer : prior_writer;
        const char* artist =
          (s_series_metadata.artist[0] != '\0') ? s_series_metadata.artist : prior_artist;
        mdl_state_set_series(&s_state, r->series_url, title, site.name, site.host, r->cfg_path);
        if (!mdl_state_set_series_metadata(&s_state,
                                           summary,
                                           writer,
                                           artist,
                                           s_series_metadata.cover_url,
                                           prior_cover_path,
                                           s_series_metadata.language,
                                           s_series_metadata.direction)) {
          (void)fprintf(stderr, "media_dl: scraped series metadata exceeded state bounds\n");
        } else {
          rc = run_prepared(r, &site, abs_dir, state_path);
        }
      }
    }
  }
  mdl_net_destroy(&net);
  return rc;
}

typedef struct {
  size_t found;      /**< Tracked series encountered.      */
  size_t unreadable; /**< Invalid state files encountered. */
} list_ctx_t;

/**
 * @brief Print one tracked series for library-list mode.
 * @details Loads its state and prints the canonical directory, source URL, and
 *          coverage, while recording unreadable state as a nonfatal visit.
 * @param[in] series_dir Canonical tracked-series directory.
 * @param[in] state_path Complete `.mdl_state` path.
 * @param[in,out] ctx Pointer to a ::list_ctx_t accumulator.
 * @return Iterator continuation result.
 * @retval k_ra8_ok Continue enumeration, including after unreadable state.
 * @pre All pointer arguments are non-NULL.
 * @pre @p ctx points to writable ::list_ctx_t storage.
 * @post `found` increases exactly once.
 * @post Unreadable state increases `unreadable` and is labeled in output.
 * @note Not thread-safe because state loading uses ::s_state.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t list_cb(const char* series_dir, const char* state_path, void* ctx)
{
  list_ctx_t* list = (list_ctx_t*)ctx;
  list->found += 1U;
  if (mdl_state_load(state_path, &s_state) != k_ra8_ok) {
    (void)printf("  %s\n    (state file unreadable)\n", series_dir);
    list->unreadable += 1U;
    return k_ra8_ok;
  }
  char cov[k_cov_bytes];
  mdl_state_coverage(&s_state, cov, sizeof(cov));
  (void)printf("  %s\n    url: %s\n    %s\n",
               series_dir,
               (s_state.series_url[0] != '\0') ? s_state.series_url : "(unknown)",
               cov);
  return k_ra8_ok;
}

/**
 * @brief Enumerate tracked series under an output root.
 * @details Visits library markers through ::list_cb and distinguishes an empty
 *          library from traversal or state-read failures.
 * @param[in] out_dir Library root to enumerate.
 * @return Process-style status.
 * @retval 0 Enumeration completed and every state file was readable.
 * @retval 1 Traversal failed or at least one state file was unreadable.
 * @pre @p out_dir is non-NULL and NUL-terminated.
 * @pre Standard output is available.
 * @post Every discovered tracked series is reported once.
 * @post No library data is modified.
 * @note Not thread-safe because callbacks use shared state scratch.
 * @since 0.1.0
 */
RA8_INTERNAL static int run_list(const char* out_dir)
{
  list_ctx_t      list = {};
  const ra8_err_t rc   = mdl_library_for_each(out_dir, list_cb, &list);
  if (list.found == 0U) {
    (void)printf("no tracked series under %s\n", out_dir);
  }
  return ((rc == k_ra8_ok) && (list.unreadable == 0U)) ? 0 : 1;
}

/**
 * @brief Remove one explicitly tracked series directory.
 * @details Derives the bounded slug, rejects symbolic-link targets, parses the
 *          regular `.mdl_state` marker, and requires its recorded series URL to
 *          resolve to the same slug before delegating recursive removal.
 * @param[in] out_dir Library root.
 * @param[in] url_or_slug Series URL or slug selected by the user.
 * @return Process-style status.
 * @retval 0 The tracked tree was removed.
 * @retval 1 Resolution, marker validation, or removal failed.
 * @pre @p out_dir and @p url_or_slug are non-NULL.
 * @pre CLI validation authorized remove mode.
 * @post Untracked, corrupt, mismatched, or symbolic-link directories are never removed.
 * @post Success prints the removed directory path.
 * @note Not safe for concurrent mutation of the same tree.
 * @since 0.1.0
 */
RA8_INTERNAL static int run_remove(const char* out_dir, const char* url_or_slug)
{
  char slug[k_slug_bytes];
  mdl_urlname_last_segment(url_or_slug, slug, sizeof(slug));
  char dir[k_dir_path_bytes];
  if (!mdl_path_join(out_dir, slug, dir, sizeof(dir))) {
    (void)fprintf(stderr, "media_dl: cannot resolve series '%s'\n", url_or_slug);
    return 1;
  }
  struct stat dir_st;
  if ((lstat(dir, &dir_st) != 0) || !S_ISDIR(dir_st.st_mode)) {
    (void)fprintf(stderr, "media_dl: refusing to remove unsafe series path '%s'\n", dir);
    return 1;
  }
  char        state_path[PATH_MAX];
  struct stat state_st;
  if (!state_path_of(dir, state_path, sizeof(state_path)) || (lstat(state_path, &state_st) != 0) ||
      !S_ISREG(state_st.st_mode)) {
    (void)fprintf(stderr,
                  "media_dl: refusing to remove untracked directory '%s' "
                  "(missing or unsafe .mdl_state)\n",
                  dir);
    return 1;
  }
  if (mdl_state_load(state_path, &s_state) != k_ra8_ok) {
    (void)fprintf(stderr,
                  "media_dl: refusing to remove '%s' (state is unreadable or corrupt)\n",
                  dir);
    return 1;
  }
  char recorded_slug[k_slug_bytes];
  mdl_urlname_last_segment(s_state.series_url, recorded_slug, sizeof(recorded_slug));
  if ((s_state.series_url[0] == '\0') || (strcmp(recorded_slug, slug) != 0)) {
    (void)fprintf(stderr,
                  "media_dl: refusing to remove '%s' (state identity does not match target)\n",
                  dir);
    return 1;
  }
  if (mdl_library_remove_tree(dir) != k_ra8_ok) {
    (void)fprintf(stderr, "media_dl: failed to remove %s\n", dir);
    return 1;
  }
  (void)printf("removed %s\n", dir);
  return 0;
}

/**
 * @struct update_all_ctx_t
 * @brief Context threaded through ::update_all_cb for `--update-all`.
 * @since 0.1.0
 */
typedef struct {
  const series_run_t* base;    /**< Template run (format/knobs); url filled per series. */
  size_t              updated; /**< Count of series updated.                            */
  size_t              failed;  /**< Count of series skipped or unsuccessfully updated.  */
} update_all_ctx_t;

/**
 * @brief Incrementally update one visited tracked series.
 * @details Loads persisted URL/config identity, derives a run from the caller's
 *          template, and records success or a nonfatal per-series failure.
 * @param[in] series_dir Canonical tracked-series directory.
 * @param[in] state_path Complete `.mdl_state` path.
 * @param[in,out] ctx Pointer to an ::update_all_ctx_t accumulator.
 * @return Iterator continuation result.
 * @retval k_ra8_ok Continue enumeration after either outcome.
 * @pre All pointer arguments are non-NULL.
 * @pre @p ctx points to a valid template and writable counters.
 * @post Exactly one of `updated` or `failed` increases.
 * @post Unreadable or incomplete identity is never overwritten.
 * @note Not thread-safe because it invokes ::run_series with shared state.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t
update_all_cb(const char* series_dir, const char* state_path, void* ctx)
{
  update_all_ctx_t* p = (update_all_ctx_t*)ctx;
  if (mdl_state_load(state_path, &s_state) != k_ra8_ok) {
    (void)printf("skip %s (state file unreadable)\n", series_dir);
    p->failed += 1U;
    return k_ra8_ok;
  }
  char url[k_mdl_url_max];
  char cfg[k_mdl_cfgpath_max];
  (void)snprintf(url, sizeof(url), "%s", s_state.series_url);
  (void)snprintf(cfg,
                 sizeof(cfg),
                 "%s",
                 (s_state.config_path[0] != '\0') ? s_state.config_path : p->base->cfg_path);
  if ((url[0] == '\0') || (cfg[0] == '\0')) {
    (void)printf("skip %s (no series URL / descriptor recorded)\n", series_dir);
    p->failed += 1U;
    return k_ra8_ok;
  }
  (void)printf("updating %s\n", url);
  series_run_t run = *p->base;
  run.series_url   = url;
  run.cfg_path     = cfg;
  run.update       = true;
  if (run_series(&run) == 0) {
    p->updated += 1U;
  } else {
    p->failed += 1U;
  }
  return k_ra8_ok;
}

/**
 * @brief Incrementally update every tracked series.
 * @details Enumerates the library with a fixed run template and summarizes any
 *          series that could not be loaded or updated.
 * @param[in] base Template series-run parameters and library root.
 * @return Process-style status.
 * @retval 0 Traversal and every attempted update succeeded.
 * @retval 1 Traversal or at least one series update failed.
 * @pre @p base is non-NULL and contains a valid output root.
 * @pre Shared composition-root buffers are initialized.
 * @post Every tracked series is visited at most once.
 * @post Failures remain visible in the returned status and diagnostic output.
 * @note Not thread-safe because updates reuse global state.
 * @since 0.1.0
 */
RA8_INTERNAL static int run_update_all(const series_run_t* base)
{
  update_all_ctx_t c  = {.base = base, .updated = 0U, .failed = 0U};
  const ra8_err_t  rc = mdl_library_for_each(base->out_dir, update_all_cb, &c);
  if (c.updated == 0U) {
    (void)printf("no tracked series to update under %s\n", base->out_dir);
  }
  if (c.failed > 0U) {
    (void)fprintf(stderr, "media_dl: %zu series failed to update\n", c.failed);
  }
  return ((rc == k_ra8_ok) && (c.failed == 0U)) ? 0 : 1;
}

/**
 * @brief Remove stale page files whose extensions differ from verified bytes.
 * @details Enumerates the bounded supported image extensions, constructs each
 *          sibling leaf with checked formatting/joining, and unlinks every
 *          alternate while preserving the just-published true extension.
 * @param[in] out_dir Output directory containing page-mode files.
 * @param[in] page_no One-based page number used in the fixed leaf pattern.
 * @param[in] true_ext Verified extension that must remain present.
 * @pre @p out_dir and @p true_ext are non-NULL and NUL-terminated.
 * @pre @p true_ext names one supported image extension.
 * @post Every resolvable alternate page leaf has been unlinked or reported.
 * @post The leaf ending in @p true_ext is never removed.
 * @note Not safe for concurrent writers targeting the same page number.
 * @since 0.1.0
 */
RA8_INTERNAL static void
remove_stale_page_variants(const char* out_dir, size_t page_no, const char* true_ext)
{
  static const char* const alternate_exts[] = {"jpg", "jpeg", "png", "webp", "gif", "bmp"};
  for (size_t ext_idx = 0U; ext_idx < (sizeof(alternate_exts) / sizeof(alternate_exts[0]));
       ++ext_idx) {
    if (strcmp(alternate_exts[ext_idx], true_ext) == 0) {
      continue;
    }
    char      alternate_leaf[k_leaf_name_bytes] = {};
    const int alternate_len                     = snprintf(alternate_leaf,
                                                           sizeof(alternate_leaf),
                                                           "page_%04zu.%s",
                                                           page_no,
                                                           alternate_exts[ext_idx]);
    char      alternate_path[k_file_path_bytes];
    if ((alternate_len <= 0) || ((size_t)alternate_len >= sizeof(alternate_leaf)) ||
        !mdl_path_join(out_dir, alternate_leaf, alternate_path, sizeof(alternate_path))) {
      (void)fprintf(stderr,
                    "media_dl: warning: cannot resolve stale page variant '%s'\n",
                    alternate_leaf);
      continue;
    }
    if ((unlink(alternate_path) != 0) && (errno != ENOENT)) {
      (void)fprintf(stderr,
                    "media_dl: warning: cannot remove stale page variant '%s': %s\n",
                    alternate_path,
                    strerror(errno));
    }
  }
}

/**
 * @brief Gate, space, and download one page-mode image.
 * @details Enforces session policy and crawl delay, downloads to a holding
 *          leaf, requires a supported byte signature, then durably publishes
 *          `page_NNNN.<true-ext>`. URL suffixes and Content-Type cannot override
 *          the bytes, and no partial/unknown file is presented as an image.
 * @param[in] url Referrer page URL.
 * @param[in] out_dir Output directory.
 * @param[in] dmin Minimum request delay in milliseconds.
 * @param[in] dmax Maximum request delay in milliseconds.
 * @param[in] timeout Per-request timeout in milliseconds.
 * @param[in,out] pol Initialized politeness state.
 * @param[in] idx Index into ::s_images.
 * @return Failure contribution for the page tally.
 * @retval 0 The image downloaded successfully.
 * @retval 1 Policy refusal or network/file failure occurred.
 * @pre @p url, @p out_dir, and @p pol are non-NULL.
 * @pre @p idx is smaller than `s_images.count`.
 * @post Success leaves one image file in @p out_dir.
 * @post Failure is reported and contributes exactly one failure.
 * @note Not thread-safe because it reads the shared session/image table.
 * @since 0.1.0
 */
RA8_INTERNAL static size_t download_page_image(const char*       url,
                                               const char*       out_dir,
                                               uint32_t          dmin,
                                               uint32_t          dmax,
                                               uint32_t          timeout,
                                               mdl_politeness_t* pol,
                                               size_t            idx)
{
  uint32_t crawl = 0U;
  if (!mdl_session_url_allowed(&s_session, s_images.urls[idx], &crawl)) {
    return 1U;
  }
  (void)mdl_politeness_wait(pol, max_u32(dmin, crawl), max_u32(dmax, crawl));
  char      holding_leaf[k_leaf_name_bytes];
  const int holding_len =
    snprintf(holding_leaf, sizeof(holding_leaf), "page_%04zu.download", idx + 1U);
  char holding[k_file_path_bytes];
  if ((holding_len < 0) || ((size_t)holding_len >= sizeof(holding_leaf)) ||
      !mdl_path_join(out_dir, holding_leaf, holding, sizeof(holding))) {
    return 1U;
  }
  const mdl_net_req_t ir   = {.user_agent = s_session.user_agent,
                              .referer    = url,
                              .timeout_ms = timeout};
  size_t              got  = 0U;
  mdl_net_resp_t      resp = {};
  const ra8_err_t     rc =
    mdl_net_get_file(s_session.net, s_images.urls[idx], &ir, holding, &got, &resp);
  if (rc != k_ra8_ok) {
    mdl_atomic_abort(holding);
    char reason[k_mdl_reason_max];
    mdl_fetch_reason(rc, resp.status, reason, sizeof(reason));
    (void)fprintf(stderr, "  page %zu FAILED %s -- %s\n", idx + 1U, s_images.urls[idx], reason);
    return 1U;
  }
  char true_ext[k_ext_bytes];
  if (!mdl_urlname_sniff_file(holding, nullptr, true_ext, sizeof(true_ext), nullptr, 0U)) {
    mdl_atomic_abort(holding);
    (void)fprintf(stderr,
                  "  page %zu FAILED %s -- unsupported image signature\n",
                  idx + 1U,
                  s_images.urls[idx]);
    return 1U;
  }
  char      final_leaf[k_leaf_name_bytes];
  const int final_len =
    snprintf(final_leaf, sizeof(final_leaf), "page_%04zu.%s", idx + 1U, true_ext);
  char final_path[k_file_path_bytes];
  if ((final_len < 0) || ((size_t)final_len >= sizeof(final_leaf)) ||
      !mdl_path_join(out_dir, final_leaf, final_path, sizeof(final_path))) {
    mdl_atomic_abort(holding);
    return 1U;
  }
  if (!mdl_atomic_commit(holding, final_path)) {
    (void)fprintf(stderr, "  page %zu FAILED -- atomic publication failed\n", idx + 1U);
    return 1U;
  }
  remove_stale_page_variants(out_dir, idx + 1U, true_ext);
  return 0U;
}

/**
 * @brief Download the extracted page images into an output directory.
 * @details Initializes deterministic delay state, applies the configured page
 *          limit, downloads each row, and prints the final tally.
 * @param[in] url Referrer page URL.
 * @param[in] out_dir Output directory.
 * @param[in] max_imgs Maximum images, or zero for every extracted row.
 * @param[in] seed Politeness jitter seed.
 * @param[in] timeout Per-request timeout in milliseconds.
 * @param[in] polite Whether conservative delay bounds apply.
 * @return Number of refused or failed images.
 * @retval 0 Every attempted image succeeded.
 * @retval positive One or more attempted images failed.
 * @pre @p url and @p out_dir are non-NULL.
 * @pre ::s_session and ::s_images are prepared.
 * @post At most @p max_imgs rows are attempted when it is nonzero.
 * @post A complete success/failure tally is printed.
 * @note Not thread-safe because it uses shared image/session state.
 * @since 0.1.0
 */
RA8_INTERNAL static size_t download_page_images(const char* url,
                                                const char* out_dir,
                                                uint32_t    max_imgs,
                                                uint64_t    seed,
                                                uint32_t    timeout,
                                                bool        polite)
{
  mdl_politeness_t pol;
  mdl_politeness_init(&pol, seed);
  const uint32_t dmin  = polite ? (uint32_t)k_polite_img_min_ms : (uint32_t)k_page_img_delay_min;
  const uint32_t dmax  = polite ? (uint32_t)k_polite_img_max_ms : (uint32_t)k_page_img_delay_max;
  const size_t   limit = (max_imgs == 0U) ? s_images.count : (size_t)max_imgs;
  size_t         fail  = 0U;
  for (size_t i = 0U; (i < s_images.count) && (i < limit); ++i) {
    fail += download_page_image(url, out_dir, dmin, dmax, timeout, &pol, i);
  }
  (void)printf("done: %zu ok, %zu failed, into %s/\n",
               (s_images.count < limit ? s_images.count : limit) - fail,
               fail,
               out_dir);
  return fail;
}

/**
 * @brief Download, validate, and atomically publish one artifact.
 * @details Accepts only formats with structural validators, stages the network
 *          response, validates through the reader path, then durably commits it.
 * @param[in] url HTTPS artifact URL.
 * @param[in] out_dir Output directory.
 * @param[in] timeout Per-request timeout in milliseconds.
 * @param[in] opts Validated network and identity policy.
 * @return Process-style status.
 * @retval 0 A structurally valid artifact was published.
 * @retval 1 Format, path, network, validation, or commit failed.
 * @pre All pointer arguments are non-NULL.
 * @pre CLI validation authorized direct-artifact mode.
 * @post Success exposes only a structurally validated final file.
 * @post Failure aborts the reserved staging path.
 * @note Not thread-safe because validation uses ::s_export_ws.
 * @since 0.1.0
 */
RA8_INTERNAL static int
run_artifact(const char* url, const char* out_dir, uint32_t timeout, const mdl_run_opts_t* opts)
{
  char leaf[k_leaf_name_bytes];
  mdl_urlname_last_segment(url, leaf, sizeof(leaf));
  mdl_format_t format = k_mdl_fmt_invalid;
  if ((mdl_format_from_path(leaf, &format) != k_ra8_ok) || !mdl_format_is_verifiable(format)) {
    (void)fprintf(stderr,
                  "media_dl: direct artifact '%s' has no supported structural validator "
                  "(cbz|cbt|cbt.gz|epub|jof)\n",
                  leaf);
    return 1;
  }
  if ((mkdir(out_dir, (mode_t)k_dir_mode) != 0) && (errno != EEXIST)) {
    (void)fprintf(stderr, "media_dl: cannot create output directory '%s'\n", out_dir);
    return 1;
  }
  char out_abs[PATH_MAX];
  char final_path[PATH_MAX];
  if ((realpath(out_dir, out_abs) == nullptr) ||
      !mdl_path_join(out_abs, leaf, final_path, sizeof(final_path))) {
    (void)fprintf(stderr, "media_dl: unsafe or unresolved output directory '%s'\n", out_dir);
    return 1;
  }

  mdl_net_iface_t        net     = {};
  mdl_net_curl_storage_t storage = {};
  if (mdl_net_curl_init(&net, &storage, &opts->policy) != k_ra8_ok) {
    (void)fprintf(stderr, "media_dl: network init failed\n");
    return 1;
  }
  char ua[k_mdl_ua_max];
  start_session(&net, opts, nullptr, ua, sizeof(ua));
  if (!mdl_session_url_allowed(&s_session, url, nullptr)) {
    mdl_net_destroy(&net);
    return 1;
  }

  char staged[PATH_MAX];
  if (!mdl_atomic_tmp_path(final_path, staged, sizeof(staged))) {
    (void)fprintf(stderr, "media_dl: cannot create staging path for '%s'\n", final_path);
    mdl_net_destroy(&net);
    return 1;
  }
  const mdl_net_req_t req      = {.user_agent = s_session.user_agent,
                                  .referer    = nullptr,
                                  .timeout_ms = timeout};
  size_t              got      = 0U;
  mdl_net_resp_t      resp     = {};
  const ra8_err_t     fetch_rc = mdl_net_get_file(&net, url, &req, staged, &got, &resp);
  mdl_net_destroy(&net);
  if (fetch_rc != k_ra8_ok) {
    char reason[k_mdl_reason_max];
    mdl_fetch_reason(fetch_rc, resp.status, reason, sizeof(reason));
    (void)fprintf(stderr, "media_dl: artifact fetch failed -- %s\n", reason);
    mdl_atomic_abort(staged);
    return 1;
  }
  mdl_verify_report_t report    = {};
  const ra8_err_t     verify_rc = mdl_verify_file(format, staged, &s_export_ws, &report);
  if (verify_rc != k_ra8_ok) {
    (void)fprintf(stderr, "media_dl: downloaded artifact failed structural validation\n");
    mdl_atomic_abort(staged);
    return 1;
  }
  if (!mdl_atomic_commit(staged, final_path)) {
    (void)fprintf(stderr, "media_dl: could not publish verified artifact '%s'\n", final_path);
    return 1;
  }
  (void)printf("downloaded and verified %s (%zu bytes, %zu page(s))\n",
               final_path,
               got,
               report.page_count);
  return 0;
}

/**
 * @brief Fetch one page and download its extracted image URLs.
 * @details Initializes the network session, enforces URL policy, extracts the
 *          selected image attribute, and downloads the bounded image set.
 * @param[in] url Absolute page URL.
 * @param[in] out_dir Output directory.
 * @param[in] attr Image attribute selector.
 * @param[in] max_imgs Maximum images, or zero for all extracted rows.
 * @param[in] seed Politeness jitter seed.
 * @param[in] timeout Per-request timeout in milliseconds.
 * @param[in] opts Validated run policy.
 * @return Process-style status.
 * @retval 0 Every attempted image succeeded.
 * @retval 1 Network, policy, or image download failed.
 * @pre All pointer arguments are non-NULL.
 * @pre CLI validation authorized page mode.
 * @post The network interface is destroyed on every initialized path.
 * @post Download failures remain visible in the returned status.
 * @note Not thread-safe because it uses global extraction buffers.
 * @since 0.1.0
 */
RA8_INTERNAL static int run_page(const char*           url,
                                 const char*           out_dir,
                                 const char*           attr,
                                 uint32_t              max_imgs,
                                 uint64_t              seed,
                                 uint32_t              timeout,
                                 const mdl_run_opts_t* opts)
{
  if ((mkdir(out_dir, (mode_t)k_dir_mode) != 0) && (errno != EEXIST)) {
    (void)fprintf(stderr, "media_dl: cannot create output directory '%s'\n", out_dir);
    return 1;
  }
  char        out_abs[PATH_MAX];
  struct stat out_state;
  if ((realpath(out_dir, out_abs) == nullptr) || (stat(out_abs, &out_state) != 0) ||
      !S_ISDIR(out_state.st_mode)) {
    (void)fprintf(stderr, "media_dl: output path is not a directory: '%s'\n", out_dir);
    return 1;
  }
  mdl_net_iface_t        net     = {};
  mdl_net_curl_storage_t storage = {};
  if (mdl_net_curl_init(&net, &storage, &opts->policy) != k_ra8_ok) {
    (void)fprintf(stderr, "media_dl: network init failed\n");
    return 1;
  }
  char ua[k_mdl_ua_max];
  start_session(&net, opts, nullptr, ua, sizeof(ua));

  if (!mdl_session_url_allowed(&s_session, url, nullptr)) {
    mdl_net_destroy(&net);
    return 1;
  }
  const mdl_net_req_t req  = {.user_agent = s_session.user_agent,
                              .referer    = nullptr,
                              .timeout_ms = timeout};
  size_t              len  = 0U;
  mdl_net_resp_t      resp = {};
  ra8_err_t rc = mdl_net_get_buf(s_session.net, url, &req, s_page, sizeof(s_page), &len, &resp);
  if (rc != k_ra8_ok) {
    char reason[k_mdl_reason_max];
    mdl_fetch_reason(rc, resp.status, reason, sizeof(reason));
    (void)fprintf(stderr, "media_dl: fetch failed -- %s\n", reason);
    mdl_net_destroy(&net);
    return 1;
  }
  rc = mdl_extract_images(s_page, len, url, attr, nullptr, &s_images);
  if ((rc != k_ra8_ok) || (s_images.count == 0U)) {
    (void)fprintf(stderr, "media_dl: page contains no supported image URLs\n");
    mdl_net_destroy(&net);
    return 1;
  }
  (void)printf("found %zu image(s)\n", s_images.count);

  const size_t fail = download_page_images(url, out_abs, max_imgs, seed, timeout, opts->polite);
  mdl_net_destroy(&net);
  return (fail == 0U) ? 0 : 1;
}

/**
 * @brief Package an existing image directory in a supported format.
 * @details Canonicalizes the source, distinguishes directory-output JOF from
 *          file containers, and delegates to the bounded exporter workspace.
 * @param[in] dir Existing image directory.
 * @param[in] format Validated output format.
 * @return Process-style status.
 * @retval 0 Packaging completed successfully.
 * @retval 1 Path resolution or export failed.
 * @retval 2 The requested format is absent or invalid for pack mode.
 * @pre @p dir is non-NULL and NUL-terminated.
 * @pre ::s_export_ws is initialized.
 * @post Successful file containers use the canonical source path plus suffix.
 * @post Export errors are reported with their exact format.
 * @note Not thread-safe because it uses the shared exporter workspace.
 * @since 0.1.0
 */
RA8_INTERNAL static int run_pack(const char* dir, mdl_format_t format)
{
  if ((format == k_mdl_fmt_loose) || (format == k_mdl_fmt_invalid)) {
    (void)fprintf(stderr,
                  "media_dl: --pack needs a --format "
                  "(cbz|cbt|cbt.gz|epub|jof)\n");
    return 2;
  }
  char abs[PATH_MAX];
  if (realpath(dir, abs) == nullptr) {
    (void)fprintf(stderr, "media_dl: cannot resolve '%s'\n", dir);
    return 1;
  }
  const char* ext = mdl_format_ext(format);
  if (mdl_format_is_dir_output(format)) {
    /* JOF writes per-page `.jof` siblings into the packed directory itself;
     * report that directory rather than a container file it never creates. */
    const ra8_err_t drc = mdl_export_chapter_ws(format, abs, abs, &s_export_ws);
    if (drc != k_ra8_ok) {
      (void)
        fprintf(stderr, "media_dl: pack '%s' as .%s FAILED (err 0x%X)\n", dir, ext, (unsigned)drc);
      return 1;
    }
    (void)printf("packed %s -> %s/*.%s\n", dir, abs, ext);
    return 0;
  }
  char      out[PATH_MAX];
  const int n = snprintf(out, sizeof(out), "%s.%s", abs, ext);
  if ((n < 0) || ((size_t)n >= sizeof(out))) {
    (void)fprintf(stderr, "media_dl: output path for '%s' is too long\n", dir);
    return 1;
  }
  const ra8_err_t rc = mdl_export_chapter_ws(format, abs, out, &s_export_ws);
  if (rc != k_ra8_ok) {
    (void)fprintf(stderr, "media_dl: pack '%s' as .%s FAILED (err 0x%X)\n", dir, ext, (unsigned)rc);
    return 1;
  }
  (void)printf("packed %s -> %s\n", dir, out);
  return 0;
}

/**
 * @brief Assemble a series-run descriptor from validated CLI state.
 * @details Binds pointer options and parsed numeric values into one immutable
 *          value passed through series, discovery, and library modes.
 * @param[in] a Parsed command-line arguments.
 * @param[in] format Validated output format.
 * @param[in] opts Validated run policy.
 * @param[in] n Validated numeric arguments.
 * @return Fully populated ::series_run_t value.
 * @retval series_run_t Value referencing the caller-owned argument storage.
 * @pre @p a, @p opts, and @p n are non-NULL.
 * @pre Referenced strings outlive all dispatched run functions.
 * @post The result contains no dynamic ownership.
 * @post Input objects remain unchanged.
 * @note Thread-safe for independent input objects.
 * @since 0.1.0
 */
RA8_INTERNAL static series_run_t
build_run(const mdl_args_t* a, mdl_format_t format, const mdl_run_opts_t* opts, const mdl_nums_t* n)
{
  return (series_run_t){.cfg_path     = a->cfg,
                        .series_url   = a->series,
                        .out_dir      = a->out,
                        .format       = format,
                        .combine      = !a->separate,
                        .update       = a->update,
                        .from_present = n->from_present,
                        .from_num     = n->from_num,
                        .chapters     = n->chapters,
                        .seed         = n->seed,
                        .timeout      = n->timeout,
                        .opts         = opts};
}

/**
 * @brief Run search or browse discovery and optionally download one result.
 * @details Loads the descriptor, establishes policy/governor state, lists
 *          bounded hits, and feeds an explicitly selected URL to series mode.
 * @param[in] a Parsed command-line arguments.
 * @param[in] opts Validated run policy.
 * @param[in] n Validated numeric arguments.
 * @param[in] base Template series-run parameters.
 * @return Process-style status from discovery or the selected series run.
 * @retval 0 Results were listed or the selected series succeeded.
 * @retval nonzero Descriptor, network, discovery, selection, or series work failed.
 * @pre All pointer arguments are non-NULL.
 * @pre Search/browse mode and descriptor requirements passed CLI validation.
 * @post The discovery network interface is destroyed before return.
 * @post A picked URL is downloaded only when selection produced a complete URL.
 * @note Not thread-safe because it uses shared discovery buffers.
 * @since 0.1.0
 */
RA8_INTERNAL static int run_discover(const mdl_args_t*     a,
                                     const mdl_run_opts_t* opts,
                                     const mdl_nums_t*     n,
                                     const series_run_t*   base)
{
  if (a->cfg == nullptr) {
    (void)fprintf(stderr, "media_dl: --search/--browse requires --config SITE.conf\n");
    return 2;
  }
  mdl_site_t site;
  if (mdl_config_load(a->cfg, &site) != k_ra8_ok) {
    return 1;
  }
  if (opts->polite) {
    mdl_config_apply_polite(&site);
  }
  mdl_net_iface_t        net     = {};
  mdl_net_curl_storage_t storage = {};
  if (mdl_net_curl_init(&net, &storage, &opts->policy) != k_ra8_ok) {
    (void)fprintf(stderr, "media_dl: network init failed\n");
    return 1;
  }
  char ua[k_mdl_ua_max];
  start_session(&net, opts, site.contact, ua, sizeof(ua));

  mdl_governor_t      gov;
  const mdl_gov_cfg_t cfg = mdl_config_gov_cfg(&site);
  mdl_governor_init(&gov, &cfg, n->seed);

  const mdl_discover_req_t req = {
    .session    = &s_session,
    .gov        = &gov,
    .site       = &site,
    .mode       = a->browse ? k_mdl_discover_browse : k_mdl_discover_search,
    .term       = a->search,
    .timeout_ms = n->timeout,
    .page_buf   = s_page,
    .page_cap   = sizeof(s_page),
    .hits       = &s_results,
  };
  char      picked[k_mdl_url_max] = {};
  const int rc                    = mdl_discover_run(&req, n->pick, picked, sizeof(picked));
  mdl_net_destroy(&net);
  if ((rc != 0) || (picked[0] == '\0')) {
    return rc; /* listed results, or an error, with nothing to download */
  }
  series_run_t run = *base;
  run.series_url   = picked; /* combined search-and-select: feed the pick in */
  return run_series(&run);
}

/**
 * @brief Dispatch one validated library command.
 * @details Selects list, remove, or update-all and enforces the descriptor
 *          requirement that remains specific to updating.
 * @param[in] a Parsed command-line arguments.
 * @param[in] run Template series-run parameters.
 * @return Process-style status from the selected library operation.
 * @retval 0 The selected operation completed successfully.
 * @retval 1 Library traversal/removal/update failed.
 * @retval 2 Update-all lacked a required descriptor.
 * @pre @p a and @p run are non-NULL.
 * @pre Exactly one library mode was selected by validation.
 * @post Exactly one library operation is invoked.
 * @post Nonselected library state is not modified.
 * @note Not thread-safe because update-all reuses shared state.
 * @since 0.1.0
 */
RA8_INTERNAL static int run_library(const mdl_args_t* a, const series_run_t* run)
{
  if (a->list) {
    return run_list(a->out);
  }
  if (a->remove_series != nullptr) {
    return run_remove(a->out, a->remove_series);
  }
  if (a->cfg == nullptr) {
    (void)fprintf(stderr, "media_dl: --update-all requires --config SITE.conf\n");
    return 2;
  }
  return run_update_all(run);
}

/**
 * @brief Generate a starter site descriptor from one URL.
 * @details Extracts and normalizes the host into bounded name/path fields,
 *          writes a commented conservative descriptor to a sibling temp, and
 *          publishes it with a no-replace hard link.
 * @param[in] url Absolute site URL.
 * @return Process-style status.
 * @retval 0 The descriptor template was created.
 * @retval 1 The destination file could not be created.
 * @retval 2 The URL was absent or its host could not be extracted.
 * @pre @p url is either NULL or points to a NUL-terminated string.
 * @pre CLI validation selected init-site mode.
 * @post Success creates one template without dynamic allocation.
 * @post Failure reports the rejected URL or destination and preserves any
 *       existing descriptor byte-for-byte.
 * @note Not safe for concurrent creation of the same descriptor path.
 * @since 0.1.0
 */
RA8_INTERNAL static int run_init_site(const char* url)
{
  if ((url == nullptr) || (url[0] == '\0')) {
    (void)fprintf(stderr, "media_dl: --init-site requires a URL\n");
    return 2;
  }
  char host[k_mdl_host_max] = {};
  if (!mdl_url_host(url, host, sizeof(host))) {
    (void)fprintf(stderr, "media_dl: --init-site could not extract host from '%s'\n", url);
    return 2;
  }

  const char* hptr = host;
  if (strncmp(hptr, "www.", 4) == 0) {
    hptr += 4;
  }

  char   slug[k_mdl_name_max] = {};
  size_t i                    = 0U;
  for (; (hptr[i] != '\0') && (hptr[i] != '.') && (i + 1U < sizeof(slug)); ++i) {
    slug[i] = hptr[i];
  }
  slug[i] = '\0';
  if (slug[0] == '\0') {
    (void)snprintf(slug, sizeof(slug), "site");
  }

  char name[k_mdl_name_max] = {};
  (void)snprintf(name, sizeof(name), "%s", slug);
  if ((name[0] >= 'a') && (name[0] <= 'z')) {
    name[0] = (char)(name[0] - 'a' + 'A');
  }

  char        out_path[PATH_MAX];
  struct stat st;
  if ((stat("sites", &st) == 0) && S_ISDIR(st.st_mode)) {
    (void)snprintf(out_path, sizeof(out_path), "sites/%s.conf", slug);
  } else {
    (void)snprintf(out_path, sizeof(out_path), "%s.conf", slug);
  }

  char tmp_path[PATH_MAX];
  if (!mdl_atomic_tmp_path(out_path, tmp_path, sizeof(tmp_path))) {
    (void)fprintf(stderr, "media_dl: failed to stage site descriptor '%s'\n", out_path);
    return 1;
  }
  FILE* fp = fopen(tmp_path, "wb");
  if (fp == nullptr) {
    mdl_atomic_abort(tmp_path);
    (void)fprintf(stderr, "media_dl: failed to create site descriptor '%s'\n", out_path);
    return 1;
  }

  const int wrote    = fprintf(fp,
                               "# Copyright (c) 2026 Brighton Sikarskie\n"
                               "# SPDX-License-Identifier: MIT\n"
                               "#\n"
                               "# Site descriptor for %s (%s).\n"
                               "# Generated by media_dl --init-site\n"
                               "\n"
                               "name = %s\n"
                               "host = %s\n"
                               "kind = manga\n"
                               "\n"
                               "# --- chapter list (on a series page) ---\n"
                               "chapter_url_contains = /chapter-\n"
                               "# chapter_url_prefix = https://%s/path/to/series/chapters/\n"
                               "chapter_order = asc\n"
                               "\n"
                               "# --- search / discovery ---\n"
                               "search_url = https://%s/?s={q}\n"
                               "search_result_contains = /manga/\n"
                               "browse_url = https://%s/\n"
                               "\n"
                               "# --- page images (on a chapter page) ---\n"
                               "page_img_attr = data-src\n"
                               "page_img_url_contains = /uploads/\n"
                               "\n"
                               "# --- politeness (milliseconds; jittered in [min,max]) ---\n"
                               "img_delay_min = 500\n"
                               "img_delay_max = 1200\n"
                               "chapter_delay_min = 1500\n"
                               "chapter_delay_max = 3000\n",
                               hptr,
                               url,
                               name,
                               host,
                               host,
                               host,
                               host);
  bool      write_ok = (wrote >= 0) && (fflush(fp) == 0);
  if (write_ok) {
    const int fd = fileno(fp);
    write_ok     = (fd >= 0) && (fsync(fd) == 0);
  }
  write_ok = (fclose(fp) == 0) && write_ok;
  if (!write_ok) {
    mdl_atomic_abort(tmp_path);
    (void)fprintf(stderr, "media_dl: failed to write site descriptor '%s'\n", out_path);
    return 1;
  }
  if (link(tmp_path, out_path) != 0) {
    const int publish_errno = errno;
    mdl_atomic_abort(tmp_path);
    if (publish_errno == EEXIST) {
      (void)fprintf(stderr,
                    "media_dl: refusing to overwrite existing site descriptor '%s'\n",
                    out_path);
    } else {
      (void)fprintf(stderr, "media_dl: failed to publish site descriptor '%s'\n", out_path);
    }
    return 1;
  }
  mdl_atomic_abort(tmp_path);

  (void)printf("generated starter site descriptor template: %s\n", out_path);
  (void)printf("edit %s to tune scraping selectors, then test with:\n", out_path);
  (void)printf("  media_dl --config %s --series %s\n", out_path, url);
  return 0;
}

/** @brief Stats accumulated during a library/series verification run. */
typedef struct {
  size_t series_checked;   /**< Tracked series verified.               */
  size_t pages_valid;      /**< Pages matching content hash on disk.   */
  size_t pages_missing;    /**< Page files missing from disk.          */
  size_t pages_corrupt;    /**< Page files failing hash match.         */
  size_t archives_checked; /**< Archive files checked.                 */
  size_t archives_valid;   /**< Structurally valid artifacts.          */
  size_t archives_corrupt; /**< Archive files empty/corrupt.           */
  size_t state_errors;     /**< State files that could not be parsed.  */
  size_t fs_errors;        /**< Directory/path operations that failed. */
} verify_stats_t;

/**
 * @brief Structurally validate recognized artifacts in one directory.
 * @details Infers supported formats from complete suffixes, rejects nonregular
 *          candidates, and validates contents through format reader paths.
 * @param[in] dir Canonical directory to scan.
 * @param[in,out] st Verification counters to update.
 * @pre @p dir and @p st are non-NULL.
 * @pre ::s_export_ws is initialized for bounded validation scratch.
 * @post Every recognized directory entry increments `archives_checked` once.
 * @post Each checked artifact increments exactly one outcome counter.
 * @note Not thread-safe because it uses the shared exporter workspace.
 * @since 0.1.0
 */
RA8_INTERNAL static void verify_artifacts(const char* dir, verify_stats_t* st)
{
  DIR* d = opendir(dir);
  if (d == nullptr) {
    (void)printf("  [VERIFY FAIL] cannot open %s\n", dir);
    st->fs_errors += 1U;
    return;
  }
  for (;;) {
    errno                    = 0;
    const struct dirent* ent = readdir(d);
    if (ent == nullptr) {
      if (errno != 0) {
        (void)printf("  [VERIFY FAIL] cannot finish reading %s\n", dir);
        st->fs_errors += 1U;
      }
      break;
    }
    if (ent->d_name[0] == '.') {
      continue;
    }
    mdl_format_t format = k_mdl_fmt_invalid;
    if (mdl_format_from_path(ent->d_name, &format) != k_ra8_ok) {
      continue;
    }
    st->archives_checked += 1U;
    char        path[PATH_MAX];
    const int   n = snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);
    struct stat sb;
    if ((n < 0) || ((size_t)n >= sizeof(path)) || (lstat(path, &sb) != 0) || !S_ISREG(sb.st_mode)) {
      (void)printf("  [CORRUPT ARTIFACT] %s (not a regular file)\n", ent->d_name);
      st->archives_corrupt += 1U;
      continue;
    }
    mdl_verify_report_t report = {};
    const ra8_err_t     rc     = mdl_verify_file(format, path, &s_export_ws, &report);
    if (rc != k_ra8_ok) {
      (void)printf("  [%s ARTIFACT] %s\n",
                   mdl_format_is_verifiable(format) ? "CORRUPT" : "UNSUPPORTED",
                   ent->d_name);
      st->archives_corrupt += 1U;
      continue;
    }
    (void)printf("  [VALID ARTIFACT] %s (%zu page(s), %zu member(s))\n",
                 ent->d_name,
                 report.page_count,
                 report.member_count);
    st->archives_valid += 1U;
  }
  (void)closedir(d);
}

/**
 * @brief Verify persisted pages and artifacts for one tracked series.
 * @details Loads validated state, resolves every page beneath the canonical
 *          series root, compares content hashes, then checks local artifacts.
 * @param[in] series_dir Canonical tracked-series directory.
 * @param[in] state_path Complete `.mdl_state` path.
 * @param[in,out] st Verification counters to update.
 * @pre All pointer arguments are non-NULL.
 * @pre @p series_dir is canonical and @p st is initialized.
 * @post `series_checked` increases exactly once.
 * @post Every recorded page is classified as valid, missing/unsafe, or corrupt.
 * @note Not thread-safe because state and validator workspace are shared.
 * @since 0.1.0
 */
RA8_INTERNAL static void
verify_series_dir(const char* series_dir, const char* state_path, verify_stats_t* st)
{
  st->series_checked += 1U;
  (void)printf("verifying %s...\n", series_dir);
  if (mdl_state_load(state_path, &s_state) != k_ra8_ok) {
    (void)printf("  [VERIFY FAIL] %s: .mdl_state file unreadable or corrupt\n", series_dir);
    st->state_errors += 1U;
  } else {
    for (uint32_t i = 0U; i < s_state.page_rec_count; ++i) {
      const mdl_page_rec_t* rec = &s_state.pages[i];
      char                  page_path[PATH_MAX];
      const int   pn = snprintf(page_path, sizeof(page_path), "%s/%s", series_dir, rec->rel_path);
      char        resolved[PATH_MAX];
      struct stat sb;
      if ((pn < 0) || ((size_t)pn >= sizeof(page_path)) ||
          (realpath(page_path, resolved) == nullptr) || !mdl_path_contained(series_dir, resolved) ||
          (stat(resolved, &sb) != 0) || !S_ISREG(sb.st_mode)) {
        (void)printf("  [MISSING/UNSAFE] %s\n", rec->rel_path);
        st->pages_missing += 1U;
        continue;
      }
      uint64_t hash = 0U;
      if ((mdl_hash_file(&s_storage, resolved, &hash) != k_ra8_ok) || (hash != rec->content_hash)) {
        (void)printf("  [CORRUPT] %s (hash mismatch)\n", rec->rel_path);
        st->pages_corrupt += 1U;
      } else {
        st->pages_valid += 1U;
      }
    }
  }
  verify_artifacts(series_dir, st);
}

/**
 * @brief Verify a tracked series, a library root, or recognized artifacts.
 * @details Canonicalizes the target, detects a direct tracked-series marker or
 *          enumerates child series, validates artifacts, and prints totals.
 * @param[in] target_dir Requested directory, or NULL/empty for `downloads`.
 * @return Process-style verification status.
 * @retval 0 At least one target was found and every check passed.
 * @retval 1 The root was invalid, empty of targets, or any check failed.
 * @pre @p target_dir is NULL or points to a NUL-terminated path.
 * @pre ::s_export_ws is initialized.
 * @post No target content is modified.
 * @post Every discovered failure contributes to status and summary counters.
 * @note Not thread-safe because it uses shared state and validator workspace.
 * @since 0.1.0
 */
RA8_INTERNAL static int run_verify(const char* target_dir)
{
  const char*    dir = (target_dir != nullptr && target_dir[0] != '\0') ? target_dir : "downloads";
  verify_stats_t st  = {};
  char           canonical[PATH_MAX];
  struct stat    root_sb;
  if ((realpath(dir, canonical) == nullptr) || (lstat(canonical, &root_sb) != 0) ||
      !S_ISDIR(root_sb.st_mode)) {
    (void)fprintf(stderr, "media_dl: verify target '%s' is not a readable directory\n", dir);
    return 1;
  }
  char        state_path[PATH_MAX];
  const int   state_n = snprintf(state_path, sizeof(state_path), "%s/.mdl_state", canonical);
  struct stat state_sb;
  if ((state_n > 0) && ((size_t)state_n < sizeof(state_path)) &&
      (lstat(state_path, &state_sb) == 0) && S_ISREG(state_sb.st_mode)) {
    verify_series_dir(canonical, state_path, &st);
  } else {
    verify_artifacts(canonical, &st);
    DIR* root = opendir(canonical);
    if (root == nullptr) {
      st.fs_errors += 1U;
    } else {
      for (;;) {
        errno                    = 0;
        const struct dirent* ent = readdir(root);
        if (ent == nullptr) {
          if (errno != 0) {
            st.fs_errors += 1U;
          }
          break;
        }
        if (ent->d_name[0] == '.') {
          continue;
        }
        char      child[PATH_MAX];
        char      child_state[PATH_MAX];
        const int cn = snprintf(child, sizeof(child), "%s/%s", canonical, ent->d_name);
        const int sn =
          snprintf(child_state, sizeof(child_state), "%s/%s/.mdl_state", canonical, ent->d_name);
        struct stat child_sb;
        struct stat marker_sb;
        if ((cn <= 0) || ((size_t)cn >= sizeof(child)) || (sn <= 0) ||
            ((size_t)sn >= sizeof(child_state)) || (lstat(child, &child_sb) != 0) ||
            !S_ISDIR(child_sb.st_mode) || (lstat(child_state, &marker_sb) != 0) ||
            !S_ISREG(marker_sb.st_mode)) {
          continue;
        }
        verify_series_dir(child, child_state, &st);
      }
      (void)closedir(root);
    }
  }

  (void)printf("verify complete: %zu series; pages %zu valid, %zu missing, %zu corrupt; "
               "artifacts %zu valid, %zu failed\n",
               st.series_checked,
               st.pages_valid,
               st.pages_missing,
               st.pages_corrupt,
               st.archives_valid,
               st.archives_corrupt);

  const bool any_target = (st.series_checked != 0U) || (st.archives_checked != 0U);
  if (!any_target) {
    (void)fprintf(stderr, "media_dl: no tracked series or recognized artifacts under '%s'\n", dir);
  }
  return (any_target && (st.state_errors == 0U) && (st.fs_errors == 0U) &&
          (st.pages_missing == 0U) && (st.pages_corrupt == 0U) && (st.archives_corrupt == 0U))
           ? 0
           : 1;
}

/**
 * @brief Dispatch the one mode selected by strict CLI validation.
 * @details Routes exactly one validated mode to its composition-root runner and
 *          supplies the corresponding parsed arguments and bounded state.
 * @param[in] a Parsed command-line arguments.
 * @param[in] mode Validated command mode.
 * @param[in] format Validated output format.
 * @param[in] opts Validated run policy.
 * @param[in] nums Validated numeric arguments.
 * @param[in] run Prepared series-run template.
 * @return Process-style status from the selected mode.
 * @retval 0 The selected operation completed successfully.
 * @retval 1 The selected operation reported an execution failure.
 * @retval 2 A usage/mode error was encountered.
 * @pre All pointer arguments are non-NULL.
 * @pre @p mode was produced by ::mdl_cli_validate.
 * @post Exactly one mode handler is invoked.
 * @post Nonselected mode handlers perform no work.
 * @note Not thread-safe because mode handlers share composition-root buffers.
 * @since 0.1.0
 */
RA8_INTERNAL static int dispatch_run(const mdl_args_t*     a,
                                     mdl_cli_mode_t        mode,
                                     mdl_format_t          format,
                                     const mdl_run_opts_t* opts,
                                     const mdl_nums_t*     nums,
                                     const series_run_t*   run)
{
  switch (mode) {
    case k_mdl_cli_mode_series:
      return run_series(run);
    case k_mdl_cli_mode_search:
    case k_mdl_cli_mode_browse:
      return run_discover(a, opts, nums, run);
    case k_mdl_cli_mode_list:
    case k_mdl_cli_mode_update_all:
    case k_mdl_cli_mode_remove:
      return run_library(a, run);
    case k_mdl_cli_mode_verify:
      return run_verify(a->verify_dir != nullptr ? a->verify_dir : a->out);
    case k_mdl_cli_mode_init_site:
      return run_init_site(a->init_site_url);
    case k_mdl_cli_mode_pack:
      return run_pack(a->pack, format);
    case k_mdl_cli_mode_artifact:
      return run_artifact(a->page_url, a->out, nums->timeout, opts);
    case k_mdl_cli_mode_page:
      return run_page(a->page_url,
                      a->out,
                      a->attr,
                      nums->max_imgs,
                      nums->seed,
                      nums->timeout,
                      opts);
    default:
      return 2;
  }
}

/**
 * @brief Program entry point: parse the command line and select a run mode.
 * @details Parses and validates the arguments, then hands off to ::dispatch_run,
 *          which selects a library command (`--list`/`--remove`/`--update-all`),
 *          search/browse discovery (`--search`/`--browse`), pack mode (`--pack`),
 *          series mode (`--config` + `--series`), or single-page mode (a bare
 *          URL), in that precedence.
 * @param[in] argc Argument count.
 * @param[in] argv Argument vector.
 * @return 0 on success, 1 on a download/export failure, 2 on a usage error.
 * @since 0.1.0
 */
int main(int argc, char** argv)
{
  mdl_export_workspace_init(&s_export_ws, s_export_arena.bytes, sizeof(s_export_arena.bytes));
  mdl_args_t a = {};
  mdl_cli_parse(argc, argv, &a);
  mdl_cli_mode_t mode = k_mdl_cli_mode_invalid;
  if (!mdl_cli_validate(&a, &mode)) {
    mdl_cli_usage(argv[0]);
    return 2;
  }
  if (mode == k_mdl_cli_mode_help) {
    mdl_cli_usage(argv[0]);
    return 0;
  }
  if (mode == k_mdl_cli_mode_version) {
    (void)printf("media_dl 0.1.0\n");
    return 0;
  }
  if (a.out == nullptr) {
    a.out = "downloads";
  }
  if (a.attr == nullptr) {
    a.attr = "data-src";
  }
  if (a.ignore_robots) {
    (void)fprintf(stderr,
                  "media_dl: WARNING: --ignore-robots set; robots.txt will NOT be honoured\n");
  }

  mdl_nums_t nums = {};
  if (!mdl_cli_parse_nums(&a, &nums)) {
    return 2;
  }
  const mdl_run_opts_t opts = mdl_cli_run_opts(&a);

  const mdl_format_t format = mdl_format_from_str(a.format);
  if (format == k_mdl_fmt_invalid) {
    (void)fprintf(stderr,
                  "media_dl: bad --format '%s' "
                  "(loose|cbz|cbt|cbt.gz|epub|jof)\n",
                  a.format);
    return 2;
  }

  const series_run_t run = build_run(&a, format, &opts, &nums);
  if (internal_storage_init() != k_ra8_ok) {
    (void)fprintf(stderr, "media_dl: could not initialize portable filesystem binding\n");
    return 1;
  }
  const int result = dispatch_run(&a, mode, format, &opts, &nums, &run);
  if (fw_fs_posix_deinit(&s_fs_posix) != k_ra8_ok) {
    (void)fprintf(stderr, "media_dl: filesystem shutdown failed\n");
    return 1;
  }
  return result;
}
