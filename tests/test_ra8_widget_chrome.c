/**
 * @file test_ra8_widget_chrome.c
 * @brief Unit tests for the ra8_widget concrete chrome leaves (#145 Phase 2).
 *
 * @details
 * Covers the pure Dependency-Injection-painted chrome widgets extracted from the
 * e-reader: the progress bar, the status bar, the toolbar (search field + count
 * chip), the navigation strip, and the book grid. Each renders through a
 * recording mock ::ra8_widget_paint_t backend (no framebuffer, so this runs
 * in-process) and routes synthetic touches, so both the paint maths and the
 * hit-routing decisions are asserted with full MC/DC vectors.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>
#include <string.h>

#include "ra8_widget.h"
#include "ra8_widget_book.h"
#include "ra8_widget_nav_bar.h"
#include "ra8_widget_progress_bar.h"
#include "ra8_widget_status_bar.h"
#include "ra8_widget_toolbar.h"
#include "unity_minimal.h"

/**
 * @enum widget_chrome_uint8_const_t
 * @brief Named uint8_t constants used by this file.
 *
 * @details
 * Every literal this translation unit needs, named so the
 * value's role is visible at the point of use (CLAUDE.md
 * "No Magic Numbers").
 */
typedef enum : uint8_t {
  k_widget_chrome_count_w_72 = 72,
  k_widget_chrome_h_24       = 24,
  k_widget_chrome_h_30       = 30,
  k_widget_chrome_h_40       = 40,
  k_widget_chrome_h_52       = 52,
  k_widget_chrome_total_10   = 10,
  k_widget_chrome_value_15   = 15,
  k_widget_chrome_w_100      = 100,
  k_widget_chrome_w_120      = 120,
  k_widget_chrome_w_200      = 200,
  k_widget_chrome_x_5        = 5,
} widget_chrome_uint8_const_t;

/**
 * @enum widget_chrome_uint16_const_t
 * @brief Named uint16_t constants used by this file.
 *
 * @details
 * Every literal this translation unit needs, named so the
 * value's role is visible at the point of use (CLAUDE.md
 * "No Magic Numbers").
 */
typedef enum : uint16_t {
  k_widget_chrome_s_nav_select_idx_ffff = 0xFFFFU,
} widget_chrome_uint16_const_t;

/**
 * @enum widget_chrome_uint32_const_t
 * @brief Named uint32_t constants used by this file.
 *
 * @details
 * Every literal this translation unit needs, named so the
 * value's role is visible at the point of use (CLAUDE.md
 * "No Magic Numbers").
 */
typedef enum : uint32_t {
  k_widget_chrome_bg_00ffffff       = 0x00FFFFFFU,
  k_widget_chrome_fg_00101010       = 0x00101010U,
  k_widget_chrome_fg_muted_00a0a0a0 = 0x00A0A0A0U,
  k_widget_chrome_fg_right_00808080 = 0x00808080U,
  k_widget_chrome_field_00f0f0f0    = 0x00F0F0F0U,
  k_widget_chrome_fill_00e0a020     = 0x00E0A020U,
  k_widget_chrome_hint_fg_00909090  = 0x00909090U,
  k_widget_chrome_rule_00c0c0c0     = 0x00C0C0C0U,
  k_widget_chrome_track_00303030    = 0x00303030U,
} widget_chrome_uint32_const_t;

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

/* --- Progress bar ----------------------------------------------------------- */

/**
 * @test ra8_widget_progress_bar render fills the track then a proportional fill.
 *
 * @par MC/DC:
 * `ra8_widget_priv_fill_frac` single-condition guards, both arms of each:
 * - `total == 0`: false (partial / full / saturate vectors) and true (total 0 ->
 *   track only);
 * - `width <= 0`: false (normal rect) and true (zero-width rect -> track only);
 * - `value > total`: false (value 3 <= 10 -> 30% fill) and true (value 15 > 10 ->
 *   saturates to a full bar).
 * Plus `internal_pb_render`'s `fw > 0` guard: true (partial fill painted) and
 * false (value 0 -> track only).
 */
