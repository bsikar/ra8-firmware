/**
 * @file test_media_dl_fetch.c
 * @brief Resumable fetch orchestration, metadata, deduplication, and retry tests.
 * @details Proves uninterrupted and resumed runs converge on identical bytes and
 *          validates checkpoint failure, retry, metadata, and deduplication paths.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#include "test_media_dl_fetch_fixture.h"

/** @brief The chapter->HTML map used by the incremental test (three chapters).
 */
static const page_map_t s_map3[] = {
  {"http://s/chapter-1",
   "<h1 class=\"chapter-title\">Chapter One</h1><img "
   "data-src=\"http://cdn/a.jpg\"><img "
   "data-src=\"http://cdn/b.jpg\">"},
  {"http://s/chapter-2",
   "<h1 class=\"chapter-title\">Chapter Two</h1><img "
   "data-src=\"http://cdn/c.jpg\">"},
  {"http://s/chapter-3",
   "<h1 class=\"chapter-title\">Chapter Three</h1><img "
   "data-src=\"http://cdn/d.jpg\">"},
};

/** @brief Number of progress callbacks reached by the output-fault scenario. */
static size_t s_progress_fault_calls;

/** @brief Inject one deterministic diagnostic/output sink failure.
 * @details Implements this test-only seam with caller-owned fixtures, bounded storage, and explicit propagation of the result observed by its caller.
 * @param[in,out] ctx Opaque caller-owned fixture context.
 * @param[in] event Progress event supplied by the fetch workflow.
 * @return Canonical media-downloader or adapter status.
 * @retval k_ra8_ok The bounded helper operation completed.
 * @retval other The documented validation, storage, or sink error occurred.
 * @pre Pointer arguments satisfy their documented readable and writable extents.
 * @pre The caller retains ownership of every supplied fixture object.
 * @post Documented outputs reflect the processed fixture on success.
 * @post Failure preserves caller-owned resources as documented.
 * @note Host-only and synchronous; assertion failure terminates the test process.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_fail_progress(void* ctx, const mdl_fetch_progress_t* event)
{
  (void)ctx;
  TEST_ASSERT_NOT_NULL(event);
  s_progress_fault_calls += 1U;
  return k_ra8_err_comm_error;
}

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
 * @test internal_test_chapter_number_selector_strict_and_persisted
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
RA8_INTERNAL static void internal_test_chapter_number_selector_strict_and_persisted(void)
{
  TEST_BEGIN("chapter number selector strict + persisted");
  internal_mdl_fetch_test_setup_site();
  (void)__builtin_snprintf(s_site.chapter_number_selector,
                           sizeof(s_site.chapter_number_selector),
                           "%s",
                           "class:chapter-number");
  s_mock.map   = s_map_number_selector;
  s_mock.map_n = sizeof(s_map_number_selector) / sizeof(s_map_number_selector[0]);
  char abs_dir[PATH_MAX];
  char state_path[PATH_MAX];
  internal_mdl_fetch_test_make_series_dir(abs_dir, sizeof(abs_dir), state_path, sizeof(state_path));
  mdl_state_init(&s_state);

  const char* valid[] = {"http://s/chapter-special"};
  internal_mdl_fetch_test_set_chapters(valid, 1U);
  mdl_fetch_stats_t valid_stats = {};
  TEST_ASSERT_EQ((int64_t)k_ra8_ok,
                 internal_mdl_fetch_test_run(abs_dir,
                                             state_path,
                                             k_mdl_layout_separate,
                                             nullptr,
                                             false,
                                             0U,
                                             &valid_stats));
  const mdl_chapter_rec_t* chapter = mdl_state_find_chapter(&s_state, "chapter-special");
  TEST_ASSERT_NOT_NULL(chapter);
  TEST_ASSERT(chapter->number_known);
  TEST_ASSERT(chapter->number == 108.5);

  mdl_state_init(&s_state);
  memset(&s_faillog, 0, sizeof(s_faillog));
  s_mock.get_file_calls = 0U;
  const char* invalid[] = {"http://s/chapter-invalid"};
  internal_mdl_fetch_test_set_chapters(invalid, 1U);
  mdl_fetch_stats_t invalid_stats = {};
  TEST_ASSERT_EQ((int64_t)k_ra8_fail,
                 internal_mdl_fetch_test_run(abs_dir,
                                             state_path,
                                             k_mdl_layout_separate,
                                             nullptr,
                                             false,
                                             0U,
                                             &invalid_stats));
  TEST_ASSERT_EQ((int64_t)0, (int64_t)s_mock.get_file_calls);
  TEST_ASSERT_EQ((int64_t)1, (int64_t)s_faillog.count);
  TEST_ASSERT_EQ((int64_t)k_ra8_err_validation_failed, (int64_t)s_faillog.items[0].err);
  TEST_END("chapter number selector strict + persisted");
}

/** @test Strict numeric text parsing consumes the complete trimmed selector
 * @brief Exercise the chapter number text parser regression scenario.
 * @details Executes the chapter number text parser scenario through production interfaces and checks its observable success, rejection, and boundary results.
 * @pre Assertions are enabled for contract verification.
 * @pre The test fixture workspace is isolated for this scenario.
 * @post Normal return means every reached contract assertion passed.
 * @post The caller receives no transferred fixture ownership.
 * @note Test helper; an assertion failure terminates the test process.
 * @since 0.1.0
 * result. */
