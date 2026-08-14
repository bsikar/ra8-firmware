/**
 * @file test_media_dl_fetch.c
 * @brief Host unit tests for the #305 resumable/incremental/deduping download
 *        loop, driven end-to-end through the #310 mdl_net vtable mock.
 *
 * @details
 * A scripted fake ::mdl_net_iface_t backend serves chapter HTML for `get_buf`
 * and writes deterministic, URL-derived page bytes for `get_file` (so the same
 * image URL always yields the same content, and a torn/absent page differs),
 * with no network. A temp directory holds the output. The suite asserts the
 * four behaviours the issue's acceptance criteria call out:
 *   - a first run fetches every page; a second run over the same series with a
 *     newly-added chapter (`--update`) fetches ONLY the new chapter, without
 * even requesting the HTML of the chapters already complete;
 *   - an interrupted download resumes to a result byte-identical to an
 *     uninterrupted one, with the same combined page numbering;
 *   - a byte-identical image already held is reused across chapters rather than
 *     re-fetched (content-hash dedup);
 *   - a corrupt state file degrades to a clean rebuild rather than a crash.
 * Uses the repo's `unity_minimal.h` harness, mirroring `tests/test_*.c`.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "mdl_config.h"
#include "mdl_extract.h"
#include "mdl_fetch.h"
#include "mdl_fetch_internal.h"
#include "mdl_hash.h"
#include "mdl_net.h"
#include "mdl_session.h"
#include "mdl_state.h"
#include "mdl_urlname.h"
#include "ra8_err.h"
#include "unity_minimal.h"

/** @brief Named constants for the fetch tests (no bare literals). */
typedef enum : uint16_t {
  k_map_max = 8,        /**< Chapter->HTML map slots.                      */
  k_html_bytes = 512,   /**< Per-chapter HTML buffer bytes.                */
  k_page_bytes = 8192,  /**< Chapter-HTML scratch bytes.                   */
  k_tmpl_bytes = 64,    /**< mkdtemp template bytes.                       */
  k_req_timeout = 1000, /**< Per-request budget, ms.                       */
  k_list_max = 8,       /**< Chapter URLs in a scenario.                   */
  k_http_unavailable =
      503, /**< HTTP 503 the throttle-injection mock returns. */
} mdl_fetch_test_const_t;

/**
 * @brief Composed-path buffer size: sized past PATH_MAX plus a leaf so the
 *        compiler can prove a `<dir>/<page>` join never truncates (silences
 *        gcc -Wformat-truncation, since the dir source is a PATH_MAX array).
 */
typedef enum : uint32_t {
  k_join_bytes =
      (uint32_t)PATH_MAX + 128U, /**< PATH_MAX + a page-leaf margin. */
} mdl_fetch_join_t;

/**
 * @struct page_map_t
 * @brief One chapter URL mapped to the HTML the fake serves for it.
 * @since 0.1.0
 */
typedef struct {
  const char *url;  /**< Chapter page URL the fake recognises. */
  const char *html; /**< HTML body returned for that URL.      */
} page_map_t;

/**
 * @struct mock_net_t
 * @brief Fake ::mdl_net_iface_t backend: serves chapter HTML, writes page
 * bytes.
 * @details `get_buf` returns the mapped HTML for a chapter URL; `get_file`
 *          writes the URL's own bytes to the target (deterministic content) and
 *          can be scripted to fail on one call to simulate an interruption.
 * @invariant `get_file_calls` counts every page transfer attempt.
 * @since 0.1.0
 */
typedef struct {
  const page_map_t *map;          /**< Chapter URL -> HTML.              */
  size_t map_n;                   /**< Entries in @ref map.              */
  size_t get_buf_calls;           /**< Chapter-HTML fetches dispatched.  */
  size_t get_file_calls;          /**< Page transfers attempted.         */
  size_t fail_on_file_call;       /**< One call to fail once (0=never).  */
  size_t busy_on_file_call;       /**< One call to return 503 (0=off).   */
  const char *busy_retry_after;   /**< Retry-After for the busy reply.   */
  const char *fail_url;           /**< URL that fails on EVERY attempt.  */
  size_t not_mod_on_file_call;    /**< 1-based call to return 304.       */
  const char *resp_etag;          /**< ETag response header to return.   */
  const char *resp_last_modified; /**< Last-Modified response to return. */
  const char *resp_content_type;  /**< Content-Type response to return.  */
  const char *response_body;      /**< Optional 200 response bytes.      */
  const uint8_t *response_prefix; /**< Optional binary prefix.            */
  size_t response_prefix_len;     /**< Bytes in @ref response_prefix.    */
  char last_if_none_match[k_mdl_etag_max]; /**< Captured If-None-Match. */
  /** Captured If-Modified-Since value. */
  char last_if_mod_since[k_mdl_last_mod_max];
} mock_net_t;

/** @brief Minimal supported image signatures used by the scripted file backend.
 */
static const uint8_t s_jpeg_magic[] = {0xFFU, 0xD8U, 0xFFU};
/** @brief Complete PNG signature; the production sniffer only needs its prefix.
 */
static const uint8_t s_png_magic[] = {0x89U, 'P',   'N',   'G',
                                      0x0DU, 0x0AU, 0x1AU, 0x0AU};

/** @brief Fake get_buf: serve the mapped HTML for a chapter URL. */
static ra8_err_t mock_get_buf(void *ctx, const char *url,
                              const mdl_net_req_t *req, char *buf, size_t cap,
                              size_t *out_len, mdl_net_resp_t *resp) {
  (void)req;
  (void)resp;
  mock_net_t *f = (mock_net_t *)ctx;
  f->get_buf_calls += 1U;
  for (size_t i = 0U; i < f->map_n; ++i) {
    if (strcmp(f->map[i].url, url) == 0) {
      const int w = snprintf(buf, cap, "%s", f->map[i].html);
      if (out_len != nullptr) {
        *out_len = (w < 0) ? 0U : (size_t)w;
      }
      return k_ra8_ok;
    }
  }
  return k_ra8_fail;
}

/** @brief Fake get_file: write the URL's own bytes, unless this call is
 * scripted to fail. */
static ra8_err_t mock_get_file(void *ctx, const char *url,
                               const mdl_net_req_t *req, const char *out_path,
                               size_t *out_len, mdl_net_resp_t *resp) {
  mock_net_t *f = (mock_net_t *)ctx;
  f->get_file_calls += 1U;
  if (req != nullptr) {
    (void)snprintf(f->last_if_none_match, sizeof(f->last_if_none_match), "%s",
                   (req->if_none_match != nullptr) ? req->if_none_match : "");
    (void)snprintf(f->last_if_mod_since, sizeof(f->last_if_mod_since), "%s",
                   (req->if_modified_since != nullptr) ? req->if_modified_since
                                                       : "");
  }
  if (resp != nullptr) {
    if (f->resp_etag != nullptr) {
      (void)snprintf(resp->etag, sizeof(resp->etag), "%s", f->resp_etag);
    }
    if (f->resp_last_modified != nullptr) {
      (void)snprintf(resp->last_modified, sizeof(resp->last_modified), "%s",
                     f->resp_last_modified);
    }
    if (f->resp_content_type != nullptr) {
      (void)snprintf(resp->content_type, sizeof(resp->content_type), "%s",
                     f->resp_content_type);
    }
  }
  if ((f->not_mod_on_file_call != 0U) &&
      (f->get_file_calls == f->not_mod_on_file_call)) {
    if (resp != nullptr) {
      resp->status = 304;
    }
    if (out_len != nullptr) {
      *out_len = 0U;
    }
    return k_ra8_ok;
  }
  if ((f->busy_on_file_call != 0U) &&
      (f->get_file_calls == f->busy_on_file_call)) {
    if (resp != nullptr) {
      resp->status = (long)k_http_unavailable;
      (void)snprintf(resp->retry_after, sizeof(resp->retry_after), "%s",
                     (f->busy_retry_after != nullptr) ? f->busy_retry_after
                                                      : "");
    }
    return k_ra8_err_busy;
  }
  if ((f->fail_on_file_call != 0U) &&
      (f->get_file_calls == f->fail_on_file_call)) {
    return k_ra8_fail;
  }
  if ((f->fail_url != nullptr) && (strcmp(f->fail_url, url) == 0)) {
    return k_ra8_fail;
  }
  if (resp != nullptr) {
    resp->status = 200;
  }
  FILE *fp = fopen(out_path, "wb");
  if (fp == nullptr) {
    return k_ra8_fail;
  }
  const char *body = (f->response_body != nullptr) ? f->response_body : url;
  const size_t body_len = strlen(body);
  if ((f->response_prefix != nullptr) && (f->response_prefix_len > 0U)) {
    if (fwrite(f->response_prefix, 1U, f->response_prefix_len, fp) !=
        f->response_prefix_len) {
      (void)fclose(fp);
      return k_ra8_fail;
    }
  }
  (void)fwrite(body, 1U, body_len, fp);
  (void)fclose(fp);
  if (out_len != nullptr) {
    *out_len = f->response_prefix_len + body_len;
  }
  return k_ra8_ok;
}

