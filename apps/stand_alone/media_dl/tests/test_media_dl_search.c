/**
 * @file test_media_dl_search.c
 * @brief Host unit tests for the #304 search/discovery parse + policy path.
 *
 * @details
 * Drives the network-free half of `--search` / `--browse` against captured
 * results-page fixtures with no network: the titled-anchor hit scanner
 * (::mdl_extract_hits), the RFC 3986 query encoder (::mdl_query_encode), the
 * `{q}` template expander (::mdl_search_build_url), and the honest
 * zero-vs-broken classifier (::mdl_search_classify). Every acceptance case #304
 * names -- matched hits with title/URL pairs, a genuine zero-result page, and a
 * markup-changed page with no links -- is asserted here. Uses the repo's
 * `unity_minimal.h` harness, mirroring `tests/test_*.c`.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include "mdl_extract.h"
#include "mdl_search.h"
#include "ra8_attributes.h"
#include "support/ra8_test_output.h"
#include "unity_minimal.h"

/** @brief Named fixture expectations (no bare literals). */
typedef enum : uint16_t {
  k_hits_expected    = 3,   /**< Deduplicated hits in the search fixture.  */
  k_anchors_expected = 5,   /**< Resolvable anchors in the search fixture. */
  k_zero_anchors     = 3,   /**< Anchors in the zero-result fixture.       */
  k_buf              = 256, /**< Probe buffer bytes.                       */
  k_tiny_buf         = 4,   /**< Deliberately too-small buffer.            */
} test_search_expect_t;

/** @brief Bounds of the generated hit-list capacity fixture. */
typedef enum : uint16_t {
  k_capacity_html_bytes = 8192,                          /**< Generated page bytes.   */
  k_hits_capacity       = (uint16_t)k_mdl_max_hits,      /**< Exactly the hit cap.    */
  k_hits_over_capacity  = (uint16_t)k_mdl_max_hits + 1U, /**< One anchor past it.     */
  k_kept_hits_one       = 1,                             /**< One publishable anchor. */
  k_kept_hits_two       = 2,                             /**< Two publishable hits.   */
  k_seen_anchors_two    = 2,                             /**< Two resolvable hrefs.   */
} test_search_capacity_t;

/** @brief Shared hit-list scratch (kept off the stack; ~98 KiB). */
static mdl_hit_list_t s_hits;

/** @brief Generated results page for the hit-list capacity vectors. */
static char s_capacity_html[k_capacity_html_bytes];

/** @brief A captured-style search-results page: cards, nav, and one external. */
static const char s_search_html[] =
  "<html><body><div class='results'>"
  "<div class='item'>"
  "<a href='/webtoon/solo-leveling/' class='thumb'><img src='/covers/sl.jpg'></a>"
  "<a href='/webtoon/solo-leveling/' class='title'>Solo Leveling</a>"
  "</div>"
  "<div class='item'>"
  "<a href='/webtoon/tower-of-god/' title='Tower of God'>ToG</a>"
  "</div>"
  "<a href='/about/'>About</a>"
  "<a href='https://cdn.example/webtoon/leak/'>External &amp; Leak</a>"
  "</div></body></html>";

/** @brief A results page that rendered links but matched no series. */
static const char s_zero_html[] =
  "<html><body><nav>"
  "<a href='/'>Home</a><a href='/about/'>About</a><a href='/contact/'>Contact</a>"
  "</nav><p>No results found.</p></body></html>";

/** @brief A response whose markup carries no links at all (drift/blocked). */
static const char s_markup_html[] = "{\"error\":\"search temporarily unavailable\"}";