RA8_INTERNAL static void internal_test_chapter_number_text_parser(void)
{
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
 * @test internal_test_first_run_then_update_only_new
 *
 * @par MC/DC:
 * (No compound decision under test; it proves the incremental contract: a fresh
 * run fetches every page, and a later `--update` over a reloaded state with one
 * new chapter fetches ONLY that chapter -- issuing no HTML request for the
 * chapters already recorded complete.)
 * @brief Exercise the first run then update only new media-downloader scenario.
 * @details Exercises the first run then update only new scenario through production media-downloader interfaces and checks its observable success, rejection, and boundary results.
 * @pre The host test process exclusively owns its fixture state.
 * @pre Required fakes and bounded buffers are initialized before use.
 * @post Normal return means every scenario assertion passed.
 * @post No fixture resource ownership is transferred by this test.
 * @note Host-only and synchronous; assertion failure terminates the test process.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_first_run_then_update_only_new(void)
{
  TEST_BEGIN("first run + update only new");
  internal_mdl_fetch_test_setup_site();
  (void)__builtin_snprintf(s_site.chapter_title_selector,
                           sizeof(s_site.chapter_title_selector),
                           "%s",
                           "class:chapter-title");
  s_mock.map   = s_map3;
  s_mock.map_n = sizeof(s_map3) / sizeof(s_map3[0]);
  char abs_dir[PATH_MAX];
  char state_path[PATH_MAX];
  internal_mdl_fetch_test_make_series_dir(abs_dir, sizeof(abs_dir), state_path, sizeof(state_path));
  mdl_state_init(&s_state);

  /* Fresh run of chapters 1 and 2: every page is fetched. */
  const char* c12[] = {"http://s/chapter-1", "http://s/chapter-2"};
  internal_mdl_fetch_test_set_chapters(c12, 2U);
  mdl_fetch_stats_t s1;
  TEST_ASSERT_EQ((int64_t)k_ra8_ok,
                 internal_mdl_fetch_test_run(abs_dir,
                                             state_path,
                                             k_mdl_layout_separate,
                                             nullptr,
                                             false,
                                             0U,
                                             &s1));
  TEST_ASSERT_EQ((uint16_t)3, (uint16_t)s1.pages_fetched);
  TEST_ASSERT_EQ((uint16_t)0, (uint16_t)s1.pages_reused);
  TEST_ASSERT_EQ((uint16_t)2, (uint16_t)s1.chapters_completed);
  TEST_ASSERT(internal_mdl_fetch_test_page_exists(abs_dir, "chapter-1/page_0001.jpg"));
  TEST_ASSERT(internal_mdl_fetch_test_page_exists(abs_dir, "chapter-2/page_0001.jpg"));
  TEST_ASSERT(strcmp(mdl_state_find_chapter(&s_state, "chapter-1")->title, "Chapter One") == 0);

  /* Reload the state from disk (simulating a fresh process). */
  TEST_ASSERT_EQ((int64_t)k_ra8_ok, mdl_state_load(&s_storage, state_path, &s_state));

  /* --update with chapter 3 newly present: only chapter 3 is fetched. */
  const char* c123[] = {"http://s/chapter-1", "http://s/chapter-2", "http://s/chapter-3"};
  internal_mdl_fetch_test_set_chapters(c123, 3U);
  mdl_fetch_stats_t s2;
  TEST_ASSERT_EQ((int64_t)k_ra8_ok,
                 internal_mdl_fetch_test_run(abs_dir,
                                             state_path,
                                             k_mdl_layout_separate,
                                             nullptr,
                                             true,
                                             0U,
                                             &s2));
  TEST_ASSERT_EQ((uint16_t)2, (uint16_t)s2.chapters_skipped);
  TEST_ASSERT_EQ((uint16_t)1, (uint16_t)s2.chapters_completed);
  TEST_ASSERT_EQ((uint16_t)1, (uint16_t)s2.pages_fetched);
  /* Only chapter 3's HTML was requested; the complete chapters were untouched.
   */
  TEST_ASSERT_EQ((uint16_t)1, (uint16_t)s_mock.get_buf_calls);
  TEST_ASSERT_EQ((uint16_t)1, (uint16_t)s_mock.get_file_calls);
  TEST_ASSERT(internal_mdl_fetch_test_page_exists(abs_dir, "chapter-3/page_0001.jpg"));
  TEST_END("first run + update only new");
}

/**
 * @test internal_test_chapter_title_overflow_fails_without_page_fetch
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
RA8_INTERNAL static void internal_test_chapter_title_overflow_fails_without_page_fetch(void)
{
  TEST_BEGIN("chapter title overflow hard-fails");
  internal_mdl_fetch_test_setup_site();
  (void)__builtin_snprintf(s_site.chapter_title_selector,
                           sizeof(s_site.chapter_title_selector),
                           "%s",
                           "class:chapter-title");
  s_mock.map   = s_map_title_overflow;
  s_mock.map_n = sizeof(s_map_title_overflow) / sizeof(s_map_title_overflow[0]);
  char abs_dir[PATH_MAX];
  char state_path[PATH_MAX];
  internal_mdl_fetch_test_make_series_dir(abs_dir, sizeof(abs_dir), state_path, sizeof(state_path));
  mdl_state_init(&s_state);
  const char* chapters[] = {"http://s/chapter-9"};
  internal_mdl_fetch_test_set_chapters(chapters, 1U);
  mdl_fetch_stats_t stats = {};
  TEST_ASSERT_EQ((int64_t)k_ra8_fail,
                 internal_mdl_fetch_test_run(abs_dir,
                                             state_path,
                                             k_mdl_layout_separate,
                                             nullptr,
                                             false,
                                             0U,
                                             &stats));
  TEST_ASSERT_EQ((int64_t)1, (int64_t)stats.chapters_failed);
  TEST_ASSERT_EQ((int64_t)0, (int64_t)s_mock.get_file_calls);
  TEST_ASSERT_EQ((int64_t)1, (int64_t)s_faillog.count);
  TEST_ASSERT_EQ((int64_t)k_ra8_err_invalid_size, (int64_t)s_faillog.items[0].err);
  const mdl_chapter_rec_t* chapter = mdl_state_find_chapter(&s_state, "chapter-9");
  TEST_ASSERT_NOT_NULL(chapter);
  TEST_ASSERT(!chapter->complete);
  TEST_ASSERT(strcmp(chapter->title, "") == 0);
  TEST_END("chapter title overflow hard-fails");
}

/**
 * @test --refetch bypasses an otherwise valid, reusable local page.
 * @brief Exercise the refetch bypasses valid cache regression scenario.
 * @details Executes the refetch bypasses valid cache scenario through production interfaces and checks its observable success, rejection, and boundary results.
 * @pre Assertions are enabled for contract verification.
 * @pre The test fixture workspace is isolated for this scenario.
 * @post Normal return means every reached contract assertion passed.
 * @post The caller receives no transferred fixture ownership.
 * @note Test helper; an assertion failure terminates the test process.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_refetch_bypasses_valid_cache(void)
{
  TEST_BEGIN("refetch bypasses valid cache");
  internal_mdl_fetch_test_setup_site();
  (void)__builtin_snprintf(s_site.chapter_title_selector,
                           sizeof(s_site.chapter_title_selector),
                           "%s",
                           "class:not-present");
  s_mock.map   = s_map3;
  s_mock.map_n = sizeof(s_map3) / sizeof(s_map3[0]);
  char abs_dir[PATH_MAX];
  char state_path[PATH_MAX];
  internal_mdl_fetch_test_make_series_dir(abs_dir, sizeof(abs_dir), state_path, sizeof(state_path));
  mdl_state_init(&s_state);
  const char* chapters[] = {"http://s/chapter-2"};
  internal_mdl_fetch_test_set_chapters(chapters, 1U);
  mdl_fetch_stats_t first = {};
  TEST_ASSERT_EQ((int64_t)k_ra8_ok,
                 internal_mdl_fetch_test_run(abs_dir,
                                             state_path,
                                             k_mdl_layout_separate,
                                             nullptr,
                                             false,
                                             0U,
                                             &first));
  TEST_ASSERT_EQ((int64_t)1, (int64_t)first.pages_fetched);
  TEST_ASSERT(strcmp(mdl_state_find_chapter(&s_state, "chapter-2")->title, "chapter-2") == 0);

  s_refetch                = true;
  mdl_fetch_stats_t second = {};
  TEST_ASSERT_EQ((int64_t)k_ra8_ok,
                 internal_mdl_fetch_test_run(abs_dir,
                                             state_path,
                                             k_mdl_layout_separate,
                                             nullptr,
                                             false,
                                             0U,
                                             &second));
  TEST_ASSERT_EQ((int64_t)1, (int64_t)second.pages_fetched);
  TEST_ASSERT_EQ((int64_t)0, (int64_t)second.pages_reused);
  TEST_ASSERT_EQ((int64_t)1, (int64_t)s_mock.get_file_calls);
  TEST_END("refetch bypasses valid cache");
}

/** @brief The two-chapter map used by the combined resume test. */
static const page_map_t s_map_combined[] = {
  {"http://s/chapter-1", "<img data-src=\"http://cdn/a.jpg\"><img data-src=\"http://cdn/b.jpg\">"},
  {"http://s/chapter-2", "<img data-src=\"http://cdn/c.jpg\"><img data-src=\"http://cdn/d.jpg\">"},
};

/**
 * @brief Assert two combined-layout downloads contain identical page bytes.
 * @details Compares the four expected chapter-relative page paths so the resume
 *          regression proves byte equivalence, not merely matching counters.
 * @param[in] dir_a First canonical fixture root.
 * @param[in] dir_b Second canonical fixture root.
 * @pre Assertions are enabled for contract verification.
 * @pre The test fixture workspace is isolated for this scenario.
 * @post Normal return means every reached contract assertion passed.
 * @post The caller receives no transferred fixture ownership.
 * @note Test helper; an assertion failure terminates the test process.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_mdl_fetch_test_assert_combined_pages_equal(const char* dir_a,
                                                                             const char* dir_b)
{
  const char* rel[] = {"foo-1-2/page_0001.jpg",
                       "foo-1-2/page_0002.jpg",
                       "foo-1-2/page_0003.jpg",
                       "foo-1-2/page_0004.jpg"};
  for (size_t i = 0U; i < (sizeof(rel) / sizeof(rel[0])); ++i) {
    char pa[k_join_bytes];
    char pb[k_join_bytes];
    (void)__builtin_snprintf(pa, sizeof(pa), "%s/%s", dir_a, rel[i]);
    (void)__builtin_snprintf(pb, sizeof(pb), "%s/%s", dir_b, rel[i]);
    TEST_ASSERT(internal_mdl_fetch_test_files_equal(pa, pb));
  }
}

/**
 * @test internal_test_resume_equals_uninterrupted
 *
 * @par MC/DC:
 * (No compound decision under test; it proves an interrupted combined download,
 * resumed from its on-disk state, reproduces a byte-identical result with the
 * same continuous page numbering as an uninterrupted run -- the page-4 fetch
 * that failed is the only one repeated on resume, pages 1-3 being reused.)
 * @brief Exercise the resume equals uninterrupted media-downloader scenario.
 * @details Exercises the resume equals uninterrupted scenario through production media-downloader interfaces and checks its observable success, rejection, and boundary results.
 * @pre The host test process exclusively owns its fixture state.
 * @pre Required fakes and bounded buffers are initialized before use.
 * @post Normal return means every scenario assertion passed.
 * @post No fixture resource ownership is transferred by this test.
 * @note Host-only and synchronous; assertion failure terminates the test process.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_resume_equals_uninterrupted(void)
{
  TEST_BEGIN("resume equals uninterrupted");
  internal_mdl_fetch_test_setup_site();
  s_mock.map        = s_map_combined;
  s_mock.map_n      = sizeof(s_map_combined) / sizeof(s_map_combined[0]);
  const char* c12[] = {"http://s/chapter-1", "http://s/chapter-2"};

  /* (a) Uninterrupted combined run into dir A: pages 1..4, continuous. */
  char dir_a[PATH_MAX];
  char state_a[PATH_MAX];
  internal_mdl_fetch_test_make_series_dir(dir_a, sizeof(dir_a), state_a, sizeof(state_a));
  mdl_state_init(&s_state);
  internal_mdl_fetch_test_set_chapters(c12, 2U);
  mdl_fetch_stats_t sa;
  TEST_ASSERT_EQ(
    (int64_t)k_ra8_ok,
    internal_mdl_fetch_test_run(dir_a, state_a, k_mdl_layout_combined, "foo-1-2", false, 0U, &sa));
  TEST_ASSERT_EQ((uint16_t)4, (uint16_t)sa.pages_fetched);
  TEST_ASSERT(internal_mdl_fetch_test_page_exists(dir_a, "foo-1-2/page_0004.jpg"));

  /* (b) Interrupted combined run into dir B: page 4 (d.jpg) is unreachable on
   *     every attempt, so its retries all fail and the chapter is left partial.
   */
  char dir_b[PATH_MAX];
  char state_b[PATH_MAX];
  internal_mdl_fetch_test_make_series_dir(dir_b, sizeof(dir_b), state_b, sizeof(state_b));
  mdl_state_init(&s_state);
  internal_mdl_fetch_test_set_chapters(c12, 2U);
  s_mock.fail_url = "http://cdn/d.jpg";
  mdl_fetch_stats_t sb;
  TEST_ASSERT_EQ(
    (int64_t)k_ra8_fail,
    internal_mdl_fetch_test_run(dir_b, state_b, k_mdl_layout_combined, "foo-1-2", false, 0U, &sb));
  TEST_ASSERT_EQ((uint16_t)1, (uint16_t)sb.chapters_failed);
  TEST_ASSERT(!internal_mdl_fetch_test_page_exists(dir_b, "foo-1-2/page_0004.jpg"));

  /* (c) Resume from dir B's on-disk state, page 4 now reachable: only it is
   *     re-fetched. */
  s_mock.fail_url = nullptr;
  TEST_ASSERT_EQ((int64_t)k_ra8_ok, mdl_state_load(&s_storage, state_b, &s_state));
  internal_mdl_fetch_test_set_chapters(c12, 2U);
  mdl_fetch_stats_t sc;
  TEST_ASSERT_EQ(
    (int64_t)k_ra8_ok,
    internal_mdl_fetch_test_run(dir_b, state_b, k_mdl_layout_combined, "foo-1-2", false, 0U, &sc));
  TEST_ASSERT_EQ((uint16_t)1, (uint16_t)sc.pages_fetched); /* only the missing page */
  TEST_ASSERT_EQ((uint16_t)3, (uint16_t)sc.pages_reused);  /* pages 1..3 reused     */
  TEST_ASSERT(internal_mdl_fetch_test_page_exists(dir_b, "foo-1-2/page_0004.jpg"));

  /* The resumed output equals the uninterrupted one, page for page. */
  internal_mdl_fetch_test_assert_combined_pages_equal(dir_a, dir_b);
  TEST_END("resume equals uninterrupted");
}

