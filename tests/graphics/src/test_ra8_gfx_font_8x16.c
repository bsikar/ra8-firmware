/**
 * @file test_ra8_gfx_font_8x16.c
 * @brief Unit tests for the bundled 8x16 bitmap font.
 *
 * @details
 * `ra8_gfx_font_8x16.c` is a data unit: one static glyph table plus the public
 * ::ra8_gfx_font_8x16 descriptor that points at it. It was compiled into the
 * host coverage build and linked by no test binary, so gcovr reported it not at
 * 0% but not at all, which is the state this test exists to end. Linking it from a
 * test is the whole point; asserting on it is what makes the link worth having.
 *
 * What is pinned here is exactly what a consumer of ::ra8_gfx_font_t is
 * entitled to assume, and nothing about how the table was generated:
 *
 * - the three descriptor invariants written down in `ra8_gfx_font.h`
 *   (non-degenerate geometry, `bytes_per_glyph == ceil(width/8) * height`,
 *   `first_codepoint <= last_codepoint`);
 * - the ASCII range the header promises, 0x20..0x7E, which is 95 glyphs;
 * - that every glyph in that range is addressable at
 *   `glyph_data[(cp - first) * bytes_per_glyph]` and readable to its last byte;
 * - that the glyph rows honour the 8-pixel cell, so no row can carry a pixel
 *   the renderer would not draw;
 * - six committed bitmaps read back byte for byte, space and '~' among them,
 *   so a table edit that shifts every glyph by one row cannot pass.
 *
 * The six spot-checked bitmaps are transcribed from the committed table, not
 * from an external font archive, so this is a change detector for the table and
 * not an independent verification of the IBM code page 437 glyph shapes.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8_gfx_font.h"
#include "unity_minimal.h"

/**
 * @enum font_test_dim_t
 * @brief Geometry the header promises for the bundled 8x16 font.
 */
typedef enum : uint16_t {
  k_ft_width       = 8,    /**< Glyph width in pixels.            */
  k_ft_height      = 16,   /**< Glyph height in pixels.           */
  k_ft_bytes       = 16,   /**< Bytes per glyph (1 byte per row). */
  k_ft_first       = 0x20, /**< First stored codepoint, space.    */
  k_ft_last        = 0x7E, /**< Last stored codepoint, tilde.     */
  k_ft_glyphs      = 95,   /**< Stored glyph count, inclusive.    */
  k_ft_table_bytes = 1520, /**< 95 glyphs x 16 bytes.             */
} font_test_dim_t;

/** @brief One committed bitmap, for the byte-for-byte spot checks. */
typedef struct {
  uint8_t codepoint;        /**< ASCII codepoint the rows belong to. */
  uint8_t rows[k_ft_bytes]; /**< Expected 16 rows, top row first.    */
} font_expect_t;

/**
 * @brief Six bitmaps transcribed from the committed table.
 *
 * @details Space is the all-zero case, '_' puts its only ink on row 13 and '~'
 *          on rows 2 and 3, so between them a whole-table row shift or an
 *          off-by-one glyph index cannot survive.
 */
