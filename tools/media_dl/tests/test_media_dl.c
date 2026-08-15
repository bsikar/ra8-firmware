/**
 * @file test_media_dl.c
 * @brief Host unit tests for the media_dl pure-logic units + export round-trip.
 *
 * @details
 * Exercises the network-independent surface of the downloader: the format-name
 * mapping, the `<img>`/`<a>` scanner and URL resolver, the site-descriptor
 * parser, and an end-to-end CBZ export re-opened with miniz. Uses the repo's
 * `unity_minimal.h` harness, mirroring `tests/test_*.c`.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "mdl_cli.h"
#include "mdl_config.h"
#include "mdl_export.h"
#include "mdl_export_internal.h"
#include "mdl_extract.h"
#include "mdl_robots.h"
#include "mdl_sanitize.h"
#include "mdl_url_guard.h"
#include "mdl_urlname.h"
#include "mdl_verify.h"
#include "miniz.h"
#include "ra8_jof.h"
#include "tiny_jpeg_fixture.h"
#include "unity_minimal.h"

/** @brief Permission bits for the scratch directories these tests create. */
typedef enum : uint16_t {
  k_mdl_test_dir_mode = 0755U, /**< rwxr-xr-x. */
} mdl_test_mode_t;

/** @brief Expected fixture counts (named to avoid bare literals). */
typedef enum : uint16_t {
  k_expect_imgs       = 2,   /**< /uploads/ images in the page fixture.       */
  k_expect_chaps      = 3,   /**< chapter links in the series fixture.        */
  k_expect_pages      = 2,   /**< pages written into the export fixture.      */
  k_fixture_bytes     = 4,   /**< bytes per synthetic page file.              */
  k_name_probe        = 256, /**< zip entry-name probe buffer.                */
  k_epub_min_entries  = 6,   /**< mimetype+container+opf+nav+pages, at least. */
  k_tar_fixture_bytes = 4096,
  k_tar_member_stride = 1024,
  k_tar_data_offset   = 512,
} test_expect_t;

/** @brief Named sizes/values for the hardening tests (no bare literals). */
typedef enum : uint16_t {
  k_crawl_5s_ms          = 5000, /**< `Crawl-delay: 5` expressed in milliseconds.   */
  k_longname_len         = 120,  /**< Page-name length exceeding the ustar field.   */
  k_join_tiny            = 6,    /**< Buffer too small for a joined path (D4 test). */
  k_buf_128              = 128,  /**< Small host/path/name probe buffer.            */
  k_buf_256              = 256,  /**< Medium probe buffer.                          */
  k_buf_320              = 320,  /**< EPUB zip-entry-path probe buffer.             */
  k_buf_512              = 512,  /**< Large probe buffer.                           */
  k_buf_2k               = 2048, /**< EPUB escaped-name / href probe buffer.        */
  k_buf_4k               = 4096, /**< robots.txt fetch scratch buffer.              */
  k_long_amp_run         = 240,  /**< '&' chars in the long-filename EPUB probe.    */
  k_amp_esc_len          = 5,    /**< strlen("&amp;"), the escape of one '&'.       */
  k_meta_line_test_slack = 16,   /**< Key/delimiter bytes around a path value.      */
} mdl_hard_const_t;

static mdl_url_list_t s_list;
enum { k_test_export_arena_bytes = 8U * 1024U * 1024U };
static uint8_t s_test_export_arena[k_test_export_arena_bytes];

static ra8_err_t mdl_export_chapter(mdl_format_t fmt, const char* chapter_dir, const char* out_path)
{
  mdl_export_workspace_t ws;
  mdl_export_workspace_init(&ws, s_test_export_arena, sizeof(s_test_export_arena));
  return mdl_export_chapter_ws(fmt, chapter_dir, out_path, &ws);
}

static ra8_err_t mdl_export_chapter_meta(mdl_format_t             fmt,
                                         const char*              chapter_dir,
                                         const char*              out_path,
                                         const mdl_export_meta_t* meta)
{
  mdl_export_workspace_t ws;
  mdl_export_workspace_init(&ws, s_test_export_arena, sizeof(s_test_export_arena));
  return mdl_export_chapter_meta_ws(fmt, chapter_dir, out_path, meta, &ws);
}
static ra8_err_t verify_file(mdl_format_t fmt, const char* path, mdl_verify_report_t* report)
{
  mdl_export_workspace_t ws;
  mdl_export_workspace_init(&ws, s_test_export_arena, sizeof(s_test_export_arena));
  return mdl_verify_file(fmt, path, &ws, report);
}

/**
 * @brief Fake robots.txt fetcher state for the cache-consult test.
 * @details Records the call count so a cache hit can be proven not to re-fetch.
 * @invariant `count` counts every fetch dispatched through ::fake_fetch.
 * @since 0.1.0
 */
typedef struct {
  int                       count;  /**< Number of fetches performed.     */
  const char*               body;   /**< Canned robots.txt body, or NULL. */
  mdl_robots_fetch_result_t result; /**< Result the fetcher reports.      */
} fake_fetch_ctx_t;

/** @brief Injected robots.txt fetcher returning a canned body/result. */
static mdl_robots_fetch_result_t
fake_fetch(void* ctx, const char* robots_url, char* buf, size_t cap, size_t* out_len)
{
  fake_fetch_ctx_t* f = (fake_fetch_ctx_t*)ctx;
  (void)robots_url;
  f->count += 1;
  *out_len = 0U;
  if ((f->result == k_mdl_robots_fetch_ok) && (f->body != nullptr)) {
    const int w   = snprintf(buf, cap, "%s", f->body);
    size_t    got = (w < 0) ? 0U : (size_t)w;
    if (got >= cap) {
      got = cap - 1U;
    }
    *out_len = got;
  }
  return f->result;
}

/** @brief Extract a zip entry as a freshly-malloc'd NUL-terminated string. */
static char* zip_entry_str(mz_zip_archive* zr, const char* name)
{
  size_t len = 0U;
  void*  raw = mz_zip_reader_extract_file_to_heap(zr, name, &len, 0);
  if (raw == nullptr) {
    return nullptr;
  }
  char* s = (char*)malloc(len + 1U);
  if (s != nullptr) {
    memcpy(s, raw, len);
    s[len] = '\0';
  }
  mz_free(raw);
  return s;
}

/** @test Format-name mapping is exact and rejects junk. */
static void test_format_mapping(void)
{
  TEST_BEGIN("format mapping");
  TEST_ASSERT(mdl_format_from_str("cbz") == k_mdl_fmt_cbz);
  TEST_ASSERT(mdl_format_from_str("cbt.xz") == k_mdl_fmt_invalid);
  TEST_ASSERT(mdl_format_from_str(nullptr) == k_mdl_fmt_loose);
  TEST_ASSERT(mdl_format_from_str("bogus") == k_mdl_fmt_invalid);
  mdl_format_t inferred = k_mdl_fmt_invalid;
  TEST_ASSERT(mdl_format_from_path("/tmp/book.CBT.GZ", &inferred) == k_ra8_ok);
  TEST_ASSERT(inferred == k_mdl_fmt_cbt_gz);
  TEST_ASSERT(mdl_format_from_path("/tmp/book.cbt.xz", &inferred) == k_ra8_err_not_supported);
  TEST_ASSERT(inferred == k_mdl_fmt_invalid);
  TEST_ASSERT(mdl_format_from_path("/tmp/book.epub", &inferred) == k_ra8_ok);
  TEST_ASSERT(inferred == k_mdl_fmt_epub);
  TEST_ASSERT(mdl_format_from_path("/tmp/book.INCOMPLETE.cbt.gz", &inferred) == k_ra8_ok);
  TEST_ASSERT(inferred == k_mdl_fmt_cbt_gz);
  TEST_ASSERT(mdl_format_from_path("/tmp/book.zip", &inferred) == k_ra8_err_not_supported);
  TEST_ASSERT(inferred == k_mdl_fmt_invalid);
  TEST_ASSERT(strcmp(mdl_format_ext(k_mdl_fmt_cbz), "cbz") == 0);
  TEST_ASSERT(strcmp(mdl_format_ext(k_mdl_fmt_cbt_gz), "cbt.gz") == 0);
  TEST_ASSERT(mdl_format_from_str("epub") == k_mdl_fmt_epub);
  TEST_ASSERT(mdl_format_from_str("jof") == k_mdl_fmt_jof);
  TEST_ASSERT(mdl_format_from_str("rabook") == k_mdl_fmt_invalid);
  TEST_ASSERT(strcmp(mdl_format_ext(k_mdl_fmt_epub), "epub") == 0);
  TEST_ASSERT(strcmp(mdl_format_ext(k_mdl_fmt_jof), "jof") == 0);
  TEST_ASSERT(strcmp(mdl_format_ext(k_mdl_fmt_rabook), "rabook") == 0);
  TEST_ASSERT(mdl_format_is_verifiable(k_mdl_fmt_cbz));
  TEST_ASSERT(mdl_format_is_verifiable(k_mdl_fmt_cbt));
  TEST_ASSERT(mdl_format_is_verifiable(k_mdl_fmt_epub));
  TEST_ASSERT(mdl_format_is_verifiable(k_mdl_fmt_jof));
  TEST_ASSERT(mdl_format_is_verifiable(k_mdl_fmt_cbt_gz));
  TEST_END("format mapping");
}

/** @test Image scanner prefers data-src, resolves URLs, applies the filter. */
static void test_extract_images(void)
{
  TEST_BEGIN("extract images");
  static const char html[] =
    "<div class='read-content'>"
    "<img src='/images/spinner.gif' alt='loading'/>"
    "<img class='loading' data-src='https://cdn.example.net/uploads/1/1.jpg' src='x'/>"
    "<img class='loading' data-src='/uploads/1/2.jpg'/>"
    "</div>";
  const ra8_err_t rc = mdl_extract_images(html,
                                          sizeof(html) - 1U,
                                          "https://example.net/webtoon/x/chapter-1/",
                                          "data-src",
                                          "/uploads/",
                                          &s_list);
  TEST_ASSERT(rc == k_ra8_ok);
  TEST_ASSERT_EQ(k_expect_imgs, s_list.count); /* spinner filtered out */
  TEST_ASSERT(strcmp(s_list.urls[0], "https://cdn.example.net/uploads/1/1.jpg") == 0);
  TEST_ASSERT(strcmp(s_list.urls[1], "https://example.net/uploads/1/2.jpg") == 0);
  TEST_END("extract images");
}

/** @test Anchor scanner keeps only hrefs containing the marker. */
static void test_extract_anchors(void)
{
  TEST_BEGIN("extract anchors");
  static const char html[] = "<a href='/webtoon/x/chapter-1/'>1</a>"
                             "<a href='/about/'>about</a>"
                             "<a href='/webtoon/x/chapter-2/'>2</a>";
  const ra8_err_t   rc     = mdl_extract_anchors(html,
                                                 sizeof(html) - 1U,
                                                 "https://example.net/webtoon/x/",
                                                 "/chapter-",
                                                 &s_list);
  TEST_ASSERT(rc == k_ra8_ok);
  TEST_ASSERT_EQ((uint16_t)2, s_list.count); /* the /about/ link is dropped */
  TEST_ASSERT(strcmp(s_list.urls[0], "https://example.net/webtoon/x/chapter-1/") == 0);
  TEST_END("extract anchors");
}

