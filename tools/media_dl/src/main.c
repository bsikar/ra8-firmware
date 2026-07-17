/*
 * Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
/**
 * @file main.c
 * @brief media_dl -- v0 host CLI: fetch a page, extract <img> URLs, download.
 *
 * @details
 * The smallest useful slice of the downloader, running as a native host binary
 * that links the firmware error contract (`ra8_err_t`). It fetches one page,
 * scans it for image URLs, and downloads them politely into a folder. No CBZ,
 * no image conversion, no per-site descriptors yet -- those are later
 * milestones. Everything above the injected libcurl backend is the same code
 * that will run on the RA8.
 *
 * Usage:
 *   media_dl <page-url> [--out DIR] [--max N] [--attr data-src|src]
 *            [--seed S] [--timeout MS]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "mdl_extract.h"
#include "mdl_net.h"
#include "mdl_politeness.h"
#include "ra8_err.h"

/** @brief Fixed sizing for the v0 CLI (zero dynamic alloc for app buffers). */
typedef enum : uint32_t {
  k_page_buf_bytes  = 8U * 1024U * 1024U, /**< Max HTML page size. */
  k_img_delay_min   = 400U,               /**< Per-image spacing floor, ms. */
  k_img_delay_max   = 800U,               /**< Per-image spacing ceiling, ms. */
  k_req_timeout_def = 20000U,             /**< Default per-request budget, ms. */
} mdl_cli_limits_t;

/* One User-Agent per session (v0 uses a single realistic string; the rotating
 * UA table is a later feature -- and rotating per request is an anti-pattern). */
static const char* const k_user_agent =
    "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/126.0.0.0 Safari/537.36";

/* Large fixed buffers live in .bss, not on the stack. */
static char           s_page[k_page_buf_bytes];
static mdl_url_list_t s_images;

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

/** @brief Print usage to stderr. */
static void usage(const char* argv0)
{
  (void)fprintf(stderr,
                "usage: %s <page-url> [--out DIR] [--max N] "
                "[--attr data-src|src] [--seed S] [--timeout MS]\n",
                argv0);
}

int main(int argc, char** argv)
{
  if (argc < 2 || argv[1][0] == '-') {
    usage(argv[0]);
    return 2;
  }
  const char* url      = argv[1];
  const char* out_dir  = "out";
  const char* attr     = "data-src";
  uint32_t    max_imgs = 0U; /* 0 = all */
  uint64_t    seed     = 1U;
  uint32_t    timeout  = (uint32_t)k_req_timeout_def;

  for (int i = 2; i < argc; ++i) {
    if ((strcmp(argv[i], "--out") == 0) && (i + 1 < argc)) {
      out_dir = argv[++i];
    } else if ((strcmp(argv[i], "--max") == 0) && (i + 1 < argc)) {
      max_imgs = (uint32_t)strtoul(argv[++i], nullptr, 10);
    } else if ((strcmp(argv[i], "--attr") == 0) && (i + 1 < argc)) {
      attr = argv[++i];
    } else if ((strcmp(argv[i], "--seed") == 0) && (i + 1 < argc)) {
      seed = strtoull(argv[++i], nullptr, 10);
    } else if ((strcmp(argv[i], "--timeout") == 0) && (i + 1 < argc)) {
      timeout = (uint32_t)strtoul(argv[++i], nullptr, 10);
    } else {
      usage(argv[0]);
      return 2;
    }
  }

  mdl_net_iface_t* net = mdl_net_curl_create();
  if (net == nullptr) {
    (void)fprintf(stderr, "media_dl: network init failed\n");
    return 1;
  }

  /* 1. Fetch the page. */
  const mdl_net_req_t page_req = {
      .user_agent = k_user_agent, .referer = nullptr, .timeout_ms = timeout};
  size_t    page_len = 0U;
  ra8_err_t rc = mdl_net_get_buf(net, url, &page_req, s_page, sizeof(s_page),
                                 &page_len);
  if (rc != k_ra8_ok) {
    (void)fprintf(stderr, "media_dl: fetch failed for %s (err 0x%X)\n", url,
                  (unsigned)rc);
    mdl_net_curl_destroy(net);
    return 1;
  }
  (void)printf("fetched %s (%zu bytes)\n", url, page_len);

  /* 2. Extract image URLs. */
  rc = mdl_extract_images(s_page, page_len, url, attr, &s_images);
  if ((rc != k_ra8_ok) && (rc != k_ra8_err_no_mem)) {
    (void)fprintf(stderr, "media_dl: extract failed (err 0x%X)\n", (unsigned)rc);
    mdl_net_curl_destroy(net);
    return 1;
  }
  (void)printf("found %zu image(s) via attr '%s'%s\n", s_images.count, attr,
               (rc == k_ra8_err_no_mem) ? " (capped)" : "");
  if (s_images.count == 0U) {
    (void)fprintf(stderr,
                  "media_dl: no images -- try --attr %s\n",
                  (strcmp(attr, "data-src") == 0) ? "src" : "data-src");
    mdl_net_curl_destroy(net);
    return 1;
  }

  /* 3. Download them politely into out_dir. */
  (void)mkdir(out_dir, 0755);
  mdl_politeness_t pol;
  mdl_politeness_init(&pol, seed);

  const size_t limit =
      (max_imgs == 0U) ? s_images.count : (size_t)max_imgs;
  size_t done = 0U;
  size_t fail = 0U;
  for (size_t i = 0U; (i < s_images.count) && (i < limit); ++i) {
    (void)mdl_politeness_wait(&pol, (uint32_t)k_img_delay_min,
                              (uint32_t)k_img_delay_max);
    char path[1200];
    (void)snprintf(path, sizeof(path), "%s/page_%03zu.%s", out_dir, i + 1U,
                   ext_of(s_images.urls[i]));

    const mdl_net_req_t img_req = {
        .user_agent = k_user_agent, .referer = url, .timeout_ms = timeout};
    size_t    got   = 0U;
    ra8_err_t irc = mdl_net_get_file(net, s_images.urls[i], &img_req, path,
                                     &got);
    if (irc == k_ra8_ok) {
      (void)printf("  [%zu/%zu] %s (%zu bytes)\n", i + 1U,
                   (s_images.count < limit) ? s_images.count : limit, path,
                   got);
      ++done;
    } else {
      (void)fprintf(stderr, "  [%zu] FAILED %s (err 0x%X)\n", i + 1U,
                    s_images.urls[i], (unsigned)irc);
      ++fail;
    }
  }

  (void)printf("done: %zu downloaded, %zu failed, into %s/\n", done, fail,
               out_dir);
  mdl_net_curl_destroy(net);
  return (fail == 0U) ? 0 : 1;
}