static void test_progress_bar_render(void)
{
  TEST_BEGIN("ra8_widget_progress_bar: track + proportional fill");
  mock_paint_t              mp    = {.glyph_w = 8, .glyph_h = 16};
  ra8_widget_paint_t        paint = make_paint(&mp, true);
  ra8_widget_progress_bar_t bar   = {.paint = &paint,
                                     .track = k_widget_chrome_track_00303030,
                                     .fill  = k_widget_chrome_fill_00e0a020,
                                     .value = 3,
                                     .total = k_widget_chrome_total_10};
  ra8_widget_t              w     = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_widget_progress_bar_init(&w, &bar));
  w.rect = (ra8_ui_rect_t){.x = k_widget_chrome_x_5, .y = 6, .w = k_widget_chrome_w_100, .h = 8};

  /* Partial: track fill + 30px fill (100 * 3 / 10). */
  w.vt->render(&w);
  TEST_ASSERT_EQ(2U, mp.fill_calls);
  TEST_ASSERT_EQ(30, mp.last_fill_w);
  TEST_ASSERT_EQ(0x00E0A020U, mp.last_fill_color);
  TEST_ASSERT_EQ(5, mp.last_fill_x);

  /* Saturate: value > total clamps to a full-width fill. */
  mp        = (mock_paint_t){};
  bar.value = k_widget_chrome_value_15;
  w.vt->render(&w);
  TEST_ASSERT_EQ(2U, mp.fill_calls);
  TEST_ASSERT_EQ(100, mp.last_fill_w);

  /* Empty: value 0 -> only the track fills (fw > 0 false). */
  mp        = (mock_paint_t){};
  bar.value = 0;
  w.vt->render(&w);
  TEST_ASSERT_EQ(1U, mp.fill_calls);

  /* total == 0 -> only the track fills. */
  mp        = (mock_paint_t){};
  bar.value = k_widget_chrome_x_5;
  bar.total = 0;
  w.vt->render(&w);
  TEST_ASSERT_EQ(1U, mp.fill_calls);

  /* Zero-width rect -> fill_frac's width guard, track fill only. */
  mp        = (mock_paint_t){};
  bar.total = k_widget_chrome_total_10;
  w.rect.w  = 0;
  w.vt->render(&w);
  TEST_ASSERT_EQ(1U, mp.fill_calls);
  TEST_END("ra8_widget_progress_bar: track + proportional fill");
}

/**
 * @test ra8_widget_progress_bar render + init guard arms.
 *
 * @par MC/DC:
 * `internal_pb_render` single-condition guards, each true arm: `pb == NULL`
 * (ctx unset), `paint == NULL`, `paint->fill_rect == NULL` -- each paints
 * nothing. Their false arms are covered by ::test_progress_bar_render. Plus the
 * two init NULL guards (`w == NULL`, `bar == NULL`), each -> null_ptr.
 */
static void test_progress_bar_guards(void)
{
  TEST_BEGIN("ra8_widget_progress_bar: render + init guards");
  mock_paint_t mp = {};

  /* pb == NULL: bound vtable over a NULL ctx. */
  ra8_widget_t wn = {.vt = ra8_widget_progress_bar_vtable(), .ctx = nullptr};
  wn.rect         = (ra8_ui_rect_t){.x = 0, .y = 0, .w = k_widget_chrome_total_10, .h = 4};
  wn.vt->render(&wn);
  TEST_ASSERT_EQ(0U, mp.fill_calls);

  /* paint == NULL. */
  ra8_widget_progress_bar_t bnp = {.paint = nullptr,
                                   .total = k_widget_chrome_total_10,
                                   .value = k_widget_chrome_x_5};
  ra8_widget_t              wp  = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_widget_progress_bar_init(&wp, &bnp));
  wp.vt->render(&wp);
  TEST_ASSERT_EQ(0U, mp.fill_calls);

  /* fill_rect == NULL. */
  ra8_widget_paint_t pnf        = make_paint(&mp, true);
  pnf.fill_rect                 = nullptr;
  ra8_widget_progress_bar_t bnf = {.paint = &pnf,
                                   .total = k_widget_chrome_total_10,
                                   .value = k_widget_chrome_x_5};
  ra8_widget_t              wf  = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_widget_progress_bar_init(&wf, &bnf));
  wf.rect = (ra8_ui_rect_t){.x = 0, .y = 0, .w = k_widget_chrome_total_10, .h = 4};
  wf.vt->render(&wf);
  TEST_ASSERT_EQ(0U, mp.fill_calls);

  /* init NULL guards. */
  ra8_widget_progress_bar_t any = {};
  ra8_widget_t              ww  = {};
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_widget_progress_bar_init(nullptr, &any));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_widget_progress_bar_init(&ww, nullptr));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_widget_progress_bar_init(&ww, &any));
  TEST_ASSERT_EQ(true, ww.vt == ra8_widget_progress_bar_vtable());
  TEST_ASSERT_EQ(true, ww.visible);
  TEST_ASSERT_EQ(true, ww.vt->on_input == nullptr);
  TEST_END("ra8_widget_progress_bar: render + init guards");
}

/* --- Status bar ------------------------------------------------------------- */

/**
 * @test ra8_widget_status_bar renders fill + left/right labels + bottom rule.
 *
 * @par MC/DC:
 * `internal_sb_render`: `rule_h <= 0` false arm (rule_h 1 -> the rule strip fills
 * at the band bottom) and, in ::test_status_bar_guards, its true arm (rule_h 0 ->
 * no rule). `internal_sb_label` `text == NULL` false arm for both labels (both
 * drawn). Right alignment exercises the measured `text_size` path.
 */
