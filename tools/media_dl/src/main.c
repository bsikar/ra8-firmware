/*
 * Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
/**
 * @file main.c
 * @brief media_dl -- host CLI for the e-reader media downloader.
 *
 * @details
 * Native host binary that links the firmware error contract (`ra8_err_t`) and
 * drives the downloader logic that will later run on the RA8 (only the injected
 * libcurl backend is host-specific). Two modes:
 *
 *  - **series** (`--config S.conf --series URL`): read a site descriptor, list
 *    a series' chapters, download the first N, and package them -- combined into
 *    one `<slug>-<lo>-<hi>.<ext>` by default, or one archive per chapter with
 *    `--separate`. `--format` selects the container (see mdl_export).
 *  - **pack** (`--pack DIR --format FMT`): package an existing image folder with
 *    no network -- re-encode a download, or drive the integration harness.
 *  - **page** (bare `URL`): fetch one page and download its `<img>` URLs -- a
 *    debug/inspection path.
 *
 * The tool identifies itself honestly (a truthful, configurable User-Agent),
 * honours robots.txt by default, and sanitises every untrusted name before it
 * reaches the filesystem.
 */
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "mdl_cli.h"
#include "mdl_config.h"
#include "mdl_export.h"
#include "mdl_extract.h"
#include "mdl_net.h"
#include "mdl_politeness.h"
#include "mdl_sanitize.h"
#include "mdl_session.h"
#include "ra8_attributes.h"
#include "ra8_err.h"

/** @brief Fixed sizing for the CLI (large buffers live in .bss). */
typedef enum : uint32_t {
  k_page_buf_bytes  = 8U * 1024U * 1024U, /**< Max HTML page size.             */
  k_req_timeout_def = 25000U,             /**< Default per-request budget, ms. */
} mdl_cli_limits_t;

/** @brief On-stack string buffer sizes and page-mode delays. */
typedef enum : uint16_t {
  k_chap_name_bytes    = 128,  /**< Chapter folder-name buffer.      */
  k_slug_bytes         = 128,  /**< Series slug buffer.              */
  k_dir_path_bytes     = 1024, /**< Directory-path buffer.           */
  k_file_path_bytes    = 1200, /**< File-path buffer.                */
  k_numbuf_bytes       = 16,   /**< Digit-run parse buffer.          */
  k_extbuf_bytes       = 8,    /**< File-extension buffer.           */
  k_page_img_delay_min = 400,  /**< page-mode per-image floor, ms.   */
  k_page_img_delay_max = 800,  /**< page-mode per-image ceiling, ms. */
} mdl_bufsize_t;

/** @brief `--polite` per-host delay floors (milliseconds). */
typedef enum : uint16_t {
  k_polite_img_min_ms  = 2000,  /**< Polite per-image floor.        */
  k_polite_img_max_ms  = 4000,  /**< Polite per-image ceiling.      */
  k_polite_chap_min_ms = 5000,  /**< Polite inter-chapter floor.    */
  k_polite_chap_max_ms = 10000, /**< Polite inter-chapter ceiling.  */
} mdl_polite_floor_t;

/** @brief Filesystem + parse constants. */
typedef enum : uint16_t {
  k_dir_mode = 0755, /**< mkdir() permission bits.  */
  k_dec_base = 10,   /**< strtoul()/strtol() radix. */
} mdl_misc_t;

/**
 * @var s_session
 * @brief The one download session (identity + robots cache) for this run.
 * @details Embeds a large per-host cache and fetch scratch, so it lives in
 *          `.bss` rather than on the stack.
 * @warning Single-threaded; reused across every fetch in one run.
 * @since 0.1.0
 */
static mdl_session_t s_session;

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
 * @var s_images
 * @brief Page-image URLs extracted from the current chapter page.
 * @warning Single-threaded; overwritten per chapter.
 * @since 0.1.0
 */
static mdl_url_list_t s_images;

/**
 * @var s_rowtmp
 * @brief Scratch row used while reordering ::s_chapters in place.
 * @warning Single-threaded; holds no meaningful value between sorts.
 * @since 0.1.0
 */
static char s_rowtmp[k_mdl_url_max];

/** @brief Larger of two unsigned values. */
RA8_INTERNAL static uint32_t max_u32(uint32_t a, uint32_t b)
{
  return (a > b) ? a : b;
}

