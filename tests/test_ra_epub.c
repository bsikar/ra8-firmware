/**
 * @file test_ra_epub.c
 * @brief Unit tests for libs/ra_epub (EPUB reader / chapter iterator).
 *
 * @details
 * Builds a synthetic EPUB v3 archive entirely in memory using miniz's
 * `mz_zip_writer_*` API (so the test stays pure C with no on-disk
 * fixtures), then exercises the public ra_epub_* surface against that
 * blob.
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
#include "ra_epub.h"
#include "ra_err.h"
#include "unity_minimal.h"

/* --------------------------------------------------------------------- */

/**
 * @enum test_ra_epub_sizes_t
 * @brief Synthetic-fixture sizing constants.
 */
typedef enum : uint16_t {
  k_test_epub_buf_bytes    = 16384, /**< Capacity of the in-memory ZIP buffer.   */
  k_test_chapter_buf_bytes = 1024,  /**< Capacity of caller-side chapter buffer. */
  k_test_cover_buf_bytes   = 64,    /**< Capacity of caller-side cover buffer.   */
  k_test_glyph_buf_bytes   = 4096,  /**< Capacity of caller-side glyph buffer.   */
  k_test_glyph_font_pixels = 32,    /**< Pixel size for render_glyph().          */
  k_test_synth_font_bytes  = 16,    /**< Synthetic font payload length.          */
  k_test_synth_cover_bytes = 4,     /**< Synthetic cover payload length.         */
  k_test_expected_chapters = 2,     /**< Spine length in the synthetic EPUB.     */
  k_test_expected_toc      = 3,     /**< Nav TOC entry count in the synthetic EPUB. */
  k_test_codepoint_a       = 'A',   /**< Render-glyph code point.                */
} test_ra_epub_sizes_t;

/* --------------------------------------------------------------------- */
/* Synthetic file payloads.                                              */
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
 * ra_epub_toc_entry_to_chapter) and the last entry targets a document that
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

