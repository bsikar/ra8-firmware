/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
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
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "mdl_extract.h"
#include "mdl_search.h"
#include "unity_minimal.h"

/** @brief Named fixture expectations (no bare literals). */
typedef enum : uint16_t {
  k_hits_expected    = 3,   /**< Deduplicated hits in the search fixture.  */
  k_anchors_expected = 5,   /**< Resolvable anchors in the search fixture. */
  k_zero_anchors     = 3,   /**< Anchors in the zero-result fixture.       */
  k_buf              = 256, /**< Probe buffer bytes.                       */
  k_tiny_buf         = 4,   /**< Deliberately too-small buffer.            */
} test_search_expect_t;

/** @brief Shared hit-list scratch (kept off the stack; ~98 KiB). */
static mdl_hit_list_t s_hits;

/** @brief A captured-style search-results page: cards, nav, and one external. */
static const char k_search_html[] =
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
static const char k_zero_html[] =
  "<html><body><nav>"
  "<a href='/'>Home</a><a href='/about/'>About</a><a href='/contact/'>Contact</a>"
  "</nav><p>No results found.</p></body></html>";

/** @brief A response whose markup carries no links at all (drift/blocked). */
static const char k_markup_html[] = "{\"error\":\"search temporarily unavailable\"}";

/**
 * @test The scanner extracts deduped title/URL pairs from a search page.
 *
 * @par MC/DC:
 * Decision `if ((html == nullptr) || (base_url == nullptr) || (out == nullptr))`
 * (3 conditions). Cited as tools/media_dl/src/mdl_extract.c@mdl_extract_hits.
 * - Vector 1: all non-NULL              -> false (control: the scan runs)
 * - Vector 2: html=NULL                 -> true  (varies condition 1)
 * - Vector 3: base_url=NULL             -> true  (varies condition 2)
 * - Vector 4: out=NULL                  -> true  (varies condition 3)
 * Vectors 1+2/1+3/1+4 prove each condition's independent influence; N+1 = 4
 * vectors for N=3 conditions: minimal MC/DC.
 */
static void test_hits_parse(void)
{
  TEST_BEGIN("search hits parse");
  const ra8_err_t rc = mdl_extract_hits(k_search_html,
                                        sizeof(k_search_html) - 1U,
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

/** @test A hit with neither text nor title attribute falls back to the slug. */
static void test_hits_slug_fallback(void)
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
 */
static void test_zero_vs_markup(void)
{
  TEST_BEGIN("search zero vs markup");
  /* Links present, none match: a genuine zero-result. */
  ra8_err_t rc =
    mdl_extract_hits(k_zero_html, sizeof(k_zero_html) - 1U, "https://s.net/", "/webtoon/", &s_hits);
  TEST_ASSERT(rc == k_ra8_ok);
  TEST_ASSERT_EQ((uint16_t)0, s_hits.count);
  TEST_ASSERT_EQ((uint16_t)k_zero_anchors, s_hits.anchors_seen);
  TEST_ASSERT(mdl_search_classify(&s_hits) == k_mdl_search_zero_results);
  /* No links at all: the markup changed or the request was blocked. */
  rc = mdl_extract_hits(k_markup_html,
                        sizeof(k_markup_html) - 1U,
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
 * Cited as tools/media_dl/src/mdl_search.c@is_unreserved.
 * - Vector 1: c='/'  -> false (control: none hold, so '/' is encoded to %2F)
 * - Vector 2: c='a'  -> true  (varies alpha)
 * - Vector 3: c='7'  -> true  (varies digit)
 * - Vector 4: c='-'  -> true  (varies mark)
 * The mixed-input strings below drive all four: unreserved bytes copy verbatim,
 * every other byte (space, &, #, +, and each UTF-8 byte) becomes %HH.
 */
static void test_query_encode(void)
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
 * tools/media_dl/src/mdl_search.c@mdl_search_build_url.
 * - Vector 1: all valid                     -> false (control: expansion runs)
 * - Vector 2: tmpl=NULL                      -> true  (varies condition 1)
 * - Vector 3: encoded_term=NULL              -> true  (varies condition 2)
 * - Vector 4: out=NULL                       -> true  (varies condition 3)
 * - Vector 5: cap=0                          -> true  (varies condition 4)
 * N+1 = 5 vectors for N=4 conditions: minimal MC/DC.
 */
static void test_build_url(void)
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
 * @brief Run every search/discovery unit test in sequence.
 * @return 0 when all tests passed, non-zero on the first failure.
 * @since 0.1.0
 */
int32_t main(void)
{
  test_hits_parse();
  test_hits_slug_fallback();
  test_zero_vs_markup();
  test_query_encode();
  test_build_url();
  (void)fprintf(stderr, "[OK  ] test_media_dl_search.c\n");
  return 0;
}