/** @brief Pick a file extension from a URL, defaulting to "jpg". */
RA8_INTERNAL static const char* ext_of(const char* url)
{
  const char* slash = strrchr(url, '/');
  const char* seg   = (slash == nullptr) ? url : slash + 1;
  const char* dot   = strrchr(seg, '.');
  if (dot == nullptr) {
    return "jpg";
  }
  static const char* const k_known[] = {"jpg", "jpeg", "png", "gif", "webp", "bmp"};
  static char              s_ext[k_extbuf_bytes];
  size_t                   n = 0U;
  for (const char* c = dot + 1;
       (*c != '\0') && (*c != '?') && (*c != '#') && (n + 1U < sizeof(s_ext));
       ++c) {
    s_ext[n] = (char)(((*c >= 'A') && (*c <= 'Z')) ? (*c + ('a' - 'A')) : *c);
    ++n;
  }
  s_ext[n] = '\0';
  for (size_t i = 0U; i < (sizeof(k_known) / sizeof(k_known[0])); ++i) {
    if (strcmp(s_ext, k_known[i]) == 0) {
      return s_ext;
    }
  }
  return "jpg";
}

/** @brief Last non-empty path segment of a URL, sanitised, into `out`. */
RA8_INTERNAL static void last_segment(const char* url, char* out, size_t cap)
{
  const char* q   = strpbrk(url, "?#");
  size_t      end = (q == nullptr) ? strlen(url) : (size_t)(q - url);
  while ((end > 0U) && (url[end - 1U] == '/')) {
    --end; /* trim trailing slashes */
  }
  size_t start = end;
  while ((start > 0U) && (url[start - 1U] != '/')) {
    --start;
  }
  char   raw[k_chap_name_bytes];
  size_t n = 0U;
  for (size_t i = start; (i < end) && (n + 1U < sizeof(raw)); ++i) {
    raw[n] = url[i];
    ++n;
  }
  raw[n] = '\0';
  /* Neutralise `..`, absolute/traversal bytes, reserved names, over-length. */
  (void)mdl_sanitize_segment(raw, out, cap);
}

/** @brief True if `child` resolves to a path inside `parent_abs`. */
RA8_INTERNAL static bool path_under(const char* parent_abs, const char* child)
{
  char resolved[PATH_MAX];
  if (realpath(child, resolved) == nullptr) {
    return false;
  }
  return mdl_path_contained(parent_abs, resolved);
}

/** @brief Parse the last run of digits in `url` (0 if none). */
RA8_INTERNAL static long chapter_num(const char* url)
{
  const size_t len   = strlen(url);
  size_t       e     = 0U;
  size_t       s     = 0U;
  bool         found = false;
  for (size_t i = 0U; i < len; ++i) {
    if ((url[i] >= '0') && (url[i] <= '9')) {
      size_t j = i;
      while ((j < len) && (url[j] >= '0') && (url[j] <= '9')) {
        ++j;
      }
      s     = i;
      e     = j;
      found = true;
      i     = j; /* skip the run */
    }
  }
  if (!found) {
    return 0L;
  }
  char   num[k_numbuf_bytes];
  size_t n = 0U;
  for (size_t i = s; (i < e) && (n + 1U < sizeof(num)); ++i) {
    num[n++] = url[i];
  }
  num[n] = '\0';
  return strtol(num, nullptr, k_dec_base);
}

/** @brief Swap two rows of a URL list. */
RA8_INTERNAL static void swap_rows(mdl_url_list_t* l, size_t a, size_t b)
{
  if (a == b) {
    return;
  }
  memcpy(s_rowtmp, l->urls[a], k_mdl_url_max);
  memcpy(l->urls[a], l->urls[b], k_mdl_url_max);
  memcpy(l->urls[b], s_rowtmp, k_mdl_url_max);
}

/** @brief Reverse a URL list in place. */
RA8_INTERNAL static void reverse_list(mdl_url_list_t* l)
{
  for (size_t i = 0U; i < (l->count / 2U); ++i) {
    swap_rows(l, i, l->count - 1U - i);
  }
}

/** @brief Selection-sort a URL list by parsed chapter number, ascending. */
RA8_INTERNAL static void sort_by_chapter_num(mdl_url_list_t* l)
{
  for (size_t i = 0U; i + 1U < l->count; ++i) {
    size_t min = i;
    for (size_t j = i + 1U; j < l->count; ++j) {
      if (chapter_num(l->urls[j]) < chapter_num(l->urls[min])) {
        min = j;
      }
    }
    swap_rows(l, i, min);
  }
}

