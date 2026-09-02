/**
 * @file test_epub.c
 * @brief Unit tests for apps/shared_libs/epub (EPUB reader / chapter iterator).
 *
 * @details
 * Builds a synthetic EPUB v3 archive entirely in memory using miniz's
 * `mz_zip_writer_*` API (so the test stays pure C with no on-disk
 * fixtures), then exercises the public epub_* surface against that
 * blob. This sibling owns the open / chapter / TOC / metadata / cover /
 * glyph / resource contract tests; the MC/DC null-guard and state-guard
 * vector tests live in test_epub_guards.c.
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

#include "epub.h"
#include "miniz.h"
#include "ra8_attributes.h"
#include "ra8_err.h"
#include "reflow.h"
#include "unity_minimal.h"

/**
 * @enum epub_fixture_t
 * @brief Poison values written into out-parameters before a call, so one that fails without assigning is detectable, plus buffer capacities and payload sizes.
 */
typedef enum : uint16_t {
  k_epub_poison_chapter =
    0xFFFFU, /**< Poison chapter index before a lookup; a lookup that skips it is detectable. */
  /** Poison length written before a read, for the same reason. */
  k_epub_poison_len = 999U,
} epub_fixture_t;

/* --------------------------------------------------------------------- */

/**
 * @enum test_epub_sizes_t
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
} test_epub_sizes_t;

/** @brief Compare byte representations without the character-type restrictions of memcmp. */
RA8_INTERNAL static bool internal_bytes_equal(const void* lhs, const void* rhs, size_t length)
{
  const uint8_t* const lhs_bytes = lhs;
  const uint8_t* const rhs_bytes = rhs;
  bool                 equal     = true;
  for (size_t index = 0U; index < length; ++index) {
    if (lhs_bytes[index] != rhs_bytes[index]) {
      equal = false;
      break;
    }
  }
  return equal;
}

/* --------------------------------------------------------------------- */
/* Synthetic file payloads. */
/* --------------------------------------------------------------------- */

/** @brief Synthetic EPUB source assets shared by the builder and assertions. */
static const struct {
  const char* mimetype;                              /**< Required uncompressed EPUB media type. */
  const char* container_xml;                         /**< Rootfile locator document.             */
  const char* content_opf;                           /**< Package manifest and spine.            */
  const char* chapter_one;                           /**< First XHTML spine document.            */
  const char* chapter_two;                           /**< Second XHTML spine document.           */
  const char* navigation;                            /**< EPUB navigation document.              */
  uint8_t     cover_bytes[k_test_synth_cover_bytes]; /**< Synthetic cover payload.               */
  const char* css;                                   /**< Linked stylesheet source.              */
  uint8_t     font_bytes[k_test_synth_font_bytes];   /**< Synthetic embedded font.               */
} s_synth = {
  .mimetype = "application/epub+zip",

  .container_xml =
    "<?xml version=\"1.0\"?>\n"
    "<container version=\"1.0\" xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\">\n"
    "  <rootfiles>\n"
    "    <rootfile full-path=\"OEBPS/content.opf\" media-type=\"application/oebps-package+xml\"/>\n"
    "  </rootfiles>\n"
    "</container>\n",

  .content_opf =
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
    "</package>\n",

  .chapter_one =
    "<?xml version=\"1.0\"?><html><body><h1>Chapter One</h1><p>Hello.</p></body></html>",

  .chapter_two =
    "<?xml version=\"1.0\"?><html><body><h1>Chapter Two</h1><p>World.</p></body></html>",

  /* The first entry carries a "#fragment" (exercises fragment-stripping in
 * epub_toc_entry_to_chapter) and the last entry targets a document that
 * is NOT in the spine (exercises the not_found resolution path). */
  .navigation =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<html xmlns=\"http://www.w3.org/1999/xhtml\" xmlns:epub=\"http://www.idpf.org/2007/ops\">\n"
    "<head><title>Contents</title></head><body>\n"
    "<nav epub:type=\"toc\"><ol>\n"
    "  <li><a href=\"ch1.xhtml#start\">Chapter One</a></li>\n"
    "  <li><a href=\"ch2.xhtml\">Chapter Two</a></li>\n"
    "  <li><a href=\"appendix.xhtml\">Appendix</a></li>\n"
    "</ol></nav></body></html>\n",

  .cover_bytes = {0x89U, 0x50U, 0x4EU, 0x47U},

  /** @brief External stylesheet body (#140) -- a distinctive class rule. */
  .css        = ".lead { color: #C00000; }\n",
  .font_bytes = {0xDEU,
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
                 0xF0U},
};