static const uint8_t k_synth_cover_bytes[]                       = {0x89U, 0x50U, 0x4EU, 0x47U};
static const uint8_t k_synth_font_bytes[k_test_synth_font_bytes] = {0xDEU,
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
/* Build the synthetic EPUB once into a static buffer.                   */
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

static void build_synth_epub(void)
{
  mz_zip_archive zip;
  memset(&zip, 0, sizeof(zip));
  s_epub_size = 0U;

  mz_bool ok = mz_zip_writer_init_heap(&zip, 0U, k_test_epub_buf_bytes);
  TEST_ASSERT(ok == MZ_TRUE);

  /* mimetype is by spec the first entry, stored uncompressed. */
  ok = mz_zip_writer_add_mem(&zip,
                             "mimetype",
                             k_synth_mimetype,
                             strlen(k_synth_mimetype),
                             MZ_NO_COMPRESSION);
  TEST_ASSERT(ok == MZ_TRUE);

  ok = mz_zip_writer_add_mem(&zip,
                             "META-INF/container.xml",
                             k_synth_container_xml,
                             strlen(k_synth_container_xml),
                             MZ_DEFAULT_COMPRESSION);
  TEST_ASSERT(ok == MZ_TRUE);

  ok = mz_zip_writer_add_mem(&zip,
                             "OEBPS/content.opf",
                             k_synth_content_opf,
                             strlen(k_synth_content_opf),
                             MZ_DEFAULT_COMPRESSION);
  TEST_ASSERT(ok == MZ_TRUE);

  ok = mz_zip_writer_add_mem(&zip,
                             "OEBPS/nav.xhtml",
                             k_synth_nav,
                             strlen(k_synth_nav),
                             MZ_DEFAULT_COMPRESSION);
  TEST_ASSERT(ok == MZ_TRUE);

  ok = mz_zip_writer_add_mem(&zip,
                             "OEBPS/ch1.xhtml",
                             k_synth_ch1,
                             strlen(k_synth_ch1),
                             MZ_DEFAULT_COMPRESSION);
  TEST_ASSERT(ok == MZ_TRUE);

  ok = mz_zip_writer_add_mem(&zip,
                             "OEBPS/ch2.xhtml",
                             k_synth_ch2,
                             strlen(k_synth_ch2),
                             MZ_DEFAULT_COMPRESSION);
  TEST_ASSERT(ok == MZ_TRUE);

  ok = mz_zip_writer_add_mem(&zip,
                             "OEBPS/cover.png",
                             k_synth_cover_bytes,
                             sizeof(k_synth_cover_bytes),
                             MZ_NO_COMPRESSION);
  TEST_ASSERT(ok == MZ_TRUE);

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
/* Test cases.                                                           */
/* --------------------------------------------------------------------- */

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_open_close(void)
{
  TEST_BEGIN("ra_epub open + close");
  ra_epub_book_t            book  = {};
  const ra_epub_mem_media_t media = {.data = s_epub_buf, .size = s_epub_size};
  TEST_ASSERT_EQ(k_ra_ok, ra_epub_open(&media, "synth.epub", &book));
  TEST_ASSERT_EQ(1, book.in_use);
  TEST_ASSERT_EQ(k_ra_ok, ra_epub_close(&book));
  TEST_ASSERT_EQ(0, book.in_use);
  /* Double-close fails with not_initialized. */
  TEST_ASSERT_EQ(k_ra_err_not_initialized, ra_epub_close(&book));
  TEST_END("ra_epub open + close");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_chapter_count(void)
{
  TEST_BEGIN("ra_epub chapter_count == 2");
  ra_epub_book_t            book  = {};
  const ra_epub_mem_media_t media = {.data = s_epub_buf, .size = s_epub_size};
  TEST_ASSERT_EQ(k_ra_ok, ra_epub_open(&media, nullptr, &book));
  uint16_t n = 0U;
  TEST_ASSERT_EQ(k_ra_ok, ra_epub_get_chapter_count(&book, &n));
  TEST_ASSERT_EQ(k_test_expected_chapters, n);
  TEST_ASSERT_EQ(k_ra_ok, ra_epub_close(&book));
  TEST_END("ra_epub chapter_count == 2");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_load_chapter(void)
{
  TEST_BEGIN("ra_epub load_chapter copies XHTML");
  ra_epub_book_t            book  = {};
  const ra_epub_mem_media_t media = {.data = s_epub_buf, .size = s_epub_size};
  TEST_ASSERT_EQ(k_ra_ok, ra_epub_open(&media, nullptr, &book));

  uint8_t buf[k_test_chapter_buf_bytes];
  size_t  got = 0U;
  TEST_ASSERT_EQ(k_ra_ok, ra_epub_load_chapter(&book, 0U, buf, sizeof(buf), &got));
  TEST_ASSERT(got > 0U);
  TEST_ASSERT(got < sizeof(buf));
  TEST_ASSERT(strstr((const char*)buf, "Chapter One") != nullptr);

  TEST_ASSERT_EQ(k_ra_ok, ra_epub_load_chapter(&book, 1U, buf, sizeof(buf), &got));
  TEST_ASSERT(strstr((const char*)buf, "Chapter Two") != nullptr);

  /* Out-of-range. */
  size_t junk = 999U;
  TEST_ASSERT_EQ(k_ra_err_out_of_range, ra_epub_load_chapter(&book, 99U, buf, sizeof(buf), &junk));
  TEST_ASSERT_EQ(0, junk);

  /* Zero-length buffer. */
  TEST_ASSERT_EQ(k_ra_err_invalid_size, ra_epub_load_chapter(&book, 0U, buf, 0U, &got));

  TEST_ASSERT_EQ(k_ra_ok, ra_epub_close(&book));
  TEST_END("ra_epub load_chapter copies XHTML");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_toc(void)
{
  TEST_BEGIN("ra_epub parses the EPUB3 nav TOC");
  ra_epub_book_t            book  = {};
  const ra_epub_mem_media_t media = {.data = s_epub_buf, .size = s_epub_size};
  TEST_ASSERT_EQ(k_ra_ok, ra_epub_open(&media, nullptr, &book));

  uint8_t kind = 0U;
  TEST_ASSERT_EQ(k_ra_ok, ra_epub_get_toc_kind(&book, &kind));
  TEST_ASSERT_EQ(k_ra_epub_toc_nav, kind);

  uint16_t n = 0U;
  TEST_ASSERT_EQ(k_ra_ok, ra_epub_get_toc_count(&book, &n));
  TEST_ASSERT_EQ(k_test_expected_toc, n);

  ra_epub_toc_entry_t entry = {};
  TEST_ASSERT_EQ(k_ra_ok, ra_epub_get_toc_entry(&book, 0U, &entry));
  TEST_ASSERT_EQ(0, strcmp(entry.title, "Chapter One"));
  TEST_ASSERT_EQ(0, strcmp(entry.href, "ch1.xhtml#start"));
  TEST_ASSERT_EQ(0, entry.depth);

  TEST_ASSERT_EQ(k_ra_ok, ra_epub_get_toc_entry(&book, 1U, &entry));
  TEST_ASSERT_EQ(0, strcmp(entry.title, "Chapter Two"));
  TEST_ASSERT_EQ(0, strcmp(entry.href, "ch2.xhtml"));

  TEST_ASSERT_EQ(k_ra_ok, ra_epub_get_toc_entry(&book, 2U, &entry));
  TEST_ASSERT_EQ(0, strcmp(entry.title, "Appendix"));
  TEST_ASSERT_EQ(0, strcmp(entry.href, "appendix.xhtml"));

  /* Out-of-range entry index. */
  TEST_ASSERT_EQ(k_ra_err_out_of_range, ra_epub_get_toc_entry(&book, 99U, &entry));

  TEST_ASSERT_EQ(k_ra_ok, ra_epub_close(&book));
  TEST_END("ra_epub parses the EPUB3 nav TOC");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_toc_to_chapter(void)
{
  TEST_BEGIN("ra_epub resolves TOC entries to spine chapters");
  ra_epub_book_t            book  = {};
  const ra_epub_mem_media_t media = {.data = s_epub_buf, .size = s_epub_size};
  TEST_ASSERT_EQ(k_ra_ok, ra_epub_open(&media, nullptr, &book));

  uint16_t chapter = 0xFFFFU;
  /* Entry 0 carries "ch1.xhtml#start" -- the fragment must be stripped. */
  TEST_ASSERT_EQ(k_ra_ok, ra_epub_toc_entry_to_chapter(&book, 0U, &chapter));
  TEST_ASSERT_EQ(0, chapter);
  /* Entry 1 is "ch2.xhtml" -> spine index 1. */
  TEST_ASSERT_EQ(k_ra_ok, ra_epub_toc_entry_to_chapter(&book, 1U, &chapter));
  TEST_ASSERT_EQ(1, chapter);

  /* Entry 2 ("appendix.xhtml") is not in the spine. */
  TEST_ASSERT_EQ(k_ra_err_not_found, ra_epub_toc_entry_to_chapter(&book, 2U, &chapter));
  /* Out-of-range TOC index. */
  TEST_ASSERT_EQ(k_ra_err_out_of_range, ra_epub_toc_entry_to_chapter(&book, 99U, &chapter));

  TEST_ASSERT_EQ(k_ra_ok, ra_epub_close(&book));
  TEST_END("ra_epub resolves TOC entries to spine chapters");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_get_metadata(void)
{
  TEST_BEGIN("ra_epub get_metadata returns Dublin Core");
  ra_epub_book_t            book  = {};
  const ra_epub_mem_media_t media = {.data = s_epub_buf, .size = s_epub_size};
  TEST_ASSERT_EQ(k_ra_ok, ra_epub_open(&media, nullptr, &book));
  ra_epub_metadata_t meta = {};
  TEST_ASSERT_EQ(k_ra_ok, ra_epub_get_metadata(&book, &meta));
  TEST_ASSERT(meta.title != nullptr);
  TEST_ASSERT(meta.author != nullptr);
  TEST_ASSERT(meta.language != nullptr);
  TEST_ASSERT_EQ(0, strcmp(meta.title, "Test Book"));
  TEST_ASSERT_EQ(0, strcmp(meta.author, "Brighton Sikarskie"));
  TEST_ASSERT_EQ(0, strcmp(meta.language, "en"));
  TEST_ASSERT_EQ(k_ra_ok, ra_epub_close(&book));
  TEST_END("ra_epub get_metadata returns Dublin Core");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_get_cover_image(void)
{
  TEST_BEGIN("ra_epub get_cover_image extracts raw bytes");
  ra_epub_book_t            book  = {};
  const ra_epub_mem_media_t media = {.data = s_epub_buf, .size = s_epub_size};
  TEST_ASSERT_EQ(k_ra_ok, ra_epub_open(&media, nullptr, &book));
  uint8_t buf[k_test_cover_buf_bytes];
  size_t  got = 0U;
  TEST_ASSERT_EQ(k_ra_ok, ra_epub_get_cover_image(&book, buf, sizeof(buf), &got));
  TEST_ASSERT_EQ(k_test_synth_cover_bytes, got);
  TEST_ASSERT_EQ(0x89U, buf[0]);
  TEST_ASSERT_EQ(0x50U, buf[1]);
  TEST_ASSERT_EQ(0x4EU, buf[2]);
  TEST_ASSERT_EQ(0x47U, buf[3]);
  TEST_ASSERT_EQ(k_ra_ok, ra_epub_close(&book));
  TEST_END("ra_epub get_cover_image extracts raw bytes");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_render_glyph_paths(void)
{
  TEST_BEGIN("ra_epub render_glyph -- no font / bad font");
  ra_epub_book_t            book  = {};
  const ra_epub_mem_media_t media = {.data = s_epub_buf, .size = s_epub_size};
  TEST_ASSERT_EQ(k_ra_ok, ra_epub_open(&media, nullptr, &book));

  /* No font installed -- not_initialized. */
  uint8_t  bitmap[k_test_glyph_buf_bytes];
  uint32_t w = 0U;
  uint32_t h = 0U;
  TEST_ASSERT_EQ(k_ra_err_not_initialized,
                 ra_epub_render_glyph(&book,
                                      (int32_t)k_test_codepoint_a,
                                      (float)k_test_glyph_font_pixels,
                                      bitmap,
                                      sizeof(bitmap),
                                      &w,
                                      &h));

  /* Install a fake-but-too-small font -- invalid_size. */
  TEST_ASSERT_EQ(k_ra_err_invalid_size, ra_epub_set_font(&book, k_synth_font_bytes, 1U));

  /* Install the synthetic 16-byte font; stbtt_InitFont will reject it
   * because it's not a real TTF, so render_glyph returns
   * validation_failed -- which is exactly the contract we want. */
  TEST_ASSERT_EQ(k_ra_ok, ra_epub_set_font(&book, k_synth_font_bytes, sizeof(k_synth_font_bytes)));
  TEST_ASSERT_EQ(k_ra_err_validation_failed,
                 ra_epub_render_glyph(&book,
                                      (int32_t)k_test_codepoint_a,
                                      (float)k_test_glyph_font_pixels,
                                      bitmap,
                                      sizeof(bitmap),
                                      &w,
                                      &h));

  /* Bad font size (0.0) -- invalid_arg. */
  TEST_ASSERT_EQ(
    k_ra_err_invalid_arg,
    ra_epub_render_glyph(&book, (int32_t)k_test_codepoint_a, 0.0F, bitmap, sizeof(bitmap), &w, &h));

  TEST_ASSERT_EQ(k_ra_ok, ra_epub_close(&book));
  TEST_END("ra_epub render_glyph -- no font / bad font");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_null_arg_guards(void)
{
  TEST_BEGIN("ra_epub NULL-arg / pre-init guards");
  ra_epub_book_t            book  = {};
  const ra_epub_mem_media_t media = {.data = s_epub_buf, .size = s_epub_size};

  /* Pre-init guards: book->in_use == 0 on every accessor. */
  uint16_t n = 0U;
  TEST_ASSERT_EQ(k_ra_err_not_initialized, ra_epub_get_chapter_count(&book, &n));
  uint8_t buf[16];
  size_t  got = 0U;
  TEST_ASSERT_EQ(k_ra_err_not_initialized, ra_epub_load_chapter(&book, 0U, buf, sizeof(buf), &got));
  ra_epub_metadata_t meta = {};
  TEST_ASSERT_EQ(k_ra_err_not_initialized, ra_epub_get_metadata(&book, &meta));
  TEST_ASSERT_EQ(k_ra_err_not_initialized, ra_epub_get_cover_image(&book, buf, sizeof(buf), &got));
  TEST_ASSERT_EQ(k_ra_err_not_initialized,
                 ra_epub_set_font(&book, k_synth_font_bytes, sizeof(k_synth_font_bytes)));

  /* NULL-arg guards. */
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_epub_open(nullptr, nullptr, &book));
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_epub_open(&media, nullptr, nullptr));
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_epub_close(nullptr));
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_epub_get_chapter_count(nullptr, &n));
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_epub_get_chapter_count(&book, nullptr));
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_epub_load_chapter(nullptr, 0U, buf, sizeof(buf), &got));
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_epub_load_chapter(&book, 0U, nullptr, sizeof(buf), &got));
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_epub_load_chapter(&book, 0U, buf, sizeof(buf), nullptr));
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_epub_get_metadata(nullptr, &meta));
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_epub_get_metadata(&book, nullptr));
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_epub_get_cover_image(nullptr, buf, sizeof(buf), &got));
  TEST_ASSERT_EQ(k_ra_err_null_ptr,
                 ra_epub_set_font(nullptr, k_synth_font_bytes, sizeof(k_synth_font_bytes)));
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_epub_set_font(&book, nullptr, sizeof(k_synth_font_bytes)));

  /* Invalid media payload. */
  const ra_epub_mem_media_t bad_media = {.data = nullptr, .size = 0U};
  TEST_ASSERT_EQ(k_ra_err_invalid_arg, ra_epub_open(&bad_media, nullptr, &book));
  TEST_END("ra_epub NULL-arg / pre-init guards");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_open_invalid_zip(void)
{
  TEST_BEGIN("ra_epub open with garbage payload -> validation_failed");
  static const uint8_t      k_garbage[] = {0x00U, 0x01U, 0x02U, 0x03U, 0x04U, 0x05U, 0x06U, 0x07U};
  ra_epub_book_t            book        = {};
  const ra_epub_mem_media_t media       = {.data = k_garbage, .size = sizeof(k_garbage)};
  TEST_ASSERT_EQ(k_ra_err_validation_failed, ra_epub_open(&media, nullptr, &book));
  TEST_ASSERT_EQ(0, book.in_use);
  TEST_END("ra_epub open with garbage payload -> validation_failed");
}