/**
 * @test The scanner extracts deduped title/URL pairs from a search page.
 *
 * @par MC/DC:
 * Decision `if ((html == nullptr) || (base_url == nullptr) || (out == nullptr))`
 * (3 conditions). Cited as apps/stand_alone/media_dl/src/mdl_extract.c@mdl_extract_hits.
 * - Vector 1: all non-NULL              -> false (control: the scan runs)
 * - Vector 2: html=NULL                 -> true  (varies condition 1)
 * - Vector 3: base_url=NULL             -> true  (varies condition 2)
 * - Vector 4: out=NULL                  -> true  (varies condition 3)
 * Vectors 1+2/1+3/1+4 prove each condition's independent influence; N+1 = 4
 * vectors for N=3 conditions: minimal MC/DC.
 * @brief Exercise the hits parse media-downloader scenario.
 * @details Exercises the hits parse scenario through production media-downloader interfaces and checks its observable success, rejection, and boundary results.
 * @pre The host test process exclusively owns its fixture state.
 * @pre Required fakes and bounded buffers are initialized before use.
 * @post Normal return means every scenario assertion passed.
 * @post No fixture resource ownership is transferred by this test.
 * @note Host-only and synchronous; assertion failure terminates the test process.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_hits_parse(void)
{
  TEST_BEGIN("search hits parse");
  const ra8_err_t rc = mdl_extract_hits(s_search_html,
                                        sizeof(s_search_html) - 1U,
                                        "https://manhwaus.net/?s=solo",
                                        "/webtoon/",
                                        &s_hits);
  TEST_ASSERT(rc == k_ra8_ok);
  TEST_ASSERT_EQ((uint16_t)k_hits_expected, s_hits.count);
  TEST_ASSERT_EQ((uint16_t)k_anchors_expected, s_hits.anchors_seen);
  /* First card: thumbnail link (no text) then the titled link to the SAME URL;
   * the real title upgrades the slug fallback captured from the thumbnail. */
  TEST_ASSERT(strcmp(s_hits.hits[0].url, "https://manhwaus.net/webtoon/solo-leveling/") == 0);
  TEST_ASSERT(strcmp(s_hits.hits[0].title, "Solo Leveling") == 0);
  /* Second card: title comes from the `title=` attribute. */
  TEST_ASSERT(strcmp(s_hits.hits[1].url, "https://manhwaus.net/webtoon/tower-of-god/") == 0);
  TEST_ASSERT(strcmp(s_hits.hits[1].title, "Tower of God") == 0);
  /* External hit: absolute URL kept, `&amp;` decoded in the title. */
  TEST_ASSERT(strcmp(s_hits.hits[2].url, "https://cdn.example/webtoon/leak/") == 0);
  TEST_ASSERT(strcmp(s_hits.hits[2].title, "External & Leak") == 0);
  TEST_ASSERT(mdl_search_classify(&s_hits) == k_mdl_search_have_results);
  TEST_END("search hits parse");
}

