/**
 * @file test_media_dl_state.c
 * @brief Host unit tests for the #305 library-state store, its content-identity
 *        hashing, and the URL-to-name helpers (pure logic, no network).
 *
 * @details
 * These prove the foundations the resumable/incremental download loop stands
 * on:
 *   - FNV-1a 64 hashing is deterministic, distinguishes different inputs, and a
 *     chunked file hash equals a single-shot buffer hash;
 *   - URL parsing yields a stable, sanitised chapter identifier, the chapter
 *     NUMBER (not a list index), and a sane page extension;
 *   - the state store round-trips through disk, survives as an atomic write,
 *     handles a corrupt or absent file cleanly, and reports coverage/gaps.
 * Uses the repo's `unity_minimal.h` harness, mirroring `tests/test_*.c`.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "mdl_hash.h"
#include "mdl_state.h"
#include "mdl_urlname.h"
#include "unity_minimal.h"

/** @brief Named constants for the state tests (no bare literals). */
typedef enum : uint16_t {
  k_tmp_path_bytes = 64, /**< Temp-path template buffer bytes.       */
  k_name_bytes = 128,    /**< Small name/segment buffer bytes.       */
  k_probe_bytes = 5,     /**< Bytes in the file-hash probe payload.  */
  k_ch_five = 5,         /**< Chapter number used in the basic test. */
  k_ch_one = 1,          /**< Chapter number 1.                      */
  k_ch_two = 2,          /**< Chapter number 2.                      */
  k_ch_four = 4,         /**< Chapter number 4 (leaves a gap at 3).  */
  k_pages_three = 3,     /**< Page count used in the basic test.     */
} mdl_state_test_const_t;

/** @brief Distinct 64-bit markers used as page hashes in the tests. */
typedef enum : uint64_t {
  k_uh_a = 0x1111111111111111ULL, /**< URL hash A.      */
  k_uh_b = 0x2222222222222222ULL, /**< URL hash B.      */
  k_ch_a = 0xAAAAAAAAAAAAAAAAULL, /**< Content hash A.  */
  k_ch_b = 0xBBBBBBBBBBBBBBBBULL, /**< Content hash B.  */
  k_uh_z = 0x9999999999999999ULL, /**< Absent URL hash. */
} mdl_state_test_hash_t;

/** @brief Two large state objects (2 MiB each: off the stack). */
static mdl_state_t s_a;
/** @brief The reload target for round-trip comparisons. */
static mdl_state_t s_b;

/** @brief Create a unique temp file, close it, and return its path in `buf`. */
static void make_tmp(char *buf, size_t cap) {
  (void)snprintf(buf, cap, "%s", "/tmp/mdl_state_test_XXXXXX");
  const int fd = mkstemp(buf);
  if (fd >= 0) {
    (void)close(fd);
  }
}

/** @brief True when a plain file exists at `path`. */
static bool file_exists(const char *path) {
  struct stat st;
  return stat(path, &st) == 0;
}

/** @brief Replace a fixture file with exactly `text`. */
static void write_text(const char *path, const char *text) {
  FILE *fp = fopen(path, "wb");
  TEST_ASSERT_NOT_NULL(fp);
  TEST_ASSERT_EQ((int64_t)strlen(text),
                 (int64_t)fwrite(text, 1U, strlen(text), fp));
  TEST_ASSERT_EQ((int64_t)0, (int64_t)fclose(fp));
}

/**
 * @test test_hash_determinism
 *
 * @par MC/DC:
 * Decision: mdl_hash_bytes_seed's guard `(data == NULL) || (len == 0)` (2
 * conditions, OR; N+1 = 3):
 * - V1: data="abc", len=3 -> false (control: bytes folded, differs from seed)
 * - V2: data=NULL, len=3  -> true  (varies data; returns the seed unchanged)
 * - V3: data="abc", len=0 -> true  (varies len; returns the seed unchanged)
 * Also pins that equal inputs hash equally, different inputs differ, and a
 * chunked fold equals the single-shot hash.
 */
