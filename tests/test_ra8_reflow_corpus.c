/**
 * @file test_ra8_reflow_corpus.c
 * @brief Content corpus + live-layout pagination-cache acceptance (#79).
 *
 * @details
 * Closes the issue #79 acceptance for the import-time pagination cache:
 * "corpus fixtures run via `make test`; cache produces identical pages to
 * live layout; config change invalidates correctly."
 *
 * Unlike `tests/test_ra8_reflow_cache.c` (which round-trips a *synthetic*
 * glyph array to unit-test the codec), this test drives the **real**
 * pipeline: it loads the bundled Literata Latin-1 face, runs
 * `ra8_reflow_layout_chapter()` over a corpus of representative XHTML
 * chapters (exercising the full v1 tag subset) plus deliberately
 * malformed inputs, then proves that
 * `ra8_reflow_cache_serialize()` -> `ra8_reflow_cache_load()` into a fresh
 * engine reproduces the live layout **glyph-for-glyph and page-for-page**,
 * and that bumping the font size invalidates the cached blob.
 *
 * It also drives the **full container pipeline** for #79's real-book corpus:
 * a real EPUB (public-domain Project Gutenberg prose -- H.G. Wells'
 * *The Time Machine*) is assembled in memory with miniz (the
 * `tests/test_ra8_epub.c` pattern), opened with `ra8_epub_open()`, and each
 * spine chapter is pulled with `ra8_epub_load_chapter()` and run through the
 * same layout -> cache -> restore identity check. A truncated archive must be
 * rejected without a crash.
 *
 * The font is located relative to `__FILE__` (mirrors
 * `tests/test_ra8_fs_font_reflow.c`); if it is missing the test SKIPs
 * rather than failing, so the suite stays green on a bare checkout.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "miniz.h"
#include "ra8_epub.h"
#include "ra8_err.h"
#include "ra8_reflow.h"
#include "ra8_reflow_cache.h"
#include "support/ra8_test_file.h"
#include "support/ra8_test_output.h"
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
  k_c_font_cap = 2U * 1024U * 1024U, /**< Literata subset < 2 MiB.    */
  k_c_blob_cap = 1U << 20U,          /**< 1 MiB serialisation buffer. */
  k_c_path_cap = 1024U,              /**< Font path buffer.           */
} c_size_t;

static uint8_t            s_font[k_c_font_cap];
static uint8_t            s_font_staging[k_c_font_cap];
static size_t             s_font_len;
static uint8_t            s_blob[k_c_blob_cap];
static ra8_reflow_t       s_live;
static ra8_reflow_t       s_restored;
static ra8_reflow_glyph_t s_glyph_snap[k_ra8_reflow_max_glyphs];
static ra8_reflow_page_t  s_page_snap[k_ra8_reflow_max_pages];

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
 * @brief Load the bundled Literata subset relative to this source file.
 * @return 1 on success, 0 if the font is absent (caller SKIPs).

 * @details Performs one bounded, deterministic operation for this host test.
 * @retval 0 Zero or false result; nonzero values describe the alternate result.
 * @pre Pointer arguments, when present, address their documented test storage.
 * @pre Scalar arguments satisfy the bounds asserted by this test helper.
 * @post The helper completes only the bounded test operation described above.
 * @post Failures are returned or reported through the test assertion surface.
 * @note Test-only helper with no production ABI.
 * @since 0.1.0