/** @test A flat key=value descriptor round-trips through the parser. */
static void test_config_load(void)
{
  TEST_BEGIN("config load");
  const char* path = "/tmp/mdl_test_site.conf";
  FILE*       f    = fopen(path, "w");
  TEST_ASSERT_NOT_NULL(f);
  (void)fputs("# comment\n[section-ignored]\nname = T\nhost = t.net\n"
              "chapter_url_contains = /chapter-\nchapter_order = asc\n"
              "page_img_attr = data-src\nimg_delay_min = 111\n",
              f);
  (void)fclose(f);

  mdl_site_t      site;
  const ra8_err_t rc = mdl_config_load(path, &site);
  TEST_ASSERT(rc == k_ra8_ok);
  TEST_ASSERT(strcmp(site.host, "t.net") == 0);
  TEST_ASSERT(strcmp(site.chapter_url_contains, "/chapter-") == 0);
  TEST_ASSERT(site.chapter_order == k_mdl_order_asc);
  TEST_ASSERT_EQ((uint16_t)111, (uint16_t)site.img_delay_min);

  f = fopen(path, "w");
  TEST_ASSERT_NOT_NULL(f);
  (void)fputs("host = t.net\nunknown_typo = accepted\n", f);
  TEST_ASSERT(fclose(f) == 0);
  TEST_ASSERT(mdl_config_load(path, &site) == k_ra8_err_invalid_state);

  f = fopen(path, "w");
  TEST_ASSERT_NOT_NULL(f);
  (void)fputs("host = t.net\nseries_title_selector = meta:\n", f);
  TEST_ASSERT(fclose(f) == 0);
  TEST_ASSERT(mdl_config_load(path, &site) == k_ra8_err_invalid_state);

  f = fopen(path, "w");
  TEST_ASSERT_NOT_NULL(f);
  (void)fputs("host = t.net\nburst = -1\n", f);
  TEST_ASSERT(fclose(f) == 0);
  TEST_ASSERT(mdl_config_load(path, &site) == k_ra8_err_invalid_state);

  f = fopen(path, "w");
  TEST_ASSERT_NOT_NULL(f);
  (void)fputs("host = t.net\nseries_title_selector = css:.title\n", f);
  TEST_ASSERT(fclose(f) == 0);
  TEST_ASSERT(mdl_config_load(path, &site) == k_ra8_err_invalid_state);

  f = fopen(path, "w");
  TEST_ASSERT_NOT_NULL(f);
  char overlong[k_buf_512];
  memset(overlong, 'a', sizeof(overlong) - 1U);
  overlong[sizeof(overlong) - 1U] = '\0';
  (void)fputs("host = t.net\nname = ", f);
  (void)fputs(overlong, f);
  TEST_ASSERT(fclose(f) == 0);
  TEST_ASSERT(mdl_config_load(path, &site) == k_ra8_err_invalid_state);
  (void)unlink(path);
  TEST_END("config load");
}

/** @brief Write `k_fixture_bytes` of `fill` to `path`. */
static void write_fixture(const char* path, char fill)
{
  FILE* f = fopen(path, "wb");
  if (f != nullptr) {
    char buf[k_fixture_bytes];
    memset(buf, fill, sizeof(buf));
    (void)fwrite(buf, 1U, sizeof(buf), f);
    (void)fclose(f);
  }
}

/** @brief Write an exact binary fixture to `path`. */
static void write_binary_fixture(const char* path, const void* data, size_t len)
{
  FILE* f = fopen(path, "wb");
  if (f != nullptr) {
    (void)fwrite(data, 1U, len, f);
    (void)fclose(f);
  }
}

/** @test Export a folder to CBZ, then re-open it with the miniz reader. */
static void test_export_cbz_roundtrip(void)
{
  TEST_BEGIN("export cbz round-trip");
  const char* dir = "/tmp/mdl_test_chap";
  const char* out = "/tmp/mdl_test_chap.cbz";
  (void)mkdir(dir, (mode_t)k_mdl_test_dir_mode);
  write_fixture("/tmp/mdl_test_chap/page_001.jpg", 'a');
  write_fixture("/tmp/mdl_test_chap/page_002.jpg", 'b');

  const ra8_err_t rc = mdl_export_chapter(k_mdl_fmt_cbz, dir, out);
  TEST_ASSERT(rc == k_ra8_ok);
  mdl_verify_report_t verified = {};
  TEST_ASSERT(verify_file(k_mdl_fmt_cbz, out, &verified) == k_ra8_ok);
  TEST_ASSERT(verified.page_count == k_expect_pages);

  mz_zip_archive zr;
  memset(&zr, 0, sizeof(zr));
  TEST_ASSERT(mz_zip_reader_init_file(&zr, out, 0) != MZ_FALSE);
  TEST_ASSERT_EQ(k_expect_pages + 1U, (uint16_t)mz_zip_reader_get_num_files(&zr));
  char name[k_name_probe];
  (void)mz_zip_reader_get_filename(&zr, 0, name, sizeof(name));
  TEST_ASSERT(strcmp(name, "page_001.jpg") == 0);
  (void)mz_zip_reader_get_filename(&zr, 2, name, sizeof(name));
  TEST_ASSERT(strcmp(name, "ComicInfo.xml") == 0);
  char page[k_fixture_bytes];
  TEST_ASSERT(mz_zip_reader_extract_file_to_mem(&zr, "page_001.jpg", page, sizeof(page), 0));
  TEST_ASSERT(page[0] == 'a' && page[k_fixture_bytes - 1U] == 'a');
  (void)mz_zip_reader_end(&zr);

  (void)unlink("/tmp/mdl_test_chap/page_001.jpg");
  (void)unlink("/tmp/mdl_test_chap/page_002.jpg");
  (void)unlink(out);
  (void)rmdir(dir);
  TEST_END("export cbz round-trip");
}
/** @test CBT contains sorted source bytes plus a semantic ComicInfo member. */
static void test_export_cbt_structure(void)
{
  TEST_BEGIN("export cbt structure");
  const char* dir = "/tmp/mdl_cbt_chap";
  const char* out = "/tmp/mdl_cbt_chap.cbt";
  const char* gz  = "/tmp/mdl_cbt_chap.cbt.gz";
  (void)mkdir(dir, (mode_t)k_mdl_test_dir_mode);
  write_fixture("/tmp/mdl_cbt_chap/page_001.jpg", 'a');
  write_fixture("/tmp/mdl_cbt_chap/page_002.jpg", 'b');
  mdl_export_meta_t meta;
  mdl_meta_init(&meta);
  (void)snprintf(meta.language, sizeof(meta.language), "fr");
  TEST_ASSERT(mdl_export_chapter_meta(k_mdl_fmt_cbt, dir, out, &meta) == k_ra8_ok);
  mdl_verify_report_t verified = {};
  TEST_ASSERT(verify_file(k_mdl_fmt_cbt, out, &verified) == k_ra8_ok);
  TEST_ASSERT(verified.page_count == k_expect_pages);
  TEST_ASSERT(mdl_export_chapter_meta(k_mdl_fmt_cbt_gz, dir, gz, &meta) == k_ra8_ok);
  verified = (mdl_verify_report_t){};
  TEST_ASSERT(verify_file(k_mdl_fmt_cbt_gz, gz, &verified) == k_ra8_ok);
  TEST_ASSERT(verified.page_count == k_expect_pages);

  uint8_t tar[k_tar_fixture_bytes];
  FILE*   f = fopen(out, "rb");
  TEST_ASSERT_NOT_NULL(f);
  TEST_ASSERT(fread(tar, 1U, sizeof(tar), f) == sizeof(tar));
  TEST_ASSERT(fgetc(f) == EOF);
  (void)fclose(f);
  TEST_ASSERT(strcmp((const char*)&tar[0], "page_001.jpg") == 0);
  TEST_ASSERT(tar[k_tar_data_offset] == (uint8_t)'a');
  TEST_ASSERT(strcmp((const char*)&tar[k_tar_member_stride], "page_002.jpg") == 0);
  TEST_ASSERT(tar[k_tar_member_stride + k_tar_data_offset] == (uint8_t)'b');
  TEST_ASSERT(strcmp((const char*)&tar[2U * k_tar_member_stride], "ComicInfo.xml") == 0);
  const char* xml = (const char*)&tar[(2U * k_tar_member_stride) + k_tar_data_offset];
  TEST_ASSERT(strstr(xml, "<PageCount>2</PageCount>") != nullptr);
  TEST_ASSERT(strstr(xml, "<LanguageISO>fr</LanguageISO>") != nullptr);

  (void)unlink("/tmp/mdl_cbt_chap/page_001.jpg");
  (void)unlink("/tmp/mdl_cbt_chap/page_002.jpg");
  (void)unlink(out);
  (void)rmdir(dir);
  (void)unlink(gz);
  TEST_END("export cbt structure");
}

/** @test Packaging ingests ONLY page images; sibling output/junk is skipped. */
static void test_export_skips_non_images(void)
{
  TEST_BEGIN("export skips non-images");
  const char* dir = "/tmp/mdl_mixed_chap";
  const char* out = "/tmp/mdl_mixed_chap.cbz";
  (void)mkdir(dir, (mode_t)k_mdl_test_dir_mode);
  /* Two real pages... */
  write_fixture("/tmp/mdl_mixed_chap/page_001.jpg", 'a');
  write_fixture("/tmp/mdl_mixed_chap/page_002.PNG", 'b'); /* upper-case ext too */
  /* ...amid this tool's own prior output + OS junk, which must be ignored so a
   * re-packaged folder does not fold a non-image "page" into the archive (the
   * 0x107 the reader hits when it tries to decode one). */
  write_fixture("/tmp/mdl_mixed_chap/page_001.jof", 'r');
  write_fixture("/tmp/mdl_mixed_chap/mdl_mixed_chap.cbz", 'z');
  write_fixture("/tmp/mdl_mixed_chap/notes.txt", 't');
  write_fixture("/tmp/mdl_mixed_chap/.DS_Store", 'd');

  TEST_ASSERT(mdl_export_chapter(k_mdl_fmt_cbz, dir, out) == k_ra8_ok);
  mz_zip_archive zr;
  memset(&zr, 0, sizeof(zr));
  TEST_ASSERT(mz_zip_reader_init_file(&zr, out, 0) != MZ_FALSE);
  TEST_ASSERT_EQ(k_expect_pages + 1U, (uint16_t)mz_zip_reader_get_num_files(&zr));
  char name[k_name_probe];
  (void)mz_zip_reader_get_filename(&zr, 0, name, sizeof(name));
  TEST_ASSERT(strcmp(name, "page_001.jpg") == 0);
  (void)mz_zip_reader_get_filename(&zr, 1, name, sizeof(name));
  TEST_ASSERT(strcmp(name, "page_002.PNG") == 0);
  (void)mz_zip_reader_end(&zr);

  (void)unlink("/tmp/mdl_mixed_chap/page_001.jpg");
  (void)unlink("/tmp/mdl_mixed_chap/page_002.PNG");
  (void)unlink("/tmp/mdl_mixed_chap/page_001.jof");
  (void)unlink("/tmp/mdl_mixed_chap/mdl_mixed_chap.cbz");
  (void)unlink("/tmp/mdl_mixed_chap/notes.txt");
  (void)unlink("/tmp/mdl_mixed_chap/.DS_Store");
  (void)unlink(out);
  (void)rmdir(dir);
  TEST_END("export skips non-images");
}

/** @brief Write `len` raw bytes to `path`. */
static void write_bytes(const char* path, const uint8_t* data, size_t len)
{
  FILE* f = fopen(path, "wb");
  if (f != nullptr) {
    (void)fwrite(data, 1U, len, f);
    (void)fclose(f);
  }
}

/** @brief Read a little-endian u16 from `p`. */
static uint16_t rd_u16(const uint8_t* p)
{
  return (uint16_t)((uint16_t)p[0] | (uint16_t)((uint16_t)p[1] << 8U));
}

/** @test Export a folder to EPUB; re-open it and assert the OCF layout. */
static void test_export_epub_roundtrip(void)
{
  TEST_BEGIN("export epub round-trip");
  const char* dir = "/tmp/mdl_epub_chap";
  const char* out = "/tmp/mdl_epub_chap.epub";
  (void)mkdir(dir, (mode_t)k_mdl_test_dir_mode);
  write_fixture("/tmp/mdl_epub_chap/page_001.jpg", 'a');
  write_fixture("/tmp/mdl_epub_chap/page_002.jpg", 'b');
  TEST_ASSERT(mdl_export_chapter(k_mdl_fmt_epub, dir, out) == k_ra8_ok);
  mdl_verify_report_t verified = {};
  TEST_ASSERT(verify_file(k_mdl_fmt_epub, out, &verified) == k_ra8_ok);
  TEST_ASSERT(verified.page_count == k_expect_pages);

  mz_zip_archive zr;
  memset(&zr, 0, sizeof(zr));
  TEST_ASSERT(mz_zip_reader_init_file(&zr, out, 0) != MZ_FALSE);
  char name[k_name_probe];
  (void)mz_zip_reader_get_filename(&zr, 0, name, sizeof(name));
  TEST_ASSERT(strcmp(name, "mimetype") == 0); /* OCF: mimetype is first */
  TEST_ASSERT(mz_zip_reader_get_num_files(&zr) >= (mz_uint)k_epub_min_entries);
  (void)mz_zip_reader_end(&zr);

  (void)unlink("/tmp/mdl_epub_chap/page_001.jpg");
  (void)unlink("/tmp/mdl_epub_chap/page_002.jpg");
  (void)unlink(out);
  (void)rmdir(dir);
  TEST_END("export epub round-trip");
}