/** @brief Apply the configured chapter ordering in place. */
RA8_INTERNAL static void apply_order(mdl_url_list_t* l, mdl_chapter_order_t order)
{
  if (order == k_mdl_order_reverse) {
    reverse_list(l);
  } else if (order == k_mdl_order_asc) {
    sort_by_chapter_num(l);
  }
}

/** @brief Keep only list entries whose URL starts with `prefix`. */
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

/** @brief `--polite`: raise a site's delay floors. */
RA8_INTERNAL static void apply_polite(mdl_site_t* site)
{
  site->img_delay_min     = max_u32(site->img_delay_min, (uint32_t)k_polite_img_min_ms);
  site->img_delay_max     = max_u32(site->img_delay_max, (uint32_t)k_polite_img_max_ms);
  site->chapter_delay_min = max_u32(site->chapter_delay_min, (uint32_t)k_polite_chap_min_ms);
  site->chapter_delay_max = max_u32(site->chapter_delay_max, (uint32_t)k_polite_chap_max_ms);
}

/** @brief One-time warning printed when no operator contact is configured. */
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
 * @brief Gate, space, and download one chapter image; 0 ok, 1 on skip/failure.
 * @param[in]     site        Site descriptor (image politeness bounds).
 * @param[in]     chapter_url Referer for the image request.
 * @param[in]     dest_dir    Directory the image is written into.
 * @param[in,out] page_no     Running 1-based page counter, advanced per image.
 * @param[in,out] pol         Jitter source spacing consecutive fetches.
 * @param[in]     timeout     Per-request budget, milliseconds.
 * @param[in]     img_url     Absolute image URL to fetch.
 * @return 0 on success, 1 when robots refused it or the fetch failed.
 * @since 0.1.0
 */
RA8_INTERNAL static size_t download_one_image(const mdl_site_t* site,
                                              const char*       chapter_url,
                                              const char*       dest_dir,
                                              size_t*           page_no,
                                              mdl_politeness_t* pol,
                                              uint32_t          timeout,
                                              const char*       img_url)
{
  uint32_t crawl = 0U;
  if (!mdl_session_url_allowed(&s_session, img_url, &crawl)) {
    return 1U; /* robots refused (message already printed) */
  }
  (void)mdl_politeness_wait(pol,
                            max_u32(site->img_delay_min, crawl),
                            max_u32(site->img_delay_max, crawl));
  *page_no += 1U;
  char path[k_file_path_bytes];
  (void)snprintf(path, sizeof(path), "%s/page_%04zu.%s", dest_dir, *page_no, ext_of(img_url));
  const mdl_net_req_t img_req = {.user_agent = s_session.user_agent,
                                 .referer    = chapter_url,
                                 .timeout_ms = timeout};
  size_t              got     = 0U;
  const ra8_err_t     irc     = mdl_net_get_file(s_session.net, img_url, &img_req, path, &got);
  if (irc != k_ra8_ok) {
    (void)fprintf(stderr, "    page %zu FAILED (err 0x%X)\n", *page_no, (unsigned)irc);
    return 1U;
  }
  return 0U;
}

/**
 * @brief Download one chapter's page images into `dest_dir`.
 *
 * @param[in]     site        Site descriptor supplying the image selectors and
 *                            the per-image politeness bounds.
 * @param[in]     series_url  Series page URL, sent as the Referer.
 * @param[in]     chapter_url Chapter page URL to scrape for `<img>` sources.
 * @param[in]     dest_dir    Existing directory the images are written into.
 * @param[in,out] page_no     Running 1-based page counter, advanced per image so
 *                            a combined download numbers pages continuously
 *                            across chapters.
 * @param[in,out] pol         Jitter source spacing consecutive image fetches.
 * @param[in]     timeout     Per-request time budget, milliseconds.
 * @return Count of images that failed to download.
 */