/** @brief The map for the dedup test: chapters 1 and 2 share image A. */
static const page_map_t s_map_shared[] = {
  {"http://s/chapter-1", "<img data-src=\"http://cdn/a.jpg\"><img data-src=\"http://cdn/b.jpg\">"},
  {"http://s/chapter-2", "<img data-src=\"http://cdn/a.jpg\"><img data-src=\"http://cdn/e.jpg\">"},
};

/**
 * @test internal_test_content_dedup_across_chapters
 *
 * @par MC/DC:
 * (No compound decision under test; it proves a byte-identical image shared by
 * two chapters is fetched once and reused for the second chapter -- the shared
 * page is served from the already-held file, and the reused copy is identical.)
 * @brief Exercise the content dedup across chapters media-downloader scenario.
 * @details Exercises the content dedup across chapters scenario through production media-downloader interfaces and checks its observable success, rejection, and boundary results.
 * @pre The host test process exclusively owns its fixture state.
 * @pre Required fakes and bounded buffers are initialized before use.
 * @post Normal return means every scenario assertion passed.
 * @post No fixture resource ownership is transferred by this test.
 * @note Host-only and synchronous; assertion failure terminates the test process.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_content_dedup_across_chapters(void)
{
  TEST_BEGIN("content dedup across chapters");
  internal_mdl_fetch_test_setup_site();
  s_mock.map   = s_map_shared;
  s_mock.map_n = sizeof(s_map_shared) / sizeof(s_map_shared[0]);
  char abs_dir[PATH_MAX];
  char state_path[PATH_MAX];
  internal_mdl_fetch_test_make_series_dir(abs_dir, sizeof(abs_dir), state_path, sizeof(state_path));
  mdl_state_init(&s_state);
  const char* c12[] = {"http://s/chapter-1", "http://s/chapter-2"};
  internal_mdl_fetch_test_set_chapters(c12, 2U);

  mdl_fetch_stats_t s;
  TEST_ASSERT_EQ((int64_t)k_ra8_ok,
                 internal_mdl_fetch_test_run(abs_dir,
                                             state_path,
                                             k_mdl_layout_separate,
                                             nullptr,
                                             false,
                                             0U,
                                             &s));
  /* Four page slots (a,b,a,e) but only three unique fetches: A is reused. */
  TEST_ASSERT_EQ((uint16_t)3, (uint16_t)s.pages_fetched);
  TEST_ASSERT_EQ((uint16_t)1, (uint16_t)s.pages_reused);
  TEST_ASSERT_EQ((uint16_t)3, (uint16_t)s_mock.get_file_calls);
  /* The reused copy in chapter 2 is byte-identical to chapter 1's page 1. */
  char p1[k_join_bytes];
  char p2[k_join_bytes];
  (void)__builtin_snprintf(p1, sizeof(p1), "%s/chapter-1/page_0001.jpg", abs_dir);
  (void)__builtin_snprintf(p2, sizeof(p2), "%s/chapter-2/page_0001.jpg", abs_dir);
  TEST_ASSERT(internal_mdl_fetch_test_files_equal(p1, p2));
  TEST_END("content dedup across chapters");
}