static void test_hash_determinism(void) {
  TEST_BEGIN("hash determinism");
  /* V2/V3: an empty or NULL range returns the seed (here, the offset basis). */
  TEST_ASSERT_EQ((uint64_t)k_mdl_fnv_offset,
                 mdl_hash_bytes(nullptr, k_probe_bytes));
  TEST_ASSERT_EQ((uint64_t)k_mdl_fnv_offset, mdl_hash_bytes("abc", 0U));
  /* V1: equal inputs hash equally; a one-byte change diverges. */
  const uint64_t h_abc = mdl_hash_str("abc");
  TEST_ASSERT_EQ(h_abc, mdl_hash_str("abc"));
  TEST_ASSERT(h_abc != mdl_hash_str("abd"));
  /* A chunked fold reproduces the single-shot digest. */
  uint64_t chained = (uint64_t)k_mdl_fnv_offset;
  chained = mdl_hash_bytes_seed("ab", 2U, chained);
  chained = mdl_hash_bytes_seed("c", 1U, chained);
  TEST_ASSERT_EQ(mdl_hash_bytes("abc", 3U), chained);
  TEST_END("hash determinism");
}

/**
 * @test test_hash_file
 *
 * @par MC/DC:
 * Decision: mdl_hash_file's guard `(path == NULL) || (out == NULL)` (2
 * conditions, OR; N+1 = 3):
 * - V1: path set, out set   -> false (control: file hashed == buffer hash)
 * - V2: path=NULL, out set  -> true  (varies path -> invalid_arg)
 * - V3: path set, out=NULL  -> true  (varies out  -> invalid_arg)
 * Also: a missing file returns k_ra8_fail.
 */
static void test_hash_file(void) {
  TEST_BEGIN("hash file");
  char path[k_tmp_path_bytes];
  make_tmp(path, sizeof(path));
  const char payload[] = "hello";
  FILE *fp = fopen(path, "wb");
  TEST_ASSERT_NOT_NULL(fp);
  (void)fwrite(payload, 1U, strlen(payload), fp);
  (void)fclose(fp);

  uint64_t fh = 0U;
  /* V1 control: the file hash equals the same bytes hashed in memory. */
  TEST_ASSERT_EQ((int64_t)k_ra8_ok, mdl_hash_file(path, &fh));
  TEST_ASSERT_EQ(mdl_hash_bytes(payload, strlen(payload)), fh);
  /* V2/V3: NULL path / out are refused. */
  TEST_ASSERT_EQ((int64_t)k_ra8_err_invalid_arg, mdl_hash_file(nullptr, &fh));
  TEST_ASSERT_EQ((int64_t)k_ra8_err_invalid_arg, mdl_hash_file(path, nullptr));
  (void)unlink(path);
  /* A now-absent file fails cleanly. */
  TEST_ASSERT_EQ((int64_t)k_ra8_fail, mdl_hash_file(path, &fh));
  TEST_END("hash file");
}

/**
 * @test test_urlname
 *
 * @par MC/DC:
 * (No compound decision under test; it pins the three URL-name helpers: the
 * sanitised last segment, explicit decimal chapter number, and the
 * allowlisted lower-cased extension with its jpg default.)
 */
static void test_urlname(void) {
  TEST_BEGIN("urlname helpers");
  char seg[k_name_bytes];
  mdl_urlname_last_segment("http://s/series/chapter-5?x=1", seg, sizeof(seg));
  TEST_ASSERT(strcmp(seg, "chapter-5") == 0);
  mdl_urlname_last_segment("http://s/series/chapter-5/", seg, sizeof(seg));
  TEST_ASSERT(strcmp(seg, "chapter-5") == 0);

  TEST_ASSERT_EQ((int64_t)137,
                 (int64_t)mdl_urlname_chapter_number("http://s/read/ch-137"));
  TEST_ASSERT_EQ((int64_t)0,
                 (int64_t)mdl_urlname_chapter_number("http://s/read/prologue"));
  TEST_ASSERT_EQ((int64_t)5,
                 (int64_t)mdl_urlname_chapter_number("http://12s/ch-5"));
  TEST_ASSERT_EQ((int64_t)108,
                 (int64_t)mdl_urlname_chapter_number(
                     "https://manhwaus.net/webtoon/s/chapter-108-5/"));
  TEST_ASSERT(mdl_urlname_chapter_value(
                  "https://manhwaus.net/webtoon/s/chapter-108-5/") == 108.5);
  TEST_ASSERT(mdl_urlname_chapter_value(
                  "https://12.example/read/chapter-7.25/") == 7.25);
  TEST_ASSERT(mdl_urlname_chapter_value(
                  "https://s.example/read/chapter-7/?next=/chapter-999/") ==
              7.0);
  TEST_ASSERT(mdl_urlname_chapter_value(
                  "https://s.example/read/chapter-8/#chapter-777") == 8.0);
  TEST_ASSERT(
      mdl_urlname_chapter_value(
          "https://12.example/read/prologue?next=chapter-999#chapter-777") ==
      0.0);
  double chapter = -1.0;
  TEST_ASSERT(
      mdl_urlname_chapter_parse("https://s.example/read/chapter-0/", &chapter));
  TEST_ASSERT(chapter == 0.0);
  TEST_ASSERT(mdl_urlname_chapter_parse(
      "https://www.peppercarrot.com/en/webcomic/ep39_The-Tavern.html",
      &chapter));
  TEST_ASSERT(chapter == 39.0);
  TEST_ASSERT(!mdl_urlname_chapter_parse(
      "https://s.example/read/book-2026-991/", &chapter));
  TEST_ASSERT(chapter == 0.0);
  TEST_ASSERT(!mdl_urlname_chapter_parse(
      "https://12.example/read/prologue?next=chapter-999#chapter-777",
      &chapter));
  TEST_ASSERT(!mdl_urlname_chapter_parse(nullptr, &chapter));
  TEST_ASSERT(!mdl_urlname_chapter_parse("https://s/read/chapter-1", nullptr));

  char ext[k_probe_bytes + 3U];
  mdl_urlname_ext("http://cdn/a.PNG", ext, sizeof(ext));
  TEST_ASSERT(strcmp(ext, "png") == 0);
  mdl_urlname_ext("http://cdn/a.jpg?v=2", ext, sizeof(ext));
  TEST_ASSERT(strcmp(ext, "jpg") == 0);
  mdl_urlname_ext("http://cdn/a", ext, sizeof(ext));
  TEST_ASSERT(strcmp(ext, "jpg") == 0);
  mdl_urlname_ext("http://cdn/a.txt", ext, sizeof(ext));
  TEST_ASSERT(strcmp(ext, "jpg") == 0);
  TEST_END("urlname helpers");
}