RA8_INTERNAL static size_t download_chapter(const mdl_site_t* site,
                                            const char*       series_url,
                                            const char*       chapter_url,
                                            const char*       dest_dir,
                                            size_t*           page_no,
                                            mdl_politeness_t* pol,
                                            uint32_t          timeout)
{
  const mdl_net_req_t page_req = {.user_agent = s_session.user_agent,
                                  .referer    = series_url,
                                  .timeout_ms = timeout};
  size_t              page_len = 0U;
  ra8_err_t           rc =
    mdl_net_get_buf(s_session.net, chapter_url, &page_req, s_page, sizeof(s_page), &page_len);
  if (rc != k_ra8_ok) {
    (void)fprintf(stderr, "  chapter fetch failed %s (err 0x%X)\n", chapter_url, (unsigned)rc);
    return 1U;
  }

  rc = mdl_extract_images(s_page,
                          page_len,
                          chapter_url,
                          site->page_img_attr,
                          site->page_img_url_contains,
                          &s_images);
  if ((rc != k_ra8_ok) && (rc != k_ra8_err_no_mem)) {
    (void)fprintf(stderr, "  image extract failed (err 0x%X)\n", (unsigned)rc);
    return 1U;
  }

  char chap[k_chap_name_bytes];
  last_segment(chapter_url, chap, sizeof(chap));
  (void)printf("  %s: %zu page(s)\n", chap, s_images.count);

  size_t fail = 0U;
  for (size_t i = 0U; i < s_images.count; ++i) {
    fail +=
      download_one_image(site, chapter_url, dest_dir, page_no, pol, timeout, s_images.urls[i]);
  }
  return fail;
}

/** @brief Fetch the series page and build the ordered, filtered chapter list. */
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
  ra8_err_t rc = mdl_net_get_buf(s_session.net, series_url, &req, s_page, sizeof(s_page), &len);
  if (rc != k_ra8_ok) {
    return rc;
  }
  (void)mdl_extract_anchors(s_page, len, series_url, site->chapter_url_contains, &s_chapters);

  /* Keep only this series' chapters (drop cross-series relation links). */
  char prefix[k_dir_path_bytes];
  (void)snprintf(prefix, sizeof(prefix), "%s", series_url);
  size_t pl = strlen(prefix);
  if ((pl > 0U) && (prefix[pl - 1U] != '/') && (pl + 1U < sizeof(prefix))) {
    prefix[pl]      = '/';
    prefix[pl + 1U] = '\0';
  }
  filter_prefix(&s_chapters, prefix);
  apply_order(&s_chapters, site->chapter_order);
  return k_ra8_ok;
}

/** @brief Package one downloaded chapter into `format`; 0 ok, 1 on failure. */
RA8_INTERNAL static size_t
export_one(mdl_format_t format, const char* series_dir, const char* chapter_url)
{
  char chap[k_chap_name_bytes];
  last_segment(chapter_url, chap, sizeof(chap));
  const char* ext = mdl_format_ext(format);

  char dir[k_dir_path_bytes];
  (void)snprintf(dir, sizeof(dir), "%s/%s", series_dir, chap);
  char out[k_dir_path_bytes];
  (void)snprintf(out, sizeof(out), "%s/%s.%s", series_dir, chap, ext);

  const ra8_err_t rc = mdl_export_chapter(format, dir, out);
  if (rc != k_ra8_ok) {
    (void)fprintf(stderr, "  export %s.%s FAILED (err 0x%X)\n", chap, ext, (unsigned)rc);
    return 1U;
  }
  (void)printf("  packaged %s.%s\n", chap, ext);
  return 0U;
}

/**
 * @brief Chapter-number range spanned by the slice `[start, last]`.
 * @details Taken as the min/max rather than the endpoints so a reverse-ordered
 *          chapter list still yields an ascending `<lo>-<hi>` archive name.
 * @param[in]  start First index into ::s_chapters.
 * @param[in]  last  Last index into ::s_chapters (inclusive, >= @p start).
 * @param[out] lo    Receives the lowest chapter number in the slice.
 * @param[out] hi    Receives the highest chapter number in the slice.
 * @since 0.1.0
 */
RA8_INTERNAL static void chapter_range(size_t start, size_t last, long* lo, long* hi)
{
  *lo = chapter_num(s_chapters.urls[start]);
  *hi = *lo;
  for (size_t i = start; i <= last; ++i) {
    const long num = chapter_num(s_chapters.urls[i]);
    *lo            = (num < *lo) ? num : *lo;
    *hi            = (num > *hi) ? num : *hi;
  }
}

/**
 * @brief Package the combined chapter folder into one archive; 0 ok, 1 on error.
 * @param[in] format     Target container (never `loose` on this path).
 * @param[in] series_dir Series output directory.
 * @param[in] dir        The combined chapter folder to package.
 * @param[in] slug       Series slug used in the archive name.
 * @param[in] lo         Lowest chapter number in the slice.
 * @param[in] hi         Highest chapter number in the slice.
 * @param[in] got_ch     Chapters actually downloaded (for the summary line).
 * @return 0 on success, 1 when the export failed.
 * @since 0.1.0
 */