*/
RA8_INTERNAL static int internal_load_font(void)
{
  char         path[k_c_path_cap];
  const char*  here = __FILE__;
  const size_t flen = strlen(here);
  size_t       base = flen;
  while (base > 0U && here[base - 1U] != '/') {
    base--; /* drop "test_ra8_reflow_corpus.c" */
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
  (void)snprintf(&path[base], sizeof(path) - base, "libs/ra8_fonts/literata_latin1.ttf");
  const ra8_test_file_result_t result =
    internal_test_file_read(path, s_font, sizeof(s_font), s_font_staging, sizeof(s_font_staging));
  if (result.status != k_ra8_test_file_ok) {
    return 0;
  }
  s_font_len = result.transferred;
  return (s_font_len > 0U) ? 1 : 0;
}

/** @brief Lay out @p xhtml live into s_live; return the layout result.
 * @details Performs one bounded, deterministic operation for this host test.
 * @param[in] xhtml Argument for the bounded test operation.
 * @param[in] font_px Argument for the bounded test operation.
 * @param[in,out] pages Argument for the bounded test operation.
 * @return Function-specific result consumed by the calling test.
 * @retval 0 Zero or false result; nonzero values describe the alternate result.
 * @pre Pointer arguments, when present, address their documented test storage.
 * @pre Scalar arguments satisfy the bounds asserted by this test helper.
 * @post The helper completes only the bounded test operation described above.
 * @post Failures are returned or reported through the test assertion surface.
 * @note Test-only helper with no production ABI.
 * @since 0.1.0
*/
RA8_INTERNAL static ra8_err_t
internal_layout_live(const char* xhtml, uint16_t font_px, uint32_t* pages)
{
  ra8_err_t err = ra8_reflow_init((uint16_t)k_c_vp_w,
                                  (uint16_t)k_c_vp_h,
                                  s_font,
                                  s_font_len,
                                  font_px,
                                  (uint32_t)k_c_body_color,
                                  (uint32_t)k_c_link_color,
                                  &s_live);
  TEST_ASSERT_EQ(k_ra8_ok, err);
  return ra8_reflow_layout_chapter(&s_live, (const uint8_t*)xhtml, strlen(xhtml), pages);
}

/** @brief Assert the cache restores s_live's layout glyph/page-identically.
 * @details Performs one bounded, deterministic operation for this host test.
 * @param[in] xhtml Argument for the bounded test operation.
 * @pre Pointer arguments, when present, address their documented test storage.
 * @pre Scalar arguments satisfy the bounds asserted by this test helper.
 * @post The helper completes only the bounded test operation described above.
 * @post Failures are returned or reported through the test assertion surface.
 * @note Test-only helper with no production ABI.
 * @since 0.1.0
*/
RA8_INTERNAL static void internal_assert_cache_identical(const char* xhtml)
{
  const uint32_t gc = s_live.glyph_count;
  const uint32_t pc = s_live.page_count;
  memcpy(s_glyph_snap, s_live.glyphs, (size_t)gc * sizeof(s_live.glyphs[0]));
  memcpy(s_page_snap, s_live.pages, (size_t)pc * sizeof(s_live.pages[0]));

  size_t n = 0U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_reflow_cache_serialize(&s_live,
                                            (const uint8_t*)xhtml,
                                            strlen(xhtml),
                                            s_blob,
                                            (size_t)k_c_blob_cap,
                                            &n));

  /* Fresh engine, same key -> load must reproduce the live layout. */
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_reflow_init((uint16_t)k_c_vp_w,
                                 (uint16_t)k_c_vp_h,
                                 s_font,
                                 s_font_len,
                                 (uint16_t)k_c_font_px,
                                 (uint32_t)k_c_body_color,
                                 (uint32_t)k_c_link_color,
                                 &s_restored));
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_reflow_cache_load(&s_restored, (const uint8_t*)xhtml, strlen(xhtml), s_blob, n));
  TEST_ASSERT_EQ(gc, s_restored.glyph_count);
  TEST_ASSERT_EQ(pc, s_restored.page_count);
  TEST_ASSERT_EQ(0, memcmp(s_glyph_snap, s_restored.glyphs, (size_t)gc * sizeof(s_glyph_snap[0])));
  TEST_ASSERT_EQ(0, memcmp(s_page_snap, s_restored.pages, (size_t)pc * sizeof(s_page_snap[0])));
}

