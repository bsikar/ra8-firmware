/**
 * @file test_ra8_epub.c
 * @brief Unit tests for libs/ra8_epub (EPUB reader / chapter iterator).
 *
 * @details
 * Builds a synthetic EPUB v3 archive entirely in memory using miniz's
 * `mz_zip_writer_*` API (so the test stays pure C with no on-disk
 * fixtures), then exercises the public ra8_epub_* surface against that
 * blob. This sibling owns the open / chapter / TOC / metadata / cover /
 * glyph / resource contract tests; the MC/DC null-guard and state-guard
 * vector tests live in test_ra8_epub_guards.c.
 *
 * The synthetic EPUB contains:
 *   - `mimetype`                                 (literal "application/epub+zip")
 *   - `META-INF/container.xml`                   (points at OEBPS/content.opf)
 *   - `OEBPS/content.opf`                        (metadata + manifest + spine)
 *   - `OEBPS/ch1.xhtml`                          (chapter 1 body)
 *   - `OEBPS/ch2.xhtml`                          (chapter 2 body)
 *   - `OEBPS/cover.png`                          (4 raw bytes used as a stand-in)
 *   - `OEBPS/font.ttf`                           (16 raw bytes used as a stand-in)
 *
 * Coverage:
 *   - open + close lifecycle.
 *   - chapter_count returns 2.
 *   - load_chapter pulls the right body.
 *   - get_metadata returns the Dublin Core fields.
 *   - get_cover_image copies the cover bytes.
 *   - render_glyph against a (synthetic) font fails with
 *     `validation_failed` (the stub bytes are not a real TTF).
 *   - render_glyph without an attached font returns `not_initialized`.
 *   - NULL-arg / pre-init guards on every public entry point.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * [Ring 4 / EPUB]
 * {World: NS}
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "miniz.h"
#include "ra8_epub.h"
#include "ra8_err.h"
#include "ra8_reflow.h"
#include "unity_minimal.h"

/**
 * @enum epub_fixture_t
 * @brief Poison values written into out-parameters before a call, so one that fails without assigning is detectable, plus buffer capacities and payload sizes.
 */
typedef enum : uint16_t {
  k_epub_poison_chapter =
    0xFFFFU, /**< Poison chapter index written before a lookup, so a lookup that fails without setting it is detectable. */
  k_epub_poison_len = 999U, /**< Poison length written before a read, for the same reason. */
} epub_fixture_t;

/* --------------------------------------------------------------------- */

/**
 * @enum test_ra8_epub_sizes_t
 * @brief Synthetic-fixture sizing constants.
 */
typedef enum : uint16_t {
  k_test_epub_buf_bytes    = 16384, /**< Capacity of the in-memory ZIP buffer.      */
  k_test_chapter_buf_bytes = 1024,  /**< Capacity of caller-side chapter buffer.    */
  k_test_cover_buf_bytes   = 64,    /**< Capacity of caller-side cover buffer.      */
  k_test_glyph_buf_bytes   = 4096,  /**< Capacity of caller-side glyph buffer.      */
  k_test_glyph_font_pixels = 32,    /**< Pixel size for render_glyph().             */
  k_test_synth_font_bytes  = 16,    /**< Synthetic font payload length.             */
  k_test_synth_cover_bytes = 4,     /**< Synthetic cover payload length.            */
  k_test_expected_chapters = 2,     /**< Spine length in the synthetic EPUB.        */
  k_test_expected_toc      = 3,     /**< Nav TOC entry count in the synthetic EPUB. */
  k_test_codepoint_a       = 'A',   /**< Render-glyph code point.                   */
} test_ra8_epub_sizes_t;

/* --------------------------------------------------------------------- */
/* Synthetic file payloads. */
/* --------------------------------------------------------------------- */

static const char* const k_synth_mimetype = "application/epub+zip";

static const char* const k_synth_container_xml =
  "<?xml version=\"1.0\"?>\n"
  "<container version=\"1.0\" xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\">\n"
  "  <rootfiles>\n"
  "    <rootfile full-path=\"OEBPS/content.opf\" media-type=\"application/oebps-package+xml\"/>\n"
  "  </rootfiles>\n"
  "</container>\n";