RA8_INTERNAL static size_t export_combined(mdl_format_t format,
                                           const char*  series_dir,
                                           const char*  dir,
                                           const char*  slug,
                                           long         lo,
                                           long         hi,
                                           size_t       got_ch)
{
  const char* ext = mdl_format_ext(format);
  char        out[k_dir_path_bytes];
  (void)snprintf(out, sizeof(out), "%s/%s-%ld-%ld.%s", series_dir, slug, lo, hi, ext);
  const ra8_err_t rc = mdl_export_chapter(format, dir, out);
  if (rc != k_ra8_ok) {
    (void)fprintf(stderr, "  combine export FAILED (err 0x%X)\n", (unsigned)rc);
    return 1U;
  }
  (void)printf("  combined %zu chapter(s) -> %s\n", got_ch, out);
  return 0U;
}

/**
 * @brief Download one chapter into its own folder, and package it.
 *
 * @details
 * The `--separate` / `loose`-format path: each chapter gets a folder named after
 * the last URL segment, page numbering restarts at zero inside it, and (unless
 * the format is `loose`, which has no archive) the folder is exported to its own
 * archive next to the others.
 *
 * @param[in]     site       Site descriptor supplying selectors and delays.
 * @param[in]     series_url Series page URL, used as the Referer.
 * @param[in]     chapter_url URL of the chapter to download.
 * @param[in]     series_dir Absolute directory holding the per-chapter folders.
 * @param[in]     format     Output format for the per-chapter archive.
 * @param[in,out] pol        Politeness state carried across chapters.
 * @param[in]     timeout    Per-request timeout in milliseconds.
 * @return Number of failures (page fetch failures plus a failed export).
 * @retval 0 Every page fetched and the archive (if any) was written.
 *
 * @pre All pointer arguments are non-NULL.
 * @pre @p series_dir exists and is absolute.
 * @post The chapter folder exists under @p series_dir.
 * @post Page numbering inside that folder starts at zero.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static size_t download_chapter_separate(const mdl_site_t* site,
                                                     const char*       series_url,
                                                     const char*       chapter_url,
                                                     const char*       series_dir,
                                                     mdl_format_t      format,
                                                     mdl_politeness_t* pol,
                                                     uint32_t          timeout)
{
  char chap[k_chap_name_bytes];
  last_segment(chapter_url, chap, sizeof(chap));
  char chap_dir[k_dir_path_bytes];
  (void)snprintf(chap_dir, sizeof(chap_dir), "%s/%s", series_dir, chap);
  (void)mkdir(chap_dir, (mode_t)k_dir_mode);
  if (!path_under(series_dir, chap_dir)) {
    (void)fprintf(stderr, "  refusing chapter dir outside %s: %s\n", series_dir, chap_dir);
    return 1U;
  }

  size_t chap_page = 0U;
  size_t fails =
    download_chapter(site, series_url, chapter_url, chap_dir, &chap_page, pol, timeout);
  if (format != k_mdl_fmt_loose) {
    fails += export_one(format, series_dir, chapter_url);
  }
  return fails;
}

/** @brief Make the combined output folder under `series_dir`; 0 ok, 1 on error. */
RA8_INTERNAL static size_t
make_combined_dir(const char* series_dir, const char* slug, long lo, long hi, char* out, size_t cap)
{
  (void)snprintf(out, cap, "%s/%s-%ld-%ld", series_dir, slug, lo, hi);
  (void)mkdir(out, (mode_t)k_dir_mode);
  if (!path_under(series_dir, out)) {
    (void)fprintf(stderr, "  refusing combined dir outside %s: %s\n", series_dir, out);
    return 1U;
  }
  return 0U;
}

/** @brief Space (if not the first) then download one chapter; returns failures. */
RA8_INTERNAL static size_t chapter_step(const mdl_site_t* site,
                                        const char*       series_url,
                                        const char*       chapter_url,
                                        const char*       series_dir,
                                        const char*       combined_dir,
                                        mdl_format_t      format,
                                        bool              one_file,
                                        uint32_t          crawl,
                                        bool              space,
                                        size_t*           page_no,
                                        mdl_politeness_t* pol,
                                        uint32_t          timeout)
{
  if (space) {
    (void)mdl_politeness_wait(pol,
                              max_u32(site->chapter_delay_min, crawl),
                              max_u32(site->chapter_delay_max, crawl));
  }
  if (one_file) {
    return download_chapter(site, series_url, chapter_url, combined_dir, page_no, pol, timeout);
  }
  return download_chapter_separate(site, series_url, chapter_url, series_dir, format, pol, timeout);
}

