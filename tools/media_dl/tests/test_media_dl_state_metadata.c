/**
 * @file test_media_dl_state_metadata.c
 * @brief Pure state metadata and coverage tests.
 * @details Separates metadata validation and coverage rendering from the
 *          storage-backed state migration and publication cases.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#include <math.h>
#include <string.h>

#include "mdl_state.h"
#include "test_media_dl_state_metadata_internal.h"
#include "unity_minimal.h"

/** @brief Process-local state fixture for pure metadata checks. */
static mdl_state_t s_metadata_state;

/**
 * @brief Assert rejected series metadata leaves the prior values unchanged.
 * @details Exercises overlong summaries, record delimiters, path traversal,
 *          and invalid reading directions after valid metadata is installed.
 * @pre The global state fixture holds accepted series metadata.
 * @pre The fixture is exclusively owned by this test process.
 * @post Every rejected tuple leaves the existing metadata unchanged.
 * @post The writer remains the value established by the accepted tuple.
 * @note Pure host helper with no filesystem or network access.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_state_reject_series_metadata(void)
{
  char too_long[k_mdl_summary_max + 1U];
  memset(too_long, 'x', sizeof(too_long));
  too_long[sizeof(too_long) - 1U] = '\0';
  TEST_ASSERT(!mdl_state_set_series_metadata(&s_metadata_state,
                                             too_long,
                                             "changed",
                                             "artist",
                                             "https://s/c.jpg",
                                             "cover/c.jpg",
                                             "en",
                                             k_mdl_state_read_ltr));
  TEST_ASSERT(strcmp(s_metadata_state.writer, "writer") == 0);
  TEST_ASSERT(!mdl_state_set_series_metadata(&s_metadata_state,
                                             "bad\tsummary",
                                             "writer",
                                             "artist",
                                             "https://s/c.jpg",
                                             "cover/c.jpg",
                                             "en",
                                             k_mdl_state_read_ltr));
  TEST_ASSERT(!mdl_state_set_series_metadata(&s_metadata_state,
                                             "summary",
                                             "writer",
                                             "artist",
                                             "https://s/c.jpg",
                                             "../cover.jpg",
                                             "en",
                                             k_mdl_state_read_ltr));
  TEST_ASSERT(!mdl_state_set_series_metadata(&s_metadata_state,
                                             "summary",
                                             "writer",
                                             "artist",
                                             "https://s/c.jpg",
                                             "cover/c.jpg",
                                             "en",
                                             (mdl_state_reading_direction_t)2));
}

/**
 * @test internal_test_state_metadata_setters
 *
 * @brief Verify rich metadata setters enforce all fixed-layout invariants.
 * @details Exercises exact-bound rejection, record-delimiter rejection,
 *          traversal rejection, explicit chapter-zero acceptance, non-finite
 *          rejection, alias-safe replacement, and the all-or-nothing setter
 *          contract.
 * @pre The global state fixture is exclusively owned by this test.
 * @pre C23 finite-double semantics are available.
 * @post Accepted chapter zero remains explicitly known.
 * @post Every rejected tuple leaves the prior metadata unchanged.
 * @note Pure host test with no filesystem or network dependency.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_state_metadata_setters(void)
{
  TEST_BEGIN("state metadata setters");
  mdl_state_init(&s_metadata_state);
  TEST_ASSERT(mdl_state_set_series_metadata(&s_metadata_state,
                                            "summary",
                                            "writer",
                                            "artist",
                                            "https://s/c.jpg",
                                            "cover/c.jpg",
                                            "en",
                                            k_mdl_state_read_ltr));
  TEST_ASSERT(mdl_state_set_series_metadata(&s_metadata_state,
                                            s_metadata_state.summary,
                                            s_metadata_state.writer,
                                            s_metadata_state.artist,
                                            s_metadata_state.cover_url,
                                            "cover/new.jpg",
                                            s_metadata_state.language,
                                            s_metadata_state.reading_direction));
  TEST_ASSERT(strcmp(s_metadata_state.summary, "summary") == 0);
  TEST_ASSERT(strcmp(s_metadata_state.writer, "writer") == 0);
  TEST_ASSERT(strcmp(s_metadata_state.artist, "artist") == 0);
  TEST_ASSERT(strcmp(s_metadata_state.cover_url, "https://s/c.jpg") == 0);
  TEST_ASSERT(strcmp(s_metadata_state.cover_path, "cover/new.jpg") == 0);
  TEST_ASSERT(strcmp(s_metadata_state.language, "en") == 0);
  internal_test_state_reject_series_metadata();

  mdl_chapter_rec_t* chapter = mdl_state_add_chapter_numbered(&s_metadata_state,
                                                              "chapter-0",
                                                              "https://s/chapter-0",
                                                              0.0,
                                                              true);
  TEST_ASSERT_NOT_NULL(chapter);
  TEST_ASSERT(chapter->number_known);
  TEST_ASSERT(mdl_state_set_chapter_metadata(chapter, "Chapter Zero", 0.0, true));
  TEST_ASSERT(!mdl_state_set_chapter_metadata(chapter, "bad\ttitle", 1.0, true));
  TEST_ASSERT(!mdl_state_set_chapter_metadata(chapter, "title", 1.0, false));
  TEST_ASSERT(!mdl_state_set_chapter_metadata(chapter, "title", NAN, true));
  TEST_ASSERT(strcmp(chapter->title, "Chapter Zero") == 0);
  TEST_ASSERT(chapter->number_known);
  TEST_ASSERT(chapter->number == 0.0);
  TEST_END("state metadata setters");
}

/**
 * @brief Run the state metadata validation test group.
 * @pre The unity-minimal assertion process is initialized.
 * @pre The caller owns any process-wide fixture binding used by the group.
 * @post Normal return means every group assertion passed.
 * @post No fixture ownership transfers to the caller.
 * @note Test-target-private and synchronous.
 * @since 0.1.0
 */
RA8_PRIV void priv_test_mdl_state_metadata_run(void)
{
  internal_test_state_metadata_setters();
}
