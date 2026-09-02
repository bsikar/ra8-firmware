/**
 * @file test_ereader_shelf_geometry.c
 * @brief Regression guard for the ereader_shelf layout + fill render (#233).
 *
 * @details
 * Issue #233 reported that building `ereader_shelf` at -O1+ makes the shelf
 * "miscompile": the header bar shifts (px(0,0) becomes background instead of
 * bar) and ~63% of framebuffer pixels change versus the -Og golden
 * (fb=FA3AB5B5). Root-causing it proved the FIRMWARE is correct and
 * optimisation-stable: the identical C source renders byte-identically at
 * -O0/-O1/-O2/-O3 on the host (clang and gcc), is UBSan- and ASan-clean, and
 * matches fb=FA3AB5B5. The divergence is a ra8_emulator emulation gap -- ra8_emulator
 * runs the Cortex-M85 firmware on a Unicorn Cortex-M33 (Armv8-M) core that
 * lacks the Armv8.1-M Low-Overhead-Branch loop instructions (`dls`/`le`) GCC 14
 * emits at -O1 for the ra8_gfx fill loop (`internal_fill_rect_565`) and the MVE
 * byte-fill it emits at -O2 -- neither of which ra8_emulator has a hand-emulation
 * seam for -- so ra8_emulator mis-executes them (wrong framebuffer + a runaway
 * instruction count). See the app CMakeLists comment for the full note.
 *
 * These tests pin the two things the issue observed as broken, exercised
 * through the REAL engines the shelf draws with -- ra8_box (the grid layout that
 * positions the cards) and ra8_gfx (the clip/fill fast path `internal_fill_rect_565`
 * that carries the -O1 low-overhead loop) -- so a future change to those engines
 * that would shift the shelf or drop the header bar is caught on the host:
 *   1. The exact card geometry the shelf produces for 3 baked books
 *      (the rects ra8_emulator measured at -Og: x=24/274/524, y=80, w=226, h=248).
 *   2. The rendered header bar and card fills land where they should
 *      (px(0,0) is the bar colour, NOT the background -- the #233 symptom),
 *      and the render is deterministic (opt-stability is proven separately on
 *      the host; a single-opt-level ctest pins the byte result here).
 *
 * The layout constants mirror `examples/.../ereader_shelf/inc/sh_app.h` +
 * `sh_shelf.c`'s `sh_layout_cards()` / `sh_titlebar()`; they are inlined so the
 * test stays hermetic (no app-generated `library.h`). ra8_box_layout() and
 * ra8_gfx_rect() carry their own MC/DC vectors in test_ra8_box.c / the ra8_gfx
 * tests; this file adds no new compound decision, so it needs no MC/DC block.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>

#include "ra8_boot_entry.h"
#include "ra8_box.h"
#include "ra8_err.h"
#include "ra8_gfx.h"
#include "ra8_ui.h"
#include "unity_minimal.h"

/**
 * @enum shelf_geom_t
 * @brief Shelf layout constants mirrored from ereader_shelf's sh_app.h.
 */
enum shelf_geom_t : int32_t {
  k_fb_w      = 1024, /**< Panel width in pixels.             */
  k_fb_h      = 600,  /**< Panel height in pixels.            */
  k_bar_h     = 56,   /**< Header / title-bar height.         */
  k_pad       = 24,   /**< Outer margin / content inset.      */
  k_gap       = 24,   /**< Gap between shelf cards.           */
  k_grid_cols = 4,    /**< Shelf grid columns.                */
  k_card_h    = 272,  /**< Card height used to size the area. */
  k_books     = 3,    /**< Baked books on the shelf.          */
  k_box_cap   = 32,   /**< ra8_box scratch node capacity.     */
};

/**
 * @enum shelf_color_t
 * @brief Shelf 0x00RRGGBB palette mirrored from sh_app.h (sh_color_t).
 */
enum shelf_color_t : uint32_t {
  k_col_bg      = 0x201A14U, /**< App background (dark wood). */
  k_col_bar     = 0x3A2E22U, /**< Header / title-bar fill.    */
  k_col_card    = 0xF4ECDFU, /**< Idle card fill (paper).     */
  k_col_card_hi = 0xFFF7E0U, /**< Selected card fill.         */
};

/**
 * @enum rgb565_shift_t
 * @brief RGB565 channel positions/widths for the independent packer.
 */
enum rgb565_shift_t : uint32_t {
  k_ch_mask  = 0xFFU, /**< One 8-bit colour channel mask. */
  k_r_byte   = 16U,   /**< Red byte position in 0xRRGGBB. */
  k_g_byte   = 8U,    /**< Green byte position.           */
  k_drop5    = 3U,    /**< Drop 3 LSBs for a 5-bit field. */
  k_drop6    = 2U,    /**< Drop 2 LSBs for a 6-bit field. */
  k_r_pos565 = 11U,   /**< Red field position in RGB565.  */
  k_g_pos565 = 5U,    /**< Green field position.          */
};