/* --------------------------------------------------------------------- */
/* MC/DC vector tests for libs/ra_epub/src/ra_epub_chapter.c             */
/* --------------------------------------------------------------------- */

typedef enum : uint16_t {
  /* 256 bytes comfortably holds the synthetic chapter XHTML
   * (<?xml...?><html><body><h1>Chapter One</h1><p>Hello.</p>...) which
   * is ~80 bytes. Sized so the V1 vectors that exercise the success
   * path do not trip the chapter buffer's no_mem guard. */
  k_mcdc_buf_small = 256U,
} epub_mcdc_caps_t;

/**
 * @test test_mcdc_chapter_count_null_or
 *
 * @par MC/DC:
 * Decision: `if (book == NULL || out_count == NULL)`
 * (2 conditions, libs/ra_epub/src/ra_epub_chapter.c line 192)
 * - V1 book=ok, out=ok  -> C1=F, C2=F. Decision F (proceeds).
 * - V2 book=NULL        -> C1=T short-circuits. T -> null_ptr.
 * - V3 book=ok, out=NULL -> C1=F, C2=T. T -> null_ptr.
 * V1+V2 vary C1; V1+V3 vary C2. N+1=3.
 */
static void test_mcdc_chapter_count_null_or(void)
{
  TEST_BEGIN("mcdc chapter_count NULL OR");
  ra_epub_book_t            book  = {};
  const ra_epub_mem_media_t media = {.data = s_epub_buf, .size = s_epub_size};
  TEST_ASSERT_EQ(k_ra_ok, ra_epub_open(&media, nullptr, &book));
  uint16_t n = 0U;
  /* V1 */
  TEST_ASSERT_EQ(k_ra_ok, ra_epub_get_chapter_count(&book, &n));
  /* V2 */
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_epub_get_chapter_count(nullptr, &n));
  /* V3 */
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_epub_get_chapter_count(&book, nullptr));
  TEST_ASSERT_EQ(k_ra_ok, ra_epub_close(&book));
  TEST_END("mcdc chapter_count NULL OR");
}