/**
 * @test internal_test_corpus_live_cache_identity
 * @brief Every well-formed chapter lays out, then the cache restores it
 *        glyph-for-glyph and page-for-page into a fresh engine.
 *
 * @par MC/DC:
 * (no compound decisions in this test -- it exercises the live layout ->
 * cache -> restore acceptance path; cache guards have `test_mcdc_*`
 * vectors in tests/test_ra8_reflow_cache.c.)

 * @details Performs one bounded, deterministic operation for this host test.
 * @pre Pointer arguments, when present, address their documented test storage.
 * @pre Scalar arguments satisfy the bounds asserted by this test helper.
 * @post The helper completes only the bounded test operation described above.
 * @post Failures are returned or reported through the test assertion surface.
 * @note Test-only helper with no production ABI.
 * @since 0.1.0
*/
RA8_INTERNAL static void internal_test_corpus_live_cache_identity(void)
{
  TEST_BEGIN("reflow_corpus: cache == live layout");
  if (internal_load_font() == 0) {
    TEST_ASSERT_EQ(
      k_ra8_test_output_ok,
      internal_test_output_fd_text(STDERR_FILENO,
                                   "[SKIP] literata_latin1.ttf not found; skipping corpus\n"));
    TEST_END("reflow_corpus: cache == live layout");
    return;
  }
  for (size_t i = 0U; i < (sizeof(s_corpus) / sizeof(s_corpus[0])); ++i) {
    uint32_t pages = 0U;
    TEST_ASSERT_EQ(k_ra8_ok, internal_layout_live(s_corpus[i], (uint16_t)k_c_font_px, &pages));
    TEST_ASSERT(pages >= 1U);
    TEST_ASSERT(s_live.glyph_count > 0U);
    internal_assert_cache_identical(s_corpus[i]);
  }
  TEST_END("reflow_corpus: cache == live layout");
}

/**
 * @test internal_test_corpus_font_size_invalidates
 * @brief A blob baked at one font size is rejected (stale) by an engine
 *        at a different size -- config change invalidates correctly.
 *
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the invalidation
 * acceptance path; cache guards have `test_mcdc_*` vectors in
 * tests/test_ra8_reflow_cache.c.)

 * @details Performs one bounded, deterministic operation for this host test.
 * @pre Pointer arguments, when present, address their documented test storage.
 * @pre Scalar arguments satisfy the bounds asserted by this test helper.
 * @post The helper completes only the bounded test operation described above.
 * @post Failures are returned or reported through the test assertion surface.
 * @note Test-only helper with no production ABI.
 * @since 0.1.0
*/
RA8_INTERNAL static void internal_test_corpus_font_size_invalidates(void)
{
  TEST_BEGIN("reflow_corpus: font-size change invalidates");
  if (internal_load_font() == 0) {
    TEST_ASSERT_EQ(
      k_ra8_test_output_ok,
      internal_test_output_fd_text(STDERR_FILENO,
                                   "[SKIP] literata_latin1.ttf not found; skipping\n"));
    TEST_END("reflow_corpus: font-size change invalidates");
    return;
  }
  uint32_t pages = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, internal_layout_live(s_corpus[0], (uint16_t)k_c_font_px, &pages));
  size_t n = 0U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_reflow_cache_serialize(&s_live,
                                            (const uint8_t*)s_corpus[0],
                                            strlen(s_corpus[0]),
                                            s_blob,
                                            (size_t)k_c_blob_cap,
                                            &n));
  /* Same content + font blob, different font size -> stale. */
  TEST_ASSERT_EQ(k_ra8_ok, internal_layout_live(s_corpus[0], (uint16_t)k_c_font_px2, &pages));
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_state,
    ra8_reflow_cache_load(&s_live, (const uint8_t*)s_corpus[0], strlen(s_corpus[0]), s_blob, n));
  TEST_END("reflow_corpus: font-size change invalidates");
}

/**
 * @test internal_test_corpus_malformed_robust
 * @brief Malformed inputs never crash; whatever the engine lays out (or
 *        rejects) still round-trips through the cache unchanged.
 *
 * @par MC/DC:
 * (no compound decisions in this test -- exercises robustness over
 * malformed inputs; cache guards have `test_mcdc_*` vectors in
 * tests/test_ra8_reflow_cache.c.)

 * @details Performs one bounded, deterministic operation for this host test.
 * @pre Pointer arguments, when present, address their documented test storage.
 * @pre Scalar arguments satisfy the bounds asserted by this test helper.
 * @post The helper completes only the bounded test operation described above.
 * @post Failures are returned or reported through the test assertion surface.
 * @note Test-only helper with no production ABI.
 * @since 0.1.0
*/
RA8_INTERNAL static void internal_test_corpus_malformed_robust(void)
{
  TEST_BEGIN("reflow_corpus: malformed inputs robust");
  if (internal_load_font() == 0) {
    TEST_ASSERT_EQ(
      k_ra8_test_output_ok,
      internal_test_output_fd_text(STDERR_FILENO,
                                   "[SKIP] literata_latin1.ttf not found; skipping\n"));
    TEST_END("reflow_corpus: malformed inputs robust");
    return;
  }
  for (size_t i = 0U; i < (sizeof(s_malformed) / sizeof(s_malformed[0])); ++i) {
    uint32_t        pages = 0U;
    const ra8_err_t err   = internal_layout_live(s_malformed[i], (uint16_t)k_c_font_px, &pages);
    /* Either a clean layout or a clean rejection -- never a crash. */
    if (err == k_ra8_ok) {
      internal_assert_cache_identical(s_malformed[i]);
    } else {
      TEST_ASSERT_EQ(k_ra8_err_validation_failed, err);
    }
  }
  TEST_END("reflow_corpus: malformed inputs robust");
}