static const char* const k_synth_content_opf =
  "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
  "<package xmlns=\"http://www.idpf.org/2007/opf\" version=\"3.0\" unique-identifier=\"id\">\n"
  "  <metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\">\n"
  "    <dc:title>Test Book</dc:title>\n"
  "    <dc:creator>Brighton Sikarskie</dc:creator>\n"
  "    <dc:language>en</dc:language>\n"
  "    <dc:identifier id=\"id\">urn:test:book</dc:identifier>\n"
  "  </metadata>\n"
  "  <manifest>\n"
  "    <item id=\"nav\" href=\"nav.xhtml\" media-type=\"application/xhtml+xml\" "
  "properties=\"nav\"/>\n"
  "    <item id=\"ch1\" href=\"ch1.xhtml\" media-type=\"application/xhtml+xml\"/>\n"
  "    <item id=\"ch2\" href=\"ch2.xhtml\" media-type=\"application/xhtml+xml\"/>\n"
  "    <item id=\"cover\" href=\"cover.png\" media-type=\"image/png\" properties=\"cover-image\"/>\n"
  "  </manifest>\n"
  "  <spine>\n"
  "    <itemref idref=\"ch1\"/>\n"
  "    <itemref idref=\"ch2\"/>\n"
  "  </spine>\n"
  "</package>\n";

static const char* const k_synth_ch1 =
  "<?xml version=\"1.0\"?><html><body><h1>Chapter One</h1><p>Hello.</p></body></html>";

static const char* const k_synth_ch2 =
  "<?xml version=\"1.0\"?><html><body><h1>Chapter Two</h1><p>World.</p></body></html>";

/* The first entry carries a "#fragment" (exercises fragment-stripping in
 * ra8_epub_toc_entry_to_chapter) and the last entry targets a document that
 * is NOT in the spine (exercises the not_found resolution path). */
static const char* const k_synth_nav =
  "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
  "<html xmlns=\"http://www.w3.org/1999/xhtml\" xmlns:epub=\"http://www.idpf.org/2007/ops\">\n"
  "<head><title>Contents</title></head><body>\n"
  "<nav epub:type=\"toc\"><ol>\n"
  "  <li><a href=\"ch1.xhtml#start\">Chapter One</a></li>\n"
  "  <li><a href=\"ch2.xhtml\">Chapter Two</a></li>\n"
  "  <li><a href=\"appendix.xhtml\">Appendix</a></li>\n"
  "</ol></nav></body></html>\n";

static const uint8_t k_synth_cover_bytes[] = {0x89U, 0x50U, 0x4EU, 0x47U};

/** @brief External stylesheet body (#140) -- a distinctive class rule. */
static const char* const k_synth_css = ".lead { color: #C00000; }\n";
static const uint8_t     k_synth_font_bytes[k_test_synth_font_bytes] = {0xDEU,
                                                                        0xADU,
                                                                        0xBEU,
                                                                        0xEFU,
                                                                        0xCAU,
                                                                        0xFEU,
                                                                        0xBAU,
                                                                        0xBEU,
                                                                        0x12U,
                                                                        0x34U,
                                                                        0x56U,
                                                                        0x78U,
                                                                        0x9AU,
                                                                        0xBCU,
                                                                        0xDEU,
                                                                        0xF0U};

/* --------------------------------------------------------------------- */
/* Build the synthetic EPUB once into a static buffer. */
/* --------------------------------------------------------------------- */

/**
 * @var s_epub_buf
 * @brief Backing store for the synthetic in-memory .epub.
 *
 * @note Populated by `build_synth_epub()` before the first test runs.
 */
static uint8_t s_epub_buf[k_test_epub_buf_bytes];

/**
 * @var s_epub_size
 * @brief Number of bytes written into `s_epub_buf` by the builder.
 */
static size_t s_epub_size;