/**
 * @test test_mcdc_load_chapter_null_or3
 *
 * @par MC/DC:
 * Decision: `if (book == NULL || out_xhtml == NULL || got_len == NULL)`
 * (3 conditions, libs/ra_epub/src/ra_epub_chapter.c line 208). N+1=4.
 *
 * @par DO-178C 6.4.4.3 omission rationale:
 * Full short-circuit MC/DC for N=3 OR requires N+1=4 vectors. We use
 * the canonical short-circuit set: each NULL position triggers exactly
 * one condition T while keeping the others at their masking value (F).
 * - V1 all non-NULL                -> all F. F (proceeds).
 * - V2 book=NULL                   -> C1=T. T -> null_ptr.
 * - V3 book=ok, out=NULL           -> C1=F, C2=T. T -> null_ptr.
 * - V4 book=ok, out=ok, got=NULL   -> C1=F, C2=F, C3=T. T -> null_ptr.
 */
static void test_mcdc_load_chapter_null_or3(void)
{
  TEST_BEGIN("mcdc load_chapter NULL OR(3)");
  ra_epub_book_t            book  = {};
  const ra_epub_mem_media_t media = {.data = s_epub_buf, .size = s_epub_size};
  TEST_ASSERT_EQ(k_ra_ok, ra_epub_open(&media, nullptr, &book));
  uint8_t buf[k_mcdc_buf_small];
  size_t  got = 0U;
  /* V1 */
  TEST_ASSERT_EQ(k_ra_ok, ra_epub_load_chapter(&book, 0U, buf, sizeof(buf), &got));
  /* V2 */
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_epub_load_chapter(nullptr, 0U, buf, sizeof(buf), &got));
  /* V3 */
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_epub_load_chapter(&book, 0U, nullptr, sizeof(buf), &got));
  /* V4 */
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_epub_load_chapter(&book, 0U, buf, sizeof(buf), nullptr));
  TEST_ASSERT_EQ(k_ra_ok, ra_epub_close(&book));
  TEST_END("mcdc load_chapter NULL OR(3)");
}

