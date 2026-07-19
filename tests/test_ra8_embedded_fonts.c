/**
 * @file test_ra8_embedded_fonts.c
 * @brief #109 foundation: enumerate + extract an EPUB-embedded font (ra8_epub)
 *        and bind it as the reflow engine's active face (ra8_reflow).
 *
 * @details
 * Covers the additive foundation of issue #109 (items 1 + 4 -- the common
 * "the EPUB ships one typeface" case + graceful degradation). The face
 * *matching* half (bold/italic/family selection across multiple faces) is
 * deferred to #109's remainder, blocked on the `@font-face` prerequisite #142.
 *
 * A synthetic EPUB is built in memory (miniz writer) embedding the baked Ahem
 * test font as a `font/ttf` manifest item. The tests then:
 *   1. enumerate (`ra8_epub_get_embedded_font_count`) + extract
 *      (`ra8_epub_get_embedded_font`) the font and CRC-check the bytes;
 *   2. bind the extracted bytes into a reflow engine (`ra8_reflow_bind_font`),
 *      measure a laid-out glyph (proving the embedded face is active), and
 *      exercise the validation decision (MC/DC) + graceful fallback.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * [Ring 4 / EPUB] {World: NS}
 *
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "fixture_ahem.h"
#include "miniz.h"
#include "ra8_epub.h"
#include "ra8_err.h"
#include "ra8_reflow.h"
#include "unity_minimal.h"

/** @brief Fill byte for the deliberately invalid font blob. */
typedef enum : uint8_t {
  k_fonts_fill_garbage = 0xA5U, /**< Carries no sfnt tag, so lookup fails. */
} fonts_fill_t;

/** @brief Local sizing + measurement constants (no magic numbers). */
typedef enum : uint32_t {
  k_tef_epub_cap     = 65536U,      /**< Synthetic .epub heap buffer.         */
  k_tef_font_cap     = 32768U,      /**< Extracted-font scratch buffer.       */
  k_tef_viewport_w   = 600U,        /**< Layout viewport width, px.           */
  k_tef_viewport_h   = 800U,        /**< Layout viewport height, px.          */
  k_tef_font_px      = 16U,         /**< Body font size for the layout.       */
  k_tef_body_color   = 0x000000U,   /**< Black body text.                     */
  k_tef_link_color   = 0x0000FFU,   /**< Blue links.                          */
  k_tef_crc_init     = 0xFFFFFFFFU, /**< CRC-32 seed.                         */
  k_tef_crc_poly     = 0xEDB88320U, /**< CRC-32 reflected polynomial.         */
  k_tef_crc_bits     = 8U,          /**< Bits folded per byte.                */
  k_tef_garbage_len  = 32U,         /**< Length of crafted invalid blobs.     */
  k_tef_min_m_glyphs = 2U,          /**< Glyphs needed to measure an advance. */
} tef_const_t;

/** @brief Backing store for the synthetic .epub. */
static uint8_t s_epub_buf[k_tef_epub_cap];
/** @brief Synthetic .epub length, bytes. */
static size_t s_epub_size;
/** @brief Scratch for the extracted embedded font. */
static uint8_t s_extracted_font[k_tef_font_cap];
/** @brief Reflow engine under test (file-scope: large struct). */
static ra8_reflow_t s_engine;

/** @brief Minimal EPUB OPF declaring one chapter + one embedded font. */
static const char* const k_opf =
  "<?xml version=\"1.0\"?>"
  "<package xmlns=\"http://www.idpf.org/2007/opf\" version=\"3.0\" "
  "unique-identifier=\"bid\">"
  "<metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\">"
  "<dc:title>Font Book</dc:title>"
  "<dc:identifier id=\"bid\">urn:uuid:font-1</dc:identifier>"
  "</metadata>"
  "<manifest>"
  "<item id=\"ch1\" href=\"ch1.xhtml\" media-type=\"application/xhtml+xml\"/>"
  "<item id=\"face\" href=\"fonts/face.ttf\" media-type=\"font/ttf\"/>"
  "</manifest>"
  "<spine><itemref idref=\"ch1\"/></spine>"
  "</package>";

static const char* const k_container =
  "<?xml version=\"1.0\"?>"
  "<container version=\"1.0\" "
  "xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\">"
  "<rootfiles><rootfile full-path=\"OEBPS/content.opf\" "
  "media-type=\"application/oebps-package+xml\"/></rootfiles></container>";

static const char* const k_chapter  = "<html><body><p>MMMM</p></body></html>";
static const char* const k_mimetype = "application/epub+zip";