/** @test Transcode a real JPEG to JOF; assert magics + webtoon column geometry. */
static void test_export_jof_roundtrip(void)
{
  TEST_BEGIN("export jof round-trip");
  const char* dir = "/tmp/mdl_jof_chap";
  const char* jpg = "/tmp/mdl_jof_chap/page_001.jpg";
  const char* jof = "/tmp/mdl_jof_chap/page_001.jof";
  (void)mkdir(dir, (mode_t)k_mdl_test_dir_mode);
  write_bytes(jpg, k_tiny_jpeg, (size_t)k_tiny_jpeg_len);
  /* JOF is a directory-output format: it writes a `.jof` sibling per page into
   * the chapter dir and does not use out_path (no single container exists), so
   * the caller must report the directory, never a phantom archive file. */
  TEST_ASSERT(mdl_format_is_dir_output(k_mdl_fmt_jof));
  TEST_ASSERT(!mdl_format_is_dir_output(k_mdl_fmt_cbz));
  TEST_ASSERT(!mdl_format_is_dir_output(k_mdl_fmt_epub));
  TEST_ASSERT(mdl_export_chapter(k_mdl_fmt_jof, dir, dir) == k_ra8_ok);
  mdl_verify_report_t verified = {};
  TEST_ASSERT(verify_file(k_mdl_fmt_jof, jof, &verified) == k_ra8_ok);
  TEST_ASSERT(verified.page_count == 1U);

  /* The reported output -- the `.jof` sibling -- actually exists on disk. */
  FILE* f = fopen(jof, "rb");
  TEST_ASSERT_NOT_NULL(f);
  (void)fseek(f, 0, SEEK_END);
  const long sz = ftell(f);
  (void)fseek(f, 0, SEEK_SET);
  TEST_ASSERT(sz > (long)k_ra8_jof_hdr_bytes);
  uint8_t* buf = (uint8_t*)malloc((size_t)sz);
  TEST_ASSERT_NOT_NULL(buf);
  TEST_ASSERT(fread(buf, 1U, (size_t)sz, f) == (size_t)sz);
  (void)fclose(f);

  const size_t mlen = (size_t)k_ra8_jof_magic_len;
  TEST_ASSERT(memcmp(buf + k_ra8_jof_ofs_magic, "JOF1", mlen) == 0);
  TEST_ASSERT(memcmp(buf + ((size_t)sz - mlen), "JOFE", mlen) == 0);
  /* webtoon-native: a single full-width tile column (tile_w == width). */
  TEST_ASSERT_EQ(rd_u16(buf + k_ra8_jof_ofs_width), rd_u16(buf + k_ra8_jof_ofs_tile_w));
  free(buf);

  (void)unlink(jpg);
  (void)unlink(jof);
  (void)rmdir(dir);
  TEST_END("export jof round-trip");
}

/* ---- #299: libcurl-backend safety predicates (mdl_url_guard) ------------- */

/** @test Scheme allowlist accepts http/https and refuses everything else. */
static void test_url_scheme(void)
{
  TEST_BEGIN("url scheme allowlist");
  TEST_ASSERT(mdl_url_scheme_allowed("http://example.net/a"));
  TEST_ASSERT(mdl_url_scheme_allowed("HTTPS://Example.net/a")); /* case-insensitive */
  TEST_ASSERT(!mdl_url_scheme_allowed("file:///etc/passwd"));
  TEST_ASSERT(!mdl_url_scheme_allowed("ftp://h/x"));
  TEST_ASSERT(!mdl_url_scheme_allowed("gopher://h/"));
  TEST_ASSERT(!mdl_url_scheme_allowed("data:text/html,x"));
  TEST_ASSERT(!mdl_url_scheme_allowed(""));
  TEST_ASSERT(!mdl_url_scheme_allowed(nullptr));
  TEST_END("url scheme allowlist");
}

/** @test Address classify buckets loopback/private/link-local vs public. */
static void test_addr_classify(void)
{
  TEST_BEGIN("address classify");
  TEST_ASSERT(mdl_classify_ip("8.8.8.8") == k_mdl_addr_public);
  TEST_ASSERT(mdl_classify_ip("2606:4700:4700::1111") == k_mdl_addr_public);
  TEST_ASSERT(mdl_classify_ip("172.15.0.1") == k_mdl_addr_public);  /* below the /12  */
  TEST_ASSERT(mdl_classify_ip("172.32.0.1") == k_mdl_addr_public);  /* above the /12  */
  TEST_ASSERT(mdl_classify_ip("169.253.0.1") == k_mdl_addr_public); /* not link-local */
  TEST_ASSERT(mdl_classify_ip("127.0.0.1") == k_mdl_addr_loopback);
  TEST_ASSERT(mdl_classify_ip("::1") == k_mdl_addr_loopback);
  TEST_ASSERT(mdl_classify_ip("::ffff:127.0.0.1") == k_mdl_addr_loopback);
  TEST_ASSERT(mdl_classify_ip("10.1.2.3") == k_mdl_addr_private);
  TEST_ASSERT(mdl_classify_ip("192.168.1.1") == k_mdl_addr_private);
  TEST_ASSERT(mdl_classify_ip("172.16.0.1") == k_mdl_addr_private);
  TEST_ASSERT(mdl_classify_ip("172.31.255.1") == k_mdl_addr_private);
  TEST_ASSERT(mdl_classify_ip("100.64.0.1") == k_mdl_addr_private); /* CGNAT */
  TEST_ASSERT(mdl_classify_ip("fc00::1") == k_mdl_addr_private);
  TEST_ASSERT(mdl_classify_ip("169.254.1.2") == k_mdl_addr_linklocal);
  TEST_ASSERT(mdl_classify_ip("fe80::1") == k_mdl_addr_linklocal);
  TEST_ASSERT(mdl_classify_ip("0.0.0.0") == k_mdl_addr_unknown);
  TEST_ASSERT(mdl_classify_ip("nonsense") == k_mdl_addr_unknown);
  TEST_ASSERT(mdl_classify_ip(nullptr) == k_mdl_addr_unknown);
  TEST_END("address classify");
}

/** @test Fetchability honours the private-space opt-in but never unknown. */
static void test_addr_fetchable(void)
{
  TEST_BEGIN("address fetchable");
  TEST_ASSERT(mdl_addr_is_fetchable(k_mdl_addr_public, false));
  TEST_ASSERT(!mdl_addr_is_fetchable(k_mdl_addr_loopback, false));
  TEST_ASSERT(mdl_addr_is_fetchable(k_mdl_addr_loopback, true));
  TEST_ASSERT(!mdl_addr_is_fetchable(k_mdl_addr_private, false));
  TEST_ASSERT(mdl_addr_is_fetchable(k_mdl_addr_private, true));
  TEST_ASSERT(!mdl_addr_is_fetchable(k_mdl_addr_linklocal, false));
  TEST_ASSERT(!mdl_addr_is_fetchable(k_mdl_addr_unknown, true)); /* never fetchable */
  TEST_END("address fetchable");
}

/** @test Response-size cap is overflow-safe and honours 0 = unlimited. */
static void test_size_cap(void)
{
  TEST_BEGIN("size cap");
  TEST_ASSERT(!mdl_size_exceeds(0U, 100U, 0U));   /* cap 0 -> unlimited */
  TEST_ASSERT(!mdl_size_exceeds(90U, 10U, 100U)); /* exactly fits       */
  TEST_ASSERT(mdl_size_exceeds(90U, 11U, 100U));  /* one byte over      */
  TEST_ASSERT(mdl_size_exceeds(200U, 1U, 100U));  /* already over cap   */
  TEST_END("size cap");
}

/** @test Host/path extraction strips scheme, userinfo, port, and query. */
static void test_url_parts(void)
{
  TEST_BEGIN("url parts");
  char h[k_buf_128];
  TEST_ASSERT(mdl_url_host("https://user:pw@Host.EXAMPLE.net:8443/p?q", h, sizeof(h)));
  TEST_ASSERT(strcmp(h, "host.example.net:8443") == 0); /* userinfo dropped, port kept */
  TEST_ASSERT(mdl_url_host("https://Plain.Example.net/x", h, sizeof(h)));
  TEST_ASSERT(strcmp(h, "plain.example.net") == 0);
  TEST_ASSERT(!mdl_url_host("notaurl", h, sizeof(h)));
  char p[k_buf_128];
  TEST_ASSERT(mdl_url_path("https://h.net/a/b?x=1#f", p, sizeof(p)));
  TEST_ASSERT(strcmp(p, "/a/b") == 0);
  TEST_ASSERT(mdl_url_path("https://h.net", p, sizeof(p)));
  TEST_ASSERT(strcmp(p, "/") == 0);
  TEST_END("url parts");
}

/* ---- #300: untrusted-name sanitisers (mdl_sanitize) --------------------- */

/** @test Segment sanitiser neutralises traversal but preserves a legal slug. */
static void test_sanitize_segment(void)
{
  TEST_BEGIN("sanitize segment");
  char out[k_buf_128];
  TEST_ASSERT(!mdl_sanitize_segment("..", out, sizeof(out)));
  TEST_ASSERT(strcmp(out, "..") != 0);
  TEST_ASSERT(out[0] != '\0');
  (void)mdl_sanitize_segment(".", out, sizeof(out));
  TEST_ASSERT(strcmp(out, ".") != 0);
  (void)mdl_sanitize_segment("", out, sizeof(out));
  TEST_ASSERT(out[0] != '\0');
  TEST_ASSERT(!mdl_sanitize_segment("a/b\tc", out, sizeof(out))); /* slash/control */
  TEST_ASSERT(strchr(out, '/') == nullptr);
  TEST_ASSERT(strchr(out, '\t') == nullptr);
  TEST_ASSERT(!mdl_sanitize_segment("CON", out, sizeof(out))); /* reserved name */
  TEST_ASSERT(out[0] == '_');
  TEST_ASSERT(!mdl_sanitize_segment("com1.txt", out, sizeof(out)));
  TEST_ASSERT(out[0] == '_');
  /* legal-but-tricky ASCII slug is preserved verbatim */
  TEST_ASSERT(mdl_sanitize_segment("Re_Zero-kara.Hajimeru_v2", out, sizeof(out)));
  TEST_ASSERT(strcmp(out, "Re_Zero-kara.Hajimeru_v2") == 0);
  TEST_END("sanitize segment");
}

/** @test Path containment respects directory boundaries. */
static void test_path_contained(void)
{
  TEST_BEGIN("path contained");
  TEST_ASSERT(mdl_path_contained("/a/b", "/a/b"));
  TEST_ASSERT(mdl_path_contained("/a/b", "/a/b/c"));
  TEST_ASSERT(mdl_path_contained("/a/b/", "/a/b/c")); /* trailing slash ignored */
  TEST_ASSERT(!mdl_path_contained("/a/b", "/a/bb"));  /* boundary not a prefix  */
  TEST_ASSERT(!mdl_path_contained("/a/b", "/a/c"));
  TEST_ASSERT(!mdl_path_contained("/a/b", "/x"));
  TEST_END("path contained");
}