/**
 * @test test_state_chapters_and_pages
 *
 * @par MC/DC:
 * Decision: mdl_state_chapter_complete's per-chapter match returns
 * `chapters[i].complete` only for the id it matched (find + flag). Vectors: a
 * matched-complete chapter (true), a matched-incomplete chapter (false), and an
 * absent id (false). Also proves add is idempotent and page lookup keys on the
 * URL hash.
 */
static void test_state_chapters_and_pages(void) {
  TEST_BEGIN("state chapters + pages");
  mdl_state_init(&s_a);
  TEST_ASSERT_EQ((uint16_t)0, s_a.chapter_count);

  mdl_chapter_rec_t *c1 =
      mdl_state_add_chapter(&s_a, "c1", "http://s/c1", (long)k_ch_five);
  TEST_ASSERT_NOT_NULL(c1);
  TEST_ASSERT(mdl_state_find_chapter(&s_a, "c1") == c1);
  /* A matched-incomplete chapter is not "complete"; pages unknown. */
  TEST_ASSERT(!mdl_state_chapter_complete(&s_a, "c1"));
  TEST_ASSERT_EQ((uint16_t)0, mdl_state_chapter_pages(&s_a, "c1"));
  /* Adding the same id twice returns the SAME record (idempotent). */
  TEST_ASSERT(
      mdl_state_add_chapter(&s_a, "c1", "http://s/c1", (long)k_ch_five) == c1);
  TEST_ASSERT_EQ((uint16_t)1, s_a.chapter_count);

  c1->complete = true;
  c1->page_count = (uint16_t)k_pages_three;
  TEST_ASSERT(mdl_state_chapter_complete(&s_a, "c1"));
  TEST_ASSERT_EQ((uint16_t)k_pages_three, mdl_state_chapter_pages(&s_a, "c1"));
  /* An absent id is neither complete nor found. */
  TEST_ASSERT(!mdl_state_chapter_complete(&s_a, "nope"));
  TEST_ASSERT_NULL(mdl_state_find_chapter(&s_a, "nope"));

  TEST_ASSERT(mdl_state_add_page(&s_a, (uint64_t)k_uh_a, (uint64_t)k_ch_a,
                                 "c1/page_0001.jpg", nullptr, nullptr));
  const mdl_page_rec_t *p = mdl_state_find_page(&s_a, (uint64_t)k_uh_a);
  TEST_ASSERT_NOT_NULL(p);
  TEST_ASSERT_EQ((uint64_t)k_ch_a, p->content_hash);
  TEST_ASSERT(mdl_state_add_page(&s_a, (uint64_t)k_uh_a, (uint64_t)k_ch_b,
                                 "c2/page_0001.png", "e2", "m2"));
  TEST_ASSERT_EQ((uint32_t)1, s_a.page_rec_count);
  p = mdl_state_find_page(&s_a, (uint64_t)k_uh_a);
  TEST_ASSERT_NOT_NULL(p);
  TEST_ASSERT_EQ((uint64_t)k_ch_b, p->content_hash);
  TEST_ASSERT(strcmp(p->rel_path, "c2/page_0001.png") == 0);
  TEST_ASSERT(strcmp(p->etag, "e2") == 0);
  TEST_ASSERT(strcmp(p->last_modified, "m2") == 0);
  TEST_ASSERT_NULL(mdl_state_find_page(&s_a, (uint64_t)k_uh_z));
  TEST_END("state chapters + pages");
}