/** @brief Add the mimetype + XML document entries to the synthetic archive. */
static void synth_add_docs(mz_zip_archive* zip)
{
  /* mimetype is by spec the first entry, stored uncompressed. */
  mz_bool ok = mz_zip_writer_add_mem(zip,
                                     "mimetype",
                                     k_synth_mimetype,
                                     strlen(k_synth_mimetype),
                                     MZ_NO_COMPRESSION);
  TEST_ASSERT(ok == MZ_TRUE);

  ok = mz_zip_writer_add_mem(zip,
                             "META-INF/container.xml",
                             k_synth_container_xml,
                             strlen(k_synth_container_xml),
                             MZ_DEFAULT_COMPRESSION);
  TEST_ASSERT(ok == MZ_TRUE);

  ok = mz_zip_writer_add_mem(zip,
                             "OEBPS/content.opf",
                             k_synth_content_opf,
                             strlen(k_synth_content_opf),
                             MZ_DEFAULT_COMPRESSION);
  TEST_ASSERT(ok == MZ_TRUE);

  ok = mz_zip_writer_add_mem(zip,
                             "OEBPS/nav.xhtml",
                             k_synth_nav,
                             strlen(k_synth_nav),
                             MZ_DEFAULT_COMPRESSION);
  TEST_ASSERT(ok == MZ_TRUE);
}

/** @brief Add the chapter bodies + cover + stylesheet to the synthetic archive. */
static void synth_add_assets(mz_zip_archive* zip)
{
  mz_bool ok = mz_zip_writer_add_mem(zip,
                                     "OEBPS/ch1.xhtml",
                                     k_synth_ch1,
                                     strlen(k_synth_ch1),
                                     MZ_DEFAULT_COMPRESSION);
  TEST_ASSERT(ok == MZ_TRUE);

  ok = mz_zip_writer_add_mem(zip,
                             "OEBPS/ch2.xhtml",
                             k_synth_ch2,
                             strlen(k_synth_ch2),
                             MZ_DEFAULT_COMPRESSION);
  TEST_ASSERT(ok == MZ_TRUE);

  ok = mz_zip_writer_add_mem(zip,
                             "OEBPS/cover.png",
                             k_synth_cover_bytes,
                             sizeof(k_synth_cover_bytes),
                             MZ_NO_COMPRESSION);
  TEST_ASSERT(ok == MZ_TRUE);

  ok = mz_zip_writer_add_mem(zip,
                             "OEBPS/style.css",
                             k_synth_css,
                             strlen(k_synth_css),
                             MZ_DEFAULT_COMPRESSION);
  TEST_ASSERT(ok == MZ_TRUE);
}