/** @brief Fake destroy: the handle is stack-owned, nothing to release. */
static void mock_destroy(void *ctx) { (void)ctx; }

/** @brief The fake backend's method table. */
static const mdl_net_vtable_t s_mock_vtable = {
    .get_buf = mock_get_buf,
    .get_file = mock_get_file,
    .destroy = mock_destroy,
};

/* ---- shared fixtures (large objects live off the stack) ------------------ */

static mock_net_t g_mock;    /**< The scripted backend context.           */
static mdl_session_t g_sess; /**< Session over the fake (64 KiB embed).   */
static mdl_site_t g_site;    /**< Selectors + (zero) politeness bounds.   */
static mdl_state_t g_state;  /**< State under test (~2 MiB).              */
static mdl_url_list_t g_chapters; /**< Live chapter list for a scenario. */
static mdl_url_list_t g_images; /**< Extracted-image scratch.                */
static char g_page[k_page_bytes]; /**< Chapter-HTML scratch. */
static mdl_governor_t
    *g_fetch_gov; /**< Governor wired into run_fetch, or NULL. */
static mdl_fetch_faillog_t
    g_faillog;         /**< Failure log run_fetch fills each run.   */
static bool g_refetch; /**< Cache bypass for the current run.       */

/**
 * @struct fetch_clock_t
 * @brief Virtual clock for the governed integration test (no real sleeps).
 * @since 0.1.0
 */
typedef struct {
  int64_t now_ms;      /**< Virtual now (ms).            */
  int64_t total_slept; /**< Sum of governor sleeps (ms). */
} fetch_clock_t;

/** @brief Injected clock: return the virtual now. */
/* cppcheck-suppress constParameterCallback ; ra8_governor clock-fn ABI is void*
 */
static int64_t fetch_now(void *c) { return ((const fetch_clock_t *)c)->now_ms; }

/** @brief Injected sleeper: advance the virtual clock and tally the wait. */
static void fetch_sleep(void *c, uint32_t ms) {
  fetch_clock_t *k = (fetch_clock_t *)c;
  k->now_ms += (int64_t)ms;
  k->total_slept += (int64_t)ms;
}

/** @brief Configure the site descriptor the fake HTML is scraped against. */
static void setup_site(void) {
  memset(&g_mock, 0, sizeof(g_mock));
  memset(&g_site, 0, sizeof(g_site));
  g_refetch = false;
  g_mock.response_prefix = s_jpeg_magic;
  g_mock.response_prefix_len = sizeof(s_jpeg_magic);
  (void)snprintf(g_site.page_img_attr, sizeof(g_site.page_img_attr), "%s",
                 "data-src");
  g_site.page_img_url_contains[0] = '\0';
}

/** @brief Copy `n` chapter URLs into `g_chapters`. */
static void set_chapters(const char *const *urls, size_t n) {
  g_chapters.count = 0U;
  for (size_t i = 0U; (i < n) && (i < (size_t)k_list_max); ++i) {
    (void)snprintf(g_chapters.urls[g_chapters.count], k_mdl_url_max, "%s",
                   urls[i]);
    g_chapters.count += 1U;
  }
}

/** @brief Make a fresh temp directory (resolved) and its `.mdl_state` path. */
static void make_series_dir(char *abs_dir, size_t abs_cap, char *state_path,
                            size_t state_cap) {
  char tmpl[k_tmpl_bytes];
  (void)snprintf(tmpl, sizeof(tmpl), "%s", "/tmp/mdl_fetch_XXXXXX");
  const char *made = mkdtemp(tmpl);
  TEST_ASSERT_NOT_NULL(made);
  if (realpath(made, abs_dir) == nullptr) {
    (void)snprintf(abs_dir, abs_cap, "%s", made);
  }
  (void)snprintf(state_path, state_cap, "%s/.mdl_state", abs_dir);
}

/** @brief Run the fetch loop for one scenario; returns its result code. */
static ra8_err_t run_fetch(const char *abs_dir, const char *state_path,
                           mdl_fetch_layout_t layout, const char *combined_rel,
                           bool update_only, size_t fail_on_file_call,
                           mdl_fetch_stats_t *out) {
  g_mock.get_buf_calls = 0U;
  g_mock.get_file_calls = 0U;
  g_mock.fail_on_file_call = fail_on_file_call;
  memset(&g_faillog, 0, sizeof(g_faillog));
  mdl_net_iface_t iface = {.vtable = &s_mock_vtable, .ctx = &g_mock};
  mdl_session_init(&g_sess, &iface, "media_dl/test", false);
  mdl_fetch_ctx_t ctx = {.session = &g_sess,
                         .state = &g_state,
                         .state_path = state_path,
                         .series_abs_dir = abs_dir,
                         .series_url = "http://s/series",
                         .site = &g_site,
                         .gov = g_fetch_gov,
                         .timeout_ms = (uint32_t)k_req_timeout,
                         .page_buf = g_page,
                         .page_cap = sizeof(g_page),
                         .images = &g_images,
                         .update_only = update_only,
                         .refetch = g_refetch,
                         .faillog = &g_faillog,
                         .progress_fn = nullptr,
                         .progress_ctx = nullptr};
  return mdl_fetch_run(&ctx, &g_chapters, layout, combined_rel, out);
}

/** @brief True when two files hold byte-identical content. */
static bool files_equal(const char *a, const char *b) {
  uint64_t ha = 0U;
  uint64_t hb = 0U;
  return (mdl_hash_file(a, &ha) == k_ra8_ok) &&
         (mdl_hash_file(b, &hb) == k_ra8_ok) && (ha == hb);
}

/** @brief True when a file exists at the composed `<abs_dir>/<rel>` path. */
static bool page_exists(const char *abs_dir, const char *rel) {
  char path[k_join_bytes];
  struct stat st;
  (void)snprintf(path, sizeof(path), "%s/%s", abs_dir, rel);
  return stat(path, &st) == 0;
}

/** @brief Hash one series-relative file, failing the test when it is
 * unreadable. */
static uint64_t page_hash(const char *abs_dir, const char *rel) {
  char path[k_join_bytes];
  uint64_t hash = 0U;
  (void)snprintf(path, sizeof(path), "%s/%s", abs_dir, rel);
  TEST_ASSERT_EQ((int64_t)k_ra8_ok, mdl_hash_file(path, &hash));
  return hash;
}

/** @brief The chapter->HTML map used by the incremental test (three chapters).
 */
static const page_map_t s_map3[] = {
    {"http://s/chapter-1", "<h1 class=\"chapter-title\">Chapter One</h1><img "
                           "data-src=\"http://cdn/a.jpg\"><img "
                           "data-src=\"http://cdn/b.jpg\">"},
    {"http://s/chapter-2", "<h1 class=\"chapter-title\">Chapter Two</h1><img "
                           "data-src=\"http://cdn/c.jpg\">"},
    {"http://s/chapter-3", "<h1 class=\"chapter-title\">Chapter Three</h1><img "
                           "data-src=\"http://cdn/d.jpg\">"},
};

/** @brief One chapter whose extracted display title exceeds the state field. */
static const page_map_t s_map_title_overflow[] = {
    {"http://s/chapter-9",
     "<h1 class=\"chapter-title\">"
     "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
     "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
     "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc"
     "dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd"
     "</h1><img data-src=\"http://cdn/overflow.jpg\">"},
};

/** @brief Chapters used to qualify strict descriptor-driven number extraction.
 */
static const page_map_t s_map_number_selector[] = {
    {"http://s/chapter-special",
     "<span class=\"chapter-number\">108.5</span><img "
     "data-src=\"http://cdn/108-5.jpg\">"},
    {"http://s/chapter-invalid",
     "<span class=\"chapter-number\">108.5 bonus</span><img "
     "data-src=\"http://cdn/invalid-number.jpg\">"},
};

/**
 * @test test_chapter_number_selector_strict_and_persisted
 * @brief Verify configured number extraction overrides an unnumbered URL and
 * rejects junk.
 * @details Runs one chapter whose bounded class selector contains `108.5`, then
 *          one whose matched value has a trailing label. The former must be
 *          persisted exactly and the latter must fail before page transfer.
 * @pre The test process may create files under `/tmp`.
 * @pre Scripted fetch globals are exclusively owned by this test.
 * @post Valid selector text sets number-known and survives the state
 * checkpoint.
 * @post Malformed matched text records validation failure and fetches no page.
 * @note A selector miss has separate production fallback semantics; this test
 *       targets the more dangerous matched-but-malformed case.
 * @since 0.1.0
 */