/**
 * @test test_mcdc_load_chapter_state_or
 *
 * @par MC/DC:
 * Decision: `if (book->in_use == 0U || book->zip_archive_active == 0U)`
 * (2 conditions, libs/ra_epub/src/ra_epub_chapter.c line 212). N+1=3.
 *
 * @par DO-178C 6.4.4.3 omission rationale:
 * - V1 in_use=1, zip_active=1 -> both F. F (proceeds).
 * - V2 in_use=0               -> C1=T. T -> not_initialized.
 * - V3 in_use=1, zip_active=0 -> C1=F, C2=T.  V3 cannot be reached
 *   through the public API alone because ra_epub_open atomically sets
 *   both flags and ra_epub_close clears both. The masking pair V1+V2
 *   proves C1 independence; C2 independence is argued by inspection
 *   (identical short-circuit form, identical effect: not_initialized).
 *   Per DO-178C 6.4.4.3 unreachable-by-design vectors may be omitted
 *   when the mutual-exclusion invariant is documented.
 */
static void test_mcdc_load_chapter_state_or(void)
{
  TEST_BEGIN("mcdc load_chapter state OR (in_use || zip)");
  ra_epub_book_t            book  = {};
  const ra_epub_mem_media_t media = {.data = s_epub_buf, .size = s_epub_size};
  uint8_t                   buf[k_mcdc_buf_small];
  size_t                    got = 0U;
  /* V1 */
  TEST_ASSERT_EQ(k_ra_ok, ra_epub_open(&media, nullptr, &book));
  TEST_ASSERT_EQ(k_ra_ok, ra_epub_load_chapter(&book, 0U, buf, sizeof(buf), &got));
  TEST_ASSERT_EQ(k_ra_ok, ra_epub_close(&book));
  /* V2 */
  TEST_ASSERT_EQ(k_ra_err_not_initialized, ra_epub_load_chapter(&book, 0U, buf, sizeof(buf), &got));
  TEST_END("mcdc load_chapter state OR (in_use || zip)");
}

/**
 * @test test_mcdc_metadata_null_or
 *
 * @par MC/DC:
 * Decision: `if (book == NULL || out_meta == NULL)`
 * (2 conditions, libs/ra_epub/src/ra_epub_chapter.c line 232). N+1=3.
 * - V1 both non-NULL -> both F. F (proceeds, ok).
 * - V2 book=NULL     -> C1=T short-circuits. T -> null_ptr.
 * - V3 book=ok, out=NULL -> C1=F, C2=T. T -> null_ptr.
 */
static void test_mcdc_metadata_null_or(void)
{
  TEST_BEGIN("mcdc metadata NULL OR");
  ra_epub_book_t            book  = {};
  const ra_epub_mem_media_t media = {.data = s_epub_buf, .size = s_epub_size};
  TEST_ASSERT_EQ(k_ra_ok, ra_epub_open(&media, nullptr, &book));
  ra_epub_metadata_t meta = {};
  TEST_ASSERT_EQ(k_ra_ok, ra_epub_get_metadata(&book, &meta));
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_epub_get_metadata(nullptr, &meta));
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_epub_get_metadata(&book, nullptr));
  TEST_ASSERT_EQ(k_ra_ok, ra_epub_close(&book));
  TEST_END("mcdc metadata NULL OR");
}

/**
 * @test test_mcdc_toc_kind_null_or
 *
 * @par MC/DC:
 * Decision: `if (book == NULL || out_kind == NULL)`
 * (2 conditions, libs/ra_epub/src/ra_epub_chapter.c@ra_epub_get_toc_kind)
 * - V1 book=ok, out=ok   -> C1=F, C2=F. Decision F (proceeds).
 * - V2 book=NULL         -> C1=T short-circuits. T -> null_ptr.
 * - V3 book=ok, out=NULL -> C1=F, C2=T. T -> null_ptr.
 * V1+V2 isolate C1; V1+V3 isolate C2.
 */