static void test_status_bar_render(void)
{
  TEST_BEGIN("ra8_widget_status_bar: fill + labels + rule");
  mock_paint_t            mp    = {.glyph_w = 8, .glyph_h = 16};
  ra8_widget_paint_t      paint = make_paint(&mp, true);
  ra8_widget_status_bar_t sb    = {.paint    = &paint,
                                   .left     = "Library",
                                   .right    = "12:00",
                                   .bg       = k_widget_chrome_bg_00ffffff,
                                   .fg       = k_widget_chrome_fg_00101010,
                                   .fg_right = k_widget_chrome_fg_right_00808080,
                                   .rule     = k_widget_chrome_rule_00c0c0c0,
                                   .pad      = 8,
                                   .rule_h   = 1};
  ra8_widget_t            w     = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_widget_status_bar_init(&w, &sb));
  w.rect = (ra8_ui_rect_t){.x = 0, .y = 0, .w = k_widget_chrome_w_200, .h = k_widget_chrome_h_40};

  w.vt->render(&w);
  /* bg fill (1) + rule fill (1) = 2 fills; left + right = 2 texts. */
  TEST_ASSERT_EQ(2U, mp.fill_calls);
  TEST_ASSERT_EQ(2U, mp.text_calls);
  /* the last fill is the bottom rule: y = 40 - 1 = 39, h = 1. */
  TEST_ASSERT_EQ(39, mp.last_fill_y);
  TEST_ASSERT_EQ(1, mp.last_fill_h);
  TEST_ASSERT_EQ(0x00C0C0C0U, mp.last_fill_color);
  /* the last text is the right-aligned "12:00": x = 200 - 8 - (5 * 8) = 152. */
  TEST_ASSERT_EQ(152, mp.last_text_x);
  TEST_ASSERT_EQ(0x00808080U, mp.last_text_fg);
  TEST_END("ra8_widget_status_bar: fill + labels + rule");
}

/**
 * @test ra8_widget_status_bar guard + init arms.
 *
 * @par MC/DC:
 * - `internal_sb_render` `rule_h <= 0` true arm (rule_h 0 -> bg + labels, no rule
 *   fill); its second guard `fill_rect == NULL` true arm (rule wanted but no
 *   backend fill -> no rule).
 * - `internal_sb_label` `text == NULL` true arm (NULL left + right -> no text);
 *   `draw_text == NULL` true arm (no text backend -> fill only).
 * - `paint == NULL` / `sb == NULL` render guards -> nothing drawn.
 * - init NULL guards.
 */
/**
 * @brief Render / input guard arms for `status_bar` (the bulk of the case).
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
static void status_bar_guard_arms(void)
{
  mock_paint_t       mp    = {.glyph_w = 8, .glyph_h = 16};
  ra8_widget_paint_t paint = make_paint(&mp, true);

  /* rule_h == 0 -> bg fill + 2 labels, no rule fill. */
  ra8_widget_status_bar_t sb = {.paint = &paint, .left = "L", .right = "R", .rule_h = 0, .pad = 4};
  ra8_widget_t            w  = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_widget_status_bar_init(&w, &sb));
  w.rect = (ra8_ui_rect_t){.x = 0, .y = 0, .w = k_widget_chrome_w_100, .h = k_widget_chrome_h_30};
  w.vt->render(&w);
  TEST_ASSERT_EQ(1U, mp.fill_calls); /* bg only */
  TEST_ASSERT_EQ(2U, mp.text_calls);

  /* NULL labels -> bg fill only. */
  mp       = (mock_paint_t){.glyph_w = 8, .glyph_h = 16};
  sb.left  = nullptr;
  sb.right = nullptr;
  w.vt->render(&w);
  TEST_ASSERT_EQ(1U, mp.fill_calls);
  TEST_ASSERT_EQ(0U, mp.text_calls);

  /* draw_text == NULL -> bg fill, no labels. */
  mp              = (mock_paint_t){.glyph_w = 8, .glyph_h = 16};
  sb.left         = "L";
  paint.draw_text = nullptr;
  w.vt->render(&w);
  TEST_ASSERT_EQ(1U, mp.fill_calls);
  TEST_ASSERT_EQ(0U, mp.text_calls);

  /* rule wanted but fill_rect == NULL -> no rule (and no bg). */
  mp              = (mock_paint_t){.glyph_w = 8, .glyph_h = 16};
  paint.draw_text = mock_paint_text;
  paint.fill_rect = nullptr;
  sb.rule_h       = 2;
  w.vt->render(&w);
  TEST_ASSERT_EQ(0U, mp.fill_calls);

  /* paint == NULL / ctx == NULL. */
  mp       = (mock_paint_t){.glyph_w = 8, .glyph_h = 16};
  sb.paint = nullptr;
  w.vt->render(&w);
  TEST_ASSERT_EQ(0U, mp.fill_calls);
  w.ctx = nullptr;
  w.vt->render(&w);
}