/**
 * @test internal_test_corrupt_state_rebuilds
 *
 * @par MC/DC:
 * (No compound decision under test; it proves a corrupt state file is reported
 * as invalid_state and, after the caller reinitialises, the fetch loop rebuilds
 * from scratch and downloads normally rather than crashing or silently doing
 * nothing.)
 * @brief Exercise the corrupt state rebuilds media-downloader scenario.
 * @details Exercises the corrupt state rebuilds scenario through production media-downloader interfaces and checks its observable success, rejection, and boundary results.
 * @pre The host test process exclusively owns its fixture state.
 * @pre Required fakes and bounded buffers are initialized before use.
 * @post Normal return means every scenario assertion passed.
 * @post No fixture resource ownership is transferred by this test.
 * @note Host-only and synchronous; assertion failure terminates the test process.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_corrupt_state_rebuilds(void)
{
  TEST_BEGIN("corrupt state rebuilds");
  internal_mdl_fetch_test_setup_site();
  s_mock.map   = s_map3;
  s_mock.map_n = sizeof(s_map3) / sizeof(s_map3[0]);
  char abs_dir[PATH_MAX];
  char state_path[PATH_MAX];
  internal_mdl_fetch_test_make_series_dir(abs_dir, sizeof(abs_dir), state_path, sizeof(state_path));

  /* Plant a corrupt state file, as a kill mid-write (pre-atomic) might have. */
  static const uint8_t corrupt[] = "corrupt garbage not a state file\n";
  TEST_ASSERT(internal_mdl_fetch_test_write_bytes(state_path, corrupt, sizeof(corrupt) - 1U));

  /* The load reports the corruption and leaves an empty, valid state. */
  TEST_ASSERT_EQ((int64_t)k_ra8_err_invalid_state,
                 mdl_state_load(&s_storage, state_path, &s_state));
  TEST_ASSERT_EQ((uint16_t)0, s_state.chapter_count);

  /* The fetch then rebuilds and downloads normally. */
  const char* c1[] = {"http://s/chapter-1"};
  internal_mdl_fetch_test_set_chapters(c1, 1U);
  mdl_fetch_stats_t s;
  TEST_ASSERT_EQ((int64_t)k_ra8_ok,
                 internal_mdl_fetch_test_run(abs_dir,
                                             state_path,
                                             k_mdl_layout_separate,
                                             nullptr,
                                             false,
                                             0U,
                                             &s));
  TEST_ASSERT_EQ((uint16_t)2, (uint16_t)s.pages_fetched);
  TEST_ASSERT_EQ((uint16_t)1, (uint16_t)s.chapters_completed);
  TEST_END("corrupt state rebuilds");
}

