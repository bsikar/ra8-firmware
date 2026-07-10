/**
 * @file test_ra_stbtt_guard.c
 * @brief Host unit tests for the #217 sfnt table-directory bounds guard.
 *
 * @details
 * `ra_stbtt_sfnt_dir_in_bounds()` (libs/ra_reflow/src/ra_stbtt_guard.c) is the
 * validator run before `stbtt_InitFont()` so that a crafted @font-face font
 * cannot drive the out-of-bounds read in stb_truetype's `stbtt__find_table()`.
 * This file exercises it two ways:
 *
 *  1. Direct calls with crafted malformed fonts (truncated directory, an
 *     out-of-range table record, a header shorter than the offset table, and
 *     a null buffer) assert the validator REJECTS them cleanly -- returning
 *     `false`, never reading past the buffer -- and accepts well-formed input
 *     (the bundled Ahem face and a zero-table header).
 *
 *  2. End-to-end through the three attacker-reachable call sites that feed
 *     stb_truetype untrusted bytes -- `ra_reflow_bind_font()`,
 *     `ra_reflow_register_face()`, and `ra_epub_render_glyph()` (via
 *     `priv_font_init`) -- asserting each rejects a bad-directory font with
 *     its documented error and leaves prior state intact.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "fixture_ahem.h"
#include "ra_epub.h"
#include "ra_err.h"
#include "ra_reflow.h"
#include "ra_stbtt_guard.h"
#include "unity_minimal.h"

/**
 * @enum guard_test_dim_t
 * @brief Geometry and glyph knobs for the end-to-end call-site tests.
 */
typedef enum : uint16_t {
  k_g_vp_w        = 200U, /**< Reflow viewport + framebuffer width, pixels.  */
  k_g_vp_h        = 150U, /**< Reflow viewport + framebuffer height, pixels. */
  k_g_font_px     = 16U,  /**< Rasterisation height, pixels.                 */
  k_g_body_color  = 0U,   /**< Body text colour (unused by these asserts).   */
  k_g_link_color  = 0U,   /**< Link colour (unused by these asserts).        */
  k_g_glyph_dim   = 64U,  /**< Per-axis glyph scratch bound, pixels.         */
  k_g_codepoint_a = 0x41U, /**< Code point 'A' to rasterise. */
} guard_test_dim_t;

/**
 * @brief A crafted font with a valid sfnt tag but a directory that overruns
 *        the 16-byte buffer (numTables = 20 needs 12 + 20*16 = 332 bytes).
 * @details `stbtt_GetFontOffsetForIndex` returns 0 for the 0x00010000 tag, so
 *          the guard is reached with fontstart 0 and rejects at the
 *          directory-fits check. 16 bytes clears the >= 16 length guards.
 */
static const uint8_t s_bad_dir_truncated[16] = {
  0x00U, 0x01U, 0x00U, 0x00U, /* sfnt version 1.0 -> offset-for-index == 0 */
  0x00U, 0x14U,               /* numTables = 20 -> directory 332 B > 16 B  */
  0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
  0x00U, 0x00U, 0x00U, 0x00U,
};

/**
 * @brief A crafted font whose single directory record fits but declares a
 *        table at offset 0x10000000 -- far past the 28-byte buffer.
 * @details Directory-fits check passes (12 + 1*16 == 28); the per-record
 *          `offset + length <= len` check rejects it.
 */
static const uint8_t s_bad_dir_record[28] = {
  0x00U, 0x01U, 0x00U, 0x00U,               /* sfnt version 1.0            */
  0x00U, 0x01U,                             /* numTables = 1               */
  0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, /* search hints (ignored)      */
  0x00U, 0x00U, 0x00U, 0x00U,               /* record 0: tag               */
  0x00U, 0x00U, 0x00U, 0x00U,               /* record 0: checksum          */
  0x10U, 0x00U, 0x00U, 0x00U,               /* record 0: offset 0x10000000 */
  0x00U, 0x00U, 0x01U, 0x00U,               /* record 0: length 0x100      */
};

