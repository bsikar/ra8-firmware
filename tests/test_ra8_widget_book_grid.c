/**
 * @file test_ra8_widget_book_grid.c
 * @brief Unit tests for the ra8_widget book-grid leaf (#145 Phase 2).
 *
 * @details
 * Covers the shelf's book grid: card layout across the grid's columns and rows,
 * the truncation and edge cases of a partially-filled last row, and the touch
 * routing that turns a hit inside a card into an `on_open(index)` callback.
 * Like its sibling chrome suites it renders through a recording mock
 * ::ra8_widget_paint_t backend (no framebuffer, so this runs in-process) and
 * routes synthetic touches, so both the paint maths and the hit-routing
 * decisions are asserted with full MC/DC vectors.
 *
 * The remaining chrome leaves -- progress bar, status bar, toolbar and
 * navigation strip -- are exercised by `test_ra8_widget_chrome.c`. Following
 * the sibling widget suites, this translation unit shares no state with them:
 * the recording backend below is its own.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>
#include <string.h>

#include "ra8_widget.h"
#include "ra8_widget_book.h"
#include "unity_minimal.h"

/**
 * @enum t_grid_geom_t
 * @brief Rectangles and card sizes the book grid is laid out with.
 *
 * @details
 * Each arm gets a distinct rectangle so a recorded fill maps to exactly one
 * region, and no height is a multiple of the 8x16 mock glyph cell -- a text run
 * aligning to an edge would mask an off-by-one in the inset arithmetic.
 */
typedef enum : int16_t {
  k_t_bar_w  = 100, /**< Width of the narrow (single-column) shelf rect. */
  k_t_row_w  = 120, /**< Width of the list-row rects; also the height of
                           the tall scrolling arms.                         */
  k_t_wide_w = 200, /**< Width of the shelf rect.      */
  k_t_card_h = 52,  /**< Height of the book-card rect. */
} t_grid_geom_t;

/** @brief Callback index seed: a widget that fires without one leaves this. */
typedef enum : uint16_t {
  k_t_idx_unset = 0xFFFFU, /**< "Nothing reported yet." */
} t_grid_idx_t;

/**
 * @enum t_grid_argb_t
 * @brief The 0x00RRGGBB palette the book-grid arms paint with.
 *
 * @details
 * Distinct per style slot, so a swapped `fg`/`fg_right` or `track`/`fill`
 * surfaces as a wrong recorded colour. Slots sharing a value do so because
 * they render the same role in different widgets.
 */
typedef enum : uint32_t {
  k_t_argb_bg        = 0x00FFFFFFU, /**< Card background.                      */
  k_t_argb_fg        = 0x00101010U, /**< Card title text.                      */
  k_t_argb_fg_right  = 0x00808080U, /**< Right-aligned secondary: card author. */
  k_t_argb_bar_track = 0x00303030U, /**< Per-card progress-bar track.          */
  k_t_argb_bar_fill  = 0x00E0A020U, /**< Per-card progress-bar fill.           */
} t_grid_argb_t;

/* --- Recording mock paint backend ------------------------------------------- */

/**
 * @struct mock_paint_t
 * @brief A recording ::ra8_widget_paint_t backend for the chrome-widget tests.
 *
 * @details
 * Counts the fill / text calls and records the last call's arguments so a render
 * can be asserted without a framebuffer. `glyph_w` / `glyph_h` drive the fake
 * `text_size`, so alignment maths is deterministic.
 */
typedef struct {
  uint32_t    fill_calls;      /**< Times fill_rect() ran.       */
  uint32_t    text_calls;      /**< Times draw_text() ran.       */
  int32_t     last_fill_x;     /**< Last fill rect left.         */
  int32_t     last_fill_y;     /**< Last fill rect top.          */
  int32_t     last_fill_w;     /**< Last fill rect width.        */
  int32_t     last_fill_h;     /**< Last fill rect height.       */
  uint32_t    last_fill_color; /**< Last fill colour.            */
  int32_t     last_text_x;     /**< Last text pen x.             */
  int32_t     last_text_y;     /**< Last text pen y.             */
  uint32_t    last_text_fg;    /**< Last text fg colour.         */
  const char* last_text;       /**< Last drawn string.           */
  int32_t     glyph_w;         /**< Fake per-char advance width. */
  int32_t     glyph_h;         /**< Fake glyph cell height.      */
} mock_paint_t;

static void mock_paint_fill(void* user, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color)
{
  mock_paint_t* m = (mock_paint_t*)user;
  m->fill_calls++;
  m->last_fill_x     = x;
  m->last_fill_y     = y;
  m->last_fill_w     = w;
  m->last_fill_h     = h;
  m->last_fill_color = color;
}