/* =========================================================================
 * Real EPUB container corpus (#79): a public-domain Project Gutenberg book
 * (H.G. Wells, "The Time Machine") assembled in memory with miniz, opened via
 * ra8_epub, and run chapter-by-chapter through the same layout -> cache check.
 * ========================================================================= */

typedef enum : size_t {
  k_c_epub_cap    = 32U * 1024U, /**< In-memory .epub ZIP buffer.       */
  k_c_chapter_cap = 16U * 1024U, /**< Per-chapter XHTML staging buffer. */
} c_epub_size_t;

static uint8_t s_epub_buf[k_c_epub_cap];
static size_t  s_epub_size;
static uint8_t s_chapter[k_c_chapter_cap];

static const char* const s_g_mimetype = "application/epub+zip";

static const char* const s_g_container =
  "<?xml version=\"1.0\"?>\n"
  "<container version=\"1.0\" xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\">\n"
  "  <rootfiles><rootfile full-path=\"OEBPS/content.opf\" "
  "media-type=\"application/oebps-package+xml\"/></rootfiles>\n"
  "</container>\n";

static const char* const s_g_opf =
  "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
  "<package xmlns=\"http://www.idpf.org/2007/opf\" version=\"3.0\" unique-identifier=\"id\">\n"
  "  <metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\">\n"
  "    <dc:title>The Time Machine</dc:title>\n"
  "    <dc:creator>H. G. Wells</dc:creator>\n"
  "    <dc:language>en</dc:language>\n"
  "    <dc:identifier id=\"id\">urn:gutenberg:35</dc:identifier>\n"
  "  </metadata>\n"
  "  <manifest>\n"
  "    <item id=\"nav\" href=\"nav.xhtml\" media-type=\"application/xhtml+xml\" properties=\"nav\"/>\n"
  "    <item id=\"c1\" href=\"c1.xhtml\" media-type=\"application/xhtml+xml\"/>\n"
  "    <item id=\"c2\" href=\"c2.xhtml\" media-type=\"application/xhtml+xml\"/>\n"
  "  </manifest>\n"
  "  <spine><itemref idref=\"c1\"/><itemref idref=\"c2\"/></spine>\n"
  "</package>\n";

static const char* const s_g_nav =
  "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
  "<html xmlns=\"http://www.w3.org/1999/xhtml\" xmlns:epub=\"http://www.idpf.org/2007/ops\">\n"
  "<head><title>Contents</title></head><body>\n"
  "<nav epub:type=\"toc\"><ol>\n"
  "  <li><a href=\"c1.xhtml\">Introduction</a></li>\n"
  "  <li><a href=\"c2.xhtml\">The Machine</a></li>\n"
  "</ol></nav></body></html>\n";

/* Real public-domain prose (Project Gutenberg #35), exercising the v1 tag
 * subset; ASCII transcription (em-dashes as "--", straight quotes). */
static const char* const s_g_chapter1 =
  "<?xml version=\"1.0\"?><html><body>"
  "<h1>I. Introduction</h1>"
  "<p>The Time Traveller (for so it will be convenient to speak of him) was "
  "expounding a <em>recondite</em> matter to us. His pale grey eyes shone and "
  "twinkled, and his usually pale face was flushed and animated.</p>"
  "<p>The fire burned brightly, and the soft radiance of the incandescent "
  "lights in the lilies of silver caught the bubbles that flashed and passed "
  "in our glasses.</p>"
  "<blockquote>You must follow me carefully. I shall have to controvert one or "
  "two ideas that are almost universally accepted.</blockquote>"
  "<p>The geometry, for instance, they taught you at school is founded on a "
  "<strong>misconception</strong>.</p>"
  "</body></html>";