/**
 * @brief Download `chapters` chapters from `start`; returns failure count.
 *
 * Default (`combine`) collects every downloaded chapter's pages -- numbered
 * continuously -- into one folder `series_dir/<slug>-<lo>-<hi>/` and packages it
 * into a SINGLE archive `series_dir/<slug>-<lo>-<hi>.<ext>` (lo/hi are the parsed
 * chapter-number range). `--separate` (combine == false), or a `loose` format
 * that has no archive, falls back to one folder (and one archive) per chapter.
 */
RA8_INTERNAL static size_t download_chapters(const mdl_site_t* site,
                                             const char*       series_url,
                                             const char*       series_dir,
                                             const char*       slug,
                                             mdl_format_t      format,
                                             bool              combine,
                                             size_t            chapters,
                                             size_t            start,
                                             uint64_t          seed,
                                             uint32_t          timeout)
{
  mdl_politeness_t pol;
  mdl_politeness_init(&pol, seed);

  if (start >= s_chapters.count) {
    (void)fprintf(stderr, "media_dl: --start %zu is past the last chapter\n", start);
    return 1U;
  }

  const bool one_file = combine && (format != k_mdl_fmt_loose);
  size_t     last     = start + (chapters - 1U);
  if (last >= s_chapters.count) {
    last = s_chapters.count - 1U;
  }
  long lo = 0;
  long hi = 0;
  chapter_range(start, last, &lo, &hi);

  char   combined_dir[k_dir_path_bytes];
  size_t page_no = 0U;
  if (one_file &&
      (make_combined_dir(series_dir, slug, lo, hi, combined_dir, sizeof(combined_dir)) != 0U)) {
    return 1U;
  }

  size_t got_ch = 0U;
  size_t fails  = 0U;
  for (size_t i = start; (i < s_chapters.count) && (got_ch < chapters); ++i) {
    const char* curl  = s_chapters.urls[i];
    uint32_t    crawl = 0U;
    if (!mdl_session_url_allowed(&s_session, curl, &crawl)) {
      continue; /* robots refused this chapter (message printed) */
    }
    fails += chapter_step(site,
                          series_url,
                          curl,
                          series_dir,
                          combined_dir,
                          format,
                          one_file,
                          crawl,
                          got_ch > 0U,
                          &page_no,
                          &pol,
                          timeout);
    ++got_ch;
  }

  if (one_file) {
    fails += export_combined(format, series_dir, combined_dir, slug, lo, hi, got_ch);
  }
  (void)printf("done: %zu chapter(s) into %s/ (%zu failure(s))\n", got_ch, series_dir, fails);
  return fails;
}

/** @brief Build the session UA + identity for a run; warns on missing contact. */
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

/** @brief Build slug + absolute series dir under `out_dir`; false on failure. */
RA8_INTERNAL static bool prepare_series_dir(
  const char* out_dir, const char* series_url, char* slug, size_t slug_cap, char* abs_dir)
{
  last_segment(series_url, slug, slug_cap);
  char series_dir[k_dir_path_bytes];
  (void)snprintf(series_dir, sizeof(series_dir), "%s/%s", out_dir, slug);
  (void)mkdir(out_dir, (mode_t)k_dir_mode);
  (void)mkdir(series_dir, (mode_t)k_dir_mode);
  /* Archive export spawns the archiver with cwd set to the chapter dir, so the
   * output paths must be absolute -- resolve the series dir now that it exists. */
  if (realpath(series_dir, abs_dir) == nullptr) {
    (void)fprintf(stderr, "media_dl: cannot resolve %s\n", series_dir);
    return false;
  }
  return true;
}