static void test_chapter_number_selector_strict_and_persisted(void) {
  TEST_BEGIN("chapter number selector strict + persisted");
  setup_site();
  (void)snprintf(g_site.chapter_number_selector,
                 sizeof(g_site.chapter_number_selector), "%s",
                 "class:chapter-number");
  g_mock.map = s_map_number_selector;
  g_mock.map_n =
      sizeof(s_map_number_selector) / sizeof(s_map_number_selector[0]);
  char abs_dir[PATH_MAX];
  char state_path[PATH_MAX];
  make_series_dir(abs_dir, sizeof(abs_dir), state_path, sizeof(state_path));
  mdl_state_init(&g_state);

  const char *valid[] = {"http://s/chapter-special"};
  set_chapters(valid, 1U);
  mdl_fetch_stats_t valid_stats = {};
  TEST_ASSERT_EQ((int64_t)k_ra8_ok,
                 run_fetch(abs_dir, state_path, k_mdl_layout_separate, nullptr,
                           false, 0U, &valid_stats));
  const mdl_chapter_rec_t *chapter =
      mdl_state_find_chapter(&g_state, "chapter-special");
  TEST_ASSERT_NOT_NULL(chapter);
  TEST_ASSERT(chapter->number_known);
  TEST_ASSERT(chapter->number == 108.5);

  mdl_state_init(&g_state);
  memset(&g_faillog, 0, sizeof(g_faillog));
  g_mock.get_file_calls = 0U;
  const char *invalid[] = {"http://s/chapter-invalid"};
  set_chapters(invalid, 1U);
  mdl_fetch_stats_t invalid_stats = {};
  TEST_ASSERT_EQ((int64_t)k_ra8_fail,
                 run_fetch(abs_dir, state_path, k_mdl_layout_separate, nullptr,
                           false, 0U, &invalid_stats));
  TEST_ASSERT_EQ((int64_t)0, (int64_t)g_mock.get_file_calls);
  TEST_ASSERT_EQ((int64_t)1, (int64_t)g_faillog.count);
  TEST_ASSERT_EQ((int64_t)k_ra8_err_validation_failed,
                 (int64_t)g_faillog.items[0].err);
  TEST_END("chapter number selector strict + persisted");
}

/** @test Strict numeric text parsing consumes the complete trimmed selector
 * result. */
static void test_chapter_number_text_parser(void) {
  TEST_BEGIN("chapter number text parser strict");
  double number = 0.0;
  TEST_ASSERT(mdl_urlname_chapter_text_parse(" 108.5\n", &number));
  TEST_ASSERT(number == 108.5);
  TEST_ASSERT(mdl_urlname_chapter_text_parse("108-5", &number));
  TEST_ASSERT(number == 108.5);
  TEST_ASSERT(!mdl_urlname_chapter_text_parse("108.5 bonus", &number));
  TEST_ASSERT(number == 0.0);
  TEST_ASSERT(!mdl_urlname_chapter_text_parse("+108.5", &number));
  TEST_ASSERT(!mdl_urlname_chapter_text_parse("1e2", &number));
  TEST_ASSERT(!mdl_urlname_chapter_text_parse("nan", &number));
  TEST_ASSERT(!mdl_urlname_chapter_text_parse("1000000000", &number));
  TEST_ASSERT(!mdl_urlname_chapter_text_parse("108.1234567", &number));
  TEST_END("chapter number text parser strict");
}

/**
 * @test test_first_run_then_update_only_new
 *
 * @par MC/DC:
 * (No compound decision under test; it proves the incremental contract: a fresh
 * run fetches every page, and a later `--update` over a reloaded state with one
 * new chapter fetches ONLY that chapter -- issuing no HTML request for the
 * chapters already recorded complete.)
 */
static void test_first_run_then_update_only_new(void) {
  TEST_BEGIN("first run + update only new");
  setup_site();
  (void)snprintf(g_site.chapter_title_selector,
                 sizeof(g_site.chapter_title_selector), "%s",
                 "class:chapter-title");
  g_mock.map = s_map3;
  g_mock.map_n = sizeof(s_map3) / sizeof(s_map3[0]);
  char abs_dir[PATH_MAX];
  char state_path[PATH_MAX];
  make_series_dir(abs_dir, sizeof(abs_dir), state_path, sizeof(state_path));
  mdl_state_init(&g_state);

  /* Fresh run of chapters 1 and 2: every page is fetched. */
  const char *c12[] = {"http://s/chapter-1", "http://s/chapter-2"};
  set_chapters(c12, 2U);
  mdl_fetch_stats_t s1;
  TEST_ASSERT_EQ((int64_t)k_ra8_ok,
                 run_fetch(abs_dir, state_path, k_mdl_layout_separate, nullptr,
                           false, 0U, &s1));
  TEST_ASSERT_EQ((uint16_t)3, (uint16_t)s1.pages_fetched);
  TEST_ASSERT_EQ((uint16_t)0, (uint16_t)s1.pages_reused);
  TEST_ASSERT_EQ((uint16_t)2, (uint16_t)s1.chapters_completed);
  TEST_ASSERT(page_exists(abs_dir, "chapter-1/page_0001.jpg"));
  TEST_ASSERT(page_exists(abs_dir, "chapter-2/page_0001.jpg"));
  TEST_ASSERT(strcmp(mdl_state_find_chapter(&g_state, "chapter-1")->title,
                     "Chapter One") == 0);

  /* Reload the state from disk (simulating a fresh process). */
  TEST_ASSERT_EQ((int64_t)k_ra8_ok, mdl_state_load(state_path, &g_state));

  /* --update with chapter 3 newly present: only chapter 3 is fetched. */
  const char *c123[] = {"http://s/chapter-1", "http://s/chapter-2",
                        "http://s/chapter-3"};
  set_chapters(c123, 3U);
  mdl_fetch_stats_t s2;
  TEST_ASSERT_EQ((int64_t)k_ra8_ok,
                 run_fetch(abs_dir, state_path, k_mdl_layout_separate, nullptr,
                           true, 0U, &s2));
  TEST_ASSERT_EQ((uint16_t)2, (uint16_t)s2.chapters_skipped);
  TEST_ASSERT_EQ((uint16_t)1, (uint16_t)s2.chapters_completed);
  TEST_ASSERT_EQ((uint16_t)1, (uint16_t)s2.pages_fetched);
  /* Only chapter 3's HTML was requested; the complete chapters were untouched.
   */
  TEST_ASSERT_EQ((uint16_t)1, (uint16_t)g_mock.get_buf_calls);
  TEST_ASSERT_EQ((uint16_t)1, (uint16_t)g_mock.get_file_calls);
  TEST_ASSERT(page_exists(abs_dir, "chapter-3/page_0001.jpg"));
  TEST_END("first run + update only new");
}

/**
 * @test test_chapter_title_overflow_fails_without_page_fetch
 *
 * @brief Verify an overlong extracted chapter title is never truncated.
 * @details Drives a chapter whose configured title selector matches more bytes
 *          than the persistent title field and requires the run to fail before
 *          dispatching its otherwise-valid image URL.
 * @pre The test process may create files under `/tmp`.
 * @pre The scripted backend and global fetch fixtures are exclusively owned.
 * @post The chapter remains incomplete with an empty persisted title.
 * @post No page-file transfer is attempted.
 * @note Host-only test; the injected backend performs no network access.
 * @since 0.1.0
 */
static void test_chapter_title_overflow_fails_without_page_fetch(void) {
  TEST_BEGIN("chapter title overflow hard-fails");
  setup_site();
  (void)snprintf(g_site.chapter_title_selector,
                 sizeof(g_site.chapter_title_selector), "%s",
                 "class:chapter-title");
  g_mock.map = s_map_title_overflow;
  g_mock.map_n = sizeof(s_map_title_overflow) / sizeof(s_map_title_overflow[0]);
  char abs_dir[PATH_MAX];
  char state_path[PATH_MAX];
  make_series_dir(abs_dir, sizeof(abs_dir), state_path, sizeof(state_path));
  mdl_state_init(&g_state);
  const char *chapters[] = {"http://s/chapter-9"};
  set_chapters(chapters, 1U);
  mdl_fetch_stats_t stats = {};
  TEST_ASSERT_EQ((int64_t)k_ra8_fail,
                 run_fetch(abs_dir, state_path, k_mdl_layout_separate, nullptr,
                           false, 0U, &stats));
  TEST_ASSERT_EQ((int64_t)1, (int64_t)stats.chapters_failed);
  TEST_ASSERT_EQ((int64_t)0, (int64_t)g_mock.get_file_calls);
  TEST_ASSERT_EQ((int64_t)1, (int64_t)g_faillog.count);
  TEST_ASSERT_EQ((int64_t)k_ra8_err_invalid_size,
                 (int64_t)g_faillog.items[0].err);
  const mdl_chapter_rec_t *chapter =
      mdl_state_find_chapter(&g_state, "chapter-9");
  TEST_ASSERT_NOT_NULL(chapter);
  TEST_ASSERT(!chapter->complete);
  TEST_ASSERT(strcmp(chapter->title, "") == 0);
  TEST_END("chapter title overflow hard-fails");
}