static void test_status_bar_guards(void)
{
  TEST_BEGIN("ra8_widget_status_bar: guard + init arms");
  status_bar_guard_arms();

  /* init guards. */
  ra8_widget_status_bar_t any = {};
  ra8_widget_t            ww  = {};
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_widget_status_bar_init(nullptr, &any));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_widget_status_bar_init(&ww, nullptr));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_widget_status_bar_init(&ww, &any));
  TEST_ASSERT_EQ(true, ww.vt == ra8_widget_status_bar_vtable());
  TEST_END("ra8_widget_status_bar: guard + init arms");
}

/* --- Toolbar ---------------------------------------------------------------- */

/** @brief Records toolbar search-callback invocations. */
static uint32_t s_tb_search_calls = 0U;

/** @brief A ::ra8_widget_toolbar_t on_search callback that counts its calls. */
static void test_tb_on_search(ra8_widget_t* w)
{
  (void)w;
  s_tb_search_calls++;
}

/** @brief Build a standard toolbar fixture over @p paint. */
static ra8_widget_toolbar_t make_toolbar(ra8_widget_paint_t* paint)
{
  return (ra8_widget_toolbar_t){.paint     = paint,
                                .hint      = "Search",
                                .count     = "12 books",
                                .on_search = test_tb_on_search,
                                .bg        = k_widget_chrome_bg_00ffffff,
                                .field     = k_widget_chrome_field_00f0f0f0,
                                .border    = k_widget_chrome_rule_00c0c0c0,
                                .hint_fg   = k_widget_chrome_hint_fg_00909090,
                                .count_fg  = k_widget_chrome_hint_fg_00909090,
                                .pad       = 8,
                                .border_w  = 1,
                                .count_w   = k_widget_chrome_count_w_72};
}

/**
 * @test ra8_widget_toolbar renders bg + bordered field + hint + count.
 *
 * @par MC/DC:
 * `internal_tb_render`: `draw_text == NULL` false arm (both texts drawn),
 * `hint != NULL` true arm, `count != NULL` true arm. The bordered field is a
 * `border_w > 0` `priv_fill_box` (two fills), so the render issues bg(1) +
 * field-border(1) + field-face(1) = 3 fills and hint + count = 2 texts.
 */
static void test_toolbar_render(void)
{
  TEST_BEGIN("ra8_widget_toolbar: fill + field + hint + count");
  mock_paint_t         mp    = {.glyph_w = 8, .glyph_h = 16};
  ra8_widget_paint_t   paint = make_paint(&mp, true);
  ra8_widget_toolbar_t tb    = make_toolbar(&paint);
  ra8_widget_t         w     = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_widget_toolbar_init(&w, &tb));
  w.rect = (ra8_ui_rect_t){.x = 0, .y = 0, .w = k_widget_chrome_w_200, .h = k_widget_chrome_h_40};

  w.vt->render(&w);
  TEST_ASSERT_EQ(3U, mp.fill_calls);
  TEST_ASSERT_EQ(2U, mp.text_calls);
  /* last text is the right-aligned count "12 books" (8 chars): x = 200-8-64 = 128. */
  TEST_ASSERT_EQ(128, mp.last_text_x);
  TEST_END("ra8_widget_toolbar: fill + field + hint + count");
}

/**
 * @test ra8_widget_toolbar routes a search-field tap and declines the rest.
 *
 * @par MC/DC:
 * `internal_tb_on_input`: `ev->kind != touch` false (touch) + true (button
 * declined); the `rect_contains(field)` guard true (tap at x=50 inside the
 * field) + false (tap at x=180 in the count chip declined); `on_search != NULL`
 * true (callback fires) + false (no-callback toolbar still latches); `bar ==
 * NULL` true (NULL ctx declines).
 */
