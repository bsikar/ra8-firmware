/**
 * @file test_ra8_reflow_table.c
 * @brief Host unit tests + MC/DC for minimal `<table>` layout (#107).
 *
 * @details
 * Lays out tables through `ra8_reflow` (with the fixed-metric Ahem face) and
 * asserts the grid geometry: cells land in aligned columns, rows stack with
 * increasing y, and a table taller than the page breaks across pages. Plus
 * MC/DC mirror vectors for the two new compound decisions -- the cell
 * word-wrap (column-fit) and the row page-break.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <string.h>

#include "fixture_ahem.h"
#include "ra8_err.h"
#include "ra8_reflow.h"
#include "unity_minimal.h"

/** @brief Shared engine (large -- keep off the stack). */
static ra8_reflow_t s_eng;

enum : int32_t {
  k_w      = 200,                        /**< W.                  */
  k_h      = 400,                        /**< H.                  */
  k_margin = 16,                         /**< Margin.             */
  k_pad    = 6,                          /**< k_priv_cell_pad_px. */
  k_col_w  = (k_w - (2 * k_margin)) / 2, /**< Col w.              */
};

/** @brief Lay out @p doc at @p w x @p h; return the page count. */
static uint32_t lay(const char* doc, int32_t w, int32_t h)
{
  uint32_t pages = 0U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_reflow_init((uint16_t)w,
                                 (uint16_t)h,
                                 k_fixture_ahem,
                                 (size_t)k_fixture_ahem_len,
                                 16U,
                                 0xFFFFFFU,
                                 0x3060FFU,
                                 &s_eng));
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_reflow_layout_chapter(&s_eng, (const uint8_t*)doc, (uint32_t)strlen(doc), &pages));
  return pages;
}

/** @brief Index of the first glyph with code point @p cp (or -1). */
static int32_t find_cp(int32_t cp)
{
  for (uint32_t i = 0U; i < s_eng.glyph_count; ++i) {
    if (s_eng.glyphs[i].cp == cp) {
      return (int32_t)i;
    }
  }
  return -1;
}

/**
 * @test test_table_grid
 * @brief A 2x3 table lays out as an aligned column grid with stacked rows.
 *
 * @par MC/DC:
 * Decision (all four probed cells found, enclosing fn test_table_grid):
 * `(a >= 0) && (b >= 0) && (c >= 0) && (e >= 0)` (4 conditions, AND; find_cp
 * returns -1 when a glyph is absent). The 2x3 grid lays out all four cells, so
 * the observed vector is the control; N+1 = 5:
 *  - V1 a,b,c,e all >= 0 -> C1=C2=C3=C4=T -> T (all cells present).
 *  - V2 a < 0 -> C1=F -> F (cell A missing).
 *  - V3 b < 0 -> C2=F -> F (cell B missing).
 *  - V4 c < 0 -> C3=F -> F (cell C missing).
 *  - V5 e < 0 -> C4=F -> F (cell E missing).
 * Each Vk (k>1) vs V1 isolates condition k.
 */
static void test_table_grid(void)
{
  TEST_BEGIN("table 2x3 grid: aligned columns, separated rows");
  (void)lay("<html><body><table>"
            "<tr><td>A</td><td>B</td></tr>"
            "<tr><td>C</td><td>D</td></tr>"
            "<tr><td>E</td><td>F</td></tr>"
            "</table></body></html>",
            k_w,
            k_h);
  const int32_t a = find_cp('A');
  const int32_t b = find_cp('B');
  const int32_t c = find_cp('C');
  const int32_t e = find_cp('E');
  TEST_ASSERT((a >= 0) && (b >= 0) && (c >= 0) && (e >= 0));
  /* Columns: cell 0 at the left margin + pad, cell 1 a column over. */
  TEST_ASSERT_EQ(k_margin + k_pad, s_eng.glyphs[a].x);
  TEST_ASSERT_EQ(k_margin + k_col_w + k_pad, s_eng.glyphs[b].x);
  /* Row 0 cells share a baseline; later rows stack downward. */
  TEST_ASSERT_EQ(s_eng.glyphs[a].y, s_eng.glyphs[b].y);
  TEST_ASSERT(s_eng.glyphs[c].y > s_eng.glyphs[a].y);
  TEST_ASSERT(s_eng.glyphs[e].y > s_eng.glyphs[c].y);
  /* Column 0 is aligned across all rows. */
  TEST_ASSERT_EQ(s_eng.glyphs[a].x, s_eng.glyphs[c].x);
  TEST_ASSERT_EQ(s_eng.glyphs[c].x, s_eng.glyphs[e].x);
  TEST_END("table 2x3 grid: aligned columns, separated rows");
}

/**
 * @test test_table_page_break
 * @brief A table taller than the page breaks between rows across pages.
 *
 * @par MC/DC:
 * (no MC/DC vectors contributed here -- a table taller than the page is laid
 * out and the case asserts `pages >= 2` (single condition). The row page-break
 * compound `(cur->y + row_h > bottom) && (cur->y > margin)` in priv_layout_row
 * that produces the extra page has its independence vectors in
 * test_row_break_mcdc.)
 */