static void build_synth_epub(void)
{
  mz_zip_archive zip;
  memset(&zip, 0, sizeof(zip));
  s_epub_size = 0U;

  mz_bool ok = mz_zip_writer_init_heap(&zip, 0U, k_test_epub_buf_bytes);
  TEST_ASSERT(ok == MZ_TRUE);

  synth_add_docs(&zip);
  synth_add_assets(&zip);

  void*  heap_buf  = nullptr;
  size_t heap_size = 0U;
  ok               = mz_zip_writer_finalize_heap_archive(&zip, &heap_buf, &heap_size);
  TEST_ASSERT(ok == MZ_TRUE);
  TEST_ASSERT(heap_buf != nullptr);
  TEST_ASSERT(heap_size > 0U);
  TEST_ASSERT(heap_size <= sizeof(s_epub_buf));

  memcpy(s_epub_buf, heap_buf, heap_size);
  s_epub_size = heap_size;

  mz_zip_writer_end(&zip);
}
/* --------------------------------------------------------------------- */
/* Test cases. */
/* --------------------------------------------------------------------- */

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_open_close(void)
{
  TEST_BEGIN("ra8_epub open + close");
  ra8_epub_book_t            book  = {};
  const ra8_epub_mem_media_t media = {.data = s_epub_buf, .size = s_epub_size};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_open(&media, "synth.epub", &book));
  TEST_ASSERT_EQ(1, book.in_use);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_close(&book));
  TEST_ASSERT_EQ(0, book.in_use);
  /* Double-close fails with not_initialized. */
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_epub_close(&book));
  TEST_END("ra8_epub open + close");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_chapter_count(void)
{
  TEST_BEGIN("ra8_epub chapter_count == 2");
  ra8_epub_book_t            book  = {};
  const ra8_epub_mem_media_t media = {.data = s_epub_buf, .size = s_epub_size};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_open(&media, nullptr, &book));
  uint16_t n = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_get_chapter_count(&book, &n));
  TEST_ASSERT_EQ(k_test_expected_chapters, n);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_close(&book));
  TEST_END("ra8_epub chapter_count == 2");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_load_chapter(void)
{
  TEST_BEGIN("ra8_epub load_chapter copies XHTML");
  ra8_epub_book_t            book  = {};
  const ra8_epub_mem_media_t media = {.data = s_epub_buf, .size = s_epub_size};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_open(&media, nullptr, &book));

  uint8_t buf[k_test_chapter_buf_bytes];
  size_t  got = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_load_chapter(&book, 0U, buf, sizeof(buf), &got));
  TEST_ASSERT(got > 0U);
  TEST_ASSERT(got < sizeof(buf));
  TEST_ASSERT(strstr((const char*)buf, "Chapter One") != nullptr);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_load_chapter(&book, 1U, buf, sizeof(buf), &got));
  TEST_ASSERT(strstr((const char*)buf, "Chapter Two") != nullptr);

  /* Out-of-range. */
  size_t junk = k_epub_poison_len;
  TEST_ASSERT_EQ(k_ra8_err_out_of_range,
                 ra8_epub_load_chapter(&book, 99U, buf, sizeof(buf), &junk));
  TEST_ASSERT_EQ(0, junk);

  /* Zero-length buffer. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, ra8_epub_load_chapter(&book, 0U, buf, 0U, &got));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_close(&book));
  TEST_END("ra8_epub load_chapter copies XHTML");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_toc(void)
{
  TEST_BEGIN("ra8_epub parses the EPUB3 nav TOC");
  ra8_epub_book_t            book  = {};
  const ra8_epub_mem_media_t media = {.data = s_epub_buf, .size = s_epub_size};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_open(&media, nullptr, &book));

  uint8_t kind = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_get_toc_kind(&book, &kind));
  TEST_ASSERT_EQ(k_ra8_epub_toc_nav, kind);

  uint16_t n = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_get_toc_count(&book, &n));
  TEST_ASSERT_EQ(k_test_expected_toc, n);

  ra8_epub_toc_entry_t entry = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_get_toc_entry(&book, 0U, &entry));
  TEST_ASSERT_EQ(0, strcmp(entry.title, "Chapter One"));
  TEST_ASSERT_EQ(0, strcmp(entry.href, "ch1.xhtml#start"));
  TEST_ASSERT_EQ(0, entry.depth);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_get_toc_entry(&book, 1U, &entry));
  TEST_ASSERT_EQ(0, strcmp(entry.title, "Chapter Two"));
  TEST_ASSERT_EQ(0, strcmp(entry.href, "ch2.xhtml"));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_get_toc_entry(&book, 2U, &entry));
  TEST_ASSERT_EQ(0, strcmp(entry.title, "Appendix"));
  TEST_ASSERT_EQ(0, strcmp(entry.href, "appendix.xhtml"));

  /* Out-of-range entry index. */
  TEST_ASSERT_EQ(k_ra8_err_out_of_range, ra8_epub_get_toc_entry(&book, 99U, &entry));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_close(&book));
  TEST_END("ra8_epub parses the EPUB3 nav TOC");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_toc_to_chapter(void)
{
  TEST_BEGIN("ra8_epub resolves TOC entries to spine chapters");
  ra8_epub_book_t            book  = {};
  const ra8_epub_mem_media_t media = {.data = s_epub_buf, .size = s_epub_size};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_open(&media, nullptr, &book));

  uint16_t chapter = k_epub_poison_chapter;
  /* Entry 0 carries "ch1.xhtml#start" -- the fragment must be stripped. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_toc_entry_to_chapter(&book, 0U, &chapter));
  TEST_ASSERT_EQ(0, chapter);
  /* Entry 1 is "ch2.xhtml" -> spine index 1. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_toc_entry_to_chapter(&book, 1U, &chapter));
  TEST_ASSERT_EQ(1, chapter);

  /* Entry 2 ("appendix.xhtml") is not in the spine. */
  TEST_ASSERT_EQ(k_ra8_err_not_found, ra8_epub_toc_entry_to_chapter(&book, 2U, &chapter));
  /* Out-of-range TOC index. */
  TEST_ASSERT_EQ(k_ra8_err_out_of_range, ra8_epub_toc_entry_to_chapter(&book, 99U, &chapter));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_close(&book));
  TEST_END("ra8_epub resolves TOC entries to spine chapters");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_get_metadata(void)
{
  TEST_BEGIN("ra8_epub get_metadata returns Dublin Core");
  ra8_epub_book_t            book  = {};
  const ra8_epub_mem_media_t media = {.data = s_epub_buf, .size = s_epub_size};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_open(&media, nullptr, &book));
  ra8_epub_metadata_t meta = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_get_metadata(&book, &meta));
  TEST_ASSERT(meta.title != nullptr);
  TEST_ASSERT(meta.author != nullptr);
  TEST_ASSERT(meta.language != nullptr);
  TEST_ASSERT(meta.identifier != nullptr);
  TEST_ASSERT_EQ(0, strcmp(meta.title, "Test Book"));
  TEST_ASSERT_EQ(0, strcmp(meta.author, "Brighton Sikarskie"));
  TEST_ASSERT_EQ(0, strcmp(meta.language, "en"));
  TEST_ASSERT_EQ(0, strcmp(meta.identifier, "urn:test:book"));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_close(&book));
  TEST_END("ra8_epub get_metadata returns Dublin Core");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_get_cover_image(void)
{
  TEST_BEGIN("ra8_epub get_cover_image extracts raw bytes");
  ra8_epub_book_t            book  = {};
  const ra8_epub_mem_media_t media = {.data = s_epub_buf, .size = s_epub_size};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_open(&media, nullptr, &book));
  uint8_t buf[k_test_cover_buf_bytes];
  size_t  got = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_get_cover_image(&book, buf, sizeof(buf), &got));
  TEST_ASSERT_EQ(k_test_synth_cover_bytes, got);
  TEST_ASSERT_EQ(0x89U, buf[0]);
  TEST_ASSERT_EQ(0x50U, buf[1]);
  TEST_ASSERT_EQ(0x4EU, buf[2]);
  TEST_ASSERT_EQ(0x47U, buf[3]);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_close(&book));
  TEST_END("ra8_epub get_cover_image extracts raw bytes");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_render_glyph_paths(void)
{
  TEST_BEGIN("ra8_epub render_glyph -- no font / bad font");
  ra8_epub_book_t            book  = {};
  const ra8_epub_mem_media_t media = {.data = s_epub_buf, .size = s_epub_size};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_open(&media, nullptr, &book));

  /* No font installed -- not_initialized. */
  uint8_t  bitmap[k_test_glyph_buf_bytes];
  uint32_t w = 0U;
  uint32_t h = 0U;
  TEST_ASSERT_EQ(k_ra8_err_not_initialized,
                 ra8_epub_render_glyph(&book,
                                       (int32_t)k_test_codepoint_a,
                                       (float)k_test_glyph_font_pixels,
                                       bitmap,
                                       sizeof(bitmap),
                                       &w,
                                       &h));

  /* Install a fake-but-too-small font -- invalid_size. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, ra8_epub_set_font(&book, k_synth_font_bytes, 1U));

  /* Install the synthetic 16-byte font; stbtt_InitFont will reject it
   * because it's not a real TTF, so render_glyph returns
   * validation_failed -- which is exactly the contract we want. */
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_epub_set_font(&book, k_synth_font_bytes, sizeof(k_synth_font_bytes)));
  TEST_ASSERT_EQ(k_ra8_err_validation_failed,
                 ra8_epub_render_glyph(&book,
                                       (int32_t)k_test_codepoint_a,
                                       (float)k_test_glyph_font_pixels,
                                       bitmap,
                                       sizeof(bitmap),
                                       &w,
                                       &h));

  /* Bad font size (0.0) -- invalid_arg. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_epub_render_glyph(&book,
                                       (int32_t)k_test_codepoint_a,
                                       0.0F,
                                       bitmap,
                                       sizeof(bitmap),
                                       &w,
                                       &h));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_close(&book));
  TEST_END("ra8_epub render_glyph -- no font / bad font");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_null_arg_guards(void)
{
  TEST_BEGIN("ra8_epub NULL-arg / pre-init guards");
  ra8_epub_book_t            book  = {};
  const ra8_epub_mem_media_t media = {.data = s_epub_buf, .size = s_epub_size};

  /* Pre-init guards: book->in_use == 0 on every accessor. */
  uint16_t n = 0U;
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_epub_get_chapter_count(&book, &n));
  uint8_t buf[16];
  size_t  got = 0U;
  TEST_ASSERT_EQ(k_ra8_err_not_initialized,
                 ra8_epub_load_chapter(&book, 0U, buf, sizeof(buf), &got));
  ra8_epub_metadata_t meta = {};
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_epub_get_metadata(&book, &meta));
  TEST_ASSERT_EQ(k_ra8_err_not_initialized,
                 ra8_epub_get_cover_image(&book, buf, sizeof(buf), &got));
  TEST_ASSERT_EQ(k_ra8_err_not_initialized,
                 ra8_epub_set_font(&book, k_synth_font_bytes, sizeof(k_synth_font_bytes)));

  /* NULL-arg guards. */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_epub_open(nullptr, nullptr, &book));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_epub_open(&media, nullptr, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_epub_close(nullptr));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_epub_get_chapter_count(nullptr, &n));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_epub_get_chapter_count(&book, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_epub_load_chapter(nullptr, 0U, buf, sizeof(buf), &got));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_epub_load_chapter(&book, 0U, nullptr, sizeof(buf), &got));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_epub_load_chapter(&book, 0U, buf, sizeof(buf), nullptr));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_epub_get_metadata(nullptr, &meta));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_epub_get_metadata(&book, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_epub_get_cover_image(nullptr, buf, sizeof(buf), &got));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_epub_set_font(nullptr, k_synth_font_bytes, sizeof(k_synth_font_bytes)));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_epub_set_font(&book, nullptr, sizeof(k_synth_font_bytes)));

  /* Invalid media payload. */
  const ra8_epub_mem_media_t bad_media = {.data = nullptr, .size = 0U};
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_epub_open(&bad_media, nullptr, &book));
  TEST_END("ra8_epub NULL-arg / pre-init guards");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_open_invalid_zip(void)
{
  TEST_BEGIN("ra8_epub open with garbage payload -> validation_failed");
  static const uint8_t       k_garbage[] = {0x00U, 0x01U, 0x02U, 0x03U, 0x04U, 0x05U, 0x06U, 0x07U};
  ra8_epub_book_t            book        = {};
  const ra8_epub_mem_media_t media       = {.data = k_garbage, .size = sizeof(k_garbage)};
  TEST_ASSERT_EQ(k_ra8_err_validation_failed, ra8_epub_open(&media, nullptr, &book));
  TEST_ASSERT_EQ(0, book.in_use);
  TEST_END("ra8_epub open with garbage payload -> validation_failed");
}

