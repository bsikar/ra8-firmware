/**
 * @file test_media_dl.c
 * @brief Host tests for downloader parsing, policy, and CLI behavior.
 *
 * @details Covers format mapping, HTML extraction, site configuration,
 * untrusted URL/path handling, robots policy, and strict CLI mode validation.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

#include "mdl_cli.h"
#include "mdl_config.h"
#include "mdl_export.h"
#include "mdl_extract.h"
#include "mdl_robots.h"
#include "mdl_sanitize.h"
#include "mdl_test_storage.h"
#include "mdl_url_guard.h"
#include "mdl_verify.h"
#include "ra8_io_stream_ram.h"
#include "test_media_dl_logic_cli_internal.h"
#include "unity_minimal.h"

/** @brief Expected parser fixture counts. */
typedef enum : uint16_t {
  k_expect_imgs  = 2, /**< Images retained from the page fixture. */
  k_expect_chaps = 3, /**< Chapter links retained by extraction.  */
} mdl_logic_expect_t;

/** @brief Named sizes and values for policy tests. */
typedef enum : uint16_t {
  k_crawl_5s_ms = 5000, /**< Five-second crawl delay in milliseconds.    */
  k_join_tiny   = 6,    /**< Deliberately undersized joined-path buffer. */
  k_buf_128     = 128,  /**< Small host and path buffer.                 */
  k_buf_256     = 256,  /**< Medium escaping buffer.                     */
  k_buf_512     = 512,  /**< Large configuration value buffer.           */
  k_buf_4k      = 4096, /**< robots.txt fetch scratch storage.           */
} mdl_logic_bound_t;

/** @brief Reused extraction output owned by this test translation unit. */
static mdl_url_list_t s_list;

/** @brief Create one fixture from at most two exact text fragments.
 * @details Implements this test-only seam with caller-owned fixtures, bounded storage, and explicit propagation of the result observed by its caller.
 * @param[in] path Filesystem path used by this fixture operation.
 * @param[in] first Optional first text fragment to write.
 * @param[in] second Optional second text fragment to write.
 * @return True when the helper condition succeeds; otherwise false.
 * @retval true The requested fixture condition succeeded.
 * @retval false The helper rejected or could not complete the condition.
 * @pre Pointer arguments satisfy their documented readable and writable extents.
 * @pre The caller retains ownership of every supplied fixture object.
 * @post Documented outputs reflect the processed fixture on success.
 * @post Failure preserves caller-owned resources as documented.
 * @note Host-only and synchronous; assertion failure terminates the test process.
 * @since 0.1.0
 */
RA8_INTERNAL static bool
internal_write_config(const char* path, const char* first, const char* second)
{
  const int descriptor = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
  if (descriptor < 0) {
    return false;
  }
  const char* fragments[] = {first, second};
  bool        ok          = true;
  for (size_t i = 0U; ok && (i < 2U); ++i) {
    if (fragments[i] == nullptr) {
      continue;
    }
    const size_t length = strlen(fragments[i]);
    size_t       offset = 0U;
    while (offset < length) {
      const ssize_t written = write(descriptor, &fragments[i][offset], length - offset);
      if (written > 0) {
        offset += (size_t)written;
      } else if ((written < 0) && (errno == EINTR)) {
        continue;
      } else {
        ok = false;
        break;
      }
    }
  }
  return (close(descriptor) == 0) && ok;
}

/**
 * @brief Fake robots.txt fetcher state for the cache-consult test.
 * @details Records the call count so a cache hit can be proven not to re-fetch.
 * @invariant `count` counts every fetch dispatched through ::internal_fake_fetch.
 * @since 0.1.0
 */
typedef struct {
  int                       count;  /**< Number of fetches performed.     */
  const char*               body;   /**< Canned robots.txt body, or NULL. */
  mdl_robots_fetch_result_t result; /**< Result the fetcher reports.      */
} fake_fetch_ctx_t;

