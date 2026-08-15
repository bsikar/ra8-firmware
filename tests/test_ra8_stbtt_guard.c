/**
 * @file test_ra8_stbtt_guard.c
 * @brief Host unit tests for the #217 sfnt table-directory bounds guard.
 *
 * @details
 * `ra8_stbtt_sfnt_dir_in_bounds()` (libs/ra8_reflow/src/ra8_stbtt_guard.c) is the
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
 *     stb_truetype untrusted bytes -- `ra8_reflow_bind_font()`,
 *     `ra8_reflow_register_face()`, and `ra8_epub_render_glyph()` (via
 *     `priv_font_init`) -- asserting each rejects a bad-directory font with
 *     its documented error and leaves prior state intact.
 *
 *  3. A deeper regression: a font that PASSES the directory guard and
 *     `stbtt_InitFont` but whose `loca` table resolves a glyph to an
 *     out-of-bounds `glyf` offset. This is the `fuzz_ra8_stbtt` crash that the
 *     directory guard alone did not cover; the stb_truetype bounds patch (see
 *     docs/SOUP/stb.md) rejects the glyph instead of reading past the buffer.
 *
 * @see docs/SOUP/stb.md  stb_truetype memory-safety hardening record.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>

#include "fixture_ahem.h"
#include "ra8_epub.h"
#include "ra8_err.h"
#include "ra8_reflow.h"
#include "ra8_stbtt_guard.h"
#include "unity_minimal.h"

/**
 * @enum t_guard_t
 * @brief Bitmap-dimension out-parameter seed.
 */
typedef enum : uint16_t {
  k_t_dim_unset = 0xFFFFU, /**< Pre-set width/height; a rasterise that refuses
                                its input must leave both rather than report a
                                plausible size.                                   */
} t_guard_t;

/**
 * @enum guard_test_dim_t
 * @brief Geometry and glyph knobs for the end-to-end call-site tests.
 */
typedef enum : uint16_t {
  k_g_vp_w        = 200U,  /**< Reflow viewport + framebuffer width, pixels.  */
  k_g_vp_h        = 150U,  /**< Reflow viewport + framebuffer height, pixels. */
  k_g_font_px     = 16U,   /**< Rasterisation height, pixels.                 */
  k_g_body_color  = 0U,    /**< Body text colour (unused by these asserts).   */
  k_g_link_color  = 0U,    /**< Link colour (unused by these asserts).        */
  k_g_glyph_dim   = 64U,   /**< Per-axis glyph scratch bound, pixels.         */
  k_g_codepoint_a = 0x41U, /**< Code point 'A' to rasterise.                  */
} guard_test_dim_t;

/**
 * @brief A crafted font with a valid sfnt tag but a directory that overruns
 *        the 16-byte buffer (numTables = 20 needs 12 + 20*16 = 332 bytes).
 * @details `stbtt_GetFontOffsetForIndex` returns 0 for the 0x00010000 tag, so
 *          the guard is reached with fontstart 0 and rejects at the
 *          directory-fits check. 16 bytes clears the >= 16 length guards.
 */