/** @brief Sizing for the external-stylesheet consumer test (#140). */
enum : size_t {
  k_test_css_buf_bytes = 256U, /**< Scratch for an extracted .css resource. */
};

/** @brief Static scratch the demonstration css-loader hands back to the engine. */
static uint8_t s_css_scratch[k_test_css_buf_bytes];

/**
 * @brief Demonstration `ra8_reflow_css_loader_fn`: the #140 consumer glue.
 *
 * @details Extracts a chapter's `<link href>` stylesheet bytes from the open
 * EPUB via ::ra8_epub_get_resource. An app wires this once with
 * `ra8_reflow_set_css_loader(engine, epub_css_loader, &book)`; the engine then
 * pulls + parses each external sheet in document order while tokenizing.
 *
 * @param[in]  ctx       The open `ra8_epub_book_t*`.
 * @param[in]  href      Stylesheet href (not NUL-terminated).
 * @param[in]  href_len  Length of @p href.
 * @param[out] out_bytes Receives the CSS bytes (static scratch; read-only).
 * @param[out] out_len   Receives the CSS length.
 * @return k_ra8_ok on success; any error => the engine skips the sheet.
 */
static ra8_err_t epub_css_loader(void*           ctx,
                                 const char*     href,
                                 uint32_t        href_len,
                                 const uint8_t** out_bytes,
                                 size_t*         out_len)
{
  ra8_epub_book_t* book = (ra8_epub_book_t*)ctx;
  char             path[k_ra8_epub_max_path_len];
  if (((size_t)href_len + 1U) > sizeof(path)) {
    return k_ra8_err_invalid_size;
  }
  memcpy(path, href, (size_t)href_len);
  path[href_len]      = '\0';
  size_t          got = 0U;
  const ra8_err_t err =
    ra8_epub_get_resource(book, path, s_css_scratch, sizeof(s_css_scratch), &got);
  if (err != k_ra8_ok) {
    return err;
  }
  *out_bytes = s_css_scratch;
  *out_len   = got;
  return k_ra8_ok;
}