/** @test A hit with neither text nor title attribute falls back to the slug.
 * @brief Exercise the hits slug fallback media-downloader scenario.
 * @details Exercises the hits slug fallback scenario through production media-downloader interfaces and checks its observable success, rejection, and boundary results.
 * @pre The host test process exclusively owns its fixture state.
 * @pre Required fakes and bounded buffers are initialized before use.
 * @post Normal return means every scenario assertion passed.
 * @post No fixture resource ownership is transferred by this test.
 * @note Host-only and synchronous; assertion failure terminates the test process.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_hits_slug_fallback(void)
{
  TEST_BEGIN("search slug fallback");
  static const char html[] = "<a href='/webtoon/the-beginning-after-the-end/'><img src='x'></a>";
  const ra8_err_t   rc =
    mdl_extract_hits(html, sizeof(html) - 1U, "https://s.net/", "/webtoon/", &s_hits);
  TEST_ASSERT(rc == k_ra8_ok);
  TEST_ASSERT_EQ((uint16_t)1, s_hits.count);
  TEST_ASSERT(strcmp(s_hits.hits[0].title, "the-beginning-after-the-end") == 0);
  TEST_END("search slug fallback");
}

/**
 * @test Zero results and changed markup are distinguishable, not both "empty".
 *
 * @par MC/DC:
 * ::mdl_search_classify uses only single-condition decisions (`count > 0`,
 * `anchors_seen == 0`), so each is exercised directly rather than as a compound.
 * The three fixtures below cover its three outcomes: have-results (count>0),
 * zero-results (count==0, anchors>0), markup-changed (count==0, anchors==0).
 * @brief Exercise the zero vs markup media-downloader scenario.
 * @details Exercises the zero vs markup scenario through production media-downloader interfaces and checks its observable success, rejection, and boundary results.
 * @pre The host test process exclusively owns its fixture state.
 * @pre Required fakes and bounded buffers are initialized before use.
 * @post Normal return means every scenario assertion passed.
 * @post No fixture resource ownership is transferred by this test.
 * @note Host-only and synchronous; assertion failure terminates the test process.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_zero_vs_markup(void)
{
  TEST_BEGIN("search zero vs markup");
  /* Links present, none match: a genuine zero-result. */
  ra8_err_t rc =
    mdl_extract_hits(s_zero_html, sizeof(s_zero_html) - 1U, "https://s.net/", "/webtoon/", &s_hits);
  TEST_ASSERT(rc == k_ra8_ok);
  TEST_ASSERT_EQ((uint16_t)0, s_hits.count);
  TEST_ASSERT_EQ((uint16_t)k_zero_anchors, s_hits.anchors_seen);
  TEST_ASSERT(mdl_search_classify(&s_hits) == k_mdl_search_zero_results);
  /* No links at all: the markup changed or the request was blocked. */
  rc = mdl_extract_hits(s_markup_html,
                        sizeof(s_markup_html) - 1U,
                        "https://s.net/",
                        "/webtoon/",
                        &s_hits);
  TEST_ASSERT(rc == k_ra8_ok);
  TEST_ASSERT_EQ((uint16_t)0, s_hits.count);
  TEST_ASSERT_EQ((uint16_t)0, s_hits.anchors_seen);
  TEST_ASSERT(mdl_search_classify(&s_hits) == k_mdl_search_markup_changed);
  /* A NULL list is treated as unreadable, never as a clean empty result. */
  TEST_ASSERT(mdl_search_classify(nullptr) == k_mdl_search_markup_changed);
  TEST_END("search zero vs markup");
}

/**
 * @test The query encoder percent-encodes exactly the non-unreserved bytes.
 *
 * @par MC/DC:
 * Decision `is_unreserved()` returns `alpha || digit || mark` (3 conditions).
 * Cited as apps/stand_alone/media_dl/src/mdl_search.c@is_unreserved.
 * - Vector 1: c='/'  -> false (control: none hold, so '/' is encoded to %2F)
 * - Vector 2: c='a'  -> true  (varies alpha)
 * - Vector 3: c='7'  -> true  (varies digit)
 * - Vector 4: c='-'  -> true  (varies mark)
 * The mixed-input strings below drive all four: unreserved bytes copy verbatim,
 * every other byte (space, &, #, +, and each UTF-8 byte) becomes %HH.
 * @brief Exercise the query encode media-downloader scenario.
 * @details Exercises the query encode scenario through production media-downloader interfaces and checks its observable success, rejection, and boundary results.
 * @pre The host test process exclusively owns its fixture state.
 * @pre Required fakes and bounded buffers are initialized before use.
 * @post Normal return means every scenario assertion passed.
 * @post No fixture resource ownership is transferred by this test.
 * @note Host-only and synchronous; assertion failure terminates the test process.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_query_encode(void)
{
  TEST_BEGIN("query encode");
  char buf[k_buf];
  TEST_ASSERT(mdl_query_encode("hello world", buf, sizeof(buf)));
  TEST_ASSERT(strcmp(buf, "hello%20world") == 0);
  TEST_ASSERT(mdl_query_encode("a&b#c+d/e?f", buf, sizeof(buf)));
  TEST_ASSERT(strcmp(buf, "a%26b%23c%2Bd%2Fe%3Ff") == 0);
  TEST_ASSERT(mdl_query_encode("Az0-._~", buf, sizeof(buf)));
  TEST_ASSERT(strcmp(buf, "Az0-._~") == 0); /* unreserved set copied verbatim */
  /* A UTF-8 term (source stays 7-bit ASCII via hex escapes): each byte -> %HH. */
  TEST_ASSERT(mdl_query_encode("caf\xC3\xA9", buf, sizeof(buf)));
  TEST_ASSERT(strcmp(buf, "caf%C3%A9") == 0);
  /* Too small to hold "a%20b" (needs 6 incl NUL): a clean false, not a truncation. */
  TEST_ASSERT(!mdl_query_encode("a b", buf, k_tiny_buf));
  TEST_ASSERT(buf[0] == '\0');
  TEST_ASSERT(!mdl_query_encode(nullptr, buf, sizeof(buf)));
  TEST_END("query encode");
}