static const char* const s_g_chapter2 =
  "<?xml version=\"1.0\"?><html><body>"
  "<h2>II. The Machine</h2>"
  "<p>I am afraid I cannot convey the peculiar sensations of time travelling. "
  "They are excessively unpleasant. There is a feeling exactly like that one "
  "has upon a switchback -- of a helpless headlong motion!</p>"
  "<ul><li>I felt the same horrible anticipation, too, of an imminent smash.</li>"
  "<li>As I put on pace, night followed day like the flapping of a black wing.</li>"
  "<li>The jerking sun became a streak of fire, the moon a fainter band.</li></ul>"
  "<p>I saw trees growing and changing like puffs of vapour, now brown, now "
  "green; they grew, spread, shivered, and passed away.</p>"
  "</body></html>";

/**
 * @brief Assemble the in-memory EPUB into s_epub_buf (miniz writer).
 * @return 1 on success, 0 on a miniz failure.

 * @details Performs one bounded, deterministic operation for this host test.
 * @retval 0 Zero or false result; nonzero values describe the alternate result.
 * @pre Pointer arguments, when present, address their documented test storage.
 * @pre Scalar arguments satisfy the bounds asserted by this test helper.
 * @post The helper completes only the bounded test operation described above.
 * @post Failures are returned or reported through the test assertion surface.
 * @note Test-only helper with no production ABI.
 * @since 0.1.0
*/
RA8_INTERNAL static int internal_build_gutenberg_epub(void)
{
  mz_zip_archive zip;
  memset(&zip, 0, sizeof(zip));
  s_epub_size = 0U;
  if (mz_zip_writer_init_heap(&zip, 0U, (size_t)k_c_epub_cap) != MZ_TRUE) {
    return 0;
  }
  struct {
    const char* path;  /**< Path.  */
    const char* data;  /**< Data.  */
    mz_uint     flags; /**< Flags. */
  } entries[] = {
    {"mimetype", s_g_mimetype, MZ_NO_COMPRESSION},
    {"META-INF/container.xml", s_g_container, MZ_DEFAULT_COMPRESSION},
    {"OEBPS/content.opf", s_g_opf, MZ_DEFAULT_COMPRESSION},
    {"OEBPS/nav.xhtml", s_g_nav, MZ_DEFAULT_COMPRESSION},
    {"OEBPS/c1.xhtml", s_g_chapter1, MZ_DEFAULT_COMPRESSION},
    {"OEBPS/c2.xhtml", s_g_chapter2, MZ_DEFAULT_COMPRESSION},
  };
  for (size_t i = 0U; i < (sizeof(entries) / sizeof(entries[0])); ++i) {
    if (mz_zip_writer_add_mem(&zip,
                              entries[i].path,
                              entries[i].data,
                              strlen(entries[i].data),
                              entries[i].flags) != MZ_TRUE) {
      mz_zip_writer_end(&zip);
      return 0;
    }
  }
  void*  heap_buf  = nullptr;
  size_t heap_size = 0U;
  if (mz_zip_writer_finalize_heap_archive(&zip, &heap_buf, &heap_size) != MZ_TRUE) {
    mz_zip_writer_end(&zip);
    return 0;
  }
  if ((heap_buf == nullptr) || (heap_size == 0U) || (heap_size > sizeof(s_epub_buf))) {
    mz_free(heap_buf);
    mz_zip_writer_end(&zip);
    return 0;
  }
  memcpy(s_epub_buf, heap_buf, heap_size);
  s_epub_size = heap_size;
  mz_free(heap_buf);
  mz_zip_writer_end(&zip);
  return 1;
}

