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
 */
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "mdl_config.h"
#include "mdl_export.h"
#include "mdl_extract.h"
#include "mdl_net.h"
#include "mdl_politeness.h"
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

/** @brief Filesystem + parse constants. */
typedef enum : uint16_t {
  k_dir_mode = 0755, /**< mkdir() permission bits.  */
  k_dec_base = 10,   /**< strtoul()/strtol() radix. */
} mdl_misc_t;

/**
 * @var k_user_agent
 * @brief Session User-Agent sent with every request.
 * @details Held constant for the whole run. Rotating it per request (as the
 *          Kotlin original did) looks more bot-like to a host, not less.
 * @note Read-only; shared by both fetch paths.
 * @since 0.1.0
 */
static const char* const k_user_agent =
  "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 "
  "(KHTML, like Gecko) Chrome/126.0.0.0 Safari/537.36";

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
  size_t n = 0U;
  for (size_t i = start; (i < end) && (n + 1U < cap); ++i) {
    const char c  = url[i];
    const bool ok = ((c >= 'A') && (c <= 'Z')) || ((c >= 'a') && (c <= 'z')) ||
                    ((c >= '0') && (c <= '9')) || (c == '.') || (c == '-') || (c == '_');
    out[n]        = ok ? c : '_';
    ++n;
  }
  out[n] = '\0';
  if (n == 0U) {
    (void)snprintf(out, cap, "item");
  }
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