/** @brief Populate `s_a` with a two-chapter, two-page fixture. */
static void seed_fixture(void) {
  mdl_state_init(&s_a);
  mdl_state_set_series(&s_a, "http://s/series/foo", "Foo Series", "mysite",
                       "s.example", "sites/mysite.conf");
  mdl_chapter_rec_t *c1 =
      mdl_state_add_chapter(&s_a, "chapter-1", "http://s/c1", (long)k_ch_one);
  c1->complete = true;
  c1->page_count = (uint16_t)k_ch_two;
  c1->pages_done = (uint16_t)k_ch_two;
  mdl_chapter_rec_t *c2 =
      mdl_state_add_chapter(&s_a, "chapter-2", "http://s/c2", (long)k_ch_two);
  c2->complete = false;
  c2->page_count = (uint16_t)k_ch_one;
  TEST_ASSERT(mdl_state_set_series_metadata(
      &s_a, "A summary", "A Writer", "An Artist", "https://s/cover.png",
      "cover/series.png", "en-US", k_mdl_state_read_rtl));
  TEST_ASSERT(mdl_state_set_chapter_metadata(c1, "Chapter 1.5", 1.5, true));
  TEST_ASSERT(mdl_state_set_chapter_metadata(c2, "Chapter Zero", 0.0, true));
  (void)mdl_state_add_page(&s_a, (uint64_t)k_uh_a, (uint64_t)k_ch_a,
                           "chapter-1/page_0001.jpg", "e1", "m1");
  (void)mdl_state_add_page(&s_a, (uint64_t)k_uh_b, (uint64_t)k_ch_b,
                           "chapter-1/page_0002.jpg", "e2", "m2");
}

/**
 * @test test_state_roundtrip_atomic
 *
 * @par MC/DC:
 * (No compound decision under test; it proves a saved state reloads field for
 * field and that the atomic write leaves no `.tmp` sidecar behind.)
 */
static void test_state_roundtrip_atomic(void) {
  TEST_BEGIN("state round-trip + atomic");
  seed_fixture();
  char path[k_tmp_path_bytes];
  make_tmp(path, sizeof(path));
  TEST_ASSERT_EQ((int64_t)k_ra8_ok, mdl_state_save(path, &s_a));
  /* Atomic write: the file exists and no leftover temp sidecar remains. */
  TEST_ASSERT(file_exists(path));
  char tmp[k_tmp_path_bytes + 8U];
  (void)snprintf(tmp, sizeof(tmp), "%s.tmp", path);
  TEST_ASSERT(!file_exists(tmp));

  TEST_ASSERT_EQ((int64_t)k_ra8_ok, mdl_state_load(path, &s_b));
  TEST_ASSERT_EQ(s_a.chapter_count, s_b.chapter_count);
  TEST_ASSERT_EQ(s_a.page_rec_count, s_b.page_rec_count);
  TEST_ASSERT(strcmp(s_b.series_url, "http://s/series/foo") == 0);
  TEST_ASSERT(strcmp(s_b.site_host, "s.example") == 0);
  TEST_ASSERT(strcmp(s_b.config_path, "sites/mysite.conf") == 0);
  TEST_ASSERT(strcmp(s_b.summary, "A summary") == 0);
  TEST_ASSERT(strcmp(s_b.writer, "A Writer") == 0);
  TEST_ASSERT(strcmp(s_b.artist, "An Artist") == 0);
  TEST_ASSERT(strcmp(s_b.cover_url, "https://s/cover.png") == 0);
  TEST_ASSERT(strcmp(s_b.cover_path, "cover/series.png") == 0);
  TEST_ASSERT(strcmp(s_b.language, "en-US") == 0);
  TEST_ASSERT_EQ((int64_t)k_mdl_state_read_rtl, (int64_t)s_b.reading_direction);
  /* Chapter fields survive the round-trip. */
  const mdl_chapter_rec_t *r1 = mdl_state_find_chapter(&s_b, "chapter-1");
  TEST_ASSERT_NOT_NULL(r1);
  TEST_ASSERT(r1->complete);
  TEST_ASSERT_EQ((uint16_t)k_ch_two, r1->page_count);
  TEST_ASSERT(strcmp(r1->title, "Chapter 1.5") == 0);
  TEST_ASSERT(r1->number_known);
  TEST_ASSERT(r1->number == 1.5);
  const mdl_chapter_rec_t *r2 = mdl_state_find_chapter(&s_b, "chapter-2");
  TEST_ASSERT_NOT_NULL(r2);
  TEST_ASSERT(strcmp(r2->title, "Chapter Zero") == 0);
  TEST_ASSERT(r2->number_known);
  TEST_ASSERT(r2->number == 0.0);
  TEST_ASSERT(!mdl_state_chapter_complete(&s_b, "chapter-2"));
  /* Page records survive, keyed by URL hash, with their content hash. */
  const mdl_page_rec_t *pb = mdl_state_find_page(&s_b, (uint64_t)k_uh_b);
  TEST_ASSERT_NOT_NULL(pb);
  TEST_ASSERT_EQ((uint64_t)k_ch_b, pb->content_hash);
  TEST_ASSERT(strcmp(pb->rel_path, "chapter-1/page_0002.jpg") == 0);
  TEST_ASSERT(strcmp(pb->etag, "e2") == 0);
  TEST_ASSERT(strcmp(pb->last_modified, "m2") == 0);
  (void)unlink(path);
  TEST_END("state round-trip + atomic");
}

