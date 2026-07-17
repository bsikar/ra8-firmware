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
 *    a series' chapters, and download the first N as folders of page images.
 *  - **page** (bare `URL`): fetch one page and download its `<img>` URLs -- a
 *    debug/inspection path.
 *
 * No CBZ packaging or image conversion yet; those are the next milestones.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "mdl_config.h"
#include "mdl_extract.h"
#include "mdl_net.h"
#include "mdl_politeness.h"
#include "ra8_err.h"

/** @brief Fixed sizing for the CLI (large buffers live in .bss). */
typedef enum : uint32_t {
  k_page_buf_bytes  = 8U * 1024U * 1024U, /**< Max HTML page size. */
  k_req_timeout_def = 25000U,             /**< Default per-request budget, ms. */
} mdl_cli_limits_t;

/* One User-Agent per session (rotating per request is an anti-pattern). */
static const char* const k_user_agent =
    "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/126.0.0.0 Safari/537.36";

static char           s_page[k_page_buf_bytes];
static mdl_url_list_t s_chapters;
static mdl_url_list_t s_images;
static char           s_rowtmp[k_mdl_url_max];

/** @brief Pick a file extension from a URL, defaulting to "jpg". */
static const char* ext_of(const char* url)
{
  const char* slash = strrchr(url, '/');
  const char* seg   = (slash == nullptr) ? url : slash + 1;
  const char* dot   = strrchr(seg, '.');
  if (dot == nullptr) {
    return "jpg";
  }
  static const char* const k_known[] = {"jpg", "jpeg", "png",
                                         "gif", "webp", "bmp"};
  static char ext[8];
  size_t      n = 0U;
  for (const char* c = dot + 1; (*c != '\0') && (*c != '?') && (*c != '#') &&
                                (n + 1U < sizeof(ext));
       ++c) {
    ext[n] = (char)(((*c >= 'A') && (*c <= 'Z')) ? (*c + 32) : *c);
    ++n;
  }
  ext[n] = '\0';
  for (size_t i = 0U; i < (sizeof(k_known) / sizeof(k_known[0])); ++i) {
    if (strcmp(ext, k_known[i]) == 0) {
      return ext;
    }
  }
  return "jpg";
}

/** @brief Last non-empty path segment of a URL, sanitised, into `out`. */
static void last_segment(const char* url, char* out, size_t cap)
{
  const char*  q   = strpbrk(url, "?#");
  size_t       end = (q == nullptr) ? strlen(url) : (size_t)(q - url);
  while ((end > 0U) && (url[end - 1U] == '/')) {
    --end; /* trim trailing slashes */
  }
  size_t start = end;
  while ((start > 0U) && (url[start - 1U] != '/')) {
    --start;
  }
  size_t n = 0U;
  for (size_t i = start; (i < end) && (n + 1U < cap); ++i) {
    const char c = url[i];
    const bool ok = ((c >= 'A') && (c <= 'Z')) || ((c >= 'a') && (c <= 'z')) ||
                    ((c >= '0') && (c <= '9')) || (c == '.') || (c == '-') ||
                    (c == '_');
    out[n] = ok ? c : '_';
    ++n;
  }
  out[n] = '\0';
  if (n == 0U) {
    (void)snprintf(out, cap, "item");
  }
}

/** @brief Parse the last run of digits in `url` (0 if none). */
static long chapter_num(const char* url)
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
  char num[16];
  size_t n = 0U;
  for (size_t i = s; (i < e) && (n + 1U < sizeof(num)); ++i) {
    num[n++] = url[i];
  }
  num[n] = '\0';
  return strtol(num, nullptr, 10);
}

/** @brief Swap two rows of a URL list. */
static void swap_rows(mdl_url_list_t* l, size_t a, size_t b)
{
  if (a == b) {
    return;
  }
  memcpy(s_rowtmp, l->urls[a], k_mdl_url_max);
  memcpy(l->urls[a], l->urls[b], k_mdl_url_max);
  memcpy(l->urls[b], s_rowtmp, k_mdl_url_max);
}