/** @brief Injected robots.txt fetcher returning a canned body/result.
 * @details Implements this test-only seam with caller-owned fixtures, bounded storage, and explicit propagation of the result observed by its caller.
 * @param[in,out] ctx Opaque caller-owned fixture context.
 * @param[in] robots_url Validated robots resource URL requested by the caller.
 * @param[in,out] buf Caller-owned bounded byte buffer.
 * @param[in] cap Supplied capacity of the destination buffer, in bytes.
 * @param[out] out_len Receives the exact produced byte count on success.
 * @return Scripted robots fetch result for this fixture.
 * @retval k_mdl_robots_fetch_ok The canned response was produced.
 * @retval other The configured fetch rejection or failure result.
 * @pre Pointer arguments satisfy their documented readable and writable extents.
 * @pre The caller retains ownership of every supplied fixture object.
 * @post Documented outputs reflect the processed fixture on success.
 * @post Failure preserves caller-owned resources as documented.
 * @note Host-only and synchronous; assertion failure terminates the test process.
 * @since 0.1.0
 */
RA8_INTERNAL static mdl_robots_fetch_result_t
internal_fake_fetch(void* ctx, const char* robots_url, char* buf, size_t cap, size_t* out_len)
{
  fake_fetch_ctx_t* f = (fake_fetch_ctx_t*)ctx;
  (void)robots_url;
  f->count += 1;
  *out_len = 0U;
  if ((f->result == k_mdl_robots_fetch_ok) && (f->body != nullptr)) {
    const int w   = __builtin_snprintf(buf, cap, "%s", f->body);
    size_t    got = (w < 0) ? 0U : (size_t)w;
    if (got >= cap) {
      got = cap - 1U;
    }
    *out_len = got;
  }
  return f->result;
}

