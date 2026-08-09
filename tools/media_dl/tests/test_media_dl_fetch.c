/**
 * @file test_media_dl_fetch.c
 * @brief Host unit tests for the #305 resumable/incremental/deduping download
 *        loop, driven end-to-end through the #310 mdl_net vtable mock.
 *
 * @details
 * A scripted fake ::mdl_net_iface_t backend serves chapter HTML for `get_buf`
 * and writes deterministic, URL-derived page bytes for `get_file` (so the same
 * image URL always yields the same content, and a torn/absent page differs),
 * with no network. A temp directory holds the output. The suite asserts the four
 * behaviours the issue's acceptance criteria call out:
 *   - a first run fetches every page; a second run over the same series with a
 *     newly-added chapter (`--update`) fetches ONLY the new chapter, without even
 *     requesting the HTML of the chapters already complete;
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
#include "ra8_err.h"
#include "unity_minimal.h"

/** @brief Named constants for the fetch tests (no bare literals). */
typedef enum : uint16_t {
  k_map_max          = 8,    /**< Chapter->HTML map slots.                      */
  k_html_bytes       = 512,  /**< Per-chapter HTML buffer bytes.                */
  k_page_bytes       = 8192, /**< Chapter-HTML scratch bytes.                   */
  k_tmpl_bytes       = 64,   /**< mkdtemp template bytes.                       */
  k_req_timeout      = 1000, /**< Per-request budget, ms.                       */
  k_list_max         = 8,    /**< Chapter URLs in a scenario.                   */
  k_http_unavailable = 503,  /**< HTTP 503 the throttle-injection mock returns. */
} mdl_fetch_test_const_t;

/**
 * @brief Composed-path buffer size: sized past PATH_MAX plus a leaf so the
 *        compiler can prove a `<dir>/<page>` join never truncates (silences
 *        gcc -Wformat-truncation, since the dir source is a PATH_MAX array).
 */
typedef enum : uint32_t {
  k_join_bytes = (uint32_t)PATH_MAX + 128U, /**< PATH_MAX + a page-leaf margin. */
} mdl_fetch_join_t;

/**
 * @struct page_map_t
 * @brief One chapter URL mapped to the HTML the fake serves for it.
 * @since 0.1.0
 */
typedef struct {
  const char* url;  /**< Chapter page URL the fake recognises. */
  const char* html; /**< HTML body returned for that URL.      */
} page_map_t;

/**
 * @struct mock_net_t
 * @brief Fake ::mdl_net_iface_t backend: serves chapter HTML, writes page bytes.
 * @details `get_buf` returns the mapped HTML for a chapter URL; `get_file`
 *          writes the URL's own bytes to the target (deterministic content) and
 *          can be scripted to fail on one call to simulate an interruption.
 * @invariant `get_file_calls` counts every page transfer attempt.
 * @since 0.1.0
 */
typedef struct {
  const page_map_t* map;               /**< Chapter URL -> HTML.                 */
  size_t            map_n;             /**< Entries in @ref map.                 */
  size_t            get_buf_calls;     /**< Chapter-HTML fetches dispatched.     */
  size_t            get_file_calls;    /**< Page transfers attempted.            */
  size_t            fail_on_file_call; /**< 1-based call to fail once (0=never). */
  size_t            busy_on_file_call; /**< 1-based call to 503+Retry (0=off).   */
  const char*       busy_retry_after;  /**< Retry-After for the busy reply.      */
  const char*       fail_url;          /**< URL that fails on EVERY attempt.     */
} mock_net_t;

