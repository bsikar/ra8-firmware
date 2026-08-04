/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
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
#include "mdl_export_internal.h"
#include "mdl_extract.h"
#include "mdl_robots.h"
#include "mdl_sanitize.h"
#include "mdl_url_guard.h"
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
  k_expect_imgs      = 2,   /**< /uploads/ images in the page fixture.       */
  k_expect_chaps     = 3,   /**< chapter links in the series fixture.        */
  k_expect_pages     = 2,   /**< pages written into the export fixture.      */
  k_fixture_bytes    = 4,   /**< bytes per synthetic page file.              */
  k_name_probe       = 256, /**< zip entry-name probe buffer.                */
  k_epub_min_entries = 6,   /**< mimetype+container+opf+nav+pages, at least. */
} test_expect_t;

/** @brief Named sizes/values for the hardening tests (no bare literals). */
typedef enum : uint16_t {
  k_crawl_5s_ms  = 5000, /**< `Crawl-delay: 5` expressed in milliseconds.   */
  k_longname_len = 120,  /**< Page-name length exceeding the ustar field.   */
  k_join_tiny    = 6,    /**< Buffer too small for a joined path (D4 test). */
  k_buf_128      = 128,  /**< Small host/path/name probe buffer.            */
  k_buf_256      = 256,  /**< Medium probe buffer.                          */
  k_buf_320      = 320,  /**< EPUB zip-entry-path probe buffer.             */
  k_buf_2k       = 2048, /**< EPUB escaped-name / href probe buffer.        */
  k_buf_4k       = 4096, /**< robots.txt fetch scratch buffer.              */
  k_long_amp_run = 240,  /**< '&' chars in the long-filename EPUB probe.    */
  k_amp_esc_len  = 5,    /**< strlen("&amp;"), the escape of one '&'.       */
} mdl_hard_const_t;

static mdl_url_list_t s_list;

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
  (void)mkdir(dir, (mode_t)k_mdl_test_dir_mode);
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
  (void)mkdir(dir, (mode_t)k_mdl_test_dir_mode);
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
  (void)mkdir(dir, (mode_t)k_mdl_test_dir_mode);
  write_bytes(jpg, k_tiny_jpeg, (size_t)k_tiny_jpeg_len);
  /* JOF is a directory-output format: it writes a `.jof` sibling per page into
   * the chapter dir and does not use out_path (no single container exists), so
   * the caller must report the directory, never a phantom archive file. */
  TEST_ASSERT(mdl_format_is_dir_output(k_mdl_fmt_jof));
  TEST_ASSERT(!mdl_format_is_dir_output(k_mdl_fmt_cbz));
  TEST_ASSERT(!mdl_format_is_dir_output(k_mdl_fmt_epub));
  TEST_ASSERT(mdl_export_chapter(k_mdl_fmt_jof, dir, dir) == k_ra8_ok);

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
  char path[k_buf_256];
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
  (void)fprintf(stderr, "[OK  ] test_media_dl.c\n");
  return 0;
}