/** @test Format-name mapping is exact and rejects junk.
 * @brief Exercise the format mapping media-downloader scenario.
 * @details Exercises the format mapping scenario through production media-downloader interfaces and checks its observable success, rejection, and boundary results.
 * @pre The host test process exclusively owns its fixture state.
 * @pre Required fakes and bounded buffers are initialized before use.
 * @post Normal return means every scenario assertion passed.
 * @post No fixture resource ownership is transferred by this test.
 * @note Host-only and synchronous; assertion failure terminates the test process.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_format_mapping(void)
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
  TEST_ASSERT(mdl_format_from_path("/tmp/book.RABOOK", &inferred) == k_ra8_ok);
  TEST_ASSERT(inferred == k_mdl_fmt_rabook);
  TEST_ASSERT(mdl_format_from_path("/tmp/book.INCOMPLETE.cbt.gz", &inferred) == k_ra8_ok);
  TEST_ASSERT(inferred == k_mdl_fmt_cbt_gz);
  TEST_ASSERT(mdl_format_from_path("/tmp/book.zip", &inferred) == k_ra8_err_not_supported);
  TEST_ASSERT(inferred == k_mdl_fmt_invalid);
  TEST_ASSERT(strcmp(mdl_format_ext(k_mdl_fmt_cbz), "cbz") == 0);
  TEST_ASSERT(strcmp(mdl_format_ext(k_mdl_fmt_cbt_gz), "cbt.gz") == 0);
  TEST_ASSERT(mdl_format_from_str("epub") == k_mdl_fmt_epub);
  TEST_ASSERT(mdl_format_from_str("jof") == k_mdl_fmt_jof);
  TEST_ASSERT(mdl_format_from_str("rabook") == k_mdl_fmt_rabook);
  TEST_ASSERT(strcmp(mdl_format_ext(k_mdl_fmt_epub), "epub") == 0);
  TEST_ASSERT(strcmp(mdl_format_ext(k_mdl_fmt_jof), "jof") == 0);
  TEST_ASSERT(strcmp(mdl_format_ext(k_mdl_fmt_rabook), "rabook") == 0);
  TEST_ASSERT(mdl_format_is_verifiable(k_mdl_fmt_cbz));
  TEST_ASSERT(mdl_format_is_verifiable(k_mdl_fmt_cbt));
  TEST_ASSERT(mdl_format_is_verifiable(k_mdl_fmt_epub));
  TEST_ASSERT(mdl_format_is_verifiable(k_mdl_fmt_jof));
  TEST_ASSERT(mdl_format_is_verifiable(k_mdl_fmt_cbt_gz));
  TEST_ASSERT(mdl_format_is_verifiable(k_mdl_fmt_rabook));
  TEST_END("format mapping");
}

/** @test Image scanner prefers data-src, resolves URLs, applies the filter.
 * @brief Exercise the extract images media-downloader scenario.
 * @details Exercises the extract images scenario through production media-downloader interfaces and checks its observable success, rejection, and boundary results.
 * @pre The host test process exclusively owns its fixture state.
 * @pre Required fakes and bounded buffers are initialized before use.
 * @post Normal return means every scenario assertion passed.
 * @post No fixture resource ownership is transferred by this test.
 * @note Host-only and synchronous; assertion failure terminates the test process.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_extract_images(void)
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

/** @test Anchor scanner keeps only hrefs containing the marker.
 * @brief Exercise the extract anchors media-downloader scenario.
 * @details Exercises the extract anchors scenario through production media-downloader interfaces and checks its observable success, rejection, and boundary results.
 * @pre The host test process exclusively owns its fixture state.
 * @pre Required fakes and bounded buffers are initialized before use.
 * @post Normal return means every scenario assertion passed.
 * @post No fixture resource ownership is transferred by this test.
 * @note Host-only and synchronous; assertion failure terminates the test process.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_extract_anchors(void)
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

/** @test A flat key=value descriptor round-trips through the parser.
 * @brief Exercise the config load media-downloader scenario.
 * @details Exercises the config load scenario through production media-downloader interfaces and checks its observable success, rejection, and boundary results.
 * @pre The host test process exclusively owns its fixture state.
 * @pre Required fakes and bounded buffers are initialized before use.
 * @post Normal return means every scenario assertion passed.
 * @post No fixture resource ownership is transferred by this test.
 * @note Host-only and synchronous; assertion failure terminates the test process.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_config_load(void)
{
  TEST_BEGIN("config load");
  const char* path = "/tmp/mdl_test_site.conf";
  TEST_ASSERT(internal_write_config(path,
                                    "# comment\n[section-ignored]\nname = T\nhost = t.net\n"
                                    "chapter_url_contains = /chapter-\nchapter_order = asc\n"
                                    "page_img_attr = data-src\nimg_delay_min = 111\n",
                                    nullptr));

  mdl_site_t      site;
  const ra8_err_t rc = mdl_config_load(mdl_test_storage_get(), path, &site);
  TEST_ASSERT(rc == k_ra8_ok);
  TEST_ASSERT(strcmp(site.host, "t.net") == 0);
  TEST_ASSERT(strcmp(site.chapter_url_contains, "/chapter-") == 0);
  TEST_ASSERT(site.chapter_order == k_mdl_order_asc);
  TEST_ASSERT_EQ((uint16_t)111, (uint16_t)site.img_delay_min);

  TEST_ASSERT(internal_write_config(path, "host = t.net\nunknown_typo = accepted\n", nullptr));
  TEST_ASSERT(mdl_config_load(mdl_test_storage_get(), path, &site) == k_ra8_err_invalid_state);

  TEST_ASSERT(
    internal_write_config(path, "host = t.net\nseries_title_selector = meta:\n", nullptr));
  TEST_ASSERT(mdl_config_load(mdl_test_storage_get(), path, &site) == k_ra8_err_invalid_state);

  TEST_ASSERT(internal_write_config(path, "host = t.net\nburst = -1\n", nullptr));
  TEST_ASSERT(mdl_config_load(mdl_test_storage_get(), path, &site) == k_ra8_err_invalid_state);

  TEST_ASSERT(
    internal_write_config(path, "host = t.net\nseries_title_selector = css:.title\n", nullptr));
  TEST_ASSERT(mdl_config_load(mdl_test_storage_get(), path, &site) == k_ra8_err_invalid_state);

  char overlong[k_buf_512];
  memset(overlong, 'a', sizeof(overlong) - 1U);
  overlong[sizeof(overlong) - 1U] = '\0';
  TEST_ASSERT(internal_write_config(path, "host = t.net\nname = ", overlong));
  TEST_ASSERT(mdl_config_load(mdl_test_storage_get(), path, &site) == k_ra8_err_invalid_state);
  (void)unlink(path);
  TEST_END("config load");
}

/* ---- #299: libcurl-backend safety predicates (mdl_url_guard) ------------- */