static void test_toolbar_input(void)
{
  TEST_BEGIN("ra8_widget_toolbar: field tap routing");
  s_tb_search_calls          = 0U;
  mock_paint_t         mp    = {.glyph_w = 8, .glyph_h = 16};
  ra8_widget_paint_t   paint = make_paint(&mp, true);
  ra8_widget_toolbar_t tb    = make_toolbar(&paint);
  ra8_widget_t         w     = {};
  (void)ra8_widget_toolbar_init(&w, &tb);
  w.rect = (ra8_ui_rect_t){.x = 0, .y = 0, .w = k_widget_chrome_w_200, .h = k_widget_chrome_h_40};

  /* field = {8, 8, 200-16-72-8=104, 24}; a tap at (50, 20) is inside it. */
  const ra8_widget_event_t hit = {.kind = k_ra8_widget_ev_touch, .x = 50, .y = 20};
  TEST_ASSERT_EQ(true, w.vt->on_input(&w, &hit));
  TEST_ASSERT_EQ(1U, tb.searches);
  TEST_ASSERT_EQ(true, w.dirty);
  TEST_ASSERT_EQ(k_ra8_widget_refresh_fast, w.refresh);
  TEST_ASSERT_EQ(1U, s_tb_search_calls);

  /* a tap at (180, 20) is in the count chip, right of the field -> declined. */
  const ra8_widget_event_t miss = {.kind = k_ra8_widget_ev_touch, .x = 180, .y = 20};
  TEST_ASSERT_EQ(false, w.vt->on_input(&w, &miss));
  TEST_ASSERT_EQ(1U, tb.searches);

  /* a button event is declined. */
  const ra8_widget_event_t bev = {.kind = k_ra8_widget_ev_button, .button_id = 1};
  TEST_ASSERT_EQ(false, w.vt->on_input(&w, &bev));

  /* no-callback toolbar: a field tap still latches. */
  ra8_widget_toolbar_t plain = make_toolbar(&paint);
  plain.on_search            = nullptr;
  ra8_widget_t wp            = {};
  (void)ra8_widget_toolbar_init(&wp, &plain);
  wp.rect = (ra8_ui_rect_t){.x = 0, .y = 0, .w = k_widget_chrome_w_200, .h = k_widget_chrome_h_40};
  TEST_ASSERT_EQ(true, wp.vt->on_input(&wp, &hit));
  TEST_ASSERT_EQ(1U, plain.searches);
  TEST_ASSERT_EQ(1U, s_tb_search_calls); /* unchanged: plain has no callback */

  /* ctx == NULL -> declined. */
  wp.ctx = nullptr;
  TEST_ASSERT_EQ(false, wp.vt->on_input(&wp, &hit));
  TEST_END("ra8_widget_toolbar: field tap routing");
}

/**
 * @test ra8_widget_toolbar render + init guard arms.
 *
 * @par MC/DC:
 * `internal_tb_render` `draw_text == NULL` true arm (field only, no text);
 * `hint == NULL` / `count == NULL` true arms (each text skipped); `paint ==
 * NULL` / `bar == NULL` true arms (nothing drawn). Plus the two init NULL
 * guards.
 */
static void test_toolbar_guards(void)
{
  TEST_BEGIN("ra8_widget_toolbar: render + init guards");
  mock_paint_t         mp    = {.glyph_w = 8, .glyph_h = 16};
  ra8_widget_paint_t   paint = make_paint(&mp, true);
  ra8_widget_toolbar_t tb    = make_toolbar(&paint);
  ra8_widget_t         w     = {};
  (void)ra8_widget_toolbar_init(&w, &tb);
  w.rect = (ra8_ui_rect_t){.x = 0, .y = 0, .w = k_widget_chrome_w_200, .h = k_widget_chrome_h_40};

  /* draw_text == NULL -> bg + field (3 fills), no text. */
  paint.draw_text = nullptr;
  w.vt->render(&w);
  TEST_ASSERT_EQ(3U, mp.fill_calls);
  TEST_ASSERT_EQ(0U, mp.text_calls);

  /* hint == NULL + count == NULL -> field only, no text. */
  mp              = (mock_paint_t){.glyph_w = 8, .glyph_h = 16};
  paint.draw_text = mock_paint_text;
  tb.hint         = nullptr;
  tb.count        = nullptr;
  w.vt->render(&w);
  TEST_ASSERT_EQ(3U, mp.fill_calls);
  TEST_ASSERT_EQ(0U, mp.text_calls);

  /* paint == NULL / ctx == NULL. */
  mp       = (mock_paint_t){};
  tb.paint = nullptr;
  w.vt->render(&w);
  TEST_ASSERT_EQ(0U, mp.fill_calls);
  w.ctx = nullptr;
  w.vt->render(&w);
  TEST_ASSERT_EQ(false,
                 w.vt->on_input(&w, &(const ra8_widget_event_t){.kind = k_ra8_widget_ev_touch}));

  /* init guards. */
  ra8_widget_toolbar_t any = {};
  ra8_widget_t         ww  = {};
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_widget_toolbar_init(nullptr, &any));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_widget_toolbar_init(&ww, nullptr));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_widget_toolbar_init(&ww, &any));
  TEST_ASSERT_EQ(true, ww.vt == ra8_widget_toolbar_vtable());
  TEST_END("ra8_widget_toolbar: render + init guards");
}

