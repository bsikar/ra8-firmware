/**
 * @file test_media_dl_state_metadata.c
 * @brief Pure state metadata and coverage tests.
 * @details Separates metadata validation and coverage rendering from the
 *          storage-backed state migration and publication cases.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "fw_if_fs_posix.h"
#include "mdl_state.h"
#include "mdl_storage.h"
#include "test_media_dl_state_metadata_internal.h"
#include "unity_minimal.h"

/** @brief Process-local state fixture for pure metadata checks. */
static mdl_state_t s_metadata_state;

/** @brief Bounds and expectations of the on-disk codec fixtures. */
typedef enum : uint16_t {
  k_codec_path_bytes = 64,   /**< Temp-path template buffer bytes.        */
  k_codec_work_bytes = 2048, /**< Opaque POSIX file/transaction extent.   */
  k_codec_records    = 1,    /**< Chapters/pages in the accepted fixture. */
  k_codec_pages      = 1,    /**< Page count on the accepted chapter.     */
  k_codec_ready      = 0,    /**< Verified pages on the accepted chapter. */
} mdl_codec_limit_t;

/** @brief Exact values the accepted codec fixtures must reproduce. */
typedef enum : int64_t {
  k_codec_epoch        = 42,     /**< Chapter fetch epoch (decimal field). */
  k_codec_number_v1    = 7,      /**< Legacy integral chapter number.      */
  k_codec_url_hash     = 0x1111, /**< Hexadecimal `1111` url-hash field.   */
  k_codec_content_hash = 0x2222, /**< Hexadecimal `2222` content field.    */
} mdl_codec_expect_t;

/** @brief Fixture file permissions. */
typedef enum : uint16_t {
  k_codec_file_mode = 0x180U, /**< rw------- (octal 0600) fixture file. */
} mdl_codec_mode_t;

/** @brief Maximally aligned opaque filesystem-backend workspace. */
typedef struct {
  alignas(max_align_t) uint8_t bytes[k_codec_work_bytes]; /**< Opaque backend bytes. */
} codec_workspace_t;

/** @brief Backend state owned by the codec filesystem binding. */
static fw_fs_posix_state_t s_codec_fs_posix = {.root_fd = -1};
/** @brief Storage binding injected into every codec load. */
static mdl_storage_t s_codec_storage;
/** @brief Destination state for every codec load. */
static mdl_state_t s_codec_state;
/** @brief Physical path of the single temporary codec fixture file. */
static char s_codec_path[k_codec_path_bytes];

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
 * @brief Bind a POSIX filesystem and a unique temporary codec fixture file.
 * @details Publishes the group-private storage binding the codec vectors load
 *          through and reserves one temporary path for the payload under test.
 * @pre The process may create files under `/tmp`.
 * @pre No other test owns the group-private codec fixtures.
 * @post ::s_codec_storage is a validated binding and ::s_codec_path exists.
 * @post The reserved descriptor is closed before any load runs.
 * @note Host-only helper; an assertion failure terminates the test process.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_codec_fixture_open(void)
{
  /* POSIX-rooted portable filesystem driving the codec fixtures. */
  static fw_fs_t s_codec_fs;
  /* Workspace for the one open codec fixture file. */
  static codec_workspace_t s_codec_file_work;
  /* Workspace for the one staged codec publication. */
  static codec_workspace_t s_codec_txn_work;
  /* Bounded stream scratch shared by every codec load. */
  static uint8_t          s_codec_io[k_mdl_storage_io_bytes];
  const fw_fs_posix_cfg_t cfg = {.root_path = "/", .removable_media = false};
  TEST_ASSERT_EQ((int64_t)k_ra8_ok, fw_fs_posix_init(&s_codec_fs, &s_codec_fs_posix, &cfg));
  TEST_ASSERT_EQ((int64_t)k_ra8_ok,
                 mdl_storage_init(&s_codec_storage,
                                  &s_codec_fs,
                                  s_codec_file_work.bytes,
                                  sizeof(s_codec_file_work.bytes),
                                  s_codec_txn_work.bytes,
                                  sizeof(s_codec_txn_work.bytes),
                                  s_codec_io,
                                  sizeof(s_codec_io)));
  (void)__builtin_snprintf(s_codec_path, sizeof(s_codec_path), "%s", "/tmp/mdl_codec_XXXXXX");
  const int descriptor = mkstemp(s_codec_path);
  TEST_ASSERT(descriptor >= 0);
  TEST_ASSERT_EQ((int64_t)0, (int64_t)close(descriptor));
}