/* --------------------------------------------------------------------- */
/* Build the synthetic EPUB once into a static buffer. */
/* --------------------------------------------------------------------- */

/**
 * @var s_epub_buf
 * @brief Backing store for the synthetic in-memory .epub.
 *
 * @note Populated by `internal_build_synth_epub()` before the first test runs.
 */
static uint8_t s_epub_buf[k_test_epub_buf_bytes];

/**
 * @var s_epub_size
 * @brief Number of bytes written into `s_epub_buf` by the builder.
 */
static size_t s_epub_size;

/** @brief Add the mimetype + XML document entries to the synthetic archive. @details Implements the synth add docs fixture operation used only by this focused test executable. @param[in,out] zip Fixture argument governed by the exercised interface contract. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_synth_add_docs(mz_zip_archive* zip)
{
  /* mimetype is by spec the first entry, stored uncompressed. */
  mz_bool ok = mz_zip_writer_add_mem(zip,
                                     "mimetype",
                                     s_synth.mimetype,
                                     strlen(s_synth.mimetype),
                                     MZ_NO_COMPRESSION);
  TEST_ASSERT(ok == MZ_TRUE);

  ok = mz_zip_writer_add_mem(zip,
                             "META-INF/container.xml",
                             s_synth.container_xml,
                             strlen(s_synth.container_xml),
                             MZ_DEFAULT_COMPRESSION);
  TEST_ASSERT(ok == MZ_TRUE);

  ok = mz_zip_writer_add_mem(zip,
                             "OEBPS/content.opf",
                             s_synth.content_opf,
                             strlen(s_synth.content_opf),
                             MZ_DEFAULT_COMPRESSION);
  TEST_ASSERT(ok == MZ_TRUE);

  ok = mz_zip_writer_add_mem(zip,
                             "OEBPS/nav.xhtml",
                             s_synth.navigation,
                             strlen(s_synth.navigation),
                             MZ_DEFAULT_COMPRESSION);
  TEST_ASSERT(ok == MZ_TRUE);
}