static const font_expect_t k_ft_expect[] = {
  {0x20, {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
  {0x21, {0, 0, 0x18, 0x3C, 0x3C, 0x3C, 0x18, 0x18, 0x18, 0x00, 0x18, 0x18, 0, 0, 0, 0}},
  {0x30, {0, 0, 0x7C, 0xC6, 0xC6, 0xCE, 0xDE, 0xF6, 0xE6, 0xC6, 0xC6, 0x7C, 0, 0, 0, 0}},
  {0x41, {0, 0, 0x10, 0x38, 0x6C, 0xC6, 0xC6, 0xFE, 0xC6, 0xC6, 0xC6, 0xC6, 0, 0, 0, 0}},
  {0x5F, {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0, 0}},
  {0x7E, {0, 0, 0x76, 0xDC, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
};

/**
 * @brief Return the first byte of one glyph, or null when out of range.
 *
 * @param[in] font      Font descriptor to index.
 * @param[in] codepoint ASCII codepoint to look up.
 *
 * @return Pointer to the glyph's first row, or `nullptr` when the codepoint is
 *         outside `[first_codepoint, last_codepoint]`.
 */
static const uint8_t* internal_glyph(const ra8_gfx_font_t* font, uint8_t codepoint)
{
  if ((codepoint < font->first_codepoint) || (codepoint > font->last_codepoint)) {
    return nullptr;
  }
  const uint32_t index = (uint32_t)(codepoint - font->first_codepoint);
  return &font->glyph_data[index * (uint32_t)font->bytes_per_glyph];
}

/** @brief The header's three descriptor invariants hold for the bundled font. */
static void test_descriptor_invariants(void)
{
  TEST_BEGIN("descriptor invariants");
  const ra8_gfx_font_t* font = &ra8_gfx_font_8x16;

  TEST_ASSERT_NOT_NULL(font->glyph_data);
  TEST_ASSERT(font->glyph_width >= 1U);
  TEST_ASSERT(font->glyph_height >= 1U);
  TEST_ASSERT(font->first_codepoint <= font->last_codepoint);

  const uint32_t row_bytes = ((uint32_t)font->glyph_width + 7U) / 8U;
  TEST_ASSERT_EQ(row_bytes * (uint32_t)font->glyph_height, (uint32_t)font->bytes_per_glyph);
  TEST_END("descriptor invariants");
}

/** @brief The bundled font is the 8x16 ASCII 0x20..0x7E table the header promises. */
static void test_promised_geometry_and_range(void)
{
  TEST_BEGIN("promised geometry and range");
  const ra8_gfx_font_t* font = &ra8_gfx_font_8x16;

  TEST_ASSERT_EQ((uint32_t)k_ft_width, (uint32_t)font->glyph_width);
  TEST_ASSERT_EQ((uint32_t)k_ft_height, (uint32_t)font->glyph_height);
  TEST_ASSERT_EQ((uint32_t)k_ft_bytes, (uint32_t)font->bytes_per_glyph);
  TEST_ASSERT_EQ((uint32_t)k_ft_first, (uint32_t)font->first_codepoint);
  TEST_ASSERT_EQ((uint32_t)k_ft_last, (uint32_t)font->last_codepoint);

  const uint32_t glyphs = (uint32_t)(font->last_codepoint - font->first_codepoint) + 1U;
  TEST_ASSERT_EQ((uint32_t)k_ft_glyphs, glyphs);
  TEST_ASSERT_EQ((uint32_t)k_ft_table_bytes, glyphs * (uint32_t)font->bytes_per_glyph);
  TEST_END("promised geometry and range");
}

/** @brief Every in-range codepoint is addressable and readable to its last byte. */
static void test_every_glyph_is_addressable(void)
{
  TEST_BEGIN("every glyph addressable");
  const ra8_gfx_font_t* font = &ra8_gfx_font_8x16;

  TEST_ASSERT_NULL(internal_glyph(font, (uint8_t)(font->first_codepoint - 1U)));
  TEST_ASSERT_NULL(internal_glyph(font, (uint8_t)(font->last_codepoint + 1U)));

  uint32_t ink = 0U;
  for (uint32_t cp = font->first_codepoint; cp <= font->last_codepoint; ++cp) {
    const uint8_t* rows = internal_glyph(font, (uint8_t)cp);
    TEST_ASSERT_NOT_NULL(rows);
    TEST_ASSERT(rows >= font->glyph_data);

    const uint32_t offset = (uint32_t)(rows - font->glyph_data);
    TEST_ASSERT_EQ((cp - font->first_codepoint) * (uint32_t)font->bytes_per_glyph, offset);
    TEST_ASSERT((offset + (uint32_t)font->bytes_per_glyph) <= (uint32_t)k_ft_table_bytes);

    for (uint32_t row = 0U; row < (uint32_t)font->bytes_per_glyph; ++row) {
      ink += (rows[row] != 0U) ? 1U : 0U;
    }
  }

  /* Space is blank by definition; the other 94 glyphs are not all blank. */
  TEST_ASSERT(ink > 0U);
  TEST_END("every glyph addressable");
}

/** @brief Space is the only blank glyph, and no other glyph is empty. */
static void test_only_space_is_blank(void)
{
  TEST_BEGIN("only space is blank");
  const ra8_gfx_font_t* font = &ra8_gfx_font_8x16;

  uint32_t blank = 0U;
  for (uint32_t cp = font->first_codepoint; cp <= font->last_codepoint; ++cp) {
    const uint8_t* rows   = internal_glyph(font, (uint8_t)cp);
    uint32_t       union_ = 0U;
    for (uint32_t row = 0U; row < (uint32_t)font->bytes_per_glyph; ++row) {
      union_ |= (uint32_t)rows[row];
    }
    if (union_ == 0U) {
      ++blank;
      TEST_ASSERT_EQ((uint32_t)k_ft_first, cp);
    }
  }
  TEST_ASSERT_EQ(1U, blank);
  TEST_END("only space is blank");
}

/** @brief Six committed bitmaps read back byte for byte. */
static void test_committed_bitmaps(void)
{
  TEST_BEGIN("committed bitmaps");
  const ra8_gfx_font_t* font  = &ra8_gfx_font_8x16;
  const uint32_t        cases = (uint32_t)(sizeof(k_ft_expect) / sizeof(k_ft_expect[0]));

  for (uint32_t i = 0U; i < cases; ++i) {
    const font_expect_t* want = &k_ft_expect[i];
    const uint8_t*       rows = internal_glyph(font, want->codepoint);
    TEST_ASSERT_NOT_NULL(rows);
    for (uint32_t row = 0U; row < (uint32_t)k_ft_bytes; ++row) {
      TEST_ASSERT_EQ((uint32_t)want->rows[row], (uint32_t)rows[row]);
    }
  }
  TEST_END("committed bitmaps");
}

int main(void)
{
  test_descriptor_invariants();
  test_promised_geometry_and_range();
  test_every_glyph_is_addressable();
  test_only_space_is_blank();
  test_committed_bitmaps();
  return 0;
}