/** @test --refetch bypasses an otherwise valid, reusable local page. */
static void test_refetch_bypasses_valid_cache(void) {
  TEST_BEGIN("refetch bypasses valid cache");
  setup_site();
  (void)snprintf(g_site.chapter_title_selector,
                 sizeof(g_site.chapter_title_selector), "%s",
                 "class:not-present");
  g_mock.map = s_map3;
  g_mock.map_n = sizeof(s_map3) / sizeof(s_map3[0]);
  char abs_dir[PATH_MAX];
  char state_path[PATH_MAX];
  make_series_dir(abs_dir, sizeof(abs_dir), state_path, sizeof(state_path));
  mdl_state_init(&g_state);
  const char *chapters[] = {"http://s/chapter-2"};
  set_chapters(chapters, 1U);
  mdl_fetch_stats_t first = {};
  TEST_ASSERT_EQ((int64_t)k_ra8_ok,
                 run_fetch(abs_dir, state_path, k_mdl_layout_separate, nullptr,
                           false, 0U, &first));
  TEST_ASSERT_EQ((int64_t)1, (int64_t)first.pages_fetched);
  TEST_ASSERT(strcmp(mdl_state_find_chapter(&g_state, "chapter-2")->title,
                     "chapter-2") == 0);

  g_refetch = true;
  mdl_fetch_stats_t second = {};
  TEST_ASSERT_EQ((int64_t)k_ra8_ok,
                 run_fetch(abs_dir, state_path, k_mdl_layout_separate, nullptr,
                           false, 0U, &second));
  TEST_ASSERT_EQ((int64_t)1, (int64_t)second.pages_fetched);
  TEST_ASSERT_EQ((int64_t)0, (int64_t)second.pages_reused);
  TEST_ASSERT_EQ((int64_t)1, (int64_t)g_mock.get_file_calls);
  TEST_END("refetch bypasses valid cache");
}

/** @brief The two-chapter map used by the combined resume test. */
static const page_map_t s_map_combined[] = {
    {"http://s/chapter-1",
     "<img data-src=\"http://cdn/a.jpg\"><img data-src=\"http://cdn/b.jpg\">"},
    {"http://s/chapter-2",
     "<img data-src=\"http://cdn/c.jpg\"><img data-src=\"http://cdn/d.jpg\">"},
};

/** @brief Assert the four combined page files under `abs_dir` are
 * `a`,`b`,`c`,`d`. */
static void assert_combined_pages_equal(const char *dir_a, const char *dir_b) {
  const char *rel[] = {"foo-1-2/page_0001.jpg", "foo-1-2/page_0002.jpg",
                       "foo-1-2/page_0003.jpg", "foo-1-2/page_0004.jpg"};
  for (size_t i = 0U; i < (sizeof(rel) / sizeof(rel[0])); ++i) {
    char pa[k_join_bytes];
    char pb[k_join_bytes];
    (void)snprintf(pa, sizeof(pa), "%s/%s", dir_a, rel[i]);
    (void)snprintf(pb, sizeof(pb), "%s/%s", dir_b, rel[i]);
    TEST_ASSERT(files_equal(pa, pb));
  }
}

/**
 * @test test_resume_equals_uninterrupted
 *
 * @par MC/DC:
 * (No compound decision under test; it proves an interrupted combined download,
 * resumed from its on-disk state, reproduces a byte-identical result with the
 * same continuous page numbering as an uninterrupted run -- the page-4 fetch
 * that failed is the only one repeated on resume, pages 1-3 being reused.)
 */
static void test_resume_equals_uninterrupted(void) {
  TEST_BEGIN("resume equals uninterrupted");
  setup_site();
  g_mock.map = s_map_combined;
  g_mock.map_n = sizeof(s_map_combined) / sizeof(s_map_combined[0]);
  const char *c12[] = {"http://s/chapter-1", "http://s/chapter-2"};

  /* (a) Uninterrupted combined run into dir A: pages 1..4, continuous. */
  char dir_a[PATH_MAX];
  char state_a[PATH_MAX];
  make_series_dir(dir_a, sizeof(dir_a), state_a, sizeof(state_a));
  mdl_state_init(&g_state);
  set_chapters(c12, 2U);
  mdl_fetch_stats_t sa;
  TEST_ASSERT_EQ((int64_t)k_ra8_ok,
                 run_fetch(dir_a, state_a, k_mdl_layout_combined, "foo-1-2",
                           false, 0U, &sa));
  TEST_ASSERT_EQ((uint16_t)4, (uint16_t)sa.pages_fetched);
  TEST_ASSERT(page_exists(dir_a, "foo-1-2/page_0004.jpg"));

  /* (b) Interrupted combined run into dir B: page 4 (d.jpg) is unreachable on
   *     every attempt, so its retries all fail and the chapter is left partial.
   */
  char dir_b[PATH_MAX];
  char state_b[PATH_MAX];
  make_series_dir(dir_b, sizeof(dir_b), state_b, sizeof(state_b));
  mdl_state_init(&g_state);
  set_chapters(c12, 2U);
  g_mock.fail_url = "http://cdn/d.jpg";
  mdl_fetch_stats_t sb;
  TEST_ASSERT_EQ((int64_t)k_ra8_fail,
                 run_fetch(dir_b, state_b, k_mdl_layout_combined, "foo-1-2",
                           false, 0U, &sb));
  TEST_ASSERT_EQ((uint16_t)1, (uint16_t)sb.chapters_failed);
  TEST_ASSERT(!page_exists(dir_b, "foo-1-2/page_0004.jpg"));

  /* (c) Resume from dir B's on-disk state, page 4 now reachable: only it is
   *     re-fetched. */
  g_mock.fail_url = nullptr;
  TEST_ASSERT_EQ((int64_t)k_ra8_ok, mdl_state_load(state_b, &g_state));
  set_chapters(c12, 2U);
  mdl_fetch_stats_t sc;
  TEST_ASSERT_EQ((int64_t)k_ra8_ok,
                 run_fetch(dir_b, state_b, k_mdl_layout_combined, "foo-1-2",
                           false, 0U, &sc));
  TEST_ASSERT_EQ((uint16_t)1,
                 (uint16_t)sc.pages_fetched); /* only the missing page */
  TEST_ASSERT_EQ((uint16_t)3,
                 (uint16_t)sc.pages_reused); /* pages 1..3 reused     */
  TEST_ASSERT(page_exists(dir_b, "foo-1-2/page_0004.jpg"));

  /* The resumed output equals the uninterrupted one, page for page. */
  assert_combined_pages_equal(dir_a, dir_b);
  TEST_END("resume equals uninterrupted");
}

/** @brief The map for the dedup test: chapters 1 and 2 share image A. */
static const page_map_t s_map_shared[] = {
    {"http://s/chapter-1",
     "<img data-src=\"http://cdn/a.jpg\"><img data-src=\"http://cdn/b.jpg\">"},
    {"http://s/chapter-2",
     "<img data-src=\"http://cdn/a.jpg\"><img data-src=\"http://cdn/e.jpg\">"},
};

/**
 * @test test_content_dedup_across_chapters
 *
 * @par MC/DC:
 * (No compound decision under test; it proves a byte-identical image shared by
 * two chapters is fetched once and reused for the second chapter -- the shared
 * page is served from the already-held file, and the reused copy is identical.)
 */