/** @test Scheme allowlist accepts http/https and refuses everything else.
 * @brief Exercise the url scheme media-downloader scenario.
 * @details Exercises the url scheme scenario through production media-downloader interfaces and checks its observable success, rejection, and boundary results.
 * @pre The host test process exclusively owns its fixture state.
 * @pre Required fakes and bounded buffers are initialized before use.
 * @post Normal return means every scenario assertion passed.
 * @post No fixture resource ownership is transferred by this test.
 * @note Host-only and synchronous; assertion failure terminates the test process.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_url_scheme(void)
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

/** @test Address classify buckets loopback/private/link-local vs public.
 * @brief Exercise the addr classify media-downloader scenario.
 * @details Exercises the addr classify scenario through production media-downloader interfaces and checks its observable success, rejection, and boundary results.
 * @pre The host test process exclusively owns its fixture state.
 * @pre Required fakes and bounded buffers are initialized before use.
 * @post Normal return means every scenario assertion passed.
 * @post No fixture resource ownership is transferred by this test.
 * @note Host-only and synchronous; assertion failure terminates the test process.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_addr_classify(void)
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

/** @test Fetchability honours the private-space opt-in but never unknown.
 * @brief Exercise the addr fetchable media-downloader scenario.
 * @details Exercises the addr fetchable scenario through production media-downloader interfaces and checks its observable success, rejection, and boundary results.
 * @pre The host test process exclusively owns its fixture state.
 * @pre Required fakes and bounded buffers are initialized before use.
 * @post Normal return means every scenario assertion passed.
 * @post No fixture resource ownership is transferred by this test.
 * @note Host-only and synchronous; assertion failure terminates the test process.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_addr_fetchable(void)
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

/** @test Response-size cap is overflow-safe and honours 0 = unlimited.
 * @brief Exercise the size cap media-downloader scenario.
 * @details Exercises the size cap scenario through production media-downloader interfaces and checks its observable success, rejection, and boundary results.
 * @pre The host test process exclusively owns its fixture state.
 * @pre Required fakes and bounded buffers are initialized before use.
 * @post Normal return means every scenario assertion passed.
 * @post No fixture resource ownership is transferred by this test.
 * @note Host-only and synchronous; assertion failure terminates the test process.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_size_cap(void)
{
  TEST_BEGIN("size cap");
  TEST_ASSERT(!mdl_size_exceeds(0U, 100U, 0U));   /* cap 0 -> unlimited */
  TEST_ASSERT(!mdl_size_exceeds(90U, 10U, 100U)); /* exactly fits       */
  TEST_ASSERT(mdl_size_exceeds(90U, 11U, 100U));  /* one byte over      */
  TEST_ASSERT(mdl_size_exceeds(200U, 1U, 100U));  /* already over cap   */
  TEST_END("size cap");
}

/** @test Host/path extraction strips scheme, userinfo, port, and query.
 * @brief Exercise the url parts media-downloader scenario.
 * @details Exercises the url parts scenario through production media-downloader interfaces and checks its observable success, rejection, and boundary results.
 * @pre The host test process exclusively owns its fixture state.
 * @pre Required fakes and bounded buffers are initialized before use.
 * @post Normal return means every scenario assertion passed.
 * @post No fixture resource ownership is transferred by this test.
 * @note Host-only and synchronous; assertion failure terminates the test process.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_url_parts(void)
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

/** @test Segment sanitiser neutralises traversal but preserves a legal slug.
 * @brief Exercise the sanitize segment media-downloader scenario.
 * @details Exercises the sanitize segment scenario through production media-downloader interfaces and checks its observable success, rejection, and boundary results.
 * @pre The host test process exclusively owns its fixture state.
 * @pre Required fakes and bounded buffers are initialized before use.
 * @post Normal return means every scenario assertion passed.
 * @post No fixture resource ownership is transferred by this test.
 * @note Host-only and synchronous; assertion failure terminates the test process.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_sanitize_segment(void)
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

/** @test Path containment respects directory boundaries.
 * @brief Exercise the path contained media-downloader scenario.
 * @details Exercises the path contained scenario through production media-downloader interfaces and checks its observable success, rejection, and boundary results.
 * @pre The host test process exclusively owns its fixture state.
 * @pre Required fakes and bounded buffers are initialized before use.
 * @post Normal return means every scenario assertion passed.
 * @post No fixture resource ownership is transferred by this test.
 * @note Host-only and synchronous; assertion failure terminates the test process.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_path_contained(void)
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
 * @brief Exercise the path join media-downloader scenario.
 * @details Exercises the path join scenario through production media-downloader interfaces and checks its observable success, rejection, and boundary results.
 * @pre The host test process exclusively owns its fixture state.
 * @pre Required fakes and bounded buffers are initialized before use.
 * @post Normal return means every scenario assertion passed.
 * @post No fixture resource ownership is transferred by this test.
 * @note Host-only and synchronous; assertion failure terminates the test process.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_path_join(void)
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

/* ---- #302: robots.txt parser / matcher / cache (mdl_robots) ------------- */

