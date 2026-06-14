/**
 * @file test_ra_reflow_corpus.c
 * @brief Content corpus + live-layout pagination-cache acceptance (#79).
 *
 * @details
 * Closes the issue #79 acceptance for the import-time pagination cache:
 * "corpus fixtures run via `make test`; cache produces identical pages to
 * live layout; config change invalidates correctly."
 *
 * Unlike `tests/test_ra_reflow_cache.c` (which round-trips a *synthetic*
 * glyph array to unit-test the codec), this test drives the **real**
 * pipeline: it loads the bundled ArnoPro Latin-1 face, runs
 * `ra_reflow_layout_chapter()` over a corpus of representative XHTML
 * chapters (exercising the full v1 tag subset) plus deliberately
 * malformed inputs, then proves that
 * `ra_reflow_cache_serialize()` -> `ra_reflow_cache_load()` into a fresh
 * engine reproduces the live layout **glyph-for-glyph and page-for-page**,
 * and that bumping the font size invalidates the cached blob.
 *
 * The font is located relative to `__FILE__` (mirrors
 * `tests/test_ra_fs_font_reflow.c`); if it is missing the test SKIPs
 * rather than failing, so the suite stays green on a bare checkout.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ra_err.h"
#include "ra_reflow.h"
#include "ra_reflow_cache.h"
#include "unity_minimal.h"

typedef enum : uint16_t {
  k_c_vp_w     = 600U, /**< Corpus viewport width.  */
  k_c_vp_h     = 800U, /**< Corpus viewport height. */
  k_c_font_px  = 18U,  /**< Baseline body font.     */
  k_c_font_px2 = 24U,  /**< Changed body font.      */
} c_dim_t;

typedef enum : uint32_t {
  k_c_body_color = 0x202020FFU, /**< Body colour key. */
  k_c_link_color = 0x0050C0FFU, /**< Link colour key. */
} c_color_t;

typedef enum : size_t {
  k_c_font_cap = 2U * 1024U * 1024U, /**< ArnoPro subset < 2 MiB. */
  k_c_blob_cap = 1U << 20U,          /**< 1 MiB serialisation buffer. */
  k_c_path_cap = 1024U,              /**< Font path buffer.           */
} c_size_t;

static uint8_t           s_font[k_c_font_cap];
static size_t            s_font_len;
static uint8_t           s_blob[k_c_blob_cap];
static ra_reflow_t       s_live;
static ra_reflow_t       s_restored;
static ra_reflow_glyph_t s_glyph_snap[k_ra_reflow_max_glyphs];
static ra_reflow_page_t  s_page_snap[k_ra_reflow_max_pages];

/** @brief Well-formed chapters spanning the v1 HTML subset. */
static const char* const s_corpus[] = {
  "<h1>The Time Machine</h1><p>The Time Traveller (for so it will be "
  "convenient to speak of him) was expounding a <em>recondite</em> matter "
  "to us.</p><p>His pale grey eyes shone and twinkled.</p>",
  "<h2>A List</h2><ul><li>First item</li><li>Second &amp; third</li>"
  "<li>Fourth</li></ul><blockquote>Quoted text that wraps across more "
  "than a single line to force a line break in the engine.</blockquote>",
  "<p>Mixed <strong>bold</strong>, <i>italic</i>, and "
  "<a href=\"x\">a link</a> with a forced<br/>break and a rule.</p><hr/>"
  "<h3>Heading three</h3><p>Trailing paragraph.</p>",
};

/** @brief Malformed / degenerate inputs that must not crash the engine. */
static const char* const s_malformed[] = {
  "<p>unterminated paragraph with <em>no close",
  "&badentity; <<>> <p></p> &#xZZ;",
  "\x01\x02\x03 raw bytes and a <broken tag",
  "<p></p>",
};

/**
 * @brief Load the bundled ArnoPro subset relative to this source file.
 * @return 1 on success, 0 if the font is absent (caller SKIPs).
 */