/* --- Navigation strip ------------------------------------------------------- */

/** @brief Records nav-strip select-callback invocations + last index. */
static uint32_t s_nav_select_calls = 0U;
static uint16_t s_nav_select_idx   = k_widget_chrome_s_nav_select_idx_ffff;

/** @brief A ::ra8_widget_nav_bar_t on_select callback that records its args. */
static void test_nav_on_select(ra8_widget_t* w, uint16_t index)
{
  (void)w;
  s_nav_select_calls++;
  s_nav_select_idx = index;
}

/** @brief Three nav destinations. */
static const char* const k_nav_items[] = {"Library", "Store", "Settings"};

/**
 * @test ra8_widget_nav_bar renders equal cells with active vs muted colours.
 *
 * @par MC/DC:
 * `internal_nav_render`: `count == 0` false arm (3 items drawn); the per-item
 * `i == active` decision -- true for the active index (fg_active) and false for
 * the rest (fg_muted). `internal_nav_item` `text == NULL` false arm (all drawn).
 * The strip fills once (bg) and draws 3 labels.
 */
static void test_nav_bar_render(void)
{
  TEST_BEGIN("ra8_widget_nav_bar: equal cells + active colour");
  mock_paint_t         mp    = {.glyph_w = 8, .glyph_h = 16};
  ra8_widget_paint_t   paint = make_paint(&mp, true);
  ra8_widget_nav_bar_t nav   = {.paint     = &paint,
                                .items     = k_nav_items,
                                .count     = 3,
                                .active    = 1,
                                .bg        = k_widget_chrome_bg_00ffffff,
                                .fg_active = k_widget_chrome_fg_00101010,
                                .fg_muted  = k_widget_chrome_fg_muted_00a0a0a0};
  ra8_widget_t         w     = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_widget_nav_bar_init(&w, &nav));
  w.rect = (ra8_ui_rect_t){.x = 0, .y = 0, .w = k_widget_chrome_w_120, .h = k_widget_chrome_h_24};

  w.vt->render(&w);
  TEST_ASSERT_EQ(1U, mp.fill_calls); /* bg */
  TEST_ASSERT_EQ(3U, mp.text_calls);
  /* the last item (index 2, "Settings") is muted. */
  TEST_ASSERT_EQ(0x00A0A0A0U, mp.last_text_fg);
  TEST_ASSERT_EQ(true, mp.last_text == k_nav_items[2]);
  TEST_END("ra8_widget_nav_bar: equal cells + active colour");
}

/**
 * @test ra8_widget_nav_bar routes a touch to the cell it landed in.
 *
 * @par MC/DC:
 * `internal_nav_hit`: `count == 0` false + (in guards test) true; `w <= 0`
 * false + true; the in-range guard `px < x || px >= x + w`: false-false (hit),
 * false-true (tap off the right edge, varies the `px >= x + w` term), and
 * true-false (tap off the left edge at px < x, varies the `px < x` term) -- the
 * N+1 = 3 vectors that give each term independent influence.
 * `internal_nav_on_input`: `kind != touch` false + true; `on_select != NULL`
 * true + false.
 */