/**
 * @brief Release the codec fixture file, its sibling slot, and the filesystem.
 * @details Removes both physical generations belonging to the logical fixture
 *          path so a later group never inherits a stale recovery generation.
 * @pre ::internal_codec_fixture_open completed for this group.
 * @pre The process owns both physical fixture generations.
 * @post Neither fixture generation remains on disk.
 * @post The POSIX filesystem binding is released.
 * @note Host-only helper; an assertion failure terminates the test process.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_codec_fixture_close(void)
{
  char alternate[k_codec_path_bytes + 8U];
  (void)__builtin_snprintf(alternate, sizeof(alternate), "%s.alt", s_codec_path);
  (void)unlink(s_codec_path);
  (void)unlink(alternate);
  TEST_ASSERT_EQ((int64_t)k_ra8_ok, fw_fs_posix_deinit(&s_codec_fs_posix));
}

/**
 * @brief Replace the codec fixture file with exactly @p payload and load it.
 * @details Writes the complete legacy payload with no envelope, so the loader
 *          selects the legacy text path and streams it through the codec.
 * @param[in] payload Complete NUL-terminated state payload to install.
 * @return The status ::mdl_state_load reported for @p payload.
 * @retval k_ra8_ok The payload parsed and satisfied every state invariant.
 * @retval other The payload was malformed, unsupported, or unreadable.
 * @pre @p payload is non-NULL and NUL-terminated.
 * @pre ::internal_codec_fixture_open completed for this group.
 * @post ::s_codec_state holds the parsed state, or an empty state on failure.
 * @post The fixture file holds exactly @p payload.
 * @note Host-only helper; an assertion failure terminates the test process.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_codec_load(const char* payload)
{
  const int descriptor =
    open(s_codec_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, (mode_t)k_codec_file_mode);
  TEST_ASSERT(descriptor >= 0);
  const size_t length = strlen(payload);
  size_t       offset = 0U;
  while (offset < length) {
    const ssize_t written = write(descriptor, &payload[offset], length - offset);
    if (written > 0) {
      offset += (size_t)written;
    } else if ((written < 0) && (errno == EINTR)) {
      continue;
    } else {
      break;
    }
  }
  TEST_ASSERT(offset == length);
  TEST_ASSERT_EQ((int64_t)0, (int64_t)close(descriptor));
  return mdl_state_load(&s_codec_storage, s_codec_path, &s_codec_state);
}

/**
 * @brief Require one payload to be rejected without partial adoption.
 * @details A damaged or concatenated state file must never leave a partially
 *          trusted library behind, so both record pools are required empty.
 * @param[in] payload Complete NUL-terminated malformed state payload.
 * @pre @p payload is non-NULL and NUL-terminated.
 * @pre ::internal_codec_fixture_open completed for this group.
 * @post The loader reported ::k_ra8_err_invalid_state.
 * @post No chapter and no page record survived the rejected payload.
 * @note Host-only helper; an assertion failure terminates the test process.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_codec_expect_reject(const char* payload)
{
  TEST_ASSERT_EQ((int64_t)k_ra8_err_invalid_state, internal_codec_load(payload));
  TEST_ASSERT_EQ((int64_t)0, (int64_t)s_codec_state.chapter_count);
  TEST_ASSERT_EQ((int64_t)0, (int64_t)s_codec_state.page_rec_count);
}

/**
 * @test internal_test_state_codec_accepts_boundary_records
 *
 * @par MC/DC:
 * Decision: internal_mdl_state_apply_page's field-count guard
 * `(current && (nf != 8)) || (!current && ((nf < 4) || (nf > 6)))`. The legacy
 * (`!current`) half is driven here with N+1 = 3 vectors:
 * - Vector 1: nf = 4 -> false (control: the shortest legacy page is accepted)
 * - Vector 2: nf = 3 -> true  (varies the lower bound; see the reject group)
 * - Vector 3: nf = 7 -> true  (varies the upper bound; see the reject group)
 * @brief Accept the exact records the legacy writers produced.
 * @details Pins three things a migration must not lose: a CRLF-terminated file
 *          written on another host loads with its fields intact (the trailing
 *          CR is stripped, not stored, and storing it would fail field
 *          validation); a legacy four-field page record defaults both cache
 *          validators to empty rather than rejecting; and a decimal chapter
 *          number at exactly the 17-significant-digit cap converts to its
 *          nearest binary64 rather than being refused at the ceiling.
 * @pre The test process may create files under `/tmp`.
 * @pre The group-private codec fixtures are exclusively owned by this test.
 * @post Every accepted payload reproduces its exact persisted field values.
 * @post The state is migrated to the current schema version.
 * @note Host-only test with no network access.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_state_codec_accepts_boundary_records(void)
{
  TEST_BEGIN("state codec accepts boundary records");
  /* A CRLF file plus the shortest legacy page record (no cache validators). */
  TEST_ASSERT_EQ((int64_t)k_ra8_ok,
                 internal_codec_load("V\t1\r\n"
                                     "S\thttps://s/series\r\n"
                                     "C\tchapter-7\t7\t0\t1\t0\t42\thttps://s/chapter-7\r\n"
                                     "P\t1111\t2222\tchapter-7/page_0001.jpg\r\n"));
  TEST_ASSERT_EQ((int64_t)k_mdl_state_version, (int64_t)s_codec_state.version);
  /* The CR is stripped: a stored CR is a record delimiter and never valid. */
  TEST_ASSERT(strcmp(s_codec_state.series_url, "https://s/series") == 0);
  TEST_ASSERT_EQ((int64_t)k_codec_records, (int64_t)s_codec_state.chapter_count);
  const mdl_chapter_rec_t* chapter = mdl_state_find_chapter(&s_codec_state, "chapter-7");
  TEST_ASSERT_NOT_NULL(chapter);
  TEST_ASSERT(chapter->number_known);
  TEST_ASSERT(chapter->number == (double)k_codec_number_v1);
  TEST_ASSERT(!chapter->complete);
  TEST_ASSERT_EQ((int64_t)k_codec_pages, (int64_t)chapter->page_count);
  TEST_ASSERT_EQ((int64_t)k_codec_ready, (int64_t)chapter->pages_done);
  TEST_ASSERT_EQ((int64_t)k_codec_epoch, chapter->fetched_at);
  TEST_ASSERT_EQ((int64_t)k_codec_records, (int64_t)s_codec_state.page_rec_count);
  const mdl_page_rec_t* page = mdl_state_find_page(&s_codec_state, (uint64_t)k_codec_url_hash);
  TEST_ASSERT_NOT_NULL(page);
  TEST_ASSERT_EQ(k_codec_content_hash, (int64_t)page->content_hash);
  TEST_ASSERT(strcmp(page->rel_path, "chapter-7/page_0001.jpg") == 0);
  /* Absent optional columns default to empty, never to unread stack bytes. */
  TEST_ASSERT(page->etag[0] == '\0');
  TEST_ASSERT(page->last_modified[0] == '\0');
  TEST_ASSERT_EQ((int64_t)0, page->fetched_at);
  TEST_ASSERT_EQ((int64_t)0, (int64_t)page->response_status);

  /* Exactly 17 significant digits: the documented cap, not one past it. */
  TEST_ASSERT_EQ((int64_t)k_ra8_ok,
                 internal_codec_load(
                   "V\t2\nC\tc\t1\t1.2345678901234567\t0\t1\t0\t42\thttps://s/c\tSeventeen\n"));
  chapter = mdl_state_find_chapter(&s_codec_state, "c");
  TEST_ASSERT_NOT_NULL(chapter);
  TEST_ASSERT(chapter->number == 1.2345678901234567);
  TEST_ASSERT(strcmp(chapter->title, "Seventeen") == 0);
  /* A well-formed exponent still parses exactly. */
  TEST_ASSERT_EQ((int64_t)k_ra8_ok,
                 internal_codec_load("V\t2\nC\tc\t1\t1.5e1\t0\t1\t0\t42\thttps://s/c\tExponent\n"));
  chapter = mdl_state_find_chapter(&s_codec_state, "c");
  TEST_ASSERT_NOT_NULL(chapter);
  TEST_ASSERT(chapter->number == 15.0);
  TEST_END("state codec accepts boundary records");
}