static int load_font(void)
{
  char         path[k_c_path_cap];
  const char*  here = __FILE__;
  const size_t flen = strlen(here);
  size_t       base = flen;
  while (base > 0U && here[base - 1U] != '/') {
    base--; /* drop "test_ra_reflow_corpus.c" */
  }
  while (base > 0U && here[base - 1U] == '/') {
    base--;
  }
  while (base > 0U && here[base - 1U] != '/') {
    base--; /* drop "tests" -> repo root */
  }
  if (base == 0U || base >= sizeof(path)) {
    return 0;
  }
  memcpy(path, here, base);
  (void)snprintf(&path[base], sizeof(path) - base, "libs/fonts/arnopro_latin1.otf");
  FILE* fp = fopen(path, "rb");
  if (fp == nullptr) {
    return 0;
  }
  s_font_len = fread(s_font, 1U, (size_t)k_c_font_cap, fp);
  (void)fclose(fp);
  return (s_font_len > 0U) ? 1 : 0;
}

/** @brief Lay out @p xhtml live into s_live; return the layout result. */
static ra_err_t layout_live(const char* xhtml, uint16_t font_px, uint32_t* pages)
{
  ra_err_t err = ra_reflow_init((uint16_t)k_c_vp_w,
                                (uint16_t)k_c_vp_h,
                                s_font,
                                s_font_len,
                                font_px,
                                (uint32_t)k_c_body_color,
                                (uint32_t)k_c_link_color,
                                &s_live);
  TEST_ASSERT_EQ(k_ra_ok, err);
  return ra_reflow_layout_chapter(&s_live, (const uint8_t*)xhtml, strlen(xhtml), pages);
}

/** @brief Assert the cache restores s_live's layout glyph/page-identically. */
static void assert_cache_identical(const char* xhtml)
{
  const uint32_t gc = s_live.glyph_count;
  const uint32_t pc = s_live.page_count;
  memcpy(s_glyph_snap, s_live.glyphs, (size_t)gc * sizeof(s_live.glyphs[0]));
  memcpy(s_page_snap, s_live.pages, (size_t)pc * sizeof(s_live.pages[0]));

  size_t n = 0U;
  TEST_ASSERT_EQ(k_ra_ok,
                 ra_reflow_cache_serialize(&s_live,
                                           (const uint8_t*)xhtml,
                                           strlen(xhtml),
                                           s_blob,
                                           (size_t)k_c_blob_cap,
                                           &n));

  /* Fresh engine, same key -> load must reproduce the live layout. */
  TEST_ASSERT_EQ(k_ra_ok,
                 ra_reflow_init((uint16_t)k_c_vp_w,
                                (uint16_t)k_c_vp_h,
                                s_font,
                                s_font_len,
                                (uint16_t)k_c_font_px,
                                (uint32_t)k_c_body_color,
                                (uint32_t)k_c_link_color,
                                &s_restored));
  TEST_ASSERT_EQ(
    k_ra_ok,
    ra_reflow_cache_load(&s_restored, (const uint8_t*)xhtml, strlen(xhtml), s_blob, n));
  TEST_ASSERT_EQ(gc, s_restored.glyph_count);
  TEST_ASSERT_EQ(pc, s_restored.page_count);
  TEST_ASSERT_EQ(0, memcmp(s_glyph_snap, s_restored.glyphs, (size_t)gc * sizeof(s_glyph_snap[0])));
  TEST_ASSERT_EQ(0, memcmp(s_page_snap, s_restored.pages, (size_t)pc * sizeof(s_page_snap[0])));
}

/**
 * @test test_corpus_live_cache_identity
 * @brief Every well-formed chapter lays out, then the cache restores it
 *        glyph-for-glyph and page-for-page into a fresh engine.
 *
 * @par MC/DC:
 * (no compound decisions in this test -- it exercises the live layout ->
 * cache -> restore acceptance path; cache guards have `test_mcdc_*`
 * vectors in tests/test_ra_reflow_cache.c.)
 */