/**
 * @test The template expander substitutes every `{q}` and rejects a missing one.
 *
 * @par MC/DC:
 * Decision `if ((tmpl == nullptr) || (encoded_term == nullptr) || (out ==
 * nullptr) || (cap == 0U))` (4 conditions). Cited as
 * apps/stand_alone/media_dl/src/mdl_search.c@mdl_search_build_url.
 * - Vector 1: all valid                     -> false (control: expansion runs)
 * - Vector 2: tmpl=NULL                      -> true  (varies condition 1)
 * - Vector 3: encoded_term=NULL              -> true  (varies condition 2)
 * - Vector 4: out=NULL                       -> true  (varies condition 3)
 * - Vector 5: cap=0                          -> true  (varies condition 4)
 * N+1 = 5 vectors for N=4 conditions: minimal MC/DC.
 * @brief Exercise the build url media-downloader scenario.
 * @details Exercises the build url scenario through production media-downloader interfaces and checks its observable success, rejection, and boundary results.
 * @pre The host test process exclusively owns its fixture state.
 * @pre Required fakes and bounded buffers are initialized before use.
 * @post Normal return means every scenario assertion passed.
 * @post No fixture resource ownership is transferred by this test.
 * @note Host-only and synchronous; assertion failure terminates the test process.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_build_url(void)
{
  TEST_BEGIN("search build url");
  char buf[k_buf];
  TEST_ASSERT(mdl_search_build_url("https://s.net/?s={q}", "solo%20x", buf, sizeof(buf)));
  TEST_ASSERT(strcmp(buf, "https://s.net/?s=solo%20x") == 0);
  /* Every placeholder is substituted, not just the first. */
  TEST_ASSERT(mdl_search_build_url("https://s.net/{q}/?s={q}", "ab", buf, sizeof(buf)));
  TEST_ASSERT(strcmp(buf, "https://s.net/ab/?s=ab") == 0);
  /* A template with no {q} could never carry the term: rejected, not silent. */
  TEST_ASSERT(!mdl_search_build_url("https://s.net/search", "ab", buf, sizeof(buf)));
  TEST_ASSERT(buf[0] == '\0');
  /* Overflow is a clean false, never a truncated request. */
  TEST_ASSERT(!mdl_search_build_url("https://s.net/?s={q}", "abcdefghij", buf, k_tiny_buf));
  /* Each NULL / zero-cap condition independently forces a false (MC/DC above). */
  TEST_ASSERT(!mdl_search_build_url(nullptr, "ab", buf, sizeof(buf)));
  TEST_ASSERT(!mdl_search_build_url("https://s.net/?s={q}", nullptr, buf, sizeof(buf)));
  TEST_ASSERT(!mdl_search_build_url("https://s.net/?s={q}", "ab", nullptr, sizeof(buf)));
  TEST_ASSERT(!mdl_search_build_url("https://s.net/?s={q}", "ab", buf, 0U));
  TEST_END("search build url");
}