/** @brief CRC-32 (reflected, poly 0xEDB88320) over @p data. */
static uint32_t tef_crc32(const uint8_t* data, size_t len)
{
  uint32_t crc = (uint32_t)k_tef_crc_init;
  for (size_t i = 0U; i < len; i++) {
    crc ^= (uint32_t)data[i];
    for (uint32_t bit = 0U; bit < (uint32_t)k_tef_crc_bits; bit++) {
      const uint32_t mask = (uint32_t)(-(int32_t)(crc & 1U));
      crc                 = (crc >> 1U) ^ ((uint32_t)k_tef_crc_poly & mask);
    }
  }
  return ~crc;
}

/** @brief Build the synthetic font-bearing .epub into ::s_epub_buf. */
static void build_font_epub(void)
{
  mz_zip_archive zip;
  memset(&zip, 0, sizeof(zip));
  s_epub_size = 0U;

  TEST_ASSERT(mz_zip_writer_init_heap(&zip, 0U, (size_t)k_tef_epub_cap) == MZ_TRUE);
  TEST_ASSERT(
    mz_zip_writer_add_mem(&zip, "mimetype", k_mimetype, strlen(k_mimetype), MZ_NO_COMPRESSION) ==
    MZ_TRUE);
  TEST_ASSERT(mz_zip_writer_add_mem(&zip,
                                    "META-INF/container.xml",
                                    k_container,
                                    strlen(k_container),
                                    MZ_DEFAULT_COMPRESSION) == MZ_TRUE);
  TEST_ASSERT(mz_zip_writer_add_mem(&zip,
                                    "OEBPS/content.opf",
                                    k_opf,
                                    strlen(k_opf),
                                    MZ_DEFAULT_COMPRESSION) == MZ_TRUE);
  TEST_ASSERT(mz_zip_writer_add_mem(&zip,
                                    "OEBPS/ch1.xhtml",
                                    k_chapter,
                                    strlen(k_chapter),
                                    MZ_DEFAULT_COMPRESSION) == MZ_TRUE);
  TEST_ASSERT(mz_zip_writer_add_mem(&zip,
                                    "OEBPS/fonts/face.ttf",
                                    k_fixture_ahem,
                                    (size_t)k_fixture_ahem_len,
                                    MZ_NO_COMPRESSION) == MZ_TRUE);

  void*  heap_buf  = nullptr;
  size_t heap_size = 0U;
  TEST_ASSERT(mz_zip_writer_finalize_heap_archive(&zip, &heap_buf, &heap_size) == MZ_TRUE);
  TEST_ASSERT(heap_buf != nullptr);
  TEST_ASSERT(heap_size > 0U && heap_size <= sizeof(s_epub_buf));
  memcpy(s_epub_buf, heap_buf, heap_size);
  s_epub_size = heap_size;
  mz_zip_writer_end(&zip);
}

/**
 * @test ra8_epub enumerates the manifest font item and extracts its bytes.
 */
static void test_enumerate_and_extract(void)
{
  TEST_BEGIN("embedded_fonts: enumerate + extract");
  const ra8_epub_mem_media_t media = {.data = s_epub_buf, .size = s_epub_size};
  ra8_epub_book_t            book  = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_open(&media, "fonts.epub", &book));

  uint16_t count = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_get_embedded_font_count(&book, &count));
  TEST_ASSERT_EQ(1, count);

  size_t got = 0U;
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_epub_get_embedded_font(&book, 0U, s_extracted_font, sizeof(s_extracted_font), &got));
  TEST_ASSERT_EQ(k_fixture_ahem_len, got);
  TEST_ASSERT_EQ(tef_crc32(k_fixture_ahem, (size_t)k_fixture_ahem_len),
                 tef_crc32(s_extracted_font, got));

  /* Index past the end is rejected, not a buffer overrun. */
  size_t over = 0U;
  TEST_ASSERT_EQ(
    k_ra8_err_out_of_range,
    ra8_epub_get_embedded_font(&book, 1U, s_extracted_font, sizeof(s_extracted_font), &over));
  (void)ra8_epub_close(&book);
  TEST_END("embedded_fonts: enumerate + extract");
}

/** @brief Lay out the chapter and return the advance between the first two glyphs. */
static int32_t priv_first_advance(void)
{
  uint32_t pages = 0U;
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_reflow_layout_chapter(&s_engine, (const uint8_t*)k_chapter, strlen(k_chapter), &pages));
  TEST_ASSERT(s_engine.glyph_count >= (uint32_t)k_tef_min_m_glyphs);
  return s_engine.glyphs[1].x - s_engine.glyphs[0].x;
}