static void test_corpus_live_cache_identity(void)
{
  TEST_BEGIN("reflow_corpus: cache == live layout");
  if (load_font() == 0) {
    (void)fprintf(stderr, "[SKIP] arnopro_latin1.otf not found; skipping corpus\n");
    TEST_END("reflow_corpus: cache == live layout");
    return;
  }
  for (size_t i = 0U; i < (sizeof(s_corpus) / sizeof(s_corpus[0])); ++i) {
    uint32_t pages = 0U;
    TEST_ASSERT_EQ(k_ra_ok, layout_live(s_corpus[i], (uint16_t)k_c_font_px, &pages));
    TEST_ASSERT(pages >= 1U);
    TEST_ASSERT(s_live.glyph_count > 0U);
    assert_cache_identical(s_corpus[i]);
  }
  TEST_END("reflow_corpus: cache == live layout");
}

/**
 * @test test_corpus_font_size_invalidates
 * @brief A blob baked at one font size is rejected (stale) by an engine
 *        at a different size -- config change invalidates correctly.
 *
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the invalidation
 * acceptance path; cache guards have `test_mcdc_*` vectors in
 * tests/test_ra_reflow_cache.c.)
 */
static void test_corpus_font_size_invalidates(void)
{
  TEST_BEGIN("reflow_corpus: font-size change invalidates");
  if (load_font() == 0) {
    (void)fprintf(stderr, "[SKIP] arnopro_latin1.otf not found; skipping\n");
    TEST_END("reflow_corpus: font-size change invalidates");
    return;
  }
  uint32_t pages = 0U;
  TEST_ASSERT_EQ(k_ra_ok, layout_live(s_corpus[0], (uint16_t)k_c_font_px, &pages));
  size_t n = 0U;
  TEST_ASSERT_EQ(k_ra_ok,
                 ra_reflow_cache_serialize(&s_live,
                                           (const uint8_t*)s_corpus[0],
                                           strlen(s_corpus[0]),
                                           s_blob,
                                           (size_t)k_c_blob_cap,
                                           &n));
  /* Same content + font blob, different font size -> stale. */
  TEST_ASSERT_EQ(k_ra_ok, layout_live(s_corpus[0], (uint16_t)k_c_font_px2, &pages));
  TEST_ASSERT_EQ(
    k_ra_err_invalid_state,
    ra_reflow_cache_load(&s_live, (const uint8_t*)s_corpus[0], strlen(s_corpus[0]), s_blob, n));
  TEST_END("reflow_corpus: font-size change invalidates");
}

/**
 * @test test_corpus_malformed_robust
 * @brief Malformed inputs never crash; whatever the engine lays out (or
 *        rejects) still round-trips through the cache unchanged.
 *
 * @par MC/DC:
 * (no compound decisions in this test -- exercises robustness over
 * malformed inputs; cache guards have `test_mcdc_*` vectors in
 * tests/test_ra_reflow_cache.c.)
 */
static void test_corpus_malformed_robust(void)
{
  TEST_BEGIN("reflow_corpus: malformed inputs robust");
  if (load_font() == 0) {
    (void)fprintf(stderr, "[SKIP] arnopro_latin1.otf not found; skipping\n");
    TEST_END("reflow_corpus: malformed inputs robust");
    return;
  }
  for (size_t i = 0U; i < (sizeof(s_malformed) / sizeof(s_malformed[0])); ++i) {
    uint32_t       pages = 0U;
    const ra_err_t err   = layout_live(s_malformed[i], (uint16_t)k_c_font_px, &pages);
    /* Either a clean layout or a clean rejection -- never a crash. */
    if (err == k_ra_ok) {
      assert_cache_identical(s_malformed[i]);
    } else {
      TEST_ASSERT_EQ(k_ra_err_validation_failed, err);
    }
  }
  TEST_END("reflow_corpus: malformed inputs robust");
}

int32_t main(void)
{
  test_corpus_live_cache_identity();
  test_corpus_font_size_invalidates();
  test_corpus_malformed_robust();
  (void)fprintf(stderr, "[OK ] test_ra_reflow_corpus.c\n");
  return 0;
}