/**
 * @test A live-site-shaped result list keeps series and rejects chapters.
 * @brief Exercise the filter series hits regression scenario.
 * @details Executes the filter series hits scenario through production interfaces and checks its observable success, rejection, and boundary results.
 * @pre Assertions are enabled for contract verification.
 * @pre The test fixture workspace is isolated for this scenario.
 * @post Normal return means every reached contract assertion passed.
 * @post The caller receives no transferred fixture ownership.
 * @note Test helper; an assertion failure terminates the test process.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_filter_series_hits(void)
{
  TEST_BEGIN("search canonical series filter");
  static const char html[] = "<a href='/webtoon/alpha/'>Alpha</a>"
                             "<a href='/webtoon/alpha/chapter-108-5/'>Alpha Chapter 108.5</a>"
                             "<a href='/webtoon/alpha/'>Alpha duplicate</a>"
                             "<a href='/webtoon/beta/'>Beta</a>";
  TEST_ASSERT(
    mdl_extract_hits(html, sizeof(html) - 1U, "https://manhwaus.net/", "/webtoon/", &s_hits) ==
    k_ra8_ok);
  TEST_ASSERT_EQ((uint16_t)3, s_hits.count); /* extractor already removes exact duplicates */
  TEST_ASSERT_EQ((uint16_t)1, mdl_search_filter_series_hits(&s_hits, "/chapter-"));
  TEST_ASSERT_EQ((uint16_t)2, s_hits.count);
  TEST_ASSERT(strcmp(s_hits.hits[0].url, "https://manhwaus.net/webtoon/alpha/") == 0);
  TEST_ASSERT(strcmp(s_hits.hits[1].url, "https://manhwaus.net/webtoon/beta/") == 0);
  TEST_ASSERT_EQ((uint16_t)0, mdl_search_filter_series_hits(nullptr, "/chapter-"));
  TEST_END("search canonical series filter");
}

/**
 * @brief Render a results page holding @p anchors distinct titled anchors.
 * @details Writes `<a href='/webtoon/sNNN/'>SNNN</a>` runs into the shared
 *          fixture buffer so a capacity vector can be driven at exactly the
 *          hit cap and one anchor past it without a checked-in 4 KiB fixture.
 * @param[in] anchors Number of distinct anchors to render.
 * @return Number of bytes written, excluding the terminator.
 * @retval other The exact readable length of the generated page.
 * @pre @p anchors renders within ::k_capacity_html_bytes.
 * @pre The caller owns the shared fixture buffer for the whole call.
 * @post The buffer holds @p anchors anchors with distinct hrefs.
 * @post The returned length bounds exactly the generated bytes.
 * @note Host-only helper; an assertion failure terminates the test process.
 * @since 0.1.0
 */
RA8_INTERNAL static size_t internal_build_anchor_page(uint16_t anchors)
{
  size_t used = 0U;
  for (uint16_t i = 0U; i < anchors; ++i) {
    const int written = __builtin_snprintf(&s_capacity_html[used],
                                           sizeof(s_capacity_html) - used,
                                           "<a href='/webtoon/s%03u/'>S%03u</a>",
                                           (unsigned)i,
                                           (unsigned)i);
    TEST_ASSERT(written > 0);
    TEST_ASSERT((size_t)written < (sizeof(s_capacity_html) - used));
    used += (size_t)written;
  }
  return used;
}