/**
 * @test test_state_v1_migration
 *
 * @brief Verify schema-v1 state migrates losslessly where representable.
 * @details Verifies the legacy integral record grammar remains readable, v1's
 *          ambiguous zero migrates as unknown, rich metadata defaults safely,
 *          and the next save upgrades the file to schema v2.
 * @pre The test process may create files under `/tmp`.
 * @pre The state fixture storage is exclusively owned by this test.
 * @post The migrated save begins with schema version 2.
 * @post Temporary state storage is removed.
 * @note Host-only test with no network access.
 * @since 0.1.0
 */
static void test_state_v1_migration(void) {
  TEST_BEGIN("state v1 migration");
  char path[k_tmp_path_bytes];
  make_tmp(path, sizeof(path));
  write_text(path, "V\t1\n"
                   "S\thttps://s/series\n"
                   "C\tchapter-7\t7\t1\t1\t1\t42\thttps://s/chapter-7\n"
                   "C\tprologue\t0\t0\t0\t0\t0\thttps://s/prologue\n");
  TEST_ASSERT_EQ((int64_t)k_ra8_ok, mdl_state_load(path, &s_b));
  TEST_ASSERT_EQ((uint16_t)k_mdl_state_version, s_b.version);
  const mdl_chapter_rec_t *numbered = mdl_state_find_chapter(&s_b, "chapter-7");
  const mdl_chapter_rec_t *prologue = mdl_state_find_chapter(&s_b, "prologue");
  TEST_ASSERT_NOT_NULL(numbered);
  TEST_ASSERT_NOT_NULL(prologue);
  TEST_ASSERT(numbered->number_known);
  TEST_ASSERT(numbered->number == 7.0);
  TEST_ASSERT(!prologue->number_known);
  TEST_ASSERT(prologue->number == 0.0);
  TEST_ASSERT(strcmp(numbered->title, "") == 0);
  TEST_ASSERT(strcmp(s_b.summary, "") == 0);
  TEST_ASSERT_EQ((int64_t)k_mdl_state_read_ltr, (int64_t)s_b.reading_direction);

  TEST_ASSERT_EQ((int64_t)k_ra8_ok, mdl_state_save(path, &s_b));
  FILE *fp = fopen(path, "rb");
  TEST_ASSERT_NOT_NULL(fp);
  char line[k_name_bytes];
  TEST_ASSERT_NOT_NULL(fgets(line, (int)sizeof(line), fp));
  TEST_ASSERT_NOT_NULL(fgets(line, (int)sizeof(line), fp));
  TEST_ASSERT(strcmp(line, "V\t2\n") == 0);
  TEST_ASSERT_EQ((int64_t)0, (int64_t)fclose(fp));
  TEST_ASSERT_EQ((int64_t)k_ra8_ok, mdl_state_load(path, &s_a));
  TEST_ASSERT(!mdl_state_find_chapter(&s_a, "prologue")->number_known);
  (void)unlink(path);
  TEST_END("state v1 migration");
}