static void test_content_dedup_across_chapters(void) {
  TEST_BEGIN("content dedup across chapters");
  setup_site();
  g_mock.map = s_map_shared;
  g_mock.map_n = sizeof(s_map_shared) / sizeof(s_map_shared[0]);
  char abs_dir[PATH_MAX];
  char state_path[PATH_MAX];
  make_series_dir(abs_dir, sizeof(abs_dir), state_path, sizeof(state_path));
  mdl_state_init(&g_state);
  const char *c12[] = {"http://s/chapter-1", "http://s/chapter-2"};
  set_chapters(c12, 2U);

  mdl_fetch_stats_t s;
  TEST_ASSERT_EQ((int64_t)k_ra8_ok,
                 run_fetch(abs_dir, state_path, k_mdl_layout_separate, nullptr,
                           false, 0U, &s));
  /* Four page slots (a,b,a,e) but only three unique fetches: A is reused. */
  TEST_ASSERT_EQ((uint16_t)3, (uint16_t)s.pages_fetched);
  TEST_ASSERT_EQ((uint16_t)1, (uint16_t)s.pages_reused);
  TEST_ASSERT_EQ((uint16_t)3, (uint16_t)g_mock.get_file_calls);
  /* The reused copy in chapter 2 is byte-identical to chapter 1's page 1. */
  char p1[k_join_bytes];
  char p2[k_join_bytes];
  (void)snprintf(p1, sizeof(p1), "%s/chapter-1/page_0001.jpg", abs_dir);
  (void)snprintf(p2, sizeof(p2), "%s/chapter-2/page_0001.jpg", abs_dir);
  TEST_ASSERT(files_equal(p1, p2));
  TEST_END("content dedup across chapters");
}

/**
 * @test test_corrupt_state_rebuilds
 *
 * @par MC/DC:
 * (No compound decision under test; it proves a corrupt state file is reported
 * as invalid_state and, after the caller reinitialises, the fetch loop rebuilds
 * from scratch and downloads normally rather than crashing or silently doing
 * nothing.)
 */
static void test_corrupt_state_rebuilds(void) {
  TEST_BEGIN("corrupt state rebuilds");
  setup_site();
  g_mock.map = s_map3;
  g_mock.map_n = sizeof(s_map3) / sizeof(s_map3[0]);
  char abs_dir[PATH_MAX];
  char state_path[PATH_MAX];
  make_series_dir(abs_dir, sizeof(abs_dir), state_path, sizeof(state_path));

  /* Plant a corrupt state file, as a kill mid-write (pre-atomic) might have. */
  FILE *fp = fopen(state_path, "wb");
  TEST_ASSERT_NOT_NULL(fp);
  (void)fprintf(fp, "corrupt garbage not a state file\n");
  (void)fclose(fp);

  /* The load reports the corruption and leaves an empty, valid state. */
  TEST_ASSERT_EQ((int64_t)k_ra8_err_invalid_state,
                 mdl_state_load(state_path, &g_state));
  TEST_ASSERT_EQ((uint16_t)0, g_state.chapter_count);

  /* The fetch then rebuilds and downloads normally. */
  const char *c1[] = {"http://s/chapter-1"};
  set_chapters(c1, 1U);
  mdl_fetch_stats_t s;
  TEST_ASSERT_EQ((int64_t)k_ra8_ok,
                 run_fetch(abs_dir, state_path, k_mdl_layout_separate, nullptr,
                           false, 0U, &s));
  TEST_ASSERT_EQ((uint16_t)2, (uint16_t)s.pages_fetched);
  TEST_ASSERT_EQ((uint16_t)1, (uint16_t)s.chapters_completed);
  TEST_END("corrupt state rebuilds");
}

/** @brief One chapter, one image, for the governed-retry integration test. */
static const page_map_t s_map1[] = {
    {"http://s/chapter-1", "<img data-src=\"http://cdn/a.jpg\">"},
};

/**
 * @test test_governor_retries_throttle
 *
 * @par MC/DC:
 * (No compound decision in this test; it proves the #301 integration: when the
 * mdl_net mock answers the first image transfer with 503 + `Retry-After: 1`,
 * the fetch loop feeds it to the governor, waits the honoured 1 s through the
 * injected clock, retries, and completes -- rather than hammering or abandoning
 * the page. Backoff observation is confirmed via ::mdl_governor_peek.)
 */
static void test_governor_retries_throttle(void) {
  TEST_BEGIN("fetch loop honours governor backoff");
  setup_site();
  g_mock.map = s_map1;
  g_mock.map_n = sizeof(s_map1) / sizeof(s_map1[0]);
  g_mock.busy_on_file_call = 1U; /* first image transfer is throttled */
  g_mock.busy_retry_after = "1"; /* Retry-After: 1 second             */
  char abs_dir[PATH_MAX];
  char state_path[PATH_MAX];
  make_series_dir(abs_dir, sizeof(abs_dir), state_path, sizeof(state_path));
  mdl_state_init(&g_state);
  const char *c1[] = {"http://s/chapter-1"};
  set_chapters(c1, 1U);

  fetch_clock_t clk = {};
  mdl_gov_cfg_t cfg = mdl_gov_cfg_default();
  cfg.rate_per_min = 0U; /* isolate: only the backoff/Retry-After gate sleeps */
  mdl_governor_t gov;
  mdl_governor_init_clock(&gov, &cfg, 1U, fetch_now, &clk, fetch_sleep, &clk);
  g_fetch_gov = &gov;

  mdl_fetch_stats_t st;
  const ra8_err_t rc = run_fetch(abs_dir, state_path, k_mdl_layout_separate,
                                 nullptr, false, 0U, &st);
  g_fetch_gov = nullptr; /* restore the default (governor-less) path */
  g_mock.busy_on_file_call = 0U;

  TEST_ASSERT_EQ((int64_t)k_ra8_ok, rc);
  TEST_ASSERT_EQ((uint16_t)1,
                 (uint16_t)st.pages_fetched); /* succeeded after the retry */
  TEST_ASSERT_EQ((uint16_t)1, (uint16_t)st.chapters_completed);
  TEST_ASSERT_EQ((uint16_t)2,
                 (uint16_t)g_mock.get_file_calls); /* initial 503 + one retry */
  TEST_ASSERT(clk.total_slept >= 1000); /* honoured the 1 s Retry-After */
  TEST_ASSERT(page_exists(abs_dir, "chapter-1/page_0001.jpg"));
  uint16_t level = 0U;
  TEST_ASSERT(
      mdl_governor_peek(&gov, "cdn", &level, nullptr)); /* host of a.jpg */
  TEST_ASSERT_EQ((int64_t)1, (int64_t)level); /* one throttle recorded */
  TEST_END("fetch loop honours governor backoff");
}

/**
 * @test test_transient_page_retry_succeeds
 *
 * @par MC/DC:
 * (No compound decision under test; it proves a single transient transport
 * failure on a page is recovered by the bounded retry -- the page's second
 * attempt succeeds, the chapter completes, and nothing is recorded as failed.)
 */
static void test_transient_page_retry_succeeds(void) {
  TEST_BEGIN("transient page retry succeeds");
  setup_site();
  g_mock.map = s_map3; /* chapter-1 has two pages (a,b) */
  g_mock.map_n = sizeof(s_map3) / sizeof(s_map3[0]);
  char abs_dir[PATH_MAX];
  char state_path[PATH_MAX];
  make_series_dir(abs_dir, sizeof(abs_dir), state_path, sizeof(state_path));
  mdl_state_init(&g_state);
  const char *c1[] = {"http://s/chapter-1"};
  set_chapters(c1, 1U);

  mdl_fetch_stats_t s;
  /* Fail only the very first page transfer; the bounded retry must recover it.
   */
  TEST_ASSERT_EQ((int64_t)k_ra8_ok,
                 run_fetch(abs_dir, state_path, k_mdl_layout_separate, nullptr,
                           false, 1U, &s));
  TEST_ASSERT_EQ((uint16_t)2, (uint16_t)s.pages_fetched);
  TEST_ASSERT_EQ((uint16_t)0, (uint16_t)s.pages_failed);
  TEST_ASSERT_EQ((uint16_t)1, (uint16_t)s.chapters_completed);
  TEST_ASSERT_EQ((uint16_t)3,
                 (uint16_t)g_mock.get_file_calls); /* a(fail)+a(ok)+b(ok) */
  TEST_ASSERT_EQ((uint16_t)0, (uint16_t)g_faillog.total);
  TEST_END("transient page retry succeeds");
}

/**
 * @test test_incomplete_chapter_not_completed_and_logged
 *
 * @par MC/DC:
 * (No compound decision under test; it proves a page that fails every retry
 * leaves its chapter recorded incomplete -- never marked complete -- and is
 * captured in the run's failure log with its URL, so a resume re-fetches it and
 * the run is reported honestly rather than packaged as if whole.)
 */