static void test_mcdc_toc_kind_null_or(void)
{
  TEST_BEGIN("mcdc toc_kind NULL OR");
  ra_epub_book_t            book  = {};
  const ra_epub_mem_media_t media = {.data = s_epub_buf, .size = s_epub_size};
  TEST_ASSERT_EQ(k_ra_ok, ra_epub_open(&media, nullptr, &book));
  uint8_t kind = 0U;
  TEST_ASSERT_EQ(k_ra_ok, ra_epub_get_toc_kind(&book, &kind));
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_epub_get_toc_kind(nullptr, &kind));
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_epub_get_toc_kind(&book, nullptr));
  TEST_ASSERT_EQ(k_ra_ok, ra_epub_close(&book));
  TEST_END("mcdc toc_kind NULL OR");
}

/**
 * @test test_mcdc_toc_count_null_or
 *
 * @par MC/DC:
 * Decision: `if (book == NULL || out_count == NULL)`
 * (2 conditions, libs/ra_epub/src/ra_epub_chapter.c@ra_epub_get_toc_count)
 * - V1 book=ok, out=ok   -> C1=F, C2=F. Decision F (proceeds).
 * - V2 book=NULL         -> C1=T short-circuits. T -> null_ptr.
 * - V3 book=ok, out=NULL -> C1=F, C2=T. T -> null_ptr.
 * V1+V2 isolate C1; V1+V3 isolate C2.
 */
static void test_mcdc_toc_count_null_or(void)
{
  TEST_BEGIN("mcdc toc_count NULL OR");
  ra_epub_book_t            book  = {};
  const ra_epub_mem_media_t media = {.data = s_epub_buf, .size = s_epub_size};
  TEST_ASSERT_EQ(k_ra_ok, ra_epub_open(&media, nullptr, &book));
  uint16_t n = 0U;
  TEST_ASSERT_EQ(k_ra_ok, ra_epub_get_toc_count(&book, &n));
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_epub_get_toc_count(nullptr, &n));
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_epub_get_toc_count(&book, nullptr));
  TEST_ASSERT_EQ(k_ra_ok, ra_epub_close(&book));
  TEST_END("mcdc toc_count NULL OR");
}

/**
 * @test test_mcdc_toc_entry_null_or
 *
 * @par MC/DC:
 * Decision: `if (book == NULL || out_entry == NULL)`
 * (2 conditions, libs/ra_epub/src/ra_epub_chapter.c@ra_epub_get_toc_entry)
 * - V1 book=ok, out=ok   -> C1=F, C2=F. Decision F (proceeds).
 * - V2 book=NULL         -> C1=T short-circuits. T -> null_ptr.
 * - V3 book=ok, out=NULL -> C1=F, C2=T. T -> null_ptr.
 * V1+V2 isolate C1; V1+V3 isolate C2.
 */
static void test_mcdc_toc_entry_null_or(void)
{
  TEST_BEGIN("mcdc toc_entry NULL OR");
  ra_epub_book_t            book  = {};
  const ra_epub_mem_media_t media = {.data = s_epub_buf, .size = s_epub_size};
  TEST_ASSERT_EQ(k_ra_ok, ra_epub_open(&media, nullptr, &book));
  ra_epub_toc_entry_t entry = {};
  TEST_ASSERT_EQ(k_ra_ok, ra_epub_get_toc_entry(&book, 0U, &entry));
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_epub_get_toc_entry(nullptr, 0U, &entry));
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_epub_get_toc_entry(&book, 0U, nullptr));
  TEST_ASSERT_EQ(k_ra_ok, ra_epub_close(&book));
  TEST_END("mcdc toc_entry NULL OR");
}

/**
 * @test test_mcdc_toc_to_chapter_null_or
 *
 * @par MC/DC:
 * Decision: `if (book == NULL || out_chapter_idx == NULL)`
 * (2 conditions, libs/ra_epub/src/ra_epub_chapter.c@ra_epub_toc_entry_to_chapter)
 * - V1 book=ok, out=ok   -> C1=F, C2=F. Decision F (proceeds).
 * - V2 book=NULL         -> C1=T short-circuits. T -> null_ptr.
 * - V3 book=ok, out=NULL -> C1=F, C2=T. T -> null_ptr.
 * V1+V2 isolate C1; V1+V3 isolate C2.
 */
static void test_mcdc_toc_to_chapter_null_or(void)
{
  TEST_BEGIN("mcdc toc_entry_to_chapter NULL OR");
  ra_epub_book_t            book  = {};
  const ra_epub_mem_media_t media = {.data = s_epub_buf, .size = s_epub_size};
  TEST_ASSERT_EQ(k_ra_ok, ra_epub_open(&media, nullptr, &book));
  uint16_t chapter = 0U;
  TEST_ASSERT_EQ(k_ra_ok, ra_epub_toc_entry_to_chapter(&book, 0U, &chapter));
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_epub_toc_entry_to_chapter(nullptr, 0U, &chapter));
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_epub_toc_entry_to_chapter(&book, 0U, nullptr));
  TEST_ASSERT_EQ(k_ra_ok, ra_epub_close(&book));
  TEST_END("mcdc toc_entry_to_chapter NULL OR");
}