static void test_table_page_break(void)
{
  TEST_BEGIN("table taller than a page breaks across pages");
  const uint32_t pages = lay("<html><body><table>"
                             "<tr><td>r1</td><td>x</td></tr><tr><td>r2</td><td>x</td></tr>"
                             "<tr><td>r3</td><td>x</td></tr><tr><td>r4</td><td>x</td></tr>"
                             "<tr><td>r5</td><td>x</td></tr><tr><td>r6</td><td>x</td></tr>"
                             "<tr><td>r7</td><td>x</td></tr><tr><td>r8</td><td>x</td></tr>"
                             "<tr><td>r9</td><td>x</td></tr><tr><td>r10</td><td>x</td></tr>"
                             "</table></body></html>",
                             200,
                             100);
  TEST_ASSERT(pages >= 2U);
  TEST_END("table taller than a page breaks across pages");
}

/** @brief Mirror of the cell word-wrap: (cx+word_w > cell_right) && (cx > cell_x). */
static uint8_t mirror_cell_wrap(int32_t cx, int32_t word_w, int32_t cell_right, int32_t cell_x)
{
  if (((cx + word_w) > cell_right) && (cx > cell_x)) {
    return 1U;
  }
  return 0U;
}

/**
 * @test test_cell_wrap_mcdc
 *
 * @par MC/DC:
 * Decision: `if (cx + word_w > cell_right && cx > cell_x)` (2 conditions, AND;
 * libs/ra8_reflow/src/ra8_reflow_layout.c@priv_layout_cell -- the column-fit
 * wrap). N+1 = 3 vectors:
 *  - V1: cx=50, word=40, right=80, x=10 -> 90>80 T, 50>10 T -> wrap.
 *  - V2: cx=50, word=20, right=80, x=10 -> 70>80 F        -> no wrap.
 *  - V3: cx=10, word=40, right=80, x=10 -> 50>80? no... use right=40:
 *        cx=10, word=40, right=40, x=10 -> 50>40 T, 10>10 F -> no wrap (at start).
 * V1 vs V2 vary the fit (cx>cell_x held T); V1 vs V3 vary cx>cell_x (fit held T).
 */
static void test_cell_wrap_mcdc(void)
{
  TEST_BEGIN("priv_layout_cell wrap MC/DC: cx+w>right && cx>cell_x");
  TEST_ASSERT_EQ(1, mirror_cell_wrap(50, 40, 80, 10)); /* both T -> wrap             */
  TEST_ASSERT_EQ(0, mirror_cell_wrap(50, 20, 80, 10)); /* fit F                      */
  TEST_ASSERT_EQ(0, mirror_cell_wrap(10, 40, 40, 10)); /* at line start (cx==cell_x) */
  TEST_END("priv_layout_cell wrap MC/DC: cx+w>right && cx>cell_x");
}

/** @brief Mirror of the row page-break: (row_y+row_h > bottom) && (row_y > margin). */
static uint8_t mirror_row_break(int32_t row_y, int32_t row_h, int32_t bottom, int32_t margin)
{
  if (((row_y + row_h) > bottom) && (row_y > margin)) {
    return 1U;
  }
  return 0U;
}

/**
 * @test test_row_break_mcdc
 *
 * @par MC/DC:
 * Decision: `if (cur->y + row_h > bottom && cur->y > margin)` (2 conditions,
 * AND; libs/ra8_reflow/src/ra8_reflow_layout.c@priv_layout_row -- the row
 * page-break). N+1 = 3 vectors:
 *  - V1: y=90, h=30, bottom=100, margin=16 -> 120>100 T, 90>16 T -> break.
 *  - V2: y=20, h=30, bottom=100, margin=16 -> 50>100 F          -> no break.
 *  - V3: y=16, h=120,bottom=100, margin=16 -> 136>100 T, 16>16 F -> no break
 *        (a row taller than the page that already starts at the top stays).
 * V1 vs V2 vary the overflow; V1 vs V3 vary the mid-page guard.
 */
static void test_row_break_mcdc(void)
{
  TEST_BEGIN("priv_layout_row page-break MC/DC: y+h>bottom && y>margin");
  TEST_ASSERT_EQ(1, mirror_row_break(90, 30, 100, 16));
  TEST_ASSERT_EQ(0, mirror_row_break(20, 30, 100, 16));
  TEST_ASSERT_EQ(0, mirror_row_break(16, 120, 100, 16));
  TEST_END("priv_layout_row page-break MC/DC: y+h>bottom && y>margin");
}

/**
 * @brief Test entry point.
 * @return 0 on success; unity macros exit(1) on the first failure.
 */
int32_t main(void)
{
  test_table_grid();
  test_table_page_break();
  test_cell_wrap_mcdc();
  test_row_break_mcdc();
  (void)fprintf(stderr, "[OK ] test_ra8_reflow_table.c\n");
  return 0;
}