static void test_incomplete_chapter_not_completed_and_logged(void) {
  TEST_BEGIN("incomplete chapter is not completed and is logged");
  setup_site();
  g_mock.map = s_map3; /* chapter-1 has pages a,b */
  g_mock.map_n = sizeof(s_map3) / sizeof(s_map3[0]);
  g_mock.fail_url = "http://cdn/b.jpg"; /* page b never succeeds */
  char abs_dir[PATH_MAX];
  char state_path[PATH_MAX];
  make_series_dir(abs_dir, sizeof(abs_dir), state_path, sizeof(state_path));
  mdl_state_init(&g_state);
  const char *c1[] = {"http://s/chapter-1"};
  set_chapters(c1, 1U);

  mdl_fetch_stats_t s;
  TEST_ASSERT_EQ((int64_t)k_ra8_fail,
                 run_fetch(abs_dir, state_path, k_mdl_layout_separate, nullptr,
                           false, 0U, &s));
  g_mock.fail_url = nullptr; /* restore for later tests */

  TEST_ASSERT_EQ((uint16_t)1, (uint16_t)s.pages_fetched); /* page a */
  TEST_ASSERT_EQ((uint16_t)1, (uint16_t)s.pages_failed);  /* page b */
  TEST_ASSERT_EQ((uint16_t)1, (uint16_t)s.chapters_failed);
  TEST_ASSERT_EQ((uint16_t)0, (uint16_t)s.chapters_completed);
  /* The chapter is recorded incomplete, so a later run resumes it. */
  TEST_ASSERT(!mdl_state_chapter_complete(&g_state, "chapter-1"));
  /* The failure is logged with the offending URL for the end-of-run summary. */
  TEST_ASSERT_EQ((uint16_t)1, (uint16_t)g_faillog.total);
  TEST_ASSERT_EQ((uint16_t)1, (uint16_t)g_faillog.count);
  TEST_ASSERT_EQ((int64_t)k_ra8_fail, (int64_t)g_faillog.items[0].err);
  TEST_ASSERT(strcmp(g_faillog.items[0].url, "http://cdn/b.jpg") == 0);
  TEST_ASSERT(g_mock.get_file_calls >
              (size_t)2); /* page b was retried, not abandoned */
  TEST_END("incomplete chapter is not completed and is logged");
}

/** @test A state checkpoint failure makes the successful page run fail
 * honestly. */
static void test_checkpoint_failure_fails_run(void) {
  TEST_BEGIN("checkpoint failure fails run");
  setup_site();
  g_mock.map = s_map1;
  g_mock.map_n = sizeof(s_map1) / sizeof(s_map1[0]);
  char abs_dir[PATH_MAX];
  char state_path[PATH_MAX];
  make_series_dir(abs_dir, sizeof(abs_dir), state_path, sizeof(state_path));
  mdl_state_init(&g_state);
  const char *c1[] = {"http://s/chapter-1"};
  set_chapters(c1, 1U);

  mdl_fetch_stats_t stats;
  TEST_ASSERT_EQ(
      (int64_t)k_ra8_fail,
      run_fetch(
          abs_dir,
          abs_dir, /* rename cannot replace the existing series directory */
          k_mdl_layout_separate, nullptr, false, 0U, &stats));
  TEST_ASSERT_EQ((uint16_t)1, (uint16_t)stats.chapters_failed);
  TEST_ASSERT_EQ((uint16_t)0, (uint16_t)stats.chapters_completed);
  TEST_ASSERT(g_faillog.total > 0U);
  TEST_ASSERT(strcmp(g_faillog.items[g_faillog.count - 1U].url, abs_dir) == 0);
  TEST_END("checkpoint failure fails run");
}

/**
 * @test test_mcdc_is_retryable
 *
 * @par MC/DC:
 * Decision: `(rc == k_ra8_err_busy) || (rc == k_ra8_err_timeout) || (rc ==
 * k_ra8_fail)` cites tools/media_dl/src/mdl_fetch.c@mdl_fetch_is_retryable.
 * - Vector 1: rc=k_ra8_err_not_found -> false (all three conditions false)
 * - Vector 2: rc=k_ra8_err_busy      -> true  (varies condition 1)
 * - Vector 3: rc=k_ra8_err_timeout   -> true  (varies condition 2)
 * - Vector 4: rc=k_ra8_fail          -> true  (varies condition 3)
 * Vectors 1+2 isolate condition 1, 1+3 condition 2, 1+4 condition 3.
 * N+1 = 4 vectors for N=3 conditions: minimal MC/DC.
 */
static void test_mcdc_is_retryable(void) {
  TEST_BEGIN("is_retryable MC/DC");
  TEST_ASSERT(!mdl_fetch_is_retryable(k_ra8_err_not_found));
  TEST_ASSERT(mdl_fetch_is_retryable(k_ra8_err_busy));
  TEST_ASSERT(mdl_fetch_is_retryable(k_ra8_err_timeout));
  TEST_ASSERT(mdl_fetch_is_retryable(k_ra8_fail));
  /* A success and other permanent errors are never retried. */
  TEST_ASSERT(!mdl_fetch_is_retryable(k_ra8_ok));
  TEST_ASSERT(!mdl_fetch_is_retryable(k_ra8_err_no_mem));
  TEST_END("is_retryable MC/DC");
}

/**
 * @test test_mcdc_run_incomplete
 *
 * @par MC/DC:
 * Decision: `(stats->chapters_failed > 0) || (stats->pages_failed > 0)` cites
 * tools/media_dl/src/mdl_fetch.c@mdl_fetch_run_incomplete.
 * - Vector 1: chapters_failed=0, pages_failed=0 -> false (both false)
 * - Vector 2: chapters_failed=1, pages_failed=0 -> true  (varies chapters)
 * - Vector 3: chapters_failed=0, pages_failed=1 -> true  (varies pages)
 * Vectors 1+2 isolate the chapter condition, 1+3 the page condition.
 * N+1 = 3 vectors for N=2 conditions: minimal MC/DC.
 */
static void test_mcdc_run_incomplete(void) {
  TEST_BEGIN("run_incomplete MC/DC");
  mdl_fetch_stats_t clean = {};
  TEST_ASSERT(!mdl_fetch_run_incomplete(&clean));
  mdl_fetch_stats_t ch = {.chapters_failed = 1U};
  TEST_ASSERT(mdl_fetch_run_incomplete(&ch));
  mdl_fetch_stats_t pg = {.pages_failed = 1U};
  TEST_ASSERT(mdl_fetch_run_incomplete(&pg));
  TEST_ASSERT(!mdl_fetch_run_incomplete(nullptr));
  TEST_END("run_incomplete MC/DC");
}

/**
 * @brief Run every fetch-orchestration unit test in sequence.
 * @return 0 when all tests passed; a failing assertion aborts via the harness.
 * @since 0.1.0
 */
static void test_conditional_fetch_304_not_modified(void) {
  TEST_BEGIN("conditional fetch 304 not modified");
  setup_site();
  g_mock.map = s_map1;
  g_mock.map_n = sizeof(s_map1) / sizeof(s_map1[0]);
  g_mock.resp_etag = "\"v100\"";
  g_mock.resp_last_modified = "Wed, 21 Oct 2015 07:28:00 GMT";
  char abs_dir[PATH_MAX];
  char state_path[PATH_MAX];
  make_series_dir(abs_dir, sizeof(abs_dir), state_path, sizeof(state_path));
  mdl_state_init(&g_state);
  const char *c1[] = {"http://s/chapter-1"};
  set_chapters(c1, 1U);

  /* Run 1: 200 OK, returns ETag "v100" and Last-Modified */
  mdl_fetch_stats_t s1;
  TEST_ASSERT_EQ((int64_t)k_ra8_ok,
                 run_fetch(abs_dir, state_path, k_mdl_layout_separate, nullptr,
                           false, 0U, &s1));
  TEST_ASSERT_EQ((uint16_t)1, (uint16_t)s1.pages_fetched);
  TEST_ASSERT(page_exists(abs_dir, "chapter-1/page_0001.jpg"));

  /* Verify state cached etag and last_modified */
  const mdl_page_rec_t *p =
      mdl_state_find_page(&g_state, mdl_hash_str("http://cdn/a.jpg"));
  TEST_ASSERT_NOT_NULL(p);
  TEST_ASSERT(strcmp(p->etag, "\"v100\"") == 0);
  TEST_ASSERT(strcmp(p->last_modified, "Wed, 21 Oct 2015 07:28:00 GMT") == 0);

  /* Reload state from disk to test persistence */
  TEST_ASSERT_EQ((int64_t)k_ra8_ok, mdl_state_load(state_path, &g_state));

  /* Mark the chapter incomplete. A valid cached hash with validators must still
   * take the conditional network path rather than short-circuiting to reuse. */
  mdl_chapter_rec_t *ch = mdl_state_find_chapter(&g_state, "chapter-1");
  TEST_ASSERT_NOT_NULL(ch);
  ch->complete = false;

  g_mock.not_mod_on_file_call = 1U; /* Script call 1 of run 2 to 304 */
  mdl_fetch_stats_t s2;
  TEST_ASSERT_EQ((int64_t)k_ra8_ok,
                 run_fetch(abs_dir, state_path, k_mdl_layout_separate, nullptr,
                           false, 0U, &s2));
  g_mock.not_mod_on_file_call = 0U;
  g_mock.resp_etag = nullptr;
  g_mock.resp_last_modified = nullptr;

  /* 304 is treated as success, counts as reused, retains existing file */
  TEST_ASSERT_EQ((uint16_t)1, (uint16_t)s2.pages_reused);
  TEST_ASSERT_EQ((uint16_t)0, (uint16_t)s2.pages_fetched);
  TEST_ASSERT(page_exists(abs_dir, "chapter-1/page_0001.jpg"));

  /* Verify conditional headers were sent */
  TEST_ASSERT(strcmp(g_mock.last_if_none_match, "\"v100\"") == 0);
  TEST_ASSERT(
      strcmp(g_mock.last_if_mod_since, "Wed, 21 Oct 2015 07:28:00 GMT") == 0);
  TEST_END("conditional fetch 304 not modified");
}