/**
 * @test Path join composes a safe child but refuses every traversal shape.
 *
 * @par MC/DC:
 * Decision A `if (out == nullptr || cap == 0U)` (2 conditions)
 * - out=buf,  cap>0  -> false (control: a normal join proceeds)
 * - out=NULL, cap>0  -> true  (varies out)
 * - out=buf,  cap=0  -> true  (varies cap)
 * Decision B `if (parent == nullptr || seg == nullptr)` (2 conditions)
 * - parent="/base", seg="c"    -> false (control)
 * - parent=NULL,    seg="c"    -> true  (varies parent)
 * - parent="/base", seg=NULL   -> true  (varies seg)
 * Decision C `if (is_dot_segment(seg) || has_separator(seg))` (2 conditions)
 * - seg="chap-1" -> false (control: dot=F, sep=F, join succeeds)
 * - seg=".."     -> true  (varies dot: dot=T, sep=F)
 * - seg="a/b"    -> true  (varies sep: dot=F, sep=T)
 * Decision D `if (need > cap)` (1 condition)
 * - result fits   -> false (control)
 * - cap too small -> true  (truncation refused, not composed)
 * Each control + single-varied-condition pair proves that condition's
 * independent influence; N+1 vectors per decision, minimal MC/DC.
 */
static void test_path_join(void)
{
  TEST_BEGIN("path join rejects traversal");
  char out[k_buf_128];
  /* Controls for decisions A/B/C/D: a legal segment joins verbatim. */
  TEST_ASSERT(mdl_path_join("/base", "chap-1", out, sizeof(out)));
  TEST_ASSERT(strcmp(out, "/base/chap-1") == 0);
  /* Decision C: `.`/`..`/empty, a `/`-bearing, and an absolute segment fail. */
  TEST_ASSERT(!mdl_path_join("/base", "..", out, sizeof(out)));
  TEST_ASSERT(out[0] == '\0'); /* no usable partial path on refusal */
  TEST_ASSERT(!mdl_path_join("/base", ".", out, sizeof(out)));
  TEST_ASSERT(!mdl_path_join("/base", "", out, sizeof(out)));
  TEST_ASSERT(!mdl_path_join("/base", "a/b", out, sizeof(out)));  /* separator */
  TEST_ASSERT(!mdl_path_join("/base", "/etc", out, sizeof(out))); /* absolute  */
  /* Decision B: a NULL parent or segment fails. */
  TEST_ASSERT(!mdl_path_join(nullptr, "c", out, sizeof(out)));
  TEST_ASSERT(!mdl_path_join("/base", nullptr, out, sizeof(out)));
  /* Decision A: a NULL destination or zero capacity fails. */
  TEST_ASSERT(!mdl_path_join("/base", "c", nullptr, sizeof(out)));
  TEST_ASSERT(!mdl_path_join("/base", "c", out, 0U));
  /* Decision D: a result that would not fit is refused, never truncated. */
  char tiny[k_join_tiny];
  TEST_ASSERT(!mdl_path_join("/base", "toolong", tiny, sizeof(tiny)));
  TEST_ASSERT(tiny[0] == '\0');
  TEST_END("path join rejects traversal");
}

/** @test XML escaper replaces metacharacters and fails rather than truncating. */
static void test_xml_escape(void)
{
  TEST_BEGIN("xml escape");
  char out[k_buf_256];
  TEST_ASSERT(mdl_xml_escape("a&b<c>\"'", out, sizeof(out)));
  TEST_ASSERT(strcmp(out, "a&amp;b&lt;c&gt;&quot;&apos;") == 0);
  TEST_ASSERT(mdl_xml_escape("page_001.jpg", out, sizeof(out))); /* legal name kept */
  TEST_ASSERT(strcmp(out, "page_001.jpg") == 0);
  char tiny[4];
  TEST_ASSERT(!mdl_xml_escape("&&&", tiny, sizeof(tiny))); /* would not fit -> fail */
  TEST_END("xml escape");
}

/** @test A ustar name that will not fit is rejected, a normal one packages. */
static void test_tar_rejects_long_name(void)
{
  TEST_BEGIN("tar rejects over-long name");
  const char* dir = "/tmp/mdl_long_chap";
  const char* out = "/tmp/mdl_long_chap.cbt";
  (void)mkdir(dir, (mode_t)k_mdl_test_dir_mode);
  char name[k_buf_256];
  memset(name, 'p', (size_t)k_longname_len);
  (void)snprintf(name + k_longname_len, sizeof(name) - (size_t)k_longname_len, ".jpg");
  char path[k_buf_320];
  (void)snprintf(path, sizeof(path), "%s/%s", dir, name);
  write_fixture(path, 'x');
  TEST_ASSERT(mdl_export_chapter(k_mdl_fmt_cbt, dir, out) == k_ra8_err_invalid_size);
  (void)unlink(path);
  (void)unlink(out);
  (void)rmdir(dir);

  const char* okdir = "/tmp/mdl_okname_chap";
  const char* okout = "/tmp/mdl_okname_chap.cbt";
  (void)mkdir(okdir, (mode_t)k_mdl_test_dir_mode);
  write_fixture("/tmp/mdl_okname_chap/page_001.jpg", 'x');
  TEST_ASSERT(mdl_export_chapter(k_mdl_fmt_cbt, okdir, okout) == k_ra8_ok);
  (void)unlink("/tmp/mdl_okname_chap/page_001.jpg");
  (void)unlink(okout);
  (void)rmdir(okdir);
  TEST_END("tar rejects over-long name");
}

/** @test A filename with XML metacharacters yields escaped, well-formed EPUB. */
static void test_epub_escapes_name(void)
{
  TEST_BEGIN("epub escapes filename");
  const char* dir = "/tmp/mdl_xml_chap";
  const char* out = "/tmp/mdl_xml_chap.epub";
  const char* img = "/tmp/mdl_xml_chap/a&b<c>d.jpg";
  (void)mkdir(dir, (mode_t)k_mdl_test_dir_mode);
  write_fixture(img, 'x');
  TEST_ASSERT(mdl_export_chapter(k_mdl_fmt_epub, dir, out) == k_ra8_ok);

  mz_zip_archive zr;
  memset(&zr, 0, sizeof(zr));
  TEST_ASSERT(mz_zip_reader_init_file(&zr, out, 0) != MZ_FALSE);
  char* xhtml = zip_entry_str(&zr, "OEBPS/page_001.xhtml");
  TEST_ASSERT_NOT_NULL(xhtml);
  TEST_ASSERT(strstr(xhtml, "a&amp;b&lt;c&gt;d.jpg") != nullptr); /* escaped */
  TEST_ASSERT(strstr(xhtml, "a&b<c>d.jpg") == nullptr);           /* not raw */
  free(xhtml);
  char* opf = zip_entry_str(&zr, "OEBPS/content.opf");
  TEST_ASSERT_NOT_NULL(opf);
  TEST_ASSERT(strstr(opf, "images/a&amp;b&lt;c&gt;d.jpg") != nullptr);
  free(opf);
  (void)mz_zip_reader_end(&zr);

  (void)unlink(img);
  (void)unlink(out);
  (void)rmdir(dir);
  TEST_END("epub escapes filename");
}

/**
 * @test An EPUB built from a maximal-length, XML-escaping filename is complete.
 * @details A 240-`&` page name escapes to 1200+ bytes and is embedded in both
 *          the manifest fragment and the page xhtml. The pre-#308 512/1024-byte
 *          accumulators could not hold it, so the export truncated (or, after
 *          the fragment guard, failed) on a legitimate long name. Correct
 *          worst-case sizing must now package it, and the OPF must carry the
 *          whole escaped href plus its `</manifest>` / `</package>` closers --
 *          proving the manifest was not cut off mid-element.
 */
static void test_epub_long_filenames(void)
{
  TEST_BEGIN("epub long filenames");
  const char* dir = "/tmp/mdl_epublong_chap";
  const char* out = "/tmp/mdl_epublong_chap.epub";
  (void)mkdir(dir, (mode_t)k_mdl_test_dir_mode);

  /* raw name: 240 '&' + ".jpg" (244 bytes, under NAME_MAX). */
  char raw[k_buf_256];
  memset(raw, '&', (size_t)k_long_amp_run);
  memcpy(raw + k_long_amp_run, ".jpg", sizeof(".jpg"));
  /* escaped name: "&amp;" x240 + ".jpg" (1204 bytes). */
  char   esc[k_buf_2k];
  size_t p = 0U;
  for (uint16_t i = 0U; i < (uint16_t)k_long_amp_run; ++i) {
    memcpy(esc + p, "&amp;", (size_t)k_amp_esc_len);
    p += (size_t)k_amp_esc_len;
  }
  memcpy(esc + p, ".jpg", sizeof(".jpg"));

  char path[k_buf_320];
  (void)snprintf(path, sizeof(path), "%s/%s", dir, raw);
  write_fixture(path, 'x');
  TEST_ASSERT(mdl_export_chapter(k_mdl_fmt_epub, dir, out) == k_ra8_ok);

  mz_zip_archive zr;
  memset(&zr, 0, sizeof(zr));
  TEST_ASSERT(mz_zip_reader_init_file(&zr, out, 0) != MZ_FALSE);
  char* opf = zip_entry_str(&zr, "OEBPS/content.opf");
  TEST_ASSERT_NOT_NULL(opf);
  char href[k_buf_4k]; /* room for the "images/" prefix + a full k_buf_2k esc */
  (void)snprintf(href, sizeof(href), "images/%s", esc);
  TEST_ASSERT(strstr(opf, href) != nullptr);          /* whole escaped name present */
  TEST_ASSERT(strstr(opf, "</manifest>") != nullptr); /* manifest not truncated     */
  TEST_ASSERT(strstr(opf, "</package>") != nullptr);  /* document closed            */
  free(opf);
  /* the image entry is stored under the raw (unescaped) name, untruncated. */
  char zipname[k_buf_320];
  (void)snprintf(zipname, sizeof(zipname), "OEBPS/images/%s", raw);
  TEST_ASSERT(mz_zip_reader_locate_file(&zr, zipname, nullptr, 0) >= 0);
  (void)mz_zip_reader_end(&zr);

  (void)unlink(path);
  (void)unlink(out);
  (void)rmdir(dir);
  TEST_END("epub long filenames");
}

/**
 * @test A directory holding more than k_max_pages images fails, not truncates.
 * @details ::list_pages fills a fixed ::k_max_pages table; before #308 it
 *          returned the capped count with no signal, so a larger chapter
 *          packaged short and successfully. It now flags the overflow and
 *          ::mdl_export_chapter fails instead of dropping the extra pages.
 */
static void test_export_page_cap(void)
{
  TEST_BEGIN("export page cap");
  const char*  dir  = "/tmp/mdl_cap_chap";
  const char*  out  = "/tmp/mdl_cap_chap.cbz";
  const size_t over = (size_t)k_max_pages + 1U;
  (void)mkdir(dir, (mode_t)k_mdl_test_dir_mode);
  for (size_t i = 0U; i < over; ++i) {
    char path[k_buf_256];
    (void)snprintf(path, sizeof(path), "%s/page_%05zu.jpg", dir, i);
    write_fixture(path, 'x');
  }
  /* One image too many -> refuse rather than package a short chapter. */
  TEST_ASSERT(mdl_export_chapter(k_mdl_fmt_cbz, dir, out) == k_ra8_err_invalid_size);

  for (size_t i = 0U; i < over; ++i) {
    char path[k_buf_256];
    (void)snprintf(path, sizeof(path), "%s/page_%05zu.jpg", dir, i);
    (void)unlink(path);
  }
  (void)unlink(out);
  (void)rmdir(dir);
  TEST_END("export page cap");
}

/* ---- #302: robots.txt parser / matcher / cache (mdl_robots) ------------- */

/** @test Most-specific User-agent group wins; Crawl-delay + reason extracted. */
static void test_robots_group_select(void)
{
  TEST_BEGIN("robots group selection");
  static const char txt[] = "User-agent: *\nDisallow: /all\n\n"
                            "User-agent: media_dl\nDisallow: /mine\nCrawl-delay: 5\n";
  mdl_robots_t      r;
  mdl_robots_parse(txt, sizeof(txt) - 1U, "media_dl", &r);
  TEST_ASSERT(!mdl_robots_allows(&r, "/mine/x")); /* specific group applies */
  TEST_ASSERT(mdl_robots_allows(&r, "/all/x"));   /* wildcard group ignored */
  TEST_ASSERT(r.have_crawl_delay);
  TEST_ASSERT_EQ((uint32_t)k_crawl_5s_ms, r.crawl_delay_ms);
  const char* why = mdl_robots_disallow_reason(&r, "/mine/x");
  TEST_ASSERT_NOT_NULL(why);
  TEST_ASSERT(strcmp(why, "/mine") == 0);
  TEST_END("robots group selection");
}