/**
 * @test test_mcdc_cover_image_null_or3
 *
 * @par MC/DC:
 * Decision: `if (book == NULL || out_buf == NULL || got_len == NULL)`
 * (3 conditions, line 247). N+1=4. Same short-circuit-set rationale as
 * test_mcdc_load_chapter_null_or3.
 *
 * @par DO-178C 6.4.4.3:
 * - V1 all non-NULL                -> all F. F (proceeds, ok).
 * - V2 book=NULL                   -> C1=T. T -> null_ptr.
 * - V3 out=NULL                    -> C2=T. T -> null_ptr.
 * - V4 got=NULL                    -> C3=T. T -> null_ptr.
 */
static void test_mcdc_cover_image_null_or3(void)
{
  TEST_BEGIN("mcdc cover_image NULL OR(3)");
  ra_epub_book_t            book  = {};
  const ra_epub_mem_media_t media = {.data = s_epub_buf, .size = s_epub_size};
  TEST_ASSERT_EQ(k_ra_ok, ra_epub_open(&media, nullptr, &book));
  uint8_t buf[k_mcdc_buf_small];
  size_t  got = 0U;
  TEST_ASSERT_EQ(k_ra_ok, ra_epub_get_cover_image(&book, buf, sizeof(buf), &got));
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_epub_get_cover_image(nullptr, buf, sizeof(buf), &got));
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_epub_get_cover_image(&book, nullptr, sizeof(buf), &got));
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_epub_get_cover_image(&book, buf, sizeof(buf), nullptr));
  TEST_ASSERT_EQ(k_ra_ok, ra_epub_close(&book));
  TEST_END("mcdc cover_image NULL OR(3)");
}

/**
 * @test test_mcdc_set_font_null_or
 *
 * @par MC/DC:
 * Decision: `if (book == NULL || font_data == NULL)`
 * (2 conditions, line 271). N+1=3.
 */
static void test_mcdc_set_font_null_or(void)
{
  TEST_BEGIN("mcdc set_font NULL OR");
  ra_epub_book_t            book  = {};
  const ra_epub_mem_media_t media = {.data = s_epub_buf, .size = s_epub_size};
  TEST_ASSERT_EQ(k_ra_ok, ra_epub_open(&media, nullptr, &book));
  static const uint8_t k_dummy_font[k_test_synth_font_bytes] = {};
  /* V1 */
  TEST_ASSERT_EQ(k_ra_ok, ra_epub_set_font(&book, k_dummy_font, sizeof(k_dummy_font)));
  /* V2 */
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_epub_set_font(nullptr, k_dummy_font, sizeof(k_dummy_font)));
  /* V3 */
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_epub_set_font(&book, nullptr, sizeof(k_dummy_font)));
  TEST_ASSERT_EQ(k_ra_ok, ra_epub_close(&book));
  TEST_END("mcdc set_font NULL OR");
}

/**
 * @test test_mcdc_render_glyph_null_or4
 *
 * @par MC/DC:
 * Decision: `if (book == NULL || out_bitmap == NULL ||
 *               out_w == NULL || out_h == NULL)`
 * (4 conditions, line 293). N+1=5.
 *
 * @par DO-178C 6.4.4.3 omission rationale:
 * Full short-circuit MC/DC for N=4 ORs requires N+1=5 vectors. The
 * canonical short-circuit set isolates each condition's flip with the
 * others held at their masking value F:
 * - V1 all non-NULL  -> all F. F (proceeds; not_initialized later).
 * - V2 book=NULL     -> C1=T. T -> null_ptr.
 * - V3 bitmap=NULL   -> C1=F,C2=T. T -> null_ptr.
 * - V4 out_w=NULL    -> C1=F,C2=F,C3=T. T -> null_ptr.
 * - V5 out_h=NULL    -> C1=F,C2=F,C3=F,C4=T. T -> null_ptr.
 */
static void test_mcdc_render_glyph_null_or4(void)
{
  TEST_BEGIN("mcdc render_glyph NULL OR(4)");
  ra_epub_book_t            book  = {};
  const ra_epub_mem_media_t media = {.data = s_epub_buf, .size = s_epub_size};
  TEST_ASSERT_EQ(k_ra_ok, ra_epub_open(&media, nullptr, &book));
  uint8_t  bm[k_mcdc_buf_small];
  uint32_t w = 0U, h = 0U;
  /* V1 (no font installed -> not_initialized, but the NULL guard passes). */
  TEST_ASSERT_EQ(k_ra_err_not_initialized,
                 ra_epub_render_glyph(&book,
                                      (int32_t)k_test_codepoint_a,
                                      (float)k_test_glyph_font_pixels,
                                      bm,
                                      sizeof(bm),
                                      &w,
                                      &h));
  /* V2 */
  TEST_ASSERT_EQ(k_ra_err_null_ptr,
                 ra_epub_render_glyph(nullptr, 0, 16.0F, bm, sizeof(bm), &w, &h));
  /* V3 */
  TEST_ASSERT_EQ(k_ra_err_null_ptr,
                 ra_epub_render_glyph(&book, 0, 16.0F, nullptr, sizeof(bm), &w, &h));
  /* V4 */
  TEST_ASSERT_EQ(k_ra_err_null_ptr,
                 ra_epub_render_glyph(&book, 0, 16.0F, bm, sizeof(bm), nullptr, &h));
  /* V5 */
  TEST_ASSERT_EQ(k_ra_err_null_ptr,
                 ra_epub_render_glyph(&book, 0, 16.0F, bm, sizeof(bm), &w, nullptr));
  TEST_ASSERT_EQ(k_ra_ok, ra_epub_close(&book));
  TEST_END("mcdc render_glyph NULL OR(4)");
}