/** @brief series mode: config + series URL -> download N chapters. */
RA8_INTERNAL static int run_series(const char*           cfg_path,
                                   const char*           series_url,
                                   const char*           out_dir,
                                   mdl_format_t          format,
                                   bool                  combine,
                                   size_t                chapters,
                                   size_t                start,
                                   uint64_t              seed,
                                   uint32_t              timeout,
                                   const mdl_run_opts_t* opts)
{
  mdl_site_t site;
  if (mdl_config_load(cfg_path, &site) != k_ra8_ok) {
    return 1;
  }
  if (opts->polite) {
    apply_polite(&site);
  }
  (void)printf("site: %s (host %s, kind %s)\n", site.name, site.host, site.kind);

  mdl_net_iface_t* net = mdl_net_curl_create(&opts->policy);
  if (net == nullptr) {
    (void)fprintf(stderr, "media_dl: network init failed\n");
    return 1;
  }
  char ua[k_mdl_ua_max];
  start_session(net, opts, site.contact, ua, sizeof(ua));

  ra8_err_t rc = prepare_chapters(&site, series_url, timeout);
  if (rc != k_ra8_ok) {
    (void)fprintf(stderr, "media_dl: series fetch failed (err 0x%X)\n", (unsigned)rc);
    mdl_net_curl_destroy(net);
    return 1;
  }
  (void)printf("chapters found: %zu\n", s_chapters.count);
  if (s_chapters.count == 0U) {
    (void)fprintf(stderr, "media_dl: no chapters (check chapter_url_contains)\n");
    mdl_net_curl_destroy(net);
    return 1;
  }

  char series_slug[k_slug_bytes];
  char abs_series_dir[PATH_MAX];
  if (!prepare_series_dir(out_dir, series_url, series_slug, sizeof(series_slug), abs_series_dir)) {
    mdl_net_curl_destroy(net);
    return 1;
  }

  const size_t fails = download_chapters(&site,
                                         series_url,
                                         abs_series_dir,
                                         series_slug,
                                         format,
                                         combine,
                                         chapters,
                                         start,
                                         seed,
                                         timeout);
  mdl_net_curl_destroy(net);
  return (fails == 0U) ? 0 : 1;
}

/** @brief Gate, space, and download one page-mode image; 0 ok, 1 on skip/fail. */
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
  char path[k_file_path_bytes];
  (void)snprintf(
    path, sizeof(path), "%s/page_%03zu.%s", out_dir, idx + 1U, ext_of(s_images.urls[idx]));
  const mdl_net_req_t ir  = {.user_agent = s_session.user_agent,
                             .referer    = url,
                             .timeout_ms = timeout};
  size_t              got = 0U;
  return (mdl_net_get_file(s_session.net, s_images.urls[idx], &ir, path, &got) != k_ra8_ok) ? 1U
                                                                                            : 0U;
}

/** @brief Download the extracted page images into `out_dir`; returns failures. */
RA8_INTERNAL static size_t download_page_images(
  const char* url, const char* out_dir, uint32_t max_imgs, uint64_t seed, uint32_t timeout, bool polite)
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

/** @brief page mode: fetch one URL, download its `<img>` URLs (debug path). */
RA8_INTERNAL static int run_page(const char*           url,
                                 const char*           out_dir,
                                 const char*           attr,
                                 uint32_t              max_imgs,
                                 uint64_t              seed,
                                 uint32_t              timeout,
                                 const mdl_run_opts_t* opts)
{
  mdl_net_iface_t* net = mdl_net_curl_create(&opts->policy);
  if (net == nullptr) {
    (void)fprintf(stderr, "media_dl: network init failed\n");
    return 1;
  }
  char ua[k_mdl_ua_max];
  start_session(net, opts, nullptr, ua, sizeof(ua));

  if (!mdl_session_url_allowed(&s_session, url, nullptr)) {
    mdl_net_curl_destroy(net);
    return 1;
  }
  const mdl_net_req_t req = {.user_agent = s_session.user_agent,
                             .referer    = nullptr,
                             .timeout_ms = timeout};
  size_t              len = 0U;
  ra8_err_t           rc  = mdl_net_get_buf(s_session.net, url, &req, s_page, sizeof(s_page), &len);
  if (rc != k_ra8_ok) {
    (void)fprintf(stderr, "media_dl: fetch failed (err 0x%X)\n", (unsigned)rc);
    mdl_net_curl_destroy(net);
    return 1;
  }
  (void)mdl_extract_images(s_page, len, url, attr, nullptr, &s_images);
  (void)printf("found %zu image(s)\n", s_images.count);
  (void)mkdir(out_dir, (mode_t)k_dir_mode);

  const size_t fail = download_page_images(url, out_dir, max_imgs, seed, timeout, opts->polite);
  mdl_net_curl_destroy(net);
  return (fail == 0U) ? 0 : 1;
}