/** @test A 200 response at the same URL replaces valid cached bytes atomically.
 */
static void test_conditional_fetch_200_replaces_changed_content(void) {
  TEST_BEGIN("conditional fetch 200 replaces changed content");
  setup_site();
  g_mock.map = s_map1;
  g_mock.map_n = sizeof(s_map1) / sizeof(s_map1[0]);
  g_mock.resp_etag = "\"v1\"";
  char abs_dir[PATH_MAX];
  char state_path[PATH_MAX];
  make_series_dir(abs_dir, sizeof(abs_dir), state_path, sizeof(state_path));
  mdl_state_init(&g_state);
  const char *c1[] = {"http://s/chapter-1"};
  set_chapters(c1, 1U);

  mdl_fetch_stats_t first = {};
  TEST_ASSERT_EQ((int64_t)k_ra8_ok,
                 run_fetch(abs_dir, state_path, k_mdl_layout_separate, nullptr,
                           false, 0U, &first));
  const mdl_page_rec_t *old =
      mdl_state_find_page(&g_state, mdl_hash_str("http://cdn/a.jpg"));
  TEST_ASSERT_NOT_NULL(old);
  const uint64_t old_hash = old->content_hash;

  mdl_chapter_rec_t *chapter = mdl_state_find_chapter(&g_state, "chapter-1");
  TEST_ASSERT_NOT_NULL(chapter);
  chapter->complete = false;
  g_mock.response_body = "replacement image bytes";
  g_mock.resp_etag = "\"v2\"";
  mdl_fetch_stats_t second = {};
  TEST_ASSERT_EQ((int64_t)k_ra8_ok,
                 run_fetch(abs_dir, state_path, k_mdl_layout_separate, nullptr,
                           false, 0U, &second));

  const mdl_page_rec_t *changed =
      mdl_state_find_page(&g_state, mdl_hash_str("http://cdn/a.jpg"));
  TEST_ASSERT_NOT_NULL(changed);
  TEST_ASSERT(changed->content_hash != old_hash);
  TEST_ASSERT(strcmp(changed->etag, "\"v2\"") == 0);
  TEST_ASSERT_EQ((uint16_t)1, (uint16_t)second.pages_fetched);
  TEST_ASSERT_EQ((uint16_t)0, (uint16_t)second.pages_reused);
  TEST_ASSERT(strcmp(g_mock.last_if_none_match, "\"v1\"") == 0);
  TEST_END("conditional fetch 200 replaces changed content");
}

/**
 * @test test_page_magic_required_and_old_destination_preserved
 * @brief Refuse a mislabeled non-image without replacing verified cached bytes.
 * @details Fetches one valid JPEG, then forces a refetch whose HTTP metadata
 *          claims JPEG while its body contains no supported image signature.
 * @pre The test process may create files under `/tmp`.
 * @pre Scripted fetch globals are exclusively owned by this test.
 * @post The second run fails validation and the first file/hash remain intact.
 * @post The URL-keyed state record continues to describe the retained file.
 * @note This is the semantic-validation boundary above the transport's atomic
 * write.
 * @since 0.1.0
 */
static void test_page_magic_required_and_old_destination_preserved(void) {
  TEST_BEGIN("page magic required + preserve old destination");
  setup_site();
  g_mock.map = s_map1;
  g_mock.map_n = sizeof(s_map1) / sizeof(s_map1[0]);
  char abs_dir[PATH_MAX];
  char state_path[PATH_MAX];
  make_series_dir(abs_dir, sizeof(abs_dir), state_path, sizeof(state_path));
  mdl_state_init(&g_state);
  const char *chapters[] = {"http://s/chapter-1"};
  set_chapters(chapters, 1U);

  mdl_fetch_stats_t first = {};
  TEST_ASSERT_EQ((int64_t)k_ra8_ok,
                 run_fetch(abs_dir, state_path, k_mdl_layout_separate, nullptr,
                           false, 0U, &first));
  const uint64_t old_hash = page_hash(abs_dir, "chapter-1/page_0001.jpg");
  const mdl_page_rec_t *old_record =
      mdl_state_find_page(&g_state, mdl_hash_str("http://cdn/a.jpg"));
  TEST_ASSERT_NOT_NULL(old_record);
  TEST_ASSERT_EQ(old_hash, old_record->content_hash);

  mdl_chapter_rec_t *chapter = mdl_state_find_chapter(&g_state, "chapter-1");
  TEST_ASSERT_NOT_NULL(chapter);
  chapter->complete = false;
  g_refetch = true;
  g_mock.response_prefix = nullptr;
  g_mock.response_prefix_len = 0U;
  g_mock.response_body = "this is HTML, not an image";
  g_mock.resp_content_type = "image/jpeg";
  mdl_fetch_stats_t rejected = {};
  TEST_ASSERT_EQ((int64_t)k_ra8_fail,
                 run_fetch(abs_dir, state_path, k_mdl_layout_separate, nullptr,
                           false, 0U, &rejected));
  TEST_ASSERT_EQ((int64_t)1, (int64_t)rejected.pages_failed);
  TEST_ASSERT_EQ(old_hash, page_hash(abs_dir, "chapter-1/page_0001.jpg"));
  const mdl_page_rec_t *retained =
      mdl_state_find_page(&g_state, mdl_hash_str("http://cdn/a.jpg"));
  TEST_ASSERT_NOT_NULL(retained);
  TEST_ASSERT_EQ(old_hash, retained->content_hash);
  TEST_ASSERT(strcmp(retained->rel_path, "chapter-1/page_0001.jpg") == 0);
  TEST_END("page magic required + preserve old destination");
}

/**
 * @test test_page_magic_changes_extension_and_rekeys_state
 * @brief Publish a page under its byte-derived extension and remove the stale
 * name.
 * @details Refetches a `.jpg` URL as PNG bytes while the server still claims
 *          JPEG, proving magic wins, publication changes to `.png`, and the
 *          URL-keyed state record is replaced instead of shadowed by an older
 * path.
 * @pre The test process may create files under `/tmp`.
 * @pre Scripted fetch globals are exclusively owned by this test.
 * @post Exactly the PNG-named page remains and the cache count stays constant.
 * @post Lookup by URL hash returns the PNG record and its new content hash.
 * @note Host-only; no real network or image decoder is involved.
 * @since 0.1.0
 */
static void test_page_magic_changes_extension_and_rekeys_state(void) {
  TEST_BEGIN("page magic extension + URL-keyed state");
  setup_site();
  g_mock.map = s_map1;
  g_mock.map_n = sizeof(s_map1) / sizeof(s_map1[0]);
  char abs_dir[PATH_MAX];
  char state_path[PATH_MAX];
  make_series_dir(abs_dir, sizeof(abs_dir), state_path, sizeof(state_path));
  mdl_state_init(&g_state);
  const char *chapters[] = {"http://s/chapter-1"};
  set_chapters(chapters, 1U);

  mdl_fetch_stats_t first = {};
  TEST_ASSERT_EQ((int64_t)k_ra8_ok,
                 run_fetch(abs_dir, state_path, k_mdl_layout_separate, nullptr,
                           false, 0U, &first));
  const uint32_t old_count = g_state.page_rec_count;
  const uint64_t old_hash = page_hash(abs_dir, "chapter-1/page_0001.jpg");
  mdl_chapter_rec_t *chapter = mdl_state_find_chapter(&g_state, "chapter-1");
  TEST_ASSERT_NOT_NULL(chapter);
  chapter->complete = false;
  g_refetch = true;
  g_mock.response_prefix = s_png_magic;
  g_mock.response_prefix_len = sizeof(s_png_magic);
  g_mock.response_body = "replacement PNG payload";
  g_mock.resp_content_type = "image/jpeg";

  mdl_fetch_stats_t second = {};
  TEST_ASSERT_EQ((int64_t)k_ra8_ok,
                 run_fetch(abs_dir, state_path, k_mdl_layout_separate, nullptr,
                           false, 0U, &second));
  TEST_ASSERT(!page_exists(abs_dir, "chapter-1/page_0001.jpg"));
  TEST_ASSERT(page_exists(abs_dir, "chapter-1/page_0001.png"));
  const uint64_t new_hash = page_hash(abs_dir, "chapter-1/page_0001.png");
  TEST_ASSERT(new_hash != old_hash);
  TEST_ASSERT_EQ(old_count, g_state.page_rec_count);
  const mdl_page_rec_t *record =
      mdl_state_find_page(&g_state, mdl_hash_str("http://cdn/a.jpg"));
  TEST_ASSERT_NOT_NULL(record);
  TEST_ASSERT_EQ(new_hash, record->content_hash);
  TEST_ASSERT(strcmp(record->rel_path, "chapter-1/page_0001.png") == 0);
  TEST_END("page magic extension + URL-keyed state");
}