/**
 * @test test_state_load_absent_and_corrupt
 *
 * @par MC/DC:
 * Decision: mdl_state_load's guard `(path == NULL) || (st == NULL)` (2
 * conditions, OR; N+1 = 3):
 * - V1: path set, st set   -> false (control: absent file -> ok, empty state)
 * - V2: path=NULL, st set  -> true  (varies path -> invalid_arg)
 * - V3: path set, st=NULL  -> true  (varies st   -> invalid_arg)
 * Also: an absent file is ok+empty, a garbage file and a wrong-version file
 * both degrade to invalid_state with an empty, valid state left behind.
 */
static void test_state_load_absent_and_corrupt(void) {
  TEST_BEGIN("state load absent + corrupt");
  char path[k_tmp_path_bytes];
  make_tmp(path, sizeof(path));
  (void)unlink(path); /* make it absent */
  /* V1: an absent file is not an error; state is empty. */
  TEST_ASSERT_EQ((int64_t)k_ra8_ok, mdl_state_load(path, &s_b));
  TEST_ASSERT_EQ((uint16_t)0, s_b.chapter_count);
  /* V2/V3: NULL arguments are refused. */
  TEST_ASSERT_EQ((int64_t)k_ra8_err_invalid_arg, mdl_state_load(nullptr, &s_b));
  TEST_ASSERT_EQ((int64_t)k_ra8_err_invalid_arg, mdl_state_load(path, nullptr));

  /* A garbage file (no version record) is corrupt -> empty, valid state. */
  FILE *fp = fopen(path, "wb");
  TEST_ASSERT_NOT_NULL(fp);
  (void)fprintf(fp, "this is not a state file\n");
  (void)fclose(fp);
  TEST_ASSERT_EQ((int64_t)k_ra8_err_invalid_state, mdl_state_load(path, &s_b));
  TEST_ASSERT_EQ((uint16_t)0, s_b.chapter_count);

  /* A wrong-version file is also refused. */
  fp = fopen(path, "wb");
  TEST_ASSERT_NOT_NULL(fp);
  (void)fprintf(fp, "V\t99\n");
  (void)fclose(fp);
  TEST_ASSERT_EQ((int64_t)k_ra8_err_invalid_state, mdl_state_load(path, &s_b));
  (void)unlink(path);
  TEST_END("state load absent + corrupt");
}

/**
 * @test test_state_rejects_malformed_records
 *
 * @details Pins strict parsing of every numeric field and state invariant. A
 *          damaged or concatenated state file must never be partially trusted.
 */
static void test_state_rejects_malformed_records(void) {
  TEST_BEGIN("state rejects malformed records");
  char path[k_tmp_path_bytes];
  make_tmp(path, sizeof(path));
  static const char *const bad[] = {
      "V\t1junk\n",
      "V\t1\nV\t1\n",
      "V\t1\nC\tc1\t1x\t0\t1\t0\t0\thttps://s/c1\n",
      "V\t1\nC\tc1\t1\t2\t1\t1\t0\thttps://s/c1\n",
      "V\t1\nC\tc1\t1\t0\t65536\t0\t0\thttps://s/c1\n",
      "V\t1\nC\tc1\t1\t0\t1\t2\t0\thttps://s/c1\n",
      "V\t1\nC\tc1\t1\t1\t0\t0\t0\thttps://s/c1\n",
      "V\t1\nP\t111x\t222\tc1/page.jpg\n",
      "V\t1\nP\t10000000000000000\t222\tc1/page.jpg\n",
      "V\t1\nS\tone\textra\n",
      "V\t2\nR\t2\n",
      "V\t2\nK\t../cover.jpg\n",
      "V\t2\nC\tc1\t2\t1\t0\t1\t0\t0\thttps://s/c1\ttitle\n",
      "V\t2\nC\tc1\t0\t1\t0\t1\t0\t0\thttps://s/c1\ttitle\n",
      "V\t2\nC\tc1\t1\tnan\t0\t1\t0\t0\thttps://s/c1\ttitle\n",
      "V\t2\nC\tc1\t1\tinf\t0\t1\t0\t0\thttps://s/c1\ttitle\n",
      "V\t2\nC\tc1\t1\t1e9999\t0\t1\t0\t0\thttps://s/c1\ttitle\n",
      "V\t2\nC\tc1\t1\t1\t0\t1\t0\t0\thttps://s/c1\ttitle\textra\n",
  };
  for (size_t i = 0U; i < (sizeof(bad) / sizeof(bad[0])); ++i) {
    write_text(path, bad[i]);
    TEST_ASSERT_EQ((int64_t)k_ra8_err_invalid_state,
                   mdl_state_load(path, &s_b));
    TEST_ASSERT_EQ((uint16_t)0, s_b.chapter_count);
    TEST_ASSERT_EQ((uint32_t)0, s_b.page_rec_count);
  }

  char overlong[1400];
  memset(overlong, 'a', sizeof(overlong));
  memcpy(overlong, "V\t1\nS\t", 6U);
  overlong[sizeof(overlong) - 2U] = '\n';
  overlong[sizeof(overlong) - 1U] = '\0';
  write_text(path, overlong);
  TEST_ASSERT_EQ((int64_t)k_ra8_err_invalid_state, mdl_state_load(path, &s_b));

  char long_id[k_mdl_chapter_id_max + 1U];
  memset(long_id, 'x', sizeof(long_id));
  long_id[sizeof(long_id) - 1U] = '\0';
  mdl_state_init(&s_a);
  TEST_ASSERT_NULL(mdl_state_add_chapter(&s_a, long_id, "https://s/c", 1L));
  TEST_ASSERT_NULL(mdl_state_add_chapter(&s_a, "bad\tid", "https://s/c", 1L));
  TEST_ASSERT(!mdl_state_add_page(&s_a, 1U, 2U, "bad\tpath", nullptr, nullptr));
  (void)unlink(path);
  TEST_END("state rejects malformed records");
}