/** @brief Add the chapter bodies + cover + stylesheet to the synthetic archive. @details Implements the synth add assets fixture operation used only by this focused test executable. @param[in,out] zip Fixture argument governed by the exercised interface contract. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_synth_add_assets(mz_zip_archive* zip)
{
  mz_bool ok = mz_zip_writer_add_mem(zip,
                                     "OEBPS/ch1.xhtml",
                                     s_synth.chapter_one,
                                     strlen(s_synth.chapter_one),
                                     MZ_DEFAULT_COMPRESSION);
  TEST_ASSERT(ok == MZ_TRUE);

  ok = mz_zip_writer_add_mem(zip,
                             "OEBPS/ch2.xhtml",
                             s_synth.chapter_two,
                             strlen(s_synth.chapter_two),
                             MZ_DEFAULT_COMPRESSION);
  TEST_ASSERT(ok == MZ_TRUE);

  ok = mz_zip_writer_add_mem(zip,
                             "OEBPS/cover.png",
                             s_synth.cover_bytes,
                             sizeof(s_synth.cover_bytes),
                             MZ_NO_COMPRESSION);
  TEST_ASSERT(ok == MZ_TRUE);

  ok = mz_zip_writer_add_mem(zip,
                             "OEBPS/style.css",
                             s_synth.css,
                             strlen(s_synth.css),
                             MZ_DEFAULT_COMPRESSION);
  TEST_ASSERT(ok == MZ_TRUE);
}

/** @brief Prepare the fixture's build synth epub state. @details Implements the build synth epub fixture operation used only by this focused test executable. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_build_synth_epub(void)
{
  mz_zip_archive zip;
  (void)memset(&zip, 0, sizeof(zip));
  s_epub_size = 0U;

  mz_bool ok = mz_zip_writer_init_heap(&zip, 0U, k_test_epub_buf_bytes);
  TEST_ASSERT(ok == MZ_TRUE);

  internal_synth_add_docs(&zip);
  internal_synth_add_assets(&zip);

  void*  heap_buf  = nullptr;
  size_t heap_size = 0U;
  ok               = mz_zip_writer_finalize_heap_archive(&zip, &heap_buf, &heap_size);
  TEST_ASSERT(ok == MZ_TRUE);
  TEST_ASSERT(heap_buf != nullptr);
  TEST_ASSERT(heap_size > 0U);
  TEST_ASSERT(heap_size <= sizeof(s_epub_buf));

  (void)memcpy(s_epub_buf, heap_buf, heap_size);
  s_epub_size = heap_size;

  mz_free(heap_buf);
  mz_zip_writer_end(&zip);
}
/* --------------------------------------------------------------------- */
/* Test cases. */
/* --------------------------------------------------------------------- */

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches) @brief Verify open close behavior. @details Executes the open close scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_open_close(void)
{
  TEST_BEGIN("epub open + close");
  epub_book_t            book  = {};
  const epub_mem_media_t media = {.data = s_epub_buf, .size = s_epub_size};
  TEST_ASSERT_EQ(k_ra8_ok, epub_open(&media, "synth.epub", &book));
  TEST_ASSERT_EQ(1, book.in_use);
  TEST_ASSERT_EQ(k_ra8_ok, epub_close(&book));
  TEST_ASSERT_EQ(0, book.in_use);
  /* Double-close fails with not_initialized. */
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, epub_close(&book));
  TEST_END("epub open + close");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches) @brief Verify chapter count behavior. @details Executes the chapter count scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_chapter_count(void)
{
  TEST_BEGIN("epub chapter_count == 2");
  epub_book_t            book  = {};
  const epub_mem_media_t media = {.data = s_epub_buf, .size = s_epub_size};
  TEST_ASSERT_EQ(k_ra8_ok, epub_open(&media, nullptr, &book));
  uint16_t n = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, epub_get_chapter_count(&book, &n));
  TEST_ASSERT_EQ(k_test_expected_chapters, n);
  TEST_ASSERT_EQ(k_ra8_ok, epub_close(&book));
  TEST_END("epub chapter_count == 2");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches) @brief Verify load chapter behavior. @details Executes the load chapter scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_load_chapter(void)
{
  TEST_BEGIN("epub load_chapter copies XHTML");
  epub_book_t            book  = {};
  const epub_mem_media_t media = {.data = s_epub_buf, .size = s_epub_size};
  TEST_ASSERT_EQ(k_ra8_ok, epub_open(&media, nullptr, &book));

  uint8_t buf[k_test_chapter_buf_bytes];
  size_t  got = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, epub_load_chapter(&book, 0U, buf, sizeof(buf), &got));
  TEST_ASSERT(got > 0U);
  TEST_ASSERT(got < sizeof(buf));
  TEST_ASSERT(strstr((const char*)buf, "Chapter One") != nullptr);

  TEST_ASSERT_EQ(k_ra8_ok, epub_load_chapter(&book, 1U, buf, sizeof(buf), &got));
  TEST_ASSERT(strstr((const char*)buf, "Chapter Two") != nullptr);

  /* Out-of-range. */
  size_t junk = k_epub_poison_len;
  TEST_ASSERT_EQ(k_ra8_err_out_of_range, epub_load_chapter(&book, 99U, buf, sizeof(buf), &junk));
  TEST_ASSERT_EQ(0, junk);

  /* Zero-length buffer. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, epub_load_chapter(&book, 0U, buf, 0U, &got));

  TEST_ASSERT_EQ(k_ra8_ok, epub_close(&book));
  TEST_END("epub load_chapter copies XHTML");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches) @brief Verify toc behavior. @details Executes the toc scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_toc(void)
{
  TEST_BEGIN("epub parses the EPUB3 nav TOC");
  epub_book_t            book  = {};
  const epub_mem_media_t media = {.data = s_epub_buf, .size = s_epub_size};
  TEST_ASSERT_EQ(k_ra8_ok, epub_open(&media, nullptr, &book));

  uint8_t kind = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, epub_get_toc_kind(&book, &kind));
  TEST_ASSERT_EQ(k_epub_toc_nav, kind);

  uint16_t n = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, epub_get_toc_count(&book, &n));
  TEST_ASSERT_EQ(k_test_expected_toc, n);

  epub_toc_entry_t entry = {};
  TEST_ASSERT_EQ(k_ra8_ok, epub_get_toc_entry(&book, 0U, &entry));
  TEST_ASSERT_EQ(0, strcmp(entry.title, "Chapter One"));
  TEST_ASSERT_EQ(0, strcmp(entry.href, "ch1.xhtml#start"));
  TEST_ASSERT_EQ(0, entry.depth);

  TEST_ASSERT_EQ(k_ra8_ok, epub_get_toc_entry(&book, 1U, &entry));
  TEST_ASSERT_EQ(0, strcmp(entry.title, "Chapter Two"));
  TEST_ASSERT_EQ(0, strcmp(entry.href, "ch2.xhtml"));

  TEST_ASSERT_EQ(k_ra8_ok, epub_get_toc_entry(&book, 2U, &entry));
  TEST_ASSERT_EQ(0, strcmp(entry.title, "Appendix"));
  TEST_ASSERT_EQ(0, strcmp(entry.href, "appendix.xhtml"));

  /* Out-of-range entry index. */
  TEST_ASSERT_EQ(k_ra8_err_out_of_range, epub_get_toc_entry(&book, 99U, &entry));

  TEST_ASSERT_EQ(k_ra8_ok, epub_close(&book));
  TEST_END("epub parses the EPUB3 nav TOC");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches) @brief Verify toc to chapter behavior. @details Executes the toc to chapter scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_toc_to_chapter(void)
{
  TEST_BEGIN("epub resolves TOC entries to spine chapters");
  epub_book_t            book  = {};
  const epub_mem_media_t media = {.data = s_epub_buf, .size = s_epub_size};
  TEST_ASSERT_EQ(k_ra8_ok, epub_open(&media, nullptr, &book));

  uint16_t chapter = k_epub_poison_chapter;
  /* Entry 0 carries "ch1.xhtml#start" -- the fragment must be stripped. */
  TEST_ASSERT_EQ(k_ra8_ok, epub_toc_entry_to_chapter(&book, 0U, &chapter));
  TEST_ASSERT_EQ(0, chapter);
  /* Entry 1 is "ch2.xhtml" -> spine index 1. */
  TEST_ASSERT_EQ(k_ra8_ok, epub_toc_entry_to_chapter(&book, 1U, &chapter));
  TEST_ASSERT_EQ(1, chapter);

  /* Entry 2 ("appendix.xhtml") is not in the spine. */
  TEST_ASSERT_EQ(k_ra8_err_not_found, epub_toc_entry_to_chapter(&book, 2U, &chapter));
  /* Out-of-range TOC index. */
  TEST_ASSERT_EQ(k_ra8_err_out_of_range, epub_toc_entry_to_chapter(&book, 99U, &chapter));

  TEST_ASSERT_EQ(k_ra8_ok, epub_close(&book));
  TEST_END("epub resolves TOC entries to spine chapters");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches) @brief Verify get metadata behavior. @details Executes the get metadata scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_get_metadata(void)
{
  TEST_BEGIN("epub get_metadata returns Dublin Core");
  epub_book_t            book  = {};
  const epub_mem_media_t media = {.data = s_epub_buf, .size = s_epub_size};
  TEST_ASSERT_EQ(k_ra8_ok, epub_open(&media, nullptr, &book));
  epub_metadata_t meta = {};
  TEST_ASSERT_EQ(k_ra8_ok, epub_get_metadata(&book, &meta));
  TEST_ASSERT(meta.title != nullptr);
  TEST_ASSERT(meta.author != nullptr);
  TEST_ASSERT(meta.language != nullptr);
  TEST_ASSERT(meta.identifier != nullptr);
  TEST_ASSERT_EQ(0, strcmp(meta.title, "Test Book"));
  TEST_ASSERT_EQ(0, strcmp(meta.author, "Brighton Sikarskie"));
  TEST_ASSERT_EQ(0, strcmp(meta.language, "en"));
  TEST_ASSERT_EQ(0, strcmp(meta.identifier, "urn:test:book"));
  TEST_ASSERT_EQ(k_ra8_ok, epub_close(&book));
  TEST_END("epub get_metadata returns Dublin Core");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches) @brief Verify get cover image behavior. @details Executes the get cover image scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_get_cover_image(void)
{
  TEST_BEGIN("epub get_cover_image extracts raw bytes");
  epub_book_t            book  = {};
  const epub_mem_media_t media = {.data = s_epub_buf, .size = s_epub_size};
  TEST_ASSERT_EQ(k_ra8_ok, epub_open(&media, nullptr, &book));
  uint8_t buf[k_test_cover_buf_bytes];
  size_t  got = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, epub_get_cover_image(&book, buf, sizeof(buf), &got));
  TEST_ASSERT_EQ(k_test_synth_cover_bytes, got);
  TEST_ASSERT_EQ(0x89U, buf[0]);
  TEST_ASSERT_EQ(0x50U, buf[1]);
  TEST_ASSERT_EQ(0x4EU, buf[2]);
  TEST_ASSERT_EQ(0x47U, buf[3]);
  TEST_ASSERT_EQ(k_ra8_ok, epub_close(&book));
  TEST_END("epub get_cover_image extracts raw bytes");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches) @brief Verify render glyph paths behavior. @details Executes the render glyph paths scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_render_glyph_paths(void)
{
  TEST_BEGIN("epub render_glyph -- no font / bad font");
  epub_book_t            book  = {};
  const epub_mem_media_t media = {.data = s_epub_buf, .size = s_epub_size};
  TEST_ASSERT_EQ(k_ra8_ok, epub_open(&media, nullptr, &book));

  /* No font installed -- not_initialized. */
  uint8_t  bitmap[k_test_glyph_buf_bytes];
  uint32_t w = 0U;
  uint32_t h = 0U;
  TEST_ASSERT_EQ(k_ra8_err_not_initialized,
                 epub_render_glyph(&book,
                                   (int32_t)k_test_codepoint_a,
                                   (float)k_test_glyph_font_pixels,
                                   bitmap,
                                   sizeof(bitmap),
                                   &w,
                                   &h));

  /* Install a fake-but-too-small font -- invalid_size. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, epub_set_font(&book, s_synth.font_bytes, 1U));

  /* Install the synthetic 16-byte font; stbtt_InitFont will reject it
   * because it's not a real TTF, so render_glyph returns
   * validation_failed -- which is exactly the contract we want. */
  TEST_ASSERT_EQ(k_ra8_ok, epub_set_font(&book, s_synth.font_bytes, sizeof(s_synth.font_bytes)));
  TEST_ASSERT_EQ(k_ra8_err_validation_failed,
                 epub_render_glyph(&book,
                                   (int32_t)k_test_codepoint_a,
                                   (float)k_test_glyph_font_pixels,
                                   bitmap,
                                   sizeof(bitmap),
                                   &w,
                                   &h));

  /* Bad font size (0.0) -- invalid_arg. */
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_arg,
    epub_render_glyph(&book, (int32_t)k_test_codepoint_a, 0.0F, bitmap, sizeof(bitmap), &w, &h));

  TEST_ASSERT_EQ(k_ra8_ok, epub_close(&book));
  TEST_END("epub render_glyph -- no font / bad font");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches) @brief Verify null arg guards behavior. @details Executes the null arg guards scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_null_arg_guards(void)
{
  TEST_BEGIN("epub NULL-arg / pre-init guards");
  epub_book_t            book  = {};
  const epub_mem_media_t media = {.data = s_epub_buf, .size = s_epub_size};

  /* Pre-init guards: book->in_use == 0 on every accessor. */
  uint16_t n = 0U;
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, epub_get_chapter_count(&book, &n));
  uint8_t buf[16];
  size_t  got = 0U;
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, epub_load_chapter(&book, 0U, buf, sizeof(buf), &got));
  epub_metadata_t meta = {};
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, epub_get_metadata(&book, &meta));
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, epub_get_cover_image(&book, buf, sizeof(buf), &got));
  TEST_ASSERT_EQ(k_ra8_err_not_initialized,
                 epub_set_font(&book, s_synth.font_bytes, sizeof(s_synth.font_bytes)));

  /* NULL-arg guards. */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, epub_open(nullptr, nullptr, &book));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, epub_open(&media, nullptr, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, epub_close(nullptr));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, epub_get_chapter_count(nullptr, &n));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, epub_get_chapter_count(&book, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, epub_load_chapter(nullptr, 0U, buf, sizeof(buf), &got));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, epub_load_chapter(&book, 0U, nullptr, sizeof(buf), &got));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, epub_load_chapter(&book, 0U, buf, sizeof(buf), nullptr));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, epub_get_metadata(nullptr, &meta));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, epub_get_metadata(&book, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, epub_get_cover_image(nullptr, buf, sizeof(buf), &got));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 epub_set_font(nullptr, s_synth.font_bytes, sizeof(s_synth.font_bytes)));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, epub_set_font(&book, nullptr, sizeof(s_synth.font_bytes)));

  /* Invalid media payload. */
  const epub_mem_media_t bad_media = {.data = nullptr, .size = 0U};
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, epub_open(&bad_media, nullptr, &book));
  TEST_END("epub NULL-arg / pre-init guards");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches) @brief Verify open invalid zip behavior. @details Executes the open invalid zip scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_open_invalid_zip(void)
{
  TEST_BEGIN("epub open with garbage payload -> validation_failed");
  static const uint8_t k_garbage[] = {0x00U, 0x01U, 0x02U, 0x03U, 0x04U, 0x05U, 0x06U, 0x07U};
  epub_book_t          book;
  (void)memset(&book, 0xA5, sizeof(book));
  const epub_mem_media_t media = {.data = k_garbage, .size = sizeof(k_garbage)};
  TEST_ASSERT_EQ(k_ra8_err_validation_failed, epub_open(&media, nullptr, &book));
  TEST_ASSERT_EQ(0, book.in_use);
  const uint8_t* const raw = (const uint8_t*)&book;
  for (size_t i = 0U; i < sizeof(book); ++i) {
    TEST_ASSERT_EQ(0U, raw[i]);
  }
  TEST_END("epub open with garbage payload -> validation_failed");
}