/** @brief Fake get_buf: serve the mapped HTML for a chapter URL. */
static ra8_err_t mock_get_buf(void*                ctx,
                              const char*          url,
                              const mdl_net_req_t* req,
                              char*                buf,
                              size_t               cap,
                              size_t*              out_len,
                              mdl_net_resp_t*      resp)
{
  (void)req;
  (void)resp;
  mock_net_t* f = (mock_net_t*)ctx;
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

/** @brief Fake get_file: write the URL's own bytes, unless this call is scripted to fail. */
static ra8_err_t mock_get_file(void*                ctx,
                               const char*          url,
                               const mdl_net_req_t* req,
                               const char*          out_path,
                               size_t*              out_len,
                               mdl_net_resp_t*      resp)
{
  (void)req;
  mock_net_t* f = (mock_net_t*)ctx;
  f->get_file_calls += 1U;
  if ((f->busy_on_file_call != 0U) && (f->get_file_calls == f->busy_on_file_call)) {
    if (resp != nullptr) { /* scripted throttle: 503 + Retry-After, nothing written */
      resp->status = (long)k_http_unavailable;
      (void)snprintf(resp->retry_after,
                     sizeof(resp->retry_after),
                     "%s",
                     (f->busy_retry_after != nullptr) ? f->busy_retry_after : "");
    }
    return k_ra8_err_busy;
  }
  if ((f->fail_on_file_call != 0U) && (f->get_file_calls == f->fail_on_file_call)) {
    return k_ra8_fail; /* a single transient failure: the retry recovers it */
  }
  if ((f->fail_url != nullptr) && (strcmp(f->fail_url, url) == 0)) {
    return k_ra8_fail; /* a permanently-unreachable page: every attempt fails */
  }
  FILE* fp = fopen(out_path, "wb");
  if (fp == nullptr) {
    return k_ra8_fail;
  }
  (void)fwrite(url, 1U, strlen(url), fp);
  (void)fclose(fp);
  if (out_len != nullptr) {
    *out_len = strlen(url);
  }
  return k_ra8_ok;
}

/** @brief Fake destroy: the handle is stack-owned, nothing to release. */
static void mock_destroy(void* ctx)
{
  (void)ctx;
}

/** @brief The fake backend's method table. */
static const mdl_net_vtable_t s_mock_vtable = {
  .get_buf  = mock_get_buf,
  .get_file = mock_get_file,
  .destroy  = mock_destroy,
};

/* ---- shared fixtures (large objects live off the stack) ------------------ */

static mock_net_t          g_mock;               /**< The scripted backend context.           */
static mdl_session_t       g_sess;               /**< Session over the fake (64 KiB embed).   */
static mdl_site_t          g_site;               /**< Selectors + (zero) politeness bounds.   */
static mdl_state_t         g_state;              /**< State under test (~2 MiB).              */
static mdl_url_list_t      g_chapters;           /**< Live chapter list for a scenario.       */
static mdl_url_list_t      g_images;             /**< Extracted-image scratch.                */
static char                g_page[k_page_bytes]; /**< Chapter-HTML scratch.                   */
static mdl_governor_t*     g_fetch_gov;          /**< Governor wired into run_fetch, or NULL. */
static mdl_fetch_faillog_t g_faillog;            /**< Failure log run_fetch fills each run.   */

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
/* cppcheck-suppress constParameterCallback ; ra8_governor clock-fn ABI is void* */
static int64_t fetch_now(void* c)
{
  return ((const fetch_clock_t*)c)->now_ms;
}

/** @brief Injected sleeper: advance the virtual clock and tally the wait. */
static void fetch_sleep(void* c, uint32_t ms)
{
  fetch_clock_t* k = (fetch_clock_t*)c;
  k->now_ms += (int64_t)ms;
  k->total_slept += (int64_t)ms;
}

/** @brief Configure the site descriptor the fake HTML is scraped against. */
static void setup_site(void)
{
  memset(&g_site, 0, sizeof(g_site));
  (void)snprintf(g_site.page_img_attr, sizeof(g_site.page_img_attr), "%s", "data-src");
  g_site.page_img_url_contains[0] = '\0';
}

/** @brief Copy `n` chapter URLs into `g_chapters`. */
static void set_chapters(const char* const* urls, size_t n)
{
  g_chapters.count = 0U;
  for (size_t i = 0U; (i < n) && (i < (size_t)k_list_max); ++i) {
    (void)snprintf(g_chapters.urls[g_chapters.count], k_mdl_url_max, "%s", urls[i]);
    g_chapters.count += 1U;
  }
}

/** @brief Make a fresh temp directory (resolved) and its `.mdl_state` path. */
static void make_series_dir(char* abs_dir, size_t abs_cap, char* state_path, size_t state_cap)
{
  char tmpl[k_tmpl_bytes];
  (void)snprintf(tmpl, sizeof(tmpl), "%s", "/tmp/mdl_fetch_XXXXXX");
  const char* made = mkdtemp(tmpl);
  TEST_ASSERT_NOT_NULL(made);
  if (realpath(made, abs_dir) == nullptr) {
    (void)snprintf(abs_dir, abs_cap, "%s", made);
  }
  (void)snprintf(state_path, state_cap, "%s/.mdl_state", abs_dir);
}

/** @brief Run the fetch loop for one scenario; returns its result code. */
static ra8_err_t run_fetch(const char*        abs_dir,
                           const char*        state_path,
                           mdl_fetch_layout_t layout,
                           const char*        combined_rel,
                           bool               update_only,
                           size_t             fail_on_file_call,
                           mdl_fetch_stats_t* out)
{
  g_mock.get_buf_calls     = 0U;
  g_mock.get_file_calls    = 0U;
  g_mock.fail_on_file_call = fail_on_file_call;
  memset(&g_faillog, 0, sizeof(g_faillog));
  mdl_net_iface_t iface = {.vtable = &s_mock_vtable, .ctx = &g_mock};
  mdl_session_init(&g_sess, &iface, "media_dl/test", false);
  mdl_fetch_ctx_t ctx = {.session        = &g_sess,
                         .state          = &g_state,
                         .state_path     = state_path,
                         .series_abs_dir = abs_dir,
                         .series_url     = "http://s/series",
                         .site           = &g_site,
                         .gov            = g_fetch_gov,
                         .timeout_ms     = (uint32_t)k_req_timeout,
                         .page_buf       = g_page,
                         .page_cap       = sizeof(g_page),
                         .images         = &g_images,
                         .update_only    = update_only,
                         .faillog        = &g_faillog,
                         .progress_fn    = nullptr,
                         .progress_ctx   = nullptr};
  return mdl_fetch_run(&ctx, &g_chapters, layout, combined_rel, out);
}

/** @brief True when two files hold byte-identical content. */
static bool files_equal(const char* a, const char* b)
{
  uint64_t ha = 0U;
  uint64_t hb = 0U;
  return (mdl_hash_file(a, &ha) == k_ra8_ok) && (mdl_hash_file(b, &hb) == k_ra8_ok) && (ha == hb);
}

/** @brief True when a file exists at the composed `<abs_dir>/<rel>` path. */
static bool page_exists(const char* abs_dir, const char* rel)
{
  char        path[k_join_bytes];
  struct stat st;
  (void)snprintf(path, sizeof(path), "%s/%s", abs_dir, rel);
  return stat(path, &st) == 0;
}

/** @brief The chapter->HTML map used by the incremental test (three chapters). */
static const page_map_t s_map3[] = {
  {"http://s/chapter-1", "<img data-src=\"http://cdn/a.jpg\"><img data-src=\"http://cdn/b.jpg\">"},
  {"http://s/chapter-2", "<img data-src=\"http://cdn/c.jpg\">"},
  {"http://s/chapter-3", "<img data-src=\"http://cdn/d.jpg\">"},
};

/**
 * @test test_first_run_then_update_only_new
 *
 * @par MC/DC:
 * (No compound decision under test; it proves the incremental contract: a fresh
 * run fetches every page, and a later `--update` over a reloaded state with one
 * new chapter fetches ONLY that chapter -- issuing no HTML request for the
 * chapters already recorded complete.)
 */
static void test_first_run_then_update_only_new(void)
{
  TEST_BEGIN("first run + update only new");
  setup_site();
  g_mock.map   = s_map3;
  g_mock.map_n = sizeof(s_map3) / sizeof(s_map3[0]);
  char abs_dir[PATH_MAX];
  char state_path[PATH_MAX];
  make_series_dir(abs_dir, sizeof(abs_dir), state_path, sizeof(state_path));
  mdl_state_init(&g_state);

  /* Fresh run of chapters 1 and 2: every page is fetched. */
  const char* c12[] = {"http://s/chapter-1", "http://s/chapter-2"};
  set_chapters(c12, 2U);
  mdl_fetch_stats_t s1;
  TEST_ASSERT_EQ((int64_t)k_ra8_ok,
                 run_fetch(abs_dir, state_path, k_mdl_layout_separate, nullptr, false, 0U, &s1));
  TEST_ASSERT_EQ((uint16_t)3, (uint16_t)s1.pages_fetched);
  TEST_ASSERT_EQ((uint16_t)0, (uint16_t)s1.pages_reused);
  TEST_ASSERT_EQ((uint16_t)2, (uint16_t)s1.chapters_completed);
  TEST_ASSERT(page_exists(abs_dir, "chapter-1/page_0001.jpg"));
  TEST_ASSERT(page_exists(abs_dir, "chapter-2/page_0001.jpg"));

  /* Reload the state from disk (simulating a fresh process). */
  TEST_ASSERT_EQ((int64_t)k_ra8_ok, mdl_state_load(state_path, &g_state));

  /* --update with chapter 3 newly present: only chapter 3 is fetched. */
  const char* c123[] = {"http://s/chapter-1", "http://s/chapter-2", "http://s/chapter-3"};
  set_chapters(c123, 3U);
  mdl_fetch_stats_t s2;
  TEST_ASSERT_EQ((int64_t)k_ra8_ok,
                 run_fetch(abs_dir, state_path, k_mdl_layout_separate, nullptr, true, 0U, &s2));
  TEST_ASSERT_EQ((uint16_t)2, (uint16_t)s2.chapters_skipped);
  TEST_ASSERT_EQ((uint16_t)1, (uint16_t)s2.chapters_completed);
  TEST_ASSERT_EQ((uint16_t)1, (uint16_t)s2.pages_fetched);
  /* Only chapter 3's HTML was requested; the complete chapters were untouched. */
  TEST_ASSERT_EQ((uint16_t)1, (uint16_t)g_mock.get_buf_calls);
  TEST_ASSERT_EQ((uint16_t)1, (uint16_t)g_mock.get_file_calls);
  TEST_ASSERT(page_exists(abs_dir, "chapter-3/page_0001.jpg"));
  TEST_END("first run + update only new");
}

/** @brief The two-chapter map used by the combined resume test. */
static const page_map_t s_map_combined[] = {
  {"http://s/chapter-1", "<img data-src=\"http://cdn/a.jpg\"><img data-src=\"http://cdn/b.jpg\">"},
  {"http://s/chapter-2", "<img data-src=\"http://cdn/c.jpg\"><img data-src=\"http://cdn/d.jpg\">"},
};

/** @brief Assert the four combined page files under `abs_dir` are `a`,`b`,`c`,`d`. */
static void assert_combined_pages_equal(const char* dir_a, const char* dir_b)
{
  const char* rel[] = {"foo-1-2/page_0001.jpg",
                       "foo-1-2/page_0002.jpg",
                       "foo-1-2/page_0003.jpg",
                       "foo-1-2/page_0004.jpg"};
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
static void test_resume_equals_uninterrupted(void)
{
  TEST_BEGIN("resume equals uninterrupted");
  setup_site();
  g_mock.map        = s_map_combined;
  g_mock.map_n      = sizeof(s_map_combined) / sizeof(s_map_combined[0]);
  const char* c12[] = {"http://s/chapter-1", "http://s/chapter-2"};

  /* (a) Uninterrupted combined run into dir A: pages 1..4, continuous. */
  char dir_a[PATH_MAX];
  char state_a[PATH_MAX];
  make_series_dir(dir_a, sizeof(dir_a), state_a, sizeof(state_a));
  mdl_state_init(&g_state);
  set_chapters(c12, 2U);
  mdl_fetch_stats_t sa;
  TEST_ASSERT_EQ((int64_t)k_ra8_ok,
                 run_fetch(dir_a, state_a, k_mdl_layout_combined, "foo-1-2", false, 0U, &sa));
  TEST_ASSERT_EQ((uint16_t)4, (uint16_t)sa.pages_fetched);
  TEST_ASSERT(page_exists(dir_a, "foo-1-2/page_0004.jpg"));

  /* (b) Interrupted combined run into dir B: page 4 (d.jpg) is unreachable on
   *     every attempt, so its retries all fail and the chapter is left partial. */
  char dir_b[PATH_MAX];
  char state_b[PATH_MAX];
  make_series_dir(dir_b, sizeof(dir_b), state_b, sizeof(state_b));
  mdl_state_init(&g_state);
  set_chapters(c12, 2U);
  g_mock.fail_url = "http://cdn/d.jpg";
  mdl_fetch_stats_t sb;
  TEST_ASSERT_EQ((int64_t)k_ra8_fail,
                 run_fetch(dir_b, state_b, k_mdl_layout_combined, "foo-1-2", false, 0U, &sb));
  TEST_ASSERT_EQ((uint16_t)1, (uint16_t)sb.chapters_failed);
  TEST_ASSERT(!page_exists(dir_b, "foo-1-2/page_0004.jpg"));

  /* (c) Resume from dir B's on-disk state, page 4 now reachable: only it is
   *     re-fetched. */
  g_mock.fail_url = nullptr;
  TEST_ASSERT_EQ((int64_t)k_ra8_ok, mdl_state_load(state_b, &g_state));
  set_chapters(c12, 2U);
  mdl_fetch_stats_t sc;
  TEST_ASSERT_EQ((int64_t)k_ra8_ok,
                 run_fetch(dir_b, state_b, k_mdl_layout_combined, "foo-1-2", false, 0U, &sc));
  TEST_ASSERT_EQ((uint16_t)1, (uint16_t)sc.pages_fetched); /* only the missing page */
  TEST_ASSERT_EQ((uint16_t)3, (uint16_t)sc.pages_reused);  /* pages 1..3 reused     */
  TEST_ASSERT(page_exists(dir_b, "foo-1-2/page_0004.jpg"));

  /* The resumed output equals the uninterrupted one, page for page. */
  assert_combined_pages_equal(dir_a, dir_b);
  TEST_END("resume equals uninterrupted");
}

/** @brief The map for the dedup test: chapters 1 and 2 share image A. */
static const page_map_t s_map_shared[] = {
  {"http://s/chapter-1", "<img data-src=\"http://cdn/a.jpg\"><img data-src=\"http://cdn/b.jpg\">"},
  {"http://s/chapter-2", "<img data-src=\"http://cdn/a.jpg\"><img data-src=\"http://cdn/e.jpg\">"},
};

/**
 * @test test_content_dedup_across_chapters
 *
 * @par MC/DC:
 * (No compound decision under test; it proves a byte-identical image shared by
 * two chapters is fetched once and reused for the second chapter -- the shared
 * page is served from the already-held file, and the reused copy is identical.)
 */
static void test_content_dedup_across_chapters(void)
{
  TEST_BEGIN("content dedup across chapters");
  setup_site();
  g_mock.map   = s_map_shared;
  g_mock.map_n = sizeof(s_map_shared) / sizeof(s_map_shared[0]);
  char abs_dir[PATH_MAX];
  char state_path[PATH_MAX];
  make_series_dir(abs_dir, sizeof(abs_dir), state_path, sizeof(state_path));
  mdl_state_init(&g_state);
  const char* c12[] = {"http://s/chapter-1", "http://s/chapter-2"};
  set_chapters(c12, 2U);

  mdl_fetch_stats_t s;
  TEST_ASSERT_EQ((int64_t)k_ra8_ok,
                 run_fetch(abs_dir, state_path, k_mdl_layout_separate, nullptr, false, 0U, &s));
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
static void test_corrupt_state_rebuilds(void)
{
  TEST_BEGIN("corrupt state rebuilds");
  setup_site();
  g_mock.map   = s_map3;
  g_mock.map_n = sizeof(s_map3) / sizeof(s_map3[0]);
  char abs_dir[PATH_MAX];
  char state_path[PATH_MAX];
  make_series_dir(abs_dir, sizeof(abs_dir), state_path, sizeof(state_path));

  /* Plant a corrupt state file, as a kill mid-write (pre-atomic) might have. */
  FILE* fp = fopen(state_path, "wb");
  TEST_ASSERT_NOT_NULL(fp);
  (void)fprintf(fp, "corrupt garbage not a state file\n");
  (void)fclose(fp);

  /* The load reports the corruption and leaves an empty, valid state. */
  TEST_ASSERT_EQ((int64_t)k_ra8_err_invalid_state, mdl_state_load(state_path, &g_state));
  TEST_ASSERT_EQ((uint16_t)0, g_state.chapter_count);

  /* The fetch then rebuilds and downloads normally. */
  const char* c1[] = {"http://s/chapter-1"};
  set_chapters(c1, 1U);
  mdl_fetch_stats_t s;
  TEST_ASSERT_EQ((int64_t)k_ra8_ok,
                 run_fetch(abs_dir, state_path, k_mdl_layout_separate, nullptr, false, 0U, &s));
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
 * mdl_net mock answers the first image transfer with 503 + `Retry-After: 1`, the
 * fetch loop feeds it to the governor, waits the honoured 1 s through the
 * injected clock, retries, and completes -- rather than hammering or abandoning
 * the page. Backoff observation is confirmed via ::mdl_governor_peek.)
 */
static void test_governor_retries_throttle(void)
{
  TEST_BEGIN("fetch loop honours governor backoff");
  setup_site();
  g_mock.map               = s_map1;
  g_mock.map_n             = sizeof(s_map1) / sizeof(s_map1[0]);
  g_mock.busy_on_file_call = 1U;  /* first image transfer is throttled */
  g_mock.busy_retry_after  = "1"; /* Retry-After: 1 second             */
  char abs_dir[PATH_MAX];
  char state_path[PATH_MAX];
  make_series_dir(abs_dir, sizeof(abs_dir), state_path, sizeof(state_path));
  mdl_state_init(&g_state);
  const char* c1[] = {"http://s/chapter-1"};
  set_chapters(c1, 1U);

  fetch_clock_t clk = {};
  mdl_gov_cfg_t cfg = mdl_gov_cfg_default();
  cfg.rate_per_min  = 0U; /* isolate: only the backoff/Retry-After gate sleeps */
  mdl_governor_t gov;
  mdl_governor_init_clock(&gov, &cfg, 1U, fetch_now, &clk, fetch_sleep, &clk);
  g_fetch_gov = &gov;

  mdl_fetch_stats_t st;
  const ra8_err_t   rc =
    run_fetch(abs_dir, state_path, k_mdl_layout_separate, nullptr, false, 0U, &st);
  g_fetch_gov              = nullptr; /* restore the default (governor-less) path */
  g_mock.busy_on_file_call = 0U;

  TEST_ASSERT_EQ((int64_t)k_ra8_ok, rc);
  TEST_ASSERT_EQ((uint16_t)1, (uint16_t)st.pages_fetched); /* succeeded after the retry */
  TEST_ASSERT_EQ((uint16_t)1, (uint16_t)st.chapters_completed);
  TEST_ASSERT_EQ((uint16_t)2, (uint16_t)g_mock.get_file_calls); /* initial 503 + one retry      */
  TEST_ASSERT(clk.total_slept >= 1000);                         /* honoured the 1 s Retry-After */
  TEST_ASSERT(page_exists(abs_dir, "chapter-1/page_0001.jpg"));
  uint16_t level = 0U;
  TEST_ASSERT(mdl_governor_peek(&gov, "cdn", &level, nullptr)); /* host of a.jpg         */
  TEST_ASSERT_EQ((int64_t)1, (int64_t)level);                   /* one throttle recorded */
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
static void test_transient_page_retry_succeeds(void)
{
  TEST_BEGIN("transient page retry succeeds");
  setup_site();
  g_mock.map   = s_map3; /* chapter-1 has two pages (a,b) */
  g_mock.map_n = sizeof(s_map3) / sizeof(s_map3[0]);
  char abs_dir[PATH_MAX];
  char state_path[PATH_MAX];
  make_series_dir(abs_dir, sizeof(abs_dir), state_path, sizeof(state_path));
  mdl_state_init(&g_state);
  const char* c1[] = {"http://s/chapter-1"};
  set_chapters(c1, 1U);

  mdl_fetch_stats_t s;
  /* Fail only the very first page transfer; the bounded retry must recover it. */
  TEST_ASSERT_EQ((int64_t)k_ra8_ok,
                 run_fetch(abs_dir, state_path, k_mdl_layout_separate, nullptr, false, 1U, &s));
  TEST_ASSERT_EQ((uint16_t)2, (uint16_t)s.pages_fetched);
  TEST_ASSERT_EQ((uint16_t)0, (uint16_t)s.pages_failed);
  TEST_ASSERT_EQ((uint16_t)1, (uint16_t)s.chapters_completed);
  TEST_ASSERT_EQ((uint16_t)3, (uint16_t)g_mock.get_file_calls); /* a(fail)+a(ok)+b(ok) */
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
static void test_incomplete_chapter_not_completed_and_logged(void)
{
  TEST_BEGIN("incomplete chapter is not completed and is logged");
  setup_site();
  g_mock.map      = s_map3; /* chapter-1 has pages a,b */
  g_mock.map_n    = sizeof(s_map3) / sizeof(s_map3[0]);
  g_mock.fail_url = "http://cdn/b.jpg"; /* page b never succeeds */
  char abs_dir[PATH_MAX];
  char state_path[PATH_MAX];
  make_series_dir(abs_dir, sizeof(abs_dir), state_path, sizeof(state_path));
  mdl_state_init(&g_state);
  const char* c1[] = {"http://s/chapter-1"};
  set_chapters(c1, 1U);

  mdl_fetch_stats_t s;
  TEST_ASSERT_EQ((int64_t)k_ra8_fail,
                 run_fetch(abs_dir, state_path, k_mdl_layout_separate, nullptr, false, 0U, &s));
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
  TEST_ASSERT(g_mock.get_file_calls > (size_t)2); /* page b was retried, not abandoned */
  TEST_END("incomplete chapter is not completed and is logged");
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
static void test_mcdc_is_retryable(void)
{
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
static void test_mcdc_run_incomplete(void)
{
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
int32_t main(void)
{
  test_first_run_then_update_only_new();
  test_resume_equals_uninterrupted();
  test_content_dedup_across_chapters();
  test_corrupt_state_rebuilds();
  test_governor_retries_throttle();
  test_transient_page_retry_succeeds();
  test_incomplete_chapter_not_completed_and_logged();
  test_mcdc_is_retryable();
  test_mcdc_run_incomplete();
  (void)fprintf(stderr, "[OK  ] test_media_dl_fetch.c\n");
  return 0;
}