/**
 * @brief A 10-byte buffer: shorter than the 12-byte sfnt offset table.
 * @details The header-fits check rejects it before numTables is ever read.
 */
static const uint8_t s_hdr_too_small[10] = {
  0x00U, 0x01U, 0x00U, 0x00U, /* sfnt version 1.0               */
  0x00U, 0x05U,               /* numTables field, never reached */
  0x00U, 0x00U, 0x00U, 0x00U,
};

/**
 * @brief A minimal, in-bounds 12-byte offset table declaring zero tables.
 * @details Memory-safe (nothing to walk), so the guard accepts it even though
 *          `stbtt_InitFont` would later reject it for lacking real tables --
 *          the guard is a memory-safety gate, not a semantic validator.
 */
static const uint8_t s_zero_tables[12] = {
  0x00U, 0x01U, 0x00U, 0x00U, /* sfnt version 1.0 */
  0x00U, 0x00U,               /* numTables = 0    */
  0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
};

/**
 * @brief Initialise a reflow engine bound to the valid Ahem face.
 *
 * @param[out] engine Engine to initialise; set up with the Ahem default face.
 * @return None.
 * @pre @p engine is non-null.
 * @pre The bundled Ahem face is a well-formed sfnt.
 * @post @p engine is in use with Ahem as its bound face.
 * @post `engine->face_count == 0`.
 * @note Test helper; aborts the process on unexpected init failure.
 * @since 0.1.0
 */
static void priv_init_ahem_engine(ra_reflow_t* engine)
{
  TEST_ASSERT_EQ(k_ra_ok,
                 ra_reflow_init((uint16_t)k_g_vp_w,
                                (uint16_t)k_g_vp_h,
                                k_fixture_ahem,
                                (size_t)k_fixture_ahem_len,
                                (uint16_t)k_g_font_px,
                                (uint32_t)k_g_body_color,
                                (uint32_t)k_g_link_color,
                                engine));
}

/**
 * @test test_guard_rejects_null_data
 *
 * @par MC/DC:
 * Decision: `if (data == nullptr)`
 * (1 condition, libs/ra_reflow/src/ra_stbtt_guard.c@ra_stbtt_sfnt_dir_in_bounds)
 * - Vector T (this test): data = nullptr -> true  -> return false.
 * - Vector F (test_guard_accepts_valid_font): data != nullptr -> false ->
 *   continue past the guard. The T/F pair gives complete MC/DC for the
 *   1-condition null decision.
 */
static void test_guard_rejects_null_data(void)
{
  TEST_BEGIN("ra_stbtt guard: null data rejected");
  TEST_ASSERT(!ra_stbtt_sfnt_dir_in_bounds(nullptr, (size_t)k_fixture_ahem_len, 0U));
  TEST_END("ra_stbtt guard: null data rejected");
}

/**
 * @test test_guard_rejects_short_header
 *
 * @par MC/DC:
 * Decision: `if ((start + 12) > buf_len)`  (the offset-table-fits check;
 * 1 condition, libs/ra_reflow/src/ra_stbtt_guard.c@ra_stbtt_sfnt_dir_in_bounds)
 * - Vector T (this test): a 10-byte buffer, 0 + 12 > 10 -> true -> reject
 *   before numTables is read.
 * - Vector F (test_guard_accepts_valid_font): a >= 12-byte buffer -> false ->
 *   proceed to read numTables. Complete MC/DC for the 1-condition check.
 */
static void test_guard_rejects_short_header(void)
{
  TEST_BEGIN("ra_stbtt guard: header shorter than offset table rejected");
  TEST_ASSERT(!ra_stbtt_sfnt_dir_in_bounds(s_hdr_too_small, sizeof s_hdr_too_small, 0U));
  TEST_END("ra_stbtt guard: header shorter than offset table rejected");
}

/**
 * @test test_guard_rejects_truncated_directory
 *
 * @par MC/DC:
 * Decision: `if (dir_end > buf_len)`  (the directory-fits check; 1 condition,
 * libs/ra_reflow/src/ra_stbtt_guard.c@ra_stbtt_sfnt_dir_in_bounds)
 * - Vector T (this test): numTables = 20 in a 16-byte buffer, dir_end 332 >
 *   16 -> true -> reject before any record is read.
 * - Vector F (test_guard_accepts_valid_font): Ahem's directory fits -> false.
 *   Complete MC/DC for the 1-condition check.
 */
