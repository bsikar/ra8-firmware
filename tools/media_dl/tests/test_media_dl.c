/*
 * Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
/**
 * @file test_media_dl.c
 * @brief Host unit tests for the media_dl pure-logic units + export round-trip.
 *
 * @details
 * Exercises the network-independent surface of the downloader: the format-name
 * mapping, the `<img>`/`<a>` scanner and URL resolver, the site-descriptor
 * parser, and an end-to-end CBZ export re-opened with miniz. Uses the repo's
 * `unity_minimal.h` harness, mirroring `tests/test_*.c`.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "mdl_config.h"
#include "mdl_export.h"
#include "mdl_extract.h"
#include "miniz.h"
#include "ra8_jof.h"
#include "tiny_jpeg_fixture.h"
#include "unity_minimal.h"

/** @brief Expected fixture counts (named to avoid bare literals). */
typedef enum : uint16_t {
  k_expect_imgs      = 2,   /**< /uploads/ images in the page fixture.       */
  k_expect_chaps     = 3,   /**< chapter links in the series fixture.        */
  k_expect_pages     = 2,   /**< pages written into the export fixture.      */
  k_fixture_bytes    = 4,   /**< bytes per synthetic page file.              */
  k_name_probe       = 256, /**< zip entry-name probe buffer.                */
  k_epub_min_entries = 6,   /**< mimetype+container+opf+nav+pages, at least. */
} test_expect_t;

static mdl_url_list_t s_list;

/** @test Format-name mapping is exact and rejects junk. */
static void test_format_mapping(void)
{
  TEST_BEGIN("format mapping");
  TEST_ASSERT(mdl_format_from_str("cbz") == k_mdl_fmt_cbz);
  TEST_ASSERT(mdl_format_from_str("cbt.xz") == k_mdl_fmt_cbt_xz);
  TEST_ASSERT(mdl_format_from_str(nullptr) == k_mdl_fmt_loose);
  TEST_ASSERT(mdl_format_from_str("bogus") == k_mdl_fmt_invalid);
  TEST_ASSERT(strcmp(mdl_format_ext(k_mdl_fmt_cbz), "cbz") == 0);
  TEST_ASSERT(strcmp(mdl_format_ext(k_mdl_fmt_cbt_gz), "cbt.gz") == 0);
  TEST_ASSERT(mdl_format_from_str("epub") == k_mdl_fmt_epub);
  TEST_ASSERT(mdl_format_from_str("jof") == k_mdl_fmt_jof);
  TEST_ASSERT(mdl_format_from_str("rabook") == k_mdl_fmt_rabook);
  TEST_ASSERT(strcmp(mdl_format_ext(k_mdl_fmt_epub), "epub") == 0);
  TEST_ASSERT(strcmp(mdl_format_ext(k_mdl_fmt_jof), "jof") == 0);
  TEST_ASSERT(strcmp(mdl_format_ext(k_mdl_fmt_rabook), "rabook") == 0);
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

/** @test Export a folder to CBZ, then re-open it with the miniz reader. */
static void test_export_cbz_roundtrip(void)
{
  TEST_BEGIN("export cbz round-trip");
  const char* dir = "/tmp/mdl_test_chap";
  const char* out = "/tmp/mdl_test_chap.cbz";
  (void)mkdir(dir, 0755);
  write_fixture("/tmp/mdl_test_chap/page_001.jpg", 'a');
  write_fixture("/tmp/mdl_test_chap/page_002.jpg", 'b');

  const ra8_err_t rc = mdl_export_chapter(k_mdl_fmt_cbz, dir, out);
  TEST_ASSERT(rc == k_ra8_ok);

  mz_zip_archive zr;
  memset(&zr, 0, sizeof(zr));
  TEST_ASSERT(mz_zip_reader_init_file(&zr, out, 0) != MZ_FALSE);
  TEST_ASSERT_EQ(k_expect_pages, (uint16_t)mz_zip_reader_get_num_files(&zr));
  char name[k_name_probe];
  (void)mz_zip_reader_get_filename(&zr, 0, name, sizeof(name));
  TEST_ASSERT(strcmp(name, "page_001.jpg") == 0);
  (void)mz_zip_reader_end(&zr);

  (void)unlink("/tmp/mdl_test_chap/page_001.jpg");
  (void)unlink("/tmp/mdl_test_chap/page_002.jpg");
  (void)unlink(out);
  (void)rmdir(dir);
  TEST_END("export cbz round-trip");
}

/** @test Packaging ingests ONLY page images; sibling output/junk is skipped. */
static void test_export_skips_non_images(void)
{
  TEST_BEGIN("export skips non-images");
  const char* dir = "/tmp/mdl_mixed_chap";
  const char* out = "/tmp/mdl_mixed_chap.cbz";
  (void)mkdir(dir, 0755);
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
  TEST_ASSERT_EQ(k_expect_pages, (uint16_t)mz_zip_reader_get_num_files(&zr));
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
  (void)mkdir(dir, 0755);
  write_fixture("/tmp/mdl_epub_chap/page_001.jpg", 'a');
  write_fixture("/tmp/mdl_epub_chap/page_002.jpg", 'b');
  TEST_ASSERT(mdl_export_chapter(k_mdl_fmt_epub, dir, out) == k_ra8_ok);

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
  (void)mkdir(dir, 0755);
  write_bytes(jpg, k_tiny_jpeg, (size_t)k_tiny_jpeg_len);
  TEST_ASSERT(mdl_export_chapter(k_mdl_fmt_jof, dir, "unused") == k_ra8_ok);

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
  test_export_skips_non_images();
  test_export_epub_roundtrip();
  test_export_jof_roundtrip();
  (void)fprintf(stderr, "[OK  ] test_media_dl.c\n");
  return 0;
}