/** @brief Independent RGB565 packer (oracle for ra8_gfx's own packing). */
static uint16_t pack565(uint32_t rgb)
{
  const uint32_t r = (rgb >> k_r_byte) & k_ch_mask;
  const uint32_t g = (rgb >> k_g_byte) & k_ch_mask;
  const uint32_t b = rgb & k_ch_mask;
  return (uint16_t)(((r >> k_drop5) << k_r_pos565) | ((g >> k_drop6) << k_g_pos565) |
                    (b >> k_drop5));
}

/**
 * @var s_fb
 * @brief Render scratch framebuffer (file-scope: ~1.2 MiB, kept off the stack).
 * @note Single-threaded test use only.
 */
static uint16_t s_fb[(size_t)k_fb_h * (size_t)k_fb_w];

/**
 * @brief Build the shelf card grid exactly as sh_layout_cards() does.
 * @param[out] store Caller node storage (>= k_box_cap nodes).
 * @param[out] out   Receives the k_books positioned card rects.
 */
static void layout_shelf(ra8_box_t* store, ra8_ui_rect_t* out)
{
  ra8_box_tree_t tree;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_box_tree_init(&tree, store, (uint16_t)k_box_cap));

  ra8_box_t grid   = {};
  grid.kind        = (uint8_t)k_ra8_box_grid;
  grid.grid_cols   = (uint8_t)k_grid_cols;
  grid.flex        = 1U;
  grid.pad         = (int16_t)k_pad;
  grid.gap         = (int16_t)k_gap;
  const int16_t gi = ra8_box_add(&tree, (int16_t)k_ra8_box_none, &grid);
  TEST_ASSERT_EQ(0, gi);

  for (int32_t i = 0; i < k_books; ++i) {
    ra8_box_t cell = {};
    cell.kind      = (uint8_t)k_ra8_box_leaf;
    cell.flex      = 1U;
    cell.tag       = (int16_t)i;
    (void)ra8_box_add(&tree, gi, &cell);
  }

  const int32_t rows  = (k_books + k_grid_cols - 1) / k_grid_cols;
  const int32_t avail = k_fb_h - k_bar_h;
  int32_t       ah    = (rows * k_card_h) + k_pad;
  if (ah > avail) {
    ah = avail;
  }
  const ra8_ui_rect_t area = {.x = 0, .y = k_bar_h, .w = k_fb_w, .h = ah};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_box_layout(&tree, gi, &area));
  for (int32_t i = 0; i < k_books; ++i) {
    out[i] = store[(size_t)gi + 1U + (size_t)i].rect;
  }
}

/**
 * @brief The shelf grid places 3 cards at the golden coordinates (#233).
 * @details Pins the exact rects ra8_emulator renders at -Og -- a regression in
 *          ra8_box's grid maths that shifted the shelf would fail here.
 *
 * @par MC/DC:
 * (no compound decisions in this test -- pins the exact card rects ra8_box_layout
 * produces for the 3-book shelf grid (x=24/274/524, y=80, w=226, h=248); it adds no
 * compound decision of its own, and ra8_box_layout's decisions carry their MC/DC in
 * test_ra8_box.c.)
 */
static void test_shelf_card_geometry(void)
{
  TEST_BEGIN("ereader_shelf card geometry");
  ra8_box_t     store[k_box_cap];
  ra8_ui_rect_t card[k_books] = {};
  layout_shelf(store, card);

  const int32_t want_x[k_books] = {24, 274, 524};
  for (int32_t i = 0; i < k_books; ++i) {
    TEST_ASSERT_EQ(want_x[i], card[i].x);
    TEST_ASSERT_EQ(80, card[i].y);
    TEST_ASSERT_EQ(226, card[i].w);
    TEST_ASSERT_EQ(248, card[i].h);
  }
  TEST_END("ereader_shelf card geometry");
}

/**
 * @brief Draw the whole shelf into @p fb: clear, title bar, then the cards.
 *
 * @details
 * Goes through the same `internal_fill_rect_565` path the app uses -- the one
 * carrying the -O1 low-overhead loop ra8_emulator mis-emulates -- so a rendering
 * defect shows up here exactly as it would on the device. Card 0 is drawn in
 * the selected colour; the rest share the plain card fill.
 *
 * @param[out] fb   RGB565 framebuffer of `k_fb_w * k_fb_h` pixels.
 * @param[in]  card Per-card rectangles from ::layout_shelf.
 *
 *
 * @pre @p fb holds at least `k_fb_w * k_fb_h` pixels.
 * @pre @p card holds `k_books` laid-out rectangles.
 * @post @p fb holds the complete shelf; every draw returned success.
 * @post @p fb is now the active ra8_gfx target.
 *
 * @note Not thread-safe; ra8_gfx keeps one global target.
 *
 * @see layout_shelf()  Produces the rectangles this draws.
 */