/** @test A longer Allow beats a shorter Disallow (RFC 9309 tie rule). */
static void test_robots_allow_wins(void)
{
  TEST_BEGIN("robots longest-match allow");
  static const char txt[] = "User-agent: *\nDisallow: /dir/\nAllow: /dir/ok\n";
  mdl_robots_t      r;
  mdl_robots_parse(txt, sizeof(txt) - 1U, "media_dl", &r);
  TEST_ASSERT(mdl_robots_allows(&r, "/dir/ok/page")); /* longer Allow wins     */
  TEST_ASSERT(!mdl_robots_allows(&r, "/dir/secret")); /* only Disallow matches */
  TEST_END("robots longest-match allow");
}

/** @test `*` glob and trailing `$` anchor behave per spec. */
static void test_robots_wildcard_anchor(void)
{
  TEST_BEGIN("robots wildcard + anchor");
  static const char txt[] = "User-agent: *\nDisallow: /*.pdf$\n";
  mdl_robots_t      r;
  mdl_robots_parse(txt, sizeof(txt) - 1U, "media_dl", &r);
  TEST_ASSERT(!mdl_robots_allows(&r, "/docs/a.pdf"));     /* matches to end    */
  TEST_ASSERT(mdl_robots_allows(&r, "/docs/a.pdf.html")); /* $ anchors the end */
  TEST_END("robots wildcard + anchor");
}

/** @test Empty, malformed, and no-matching-group inputs all allow everything. */
static void test_robots_edge(void)
{
  TEST_BEGIN("robots edge cases");
  mdl_robots_t r;
  mdl_robots_parse("", 0U, "media_dl", &r);
  TEST_ASSERT(mdl_robots_allows(&r, "/anything"));
  static const char junk[] = "###\n:::\nDisallow no colon\n\n\n";
  mdl_robots_parse(junk, sizeof(junk) - 1U, "media_dl", &r);
  TEST_ASSERT(mdl_robots_allows(&r, "/x"));
  static const char other[] = "User-agent: googlebot\nDisallow: /\n";
  mdl_robots_parse(other, sizeof(other) - 1U, "media_dl", &r);
  TEST_ASSERT(mdl_robots_allows(&r, "/x")); /* no group for us -> allow all */
  TEST_END("robots edge cases");
}

/** @test Per-host cache fetches once, and applies the 5xx/absent conventions. */
static void test_robots_cache(void)
{
  TEST_BEGIN("robots cache");
  static mdl_robots_cache_t s_cache;
  memset(&s_cache, 0, sizeof(s_cache));
  char                scratch[k_buf_4k];
  fake_fetch_ctx_t    ok = {.count  = 0,
                            .body   = "User-agent: *\nDisallow: /x\n",
                            .result = k_mdl_robots_fetch_ok};
  const mdl_robots_t* r  = mdl_robots_cache_consult(&s_cache,
                                                    "https",
                                                    "site.net",
                                                    "media_dl",
                                                    fake_fetch,
                                                    &ok,
                                                    scratch,
                                                    sizeof(scratch));
  TEST_ASSERT_NOT_NULL(r);
  TEST_ASSERT(!mdl_robots_allows(r, "/x/y"));
  TEST_ASSERT(mdl_robots_allows(r, "/z"));
  const mdl_robots_t* r2 = mdl_robots_cache_consult(&s_cache,
                                                    "https",
                                                    "site.net",
                                                    "media_dl",
                                                    fake_fetch,
                                                    &ok,
                                                    scratch,
                                                    sizeof(scratch));
  TEST_ASSERT(r2 == r);                            /* cache hit: same entry */
  TEST_ASSERT_EQ((uint16_t)1, (uint16_t)ok.count); /* fetched exactly once  */

  fake_fetch_ctx_t    deny = {.count = 0, .body = nullptr, .result = k_mdl_robots_fetch_denied};
  const mdl_robots_t* rd   = mdl_robots_cache_consult(&s_cache,
                                                      "https",
                                                      "deny.net",
                                                      "media_dl",
                                                      fake_fetch,
                                                      &deny,
                                                      scratch,
                                                      sizeof(scratch));
  TEST_ASSERT(rd == nullptr); /* 5xx -> disallow all */

  fake_fetch_ctx_t    gone = {.count = 0, .body = nullptr, .result = k_mdl_robots_fetch_absent};
  const mdl_robots_t* rg   = mdl_robots_cache_consult(&s_cache,
                                                      "https",
                                                      "gone.net",
                                                      "media_dl",
                                                      fake_fetch,
                                                      &gone,
                                                      scratch,
                                                      sizeof(scratch));
  TEST_ASSERT_NOT_NULL(rg);
  TEST_ASSERT(mdl_robots_allows(rg, "/anything")); /* absent -> allow all */
  TEST_END("robots cache");
}

/** @test Test new CLI flags (--proxy, --socks5, --cookie-file, --progress, --verify, --init-site). */
static void test_cli_new_flags(void)
{
  TEST_BEGIN("cli new flags parsing");
  char* argv[] = {"media_dl",
                  "--proxy",
                  "http://proxy.example.com:8080",
                  "--socks5",
                  "socks5://127.0.0.1:1080",
                  "--cookie-file",
                  "/tmp/cookies.txt",
                  "--progress",
                  "--refetch",
                  "--init-site",
                  "https://example.com/manga/",
                  "--verify",
                  "/tmp/downloads"};
  int   argc   = sizeof(argv) / sizeof(argv[0]);

  mdl_args_t a = {};
  mdl_cli_parse(argc, argv, &a);

  TEST_ASSERT(!a.bad);
  TEST_ASSERT(a.proxy != nullptr && strcmp(a.proxy, "http://proxy.example.com:8080") == 0);
  TEST_ASSERT(a.socks5 != nullptr && strcmp(a.socks5, "socks5://127.0.0.1:1080") == 0);
  TEST_ASSERT(a.cookie_file != nullptr && strcmp(a.cookie_file, "/tmp/cookies.txt") == 0);
  TEST_ASSERT(a.progress == true);
  TEST_ASSERT(a.refetch == true);
  TEST_ASSERT(a.init_site_url != nullptr &&
              strcmp(a.init_site_url, "https://example.com/manga/") == 0);
  TEST_ASSERT(a.verify == true);
  TEST_ASSERT(a.verify_dir != nullptr && strcmp(a.verify_dir, "/tmp/downloads") == 0);

  mdl_run_opts_t opts = mdl_cli_run_opts(&a);
  TEST_ASSERT(opts.progress == true);
  TEST_ASSERT(opts.refetch == true);
  TEST_ASSERT(opts.policy.proxy != nullptr &&
              strcmp(opts.policy.proxy, "http://proxy.example.com:8080") == 0);
  TEST_ASSERT(opts.policy.socks5 != nullptr &&
              strcmp(opts.policy.socks5, "socks5://127.0.0.1:1080") == 0);
  TEST_ASSERT(opts.policy.cookie_file != nullptr &&
              strcmp(opts.policy.cookie_file, "/tmp/cookies.txt") == 0);
  TEST_END("cli new flags parsing");
}

/** @test A value-taking option at argv end is a usage error. */
static void test_cli_missing_value(void)
{
  TEST_BEGIN("cli missing option value");
  char*      argv[] = {"media_dl", "--config"};
  mdl_args_t args   = {};
  mdl_cli_parse(2, argv, &args);
  TEST_ASSERT(args.bad);
  TEST_ASSERT(args.cfg == nullptr);
  TEST_END("cli missing option value");
}

/** @brief Parse one vector and assert its validation result/mode. */
static void assert_cli(int argc, char** argv, bool valid, mdl_cli_mode_t expected)
{
  mdl_args_t     args = {};
  mdl_cli_mode_t mode = k_mdl_cli_mode_invalid;
  mdl_cli_parse(argc, argv, &args);
  TEST_ASSERT(mdl_cli_validate(&args, &mode) == valid);
  TEST_ASSERT(mode == expected);
}

/** @test Every advertised command mode has an unambiguous option allowlist. */
static void test_cli_mode_matrix(void)
{
  TEST_BEGIN("cli strict mode matrix");
  char* series[] = {"media_dl",
                    "--config",
                    "site.conf",
                    "--series",
                    "https://s/x/",
                    "--out",
                    "library",
                    "--chapters",
                    "2",
                    "--from",
                    "1",
                    "--seed",
                    "2",
                    "--timeout",
                    "100",
                    "--format",
                    "loose",
                    "--contact",
                    "ops",
                    "--max-bytes",
                    "4096",
                    "--proxy",
                    "http://p",
                    "--allow-private",
                    "--separate",
                    "--update",
                    "--polite",
                    "--ignore-robots",
                    "--cross-host",
                    "--allow-incomplete",
                    "--progress",
                    "--refetch",
                    "--cookie-file",
                    "cookies"};
  assert_cli((int)(sizeof(series) / sizeof(series[0])), series, true, k_mdl_cli_mode_series);

  char* search[] = {"media_dl", "--config", "site.conf", "--search", "alpha"};
  assert_cli((int)(sizeof(search) / sizeof(search[0])), search, true, k_mdl_cli_mode_search);
  char* search_pick[] = {"media_dl",
                         "--config",
                         "site.conf",
                         "--search",
                         "alpha",
                         "--pick",
                         "1",
                         "--format",
                         "cbz",
                         "--out",
                         "library"};
  assert_cli((int)(sizeof(search_pick) / sizeof(search_pick[0])),
             search_pick,
             true,
             k_mdl_cli_mode_search);
  char* browse[] = {"media_dl", "--config", "site.conf", "--browse"};
  assert_cli((int)(sizeof(browse) / sizeof(browse[0])), browse, true, k_mdl_cli_mode_browse);
  char* list[] = {"media_dl", "--list", "--out", "library"};
  assert_cli((int)(sizeof(list) / sizeof(list[0])), list, true, k_mdl_cli_mode_list);
  char* update_all[] = {"media_dl",
                        "--update-all",
                        "--config",
                        "site.conf",
                        "--out",
                        "library",
                        "--format",
                        "loose",
                        "--refetch"};
  assert_cli((int)(sizeof(update_all) / sizeof(update_all[0])),
             update_all,
             true,
             k_mdl_cli_mode_update_all);
  char* remove[] = {"media_dl", "--remove", "alpha", "--out", "library"};
  assert_cli((int)(sizeof(remove) / sizeof(remove[0])), remove, true, k_mdl_cli_mode_remove);
  char* verify[] = {"media_dl", "--verify", "library"};
  assert_cli((int)(sizeof(verify) / sizeof(verify[0])), verify, true, k_mdl_cli_mode_verify);
  char* init[] = {"media_dl", "--init-site", "https://s.example/"};
  assert_cli((int)(sizeof(init) / sizeof(init[0])), init, true, k_mdl_cli_mode_init_site);
  char* pack[] = {"media_dl", "--pack", "pages", "--format", "cbz"};
  assert_cli((int)(sizeof(pack) / sizeof(pack[0])), pack, true, k_mdl_cli_mode_pack);
  char* page[] = {"media_dl",
                  "https://s/page",
                  "--out",
                  "pages",
                  "--attr",
                  "src",
                  "--max",
                  "2",
                  "--socks5",
                  "socks5://proxy",
                  "--allow-private"};
  assert_cli((int)(sizeof(page) / sizeof(page[0])), page, true, k_mdl_cli_mode_page);

  char* conflict[] = {"media_dl", "--list", "--series", "https://s/x", "--config", "s"};
  assert_cli((int)(sizeof(conflict) / sizeof(conflict[0])),
             conflict,
             false,
             k_mdl_cli_mode_invalid);
  char* no_mode[] = {"media_dl", "--config", "s"};
  assert_cli((int)(sizeof(no_mode) / sizeof(no_mode[0])), no_mode, false, k_mdl_cli_mode_invalid);
  char* no_cfg[] = {"media_dl", "--search", "x"};
  assert_cli((int)(sizeof(no_cfg) / sizeof(no_cfg[0])), no_cfg, false, k_mdl_cli_mode_invalid);
  char* no_effect[] = {"media_dl", "--search", "x", "--config", "s", "--format", "cbz"};
  assert_cli((int)(sizeof(no_effect) / sizeof(no_effect[0])),
             no_effect,
             false,
             k_mdl_cli_mode_invalid);
  char* pack_default[] = {"media_dl", "--pack", "pages"};
  assert_cli((int)(sizeof(pack_default) / sizeof(pack_default[0])),
             pack_default,
             false,
             k_mdl_cli_mode_invalid);
  char* proxy_closed[] = {"media_dl", "https://s/page", "--proxy", "http://p"};
  assert_cli((int)(sizeof(proxy_closed) / sizeof(proxy_closed[0])),
             proxy_closed,
             false,
             k_mdl_cli_mode_invalid);
  char* verify_dirs[] = {"media_dl", "--verify", "a", "--out", "b"};
  assert_cli((int)(sizeof(verify_dirs) / sizeof(verify_dirs[0])),
             verify_dirs,
             false,
             k_mdl_cli_mode_invalid);
  char* duplicate[] = {"media_dl", "--list", "--list"};
  assert_cli((int)(sizeof(duplicate) / sizeof(duplicate[0])),
             duplicate,
             false,
             k_mdl_cli_mode_invalid);
  TEST_END("cli strict mode matrix");
}