static void test_nav_bar_input(void)
{
  TEST_BEGIN("ra8_widget_nav_bar: cell touch routing");
  s_nav_select_calls         = 0U;
  s_nav_select_idx           = k_widget_chrome_s_nav_select_idx_ffff;
  mock_paint_t         mp    = {.glyph_w = 8, .glyph_h = 16};
  ra8_widget_paint_t   paint = make_paint(&mp, true);
  ra8_widget_nav_bar_t nav   = {.paint     = &paint,
                                .items     = k_nav_items,
                                .count     = 3,
                                .active    = 0,
                                .on_select = test_nav_on_select};
  ra8_widget_t         w     = {};
  (void)ra8_widget_nav_bar_init(&w, &nav);
  w.rect = (ra8_ui_rect_t){.x = 0, .y = 0, .w = k_widget_chrome_w_120, .h = k_widget_chrome_h_24};

  /* cells: [0,40) [40,80) [80,120). A tap at x=50 -> cell 1. */
  const ra8_widget_event_t t1 = {.kind = k_ra8_widget_ev_touch, .x = 50, .y = 10};
  TEST_ASSERT_EQ(true, w.vt->on_input(&w, &t1));
  TEST_ASSERT_EQ(1U, nav.selected);
  TEST_ASSERT_EQ(1U, s_nav_select_calls);
  TEST_ASSERT_EQ(1U, s_nav_select_idx);
  TEST_ASSERT_EQ(true, w.dirty);

  /* a tap near the right edge (x=115) -> cell 2 (no clamp needed). */
  const ra8_widget_event_t t2 = {.kind = k_ra8_widget_ev_touch, .x = 115, .y = 10};
  TEST_ASSERT_EQ(true, w.vt->on_input(&w, &t2));
  TEST_ASSERT_EQ(2U, nav.selected);

  /* a tap off the right edge (x=120) -> declined (px >= x + w term true). */
  const ra8_widget_event_t off = {.kind = k_ra8_widget_ev_touch, .x = 120, .y = 10};
  TEST_ASSERT_EQ(false, w.vt->on_input(&w, &off));

  /* a tap off the left edge (x=-1 < strip->x) -> declined; this is the
   * `px < x` true / `px >= x + w` false vector that gives the first term of the
   * in-range guard its independent MC/DC influence. */
  const ra8_widget_event_t offl = {.kind = k_ra8_widget_ev_touch, .x = -1, .y = 10};
  TEST_ASSERT_EQ(false, w.vt->on_input(&w, &offl));

  /* a button event is declined. */
  const ra8_widget_event_t bev = {.kind = k_ra8_widget_ev_button};
  TEST_ASSERT_EQ(false, w.vt->on_input(&w, &bev));

  /* no-callback strip still records selected (t1 + t2 already fired on_select
   * twice; a NULL-callback tap records `selected` without a third call). */
  nav.on_select               = nullptr;
  const ra8_widget_event_t t0 = {.kind = k_ra8_widget_ev_touch, .x = 10, .y = 10};
  TEST_ASSERT_EQ(true, w.vt->on_input(&w, &t0));
  TEST_ASSERT_EQ(0U, nav.selected);
  TEST_ASSERT_EQ(2U, s_nav_select_calls); /* unchanged by the NULL-callback tap */
  TEST_END("ra8_widget_nav_bar: cell touch routing");
}

/**
 * @brief Exercise the render OR-guard arms (count / items / draw_text / labels).
 * @param[in,out] w     Widget bound to @p nav.
 * @param[in,out] nav   Nav-bar widget under test.
 * @param[in,out] paint Paint sink (draw_text toggled).
 * @param[in,out] mp    Paint-call counters asserted per vector.
 * @return None.
 * @pre @p w is initialised over @p nav.
 * @post Each render arm produced the expected fill/text counts; valid items and
 *       draw_text are restored on exit.
 * @note Not thread-safe; single-threaded host-test helper.
 * @since 0.1.0
 */
static void nav_render_guards(ra8_widget_t*         w,
                              ra8_widget_nav_bar_t* nav,
                              ra8_widget_paint_t*   paint,
                              mock_paint_t*         mp)
{
  /* count == 0 -> bg only. */
  w->vt->render(w);
  TEST_ASSERT_EQ(1U, mp->fill_calls);
  TEST_ASSERT_EQ(0U, mp->text_calls);

  /* items == NULL -> bg only (right arm of the render OR-guard). */
  *mp        = (mock_paint_t){.glyph_w = 8, .glyph_h = 16};
  nav->count = 3;
  nav->items = nullptr;
  w->vt->render(w);
  TEST_ASSERT_EQ(1U, mp->fill_calls);
  TEST_ASSERT_EQ(0U, mp->text_calls);

  /* draw_text == NULL -> bg only (left arm of the render OR-guard). */
  *mp              = (mock_paint_t){.glyph_w = 8, .glyph_h = 16};
  nav->items       = k_nav_items;
  paint->draw_text = nullptr;
  w->vt->render(w);
  TEST_ASSERT_EQ(1U, mp->fill_calls);
  TEST_ASSERT_EQ(0U, mp->text_calls);
  paint->draw_text = mock_paint_text;

  /* an item with a NULL label is skipped. */
  static const char* const holey[] = {"A", nullptr, "C"};
  *mp                              = (mock_paint_t){.glyph_w = 8, .glyph_h = 16};
  nav->items                       = holey;
  w->vt->render(w);
  TEST_ASSERT_EQ(2U, mp->text_calls); /* only A + C */
}

/**
 * @test ra8_widget_nav_bar render + hit + init guard arms.
 *
 * @par MC/DC:
 * `internal_nav_render`: `count == 0` true arm (bg only); `draw_text == NULL ||
 * items == NULL` true arms (bg only). `internal_nav_item` `text == NULL` true
 * arm (that cell skipped). `internal_nav_hit` `count == 0` true + `w <= 0` true.
 * `internal_nav_on_input` `nav == NULL` true. Plus init NULL guards.
 */