/** @test Most-specific User-agent group wins; Crawl-delay + reason extracted.
 * @brief Exercise the robots group select media-downloader scenario.
 * @details Exercises the robots group select scenario through production media-downloader interfaces and checks its observable success, rejection, and boundary results.
 * @pre The host test process exclusively owns its fixture state.
 * @pre Required fakes and bounded buffers are initialized before use.
 * @post Normal return means every scenario assertion passed.
 * @post No fixture resource ownership is transferred by this test.
 * @note Host-only and synchronous; assertion failure terminates the test process.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_robots_group_select(void)
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

/** @test A longer Allow beats a shorter Disallow (RFC 9309 tie rule).
 * @brief Exercise the robots allow wins media-downloader scenario.
 * @details Exercises the robots allow wins scenario through production media-downloader interfaces and checks its observable success, rejection, and boundary results.
 * @pre The host test process exclusively owns its fixture state.
 * @pre Required fakes and bounded buffers are initialized before use.
 * @post Normal return means every scenario assertion passed.
 * @post No fixture resource ownership is transferred by this test.
 * @note Host-only and synchronous; assertion failure terminates the test process.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_robots_allow_wins(void)
{
  TEST_BEGIN("robots longest-match allow");
  static const char txt[] = "User-agent: *\nDisallow: /dir/\nAllow: /dir/ok\n";
  mdl_robots_t      r;
  mdl_robots_parse(txt, sizeof(txt) - 1U, "media_dl", &r);
  TEST_ASSERT(mdl_robots_allows(&r, "/dir/ok/page")); /* longer Allow wins     */
  TEST_ASSERT(!mdl_robots_allows(&r, "/dir/secret")); /* only Disallow matches */
  TEST_END("robots longest-match allow");
}

/** @test `*` glob and trailing `$` anchor behave per spec.
 * @brief Exercise the robots wildcard anchor media-downloader scenario.
 * @details Exercises the robots wildcard anchor scenario through production media-downloader interfaces and checks its observable success, rejection, and boundary results.
 * @pre The host test process exclusively owns its fixture state.
 * @pre Required fakes and bounded buffers are initialized before use.
 * @post Normal return means every scenario assertion passed.
 * @post No fixture resource ownership is transferred by this test.
 * @note Host-only and synchronous; assertion failure terminates the test process.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_robots_wildcard_anchor(void)
{
  TEST_BEGIN("robots wildcard + anchor");
  static const char txt[] = "User-agent: *\nDisallow: /*.pdf$\n";
  mdl_robots_t      r;
  mdl_robots_parse(txt, sizeof(txt) - 1U, "media_dl", &r);
  TEST_ASSERT(!mdl_robots_allows(&r, "/docs/a.pdf"));     /* matches to end    */
  TEST_ASSERT(mdl_robots_allows(&r, "/docs/a.pdf.html")); /* $ anchors the end */
  TEST_END("robots wildcard + anchor");
}