static void
mock_paint_text(void* user, int32_t x, int32_t y, const char* str, uint32_t fg, uint32_t bg)
{
  mock_paint_t* m = (mock_paint_t*)user;
  (void)bg;
  m->text_calls++;
  m->last_text_x  = x;
  m->last_text_y  = y;
  m->last_text    = str;
  m->last_text_fg = fg;
}

static void mock_paint_size(void* user, const char* str, int32_t* out_w, int32_t* out_h)
{
  mock_paint_t* m = (mock_paint_t*)user;
  *out_w          = (int32_t)strlen(str) * m->glyph_w;
  *out_h          = m->glyph_h;
}

/** @brief Build a paint backend bound to @p m, with or without `text_size`. */
static ra8_widget_paint_t make_paint(mock_paint_t* m, bool with_measure)
{
  ra8_widget_paint_t p = {};
  p.user               = m;
  p.fill_rect          = mock_paint_fill;
  p.draw_text          = mock_paint_text;
  p.text_size          = with_measure ? mock_paint_size : nullptr;
  return p;
}
/** @brief Records book-grid open-callback invocations + last index. */
static uint32_t s_bg_open_calls = 0U;
/** @brief Index reported by the most recent open callback. */
static uint16_t s_bg_open_idx = k_t_idx_unset;

/** @brief A ::ra8_widget_book_grid_t on_open callback that records its args. */
static void test_bg_on_open(ra8_widget_t* w, uint16_t index)
{
  (void)w;
  s_bg_open_calls++;
  s_bg_open_idx = index;
}

/** @brief Two book records. */
static const ra8_widget_book_t k_books[] = {
  {.title = "Dune", .author = "Herbert", .cover = 0x00806040U, .percent = 42},
  {.title = "1984", .author = "Orwell", .cover = 0x00404060U, .percent = 100},
};

/** @brief Build a standard 2-column book grid over @p paint. */
static ra8_widget_book_grid_t make_grid(ra8_widget_paint_t* paint)
{
  return (ra8_widget_book_grid_t){.paint     = paint,
                                  .books     = k_books,
                                  .count     = 2,
                                  .cols      = 2,
                                  .bg        = k_t_argb_bg,
                                  .title_fg  = k_t_argb_fg,
                                  .author_fg = k_t_argb_fg_right,
                                  .bar_track = k_t_argb_bar_track,
                                  .bar_fill  = k_t_argb_bar_fill,
                                  .pad       = 8,
                                  .gap       = 8,
                                  .label_h   = 16,
                                  .bar_h     = 4};
}

/**
 * @test ra8_widget_book_grid renders the background then every card.
 *
 * @par MC/DC:
 * `internal_bg_render`: `count == 0 || books == NULL` false arm (2 cards drawn);
 * `cols >= min` true arm (cols 2). `internal_bg_card`: `cover.h > 0` true arm
 * (tall cells -> cover fills); `draw_text != NULL` true arm (title + author);
 * `fw > 0 && fill_rect != NULL` true arm (both books have percent > 0 -> a bar
 * fill). Two cards => bg(1) + 2 x (cover + track + bar-fill = 3) = 7 fills, and
 * 2 x (title + author) = 4 texts.
 */
static void test_book_grid_render(void)
{
  TEST_BEGIN("ra8_widget_book_grid: background + cards");
  mock_paint_t           mp    = {.glyph_w = 8, .glyph_h = 16};
  ra8_widget_paint_t     paint = make_paint(&mp, true);
  ra8_widget_book_grid_t g     = make_grid(&paint);
  ra8_widget_t           w     = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_widget_book_grid_init(&w, &g));
  w.rect = (ra8_ui_rect_t){.x = 0, .y = 0, .w = k_t_wide_w, .h = k_t_row_w};

  w.vt->render(&w);
  TEST_ASSERT_EQ(7U, mp.fill_calls);
  TEST_ASSERT_EQ(4U, mp.text_calls);
  TEST_END("ra8_widget_book_grid: background + cards");
}

/**
 * @test ra8_widget_book_grid renders the degenerate-cell + zero-progress arms.
 *
 * @par MC/DC:
 * `internal_bg_card`: `cover.h > 0` false arm (a short cell whose labels + bar
 * consume the whole height -> no cover fill); `fw > 0 && fill_rect != NULL` --
 * `fw > 0` false arm (a percent-0 book -> track only, no bar fill) and, in the
 * second render below, `fill_rect != NULL` false arm (a percent-100 book with a
 * NULL fill backend -> the `fw > 0` term is true but the bar fill is still
 * skipped). Those two arms plus the true-true arm in test_book_grid_render()
 * give each term of the bar-fill decision independent MC/DC influence.
 * `internal_bg_label` `text == NULL` true arm (a book with a NULL author ->
 * author row skipped). `cols >= min` false arm (cols 0 -> clamped to 1 column).
 */