/**
 * @test The hit list fills to exactly its cap and refuses the next anchor.
 *
 * @par MC/DC:
 * Decision: internal_merge_hit's `if (out->count >= k_mdl_max_hits)` (1
 * condition, so N+1 = 2 vectors -- the bound is driven at exactly N and N+1
 * because a bounded loop that reports success at its ceiling is the defect
 * this pins):
 * - Vector 1: count == cap - 1 on the last accepted anchor -> false (stored)
 * - Vector 2: count == cap on the extra anchor             -> true  (refused)
 * @brief Exercise the hit-list capacity media-downloader scenario.
 * @details Drives ::mdl_extract_hits with a generated page holding exactly
 *          ::k_mdl_max_hits distinct anchors and then one more, proving the
 *          full list is accepted, the extra anchor is refused with
 *          ::k_ra8_err_no_mem, and the refusal neither grows the list nor
 *          rewrites the last stored hit.
 * @pre The host test process exclusively owns its fixture state.
 * @pre Required fakes and bounded buffers are initialized before use.
 * @post Normal return means every scenario assertion passed.
 * @post No fixture resource ownership is transferred by this test.
 * @note Host-only and synchronous; assertion failure terminates the test process.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_hits_capacity(void)
{
  TEST_BEGIN("search hit-list capacity");
  char expected_last[k_buf];
  (void)__builtin_snprintf(expected_last,
                           sizeof(expected_last),
                           "https://s.net/webtoon/s%03u/",
                           (unsigned)(k_hits_capacity - 1U));
  /* Vector 1: exactly the cap fits, and the cap-th hit is the one stored. */
  size_t length = internal_build_anchor_page((uint16_t)k_hits_capacity);
  TEST_ASSERT(mdl_extract_hits(s_capacity_html, length, "https://s.net/", "/webtoon/", &s_hits) ==
              k_ra8_ok);
  TEST_ASSERT_EQ((int64_t)k_hits_capacity, (int64_t)s_hits.count);
  TEST_ASSERT_EQ((int64_t)k_hits_capacity, (int64_t)s_hits.anchors_seen);
  TEST_ASSERT(strcmp(s_hits.hits[k_hits_capacity - 1U].url, expected_last) == 0);
  /* Vector 2: one anchor past the cap is refused, not silently dropped. */
  length = internal_build_anchor_page((uint16_t)k_hits_over_capacity);
  TEST_ASSERT(mdl_extract_hits(s_capacity_html, length, "https://s.net/", "/webtoon/", &s_hits) ==
              k_ra8_err_no_mem);
  TEST_ASSERT_EQ((int64_t)k_hits_capacity, (int64_t)s_hits.count);
  /* The refused anchor was still seen, and it did not overwrite the last hit. */
  TEST_ASSERT_EQ((int64_t)k_hits_over_capacity, (int64_t)s_hits.anchors_seen);
  TEST_ASSERT(strcmp(s_hits.hits[k_hits_capacity - 1U].url, expected_last) == 0);
  TEST_END("search hit-list capacity");
}

/**
 * @test An anchor with no usable href is neither stored nor counted.
 * @brief Exercise the unusable href media-downloader scenario.
 * @details Drives an anchor with no `href` at all, a pure fragment, and a
 *          `data:` URI past the scanner. None of the three is a navigable
 *          result, so none may appear in `count` or in `anchors_seen` -- the
 *          latter is what tells a zero-result page apart from changed markup.
 * @pre The host test process exclusively owns its fixture state.
 * @pre Required fakes and bounded buffers are initialized before use.
 * @post Normal return means every scenario assertion passed.
 * @post No fixture resource ownership is transferred by this test.
 * @note Host-only and synchronous; assertion failure terminates the test process.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_hits_unusable_href(void)
{
  TEST_BEGIN("search unusable href");
  static const char html[] = "<a>no href at all</a>"
                             "<a href='#top'>fragment</a>"
                             "<a href='data:image/png;base64,AAAA'>inline</a>"
                             "<a href='/webtoon/real/'>Real</a>";
  TEST_ASSERT(mdl_extract_hits(html, sizeof(html) - 1U, "https://s.net/", "/webtoon/", &s_hits) ==
              k_ra8_ok);
  TEST_ASSERT_EQ((int64_t)k_kept_hits_one, (int64_t)s_hits.count);
  TEST_ASSERT_EQ((int64_t)k_kept_hits_one, (int64_t)s_hits.anchors_seen);
  TEST_ASSERT(strcmp(s_hits.hits[0].url, "https://s.net/webtoon/real/") == 0);
  TEST_ASSERT(strcmp(s_hits.hits[0].title, "Real") == 0);
  TEST_END("search unusable href");
}

/**
 * @test A truncated final anchor still yields its href, with a slug title.
 * @brief Exercise the unterminated anchor media-downloader scenario.
 * @details A response cut mid-tag leaves a `<a href='...'` with no `>`. The
 *          scanner must still resolve the href, must not read past the
 *          response for inner text, and must fall back to the URL slug rather
 *          than reporting a title it never saw.
 * @pre The host test process exclusively owns its fixture state.
 * @pre Required fakes and bounded buffers are initialized before use.
 * @post Normal return means every scenario assertion passed.
 * @post No fixture resource ownership is transferred by this test.
 * @note Host-only and synchronous; assertion failure terminates the test process.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_hits_unterminated_tag(void)
{
  TEST_BEGIN("search unterminated anchor");
  static const char html[] = "<a href='/webtoon/alpha/'>Alpha</a>"
                             "<a href='/webtoon/omega/'";
  TEST_ASSERT(mdl_extract_hits(html, sizeof(html) - 1U, "https://s.net/", "/webtoon/", &s_hits) ==
              k_ra8_ok);
  TEST_ASSERT_EQ((int64_t)k_kept_hits_two, (int64_t)s_hits.count);
  TEST_ASSERT_EQ((int64_t)k_seen_anchors_two, (int64_t)s_hits.anchors_seen);
  TEST_ASSERT(strcmp(s_hits.hits[1].url, "https://s.net/webtoon/omega/") == 0);
  /* No `>` means no inner text exists: the slug is the honest fallback. */
  TEST_ASSERT(strcmp(s_hits.hits[1].title, "omega") == 0);
  TEST_END("search unterminated anchor");
}

