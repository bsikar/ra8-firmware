/**
 * @file test_media_dl_fetch_cache.c
 * @brief Fetch cache validators, content typing, and standalone asset tests.
 * @details Isolates conditional 304 reuse, changed-content replacement, retry
 *          decisions, and incomplete-run conjunctions with scripted dependencies.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#include "test_media_dl_fetch_fixture.h"

/**
 * @test internal_test_mcdc_is_retryable
 *
 * @par MC/DC:
 * Decision: `(rc == k_ra8_err_busy) || (rc == k_ra8_err_timeout) || (rc ==
 * k_ra8_fail)` cites apps/stand_alone/media_dl/src/mdl_fetch.c@priv_mdl_fetch_is_retryable.
 * - Vector 1: rc=k_ra8_err_not_found -> false (all three conditions false)
 * - Vector 2: rc=k_ra8_err_busy      -> true  (varies condition 1)
 * - Vector 3: rc=k_ra8_err_timeout   -> true  (varies condition 2)
 * - Vector 4: rc=k_ra8_fail          -> true  (varies condition 3)
 * Vectors 1+2 isolate condition 1, 1+3 condition 2, 1+4 condition 3.
 * N+1 = 4 vectors for N=3 conditions: minimal MC/DC.
 * @brief Exercise the is retryable regression scenario.
 * @details Executes the is retryable scenario through production interfaces and checks its observable success, rejection, and boundary results.
 * @pre Assertions are enabled for contract verification.
 * @pre The test fixture workspace is isolated for this scenario.
 * @post Normal return means every reached contract assertion passed.
 * @post The caller receives no transferred fixture ownership.
 * @note Test helper; an assertion failure terminates the test process.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_mcdc_is_retryable(void)
{
  TEST_BEGIN("is_retryable MC/DC");
  TEST_ASSERT(!priv_mdl_fetch_is_retryable(k_ra8_err_not_found));
  TEST_ASSERT(priv_mdl_fetch_is_retryable(k_ra8_err_busy));
  TEST_ASSERT(priv_mdl_fetch_is_retryable(k_ra8_err_timeout));
  TEST_ASSERT(priv_mdl_fetch_is_retryable(k_ra8_fail));
  /* A success and other permanent errors are never retried. */
  TEST_ASSERT(!priv_mdl_fetch_is_retryable(k_ra8_ok));
  TEST_ASSERT(!priv_mdl_fetch_is_retryable(k_ra8_err_no_mem));
  TEST_END("is_retryable MC/DC");
}