/**
 * @test test_state_metadata_setters
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
static void test_state_metadata_setters(void) {
  TEST_BEGIN("state metadata setters");
  mdl_state_init(&s_a);
  TEST_ASSERT(mdl_state_set_series_metadata(&s_a, "summary", "writer", "artist",
                                            "https://s/c.jpg", "cover/c.jpg",
                                            "en", k_mdl_state_read_ltr));
  TEST_ASSERT(mdl_state_set_series_metadata(
      &s_a, s_a.summary, s_a.writer, s_a.artist, s_a.cover_url, "cover/new.jpg",
      s_a.language, s_a.reading_direction));
  TEST_ASSERT(strcmp(s_a.summary, "summary") == 0);
  TEST_ASSERT(strcmp(s_a.writer, "writer") == 0);
  TEST_ASSERT(strcmp(s_a.artist, "artist") == 0);
  TEST_ASSERT(strcmp(s_a.cover_url, "https://s/c.jpg") == 0);
  TEST_ASSERT(strcmp(s_a.cover_path, "cover/new.jpg") == 0);
  TEST_ASSERT(strcmp(s_a.language, "en") == 0);
  char too_long[k_mdl_summary_max + 1U];
  memset(too_long, 'x', sizeof(too_long));
  too_long[sizeof(too_long) - 1U] = '\0';
  TEST_ASSERT(!mdl_state_set_series_metadata(
      &s_a, too_long, "changed", "artist", "https://s/c.jpg", "cover/c.jpg",
      "en", k_mdl_state_read_ltr));
  TEST_ASSERT(strcmp(s_a.writer, "writer") == 0);
  TEST_ASSERT(!mdl_state_set_series_metadata(
      &s_a, "bad\tsummary", "writer", "artist", "https://s/c.jpg",
      "cover/c.jpg", "en", k_mdl_state_read_ltr));
  TEST_ASSERT(!mdl_state_set_series_metadata(
      &s_a, "summary", "writer", "artist", "https://s/c.jpg", "../cover.jpg",
      "en", k_mdl_state_read_ltr));
  TEST_ASSERT(!mdl_state_set_series_metadata(
      &s_a, "summary", "writer", "artist", "https://s/c.jpg", "cover/c.jpg",
      "en", (mdl_state_reading_direction_t)2));

  mdl_chapter_rec_t *chapter = mdl_state_add_chapter_numbered(
      &s_a, "chapter-0", "https://s/chapter-0", 0.0, true);
  TEST_ASSERT_NOT_NULL(chapter);
  TEST_ASSERT(chapter->number_known);
  TEST_ASSERT(
      mdl_state_set_chapter_metadata(chapter, "Chapter Zero", 0.0, true));
  TEST_ASSERT(
      !mdl_state_set_chapter_metadata(chapter, "bad\ttitle", 1.0, true));
  TEST_ASSERT(!mdl_state_set_chapter_metadata(chapter, "title", 1.0, false));
  TEST_ASSERT(!mdl_state_set_chapter_metadata(chapter, "title", NAN, true));
  TEST_ASSERT(strcmp(chapter->title, "Chapter Zero") == 0);
  TEST_ASSERT(chapter->number_known);
  TEST_ASSERT(chapter->number == 0.0);
  TEST_END("state metadata setters");
}

/**
 * @test test_state_save_validates_invariants
 *
 * @brief Verify invalid in-memory state is never serialized.
 * @details Persistence rejects in-memory corruption before creating a temp
 *          file, including page coverage, bounded fields, explicit-number
 *          canonical form, and reading-direction validity.
 * @pre The test process may create files under `/tmp`.
 * @pre The global state fixture is exclusively owned by this test.
 * @post Every malformed state returns ::k_ra8_err_invalid_state.
 * @post Temporary state storage is removed.
 * @note Host-only test with no network access.
 * @since 0.1.0
 */