/** @test Numeric values that used to wrap or silently clamp are usage errors. */
static void test_cli_numeric_bounds(void)
{
  TEST_BEGIN("cli numeric bounds");
  mdl_nums_t nums = {};
  mdl_args_t args = {.timeout = "0"};
  TEST_ASSERT(!mdl_cli_parse_nums(&args, &nums));
  args = (mdl_args_t){.chapters = "0"};
  TEST_ASSERT(!mdl_cli_parse_nums(&args, &nums));
  args = (mdl_args_t){.max_bytes = "0"};
  TEST_ASSERT(!mdl_cli_parse_nums(&args, &nums));
  args = (mdl_args_t){.pick = "0"};
  TEST_ASSERT(!mdl_cli_parse_nums(&args, &nums));
  TEST_END("cli numeric bounds");
}

/** @test Rich metadata init, key-value parsing, XML parsing, and dir auto-discovery. */
static void test_meta_init_parse_load(void)
{
  TEST_BEGIN("metadata init, parse, and load");
  mdl_export_meta_t meta;
  mdl_meta_init(&meta);
  TEST_ASSERT_EQ(-1, meta.cover_index);
  TEST_ASSERT(meta.series_title[0] == '\0');

  /* Key-value parsing */
  static const char kv[] = "series = Test Series & Saga\n"
                           "title = Chapter 12: Beginning & End\n"
                           "writer = Author & Writer\n"
                           "artist = Illustrator & Artist\n"
                           "number = 12.5\n"
                           "summary = A great story <start>\n"
                           "source_url = https://example.test/series?a=1&title=<Origins>\n"
                           "cover = page_002.jpg\n"
                           "cover_index = 1\n"
                           "language = ja\n"
                           "reading_direction = rtl\n"
                           "identifier = book:test\n"
                           "modified = 2025-02-03T04:05:06Z\n";
  TEST_ASSERT(mdl_meta_parse(&meta, kv) == k_ra8_ok);
  TEST_ASSERT(strcmp(meta.series_title, "Test Series & Saga") == 0);
  TEST_ASSERT(strcmp(meta.chapter_title, "Chapter 12: Beginning & End") == 0);
  TEST_ASSERT(strcmp(meta.writer, "Author & Writer") == 0);
  TEST_ASSERT(strcmp(meta.artist, "Illustrator & Artist") == 0);
  TEST_ASSERT(meta.chapter_number == 12.5);
  TEST_ASSERT(strcmp(meta.summary, "A great story <start>") == 0);
  TEST_ASSERT(strcmp(meta.source_url, "https://example.test/series?a=1&title=<Origins>") == 0);
  TEST_ASSERT(strcmp(meta.cover_path, "page_002.jpg") == 0);
  TEST_ASSERT_EQ(1, meta.cover_index);
  TEST_ASSERT(strcmp(meta.language, "ja") == 0);
  TEST_ASSERT(meta.reading_direction == k_mdl_read_rtl);
  TEST_ASSERT(strcmp(meta.identifier, "book:test") == 0);
  TEST_ASSERT(strcmp(meta.modified, "2025-02-03T04:05:06Z") == 0);

  /* A PATH_MAX-style cover fits exactly; one additional byte is rejected. */
  char exact_cover[k_mdl_meta_path_max];
  memset(exact_cover, 'a', sizeof(exact_cover) - 1U);
  exact_cover[sizeof(exact_cover) - 1U] = '\0';
  char cover_line[k_mdl_meta_path_max + k_meta_line_test_slack];
  (void)snprintf(cover_line, sizeof(cover_line), "cover=%s\n", exact_cover);
  mdl_meta_init(&meta);
  TEST_ASSERT(mdl_meta_parse(&meta, cover_line) == k_ra8_ok);
  TEST_ASSERT(strlen(meta.cover_path) == sizeof(exact_cover) - 1U);

  char overlong_cover[k_mdl_meta_path_max + 1U];
  memset(overlong_cover, 'b', sizeof(overlong_cover) - 1U);
  overlong_cover[sizeof(overlong_cover) - 1U] = '\0';
  (void)snprintf(cover_line, sizeof(cover_line), "cover=%s\n", overlong_cover);
  mdl_meta_init(&meta);
  TEST_ASSERT(mdl_meta_parse(&meta, cover_line) == k_ra8_err_invalid_size);
  TEST_ASSERT(meta.cover_path[0] == '\0');

  /* The complete bounded URL fits; overflow and non-web schemes fail closed. */
  char exact_source[k_mdl_meta_url_max];
  memset(exact_source, 'a', sizeof(exact_source) - 1U);
  static const char source_prefix[] = "https://example.test/";
  memcpy(exact_source, source_prefix, sizeof(source_prefix) - 1U);
  exact_source[sizeof(exact_source) - 1U] = '\0';
  char source_line[k_mdl_meta_url_max + k_meta_line_test_slack];
  (void)snprintf(source_line, sizeof(source_line), "source_url=%s\n", exact_source);
  mdl_meta_init(&meta);
  TEST_ASSERT(mdl_meta_parse(&meta, source_line) == k_ra8_ok);
  TEST_ASSERT(strlen(meta.source_url) == sizeof(exact_source) - 1U);

  char overlong_source[k_mdl_meta_url_max + 1U];
  memset(overlong_source, 'b', sizeof(overlong_source) - 1U);
  memcpy(overlong_source, source_prefix, sizeof(source_prefix) - 1U);
  overlong_source[sizeof(overlong_source) - 1U] = '\0';
  (void)snprintf(source_line, sizeof(source_line), "source_url=%s\n", overlong_source);
  mdl_meta_init(&meta);
  TEST_ASSERT(mdl_meta_parse(&meta, source_line) == k_ra8_err_invalid_size);
  TEST_ASSERT(meta.source_url[0] == '\0');

  mdl_meta_init(&meta);
  TEST_ASSERT(mdl_meta_parse(&meta, "source_url=file:///tmp/private\n") == k_ra8_err_invalid_arg);
  TEST_ASSERT(meta.source_url[0] == '\0');

  /* XML ComicInfo parsing */
  mdl_meta_init(&meta);
  static const char xml[] = "<?xml version=\"1.0\"?>\n"
                            "<ComicInfo>\n"
                            "  <Series>XML &amp; Series</Series>\n"
                            "  <Title>XML &lt;Title&gt;</Title>\n"
                            "  <Writer>XML Writer</Writer>\n"
                            "  <Artist>XML Artist</Artist>\n"
                            "  <Number>5</Number>\n"
                            "  <Summary>XML Summary &quot;Quote&quot;</Summary>\n"
                            "  <Web>https://example.test/xml?a=1&amp;title=&lt;Origins&gt;</Web>\n"
                            "  <CoverImage>cover.jpg</CoverImage>\n"
                            "</ComicInfo>";
  TEST_ASSERT(mdl_meta_parse(&meta, xml) == k_ra8_ok);
  TEST_ASSERT(strcmp(meta.series_title, "XML & Series") == 0);
  TEST_ASSERT(strcmp(meta.chapter_title, "XML <Title>") == 0);
  TEST_ASSERT(strcmp(meta.writer, "XML Writer") == 0);
  TEST_ASSERT(strcmp(meta.artist, "XML Artist") == 0);
  TEST_ASSERT(meta.chapter_number == 5.0);
  TEST_ASSERT(strcmp(meta.summary, "XML Summary \"Quote\"") == 0);
  TEST_ASSERT(strcmp(meta.source_url, "https://example.test/xml?a=1&title=<Origins>") == 0);
  TEST_ASSERT(strcmp(meta.cover_path, "cover.jpg") == 0);

  /* Load dir auto-discovery */
  const char* dir = "/tmp/mdl_meta_dir";
  (void)mkdir(dir, (mode_t)k_mdl_test_dir_mode);
  write_fixture("/tmp/mdl_meta_dir/metadata.txt", 'm');
  FILE* f = fopen("/tmp/mdl_meta_dir/metadata.txt", "w");
  TEST_ASSERT_NOT_NULL(f);
  (void)fputs("series: Auto Discovered Series\nwriter: Auto Writer\n", f);
  (void)fclose(f);

  mdl_meta_init(&meta);
  TEST_ASSERT(mdl_meta_load_dir(&meta, dir) == k_ra8_ok);
  TEST_ASSERT(strcmp(meta.series_title, "Auto Discovered Series") == 0);
  TEST_ASSERT(strcmp(meta.writer, "Auto Writer") == 0);

  (void)unlink("/tmp/mdl_meta_dir/metadata.txt");
  (void)rmdir(dir);
  TEST_END("metadata init, parse, and load");
}