static void test_guard_rejects_truncated_directory(void)
{
  TEST_BEGIN("ra_stbtt guard: truncated directory rejected");
  TEST_ASSERT(!ra_stbtt_sfnt_dir_in_bounds(s_bad_dir_truncated, sizeof s_bad_dir_truncated, 0U));
  TEST_END("ra_stbtt guard: truncated directory rejected");
}

/**
 * @test test_guard_rejects_out_of_range_record
 *
 * @par MC/DC:
 * Decision: `if ((t_off + t_len) > buf_len)`  (the per-record extent check;
 * 1 condition, libs/ra_reflow/src/ra_stbtt_guard.c@ra_stbtt_sfnt_dir_in_bounds)
 * - Vector T (this test): record 0 declares offset 0x10000000 + length 0x100,
 *   far past the 28-byte buffer -> true -> reject.
 * - Vector F (test_guard_accepts_valid_font): every Ahem record fits -> false
 *   for all iterations. Complete MC/DC for the 1-condition record check.
 */
static void test_guard_rejects_out_of_range_record(void)
{
  TEST_BEGIN("ra_stbtt guard: out-of-range table record rejected");
  TEST_ASSERT(!ra_stbtt_sfnt_dir_in_bounds(s_bad_dir_record, sizeof s_bad_dir_record, 0U));
  TEST_END("ra_stbtt guard: out-of-range table record rejected");
}

/**
 * @test test_guard_accepts_valid_font
 *
 * @par MC/DC:
 * Provides the all-false control (accept) vector for every 1-condition
 * decision in ra_stbtt_sfnt_dir_in_bounds
 * (libs/ra_reflow/src/ra_stbtt_guard.c@ra_stbtt_sfnt_dir_in_bounds):
 * - The bundled Ahem face: data != nullptr, header fits, directory fits, and
 *   every record's extent fits -> all four checks false -> return true. This
 *   supplies the F vector paired with the T vectors in the four reject tests
 *   above, completing MC/DC for each check, and exercises the record loop's
 *   in-bounds arm across many records.
 * - A 12-byte zero-table header: header + directory fit and the record loop
 *   is skipped (numTables == 0) -> return true, covering the loop-not-taken
 *   edge.
 */
static void test_guard_accepts_valid_font(void)
{
  TEST_BEGIN("ra_stbtt guard: valid fonts accepted");
  TEST_ASSERT(ra_stbtt_sfnt_dir_in_bounds(k_fixture_ahem, (size_t)k_fixture_ahem_len, 0U));
  TEST_ASSERT(ra_stbtt_sfnt_dir_in_bounds(s_zero_tables, sizeof s_zero_tables, 0U));
  TEST_END("ra_stbtt guard: valid fonts accepted");
}

/**
 * @test test_bind_font_rejects_bad_directory
 *
 * @par MC/DC:
 * Decision: `if (!ra_stbtt_sfnt_dir_in_bounds(font_data, font_len, offset))`
 * (1 condition, guarded by `if (offset >= 0)`;
 * libs/ra_reflow/src/ra_reflow_layout_driver.c@ra_reflow_bind_font)
 * - Vector T (this test): a valid-tag / bad-directory blob -> offset >= 0 and
 *   guard returns false -> `!false` true -> k_ra_err_not_supported, prior face
 *   preserved.
 * - Vector F (a valid font, covered where bind_font binds Ahem in other
 *   tests): guard returns true -> `!true` false -> proceed to InitFont.
 */