/**
 * @test internal_test_governor_retries_throttle
 *
 * @par MC/DC:
 * (No compound decision in this test; it proves the #301 integration: when the
 * mdl_net mock answers the first image transfer with 503 + `Retry-After: 1`,
 * the fetch loop feeds it to the governor, waits the honoured 1 s through the
 * injected clock, retries, and completes -- rather than hammering or abandoning
 * the page. Backoff observation is confirmed via ::mdl_governor_peek.)
 * @brief Exercise the governor retries throttle media-downloader scenario.
 * @details Exercises the governor retries throttle scenario through production media-downloader interfaces and checks its observable success, rejection, and boundary results.
 * @pre The host test process exclusively owns its fixture state.
 * @pre Required fakes and bounded buffers are initialized before use.
 * @post Normal return means every scenario assertion passed.
 * @post No fixture resource ownership is transferred by this test.
 * @note Host-only and synchronous; assertion failure terminates the test process.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_governor_retries_throttle(void)
{
  TEST_BEGIN("fetch loop honours governor backoff");
  internal_mdl_fetch_test_setup_site();
  s_mock.map               = s_map1;
  s_mock.map_n             = sizeof(s_map1) / sizeof(s_map1[0]);
  s_mock.busy_on_file_call = 1U;  /* first image transfer is throttled */
  s_mock.busy_retry_after  = "1"; /* Retry-After: 1 second             */
  char abs_dir[PATH_MAX];
  char state_path[PATH_MAX];
  internal_mdl_fetch_test_make_series_dir(abs_dir, sizeof(abs_dir), state_path, sizeof(state_path));
  mdl_state_init(&s_state);
  const char* c1[] = {"http://s/chapter-1"};
  internal_mdl_fetch_test_set_chapters(c1, 1U);

  fetch_clock_t clk = {};
  mdl_gov_cfg_t cfg = mdl_gov_cfg_default();
  cfg.rate_per_min  = 0U; /* isolate: only the backoff/Retry-After gate sleeps */
  mdl_governor_t gov;
  mdl_governor_init_clock(&gov,
                          &cfg,
                          1U,
                          internal_mdl_fetch_test_now,
                          &clk,
                          internal_mdl_fetch_test_sleep,
                          &clk);
  s_fetch_gov = &gov;

  mdl_fetch_stats_t st;
  const ra8_err_t   rc     = internal_mdl_fetch_test_run(abs_dir,
                                                         state_path,
                                                         k_mdl_layout_separate,
                                                         nullptr,
                                                         false,
                                                         0U,
                                                         &st);
  s_fetch_gov              = nullptr; /* restore the default (governor-less) path */
  s_mock.busy_on_file_call = 0U;

  TEST_ASSERT_EQ((int64_t)k_ra8_ok, rc);
  TEST_ASSERT_EQ((uint16_t)1, (uint16_t)st.pages_fetched); /* succeeded after the retry */
  TEST_ASSERT_EQ((uint16_t)1, (uint16_t)st.chapters_completed);
  TEST_ASSERT_EQ((uint16_t)2, (uint16_t)s_mock.get_file_calls); /* initial 503 + one retry      */
  TEST_ASSERT(clk.total_slept >= 1000);                         /* honoured the 1 s Retry-After */
  TEST_ASSERT(internal_mdl_fetch_test_page_exists(abs_dir, "chapter-1/page_0001.jpg"));
  uint16_t level = 0U;
  TEST_ASSERT(mdl_governor_peek(&gov, "cdn", &level, nullptr)); /* host of a.jpg         */
  TEST_ASSERT_EQ((int64_t)1, (int64_t)level);                   /* one throttle recorded */
  TEST_END("fetch loop honours governor backoff");
}