/**
 * @test internal_test_two_live_books_isolate_miniz_arenas
 * @brief Simultaneous books keep independent arenas; close/reopen is reusable.
 *
 * @par MC/DC:
 * No compound decision is independently varied by this lifecycle test. Both
 * opens use the same valid archive, then only the first arena is closed and
 * reopened while metadata remains readable from the second. Required-pointer
 * and archive-validity guards therefore stay on their valid controls; the
 * test varies object lifetime and ownership, not boolean guard conditions. @details Executes the two live books isolate miniz arenas scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_two_live_books_isolate_miniz_arenas(void)
{
  TEST_BEGIN("epub: two live books isolate arenas + reopen");
  epub_book_t            first  = {};
  epub_book_t            second = {};
  const epub_mem_media_t media  = {.data = s_epub_buf, .size = s_epub_size};
  TEST_ASSERT_EQ(k_ra8_ok, epub_open(&media, "first.epub", &first));
  TEST_ASSERT_EQ(k_ra8_ok, epub_open(&media, "second.epub", &second));
  TEST_ASSERT(first.miniz_arena.base == &first.miniz_workspace.bytes[0]);
  TEST_ASSERT(second.miniz_arena.base == &second.miniz_workspace.bytes[0]);
  TEST_ASSERT(first.miniz_arena.base != second.miniz_arena.base);

  uint8_t first_body[k_test_chapter_buf_bytes];
  size_t  first_len = 0U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 epub_load_chapter(&first, 0U, first_body, sizeof(first_body), &first_len));
  TEST_ASSERT(first_len > 0U);
  TEST_ASSERT_EQ(k_ra8_ok, epub_close(&first));
  TEST_ASSERT(first.miniz_arena.base == nullptr);

  epub_metadata_t metadata = {};
  TEST_ASSERT_EQ(k_ra8_ok, epub_get_metadata(&second, &metadata));
  TEST_ASSERT_EQ(0, strcmp("Test Book", metadata.title));
  TEST_ASSERT_EQ(k_ra8_ok, epub_open(&media, "first-reused.epub", &first));
  TEST_ASSERT_EQ(k_ra8_ok, epub_close(&first));
  TEST_ASSERT_EQ(k_ra8_ok, epub_close(&second));
  TEST_END("epub: two live books isolate arenas + reopen");
}

/** @brief Sizing for the external-stylesheet consumer test (#140). */
enum : size_t {
  k_test_css_buf_bytes = 256U, /**< Scratch for an extracted .css resource. */
};