static void test_state_save_validates_invariants(void) {
  TEST_BEGIN("state save validates invariants");
  char path[k_tmp_path_bytes];
  make_tmp(path, sizeof(path));
  seed_fixture();
  s_a.chapters[0].pages_done = 0U;
  TEST_ASSERT_EQ((int64_t)k_ra8_err_invalid_state, mdl_state_save(path, &s_a));

  seed_fixture();
  (void)snprintf(s_a.chapters[0].source_url, sizeof(s_a.chapters[0].source_url),
                 "bad\turl");
  TEST_ASSERT_EQ((int64_t)k_ra8_err_invalid_state, mdl_state_save(path, &s_a));

  seed_fixture();
  memset(s_a.pages[0].rel_path, 'x', sizeof(s_a.pages[0].rel_path));
  TEST_ASSERT_EQ((int64_t)k_ra8_err_invalid_state, mdl_state_save(path, &s_a));

  seed_fixture();
  s_a.chapters[0].number_known = false;
  TEST_ASSERT_EQ((int64_t)k_ra8_err_invalid_state, mdl_state_save(path, &s_a));

  seed_fixture();
  s_a.reading_direction = (mdl_state_reading_direction_t)2;
  TEST_ASSERT_EQ((int64_t)k_ra8_err_invalid_state, mdl_state_save(path, &s_a));
  (void)unlink(path);
  TEST_END("state save validates invariants");
}

/**
 * @test test_state_coverage
 *
 * @par MC/DC:
 * (No compound decision under test; it checks the coverage line reports the
 * complete-chapter count, the numeric span, and a gap inside that span, plus
 * the empty-library wording.)
 */
static void test_state_coverage(void) {
  TEST_BEGIN("state coverage");
  char cov[k_name_bytes + k_name_bytes];
  mdl_state_init(&s_a);
  mdl_state_coverage(&s_a, cov, sizeof(cov));
  TEST_ASSERT(strstr(cov, "no chapters complete") != nullptr);

  /* Complete chapters 1, 2 and 4 -> span 1..4 with a gap at 3. */
  mdl_chapter_rec_t *a =
      mdl_state_add_chapter(&s_a, "c1", "u1", (long)k_ch_one);
  a->complete = true;
  mdl_chapter_rec_t *b =
      mdl_state_add_chapter(&s_a, "c2", "u2", (long)k_ch_two);
  b->complete = true;
  mdl_chapter_rec_t *d =
      mdl_state_add_chapter(&s_a, "c4", "u4", (long)k_ch_four);
  d->complete = true;
  mdl_state_coverage(&s_a, cov, sizeof(cov));
  TEST_ASSERT(strstr(cov, "3 chapter(s) complete") != nullptr);
  TEST_ASSERT(strstr(cov, "1..4") != nullptr);
  TEST_ASSERT(strstr(cov, "missing 3") != nullptr);
  TEST_END("state coverage");
}

/**
 * @brief Run every state/hash/urlname unit test in sequence.
 * @return 0 when all tests passed; a failing assertion aborts via the harness.
 * @since 0.1.0
 */
int32_t main(void) {
  test_hash_determinism();
  test_hash_file();
  test_urlname();
  test_state_chapters_and_pages();
  test_state_roundtrip_atomic();
  test_state_v1_migration();
  test_state_load_absent_and_corrupt();
  test_state_rejects_malformed_records();
  test_state_metadata_setters();
  test_state_save_validates_invariants();
  test_state_coverage();
  (void)fprintf(stderr, "[OK  ] test_media_dl_state.c\n");
  return 0;
}