/**
 * @brief Download one chapter's page images into `dest_dir`.
 *
 * @param[in]     net         Network interface used for every fetch.
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
RA8_INTERNAL static size_t download_chapter(mdl_net_iface_t*  net,
                                            const mdl_site_t* site,
                                            const char*       series_url,
                                            const char*       chapter_url,
                                            const char*       dest_dir,
                                            size_t*           page_no,
                                            mdl_politeness_t* pol,
                                            uint32_t          timeout)
{
  const mdl_net_req_t page_req = {.user_agent = k_user_agent,
                                  .referer    = series_url,
                                  .timeout_ms = timeout};
  size_t              page_len = 0U;
  ra8_err_t rc = mdl_net_get_buf(net, chapter_url, &page_req, s_page, sizeof(s_page), &page_len);
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
    (void)mdl_politeness_wait(pol, site->img_delay_min, site->img_delay_max);
    *page_no += 1U;
    char path[k_file_path_bytes];
    (void)snprintf(path,
                   sizeof(path),
                   "%s/page_%04zu.%s",
                   dest_dir,
                   *page_no,
                   ext_of(s_images.urls[i]));
    const mdl_net_req_t img_req = {.user_agent = k_user_agent,
                                   .referer    = chapter_url,
                                   .timeout_ms = timeout};
    size_t              got     = 0U;
    ra8_err_t           irc     = mdl_net_get_file(net, s_images.urls[i], &img_req, path, &got);
    if (irc != k_ra8_ok) {
      (void)fprintf(stderr, "    page %zu FAILED (err 0x%X)\n", *page_no, (unsigned)irc);
      ++fail;
    }
  }
  return fail;
}

/** @brief Fetch the series page and build the ordered, filtered chapter list. */
RA8_INTERNAL static ra8_err_t prepare_chapters(mdl_net_iface_t*  net,
                                               const mdl_site_t* site,
                                               const char*       series_url,
                                               uint32_t          timeout)
{
  const mdl_net_req_t req = {.user_agent = k_user_agent, .referer = nullptr, .timeout_ms = timeout};
  size_t              len = 0U;
  ra8_err_t           rc  = mdl_net_get_buf(net, series_url, &req, s_page, sizeof(s_page), &len);
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
 * @brief Download `chapters` chapters from `start`; returns failure count.
 *
 * Default (`combine`) collects every downloaded chapter's pages -- numbered
 * continuously -- into one folder `series_dir/<slug>-<lo>-<hi>/` and packages it
 * into a SINGLE archive `series_dir/<slug>-<lo>-<hi>.<ext>` (lo/hi are the parsed
 * chapter-number range). `--separate` (combine == false), or a `loose` format
 * that has no archive, falls back to one folder (and one archive) per chapter.
 */
RA8_INTERNAL static size_t download_chapters(mdl_net_iface_t*  net,
                                             const mdl_site_t* site,
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

  size_t last = start + (chapters - 1U);
  if (last >= s_chapters.count) {
    last = s_chapters.count - 1U;
  }
  long lo = 0;
  long hi = 0;
  chapter_range(start, last, &lo, &hi);

  char   combined_dir[k_dir_path_bytes];
  size_t page_no = 0U;
  if (one_file) {
    (void)snprintf(combined_dir, sizeof(combined_dir), "%s/%s-%ld-%ld", series_dir, slug, lo, hi);
    (void)mkdir(combined_dir, (mode_t)k_dir_mode);
  }

  size_t got_ch = 0U;
  size_t fails  = 0U;
  for (size_t i = start; (i < s_chapters.count) && (got_ch < chapters); ++i) {
    if (got_ch > 0U) {
      (void)mdl_politeness_wait(&pol, site->chapter_delay_min, site->chapter_delay_max);
    }
    const char* curl = s_chapters.urls[i];
    if (one_file) {
      /* Continuous page numbering across chapters -> one contiguous archive. */
      fails += download_chapter(net, site, series_url, curl, combined_dir, &page_no, &pol, timeout);
    } else {
      char chap[k_chap_name_bytes];
      last_segment(curl, chap, sizeof(chap));
      char chap_dir[k_dir_path_bytes];
      (void)snprintf(chap_dir, sizeof(chap_dir), "%s/%s", series_dir, chap);
      (void)mkdir(chap_dir, (mode_t)k_dir_mode);
      size_t chap_page = 0U;
      fails += download_chapter(net, site, series_url, curl, chap_dir, &chap_page, &pol, timeout);
      if (format != k_mdl_fmt_loose) {
        fails += export_one(format, series_dir, curl);
      }
    }
    ++got_ch;
  }

  if (one_file) {
    fails += export_combined(format, series_dir, combined_dir, slug, lo, hi, got_ch);
  }
  (void)printf("done: %zu chapter(s) into %s/ (%zu failure(s))\n", got_ch, series_dir, fails);
  return fails;
}

/** @brief series mode: config + series URL -> download N chapters. */
RA8_INTERNAL static int run_series(const char*  cfg_path,
                                   const char*  series_url,
                                   const char*  out_dir,
                                   mdl_format_t format,
                                   bool         combine,
                                   size_t       chapters,
                                   size_t       start,
                                   uint64_t     seed,
                                   uint32_t     timeout)
{
  mdl_site_t site;
  if (mdl_config_load(cfg_path, &site) != k_ra8_ok) {
    return 1;
  }
  (void)printf("site: %s (host %s, kind %s)\n", site.name, site.host, site.kind);

  mdl_net_iface_t* net = mdl_net_curl_create();
  if (net == nullptr) {
    (void)fprintf(stderr, "media_dl: network init failed\n");
    return 1;
  }

  ra8_err_t rc = prepare_chapters(net, &site, series_url, timeout);
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
  last_segment(series_url, series_slug, sizeof(series_slug));
  char series_dir[k_dir_path_bytes];
  (void)snprintf(series_dir, sizeof(series_dir), "%s/%s", out_dir, series_slug);
  (void)mkdir(out_dir, (mode_t)k_dir_mode);
  (void)mkdir(series_dir, (mode_t)k_dir_mode);

  /* Archive export spawns the archiver with cwd set to the chapter dir, so the
   * output paths must be absolute -- resolve the series dir now that it exists. */
  char abs_dir[PATH_MAX];
  if (realpath(series_dir, abs_dir) == nullptr) {
    (void)fprintf(stderr, "media_dl: cannot resolve %s\n", series_dir);
    mdl_net_curl_destroy(net);
    return 1;
  }

  const size_t fails = download_chapters(net,
                                         &site,
                                         series_url,
                                         abs_dir,
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

/** @brief page mode: fetch one URL, download its `<img>` URLs (debug path). */
RA8_INTERNAL static int run_page(const char* url,
                                 const char* out_dir,
                                 const char* attr,
                                 uint32_t    max_imgs,
                                 uint64_t    seed,
                                 uint32_t    timeout)
{
  mdl_net_iface_t* net = mdl_net_curl_create();
  if (net == nullptr) {
    (void)fprintf(stderr, "media_dl: network init failed\n");
    return 1;
  }
  const mdl_net_req_t req = {.user_agent = k_user_agent, .referer = nullptr, .timeout_ms = timeout};
  size_t              len = 0U;
  ra8_err_t           rc  = mdl_net_get_buf(net, url, &req, s_page, sizeof(s_page), &len);
  if (rc != k_ra8_ok) {
    (void)fprintf(stderr, "media_dl: fetch failed (err 0x%X)\n", (unsigned)rc);
    mdl_net_curl_destroy(net);
    return 1;
  }
  (void)mdl_extract_images(s_page, len, url, attr, nullptr, &s_images);
  (void)printf("found %zu image(s)\n", s_images.count);
  (void)mkdir(out_dir, (mode_t)k_dir_mode);

  mdl_politeness_t pol;
  mdl_politeness_init(&pol, seed);
  const size_t limit = (max_imgs == 0U) ? s_images.count : (size_t)max_imgs;
  size_t       fail  = 0U;
  for (size_t i = 0U; (i < s_images.count) && (i < limit); ++i) {
    (void)mdl_politeness_wait(&pol, (uint32_t)k_page_img_delay_min, (uint32_t)k_page_img_delay_max);
    char path[k_file_path_bytes];
    (void)
      snprintf(path, sizeof(path), "%s/page_%03zu.%s", out_dir, i + 1U, ext_of(s_images.urls[i]));
    const mdl_net_req_t ir  = {.user_agent = k_user_agent, .referer = url, .timeout_ms = timeout};
    size_t              got = 0U;
    if (mdl_net_get_file(net, s_images.urls[i], &ir, path, &got) != k_ra8_ok) {
      ++fail;
    }
  }
  (void)printf("done: %zu ok, %zu failed, into %s/\n",
               (s_images.count < limit ? s_images.count : limit) - fail,
               fail,
               out_dir);
  mdl_net_curl_destroy(net);
  return (fail == 0U) ? 0 : 1;
}

/** @brief Print usage to stderr. */
RA8_INTERNAL static void usage(const char* a0)
{
  (void)fprintf(stderr,
                "usage:\n"
                "  series: %s --config SITE.conf --series URL [--chapters N] "
                "[--start K] [--out DIR] "
                "[--format cbz|cbt|cbr|cbt.xz|cbt.gz|epub|jof|rabook] "
                "[--separate] [--seed S] [--timeout MS]\n"
                "          N chapters combine into ONE <slug>-<lo>-<hi>.<ext> by "
                "default; --separate keeps one archive per chapter.\n"
                "  pack:   %s --pack DIR --format FMT   "
                "package an existing folder of page images (no network)\n"
                "  page:   %s URL [--out DIR] [--max N] [--attr data-src|src] "
                "[--seed S] [--timeout MS]\n",
                a0,
                a0,
                a0);
}

/** @brief Parsed command-line options in string form (converted by main). */
typedef struct {
  const char* cfg;      /**< --config path.                                          */
  const char* series;   /**< --series URL.                                           */
  const char* page_url; /**< positional page URL (page mode).                        */
  const char* out;      /**< --out dir.                                              */
  const char* attr;     /**< --attr.                                                 */
  const char* chapters; /**< --chapters.                                             */
  const char* start;    /**< --start.                                                */
  const char* max;      /**< --max.                                                  */
  const char* seed;     /**< --seed.                                                 */
  const char* timeout;  /**< --timeout.                                              */
  const char* format;   /**< --format (cbz/cbt/cbr/cbt.xz/cbt.gz/epub/jof/rabook).   */
  const char* pack;     /**< --pack DIR: package an existing folder, no network.     */
  bool        separate; /**< --separate: one archive per chapter (default: combine). */
  bool        bad;      /**< An unrecognised argument was seen.                      */
} mdl_args_t;

/** @brief If argv[*i] == `flag`, store its value in *dst and advance `*i`. */
RA8_INTERNAL static bool take_opt(char** argv, int argc, int* i, const char* flag, const char** dst)
{
  if ((argv[*i] == nullptr) || (strcmp(argv[*i], flag) != 0)) {
    return false;
  }
  if ((*i + 1) < argc) {
    *i += 1;
    *dst = argv[*i];
  }
  return true;
}

/** @brief Parse argv into `a`; numeric fields stay as strings for main. */
RA8_INTERNAL static void parse_args(int argc, char** argv, mdl_args_t* a)
{
  /* Table-driven long options: each entry binds a flag to the field it fills. */
  const struct {
    const char*  flag; /**< Long-option spelling, including the leading "--". */
    const char** dst;  /**< Field in @p a that receives the option's value.   */
  } opts[] = {
    {"--config", &a->cfg},
    {"--series", &a->series},
    {"--out", &a->out},
    {"--attr", &a->attr},
    {"--chapters", &a->chapters},
    {"--start", &a->start},
    {"--max", &a->max},
    {"--seed", &a->seed},
    {"--timeout", &a->timeout},
    {"--format", &a->format},
    {"--pack", &a->pack},
  };
  for (int i = 1; i < argc; ++i) {
    if ((argv[i] != nullptr) && (strcmp(argv[i], "--separate") == 0)) {
      a->separate = true;
      continue;
    }
    bool matched = false;
    for (size_t k = 0U; (k < (sizeof(opts) / sizeof(opts[0]))) && !matched; ++k) {
      matched = take_opt(argv, argc, &i, opts[k].flag, opts[k].dst);
    }
    if (matched) {
      continue;
    }
    if ((argv[i] != nullptr) && (argv[i][0] != '-')) {
      a->page_url = argv[i];
      continue;
    }
    a->bad = true;
  }
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
  (void)snprintf(out, sizeof(out), "%s.%s", abs, ext);
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
  parse_args(argc, argv, &a);
  if (a.bad) {
    usage(argv[0]);
    return 2;
  }

  const uint64_t seed    = (a.seed == nullptr) ? 1UL : strtoull(a.seed, nullptr, k_dec_base);
  const uint32_t timeout = (uint32_t)to_ul(a.timeout, (unsigned long)k_req_timeout_def);
  const size_t   start   = (size_t)to_ul(a.start, 0UL);

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
    return run_series(a.cfg, a.series, a.out, format, !a.separate, chapters, start, seed, timeout);
  }
  if (a.page_url != nullptr) {
    const uint32_t max_imgs = (uint32_t)to_ul(a.max, 0UL);
    return run_page(a.page_url, a.out, a.attr, max_imgs, seed, timeout);
  }
  usage(argv[0]);
  return 2;
}