/** @test Empty, malformed, and no-matching-group inputs all allow everything.
 * @brief Exercise the robots edge media-downloader scenario.
 * @details Exercises the robots edge scenario through production media-downloader interfaces and checks its observable success, rejection, and boundary results.
 * @pre The host test process exclusively owns its fixture state.
 * @pre Required fakes and bounded buffers are initialized before use.
 * @post Normal return means every scenario assertion passed.
 * @post No fixture resource ownership is transferred by this test.
 * @note Host-only and synchronous; assertion failure terminates the test process.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_robots_edge(void)
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

/** @test Per-host cache fetches once, and applies the 5xx/absent conventions.
 * @brief Exercise the robots cache media-downloader scenario.
 * @details Exercises the robots cache scenario through production media-downloader interfaces and checks its observable success, rejection, and boundary results.
 * @pre The host test process exclusively owns its fixture state.
 * @pre Required fakes and bounded buffers are initialized before use.
 * @post Normal return means every scenario assertion passed.
 * @post No fixture resource ownership is transferred by this test.
 * @note Host-only and synchronous; assertion failure terminates the test process.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_robots_cache(void)
{
  TEST_BEGIN("robots cache");
  static mdl_robots_cache_t cache;
  memset(&cache, 0, sizeof(cache));
  char                scratch[k_buf_4k];
  fake_fetch_ctx_t    ok = {.count  = 0,
                            .body   = "User-agent: *\nDisallow: /x\n",
                            .result = k_mdl_robots_fetch_ok};
  const mdl_robots_t* r  = mdl_robots_cache_consult(&cache,
                                                    "https",
                                                    "site.net",
                                                    "media_dl",
                                                    internal_fake_fetch,
                                                    &ok,
                                                    scratch,
                                                    sizeof(scratch));
  TEST_ASSERT_NOT_NULL(r);
  TEST_ASSERT(!mdl_robots_allows(r, "/x/y"));
  TEST_ASSERT(mdl_robots_allows(r, "/z"));
  const mdl_robots_t* r2 = mdl_robots_cache_consult(&cache,
                                                    "https",
                                                    "site.net",
                                                    "media_dl",
                                                    internal_fake_fetch,
                                                    &ok,
                                                    scratch,
                                                    sizeof(scratch));
  TEST_ASSERT(r2 == r);                            /* cache hit: same entry */
  TEST_ASSERT_EQ((uint16_t)1, (uint16_t)ok.count); /* fetched exactly once  */

  fake_fetch_ctx_t    deny = {.count = 0, .body = nullptr, .result = k_mdl_robots_fetch_denied};
  const mdl_robots_t* rd   = mdl_robots_cache_consult(&cache,
                                                      "https",
                                                      "deny.net",
                                                      "media_dl",
                                                      internal_fake_fetch,
                                                      &deny,
                                                      scratch,
                                                      sizeof(scratch));
  TEST_ASSERT(rd == nullptr); /* 5xx -> disallow all */

  fake_fetch_ctx_t    gone = {.count = 0, .body = nullptr, .result = k_mdl_robots_fetch_absent};
  const mdl_robots_t* rg   = mdl_robots_cache_consult(&cache,
                                                      "https",
                                                      "gone.net",
                                                      "media_dl",
                                                      internal_fake_fetch,
                                                      &gone,
                                                      scratch,
                                                      sizeof(scratch));
  TEST_ASSERT_NOT_NULL(rg);
  TEST_ASSERT(mdl_robots_allows(rg, "/anything")); /* absent -> allow all */
  TEST_END("robots cache");
}

/**
 * @brief Run downloader logic tests.
 * @return Zero after every assertion passes.
 * @retval 0 Every logic test passed.
 * @pre Test fixtures are writable below `/tmp`.
 * @pre The unity-minimal assertion process is initialized.
 * @post Every invoked test completed.
 * @post Temporary configuration files are removed.
 * @note Runs serially in one process.
 * @since 0.1.0
 */
int main(void)
{
  TEST_ASSERT_EQ(k_ra8_ok, mdl_test_storage_init());
  internal_test_format_mapping();
  internal_test_extract_images();
  internal_test_extract_anchors();
  internal_test_config_load();
  internal_test_url_scheme();
  internal_test_addr_classify();
  internal_test_addr_fetchable();
  internal_test_size_cap();
  internal_test_url_parts();
  internal_test_sanitize_segment();
  internal_test_path_contained();
  internal_test_path_join();
  internal_test_robots_group_select();
  internal_test_robots_allow_wins();
  internal_test_robots_wildcard_anchor();
  internal_test_robots_edge();
  internal_test_robots_cache();
  priv_test_mdl_logic_cli_run();
  TEST_ASSERT_EQ(k_ra8_ok, mdl_test_storage_deinit());
  (void)write(STDERR_FILENO, "[OK  ] test_media_dl.c\n", sizeof("[OK  ] test_media_dl.c\n") - 1U);
  return 0;
}