/**
 * @test test_304_without_cached_entity_refetches_unconditionally
 * @brief Recover from a 304 response when the recorded local entity is missing.
 * @details Deletes a validator-backed page, scripts one HTTP 304, and requires
 *          the fetcher to discard the empty stage and retry without validators.
 * @pre The test process may create and unlink files under `/tmp`.
 * @pre Scripted fetch globals are exclusively owned by this test.
 * @post The run succeeds with a new 200 body and records one fetched page.
 * @post The final retry carries no conditional request headers.
 * @note Prevents a permanent 304 loop when local cache bytes were lost.
 * @since 0.1.0
 */
static void test_304_without_cached_entity_refetches_unconditionally(void) {
  TEST_BEGIN("304 missing entity -> unconditional refetch");
  setup_site();
  g_mock.map = s_map1;
  g_mock.map_n = sizeof(s_map1) / sizeof(s_map1[0]);
  g_mock.resp_etag = "\"v1\"";
  g_mock.resp_last_modified = "Wed, 21 Oct 2015 07:28:00 GMT";
  char abs_dir[PATH_MAX];
  char state_path[PATH_MAX];
  make_series_dir(abs_dir, sizeof(abs_dir), state_path, sizeof(state_path));
  mdl_state_init(&g_state);
  const char *chapters[] = {"http://s/chapter-1"};
  set_chapters(chapters, 1U);
  mdl_fetch_stats_t first = {};
  TEST_ASSERT_EQ((int64_t)k_ra8_ok,
                 run_fetch(abs_dir, state_path, k_mdl_layout_separate, nullptr,
                           false, 0U, &first));

  char page[k_join_bytes];
  (void)snprintf(page, sizeof(page), "%s/chapter-1/page_0001.jpg", abs_dir);
  TEST_ASSERT_EQ((int64_t)0, (int64_t)unlink(page));
  mdl_chapter_rec_t *chapter = mdl_state_find_chapter(&g_state, "chapter-1");
  TEST_ASSERT_NOT_NULL(chapter);
  chapter->complete = false;
  g_mock.not_mod_on_file_call = 1U;
  mdl_fetch_stats_t recovered = {};
  TEST_ASSERT_EQ((int64_t)k_ra8_ok,
                 run_fetch(abs_dir, state_path, k_mdl_layout_separate, nullptr,
                           false, 0U, &recovered));
  TEST_ASSERT_EQ((int64_t)2, (int64_t)g_mock.get_file_calls);
  TEST_ASSERT_EQ((int64_t)1, (int64_t)recovered.pages_fetched);
  TEST_ASSERT_EQ((int64_t)0, (int64_t)recovered.pages_reused);
  TEST_ASSERT(page_exists(abs_dir, "chapter-1/page_0001.jpg"));
  TEST_ASSERT(strcmp(g_mock.last_if_none_match, "") == 0);
  TEST_ASSERT(strcmp(g_mock.last_if_mod_since, "") == 0);
  TEST_END("304 missing entity -> unconditional refetch");
}

/**
 * @test test_fetch_asset_policy_atomic_and_nonempty
 *
 * @brief Verify generic asset transfer policy and publication invariants.
 * @details Proves the generic asset path uses the injected network session,
 *          reports response bytes/status, atomically publishes a non-empty
 *          body, and rejects a successful empty body without replacing an
 *          already-good destination.
 * @pre The test process may create files under `/tmp`.
 * @pre The scripted backend and global fetch fixtures are exclusively owned.
 * @post A non-empty mock response is committed with its byte count.
 * @post An empty mock response leaves the prior destination bytes unchanged.
 * @note Host-only test; the injected backend performs no network access.
 * @since 0.1.0
 */
static void test_fetch_asset_policy_atomic_and_nonempty(void) {
  TEST_BEGIN("fetch asset policy + atomic + nonempty");
  setup_site();
  char abs_dir[PATH_MAX];
  char state_path[PATH_MAX];
  make_series_dir(abs_dir, sizeof(abs_dir), state_path, sizeof(state_path));
  char target[k_join_bytes];
  (void)snprintf(target, sizeof(target), "%s/cover.jpg", abs_dir);

  mdl_net_iface_t iface = {.vtable = &s_mock_vtable, .ctx = &g_mock};
  mdl_session_init(&g_sess, &iface, "media_dl/test", false);
  memset(&g_faillog, 0, sizeof(g_faillog));
  mdl_fetch_ctx_t ctx = {.session = &g_sess,
                         .site = &g_site,
                         .gov = nullptr,
                         .timeout_ms = (uint32_t)k_req_timeout,
                         .faillog = &g_faillog};
  g_mock.response_prefix = nullptr;
  g_mock.response_prefix_len = 0U;
  g_mock.response_body = "cover bytes";
  mdl_net_resp_t resp = {};
  size_t bytes = 0U;
  TEST_ASSERT_EQ((int64_t)k_ra8_ok,
                 mdl_fetch_asset(&ctx, "http://cdn/cover.jpg", target,
                                 "http://s/series", &resp, &bytes));
  TEST_ASSERT_EQ((int64_t)strlen("cover bytes"), (int64_t)bytes);
  TEST_ASSERT_EQ((int64_t)200, (int64_t)resp.status);
  TEST_ASSERT(page_exists(abs_dir, "cover.jpg"));

  FILE *fp = fopen(target, "wb");
  TEST_ASSERT_NOT_NULL(fp);
  TEST_ASSERT_EQ((int64_t)strlen("keep"),
                 (int64_t)fwrite("keep", 1U, strlen("keep"), fp));
  TEST_ASSERT_EQ((int64_t)0, (int64_t)fclose(fp));
  g_mock.response_body = "";
  bytes = 99U;
  TEST_ASSERT_EQ((int64_t)k_ra8_err_invalid_size,
                 mdl_fetch_asset(&ctx, "http://cdn/empty.jpg", target, nullptr,
                                 &resp, &bytes));
  TEST_ASSERT_EQ((int64_t)0, (int64_t)bytes);
  fp = fopen(target, "rb");
  TEST_ASSERT_NOT_NULL(fp);
  char retained[8] = {};
  TEST_ASSERT_EQ((int64_t)strlen("keep"),
                 (int64_t)fread(retained, 1U, sizeof(retained), fp));
  TEST_ASSERT_EQ((int64_t)0, (int64_t)fclose(fp));
  TEST_ASSERT(strcmp(retained, "keep") == 0);
  TEST_ASSERT_EQ((int64_t)k_ra8_err_invalid_arg,
                 mdl_fetch_asset(&ctx, "http://cdn/cover.jpg", "relative.jpg",
                                 nullptr, nullptr, nullptr));
  TEST_END("fetch asset policy + atomic + nonempty");
}

int32_t main(void) {
  test_fetch_asset_policy_atomic_and_nonempty();
  test_chapter_number_text_parser();
  test_chapter_number_selector_strict_and_persisted();
  test_chapter_title_overflow_fails_without_page_fetch();
  test_conditional_fetch_304_not_modified();
  test_304_without_cached_entity_refetches_unconditionally();
  test_conditional_fetch_200_replaces_changed_content();
  test_page_magic_required_and_old_destination_preserved();
  test_page_magic_changes_extension_and_rekeys_state();
  test_first_run_then_update_only_new();
  test_refetch_bypasses_valid_cache();
  test_resume_equals_uninterrupted();
  test_content_dedup_across_chapters();
  test_corrupt_state_rebuilds();
  test_governor_retries_throttle();
  test_transient_page_retry_succeeds();
  test_incomplete_chapter_not_completed_and_logged();
  test_checkpoint_failure_fails_run();
  test_mcdc_is_retryable();
  test_mcdc_run_incomplete();
  (void)fprintf(stderr, "[OK  ] test_media_dl_fetch.c\n");
  return 0;
}