/**
 * @test internal_test_mcdc_run_incomplete
 *
 * @par MC/DC:
 * Decision: `(stats->chapters_failed > 0) || (stats->pages_failed > 0)` cites
 * apps/stand_alone/media_dl/src/mdl_fetch.c@priv_mdl_fetch_run_incomplete.
 * - Vector 1: chapters_failed=0, pages_failed=0 -> false (both false)
 * - Vector 2: chapters_failed=1, pages_failed=0 -> true  (varies chapters)
 * - Vector 3: chapters_failed=0, pages_failed=1 -> true  (varies pages)
 * Vectors 1+2 isolate the chapter condition, 1+3 the page condition.
 * N+1 = 3 vectors for N=2 conditions: minimal MC/DC.
 * @brief Exercise the run incomplete regression scenario.
 * @details Executes the run incomplete scenario through production interfaces and checks its observable success, rejection, and boundary results.
 * @pre Assertions are enabled for contract verification.
 * @pre The test fixture workspace is isolated for this scenario.
 * @post Normal return means every reached contract assertion passed.
 * @post The caller receives no transferred fixture ownership.
 * @note Test helper; an assertion failure terminates the test process.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_mcdc_run_incomplete(void)
{
  TEST_BEGIN("run_incomplete MC/DC");
  mdl_fetch_stats_t clean = {};
  TEST_ASSERT(!priv_mdl_fetch_run_incomplete(&clean));
  mdl_fetch_stats_t ch = {.chapters_failed = 1U};
  TEST_ASSERT(priv_mdl_fetch_run_incomplete(&ch));
  mdl_fetch_stats_t pg = {.pages_failed = 1U};
  TEST_ASSERT(priv_mdl_fetch_run_incomplete(&pg));
  TEST_ASSERT(!priv_mdl_fetch_run_incomplete(nullptr));
  TEST_END("run_incomplete MC/DC");
}

/**
 * @brief Prove a conditional 304 reuses the verified local entity.
 * @details Seeds one page with validators, then verifies the next conditional
 * request preserves its bytes without counting a new transfer.
 * @param[in] abs_dir Abs dir value for this operation.
 * @param[in] state_path Persistent state path.
 * @pre Assertions are enabled for contract verification.
 * @pre The test fixture workspace is isolated for this scenario.
 * @post Normal return means every reached contract assertion passed.
 * @post The caller receives no transferred fixture ownership.
 * @note Test helper; an assertion failure terminates the test process.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_seed_conditional_cache(const char* abs_dir,
                                                         const char* state_path)
{
  mdl_fetch_stats_t first = {};
  TEST_ASSERT_EQ((int64_t)k_ra8_ok,
                 internal_mdl_fetch_test_run(abs_dir,
                                             state_path,
                                             k_mdl_layout_separate,
                                             nullptr,
                                             false,
                                             0U,
                                             &first));
  TEST_ASSERT_EQ((uint16_t)1, (uint16_t)first.pages_fetched);
  TEST_ASSERT(internal_mdl_fetch_test_page_exists(abs_dir, "chapter-1/page_0001.jpg"));
  const mdl_page_rec_t* page = mdl_state_find_page(&s_state, mdl_hash_str("http://cdn/a.jpg"));
  TEST_ASSERT_NOT_NULL(page);
  TEST_ASSERT(strcmp(page->etag, "\"v100\"") == 0);
  TEST_ASSERT(strcmp(page->last_modified, "Wed, 21 Oct 2015 07:28:00 GMT") == 0);
  TEST_ASSERT_EQ((uint16_t)200, page->response_status);
  TEST_ASSERT(page->fetched_at > 0);
  TEST_ASSERT_EQ((int64_t)k_ra8_ok, mdl_state_load(&s_storage, state_path, &s_state));
  mdl_chapter_rec_t* chapter = mdl_state_find_chapter(&s_state, "chapter-1");
  TEST_ASSERT_NOT_NULL(chapter);
  chapter->complete = false;
}

/**
 * @brief Exercise the conditional fetch 304 not modified regression scenario.
 * @details Executes the conditional fetch 304 not modified scenario through production interfaces and checks its observable success, rejection, and boundary results.
 * @pre Assertions are enabled for contract verification.
 * @pre The test fixture workspace is isolated for this scenario.
 * @post Normal return means every reached contract assertion passed.
 * @post The caller receives no transferred fixture ownership.
 * @note Test helper; an assertion failure terminates the test process.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_conditional_fetch_304_not_modified(void)
{
  TEST_BEGIN("conditional fetch 304 not modified");
  internal_mdl_fetch_test_setup_site();
  s_mock.map                = s_map1;
  s_mock.map_n              = sizeof(s_map1) / sizeof(s_map1[0]);
  s_mock.resp_etag          = "\"v100\"";
  s_mock.resp_last_modified = "Wed, 21 Oct 2015 07:28:00 GMT";
  char abs_dir[PATH_MAX];
  char state_path[PATH_MAX];
  internal_mdl_fetch_test_make_series_dir(abs_dir, sizeof(abs_dir), state_path, sizeof(state_path));
  mdl_state_init(&s_state);
  const char* c1[] = {"http://s/chapter-1"};
  internal_mdl_fetch_test_set_chapters(c1, 1U);
  internal_seed_conditional_cache(abs_dir, state_path);
  const mdl_page_rec_t* seeded = mdl_state_find_page(&s_state, mdl_hash_str("http://cdn/a.jpg"));
  TEST_ASSERT_NOT_NULL(seeded);
  const int64_t  seeded_at = seeded->fetched_at;
  const uint64_t retained_hash =
    internal_mdl_fetch_test_page_hash(abs_dir, "chapter-1/page_0001.jpg");
  s_mock.not_mod_on_file_call = 1U; /* Script call 1 of run 2 to 304 */
  mdl_fetch_stats_t s2;
  TEST_ASSERT_EQ((int64_t)k_ra8_ok,
                 internal_mdl_fetch_test_run(abs_dir,
                                             state_path,
                                             k_mdl_layout_separate,
                                             nullptr,
                                             false,
                                             0U,
                                             &s2));
  s_mock.not_mod_on_file_call = 0U;
  s_mock.resp_etag            = nullptr;
  s_mock.resp_last_modified   = nullptr;

  /* 304 is treated as success, counts as reused, retains existing file */
  TEST_ASSERT_EQ((uint16_t)1, (uint16_t)s2.pages_reused);
  TEST_ASSERT_EQ((uint16_t)0, (uint16_t)s2.pages_fetched);
  TEST_ASSERT(internal_mdl_fetch_test_page_exists(abs_dir, "chapter-1/page_0001.jpg"));
  TEST_ASSERT_EQ(retained_hash,
                 internal_mdl_fetch_test_page_hash(abs_dir, "chapter-1/page_0001.jpg"));
  const mdl_page_rec_t* retained = mdl_state_find_page(&s_state, mdl_hash_str("http://cdn/a.jpg"));
  TEST_ASSERT_NOT_NULL(retained);
  TEST_ASSERT_EQ(retained_hash, retained->content_hash);
  TEST_ASSERT(strcmp(retained->etag, "\"v100\"") == 0);
  TEST_ASSERT(strcmp(retained->last_modified, "Wed, 21 Oct 2015 07:28:00 GMT") == 0);
  TEST_ASSERT_EQ((uint16_t)304, retained->response_status);
  TEST_ASSERT(retained->fetched_at >= seeded_at);

  /* Verify conditional headers were sent */
  TEST_ASSERT(strcmp(s_mock.last_if_none_match, "\"v100\"") == 0);
  TEST_ASSERT(strcmp(s_mock.last_if_mod_since, "Wed, 21 Oct 2015 07:28:00 GMT") == 0);
  TEST_END("conditional fetch 304 not modified");
}