static void test_bind_font_rejects_bad_directory(void)
{
  TEST_BEGIN("ra_reflow_bind_font: bad sfnt directory rejected");
  ra_reflow_t engine = {};
  priv_init_ahem_engine(&engine);
  TEST_ASSERT(engine.font_data == k_fixture_ahem);

  TEST_ASSERT_EQ(k_ra_err_not_supported,
                 ra_reflow_bind_font(&engine, s_bad_dir_truncated, sizeof s_bad_dir_truncated));
  /* Graceful degradation: the engine keeps its prior (Ahem) face. */
  TEST_ASSERT(engine.font_data == k_fixture_ahem);

  (void)ra_reflow_close(&engine);
  TEST_END("ra_reflow_bind_font: bad sfnt directory rejected");
}

/**
 * @test test_register_face_rejects_bad_directory
 *
 * @par MC/DC:
 * Decision: `if (!ra_stbtt_sfnt_dir_in_bounds(blob, len, offset))`
 * (1 condition, guarded by `if (offset >= 0)`;
 * libs/ra_reflow/src/ra_reflow_layout_driver.c@ra_reflow_register_face)
 * - Vector T (this test): a valid-tag / bad-directory blob -> offset >= 0 and
 *   guard returns false -> `!false` true -> k_ra_err_not_supported, face_count
 *   unchanged.
 * - Vector F (a valid @font-face, covered where register_face accepts Ahem in
 *   other tests): guard returns true -> proceed to InitFont.
 */
static void test_register_face_rejects_bad_directory(void)
{
  TEST_BEGIN("ra_reflow_register_face: bad sfnt directory rejected");
  ra_reflow_t engine = {};
  priv_init_ahem_engine(&engine);
  TEST_ASSERT_EQ(0, (int64_t)engine.face_count);

  TEST_ASSERT_EQ(k_ra_err_not_supported,
                 ra_reflow_register_face(&engine, 0U, s_bad_dir_record, sizeof s_bad_dir_record));
  /* Rejected face is not recorded. */
  TEST_ASSERT_EQ(0, (int64_t)engine.face_count);

  (void)ra_reflow_close(&engine);
  TEST_END("ra_reflow_register_face: bad sfnt directory rejected");
}

/**
 * @test test_epub_render_glyph_rejects_bad_directory
 *
 * @par MC/DC:
 * Decision: `if (!ra_stbtt_sfnt_dir_in_bounds(book->font_data, book->font_size,
 * offset))` (1 condition, reached after `offset >= 0`;
 * libs/ra_epub/src/ra_epub_chapter.c@priv_font_init)
 * - Vector T (this test): a valid-tag / bad-directory book font -> guard
 *   returns false -> `!false` true -> k_ra_err_validation_failed from
 *   ra_epub_render_glyph, without stb_truetype ever walking the directory.
 * - Vector F (a valid book font, covered by the render tests in
 *   test_ra_epub_chapter_cov.c): guard returns true -> proceed to InitFont.
 */
static void test_epub_render_glyph_rejects_bad_directory(void)
{
  TEST_BEGIN("ra_epub_render_glyph: bad sfnt directory rejected");
  ra_epub_book_t book = {};
  book.in_use         = 1U;
  TEST_ASSERT_EQ(k_ra_ok,
                 ra_epub_set_font(&book, s_bad_dir_record, sizeof s_bad_dir_record));

  uint8_t  bitmap[(size_t)k_g_glyph_dim * (size_t)k_g_glyph_dim] = {};
  uint32_t w = 0U;
  uint32_t h = 0U;
  TEST_ASSERT_EQ(k_ra_err_validation_failed,
                 ra_epub_render_glyph(&book,
                                      (int32_t)k_g_codepoint_a,
                                      (float)k_g_font_px,
                                      bitmap,
                                      sizeof bitmap,
                                      &w,
                                      &h));
  TEST_END("ra_epub_render_glyph: bad sfnt directory rejected");
}

int main(void)
{
  test_guard_rejects_null_data();
  test_guard_rejects_short_header();
  test_guard_rejects_truncated_directory();
  test_guard_rejects_out_of_range_record();
  test_guard_accepts_valid_font();
  test_bind_font_rejects_bad_directory();
  test_register_face_rejects_bad_directory();
  test_epub_render_glyph_rejects_bad_directory();
  (void)fprintf(stderr, "[OK ] test_ra_stbtt_guard.c\n");
  return 0;
}