static const uint8_t s_bad_dir_truncated[16] = {
  0x00U,
  0x01U,
  0x00U,
  0x00U, /* sfnt version 1.0 -> offset-for-index == 0 */
  0x00U,
  0x14U, /* numTables = 20 -> directory 332 B > 16 B */
  0x00U,
  0x00U,
  0x00U,
  0x00U,
  0x00U,
  0x00U,
  0x00U,
  0x00U,
  0x00U,
  0x00U,
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
 * @brief A crafted font whose cmap record fits but whose in-table encoding
 *        count overruns the buffer (the fuzz-found #239 out-of-bounds read).
 * @details The top-level directory is well-formed: one record tagged "cmap" at
 *          offset 28, declared length 4 (28 + 4 == 32 == buffer len), so checks
 *          (1)-(3) pass. The cmap table's internal numTables field is 0xFFFF, so
 *          `stbtt_InitFont` would stride `cmap + 4 + 8 * 0xFFFF` (~512 KiB) past
 *          the buffer. Check (4) reads that count and rejects it.
 */
static const uint8_t s_bad_cmap_subdir[32] = {
  0x00U, 0x01U, 0x00U, 0x00U,               /* sfnt version 1.0              */
  0x00U, 0x01U,                             /* numTables = 1                 */
  0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, /* search hints (ignored)        */
  0x63U, 0x6DU, 0x61U, 0x70U,               /* record 0: tag "cmap"          */
  0x00U, 0x00U, 0x00U, 0x00U,               /* record 0: checksum            */
  0x00U, 0x00U, 0x00U, 0x1CU,               /* record 0: offset 28           */
  0x00U, 0x00U, 0x00U, 0x04U,               /* record 0: length 4 (fits)     */
  0x00U, 0x00U,                             /* cmap version 0                */
  0xFFU, 0xFFU,                             /* cmap numTables = 65535 (huge) */
};

/**
 * @brief A 10-byte buffer: shorter than the 12-byte sfnt offset table.
 * @details The header-fits check rejects it before numTables is ever read.
 */
static const uint8_t s_hdr_too_small[10] = {
  0x00U,
  0x01U,
  0x00U,
  0x00U, /* sfnt version 1.0 */
  0x00U,
  0x05U, /* numTables field, never reached */
  0x00U,
  0x00U,
  0x00U,
  0x00U,
};

/**
 * @brief A minimal, in-bounds 12-byte offset table declaring zero tables.
 * @details Memory-safe (nothing to walk), so the guard accepts it even though
 *          `stbtt_InitFont` would later reject it for lacking real tables --
 *          the guard is a memory-safety gate, not a semantic validator.
 */
static const uint8_t s_zero_tables[12] = {
  0x00U,
  0x01U,
  0x00U,
  0x00U, /* sfnt version 1.0 */
  0x00U,
  0x00U, /* numTables = 0 */
  0x00U,
  0x00U,
  0x00U,
  0x00U,
  0x00U,
  0x00U,
};

/**
 * @brief Initialise a reflow engine bound to the valid Ahem face.
 *
 * @param[out] engine Engine to initialise; set up with the Ahem default face.
 * @pre @p engine is non-null.
 * @pre The bundled Ahem face is a well-formed sfnt.
 * @post @p engine is in use with Ahem as its bound face.
 * @post `engine->face_count == 0`.
 * @note Test helper; aborts the process on unexpected init failure.
 * @since 0.1.0
 */
static void priv_init_ahem_engine(ra8_reflow_t* engine)
{
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_reflow_init((uint16_t)k_g_vp_w,
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
 * (1 condition, libs/ra8_reflow/src/ra8_stbtt_guard.c@ra8_stbtt_sfnt_dir_in_bounds)
 * - Vector T (this test): data = nullptr -> true  -> return false.
 * - Vector F (test_guard_accepts_valid_font): data != nullptr -> false ->
 *   continue past the guard. The T/F pair gives complete MC/DC for the
 *   1-condition null decision.
 */
static void test_guard_rejects_null_data(void)
{
  TEST_BEGIN("ra8_stbtt guard: null data rejected");
  TEST_ASSERT(!ra8_stbtt_sfnt_dir_in_bounds(nullptr, (size_t)k_fixture_ahem_len, 0U));
  TEST_END("ra8_stbtt guard: null data rejected");
}

/**
 * @test test_guard_rejects_short_header
 *
 * @par MC/DC:
 * Decision: `if ((start + 12) > buf_len)`  (the offset-table-fits check;
 * 1 condition, libs/ra8_reflow/src/ra8_stbtt_guard.c@ra8_stbtt_sfnt_dir_in_bounds)
 * - Vector T (this test): a 10-byte buffer, 0 + 12 > 10 -> true -> reject
 *   before numTables is read.
 * - Vector F (test_guard_accepts_valid_font): a >= 12-byte buffer -> false ->
 *   proceed to read numTables. Complete MC/DC for the 1-condition check.
 */
static void test_guard_rejects_short_header(void)
{
  TEST_BEGIN("ra8_stbtt guard: header shorter than offset table rejected");
  TEST_ASSERT(!ra8_stbtt_sfnt_dir_in_bounds(s_hdr_too_small, sizeof s_hdr_too_small, 0U));
  TEST_END("ra8_stbtt guard: header shorter than offset table rejected");
}

/**
 * @test test_guard_rejects_truncated_directory
 *
 * @par MC/DC:
 * Decision: `if (dir_end > buf_len)`  (the directory-fits check; 1 condition,
 * libs/ra8_reflow/src/ra8_stbtt_guard.c@ra8_stbtt_sfnt_dir_in_bounds)
 * - Vector T (this test): numTables = 20 in a 16-byte buffer, dir_end 332 >
 *   16 -> true -> reject before any record is read.
 * - Vector F (test_guard_accepts_valid_font): Ahem's directory fits -> false.
 *   Complete MC/DC for the 1-condition check.
 */
static void test_guard_rejects_truncated_directory(void)
{
  TEST_BEGIN("ra8_stbtt guard: truncated directory rejected");
  TEST_ASSERT(!ra8_stbtt_sfnt_dir_in_bounds(s_bad_dir_truncated, sizeof s_bad_dir_truncated, 0U));
  TEST_END("ra8_stbtt guard: truncated directory rejected");
}

/**
 * @test test_guard_rejects_out_of_range_record
 *
 * @par MC/DC:
 * Decision: `if ((t_off + t_len) > buf_len)`  (the per-record extent check;
 * 1 condition, libs/ra8_reflow/src/ra8_stbtt_guard.c@ra8_stbtt_sfnt_dir_in_bounds)
 * - Vector T (this test): record 0 declares offset 0x10000000 + length 0x100,
 *   far past the 28-byte buffer -> true -> reject.
 * - Vector F (test_guard_accepts_valid_font): every Ahem record fits -> false
 *   for all iterations. Complete MC/DC for the 1-condition record check.
 */
static void test_guard_rejects_out_of_range_record(void)
{
  TEST_BEGIN("ra8_stbtt guard: out-of-range table record rejected");
  TEST_ASSERT(!ra8_stbtt_sfnt_dir_in_bounds(s_bad_dir_record, sizeof s_bad_dir_record, 0U));
  TEST_END("ra8_stbtt guard: out-of-range table record rejected");
}

/**
 * @test test_guard_rejects_bad_cmap_subdir
 *
 * @par MC/DC:
 * Decision: `if (sub_end > buf_len)` inside the cmap arm of
 * priv_table_internal_in_bounds
 * (libs/ra8_reflow/src/ra8_stbtt_guard.c@priv_table_internal_in_bounds), plus
 * the `tag == k_sfnt_tag_cmap` branch selection.
 * - Vector T (this test): a well-formed directory whose cmap record fits but
 *   whose in-table numTables is 0xFFFF -> `cmap + 4 + 8 * 0xFFFF` >> 32-byte
 *   buffer -> true -> reject before `stbtt_InitFont` strides the sub-directory.
 * - Vector F (test_guard_accepts_valid_font): the Ahem face has a cmap whose
 *   sub-directory fits -> false, so the tag-cmap branch is taken with the
 *   bound satisfied. The T/F pair completes MC/DC for the cmap internal check.
 */
static void test_guard_rejects_bad_cmap_subdir(void)
{
  TEST_BEGIN("ra8_stbtt guard: cmap in-table subdir overrun rejected");
  TEST_ASSERT(!ra8_stbtt_sfnt_dir_in_bounds(s_bad_cmap_subdir, sizeof s_bad_cmap_subdir, 0U));
  TEST_END("ra8_stbtt guard: cmap in-table subdir overrun rejected");
}

/**
 * @test test_guard_accepts_valid_font
 *
 * @par MC/DC:
 * Provides the all-false control (accept) vector for every 1-condition
 * decision in ra8_stbtt_sfnt_dir_in_bounds and its helper
 * (libs/ra8_reflow/src/ra8_stbtt_guard.c@ra8_stbtt_sfnt_dir_in_bounds):
 * - The bundled Ahem face: data != nullptr, header fits, directory fits, every
 *   record's extent fits, and every cmap / head / maxp internal read fits ->
 *   all checks false -> return true. This supplies the F vector paired with the
 *   T vectors in the reject tests above (including the cmap-subdir check),
 *   completing MC/DC for each, and exercises the record loop's in-bounds arm
 *   across many records with the cmap / head / maxp tag branches taken.
 * - A 12-byte zero-table header: header + directory fit and the record loop
 *   is skipped (numTables == 0) -> return true, covering the loop-not-taken
 *   edge.
 */
static void test_guard_accepts_valid_font(void)
{
  TEST_BEGIN("ra8_stbtt guard: valid fonts accepted");
  TEST_ASSERT(ra8_stbtt_sfnt_dir_in_bounds(k_fixture_ahem, (size_t)k_fixture_ahem_len, 0U));
  TEST_ASSERT(ra8_stbtt_sfnt_dir_in_bounds(s_zero_tables, sizeof s_zero_tables, 0U));
  TEST_END("ra8_stbtt guard: valid fonts accepted");
}

/**
 * @test test_bind_font_rejects_bad_directory
 *
 * @par MC/DC:
 * Decision: `if (!ra8_stbtt_sfnt_dir_in_bounds(font_data, font_len, offset))`
 * (1 condition, guarded by `if (offset >= 0)`;
 * libs/ra8_reflow/src/ra8_reflow_layout_driver.c@ra8_reflow_bind_font)
 * - Vector T (this test): a valid-tag / bad-directory blob -> offset >= 0 and
 *   guard returns false -> `!false` true -> k_ra8_err_not_supported, prior face
 *   preserved.
 * - Vector F (a valid font, covered where bind_font binds Ahem in other
 *   tests): guard returns true -> `!true` false -> proceed to InitFont.
 */
static void test_bind_font_rejects_bad_directory(void)
{
  TEST_BEGIN("ra8_reflow_bind_font: bad sfnt directory rejected");
  ra8_reflow_t engine = {};
  priv_init_ahem_engine(&engine);
  TEST_ASSERT(engine.font_data == k_fixture_ahem);

  TEST_ASSERT_EQ(k_ra8_err_not_supported,
                 ra8_reflow_bind_font(&engine, s_bad_dir_truncated, sizeof s_bad_dir_truncated));
  /* Graceful degradation: the engine keeps its prior (Ahem) face. */
  TEST_ASSERT(engine.font_data == k_fixture_ahem);

  (void)ra8_reflow_close(&engine);
  TEST_END("ra8_reflow_bind_font: bad sfnt directory rejected");
}

/**
 * @test test_register_face_rejects_bad_directory
 *
 * @par MC/DC:
 * Decision: `if (!ra8_stbtt_sfnt_dir_in_bounds(blob, len, offset))`
 * (1 condition, guarded by `if (offset >= 0)`;
 * libs/ra8_reflow/src/ra8_reflow_layout_driver.c@ra8_reflow_register_face)
 * - Vector T (this test): a valid-tag / bad-directory blob -> offset >= 0 and
 *   guard returns false -> `!false` true -> k_ra8_err_not_supported, face_count
 *   unchanged.
 * - Vector F (a valid @font-face, covered where register_face accepts Ahem in
 *   other tests): guard returns true -> proceed to InitFont.
 */
static void test_register_face_rejects_bad_directory(void)
{
  TEST_BEGIN("ra8_reflow_register_face: bad sfnt directory rejected");
  ra8_reflow_t engine = {};
  priv_init_ahem_engine(&engine);
  TEST_ASSERT_EQ(0, engine.face_count);

  TEST_ASSERT_EQ(k_ra8_err_not_supported,
                 ra8_reflow_register_face(&engine, 0U, s_bad_dir_record, sizeof s_bad_dir_record));
  /* Rejected face is not recorded. */
  TEST_ASSERT_EQ(0, engine.face_count);

  (void)ra8_reflow_close(&engine);
  TEST_END("ra8_reflow_register_face: bad sfnt directory rejected");
}

/**
 * @test test_epub_render_glyph_rejects_bad_directory
 *
 * @par MC/DC:
 * Decision: `if (!ra8_stbtt_sfnt_dir_in_bounds(book->font_data, book->font_size,
 * offset))` (1 condition, reached after `offset >= 0`;
 * libs/ra8_epub/src/ra8_epub_chapter.c@priv_font_init)
 * - Vector T (this test): a valid-tag / bad-directory book font -> guard
 *   returns false -> `!false` true -> k_ra8_err_validation_failed from
 *   ra8_epub_render_glyph, without stb_truetype ever walking the directory.
 * - Vector F (a valid book font, covered by the render tests in
 *   test_ra8_epub_chapter_cov.c): guard returns true -> proceed to InitFont.
 */
static void test_epub_render_glyph_rejects_bad_directory(void)
{
  TEST_BEGIN("ra8_epub_render_glyph: bad sfnt directory rejected");
  ra8_epub_book_t book = {};
  book.in_use          = 1U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_set_font(&book, s_bad_dir_record, sizeof s_bad_dir_record));

  uint8_t  bitmap[(size_t)k_g_glyph_dim * (size_t)k_g_glyph_dim] = {};
  uint32_t w                                                     = 0U;
  uint32_t h                                                     = 0U;
  TEST_ASSERT_EQ(k_ra8_err_validation_failed,
                 ra8_epub_render_glyph(&book,
                                       (int32_t)k_g_codepoint_a,
                                       (float)k_g_font_px,
                                       bitmap,
                                       sizeof bitmap,
                                       &w,
                                       &h));
  TEST_END("ra8_epub_render_glyph: bad sfnt directory rejected");
}

/**
 * @brief A structurally valid font whose `loca` table resolves glyph 1 to an
 *        out-of-bounds `glyf` offset.
 *
 * @details
 * This 276-byte TrueType font passes ::ra8_stbtt_sfnt_dir_in_bounds and
 * `stbtt_InitFont` (every table record lies inside the buffer), then maps code
 * point 'A' to glyph 1 whose short-`loca` entry (0x7FFF) resolves to byte
 * offset 0xFFFE -- far past the 276-byte buffer. Upstream stb_truetype
 * dereferences that wild `glyf` offset while walking the outline
 * (`stbtt__GetGlyfOffset` -> `stbtt_GetGlyphBox` / `stbtt__GetGlyphShapeTT`),
 * an attacker-reachable out-of-bounds read. This is the hand-minimised
 * reproducer committed at
 * `tests/fuzz/corpus/fuzz_ra8_stbtt/crash-3c1c53513113a34cbe60414887e1632540bade93`;
 * the `stb_truetype.h` bounds patch (see docs/SOUP/stb.md) rejects the glyph
 * instead of reading past the buffer.
 */
static const uint8_t s_oob_loca_font[276] = {
  0x00U, 0x01U, 0x00U, 0x00U, 0x00U, 0x07U, 0x00U, 0x40U, 0x00U, 0x02U, 0x00U, 0x30U, 0x63U, 0x6dU,
  0x61U, 0x70U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x7cU, 0x00U, 0x00U, 0x00U, 0x18U,
  0x67U, 0x6cU, 0x79U, 0x66U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x94U, 0x00U, 0x00U,
  0x00U, 0x10U, 0x68U, 0x65U, 0x61U, 0x64U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0xa4U,
  0x00U, 0x00U, 0x00U, 0x36U, 0x68U, 0x68U, 0x65U, 0x61U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
  0x00U, 0xdcU, 0x00U, 0x00U, 0x00U, 0x24U, 0x68U, 0x6dU, 0x74U, 0x78U, 0x00U, 0x00U, 0x00U, 0x00U,
  0x00U, 0x00U, 0x01U, 0x00U, 0x00U, 0x00U, 0x00U, 0x04U, 0x6cU, 0x6fU, 0x63U, 0x61U, 0x00U, 0x00U,
  0x00U, 0x00U, 0x00U, 0x00U, 0x01U, 0x04U, 0x00U, 0x00U, 0x00U, 0x06U, 0x6dU, 0x61U, 0x78U, 0x70U,
  0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x01U, 0x0cU, 0x00U, 0x00U, 0x00U, 0x06U, 0x00U, 0x00U,
  0x00U, 0x01U, 0x00U, 0x03U, 0x00U, 0x01U, 0x00U, 0x00U, 0x00U, 0x0cU, 0x00U, 0x06U, 0x00U, 0x0cU,
  0x00U, 0x00U, 0x00U, 0x41U, 0x00U, 0x01U, 0x00U, 0x01U, 0x00U, 0x01U, 0x00U, 0x00U, 0x00U, 0x00U,
  0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x01U, 0x00U, 0x00U,
  0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x5fU, 0x0fU, 0x3cU, 0xf5U, 0x00U, 0x00U,
  0x03U, 0xe8U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
  0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
  0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x01U, 0x00U, 0x00U,
  0x03U, 0x20U, 0xffU, 0x38U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
  0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
  0x00U, 0x00U, 0x00U, 0x01U, 0x01U, 0xf4U, 0x00U, 0x00U, 0x00U, 0x00U, 0x7fU, 0xffU, 0x7fU, 0xf0U,
  0x00U, 0x00U, 0x00U, 0x00U, 0x50U, 0x00U, 0x00U, 0x02U, 0x00U, 0x00U,
};

/**
 * @test test_epub_render_glyph_oob_loca_safe
 *
 * @details
 * Regression for the `fuzz_ra8_stbtt` crash that survived the #217 directory
 * guard: a font can pass the directory bounds check and `stbtt_InitFont`, yet
 * still drive an out-of-bounds read when a `loca` entry resolves a glyph to a
 * `glyf` offset past the buffer. Asserts that (1) the font DOES pass the
 * directory guard -- so it genuinely reaches the deeper glyph-outline path this
 * test is meant to cover -- and (2) `ra8_epub_render_glyph()` returns cleanly
 * (`k_ra8_ok` with a zero-size `0x0` bitmap: the rejected glyph is reported as
 * empty) instead of reading past the buffer. Under AddressSanitizer a
 * regression that removes the stb_truetype bounds check aborts here rather than
 * returning.
 *
 * @par MC/DC: not applicable -- the guarded decisions live in vendored SOUP
 * (`libs/third_party/stb/stb_truetype.h`), which is exempt from MC/DC re-test;
 * this is an end-to-end memory-safety assertion, not a first-party decision.
 */
static void test_epub_render_glyph_oob_loca_safe(void)
{
  TEST_BEGIN("ra8_epub_render_glyph: out-of-bounds loca handled safely");
  /* The font must clear the directory guard so it reaches the glyph-outline
   * path (otherwise this would only re-test the guard rejection above). */
  TEST_ASSERT(ra8_stbtt_sfnt_dir_in_bounds(s_oob_loca_font, sizeof s_oob_loca_font, 0U));

  ra8_epub_book_t book = {};
  book.in_use          = 1U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_set_font(&book, s_oob_loca_font, sizeof s_oob_loca_font));

  uint8_t  bitmap[(size_t)k_g_glyph_dim * (size_t)k_g_glyph_dim] = {};
  uint32_t w                                                     = k_t_dim_unset;
  uint32_t h                                                     = k_t_dim_unset;
  /* Pre-fix this call read past the font buffer inside stb_truetype. Post-fix
   * the glyph's out-of-bounds glyf offset is rejected, so stb reports an empty
   * glyph: the render succeeds with a zero-size bitmap (0x0) and no read
   * escapes the buffer. Under AddressSanitizer a regression aborts before this
   * assertion is reached. */
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_epub_render_glyph(&book,
                                       (int32_t)k_g_codepoint_a,
                                       (float)k_g_font_px,
                                       bitmap,
                                       sizeof bitmap,
                                       &w,
                                       &h));
  TEST_ASSERT_EQ(0U, w);
  TEST_ASSERT_EQ(0U, h);
  TEST_END("ra8_epub_render_glyph: out-of-bounds loca handled safely");
}

int main(void)
{
  test_guard_rejects_null_data();
  test_guard_rejects_short_header();
  test_guard_rejects_truncated_directory();
  test_guard_rejects_out_of_range_record();
  test_guard_rejects_bad_cmap_subdir();
  test_guard_accepts_valid_font();
  test_bind_font_rejects_bad_directory();
  test_register_face_rejects_bad_directory();
  test_epub_render_glyph_rejects_bad_directory();
  test_epub_render_glyph_oob_loca_safe();
  return 0;
}