/**
 * @test internal_test_transient_page_retry_succeeds
 *
 * @par MC/DC:
 * (No compound decision under test; it proves a single transient transport
 * failure on a page is recovered by the bounded retry -- the page's second
 * attempt succeeds, the chapter completes, and nothing is recorded as failed.)
 * @brief Exercise the transient page retry succeeds media-downloader scenario.
 * @details Exercises the transient page retry succeeds scenario through production media-downloader interfaces and checks its observable success, rejection, and boundary results.
 * @pre The host test process exclusively owns its fixture state.
 * @pre Required fakes and bounded buffers are initialized before use.
 * @post Normal return means every scenario assertion passed.
 * @post No fixture resource ownership is transferred by this test.
 * @note Host-only and synchronous; assertion failure terminates the test process.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_transient_page_retry_succeeds(void)
{
  TEST_BEGIN("transient page retry succeeds");
  internal_mdl_fetch_test_setup_site();
  s_mock.map   = s_map3; /* chapter-1 has two pages (a,b) */
  s_mock.map_n = sizeof(s_map3) / sizeof(s_map3[0]);
  char abs_dir[PATH_MAX];
  char state_path[PATH_MAX];
  internal_mdl_fetch_test_make_series_dir(abs_dir, sizeof(abs_dir), state_path, sizeof(state_path));
  mdl_state_init(&s_state);
  const char* c1[] = {"http://s/chapter-1"};
  internal_mdl_fetch_test_set_chapters(c1, 1U);

  mdl_fetch_stats_t s;
  /* Fail only the very first page transfer; the bounded retry must recover it.
   */
  TEST_ASSERT_EQ((int64_t)k_ra8_ok,
                 internal_mdl_fetch_test_run(abs_dir,
                                             state_path,
                                             k_mdl_layout_separate,
                                             nullptr,
                                             false,
                                             1U,
                                             &s));
  TEST_ASSERT_EQ((uint16_t)2, (uint16_t)s.pages_fetched);
  TEST_ASSERT_EQ((uint16_t)0, (uint16_t)s.pages_failed);
  TEST_ASSERT_EQ((uint16_t)1, (uint16_t)s.chapters_completed);
  TEST_ASSERT_EQ((uint16_t)3, (uint16_t)s_mock.get_file_calls); /* a(fail)+a(ok)+b(ok) */
  TEST_ASSERT_EQ((uint16_t)0, (uint16_t)s_faillog.total);
  TEST_END("transient page retry succeeds");
}