/**
 * @brief Demonstration `reflow_css_loader_fn`: the #140 consumer glue.
 *
 * @details Extracts a chapter's `<link href>` stylesheet bytes from the open
 * EPUB via ::epub_get_resource. An app wires this once with
 * `reflow_set_css_loader(engine, internal_epub_css_loader, &book)`; the engine then
 * pulls + parses each external sheet in document order while tokenizing.
 *
 * @param[in]  ctx       The open `epub_book_t*`.
 * @param[in]  href      Stylesheet href (not NUL-terminated).
 * @param[in]  href_len  Length of @p href.
 * @param[out] out_bytes Receives the CSS bytes (static scratch; read-only).
 * @param[out] out_len   Receives the CSS length.
 * @return k_ra8_ok on success; any error => the engine skips the sheet. @retval k_ra8_ok The fixture operation completed successfully. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static ra8_err_t internal_epub_css_loader(void*           ctx,
                                                       const char*     href,
                                                       uint32_t        href_len,
                                                       const uint8_t** out_bytes,
                                                       size_t*         out_len)
{
  static uint8_t s_css_scratch[k_test_css_buf_bytes];
  epub_book_t*   book = (epub_book_t*)ctx;
  char           path[k_epub_max_path_len];
  if (((size_t)href_len + 1U) > sizeof(path)) {
    return k_ra8_err_invalid_size;
  }
  (void)memcpy(path, href, (size_t)href_len);
  path[href_len]      = '\0';
  size_t          got = 0U;
  const ra8_err_t err = epub_get_resource(book, path, s_css_scratch, sizeof(s_css_scratch), &got);
  if (err != k_ra8_ok) {
    return err;
  }
  *out_bytes = s_css_scratch;
  *out_len   = got;
  return k_ra8_ok;
}

/**
 * @test internal_test_get_resource
 * @brief `epub_get_resource` extracts an arbitrary archive entry, and the
 *        #140 css-loader glue returns a chapter's external stylesheet (#140).
 *
 * @par MC/DC:
 * No compound decision in the test itself; the loader's
 * `href_len+1 > sizeof(path)` guard is the only branch and is covered by the
 * normal (short href) path plus `epub_get_resource`'s own guards. @details Executes the get resource scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_get_resource(void)
{
  TEST_BEGIN("epub get_resource + the #140 external-stylesheet css-loader");
  epub_book_t            book  = {};
  const epub_mem_media_t media = {.data = s_epub_buf, .size = s_epub_size};
  TEST_ASSERT_EQ(k_ra8_ok, epub_open(&media, nullptr, &book));

  uint8_t buf[k_test_css_buf_bytes];
  size_t  got = 0U;
  /* OPF-dir-relative resolution: "style.css" -> "OEBPS/style.css". */
  TEST_ASSERT_EQ(k_ra8_ok, epub_get_resource(&book, "style.css", buf, sizeof(buf), &got));
  TEST_ASSERT_EQ(strlen(s_synth.css), got);
  TEST_ASSERT(internal_bytes_equal(buf, s_synth.css, got));
  /* Archive-rooted fallback resolves too. */
  got = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, epub_get_resource(&book, "OEBPS/style.css", buf, sizeof(buf), &got));
  TEST_ASSERT_EQ(strlen(s_synth.css), got);
  /* Missing entry -> not_found; too-small buffer -> no_mem; nulls rejected. */
  TEST_ASSERT_EQ(k_ra8_err_not_found, epub_get_resource(&book, "nope.css", buf, sizeof(buf), &got));
  TEST_ASSERT_EQ(k_ra8_err_no_mem, epub_get_resource(&book, "style.css", buf, 1U, &got));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 epub_get_resource(nullptr, "style.css", buf, sizeof(buf), &got));

  /* The consumer glue returns the external sheet bytes for a `<link href>`. */
  const uint8_t* css     = nullptr;
  size_t         css_len = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, internal_epub_css_loader(&book, "style.css", 9U, &css, &css_len));
  TEST_ASSERT_EQ(strlen(s_synth.css), css_len);
  TEST_ASSERT(internal_bytes_equal(css, s_synth.css, css_len));

  TEST_ASSERT_EQ(k_ra8_ok, epub_close(&book));
  TEST_END("epub get_resource + the #140 external-stylesheet css-loader");
}

int main(void)
{
  internal_build_synth_epub();
  internal_test_open_close();
  internal_test_chapter_count();
  internal_test_load_chapter();
  internal_test_toc();
  internal_test_toc_to_chapter();
  internal_test_get_metadata();
  internal_test_get_cover_image();
  internal_test_get_resource();
  internal_test_render_glyph_paths();
  internal_test_null_arg_guards();
  internal_test_open_invalid_zip();
  internal_test_two_live_books_isolate_miniz_arenas();
  return 0;
}