/** @brief Apply the configured chapter ordering in place. */
static void apply_order(mdl_url_list_t* l, mdl_chapter_order_t order)
{
  if (order == k_mdl_order_reverse) {
    for (size_t i = 0U; i < (l->count / 2U); ++i) {
      swap_rows(l, i, l->count - 1U - i);
    }
  } else if (order == k_mdl_order_asc) {
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
}

/** @brief Keep only list entries whose URL starts with `prefix`. */
static void filter_prefix(mdl_url_list_t* l, const char* prefix)
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
 * @brief Download one chapter into `series_dir`; returns failed-image count.
 */
static size_t download_chapter(mdl_net_iface_t* net, const mdl_site_t* site,
                               const char* series_url, const char* chapter_url,
                               const char* series_dir, mdl_politeness_t* pol,
                               uint32_t timeout)
{
  const mdl_net_req_t page_req = {
      .user_agent = k_user_agent, .referer = series_url, .timeout_ms = timeout};
  size_t    page_len = 0U;
  ra8_err_t rc =
      mdl_net_get_buf(net, chapter_url, &page_req, s_page, sizeof(s_page),
                      &page_len);
  if (rc != k_ra8_ok) {
    (void)fprintf(stderr, "  chapter fetch failed %s (err 0x%X)\n", chapter_url,
                  (unsigned)rc);
    return 1U;
  }

  rc = mdl_extract_images(s_page, page_len, chapter_url, site->page_img_attr,
                          site->page_img_url_contains, &s_images);
  if ((rc != k_ra8_ok) && (rc != k_ra8_err_no_mem)) {
    (void)fprintf(stderr, "  image extract failed (err 0x%X)\n", (unsigned)rc);
    return 1U;
  }

  char chap[128];
  last_segment(chapter_url, chap, sizeof(chap));
  char dir[1024];
  (void)snprintf(dir, sizeof(dir), "%s/%s", series_dir, chap);
  (void)mkdir(dir, 0755);
  (void)printf("  %s: %zu page(s)\n", chap, s_images.count);

  size_t fail = 0U;
  for (size_t i = 0U; i < s_images.count; ++i) {
    (void)mdl_politeness_wait(pol, site->img_delay_min, site->img_delay_max);
    char path[1200];
    (void)snprintf(path, sizeof(path), "%s/page_%03zu.%s", dir, i + 1U,
                   ext_of(s_images.urls[i]));
    const mdl_net_req_t img_req = {.user_agent = k_user_agent,
                                   .referer     = chapter_url,
                                   .timeout_ms  = timeout};
    size_t    got = 0U;
    ra8_err_t irc =
        mdl_net_get_file(net, s_images.urls[i], &img_req, path, &got);
    if (irc != k_ra8_ok) {
      (void)fprintf(stderr, "    page %zu FAILED (err 0x%X)\n", i + 1U,
                    (unsigned)irc);
      ++fail;
    }
  }
  return fail;
}

/** @brief series mode: config + series URL -> download N chapters. */
static int run_series(const char* cfg_path, const char* series_url,
                      const char* out_dir, size_t chapters, size_t start,
                      uint64_t seed, uint32_t timeout)
{
  mdl_site_t site;
  if (mdl_config_load(cfg_path, &site) != k_ra8_ok) {
    return 1;
  }
  (void)printf("site: %s (host %s, kind %s)\n", site.name, site.host,
               site.kind);

  mdl_net_iface_t* net = mdl_net_curl_create();
  if (net == nullptr) {
    (void)fprintf(stderr, "media_dl: network init failed\n");
    return 1;
  }

  const mdl_net_req_t req = {
      .user_agent = k_user_agent, .referer = nullptr, .timeout_ms = timeout};
  size_t    len = 0U;
  ra8_err_t rc =
      mdl_net_get_buf(net, series_url, &req, s_page, sizeof(s_page), &len);
  if (rc != k_ra8_ok) {
    (void)fprintf(stderr, "media_dl: series fetch failed (err 0x%X)\n",
                  (unsigned)rc);
    mdl_net_curl_destroy(net);
    return 1;
  }

  (void)mdl_extract_anchors(s_page, len, series_url, site.chapter_url_contains,
                            &s_chapters);
  /* Keep only this series' chapters (drop cross-series relation links). */
  char prefix[1024];
  (void)snprintf(prefix, sizeof(prefix), "%s", series_url);
  size_t pl = strlen(prefix);
  if ((pl > 0U) && (prefix[pl - 1U] != '/') && (pl + 1U < sizeof(prefix))) {
    prefix[pl]      = '/';
    prefix[pl + 1U] = '\0';
  }
  filter_prefix(&s_chapters, prefix);
  apply_order(&s_chapters, site.chapter_order);
  (void)printf("chapters found: %zu\n", s_chapters.count);
  if (s_chapters.count == 0U) {
    (void)fprintf(stderr,
                  "media_dl: no chapters (check chapter_url_contains)\n");
    mdl_net_curl_destroy(net);
    return 1;
  }

  char series_slug[128];
  last_segment(series_url, series_slug, sizeof(series_slug));
  char series_dir[1024];
  (void)snprintf(series_dir, sizeof(series_dir), "%s/%s", out_dir,
                 series_slug);
  (void)mkdir(out_dir, 0755);
  (void)mkdir(series_dir, 0755);

  mdl_politeness_t pol;
  mdl_politeness_init(&pol, seed);

  size_t got_ch = 0U;
  size_t fails  = 0U;
  for (size_t i = start; (i < s_chapters.count) && (got_ch < chapters); ++i) {
    if (got_ch > 0U) {
      (void)mdl_politeness_wait(&pol, site.chapter_delay_min,
                                site.chapter_delay_max);
    }
    fails += download_chapter(net, &site, series_url, s_chapters.urls[i],
                              series_dir, &pol, timeout);
    ++got_ch;
  }

  (void)printf("done: %zu chapter(s) into %s/ (%zu page failure(s))\n", got_ch,
               series_dir, fails);
  mdl_net_curl_destroy(net);
  return (fails == 0U) ? 0 : 1;
}

/** @brief page mode: fetch one URL, download its <img> URLs (debug path). */
static int run_page(const char* url, const char* out_dir, const char* attr,
                    uint32_t max_imgs, uint64_t seed, uint32_t timeout)
{
  mdl_net_iface_t* net = mdl_net_curl_create();
  if (net == nullptr) {
    (void)fprintf(stderr, "media_dl: network init failed\n");
    return 1;
  }
  const mdl_net_req_t req = {
      .user_agent = k_user_agent, .referer = nullptr, .timeout_ms = timeout};
  size_t    len = 0U;
  ra8_err_t rc =
      mdl_net_get_buf(net, url, &req, s_page, sizeof(s_page), &len);
  if (rc != k_ra8_ok) {
    (void)fprintf(stderr, "media_dl: fetch failed (err 0x%X)\n", (unsigned)rc);
    mdl_net_curl_destroy(net);
    return 1;
  }
  (void)mdl_extract_images(s_page, len, url, attr, nullptr, &s_images);
  (void)printf("found %zu image(s)\n", s_images.count);
  (void)mkdir(out_dir, 0755);

  mdl_politeness_t pol;
  mdl_politeness_init(&pol, seed);
  const size_t limit = (max_imgs == 0U) ? s_images.count : (size_t)max_imgs;
  size_t       fail  = 0U;
  for (size_t i = 0U; (i < s_images.count) && (i < limit); ++i) {
    (void)mdl_politeness_wait(&pol, 400U, 800U);
    char path[1200];
    (void)snprintf(path, sizeof(path), "%s/page_%03zu.%s", out_dir, i + 1U,
                   ext_of(s_images.urls[i]));
    const mdl_net_req_t ir = {
        .user_agent = k_user_agent, .referer = url, .timeout_ms = timeout};
    size_t got = 0U;
    if (mdl_net_get_file(net, s_images.urls[i], &ir, path, &got) != k_ra8_ok) {
      ++fail;
    }
  }
  (void)printf("done: %zu ok, %zu failed, into %s/\n",
               (s_images.count < limit ? s_images.count : limit) - fail, fail,
               out_dir);
  mdl_net_curl_destroy(net);
  return (fail == 0U) ? 0 : 1;
}

/** @brief Print usage to stderr. */
static void usage(const char* a0)
{
  (void)fprintf(stderr,
                "usage:\n"
                "  series: %s --config SITE.conf --series URL [--chapters N] "
                "[--start K] [--out DIR] [--seed S] [--timeout MS]\n"
                "  page:   %s URL [--out DIR] [--max N] [--attr data-src|src] "
                "[--seed S] [--timeout MS]\n",
                a0, a0);
}

int main(int argc, char** argv)
{
  const char* cfg      = nullptr;
  const char* series   = nullptr;
  const char* page_url = nullptr;
  const char* out_dir  = "downloads";
  const char* attr     = "data-src";
  size_t      chapters = 1U;
  size_t      start    = 0U;
  uint32_t    max_imgs = 0U;
  uint64_t    seed     = 1U;
  uint32_t    timeout  = (uint32_t)k_req_timeout_def;

  for (int i = 1; i < argc; ++i) {
    if ((strcmp(argv[i], "--config") == 0) && (i + 1 < argc)) {
      cfg = argv[++i];
    } else if ((strcmp(argv[i], "--series") == 0) && (i + 1 < argc)) {
      series = argv[++i];
    } else if ((strcmp(argv[i], "--chapters") == 0) && (i + 1 < argc)) {
      chapters = (size_t)strtoul(argv[++i], nullptr, 10);
    } else if ((strcmp(argv[i], "--start") == 0) && (i + 1 < argc)) {
      start = (size_t)strtoul(argv[++i], nullptr, 10);
    } else if ((strcmp(argv[i], "--out") == 0) && (i + 1 < argc)) {
      out_dir = argv[++i];
    } else if ((strcmp(argv[i], "--max") == 0) && (i + 1 < argc)) {
      max_imgs = (uint32_t)strtoul(argv[++i], nullptr, 10);
    } else if ((strcmp(argv[i], "--attr") == 0) && (i + 1 < argc)) {
      attr = argv[++i];
    } else if ((strcmp(argv[i], "--seed") == 0) && (i + 1 < argc)) {
      seed = strtoull(argv[++i], nullptr, 10);
    } else if ((strcmp(argv[i], "--timeout") == 0) && (i + 1 < argc)) {
      timeout = (uint32_t)strtoul(argv[++i], nullptr, 10);
    } else if (argv[i][0] != '-') {
      page_url = argv[i];
    } else {
      usage(argv[0]);
      return 2;
    }
  }

  if (cfg != nullptr) {
    if (series == nullptr) {
      (void)fprintf(stderr, "media_dl: --config requires --series URL\n");
      return 2;
    }
    if (chapters == 0U) {
      chapters = 1U;
    }
    return run_series(cfg, series, out_dir, chapters, start, seed, timeout);
  }
  if (page_url != nullptr) {
    return run_page(page_url, out_dir, attr, max_imgs, seed, timeout);
  }
  usage(argv[0]);
  return 2;
}