/** @test Build ComicInfo.xml string and verify CBZ metadata insertion. */
static void test_comicinfo_xml_generation(void)
{
  TEST_BEGIN("ComicInfo.xml generation & CBZ metadata");
  mdl_export_meta_t meta;
  mdl_meta_init(&meta);
  (void)snprintf(meta.series_title, sizeof(meta.series_title), "Comic & Series");
  (void)snprintf(meta.chapter_title, sizeof(meta.chapter_title), "Ch 1: <Origin>");
  meta.chapter_number = 1.0;
  (void)snprintf(meta.writer, sizeof(meta.writer), "Writer & Author");
  (void)snprintf(meta.artist, sizeof(meta.artist), "Artist & Penciller");
  (void)snprintf(meta.summary, sizeof(meta.summary), "Summary & Description");
  (void)snprintf(meta.source_url,
                 sizeof(meta.source_url),
                 "https://example.test/series?a=1&title=<Origins>");
  (void)snprintf(meta.language, sizeof(meta.language), "ja");
  meta.reading_direction = k_mdl_read_rtl;
  meta.cover_index       = 0;

  char xml_buf[4096];
  TEST_ASSERT(mdl_export_build_comicinfo_pages(&meta, 3U, xml_buf, sizeof(xml_buf)) == k_ra8_ok);
  TEST_ASSERT(strstr(xml_buf, "<Series>Comic &amp; Series</Series>") != nullptr);
  TEST_ASSERT(strstr(xml_buf, "<Title>Ch 1: &lt;Origin&gt;</Title>") != nullptr);
  TEST_ASSERT(strstr(xml_buf, "<Number>1</Number>") != nullptr);
  TEST_ASSERT(strstr(xml_buf, "<Writer>Writer &amp; Author</Writer>") != nullptr);
  TEST_ASSERT(strstr(xml_buf, "<Artist>Artist &amp; Penciller</Artist>") != nullptr);
  TEST_ASSERT(strstr(xml_buf, "<Summary>Summary &amp; Description</Summary>") != nullptr);
  TEST_ASSERT(
    strstr(xml_buf, "<Web>https://example.test/series?a=1&amp;title=&lt;Origins&gt;</Web>") !=
    nullptr);
  TEST_ASSERT(strstr(xml_buf, "<PageCount>3</PageCount>") != nullptr);
  TEST_ASSERT(strstr(xml_buf, "<LanguageISO>ja</LanguageISO>") != nullptr);
  TEST_ASSERT(strstr(xml_buf, "<Manga>YesAndRightToLeft</Manga>") != nullptr);
  TEST_ASSERT(strstr(xml_buf, "Image=\"0\" Type=\"FrontCover\"") != nullptr);

  /* Export CBZ with metadata and verify ComicInfo.xml inside zip */
  const char* dir = "/tmp/mdl_cbz_meta_chap";
  const char* out = "/tmp/mdl_cbz_meta_chap.cbz";
  (void)mkdir(dir, (mode_t)k_mdl_test_dir_mode);
  write_fixture("/tmp/mdl_cbz_meta_chap/page_001.jpg", 'a');

  TEST_ASSERT(mdl_export_chapter_meta(k_mdl_fmt_cbz, dir, out, &meta) == k_ra8_ok);

  mz_zip_archive zr;
  memset(&zr, 0, sizeof(zr));
  TEST_ASSERT(mz_zip_reader_init_file(&zr, out, 0) != MZ_FALSE);
  char* content = zip_entry_str(&zr, "ComicInfo.xml");
  TEST_ASSERT_NOT_NULL(content);
  TEST_ASSERT(strstr(content, "<Series>Comic &amp; Series</Series>") != nullptr);
  TEST_ASSERT(strstr(content, "<PageCount>1</PageCount>") != nullptr);
  TEST_ASSERT(strstr(content, "<LanguageISO>ja</LanguageISO>") != nullptr);
  TEST_ASSERT(
    strstr(content, "<Web>https://example.test/series?a=1&amp;title=&lt;Origins&gt;</Web>") !=
    nullptr);
  free(content);
  (void)mz_zip_reader_end(&zr);

  mdl_export_meta_t overlong_meta = meta;
  memset(overlong_meta.source_url, 'x', sizeof(overlong_meta.source_url));
  const char* rejected_out = "/tmp/mdl_cbz_meta_rejected.cbz";
  (void)unlink(rejected_out);
  TEST_ASSERT(mdl_export_chapter_meta(k_mdl_fmt_cbz, dir, rejected_out, &overlong_meta) ==
              k_ra8_err_invalid_size);
  TEST_ASSERT(access(rejected_out, F_OK) != 0);

  (void)unlink("/tmp/mdl_cbz_meta_chap/page_001.jpg");
  (void)unlink(out);
  (void)unlink(rejected_out);
  (void)rmdir(dir);
  TEST_END("ComicInfo.xml generation & CBZ metadata");
}

/** @test EPUB metadata generation, unique UUID identifier, and cover image property. */
static void test_epub_metadata_and_uuid(void)
{
  TEST_BEGIN("EPUB metadata, UUID, and cover image");
  const char* dir  = "/tmp/mdl_epub_meta_chap";
  const char* out  = "/tmp/mdl_epub_meta_chap.epub";
  const char* out2 = "/tmp/mdl_epub_meta_chap_2.epub";
  (void)mkdir(dir, (mode_t)k_mdl_test_dir_mode);
  write_fixture("/tmp/mdl_epub_meta_chap/page_001.jpg", 'a');
  write_fixture("/tmp/mdl_epub_meta_chap/page_002.jpg", 'b');

  mdl_export_meta_t meta;
  mdl_meta_init(&meta);
  (void)snprintf(meta.series_title, sizeof(meta.series_title), "EPUB Series");
  (void)snprintf(meta.chapter_title, sizeof(meta.chapter_title), "EPUB Chapter 1");
  (void)snprintf(meta.writer, sizeof(meta.writer), "EPUB Writer");
  (void)snprintf(meta.artist, sizeof(meta.artist), "EPUB Artist");
  (void)snprintf(meta.summary, sizeof(meta.summary), "EPUB Summary");
  (void)snprintf(meta.source_url,
                 sizeof(meta.source_url),
                 "https://example.test/series?a=1&title=<Origins>");
  meta.cover_index = 1; /* page_002.jpg is cover */
  (void)snprintf(meta.language, sizeof(meta.language), "ja");
  meta.reading_direction = k_mdl_read_rtl;

  TEST_ASSERT(mdl_export_chapter_meta(k_mdl_fmt_epub, dir, out, &meta) == k_ra8_ok);

  mz_zip_archive zr;
  memset(&zr, 0, sizeof(zr));
  TEST_ASSERT(mz_zip_reader_init_file(&zr, out, 0) != MZ_FALSE);
  char* opf = zip_entry_str(&zr, "OEBPS/content.opf");
  TEST_ASSERT_NOT_NULL(opf);

  /* Check UUID identifier */
  TEST_ASSERT(strstr(opf, "<dc:identifier id=\"bookid\">urn:uuid:") != nullptr);

  /* Check metadata tags */
  TEST_ASSERT(strstr(opf, "<dc:title>EPUB Chapter 1</dc:title>") != nullptr);
  TEST_ASSERT(strstr(opf, "<dc:creator opf:role=\"aut\">EPUB Writer</dc:creator>") != nullptr);
  TEST_ASSERT(strstr(opf, "<dc:creator opf:role=\"art\">EPUB Artist</dc:creator>") != nullptr);
  TEST_ASSERT(strstr(opf, "<dc:description>EPUB Summary</dc:description>") != nullptr);
  TEST_ASSERT(
    strstr(opf,
           "<dc:source>https://example.test/series?a=1&amp;title=&lt;Origins&gt;</dc:source>") !=
    nullptr);
  TEST_ASSERT(strstr(opf, "<dc:language>ja</dc:language>") != nullptr);
  TEST_ASSERT(strstr(opf, "<meta property=\"schema:numberOfPages\">2</meta>") != nullptr);
  TEST_ASSERT(strstr(opf, "<spine page-progression-direction=\"rtl\">") != nullptr);

  /* Check cover image property on img1 (page_002.jpg) */
  TEST_ASSERT(
    strstr(
      opf,
      "id=\"img1\" href=\"images/page_002.jpg\" media-type=\"image/jpeg\" properties=\"cover-image\"") !=
    nullptr);

  (void)mz_zip_reader_end(&zr);
  TEST_ASSERT(mdl_export_chapter_meta(k_mdl_fmt_epub, dir, out2, &meta) == k_ra8_ok);
  mz_zip_archive zr2;
  memset(&zr2, 0, sizeof(zr2));
  TEST_ASSERT(mz_zip_reader_init_file(&zr2, out2, 0) != MZ_FALSE);
  char* opf2 = zip_entry_str(&zr2, "OEBPS/content.opf");
  TEST_ASSERT_NOT_NULL(opf2);
  TEST_ASSERT(strcmp(opf, opf2) == 0);
  free(opf2);
  free(opf);
  (void)mz_zip_reader_end(&zr2);

  mdl_export_meta_t overlong_meta = meta;
  memset(overlong_meta.source_url, 'x', sizeof(overlong_meta.source_url));
  const char* rejected_out = "/tmp/mdl_epub_meta_rejected.epub";
  (void)unlink(rejected_out);
  TEST_ASSERT(mdl_export_chapter_meta(k_mdl_fmt_epub, dir, rejected_out, &overlong_meta) ==
              k_ra8_err_invalid_size);
  TEST_ASSERT(access(rejected_out, F_OK) != 0);

  (void)unlink("/tmp/mdl_epub_meta_chap/page_001.jpg");
  (void)unlink("/tmp/mdl_epub_meta_chap/page_002.jpg");
  (void)unlink(out);
  (void)unlink(out2);
  (void)unlink(rejected_out);
  (void)rmdir(dir);
  TEST_END("EPUB metadata, UUID, and cover image");
}

/** @test An external series cover is typed by bytes and embedded canonically. */
static void test_external_cover_embedding(void)
{
  TEST_BEGIN("external cover embedding");
  static const char cover_bytes[] = "\x89PNG\r\n\x1a\nseries-cover";
  const char*       dir           = "/tmp/mdl_external_cover_chap";
  const char*       cover_path    = "/tmp/mdl_series_cover.jpg";
  const char*       bad_path      = "/tmp/mdl_series_cover_bad.jpg";
  const char*       cbz_out       = "/tmp/mdl_external_cover_chap.cbz";
  const char*       bad_out       = "/tmp/mdl_external_cover_bad.cbz";
  const char*       epub_out      = "/tmp/mdl_external_cover_chap.epub";
  (void)mkdir(dir, (mode_t)k_mdl_test_dir_mode);
  write_fixture("/tmp/mdl_external_cover_chap/page_001.jpg", 'p');
  write_binary_fixture(cover_path, cover_bytes, sizeof(cover_bytes) - 1U);

  mdl_export_meta_t meta;
  mdl_meta_init(&meta);
  (void)snprintf(meta.cover_path, sizeof(meta.cover_path), "%s", cover_path);

  TEST_ASSERT(mdl_export_chapter_meta(k_mdl_fmt_cbz, dir, cbz_out, &meta) == k_ra8_ok);
  mz_zip_archive cbz;
  memset(&cbz, 0, sizeof(cbz));
  TEST_ASSERT(mz_zip_reader_init_file(&cbz, cbz_out, 0) != MZ_FALSE);
  char embedded[sizeof(cover_bytes) - 1U];
  TEST_ASSERT(
    mz_zip_reader_extract_file_to_mem(&cbz, "cover/cover.png", embedded, sizeof(embedded), 0));
  TEST_ASSERT(memcmp(embedded, cover_bytes, sizeof(embedded)) == 0);
  char* comic_info = zip_entry_str(&cbz, "ComicInfo.xml");
  TEST_ASSERT_NOT_NULL(comic_info);
  TEST_ASSERT(strstr(comic_info, "<PageCount>2</PageCount>") != nullptr);
  TEST_ASSERT(strstr(comic_info, "Image=\"0\" Type=\"FrontCover\"") != nullptr);
  free(comic_info);
  (void)mz_zip_reader_end(&cbz);

  TEST_ASSERT(mdl_export_chapter_meta(k_mdl_fmt_epub, dir, epub_out, &meta) == k_ra8_ok);
  mz_zip_archive epub;
  memset(&epub, 0, sizeof(epub));
  TEST_ASSERT(mz_zip_reader_init_file(&epub, epub_out, 0) != MZ_FALSE);
  TEST_ASSERT(mz_zip_reader_extract_file_to_mem(&epub,
                                                "OEBPS/cover/cover.png",
                                                embedded,
                                                sizeof(embedded),
                                                0));
  TEST_ASSERT(memcmp(embedded, cover_bytes, sizeof(embedded)) == 0);
  char* opf = zip_entry_str(&epub, "OEBPS/content.opf");
  TEST_ASSERT_NOT_NULL(opf);
  TEST_ASSERT(strstr(opf,
                     "id=\"cover-image\" href=\"cover/cover.png\" media-type=\"image/png\" "
                     "properties=\"cover-image\"") != nullptr);
  free(opf);
  (void)mz_zip_reader_end(&epub);

  write_fixture(bad_path, 'x');
  (void)snprintf(meta.cover_path, sizeof(meta.cover_path), "%s", bad_path);
  (void)unlink(bad_out);
  TEST_ASSERT(mdl_export_chapter_meta(k_mdl_fmt_cbz, dir, bad_out, &meta) ==
              k_ra8_err_validation_failed);
  TEST_ASSERT(access(bad_out, F_OK) != 0);

  (void)unlink("/tmp/mdl_external_cover_chap/page_001.jpg");
  (void)unlink(cover_path);
  (void)unlink(bad_path);
  (void)unlink(cbz_out);
  (void)unlink(epub_out);
  (void)rmdir(dir);
  TEST_END("external cover embedding");
}

/**
 * @test test_image_magic_bytes
 * @brief Unit tests for image magic and MIME typing, including BMP consistency.
 */