/**
 * @test ra8_reflow_bind_font binds the embedded face; a laid-out glyph is
 *       measurable from it, and the validation decision degrades gracefully.
 *
 * @par MC/DC:
 * Decision: `if (offset < 0 || stbtt_InitFont(&probe, font_data, offset) == 0)`
 *   (2 conditions, libs/ra8_reflow/src/ra8_reflow_layout.c@ra8_reflow_bind_font)
 * - Vector 1: valid sfnt -> offset>=0, InitFont!=0 -> false  (accept; control)
 * - Vector 2: garbage (no sfnt tag) -> offset<0 -> true       (varies cond 1)
 * - Vector 3: sfnt tag + corrupt body -> offset>=0, InitFont==0 -> true (cond 2)
 * Vectors 1+2 prove `offset<0` independently flips the outcome; 1+3 prove the
 * same for the InitFont result. N+1 = 3 vectors for N=2: minimal MC/DC. On every
 * reject the engine keeps its prior face (graceful fallback) -- asserted below.
 */
static void test_bind_measure_and_mcdc(void)
{
  TEST_BEGIN("embedded_fonts: bind + measure + mcdc");

  /* Extract the embedded font from the EPUB into a distinct buffer. */
  const ra8_epub_mem_media_t media = {.data = s_epub_buf, .size = s_epub_size};
  ra8_epub_book_t            book  = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_open(&media, nullptr, &book));
  size_t got = 0U;
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_epub_get_embedded_font(&book, 0U, s_extracted_font, sizeof(s_extracted_font), &got));
  (void)ra8_epub_close(&book);

  /* Init with the baked face, then bind the EPUB-extracted bytes (a distinct
   * buffer): the bind must swap the active face to the embedded font. */
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_reflow_init((uint16_t)k_tef_viewport_w,
                                 (uint16_t)k_tef_viewport_h,
                                 k_fixture_ahem,
                                 (size_t)k_fixture_ahem_len,
                                 (uint16_t)k_tef_font_px,
                                 (uint32_t)k_tef_body_color,
                                 (uint32_t)k_tef_link_color,
                                 &s_engine));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_reflow_bind_font(&s_engine, s_extracted_font, got));
  TEST_ASSERT(s_engine.font_data == s_extracted_font); /* face swapped to embedded bytes */
  TEST_ASSERT_EQ(got, s_engine.font_len);

  /* Measure a glyph from the embedded face: a positive inter-glyph advance
   * proves the bound face rasterises real metrics. */
  TEST_ASSERT(priv_first_advance() > 0);

  /* --- MC/DC for the validation decision + graceful fallback ---------- */
  static uint8_t s_garbage[k_tef_garbage_len];
  memset(s_garbage, k_fonts_fill_garbage, sizeof(s_garbage)); /* no sfnt tag -> offset < 0 */
  static uint8_t s_tagged_corrupt[k_tef_garbage_len];
  memset(s_tagged_corrupt, 0x00, sizeof(s_tagged_corrupt));
  s_tagged_corrupt[1] = 0x01; /* 0x00010000 sfnt version -> offset 0, InitFont fails */

  /* Vector 2: cond1 true. Vector 3: cond2 true. Face must be preserved. */
  TEST_ASSERT_EQ(k_ra8_err_not_supported,
                 ra8_reflow_bind_font(&s_engine, s_garbage, sizeof(s_garbage)));
  TEST_ASSERT(s_engine.font_data == s_extracted_font); /* unchanged */
  TEST_ASSERT_EQ(k_ra8_err_not_supported,
                 ra8_reflow_bind_font(&s_engine, s_tagged_corrupt, sizeof(s_tagged_corrupt)));
  TEST_ASSERT(s_engine.font_data == s_extracted_font); /* unchanged */

  /* Single-condition guards. */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_reflow_bind_font(nullptr, s_extracted_font, got));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_reflow_bind_font(&s_engine, nullptr, got));
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_size,
    ra8_reflow_bind_font(&s_engine, s_extracted_font, (size_t)k_ra8_reflow_min_font_bytes - 1U));

  (void)ra8_reflow_close(&s_engine);
  TEST_END("embedded_fonts: bind + measure + mcdc");
}

int main(void)
{
  build_font_epub();
  test_enumerate_and_extract();
  test_bind_measure_and_mcdc();
  return 0;
}