/** @test A 200 response at the same URL replaces valid cached bytes atomically.
 * @brief Exercise the conditional fetch 200 replaces changed content regression scenario.
 * @details Executes the conditional fetch 200 replaces changed content scenario through production interfaces and checks its observable success, rejection, and boundary results.
 * @pre Assertions are enabled for contract verification.
 * @pre The test fixture workspace is isolated for this scenario.
 * @post Normal return means every reached contract assertion passed.
 * @post The caller receives no transferred fixture ownership.
 * @note Test helper; an assertion failure terminates the test process.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_conditional_fetch_200_replaces_changed_content(void)
{
  TEST_BEGIN("conditional fetch 200 replaces changed content");
  internal_mdl_fetch_test_setup_site();
  s_mock.map       = s_map1;
  s_mock.map_n     = sizeof(s_map1) / sizeof(s_map1[0]);
  s_mock.resp_etag = "\"v1\"";
  char abs_dir[PATH_MAX];
  char state_path[PATH_MAX];
  internal_mdl_fetch_test_make_series_dir(abs_dir, sizeof(abs_dir), state_path, sizeof(state_path));
  mdl_state_init(&s_state);
  const char* c1[] = {"http://s/chapter-1"};
  internal_mdl_fetch_test_set_chapters(c1, 1U);

  mdl_fetch_stats_t first = {};
  TEST_ASSERT_EQ((int64_t)k_ra8_ok,
                 internal_mdl_fetch_test_run(abs_dir,
                                             state_path,
                                             k_mdl_layout_separate,
                                             nullptr,
                                             false,
                                             0U,
                                             &first));
  const mdl_page_rec_t* old = mdl_state_find_page(&s_state, mdl_hash_str("http://cdn/a.jpg"));
  TEST_ASSERT_NOT_NULL(old);
  const uint64_t old_hash = old->content_hash;

  mdl_chapter_rec_t* chapter = mdl_state_find_chapter(&s_state, "chapter-1");
  TEST_ASSERT_NOT_NULL(chapter);
  chapter->complete        = false;
  s_mock.response_body     = "replacement image bytes";
  s_mock.resp_etag         = "\"v2\"";
  mdl_fetch_stats_t second = {};
  TEST_ASSERT_EQ((int64_t)k_ra8_ok,
                 internal_mdl_fetch_test_run(abs_dir,
                                             state_path,
                                             k_mdl_layout_separate,
                                             nullptr,
                                             false,
                                             0U,
                                             &second));

  const mdl_page_rec_t* changed = mdl_state_find_page(&s_state, mdl_hash_str("http://cdn/a.jpg"));
  TEST_ASSERT_NOT_NULL(changed);
  TEST_ASSERT(changed->content_hash != old_hash);
  TEST_ASSERT(strcmp(changed->etag, "\"v2\"") == 0);
  TEST_ASSERT_EQ((uint16_t)1, (uint16_t)second.pages_fetched);
  TEST_ASSERT_EQ((uint16_t)0, (uint16_t)second.pages_reused);
  TEST_ASSERT(strcmp(s_mock.last_if_none_match, "\"v1\"") == 0);
  TEST_END("conditional fetch 200 replaces changed content");
}

/**
 * @test internal_test_refetch_omits_validators_and_replaces_changed_content
 * @brief Force an unconditional transfer even when the URL validators are unchanged.
 * @details Seeds a verified JPEG with both validators, changes the scripted
 *          bytes at the same URL without changing either validator, and runs
 *          with explicit refetch policy. The request must omit both conditional
 *          headers so the replacement reaches atomic validation and publication.
 * @pre The test process may create files under `/tmp`.
 * @pre Scripted fetch globals are exclusively owned by this test.
 * @post Exactly one unconditional request replaces the page and its content hash.
 * @post The unchanged response validators remain attached to the new state record.
 * @note This is the regression boundary for CLI/RPC refetch policy below composition.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_refetch_omits_validators_and_replaces_changed_content(void)
{
  TEST_BEGIN("refetch omits validators + replaces changed content");
  internal_mdl_fetch_test_setup_site();
  s_mock.map                = s_map1;
  s_mock.map_n              = sizeof(s_map1) / sizeof(s_map1[0]);
  s_mock.resp_etag          = "\"stable\"";
  s_mock.resp_last_modified = "Wed, 21 Oct 2015 07:28:00 GMT";
  char abs_dir[PATH_MAX];
  char state_path[PATH_MAX];
  internal_mdl_fetch_test_make_series_dir(abs_dir, sizeof(abs_dir), state_path, sizeof(state_path));
  mdl_state_init(&s_state);
  const char* chapters[] = {"http://s/chapter-1"};
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
  const uint64_t old_hash = internal_mdl_fetch_test_page_hash(abs_dir, "chapter-1/page_0001.jpg");
  mdl_chapter_rec_t* chapter = mdl_state_find_chapter(&s_state, "chapter-1");
  TEST_ASSERT_NOT_NULL(chapter);
  chapter->complete    = false;
  s_refetch            = true;
  s_mock.response_body = "replacement image bytes with stable validators";

  mdl_fetch_stats_t second = {};
  TEST_ASSERT_EQ((int64_t)k_ra8_ok,
                 internal_mdl_fetch_test_run(abs_dir,
                                             state_path,
                                             k_mdl_layout_separate,
                                             nullptr,
                                             false,
                                             0U,
                                             &second));
  const uint64_t new_hash = internal_mdl_fetch_test_page_hash(abs_dir, "chapter-1/page_0001.jpg");
  TEST_ASSERT(new_hash != old_hash);
  TEST_ASSERT_EQ((int64_t)1, (int64_t)s_mock.get_file_calls);
  TEST_ASSERT_EQ((uint16_t)1, (uint16_t)second.pages_fetched);
  TEST_ASSERT_EQ((uint16_t)0, (uint16_t)second.pages_reused);
  TEST_ASSERT(strcmp(s_mock.last_if_none_match, "") == 0);
  TEST_ASSERT(strcmp(s_mock.last_if_mod_since, "") == 0);
  const mdl_page_rec_t* replaced = mdl_state_find_page(&s_state, mdl_hash_str("http://cdn/a.jpg"));
  TEST_ASSERT_NOT_NULL(replaced);
  TEST_ASSERT_EQ(new_hash, replaced->content_hash);
  TEST_ASSERT(strcmp(replaced->etag, "\"stable\"") == 0);
  TEST_ASSERT(strcmp(replaced->last_modified, "Wed, 21 Oct 2015 07:28:00 GMT") == 0);
  TEST_END("refetch omits validators + replaces changed content");
}

/**
 * @test internal_test_page_magic_required_and_old_destination_preserved
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
RA8_INTERNAL static void internal_test_page_magic_required_and_old_destination_preserved(void)
{
  TEST_BEGIN("page magic required + preserve old destination");
  internal_mdl_fetch_test_setup_site();
  s_mock.map   = s_map1;
  s_mock.map_n = sizeof(s_map1) / sizeof(s_map1[0]);
  char abs_dir[PATH_MAX];
  char state_path[PATH_MAX];
  internal_mdl_fetch_test_make_series_dir(abs_dir, sizeof(abs_dir), state_path, sizeof(state_path));
  mdl_state_init(&s_state);
  const char* chapters[] = {"http://s/chapter-1"};
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
  const uint64_t old_hash = internal_mdl_fetch_test_page_hash(abs_dir, "chapter-1/page_0001.jpg");
  const mdl_page_rec_t* old_record =
    mdl_state_find_page(&s_state, mdl_hash_str("http://cdn/a.jpg"));
  TEST_ASSERT_NOT_NULL(old_record);
  TEST_ASSERT_EQ(old_hash, old_record->content_hash);

  mdl_chapter_rec_t* chapter = mdl_state_find_chapter(&s_state, "chapter-1");
  TEST_ASSERT_NOT_NULL(chapter);
  chapter->complete          = false;
  s_refetch                  = true;
  s_mock.response_prefix     = nullptr;
  s_mock.response_prefix_len = 0U;
  s_mock.response_body       = "this is HTML, not an image";
  s_mock.resp_content_type   = "image/jpeg";
  mdl_fetch_stats_t rejected = {};
  TEST_ASSERT_EQ((int64_t)k_ra8_fail,
                 internal_mdl_fetch_test_run(abs_dir,
                                             state_path,
                                             k_mdl_layout_separate,
                                             nullptr,
                                             false,
                                             0U,
                                             &rejected));
  TEST_ASSERT_EQ((int64_t)1, (int64_t)rejected.pages_failed);
  TEST_ASSERT_EQ(old_hash, internal_mdl_fetch_test_page_hash(abs_dir, "chapter-1/page_0001.jpg"));
  const mdl_page_rec_t* retained = mdl_state_find_page(&s_state, mdl_hash_str("http://cdn/a.jpg"));
  TEST_ASSERT_NOT_NULL(retained);
  TEST_ASSERT_EQ(old_hash, retained->content_hash);
  TEST_ASSERT(strcmp(retained->rel_path, "chapter-1/page_0001.jpg") == 0);
  TEST_END("page magic required + preserve old destination");
}

/**
 * @test internal_test_page_magic_changes_extension_and_rekeys_state
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
RA8_INTERNAL static void internal_test_page_magic_changes_extension_and_rekeys_state(void)
{
  TEST_BEGIN("page magic extension + URL-keyed state");
  internal_mdl_fetch_test_setup_site();
  s_mock.map   = s_map1;
  s_mock.map_n = sizeof(s_map1) / sizeof(s_map1[0]);
  char abs_dir[PATH_MAX];
  char state_path[PATH_MAX];
  internal_mdl_fetch_test_make_series_dir(abs_dir, sizeof(abs_dir), state_path, sizeof(state_path));
  mdl_state_init(&s_state);
  const char* chapters[] = {"http://s/chapter-1"};
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
  const uint32_t old_count = s_state.page_rec_count;
  const uint64_t old_hash  = internal_mdl_fetch_test_page_hash(abs_dir, "chapter-1/page_0001.jpg");
  mdl_chapter_rec_t* chapter = mdl_state_find_chapter(&s_state, "chapter-1");
  TEST_ASSERT_NOT_NULL(chapter);
  chapter->complete          = false;
  s_refetch                  = true;
  s_mock.response_prefix     = s_png_magic;
  s_mock.response_prefix_len = sizeof(s_png_magic);
  s_mock.response_body       = "replacement PNG payload";
  s_mock.resp_content_type   = "image/jpeg";

  mdl_fetch_stats_t second = {};
  TEST_ASSERT_EQ((int64_t)k_ra8_ok,
                 internal_mdl_fetch_test_run(abs_dir,
                                             state_path,
                                             k_mdl_layout_separate,
                                             nullptr,
                                             false,
                                             0U,
                                             &second));
  TEST_ASSERT(!internal_mdl_fetch_test_page_exists(abs_dir, "chapter-1/page_0001.jpg"));
  TEST_ASSERT(internal_mdl_fetch_test_page_exists(abs_dir, "chapter-1/page_0001.png"));
  const uint64_t new_hash = internal_mdl_fetch_test_page_hash(abs_dir, "chapter-1/page_0001.png");
  TEST_ASSERT(new_hash != old_hash);
  TEST_ASSERT_EQ(old_count, s_state.page_rec_count);
  const mdl_page_rec_t* record = mdl_state_find_page(&s_state, mdl_hash_str("http://cdn/a.jpg"));
  TEST_ASSERT_NOT_NULL(record);
  TEST_ASSERT_EQ(new_hash, record->content_hash);
  TEST_ASSERT(strcmp(record->rel_path, "chapter-1/page_0001.png") == 0);
  TEST_END("page magic extension + URL-keyed state");
}

/**
 * @test internal_test_304_without_cached_entity_refetches_unconditionally
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
RA8_INTERNAL static void internal_test_304_without_cached_entity_refetches_unconditionally(void)
{
  TEST_BEGIN("304 missing entity -> unconditional refetch");
  internal_mdl_fetch_test_setup_site();
  s_mock.map                = s_map1;
  s_mock.map_n              = sizeof(s_map1) / sizeof(s_map1[0]);
  s_mock.resp_etag          = "\"v1\"";
  s_mock.resp_last_modified = "Wed, 21 Oct 2015 07:28:00 GMT";
  char abs_dir[PATH_MAX];
  char state_path[PATH_MAX];
  internal_mdl_fetch_test_make_series_dir(abs_dir, sizeof(abs_dir), state_path, sizeof(state_path));
  mdl_state_init(&s_state);
  const char* chapters[] = {"http://s/chapter-1"};
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

  char page[k_join_bytes];
  (void)__builtin_snprintf(page, sizeof(page), "%s/chapter-1/page_0001.jpg", abs_dir);
  TEST_ASSERT_EQ((int64_t)0, (int64_t)unlink(page));
  mdl_chapter_rec_t* chapter = mdl_state_find_chapter(&s_state, "chapter-1");
  TEST_ASSERT_NOT_NULL(chapter);
  chapter->complete           = false;
  s_mock.not_mod_on_file_call = 1U;
  mdl_fetch_stats_t recovered = {};
  TEST_ASSERT_EQ((int64_t)k_ra8_ok,
                 internal_mdl_fetch_test_run(abs_dir,
                                             state_path,
                                             k_mdl_layout_separate,
                                             nullptr,
                                             false,
                                             0U,
                                             &recovered));
  TEST_ASSERT_EQ((int64_t)2, (int64_t)s_mock.get_file_calls);
  TEST_ASSERT_EQ((int64_t)1, (int64_t)recovered.pages_fetched);
  TEST_ASSERT_EQ((int64_t)0, (int64_t)recovered.pages_reused);
  TEST_ASSERT(internal_mdl_fetch_test_page_exists(abs_dir, "chapter-1/page_0001.jpg"));
  TEST_ASSERT(strcmp(s_mock.last_if_none_match, "") == 0);
  TEST_ASSERT(strcmp(s_mock.last_if_mod_since, "") == 0);
  TEST_END("304 missing entity -> unconditional refetch");
}

/**
 * @test internal_test_fetch_asset_policy_atomic_and_nonempty
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
RA8_INTERNAL static void internal_test_fetch_asset_policy_atomic_and_nonempty(void)
{
  TEST_BEGIN("fetch asset policy + atomic + nonempty");
  internal_mdl_fetch_test_setup_site();
  char abs_dir[PATH_MAX];
  char state_path[PATH_MAX];
  internal_mdl_fetch_test_make_series_dir(abs_dir, sizeof(abs_dir), state_path, sizeof(state_path));
  char target[k_join_bytes];
  (void)__builtin_snprintf(target, sizeof(target), "%s/cover.jpg", abs_dir);

  mdl_net_iface_t iface = {.vtable = &s_mock_vtable, .ctx = &s_mock};
  mdl_session_init(&s_sess, &iface, "media_dl/test", nullptr, false);
  memset(&s_faillog, 0, sizeof(s_faillog));
  mdl_fetch_ctx_t ctx        = {.session    = &s_sess,
                                .storage    = &s_storage,
                                .site       = &s_site,
                                .gov        = nullptr,
                                .timeout_ms = (uint32_t)k_req_timeout,
                                .faillog    = &s_faillog};
  s_mock.response_prefix     = nullptr;
  s_mock.response_prefix_len = 0U;
  s_mock.response_body       = "cover bytes";
  mdl_net_resp_t resp        = {};
  size_t         bytes       = 0U;
  TEST_ASSERT_EQ(
    (int64_t)k_ra8_ok,
    mdl_fetch_asset(&ctx, "http://cdn/cover.jpg", target, "http://s/series", &resp, &bytes));
  TEST_ASSERT_EQ((int64_t)strlen("cover bytes"), (int64_t)bytes);
  TEST_ASSERT_EQ((int64_t)200, (int64_t)resp.status);
  TEST_ASSERT(internal_mdl_fetch_test_page_exists(abs_dir, "cover.jpg"));

  static const uint8_t retained_fixture[] = "keep";
  TEST_ASSERT(
    internal_mdl_fetch_test_write_bytes(target, retained_fixture, sizeof(retained_fixture) - 1U));
  s_mock.response_body = "";
  bytes                = 99U;
  TEST_ASSERT_EQ((int64_t)k_ra8_err_invalid_size,
                 mdl_fetch_asset(&ctx, "http://cdn/empty.jpg", target, nullptr, &resp, &bytes));
  TEST_ASSERT_EQ((int64_t)0, (int64_t)bytes);
  char   retained[8]    = {};
  size_t retained_bytes = 0U;
  TEST_ASSERT(internal_mdl_fetch_test_read_bytes(target,
                                                 (uint8_t*)retained,
                                                 sizeof(retained),
                                                 &retained_bytes));
  TEST_ASSERT_EQ((int64_t)(sizeof(retained_fixture) - 1U), (int64_t)retained_bytes);
  TEST_ASSERT(strcmp(retained, "keep") == 0);
  TEST_ASSERT_EQ(
    (int64_t)k_ra8_err_invalid_arg,
    mdl_fetch_asset(&ctx, "http://cdn/cover.jpg", "relative.jpg", nullptr, nullptr, nullptr));
  TEST_END("fetch asset policy + atomic + nonempty");
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
  internal_test_mcdc_is_retryable();
  internal_test_mcdc_run_incomplete();
  internal_test_conditional_fetch_304_not_modified();
  internal_test_conditional_fetch_200_replaces_changed_content();
  internal_test_refetch_omits_validators_and_replaces_changed_content();
  internal_test_page_magic_required_and_old_destination_preserved();
  internal_test_page_magic_changes_extension_and_rekeys_state();
  internal_test_304_without_cached_entity_refetches_unconditionally();
  internal_test_fetch_asset_policy_atomic_and_nonempty();
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_posix_deinit(&s_fs_posix));
  (void)internal_test_output_fd_text(STDERR_FILENO, "[OK  ] test_media_dl_fetch_cache.c\n");
  return 0;
}