static void test_book_grid_render_edges(void)
{
  TEST_BEGIN("ra8_widget_book_grid: short cell + zero progress");
  mock_paint_t       mp    = {.glyph_w = 8, .glyph_h = 16};
  ra8_widget_paint_t paint = make_paint(&mp, true);

  /* One empty (percent 0) book with a NULL author, in a single (cols 0 -> 1)
   * column, short cell. */
  static const ra8_widget_book_t one = {.title   = "T",
                                        .author  = nullptr,
                                        .cover   = 0x00112233U,
                                        .percent = 0};
  ra8_widget_book_grid_t         g   = make_grid(&paint);
  g.books                            = &one;
  g.count                            = 1;
  g.cols                             = 0; /* clamped to 1 */
  ra8_widget_t w                     = {};
  (void)ra8_widget_book_grid_init(&w, &g);
  /* content height = 52 - 2*pad(8) = 36 = 2*label_h(16) + bar_h(4) exactly, so
   * the cover band has zero height (cover.h == 0 -> skipped). */
  w.rect = (ra8_ui_rect_t){.x = 0, .y = 0, .w = k_t_bar_w, .h = k_t_card_h};

  w.vt->render(&w);
  /* bg(1) + track(1); no cover (h==0), no bar fill (percent 0). */
  TEST_ASSERT_EQ(2U, mp.fill_calls);
  TEST_ASSERT_EQ(1U, mp.text_calls); /* title drawn; NULL author skipped */

  /* Second render: fw > 0 (percent 100) but fill_rect == NULL -> the bar-fill
   * decision's `fill_rect != NULL` false arm. With no fill backend every
   * priv_fill_box is a no-op, so nothing is recorded. */
  mock_paint_t       mp2              = {.glyph_w = 8, .glyph_h = 16};
  ra8_widget_paint_t pnf              = make_paint(&mp2, true);
  pnf.fill_rect                       = nullptr;
  static const ra8_widget_book_t full = {.title   = "F",
                                         .author  = nullptr,
                                         .cover   = 0x00112233U,
                                         .percent = 100};
  ra8_widget_book_grid_t         gnf  = make_grid(&pnf);
  gnf.books                           = &full;
  gnf.count                           = 1;
  gnf.cols                            = 1;
  ra8_widget_t wnf                    = {};
  (void)ra8_widget_book_grid_init(&wnf, &gnf);
  wnf.rect = (ra8_ui_rect_t){.x = 0, .y = 0, .w = k_t_bar_w, .h = k_t_row_w};
  wnf.vt->render(&wnf);
  TEST_ASSERT_EQ(0U, mp2.fill_calls); /* fill_rect NULL -> no fills recorded */
  TEST_END("ra8_widget_book_grid: short cell + zero progress");
}

/**
 * @test ra8_widget_book_grid routes a touch to the card it landed in.
 *
 * @par MC/DC:
 * `internal_bg_on_input`: `kind != touch` false (touch) + true (button); `count
 * == 0` false + (guards) true; the per-card `rect_contains(cell)` true (a tap in
 * card 1's cell) + false (a tap in no cell -> declined); `on_open != NULL` true
 * + (guards) false.
 */
static void test_book_grid_input(void)
{
  TEST_BEGIN("ra8_widget_book_grid: card touch routing");
  s_bg_open_calls              = 0U;
  s_bg_open_idx                = k_t_idx_unset;
  mock_paint_t           mp    = {.glyph_w = 8, .glyph_h = 16};
  ra8_widget_paint_t     paint = make_paint(&mp, true);
  ra8_widget_book_grid_t g     = make_grid(&paint);
  g.on_open                    = test_bg_on_open;
  ra8_widget_t w               = {};
  (void)ra8_widget_book_grid_init(&w, &g);
  w.rect = (ra8_ui_rect_t){.x = 0, .y = 0, .w = k_t_wide_w, .h = k_t_row_w};

  /* content = {8,8,184,104}; cell_w = (184-8)/2 = 88; cell0 = {8,8,88,104},
   * cell1 = {104,8,88,104}. A tap at (150, 60) is in card 1. */
  const ra8_widget_event_t t1 = {.kind = k_ra8_widget_ev_touch, .x = 150, .y = 60};
  TEST_ASSERT_EQ(true, w.vt->on_input(&w, &t1));
  TEST_ASSERT_EQ(1U, g.selected);
  TEST_ASSERT_EQ(1U, s_bg_open_calls);
  TEST_ASSERT_EQ(1U, s_bg_open_idx);
  TEST_ASSERT_EQ(true, w.dirty);

  /* a tap in the gap between cards (x=98) hits no cell -> declined. */
  const ra8_widget_event_t gap = {.kind = k_ra8_widget_ev_touch, .x = 98, .y = 60};
  TEST_ASSERT_EQ(false, w.vt->on_input(&w, &gap));

  /* a button event is declined. */
  const ra8_widget_event_t bev = {.kind = k_ra8_widget_ev_button};
  TEST_ASSERT_EQ(false, w.vt->on_input(&w, &bev));
  TEST_END("ra8_widget_book_grid: card touch routing");
}