/**
 * @test internal_test_state_codec_rejects_malformed_records
 *
 * @brief Reject every malformed record shape a damaged state file can carry.
 * @details Each payload is one mutation away from a payload the accepting test
 *          above proves loadable, so a rejection here is attributable to the
 *          guard named beside it: an out-of-order first record, an unknown
 *          record tag, a record used before its schema introduced it, a
 *          truncated or overlong field list, an empty numeric field, a
 *          significand one digit past the cap, a malformed exponent, an epoch
 *          one past the signed 64-bit ceiling, and a duplicate chapter key.
 * @pre The test process may create files under `/tmp`.
 * @pre The group-private codec fixtures are exclusively owned by this test.
 * @post Every payload returns ::k_ra8_err_invalid_state.
 * @post No payload leaves a chapter or page record behind.
 * @note Host-only test with no network access.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_state_codec_rejects_malformed_records(void)
{
  TEST_BEGIN("state codec rejects malformed records");
  static const char* const malformed[] = {
    /* The version record must come first: a payload cannot start mid-stream. */
    "S\thttps://s/series\n",
    /* An unrecognised record type is corruption, not something to skip. */
    "V\t1\nZ\tvalue\n",
    /* Rich metadata did not exist in v1: accepting it would forge history. */
    "V\t1\nD\tsummary\n",
    /* A non-numeric reading direction is not a direction. */
    "V\t2\nR\tx\n",
    /* One column short of a legacy chapter record (no source URL). */
    "V\t1\nC\tchapter-7\t7\t0\t1\t0\t42\n",
    /* One column short of a v2 chapter record (no title). */
    "V\t2\nC\tc\t1\t1\t0\t1\t0\t42\thttps://s/c\n",
    /* An empty url-hash field is truncation, not a zero hash. */
    "V\t1\nP\t\t2222\tchapter-7/page_0001.jpg\n",
    /* One column past the widest legacy page record. */
    "V\t1\nP\t1111\t2222\tchapter-7/page_0001.jpg\te\tm\t7\n",
    /* One column short of the narrowest legacy page record. */
    "V\t1\nP\t1111\t2222\n",
    /* An empty chapter number is not the number zero. */
    "V\t2\nC\tc\t1\t\t0\t1\t0\t42\thttps://s/c\tT\n",
    /* One significant digit past the 17-digit cap the accepting test pins. */
    "V\t2\nC\tc\t1\t1.23456789012345678\t0\t1\t0\t42\thttps://s/c\tT\n",
    /* An exponent marker with no exponent. */
    "V\t2\nC\tc\t1\t1e\t0\t1\t0\t42\thttps://s/c\tT\n",
    /* Trailing bytes after an otherwise valid exponent. */
    "V\t2\nC\tc\t1\t1e1x\t0\t1\t0\t42\thttps://s/c\tT\n",
    /* One past INT64_MAX: an epoch that must not wrap into the past. */
    "V\t2\nC\tc\t1\t1\t0\t1\t0\t9223372036854775808\thttps://s/c\tT\n",
    /* A sign with no magnitude is not an epoch. */
    "V\t2\nC\tc\t1\t1\t0\t1\t0\t-\thttps://s/c\tT\n",
    /* A duplicate chapter key: silently merging would lose one record. */
    "V\t1\nC\tc1\t1\t0\t1\t0\t42\thttps://s/c1\nC\tc1\t2\t0\t1\t0\t42\thttps://s/c1\n",
  };
  for (size_t i = 0U; i < (sizeof(malformed) / sizeof(malformed[0])); ++i) {
    internal_codec_expect_reject(malformed[i]);
  }
  TEST_END("state codec rejects malformed records");
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
  internal_codec_fixture_open();
  internal_test_state_codec_accepts_boundary_records();
  internal_test_state_codec_rejects_malformed_records();
  internal_codec_fixture_close();
}