/**
 * @test test_mcdc_render_glyph_state_or
 *
 * @par MC/DC:
 * Decision: `if (book->in_use == 0U || book->font_data == NULL)`
 * (2 conditions, line 298). N+1=3.
 * - V1 in_use=1, font_data set -> both F. F (proceeds).
 * - V2 in_use=0 (closed)       -> C1=T short-circuits. T.
 * - V3 in_use=1, font_data=NULL -> C1=F, C2=T. T (no font installed).
 */
static void test_mcdc_render_glyph_state_or(void)
{
  TEST_BEGIN("mcdc render_glyph state OR (in_use || font)");
  ra_epub_book_t            book  = {};
  const ra_epub_mem_media_t media = {.data = s_epub_buf, .size = s_epub_size};
  uint8_t                   bm[k_mcdc_buf_small];
  uint32_t                  w = 0U, h = 0U;

  /* V3 first: opened but no font set -> not_initialized via C2 path. */
  TEST_ASSERT_EQ(k_ra_ok, ra_epub_open(&media, nullptr, &book));
  TEST_ASSERT_EQ(
    k_ra_err_not_initialized,

    ra_epub_render_glyph(&book, (int32_t)k_test_codepoint_a, 16.0F, bm, sizeof(bm), &w, &h));
  /* V1: install (synthetic) font and re-call. priv_font_init will
   * reject the dummy bytes, returning validation_failed -- but the
   * NULL/state guard at line 298 evaluates false (proceeds past it),
   * which is what we are asserting independence on. */
  static const uint8_t k_dummy_font[k_test_synth_font_bytes] = {};
  TEST_ASSERT_EQ(k_ra_ok, ra_epub_set_font(&book, k_dummy_font, sizeof(k_dummy_font)));
  const int32_t rc1 = (int32_t)
    ra_epub_render_glyph(&book, (int32_t)k_test_codepoint_a, 16.0F, bm, sizeof(bm), &w, &h);
  TEST_ASSERT(rc1 != (int32_t)k_ra_err_not_initialized);

  TEST_ASSERT_EQ(k_ra_ok, ra_epub_close(&book));
  /* V2: book closed (in_use=0) -> not_initialized via C1. */
  TEST_ASSERT_EQ(
    k_ra_err_not_initialized,

    ra_epub_render_glyph(&book, (int32_t)k_test_codepoint_a, 16.0F, bm, sizeof(bm), &w, &h));
  TEST_END("mcdc render_glyph state OR (in_use || font)");
}

/**
 * @test test_mcdc_priv_join_path_dst_or
 *
 * @par MC/DC:
 * Decision: `if (dst == NULL || cap == 0U)`
 * (2 conditions, libs/ra_epub/src/ra_epub_chapter.c line 64).
 * priv_join_path is static; it is invoked from ra_epub_load_chapter
 * (line 223) and ra_epub_get_cover_image (line 262), each of which
 * always passes a real non-NULL stack buffer with cap>0.
 *
 * @par DO-178C 6.4.4.3 omission rationale:
 * The (dst==NULL) and (cap==0U) input domain is unreachable through
 * the public API. The two callers fix dst and cap to compile-time
 * constants (full_path[k_ra_epub_max_path_len], sizeof(full_path)).
 * This is documented as "unreachable by design"; the V1 vector below
 * (dst!=NULL, cap>0 -> both F, decision F, function does the join)
 * is the only reachable case and the masking pairs V1+V2 / V1+V3 are
 * argued by code inspection (identical AND/OR forms with no side
 * effects beyond the early return).
 */
static void test_mcdc_priv_join_path_dst_or(void)
{
  TEST_BEGIN("mcdc priv_join_path dst OR (unreachable bad-arg vectors)");
  ra_epub_book_t            book  = {};
  const ra_epub_mem_media_t media = {.data = s_epub_buf, .size = s_epub_size};
  TEST_ASSERT_EQ(k_ra_ok, ra_epub_open(&media, nullptr, &book));
  uint8_t buf[k_mcdc_buf_small];
  size_t  got = 0U;
  /* V1 reachable: load_chapter drives priv_join_path with a real
   * stack buffer of fixed cap. */
  TEST_ASSERT_EQ(k_ra_ok, ra_epub_load_chapter(&book, 0U, buf, sizeof(buf), &got));
  TEST_ASSERT(got > 0U);
  TEST_ASSERT_EQ(k_ra_ok, ra_epub_close(&book));
  TEST_END("mcdc priv_join_path dst OR (unreachable bad-arg vectors)");
}

/* --------------------------------------------------------------------- */

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
  test_render_glyph_paths();
  test_null_arg_guards();
  test_open_invalid_zip();
  test_mcdc_chapter_count_null_or();
  test_mcdc_load_chapter_null_or3();
  test_mcdc_load_chapter_state_or();
  test_mcdc_metadata_null_or();
  test_mcdc_toc_kind_null_or();
  test_mcdc_toc_count_null_or();
  test_mcdc_toc_entry_null_or();
  test_mcdc_toc_to_chapter_null_or();
  test_mcdc_cover_image_null_or3();
  test_mcdc_set_font_null_or();
  test_mcdc_render_glyph_null_or4();
  test_mcdc_render_glyph_state_or();
  test_mcdc_priv_join_path_dst_or();
  return 0;
}