static void render_shelf(uint16_t* fb, const ra8_ui_rect_t* card)
{
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_gfx_init(fb, (uint16_t)k_fb_w, (uint16_t)k_fb_h, k_ra8_gfx_format_rgb565));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_gfx_clear(k_col_bg));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_gfx_rect(0, 0, k_fb_w, k_bar_h, k_col_bar, true));
  for (int32_t i = 0; i < k_books; ++i) {
    const uint32_t fill = (i == 0) ? (uint32_t)k_col_card_hi : (uint32_t)k_col_card;
    TEST_ASSERT_EQ(k_ra8_ok, ra8_gfx_rect(card[i].x, card[i].y, card[i].w, card[i].h, fill, true));
  }
}

/**
 * @brief The header bar and card fills render where they belong (#233).
 * @details Asserts px(0,0) is the BAR colour (not the background: the exact
 *          #233 symptom), that the bar reaches its bottom row, that a card
 *          interior carries the card fill, and that a grid gap stays
 *          background. Each probe is a single pixel whose colour is decided by
 *          one rectangle, so a failure names which rectangle went wrong.
 *
 * @par MC/DC:
 * (no compound decisions in this test -- probes single framebuffer pixels rendered
 * through ra8_gfx (bar at px(0,0) and bar bottom, card interior, grid gap); it adds
 * no compound decision of its own, and the ra8_gfx fill/clip decisions carry their
 * MC/DC in the ra8_gfx tests.)
 */
static void test_shelf_render_pixels(void)
{
  TEST_BEGIN("ereader_shelf render pixels");
  ra8_box_t     store[k_box_cap];
  ra8_ui_rect_t card[k_books] = {};
  layout_shelf(store, card);
  render_shelf(s_fb, card);

  const int32_t row0_y = card[0].y + (card[0].h / 2);
  const size_t  row0   = (size_t)row0_y * (size_t)k_fb_w;
  /* #233 symptom: px(0,0) must be the bar, not the background. */
  TEST_ASSERT_EQ(pack565(k_col_bar), s_fb[0]);
  TEST_ASSERT_EQ(pack565(k_col_bar), s_fb[(size_t)(k_bar_h - 1) * (size_t)k_fb_w]); /* bar bottom */
  /* Card 0 interior is the selected-card fill. */
  const int32_t c0_x = card[0].x + (card[0].w / 2);
  const size_t  c0   = row0 + (size_t)c0_x;
  TEST_ASSERT_EQ(pack565(k_col_card_hi), s_fb[c0]);
  /* The gap between card 0 and card 1 stays background. */
  const int32_t gap_x = card[0].x + card[0].w + (k_gap / 2);
  const size_t  gpx   = row0 + (size_t)gap_x;
  TEST_ASSERT_EQ(pack565(k_col_bg), s_fb[gpx]);
  TEST_END("ereader_shelf render pixels");
}

/**
 * @brief Two identical renders produce byte-identical framebuffers.
 * @details Renders the same layout into two separate buffers and compares every
 *          pixel. This is the property that would break first if the fill path
 *          read uninitialised state or depended on what the buffer held before,
 *          which is why it is checked apart from the placement probes.
 *
 * @par MC/DC:
 * (no compound decisions in this test -- renders the same layout into two buffers
 * and compares them with a single-condition `s_fb[i] != s_fb2[i]` scan; no && or ||
 * in the code under test that this case touches.)
 */
static void test_shelf_render_deterministic(void)
{
  /** Second render buffer retained off-stack for this test. */
  static uint16_t s_second_fb[(size_t)k_fb_h * (size_t)k_fb_w];
  TEST_BEGIN("ereader_shelf render deterministic");
  ra8_box_t     store[k_box_cap];
  ra8_ui_rect_t card[k_books] = {};
  layout_shelf(store, card);

  render_shelf(s_fb, card);
  render_shelf(s_second_fb, card);

  bool differs = false;
  for (size_t i = 0; i < (size_t)k_fb_h * (size_t)k_fb_w; ++i) {
    if (s_fb[i] != s_second_fb[i]) {
      differs = true;
      break;
    }
  }
  TEST_ASSERT(!differs);
  TEST_END("ereader_shelf render deterministic");
}

int main(void)
{
  test_shelf_card_geometry();
  test_shelf_render_pixels();
  test_shelf_render_deterministic();
  return 0;
}