/**
 * @test test_get_resource
 * @brief `ra8_epub_get_resource` extracts an arbitrary archive entry, and the
 *        #140 css-loader glue returns a chapter's external stylesheet (#140).
 *
 * @par MC/DC:
 * No compound decision in the test itself; the loader's
 * `href_len+1 > sizeof(path)` guard is the only branch and is covered by the
 * normal (short href) path plus `ra8_epub_get_resource`'s own guards.
 */
static void test_get_resource(void)
{
  TEST_BEGIN("ra8_epub get_resource + the #140 external-stylesheet css-loader");
  ra8_epub_book_t            book  = {};
  const ra8_epub_mem_media_t media = {.data = s_epub_buf, .size = s_epub_size};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_open(&media, nullptr, &book));

  uint8_t buf[k_test_css_buf_bytes];
  size_t  got = 0U;
  /* OPF-dir-relative resolution: "style.css" -> "OEBPS/style.css". */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_get_resource(&book, "style.css", buf, sizeof(buf), &got));
  TEST_ASSERT_EQ(strlen(k_synth_css), got);
  TEST_ASSERT(memcmp(buf, k_synth_css, got) == 0);
  /* Archive-rooted fallback resolves too. */
  got = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_get_resource(&book, "OEBPS/style.css", buf, sizeof(buf), &got));
  TEST_ASSERT_EQ(strlen(k_synth_css), got);
  /* Missing entry -> not_found; too-small buffer -> no_mem; nulls rejected. */
  TEST_ASSERT_EQ(k_ra8_err_not_found,
                 ra8_epub_get_resource(&book, "nope.css", buf, sizeof(buf), &got));
  TEST_ASSERT_EQ(k_ra8_err_no_mem, ra8_epub_get_resource(&book, "style.css", buf, 1U, &got));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_epub_get_resource(nullptr, "style.css", buf, sizeof(buf), &got));

  /* The consumer glue returns the external sheet bytes for a `<link href>`. */
  const uint8_t* css     = nullptr;
  size_t         css_len = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, epub_css_loader(&book, "style.css", 9U, &css, &css_len));
  TEST_ASSERT_EQ(strlen(k_synth_css), css_len);
  TEST_ASSERT(memcmp(css, k_synth_css, css_len) == 0);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_close(&book));
  TEST_END("ra8_epub get_resource + the #140 external-stylesheet css-loader");
}

int main(void)
{
  build_synth_epub();
  test_open_close();
  test_chapter_count();
  test_load_chapter();
  test_toc();
  test_toc_to_chapter();
  test_get_metadata();
  test_get_cover_image();
  test_get_resource();
  test_render_glyph_paths();
  test_null_arg_guards();
  test_open_invalid_zip();
  return 0;
}