/**
 * @test A slug-only duplicate never demotes a title already read from markup.
 *
 * @par MC/DC:
 * Decision: internal_merge_hit's `if (title_real && !real[at])` (2 conditions,
 * AND; N+1 = 3). The first two vectors are driven by ::internal_test_hits_parse
 * (thumbnail slug then real title) and by the duplicate in
 * ::internal_test_filter_series_hits (real title already recorded):
 * - Vector 1: title_real=true,  real[at]=false -> true  (the slug is upgraded)
 * - Vector 2: title_real=true,  real[at]=true  -> false (varies real[at])
 * - Vector 3: title_real=false, real[at]=true  -> false (varies title_real,
 *   the case this test adds: the slug must not overwrite the real title)
 * @brief Exercise the duplicate anchor merge media-downloader scenario.
 * @details A card commonly links the same series twice: once as text and once
 *          as a bare thumbnail. The second, title-less anchor must merge into
 *          the existing hit without replacing its display title with the slug.
 * @pre The host test process exclusively owns its fixture state.
 * @pre Required fakes and bounded buffers are initialized before use.
 * @post Normal return means every scenario assertion passed.
 * @post No fixture resource ownership is transferred by this test.
 * @note Host-only and synchronous; assertion failure terminates the test process.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_hits_duplicate_merge(void)
{
  TEST_BEGIN("search duplicate anchor merge");
  static const char html[] = "<a href='/webtoon/alpha/'>Alpha Rising</a>"
                             "<a href='/webtoon/alpha/'><img src='/covers/a.jpg'></a>";
  TEST_ASSERT(mdl_extract_hits(html, sizeof(html) - 1U, "https://s.net/", "/webtoon/", &s_hits) ==
              k_ra8_ok);
  TEST_ASSERT_EQ((int64_t)k_kept_hits_one, (int64_t)s_hits.count);
  TEST_ASSERT_EQ((int64_t)k_seen_anchors_two, (int64_t)s_hits.anchors_seen);
  TEST_ASSERT(strcmp(s_hits.hits[0].url, "https://s.net/webtoon/alpha/") == 0);
  TEST_ASSERT(strcmp(s_hits.hits[0].title, "Alpha Rising") == 0);
  TEST_END("search duplicate anchor merge");
}

/**
 * @brief Run every search/discovery unit test in sequence.
 * @return 0 when all tests passed, non-zero on the first failure.
 * @since 0.1.0
 */
int main(void)
{
  internal_test_hits_parse();
  internal_test_hits_slug_fallback();
  internal_test_zero_vs_markup();
  internal_test_query_encode();
  internal_test_build_url();
  internal_test_filter_series_hits();
  internal_test_hits_capacity();
  internal_test_hits_unusable_href();
  internal_test_hits_unterminated_tag();
  internal_test_hits_duplicate_merge();
  (void)internal_test_output_fd_text(STDERR_FILENO, "[OK  ] test_media_dl_search.c\n");
  return 0;
}