/**
 * @test internal_test_incomplete_chapter_not_completed_and_logged
 *
 * @par MC/DC:
 * (No compound decision under test; it proves a page that fails every retry
 * leaves its chapter recorded incomplete -- never marked complete -- and is
 * captured in the run's failure log with its URL, so a resume re-fetches it and
 * the run is reported honestly rather than packaged as if whole.)
 * @brief Exercise the incomplete chapter not completed and logged media-downloader scenario.
 * @details Exercises the incomplete chapter not completed and logged scenario through production media-downloader interfaces and checks its observable success, rejection, and boundary results.
 * @pre The host test process exclusively owns its fixture state.
 * @pre Required fakes and bounded buffers are initialized before use.
 * @post Normal return means every scenario assertion passed.
 * @post No fixture resource ownership is transferred by this test.
 * @note Host-only and synchronous; assertion failure terminates the test process.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_incomplete_chapter_not_completed_and_logged(void)
{
  TEST_BEGIN("incomplete chapter is not completed and is logged");
  internal_mdl_fetch_test_setup_site();
  s_mock.map      = s_map3; /* chapter-1 has pages a,b */
  s_mock.map_n    = sizeof(s_map3) / sizeof(s_map3[0]);
  s_mock.fail_url = "http://cdn/b.jpg"; /* page b never succeeds */
  char abs_dir[PATH_MAX];
  char state_path[PATH_MAX];
  internal_mdl_fetch_test_make_series_dir(abs_dir, sizeof(abs_dir), state_path, sizeof(state_path));
  mdl_state_init(&s_state);
  const char* c1[] = {"http://s/chapter-1"};
  internal_mdl_fetch_test_set_chapters(c1, 1U);

  mdl_fetch_stats_t s;
  TEST_ASSERT_EQ((int64_t)k_ra8_fail,
                 internal_mdl_fetch_test_run(abs_dir,
                                             state_path,
                                             k_mdl_layout_separate,
                                             nullptr,
                                             false,
                                             0U,
                                             &s));
  s_mock.fail_url = nullptr; /* restore for later tests */

  TEST_ASSERT_EQ((uint16_t)1, (uint16_t)s.pages_fetched); /* page a */
  TEST_ASSERT_EQ((uint16_t)1, (uint16_t)s.pages_failed);  /* page b */
  TEST_ASSERT_EQ((uint16_t)1, (uint16_t)s.chapters_failed);
  TEST_ASSERT_EQ((uint16_t)0, (uint16_t)s.chapters_completed);
  /* The chapter is recorded incomplete, so a later run resumes it. */
  TEST_ASSERT(!mdl_state_chapter_complete(&s_state, "chapter-1"));
  /* The failure is logged with the offending URL for the end-of-run summary. */
  TEST_ASSERT_EQ((uint16_t)1, (uint16_t)s_faillog.total);
  TEST_ASSERT_EQ((uint16_t)1, (uint16_t)s_faillog.count);
  TEST_ASSERT_EQ((int64_t)k_ra8_fail, (int64_t)s_faillog.items[0].err);
  TEST_ASSERT(strcmp(s_faillog.items[0].url, "http://cdn/b.jpg") == 0);
  TEST_ASSERT(s_mock.get_file_calls > (size_t)2); /* page b was retried, not abandoned */
  TEST_END("incomplete chapter is not completed and is logged");
}

/** @test A state checkpoint failure makes the successful page run fail
 * @brief Exercise the checkpoint failure fails run regression scenario.
 * @details Executes the checkpoint failure fails run scenario through production interfaces and checks its observable success, rejection, and boundary results.
 * @pre Assertions are enabled for contract verification.
 * @pre The test fixture workspace is isolated for this scenario.
 * @post Normal return means every reached contract assertion passed.
 * @post The caller receives no transferred fixture ownership.
 * @note Test helper; an assertion failure terminates the test process.
 * @since 0.1.0
 * honestly. */