static void test_nav_bar_guards(void)
{
  TEST_BEGIN("ra8_widget_nav_bar: render + hit + init guards");
  mock_paint_t         mp    = {.glyph_w = 8, .glyph_h = 16};
  ra8_widget_paint_t   paint = make_paint(&mp, true);
  ra8_widget_nav_bar_t nav   = {.paint = &paint, .items = k_nav_items, .count = 0};
  ra8_widget_t         w     = {};
  (void)ra8_widget_nav_bar_init(&w, &nav);
  w.rect = (ra8_ui_rect_t){.x = 0, .y = 0, .w = k_widget_chrome_w_120, .h = k_widget_chrome_h_24};

  nav_render_guards(&w, &nav, &paint, &mp);

  /* hit guards: count == 0 and w <= 0 both decline a touch. */
  const ra8_widget_event_t t = {.kind = k_ra8_widget_ev_touch, .x = 10, .y = 10};
  nav.items                  = k_nav_items;
  nav.count                  = 0;
  TEST_ASSERT_EQ(false, w.vt->on_input(&w, &t));
  nav.count = 3;
  w.rect.w  = 0;
  TEST_ASSERT_EQ(false, w.vt->on_input(&w, &t));

  /* paint == NULL render arm. */
  mp        = (mock_paint_t){.glyph_w = 8, .glyph_h = 16};
  w.rect.w  = k_widget_chrome_w_120;
  nav.paint = nullptr;
  w.vt->render(&w);
  TEST_ASSERT_EQ(0U, mp.fill_calls);

  /* ctx == NULL declines. */
  w.ctx = nullptr;
  TEST_ASSERT_EQ(false, w.vt->on_input(&w, &t));
  w.vt->render(&w); /* nav == NULL render arm */

  /* init guards. */
  ra8_widget_nav_bar_t any = {};
  ra8_widget_t         ww  = {};
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_widget_nav_bar_init(nullptr, &any));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_widget_nav_bar_init(&ww, nullptr));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_widget_nav_bar_init(&ww, &any));
  TEST_ASSERT_EQ(true, ww.vt == ra8_widget_nav_bar_vtable());
  TEST_END("ra8_widget_nav_bar: render + hit + init guards");
}

/* --- Book grid -------------------------------------------------------------- */

/** @brief Records book-grid open-callback invocations + last index. */
static uint32_t s_bg_open_calls = 0U;
static uint16_t s_bg_open_idx   = k_widget_chrome_s_nav_select_idx_ffff;

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
                                  .bg        = k_widget_chrome_bg_00ffffff,
                                  .title_fg  = k_widget_chrome_fg_00101010,
                                  .author_fg = k_widget_chrome_fg_right_00808080,
                                  .bar_track = k_widget_chrome_track_00303030,
                                  .bar_fill  = k_widget_chrome_fill_00e0a020,
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
  w.rect = (ra8_ui_rect_t){.x = 0, .y = 0, .w = k_widget_chrome_w_200, .h = k_widget_chrome_w_120};

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
  w.rect = (ra8_ui_rect_t){.x = 0, .y = 0, .w = k_widget_chrome_w_100, .h = k_widget_chrome_h_52};

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
  wnf.rect =
    (ra8_ui_rect_t){.x = 0, .y = 0, .w = k_widget_chrome_w_100, .h = k_widget_chrome_w_120};
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
  s_bg_open_idx                = k_widget_chrome_s_nav_select_idx_ffff;
  mock_paint_t           mp    = {.glyph_w = 8, .glyph_h = 16};
  ra8_widget_paint_t     paint = make_paint(&mp, true);
  ra8_widget_book_grid_t g     = make_grid(&paint);
  g.on_open                    = test_bg_on_open;
  ra8_widget_t w               = {};
  (void)ra8_widget_book_grid_init(&w, &g);
  w.rect = (ra8_ui_rect_t){.x = 0, .y = 0, .w = k_widget_chrome_w_200, .h = k_widget_chrome_w_120};

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
 * @test ra8_widget_book_grid render + input + init guard arms.
 *
 * @par MC/DC:
 * `internal_bg_render`: `count == 0` true (bg only) and `books == NULL` true
 * (bg only); `paint == NULL` / `g == NULL` true (nothing). `internal_bg_card`
 * `draw_text == NULL` true (no labels). `internal_bg_on_input`: `count == 0`
 * true (declined); `on_open == NULL` false-callback (still records selected);
 * `g == NULL` true (declined). Plus init NULL guards.
 */
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
  w.rect = (ra8_ui_rect_t){.x = 0, .y = 0, .w = k_widget_chrome_w_200, .h = k_widget_chrome_w_120};

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

int main(void)
{
  test_progress_bar_render();
  test_progress_bar_guards();
  test_status_bar_render();
  test_status_bar_guards();
  test_toolbar_render();
  test_toolbar_input();
  test_toolbar_guards();
  test_nav_bar_render();
  test_nav_bar_input();
  test_nav_bar_guards();
  test_book_grid_render();
  test_book_grid_render_edges();
  test_book_grid_input();
  test_book_grid_guards();
  return 0;
}