static void test_image_magic_bytes(void)
{
  TEST_BEGIN("image magic byte & Content-Type typing");
  char ext[16];
  char mime[32];

  /* 1. JPEG magic bytes: FF D8 FF */
  static const uint8_t jpeg_hdr[] = {0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x10};
  TEST_ASSERT(mdl_urlname_sniff_image_type(jpeg_hdr,
                                           sizeof(jpeg_hdr),
                                           nullptr,
                                           ext,
                                           sizeof(ext),
                                           mime,
                                           sizeof(mime)));
  TEST_ASSERT(strcmp(ext, "jpg") == 0);
  TEST_ASSERT(strcmp(mime, "image/jpeg") == 0);

  /* 2. PNG magic bytes: 89 50 4E 47 */
  static const uint8_t png_hdr[] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  TEST_ASSERT(mdl_urlname_sniff_image_type(png_hdr,
                                           sizeof(png_hdr),
                                           nullptr,
                                           ext,
                                           sizeof(ext),
                                           mime,
                                           sizeof(mime)));
  TEST_ASSERT(strcmp(ext, "png") == 0);
  TEST_ASSERT(strcmp(mime, "image/png") == 0);

  /* 3. WebP magic bytes: RIFF....WEBP */
  static const uint8_t webp_hdr[] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'E', 'B', 'P'};
  TEST_ASSERT(mdl_urlname_sniff_image_type(webp_hdr,
                                           sizeof(webp_hdr),
                                           nullptr,
                                           ext,
                                           sizeof(ext),
                                           mime,
                                           sizeof(mime)));
  TEST_ASSERT(strcmp(ext, "webp") == 0);
  TEST_ASSERT(strcmp(mime, "image/webp") == 0);

  /* 4. GIF magic bytes: GIF87a / GIF89a */
  static const uint8_t gif87_hdr[] = {'G', 'I', 'F', '8', '7', 'a', 0, 0};
  TEST_ASSERT(mdl_urlname_sniff_image_type(gif87_hdr,
                                           sizeof(gif87_hdr),
                                           nullptr,
                                           ext,
                                           sizeof(ext),
                                           mime,
                                           sizeof(mime)));
  TEST_ASSERT(strcmp(ext, "gif") == 0);
  TEST_ASSERT(strcmp(mime, "image/gif") == 0);

  static const uint8_t gif89_hdr[] = {'G', 'I', 'F', '8', '9', 'a', 0, 0};
  TEST_ASSERT(mdl_urlname_sniff_image_type(gif89_hdr,
                                           sizeof(gif89_hdr),
                                           nullptr,
                                           ext,
                                           sizeof(ext),
                                           mime,
                                           sizeof(mime)));
  TEST_ASSERT(strcmp(ext, "gif") == 0);
  TEST_ASSERT(strcmp(mime, "image/gif") == 0);

  /* 5. BMP magic bytes: ASCII BM. */
  static const uint8_t bmp_hdr[] = {'B', 'M', 0, 0, 0, 0};
  TEST_ASSERT(mdl_urlname_sniff_image_type(bmp_hdr,
                                           sizeof(bmp_hdr),
                                           nullptr,
                                           ext,
                                           sizeof(ext),
                                           mime,
                                           sizeof(mime)));
  TEST_ASSERT(strcmp(ext, "bmp") == 0);
  TEST_ASSERT(strcmp(mime, "image/bmp") == 0);

  /* 6. Fallback to Content-Type header when magic bytes are missing or unknown. */
  TEST_ASSERT(
    mdl_urlname_sniff_image_type(nullptr, 0, "image/jpeg", ext, sizeof(ext), mime, sizeof(mime)));
  TEST_ASSERT(strcmp(ext, "jpg") == 0);
  TEST_ASSERT(strcmp(mime, "image/jpeg") == 0);

  TEST_ASSERT(mdl_urlname_sniff_image_type(nullptr,
                                           0,
                                           "image/png; charset=utf-8",
                                           ext,
                                           sizeof(ext),
                                           mime,
                                           sizeof(mime)));
  TEST_ASSERT(strcmp(ext, "png") == 0);
  TEST_ASSERT(strcmp(mime, "image/png") == 0);

  TEST_ASSERT(
    mdl_urlname_sniff_image_type(nullptr, 0, "IMAGE/WEBP", ext, sizeof(ext), mime, sizeof(mime)));
  TEST_ASSERT(strcmp(ext, "webp") == 0);
  TEST_ASSERT(strcmp(mime, "image/webp") == 0);

  TEST_ASSERT(
    mdl_urlname_sniff_image_type(nullptr, 0, "image/gif", ext, sizeof(ext), mime, sizeof(mime)));
  TEST_ASSERT(strcmp(ext, "gif") == 0);
  TEST_ASSERT(strcmp(mime, "image/gif") == 0);

  TEST_ASSERT(
    mdl_urlname_sniff_image_type(nullptr, 0, "image/bmp", ext, sizeof(ext), mime, sizeof(mime)));
  TEST_ASSERT(strcmp(ext, "bmp") == 0);
  TEST_ASSERT(strcmp(mime, "image/bmp") == 0);

  /* 7. Priority: Magic bytes take precedence over Content-Type header. */
  TEST_ASSERT(mdl_urlname_sniff_image_type(png_hdr,
                                           sizeof(png_hdr),
                                           "image/jpeg",
                                           ext,
                                           sizeof(ext),
                                           mime,
                                           sizeof(mime)));
  TEST_ASSERT(strcmp(ext, "png") == 0);
  TEST_ASSERT(strcmp(mime, "image/png") == 0);

  /* 8. Unrecognized header / invalid content type. */
  static const uint8_t junk_hdr[] = {0x00, 0x00, 0x00, 0x00};
  TEST_ASSERT(!mdl_urlname_sniff_image_type(junk_hdr,
                                            sizeof(junk_hdr),
                                            "text/html",
                                            ext,
                                            sizeof(ext),
                                            mime,
                                            sizeof(mime)));

  TEST_END("image magic byte & Content-Type typing");
}
/** @test Export and miniz writer arenas fail closed and report deterministic high-water. */
static void test_export_workspace_bounds(void)
{
  TEST_BEGIN("export workspace bounds");
  enum { k_names_bytes = 2048U * 256U };
  const char* dir = "/tmp/mdl_ws_chap";
  const char* out = "/tmp/mdl_ws_chap.cbz";
  (void)mkdir(dir, (mode_t)k_mdl_test_dir_mode);
  write_fixture("/tmp/mdl_ws_chap/page_001.jpg", 'a');

  mdl_export_workspace_t ws;
  mdl_export_workspace_init(&ws, s_test_export_arena, k_names_bytes - 1U);
  TEST_ASSERT(mdl_export_chapter_ws(k_mdl_fmt_cbz, dir, out, &ws) == k_ra8_err_invalid_size);
  TEST_ASSERT(ws.high_water == 0U);

  mdl_export_workspace_init(&ws, s_test_export_arena, sizeof(s_test_export_arena));
  TEST_ASSERT(mdl_export_chapter_ws(k_mdl_fmt_cbz, dir, out, &ws) == k_ra8_ok);
  const size_t cbz_high_water = ws.high_water;
  TEST_ASSERT(cbz_high_water > k_names_bytes);
  TEST_ASSERT(cbz_high_water <= sizeof(s_test_export_arena));

  write_fixture(out, 'z');
  mdl_export_workspace_init(&ws, s_test_export_arena, cbz_high_water - 1U);
  TEST_ASSERT(mdl_export_chapter_ws(k_mdl_fmt_cbz, dir, out, &ws) == k_ra8_err_invalid_size);
  TEST_ASSERT(ws.high_water <= cbz_high_water - 1U);
  FILE* intact = fopen(out, "rb");
  TEST_ASSERT(intact != nullptr);
  for (size_t i = 0U; i < (size_t)k_fixture_bytes; ++i) {
    TEST_ASSERT(fgetc(intact) == 'z');
  }
  TEST_ASSERT(fgetc(intact) == EOF);
  (void)fclose(intact);

  const char* epub_out = "/tmp/mdl_ws_chap.epub";
  mdl_export_workspace_init(&ws, s_test_export_arena, sizeof(s_test_export_arena));
  TEST_ASSERT(mdl_export_chapter_ws(k_mdl_fmt_epub, dir, epub_out, &ws) == k_ra8_ok);
  const size_t epub_high_water = ws.high_water;
  TEST_ASSERT(epub_high_water > cbz_high_water);
  TEST_ASSERT(epub_high_water <= sizeof(s_test_export_arena));

  write_fixture(epub_out, 'e');
  mdl_export_workspace_init(&ws, s_test_export_arena, epub_high_water - 1U);
  TEST_ASSERT(mdl_export_chapter_ws(k_mdl_fmt_epub, dir, epub_out, &ws) == k_ra8_err_invalid_size);
  TEST_ASSERT(ws.high_water <= epub_high_water - 1U);
  intact = fopen(epub_out, "rb");
  TEST_ASSERT(intact != nullptr);
  for (size_t i = 0U; i < (size_t)k_fixture_bytes; ++i) {
    TEST_ASSERT(fgetc(intact) == 'e');
  }
  TEST_ASSERT(fgetc(intact) == EOF);
  (void)fclose(intact);

  (void)unlink("/tmp/mdl_ws_chap/page_001.jpg");
  (void)unlink(out);
  (void)unlink(epub_out);
  (void)rmdir(dir);
  TEST_END("export workspace bounds");
}

/** @test Every advertised validator rejects a truncated or wrong container. */
static void test_verify_rejects_truncation(void)
{
  TEST_BEGIN("verify rejects truncation");
  static const struct {
    const char*  path;
    mdl_format_t format;
  } cases[] = {{"/tmp/mdl_bad.cbz", k_mdl_fmt_cbz},
               {"/tmp/mdl_bad.cbt", k_mdl_fmt_cbt},
               {"/tmp/mdl_bad.cbt.gz", k_mdl_fmt_cbt_gz},
               {"/tmp/mdl_bad.epub", k_mdl_fmt_epub},
               {"/tmp/mdl_bad.jof", k_mdl_fmt_jof}};
  for (size_t i = 0U; i < sizeof(cases) / sizeof(cases[0]); ++i) {
    write_fixture(cases[i].path, 'x');
    mdl_verify_report_t report = {};
    TEST_ASSERT(verify_file(cases[i].format, cases[i].path, &report) != k_ra8_ok);
    (void)unlink(cases[i].path);
  }
  mdl_format_t inferred = k_mdl_fmt_invalid;
  TEST_ASSERT(mdl_format_from_path("/tmp/book.cbt.gz.part", &inferred) == k_ra8_err_not_supported);
  TEST_ASSERT(inferred == k_mdl_fmt_invalid);
  TEST_END("verify rejects truncation");
}

/**
 * @brief Run every media_dl unit test in sequence.
 * @return 0 when all tests passed, non-zero on the first failure.
 * @since 0.1.0
 */
int32_t main(void)
{
  test_format_mapping();
  test_extract_images();
  test_extract_anchors();
  test_config_load();
  test_export_cbz_roundtrip();
  test_export_workspace_bounds();
  test_export_cbt_structure();
  test_verify_rejects_truncation();
  test_export_skips_non_images();
  test_export_epub_roundtrip();
  test_export_jof_roundtrip();
  test_url_scheme();
  test_addr_classify();
  test_addr_fetchable();
  test_size_cap();
  test_url_parts();
  test_sanitize_segment();
  test_path_contained();
  test_path_join();
  test_xml_escape();
  test_tar_rejects_long_name();
  test_epub_escapes_name();
  test_epub_long_filenames();
  test_export_page_cap();
  test_robots_group_select();
  test_robots_allow_wins();
  test_robots_wildcard_anchor();
  test_robots_edge();
  test_robots_cache();
  test_cli_new_flags();
  test_cli_missing_value();
  test_cli_mode_matrix();
  test_cli_numeric_bounds();
  test_meta_init_parse_load();
  test_comicinfo_xml_generation();
  test_epub_metadata_and_uuid();
  test_external_cover_embedding();
  test_image_magic_bytes();
  (void)fprintf(stderr, "[OK  ] test_media_dl.c\n");
  return 0;
}