RA8_INTERNAL static void internal_test_checkpoint_failure_fails_run(void)
{
  TEST_BEGIN("checkpoint failure fails run");
  internal_mdl_fetch_test_setup_site();
  s_mock.map   = s_map1;
  s_mock.map_n = sizeof(s_map1) / sizeof(s_map1[0]);
  char abs_dir[PATH_MAX];
  char state_path[PATH_MAX];
  internal_mdl_fetch_test_make_series_dir(abs_dir, sizeof(abs_dir), state_path, sizeof(state_path));
  mdl_state_init(&s_state);
  const char* c1[] = {"http://s/chapter-1"};
  internal_mdl_fetch_test_set_chapters(c1, 1U);

  mdl_fetch_stats_t stats;
  TEST_ASSERT_EQ(
    (int64_t)k_ra8_fail,
    internal_mdl_fetch_test_run(abs_dir,
                                abs_dir, /* rename cannot replace the existing series directory */
                                k_mdl_layout_separate,
                                nullptr,
                                false,
                                0U,
                                &stats));
  TEST_ASSERT_EQ((uint16_t)1, (uint16_t)stats.chapters_failed);
  TEST_ASSERT_EQ((uint16_t)0, (uint16_t)stats.chapters_completed);
  TEST_ASSERT(s_faillog.total > 0U);
  TEST_ASSERT(strcmp(s_faillog.items[s_faillog.count - 1U].url, abs_dir) == 0);
  TEST_END("checkpoint failure fails run");
}

/**
 * @test A failed progress sink stops the remaining page and state work.
 * @brief Verify explicit progress error propagation and early termination.
 * @details Uses a two-page chapter and rejects the first completed-page event;
 *          the second page must never reach the injected network backend.
 * @pre The portable POSIX storage fixture is initialized.
 * @pre The scripted chapter exposes exactly two supported image URLs.
 * @post The callback error is returned without translation.
 * @post Exactly one page transfer occurs and the chapter remains incomplete.
 * @note Test helper; an assertion failure terminates the test process.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_progress_failure_stops_run(void)
{
  TEST_BEGIN("progress failure stops remaining work");
  internal_mdl_fetch_test_setup_site();
  s_mock.map   = s_map3;
  s_mock.map_n = 1U;
  char abs_dir[PATH_MAX];
  char state_path[PATH_MAX];
  internal_mdl_fetch_test_make_series_dir(abs_dir, sizeof(abs_dir), state_path, sizeof(state_path));
  mdl_state_init(&s_state);
  const char* chapter[] = {"http://s/chapter-1"};
  internal_mdl_fetch_test_set_chapters(chapter, 1U);
  s_progress_fault_calls = 0U;
  s_fetch_progress_fn    = internal_fail_progress;
  s_fetch_progress_ctx   = nullptr;
  mdl_fetch_stats_t stats;
  const ra8_err_t   error = internal_mdl_fetch_test_run(abs_dir,
                                                        state_path,
                                                        k_mdl_layout_separate,
                                                        nullptr,
                                                        false,
                                                        0U,
                                                        &stats);
  s_fetch_progress_fn     = nullptr;
  s_fetch_progress_ctx    = nullptr;
  TEST_ASSERT_EQ((int64_t)k_ra8_err_comm_error, (int64_t)error);
  TEST_ASSERT_EQ((uint16_t)1, (uint16_t)s_progress_fault_calls);
  TEST_ASSERT_EQ((uint16_t)1, (uint16_t)s_mock.get_file_calls);
  TEST_ASSERT_EQ((uint16_t)1, (uint16_t)stats.pages_fetched);
  TEST_ASSERT(!mdl_state_chapter_complete(&s_state, "chapter-1"));
  TEST_END("progress failure stops remaining work");
}

int main(void)
{
  const fw_fs_posix_cfg_t cfg = {.root_path = "/", .removable_media = false};
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_posix_init(&s_fs, &s_fs_posix, &cfg));
  TEST_ASSERT_EQ(k_ra8_ok,
                 mdl_storage_init(&s_storage,
                                  &s_fs,
                                  s_file_work.bytes,
                                  sizeof(s_file_work.bytes),
                                  s_transaction_work.bytes,
                                  sizeof(s_transaction_work.bytes),
                                  s_io_buffer,
                                  sizeof(s_io_buffer)));
  internal_test_chapter_number_selector_strict_and_persisted();
  internal_test_chapter_number_text_parser();
  internal_test_first_run_then_update_only_new();
  internal_test_chapter_title_overflow_fails_without_page_fetch();
  internal_test_refetch_bypasses_valid_cache();
  internal_test_resume_equals_uninterrupted();
  internal_test_content_dedup_across_chapters();
  internal_test_corrupt_state_rebuilds();
  internal_test_governor_retries_throttle();
  internal_test_transient_page_retry_succeeds();
  internal_test_incomplete_chapter_not_completed_and_logged();
  internal_test_checkpoint_failure_fails_run();
  internal_test_progress_failure_stops_run();
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_posix_deinit(&s_fs_posix));
  (void)internal_test_output_fd_text(STDERR_FILENO, "[OK  ] test_media_dl_fetch.c\n");
  return 0;
}