/**
 * @brief Render / input guard arms for `book_grid` (the bulk of the case).
 *
 * @details
 * Split out of the test body so the case stays under the review size cap:
 * these arms drive the widget with a mock painter, the init-argument guards
 * below are a separate concern.
 *
 * @pre The mock paint vtable is available to this translation unit.
 * @pre No previous case left mock counters set.
 * @post Every guarded path has been driven at least once.
 * @post All assertions have run; a failure aborts the process.
 *
 * @note Not thread-safe; the mocks are file-scope state.
 */
static void book_grid_guard_arms(void)
{
  s_bg_open_calls              = 0U;
  mock_paint_t           mp    = {.glyph_w = 8, .glyph_h = 16};
  ra8_widget_paint_t     paint = make_paint(&mp, true);
  ra8_widget_book_grid_t g     = make_grid(&paint);
  ra8_widget_t           w     = {};
  (void)ra8_widget_book_grid_init(&w, &g);
  w.rect = (ra8_ui_rect_t){.x = 0, .y = 0, .w = k_t_wide_w, .h = k_t_row_w};

  /* count == 0 -> bg only. */
  g.count = 0;
  w.vt->render(&w);
  TEST_ASSERT_EQ(1U, mp.fill_calls);

  /* books == NULL -> bg only. */
  mp      = (mock_paint_t){.glyph_w = 8, .glyph_h = 16};
  g.count = 2;
  g.books = nullptr;
  w.vt->render(&w);
  TEST_ASSERT_EQ(1U, mp.fill_calls);

  /* draw_text == NULL -> cards paint but no labels. */
  mp              = (mock_paint_t){.glyph_w = 8, .glyph_h = 16};
  g.books         = k_books;
  paint.draw_text = nullptr;
  w.vt->render(&w);
  TEST_ASSERT_EQ(0U, mp.text_calls);

  /* input: count == 0 declined; no-callback grid still records selected. */
  const ra8_widget_event_t t = {.kind = k_ra8_widget_ev_touch, .x = 150, .y = 60};
  g.count                    = 0;
  TEST_ASSERT_EQ(false, w.vt->on_input(&w, &t));
  g.count   = 2;
  g.on_open = nullptr;
  TEST_ASSERT_EQ(true, w.vt->on_input(&w, &t));
  TEST_ASSERT_EQ(1U, g.selected);
  TEST_ASSERT_EQ(0U, s_bg_open_calls);

  /* paint == NULL / ctx == NULL. */
  mp      = (mock_paint_t){};
  g.paint = nullptr;
  w.vt->render(&w);
  TEST_ASSERT_EQ(0U, mp.fill_calls);
  w.ctx = nullptr;
  w.vt->render(&w);
  TEST_ASSERT_EQ(false, w.vt->on_input(&w, &t));
}

/**
 * @test ra8_widget_book_grid render + input + init guard arms.
 *
 * @par MC/DC:
 * `internal_bg_render`: `count == 0` true (bg only) and `books == NULL` true
 * (bg only); `paint == NULL` / `g == NULL` true (nothing). `internal_bg_card`
 * `draw_text == NULL` true (no labels). `internal_bg_on_input`: `count == 0`
 * true (declined); `on_open == NULL` false-callback (still records selected);
 * `g == NULL` true (declined). Plus init NULL guards.
 */
static void test_book_grid_guards(void)
{
  TEST_BEGIN("ra8_widget_book_grid: render + input + init guards");
  book_grid_guard_arms();

  /* init guards. */
  ra8_widget_book_grid_t any = {};
  ra8_widget_t           ww  = {};
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_widget_book_grid_init(nullptr, &any));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_widget_book_grid_init(&ww, nullptr));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_widget_book_grid_init(&ww, &any));
  TEST_ASSERT_EQ(true, ww.vt == ra8_widget_book_grid_vtable());
  TEST_END("ra8_widget_book_grid: render + input + init guards");
}
/**
 * @brief Test entry point -- runs the book-grid suite in order.
 * @return 0 on success; unity_minimal.h exits non-zero on first failure.
 * @pre None.
 * @pre None.
 * @post All tests executed (or the process exited on first failure).
 * @post stderr carries a per-test RUN/PASS log.
 * @note Not thread-safe. No SIGALRM / timers used.
 * @since 0.1.0
 */
int32_t main(void)
{
  test_book_grid_render();
  test_book_grid_render_edges();
  test_book_grid_input();
  test_book_grid_guards();
  return 0;
}