/** @brief Convert a decimal string, or `dflt` when NULL. */
RA8_INTERNAL static unsigned long to_ul(const char* s, unsigned long dflt)
{
  return (s == nullptr) ? dflt : strtoul(s, nullptr, k_dec_base);
}

/** @brief pack mode: package an existing folder of images into `format`. */
RA8_INTERNAL static int run_pack(const char* dir, mdl_format_t format)
{
  if ((format == k_mdl_fmt_loose) || (format == k_mdl_fmt_invalid)) {
    (void)fprintf(stderr,
                  "media_dl: --pack needs a --format "
                  "(cbz|cbt|cbt.gz|cbt.xz|cbr|epub|jof|rabook)\n");
    return 2;
  }
  char abs[PATH_MAX];
  if (realpath(dir, abs) == nullptr) {
    (void)fprintf(stderr, "media_dl: cannot resolve '%s'\n", dir);
    return 1;
  }
  const char* ext = mdl_format_ext(format);
  char        out[PATH_MAX];
  /* `abs` is itself PATH_MAX, so appending ".<ext>" can overrun `out`.
   * snprintf would silently truncate and we would then package the chapter
   * into a path that is not the one asked for; report it instead. */
  const int n = snprintf(out, sizeof(out), "%s.%s", abs, ext);
  if ((n < 0) || ((size_t)n >= sizeof(out))) {
    (void)fprintf(stderr, "media_dl: output path for '%s' is too long\n", dir);
    return 1;
  }
  const ra8_err_t rc = mdl_export_chapter(format, abs, out);
  if (rc != k_ra8_ok) {
    (void)fprintf(stderr, "media_dl: pack '%s' as .%s FAILED (err 0x%X)\n", dir, ext, (unsigned)rc);
    return 1;
  }
  (void)printf("packed %s -> %s\n", dir, out);
  return 0;
}

/**
 * @brief Program entry point: parse the command line and select a run mode.
 * @details Dispatches to pack mode (`--pack`), series mode (`--config` +
 *          `--series`), or single-page mode (a bare URL), in that precedence.
 * @param[in] argc Argument count.
 * @param[in] argv Argument vector.
 * @return 0 on success, 1 on a download/export failure, 2 on a usage error.
 * @since 0.1.0
 */
int main(int argc, char** argv)
{
  mdl_args_t a = {};
  a.out        = "downloads";
  a.attr       = "data-src";
  mdl_cli_parse(argc, argv, &a);
  if (a.bad) {
    mdl_cli_usage(argv[0]);
    return 2;
  }
  if (a.ignore_robots) {
    (void)fprintf(stderr,
                  "media_dl: WARNING: --ignore-robots set; robots.txt will NOT be honoured\n");
  }

  const uint64_t       seed    = (a.seed == nullptr) ? 1UL : strtoull(a.seed, nullptr, k_dec_base);
  const uint32_t       timeout = (uint32_t)to_ul(a.timeout, (unsigned long)k_req_timeout_def);
  const size_t         start   = (size_t)to_ul(a.start, 0UL);
  const mdl_run_opts_t opts    = mdl_cli_run_opts(&a);

  const mdl_format_t format = mdl_format_from_str(a.format);
  if (format == k_mdl_fmt_invalid) {
    (void)fprintf(stderr,
                  "media_dl: bad --format '%s' "
                  "(loose|cbz|cbt|cbr|cbt.xz|cbt.gz|epub|jof|rabook)\n",
                  a.format);
    return 2;
  }

  if (a.pack != nullptr) {
    return run_pack(a.pack, format);
  }

  if (a.cfg != nullptr) {
    if (a.series == nullptr) {
      (void)fprintf(stderr, "media_dl: --config requires --series URL\n");
      return 2;
    }
    size_t chapters = (size_t)to_ul(a.chapters, 1UL);
    if (chapters == 0U) {
      chapters = 1U;
    }
    return run_series(a.cfg,
                      a.series,
                      a.out,
                      format,
                      !a.separate,
                      chapters,
                      start,
                      seed,
                      timeout,
                      &opts);
  }
  if (a.page_url != nullptr) {
    const uint32_t max_imgs = (uint32_t)to_ul(a.max, 0UL);
    return run_page(a.page_url, a.out, a.attr, max_imgs, seed, timeout, &opts);
  }
  mdl_cli_usage(argv[0]);
  return 2;
}