/**
 * @test internal_test_corpus_real_epub_pipeline
 * @brief Open a real EPUB container, then for every spine chapter prove the
 *        live layout caches and restores glyph/page-identically -- the full
 *        ra8_epub -> ra8_reflow -> ra8_reflow_cache pipeline for #79's real-book
 *        corpus.
 *
 * @par MC/DC:
 * (no compound decisions in this test -- it exercises the container ->
 * layout -> cache acceptance path; ra8_epub guards have `test_mcdc_*` vectors
 * in tests/test_ra8_epub_*.c and cache guards in tests/test_ra8_reflow_cache.c.)

 * @details Performs one bounded, deterministic operation for this host test.
 * @pre Pointer arguments, when present, address their documented test storage.
 * @pre Scalar arguments satisfy the bounds asserted by this test helper.
 * @post The helper completes only the bounded test operation described above.
 * @post Failures are returned or reported through the test assertion surface.
 * @note Test-only helper with no production ABI.
 * @since 0.1.0
*/
RA8_INTERNAL static void internal_test_corpus_real_epub_pipeline(void)
{
  TEST_BEGIN("reflow_corpus: real EPUB container -> layout -> cache");
  if (internal_load_font() == 0) {
    TEST_ASSERT_EQ(
      k_ra8_test_output_ok,
      internal_test_output_fd_text(STDERR_FILENO,
                                   "[SKIP] literata_latin1.ttf not found; skipping epub corpus\n"));
    TEST_END("reflow_corpus: real EPUB container -> layout -> cache");
    return;
  }
  TEST_ASSERT(internal_build_gutenberg_epub() == 1);

  ra8_epub_book_t            book  = {};
  const ra8_epub_mem_media_t media = {.data = s_epub_buf, .size = s_epub_size};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_open(&media, "the_time_machine.epub", &book));

  uint16_t chapters = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_get_chapter_count(&book, &chapters));
  TEST_ASSERT_EQ(2U, chapters);

  for (uint16_t i = 0U; i < chapters; ++i) {
    size_t    got = 0U;
    ra8_err_t err = ra8_epub_load_chapter(&book, i, s_chapter, (size_t)k_c_chapter_cap - 1U, &got);
    TEST_ASSERT_EQ(k_ra8_ok, err);
    TEST_ASSERT(got > 0U);
    s_chapter[got] = (uint8_t)'\0'; /* NUL-terminate for the strlen-based helpers */

    uint32_t pages = 0U;
    TEST_ASSERT_EQ(k_ra8_ok,
                   internal_layout_live((const char*)s_chapter, (uint16_t)k_c_font_px, &pages));
    TEST_ASSERT(pages >= 1U);
    TEST_ASSERT(s_live.glyph_count > 0U);
    internal_assert_cache_identical((const char*)s_chapter);
  }
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_close(&book));
  TEST_END("reflow_corpus: real EPUB container -> layout -> cache");
}

/**
 * @test internal_test_corpus_truncated_epub_rejected
 * @brief A truncated/garbage archive is rejected by ra8_epub_open without a
 *        crash (the malformed-file half of #79's corpus).
 *
 * @par MC/DC:
 * (no compound decisions in this test -- robustness of the container open
 * path; ra8_epub_open guards have `test_mcdc_*` vectors in test_ra8_epub_open.c.)

 * @details Performs one bounded, deterministic operation for this host test.
 * @pre Pointer arguments, when present, address their documented test storage.
 * @pre Scalar arguments satisfy the bounds asserted by this test helper.
 * @post The helper completes only the bounded test operation described above.
 * @post Failures are returned or reported through the test assertion surface.
 * @note Test-only helper with no production ABI.
 * @since 0.1.0
*/
RA8_INTERNAL static void internal_test_corpus_truncated_epub_rejected(void)
{
  TEST_BEGIN("reflow_corpus: truncated EPUB rejected");
  TEST_ASSERT(internal_build_gutenberg_epub() == 1);

  /* Lop off the central directory -> not a valid archive. */
  ra8_epub_book_t            book  = {};
  const ra8_epub_mem_media_t media = {.data = s_epub_buf, .size = s_epub_size / 2U};
  const ra8_err_t            err   = ra8_epub_open(&media, "truncated.epub", &book);
  TEST_ASSERT(err != k_ra8_ok); /* rejected, not crashed */
  TEST_END("reflow_corpus: truncated EPUB rejected");
}

int32_t main(void)
{
  internal_test_corpus_live_cache_identity();
  internal_test_corpus_font_size_invalidates();
  internal_test_corpus_malformed_robust();
  internal_test_corpus_real_epub_pipeline();
  internal_test_corpus_truncated_epub_rejected();
  TEST_ASSERT_EQ(k_ra8_test_output_ok,
                 internal_test_output_fd_text(STDERR_FILENO, "[OK ] test_ra8_reflow_corpus.c\n"));
  return 0;
}
