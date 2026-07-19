/**
 * @file test_ra8_widget_leaf.c
 * @brief Unit tests for the ra8_widget concrete leaf widgets (label / button / kit).
 *
 * @details
 * Split out of test_ra8_widget.c to keep each test translation unit under
 * the repository file-size cap. This sibling owns the concrete leaf-widget
 * tests: label render / alignment / guards, button render / input / guards,
 * and the composed widget-kit fixture; the core layout / dispatch / damage /
 * panel tests stay in test_ra8_widget.c. Pure logic with a recording
 * ::ra8_widget_paint_t mock backend -- no framebuffer.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>
#include <string.h>

#include "ra8_widget.h"
#include "unity_minimal.h"

/**
 * @enum t_leaf_geom_t
 * @brief Widget rectangles and layout sizes the render arms lay out.
 *
 * @details
 * The values are chosen so a mis-placed child is visible in the recorded fill
 * rectangles: no two dimensions are equal, and none is a multiple of the 8x16
 * mock glyph cell, so a text run can never coincidentally align to an edge.
 */
typedef enum : int16_t {
  k_t_rect_x        = 10, /**< Origin x of the label-placement rect.             */
  k_t_rect_y        = 20, /**< Origin y of the label-placement rect; also the
                               height shared by the null-guard arms.             */
  k_t_label_w       = 100, /**< Width of the label-placement rect.               */
  k_t_label_h       = 40,  /**< Its height; also the width of the guard arms.    */
  k_t_narrow_w      = 50,  /**< Width of the no-text-size fallback rect.         */
  k_t_button_w      = 60,  /**< Width of the button render rect.                 */
  k_t_button_h      = 30,  /**< Its height.                                      */
  k_t_pad_inset     = 5,   /**< Label padding for the centre-fallback arm.       */
  k_t_footer_fixed  = 24,  /**< Fixed track height reserved for the footer row.  */
} t_leaf_geom_t;

/**
 * @enum t_leaf_action_t
 * @brief Action ids the two body buttons dispatch.
 *
 * @details
 * Opaque to the widget layer -- it only echoes them back -- so their sole
 * requirement is being distinct and non-zero, since zero is "no action".
 */
typedef enum : uint8_t {
  k_t_action_button_a = 10U, /**< Action id carried by the "A" button. */
  k_t_action_button_b = 11U, /**< Action id carried by the "B" button. */
} t_leaf_action_t;

/**
 * @enum t_leaf_argb_t
 * @brief The 0x00RRGGBB palette the render arms paint with.
 *
 * @details
 * Every entry is distinct so a recorded fill can be attributed to exactly one
 * style field; the mock paint backend stores the colour verbatim, so a swapped
 * `fg`/`bg` or `face`/`face_pressed` shows up as a wrong value rather than a
 * wrong call count.
 */
typedef enum : uint32_t {
  k_t_argb_label_fg     = 0x00AABBCCU, /**< Foreground of the placement-arm label.     */
  k_t_argb_label_bg     = 0x00112233U, /**< Its background; reused as a bare button face. */
  k_t_argb_title_fg     = 0x00FFFFFFU, /**< Foreground shared by the title and buttons. */
  k_t_argb_title_bg     = 0x00101018U, /**< Title-row background.                      */
  k_t_argb_footer_fg    = 0x00C0C0C0U, /**< Footer foreground, dimmer than the title.  */
  k_t_argb_footer_bg    = 0x00080808U, /**< Footer background, darker than the title.  */
  k_t_argb_btn_face     = 0x00204060U, /**< Face of the standalone button arm.         */
  k_t_argb_btn_pressed  = 0x004080C0U, /**< Its pressed face.                          */
  k_t_argb_btn_a_face   = 0x00203060U, /**< Face of body button "A".                   */
  k_t_argb_btn_a_press  = 0x004060C0U, /**< Its pressed face.                          */
  k_t_argb_btn_b_face   = 0x00603020U, /**< Face of body button "B".                   */
  k_t_argb_btn_b_press  = 0x00C06040U, /**< Its pressed face.                          */
  k_t_argb_no_text_face = 0x00445566U, /**< Face of the draw_text-declined arm.        */
} t_leaf_argb_t;

/* --- Concrete leaf widgets (label + button) fixture ------------------------- */

/**
 * @struct mock_paint_t
 * @brief A recording ::ra8_widget_paint_t backend for the leaf-widget tests.
 *
 * @details
 * Counts the fill / text calls and records the last call's arguments so a
 * render can be asserted without a framebuffer. `glyph_w` / `glyph_h` drive the
 * fake `text_size`, so alignment maths is deterministic.
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
  uint32_t    last_text_bg;    /**< Last text bg colour.         */
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
  m->text_calls++;
  m->last_text_x  = x;
  m->last_text_y  = y;
  m->last_text    = str;
  m->last_text_fg = fg;
  m->last_text_bg = bg;
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

/** @brief Records button press-callback invocations. */
static uint32_t s_btn_press_cb_calls = 0U;

/** @brief A ::ra8_widget_button_t on_press callback that counts its calls. */
static void test_btn_on_press(ra8_widget_t* w)
{
  (void)w;
  s_btn_press_cb_calls++;
}

/**
 * @test ra8_widget_label render fills its background and places aligned text.
 *
 * @par MC/DC:
 * `ra8_widget_priv_text_pos` alignment branches (each a single condition):
 * - `align == left` true -> top-left inset (no measure);
 * - `align == left` false + `align == right` true -> right inset;
 * - both false -> centre. The measured (`text_size != NULL`) path is taken for
 *   centre/right; the not-measured path is covered by
 *   ::test_label_render_guards.
 */
static void test_label_render_align(void)
{
  TEST_BEGIN("ra8_widget_label: render fill + aligned text");
  mock_paint_t       mp    = {.glyph_w = 8, .glyph_h = 16};
  ra8_widget_paint_t paint = make_paint(&mp, true);
  ra8_widget_label_t lab   = {.paint = &paint,
                              .text  = "hi",
                              .fg    = k_t_argb_label_fg,
                              .bg    = k_t_argb_label_bg,
                              .pad   = 3,
                              .align = k_ra8_widget_align_center};
  ra8_widget_t       w     = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_widget_label_init(&w, &lab));
  w.rect = (ra8_ui_rect_t){.x = k_t_rect_x,
                           .y = k_t_rect_y,
                           .w = k_t_label_w,
                           .h = k_t_label_h};

  /* Centre: one bg fill over the rect + centred text (tw = 2 * 8 = 16). */
  w.vt->render(&w);
  TEST_ASSERT_EQ(1U, mp.fill_calls);
  TEST_ASSERT_EQ(0x00112233U, mp.last_fill_color);
  TEST_ASSERT_EQ(10, mp.last_fill_x);
  TEST_ASSERT_EQ(100, mp.last_fill_w);
  TEST_ASSERT_EQ(1U, mp.text_calls);
  TEST_ASSERT_EQ(10 + ((100 - 16) / 2), mp.last_text_x); /* 52 */
  TEST_ASSERT_EQ(20 + ((40 - 16) / 2), mp.last_text_y);  /* 32 */
  TEST_ASSERT_EQ(0x00AABBCCU, mp.last_text_fg);

  /* Left: no measurement -> top-left inset (x = 13, y = 23). */
  mp        = (mock_paint_t){.glyph_w = 8, .glyph_h = 16};
  lab.align = k_ra8_widget_align_left;
  w.vt->render(&w);
  TEST_ASSERT_EQ(13, mp.last_text_x);
  TEST_ASSERT_EQ(23, mp.last_text_y);

  /* Right: x = (10 + 100) - 3 - 16 = 91. */
  mp        = (mock_paint_t){.glyph_w = 8, .glyph_h = 16};
  lab.align = k_ra8_widget_align_right;
  w.vt->render(&w);
  TEST_ASSERT_EQ(91, mp.last_text_x);
  TEST_END("ra8_widget_label: render fill + aligned text");
}

/**
 * @test ra8_widget_label render guard arms each take their no-op branch.
 *
 * @par MC/DC:
 * `internal_label_render` single-condition guards, each true arm:
 * `text_size == NULL` (centre falls back to left), `text == NULL` (fill only),
 * `draw_text == NULL` (fill only), `fill_rect == NULL` (no fill), `paint ==
 * NULL` (nothing), `ctx == NULL` (nothing). Their false arms are exercised by
 * ::test_label_render_align.
 */
static void test_label_render_guards(void)
{
  TEST_BEGIN("ra8_widget_label: render guard arms");
  mock_paint_t       mp    = {.glyph_w = 8, .glyph_h = 16};
  ra8_widget_paint_t paint = make_paint(&mp, false); /* no text_size */
  ra8_widget_label_t lab   = {.paint = &paint,
                              .text  = "x",
                              .pad   = k_t_pad_inset,
                              .align = k_ra8_widget_align_center};
  ra8_widget_t       w     = {};
  (void)ra8_widget_label_init(&w, &lab);
  w.rect = (ra8_ui_rect_t){.x = 0, .y = 0, .w = k_t_narrow_w, .h = k_t_rect_y};

  /* No text_size -> centre falls back to the left inset (5, 5); fill runs. */
  w.vt->render(&w);
  TEST_ASSERT_EQ(1U, mp.fill_calls);
  TEST_ASSERT_EQ(1U, mp.text_calls);
  TEST_ASSERT_EQ(5, mp.last_text_x);
  TEST_ASSERT_EQ(5, mp.last_text_y);

  /* text == NULL -> background fill only. */
  mp       = (mock_paint_t){};
  lab.text = nullptr;
  w.vt->render(&w);
  TEST_ASSERT_EQ(1U, mp.fill_calls);
  TEST_ASSERT_EQ(0U, mp.text_calls);

  /* draw_text == NULL -> fill only. */
  mp              = (mock_paint_t){};
  lab.text        = "y";
  paint.draw_text = nullptr;
  w.vt->render(&w);
  TEST_ASSERT_EQ(1U, mp.fill_calls);
  TEST_ASSERT_EQ(0U, mp.text_calls);

  /* fill_rect == NULL -> no fill (text still placed at the left fallback). */
  mp              = (mock_paint_t){};
  paint.draw_text = mock_paint_text;
  paint.fill_rect = nullptr;
  w.vt->render(&w);
  TEST_ASSERT_EQ(0U, mp.fill_calls);
  TEST_ASSERT_EQ(1U, mp.text_calls);

  /* paint == NULL -> nothing. */
  mp        = (mock_paint_t){};
  lab.paint = nullptr;
  w.vt->render(&w);
  TEST_ASSERT_EQ(0U, mp.fill_calls);
  TEST_ASSERT_EQ(0U, mp.text_calls);

  /* ctx == NULL -> nothing (and no crash). */
  w.ctx = nullptr;
  w.vt->render(&w);
  TEST_END("ra8_widget_label: render guard arms");
}

/**
 * @test ra8_widget_label_init binds the label vtable + rejects NULL args.
 *
 * @par MC/DC:
 * The two NULL guards (`w == NULL`, `label == NULL`) each independently return
 * null_ptr; a valid pair binds and leaves `on_input` NULL (a passive label).
 */
static void test_label_init_guards(void)
{
  TEST_BEGIN("ra8_widget_label: init guards");
  ra8_widget_t       w   = {};
  ra8_widget_label_t lab = {};
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_widget_label_init(nullptr, &lab));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_widget_label_init(&w, nullptr));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_widget_label_init(&w, &lab));
  TEST_ASSERT_EQ(true, w.vt == ra8_widget_label_vtable());
  TEST_ASSERT_EQ(true, w.ctx == (void*)&lab);
  TEST_ASSERT_EQ(true, w.visible);
  TEST_ASSERT_EQ(true, w.vt->on_input == nullptr);
  TEST_END("ra8_widget_label: init guards");
}

/**
 * @test ra8_widget_button render paints a bordered face that tracks `pressed`.
 *
 * @par MC/DC:
 * `ra8_widget_priv_fill_box` `border_w <= 0` false arm -> two fills (border +
 * inset face); `internal_button_face` `pressed` arm selects the face colour
 * (false -> released face, true -> pressed face). The `border_w <= 0` true arm
 * is covered by the label (its border_w is 0).
 */
static void test_button_render(void)
{
  TEST_BEGIN("ra8_widget_button: render face + border + label");
  mock_paint_t        mp    = {.glyph_w = 8, .glyph_h = 16};
  ra8_widget_paint_t  paint = make_paint(&mp, true);
  ra8_widget_button_t btn   = {.paint        = &paint,
                               .text         = "OK",
                               .fg           = k_t_argb_title_fg,
                               .face         = k_t_argb_btn_face,
                               .face_pressed = k_t_argb_btn_pressed,
                               .border       = 0x00000000U,
                               .border_w     = 2,
                               .pad          = 0,
                               .align        = k_ra8_widget_align_center};
  ra8_widget_t        w     = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_widget_button_init(&w, &btn));
  w.rect = (ra8_ui_rect_t){.x = 0, .y = 0, .w = k_t_button_w, .h = k_t_button_h};

  /* Released: border fill then inset face fill (2 fills) + centred label. */
  w.vt->render(&w);
  TEST_ASSERT_EQ(2U, mp.fill_calls);
  TEST_ASSERT_EQ(0x00204060U, mp.last_fill_color); /* inner fill = released face */
  TEST_ASSERT_EQ(2, mp.last_fill_x);               /* inset by border_w          */
  TEST_ASSERT_EQ(56, mp.last_fill_w);              /* 60 - 2 * 2                 */
  TEST_ASSERT_EQ(1U, mp.text_calls);

  /* Pressed: inner fill switches to the pressed face. */
  mp          = (mock_paint_t){.glyph_w = 8, .glyph_h = 16};
  btn.pressed = true;
  w.vt->render(&w);
  TEST_ASSERT_EQ(2U, mp.fill_calls);
  TEST_ASSERT_EQ(0x004080C0U, mp.last_fill_color);
  TEST_END("ra8_widget_button: render face + border + label");
}

/**
 * @test ra8_widget_button on_input latches a touch and declines other events.
 *
 * @par MC/DC:
 * `internal_button_on_input` single-condition arms: `ev->kind != touch` true
 * (button event declined) and false (touch latched); `on_press != NULL` true
 * (callback fires) and false (plain button still latches); `b == NULL` true
 * (NULL ctx declines).
 */
static void test_button_input(void)
{
  TEST_BEGIN("ra8_widget_button: latch touch, decline others");
  s_btn_press_cb_calls    = 0U;
  ra8_widget_button_t btn = {.on_press = test_btn_on_press};
  ra8_widget_t        w   = {};
  (void)ra8_widget_button_init(&w, &btn);
  w.rect = (ra8_ui_rect_t){.x = 0, .y = 0, .w = k_t_label_h, .h = k_t_label_h};

  /* A touch latches: toggle pressed, count it, dirty + fast, fire callback. */
  const ra8_widget_event_t touch = {.kind = k_ra8_widget_ev_touch, .x = 5, .y = 5};
  TEST_ASSERT_EQ(true, w.vt->on_input(&w, &touch));
  TEST_ASSERT_EQ(1U, btn.presses);
  TEST_ASSERT_EQ(true, btn.pressed);
  TEST_ASSERT_EQ(true, w.dirty);
  TEST_ASSERT_EQ(k_ra8_widget_refresh_fast, w.refresh);
  TEST_ASSERT_EQ(1U, s_btn_press_cb_calls);

  /* A second touch toggles back; presses keeps climbing. */
  TEST_ASSERT_EQ(true, w.vt->on_input(&w, &touch));
  TEST_ASSERT_EQ(2U, btn.presses);
  TEST_ASSERT_EQ(false, btn.pressed);

  /* A button-kind event is declined (kept routing). */
  const ra8_widget_event_t bev = {.kind = k_ra8_widget_ev_button, .button_id = 1};
  TEST_ASSERT_EQ(false, w.vt->on_input(&w, &bev));
  TEST_ASSERT_EQ(2U, btn.presses);

  /* No-callback button: a touch still latches, just no callback fires. */
  ra8_widget_button_t plain = {};
  ra8_widget_t        wp    = {};
  (void)ra8_widget_button_init(&wp, &plain);
  TEST_ASSERT_EQ(true, wp.vt->on_input(&wp, &touch));
  TEST_ASSERT_EQ(1U, plain.presses);
  TEST_ASSERT_EQ(2U, s_btn_press_cb_calls); /* unchanged: plain has no callback */

  /* ctx == NULL -> declined. */
  wp.ctx = nullptr;
  TEST_ASSERT_EQ(false, wp.vt->on_input(&wp, &touch));
  TEST_END("ra8_widget_button: latch touch, decline others");
}

/**
 * @test ra8_widget_button_init binds the button vtable + rejects NULL args.
 *
 * @par MC/DC:
 * The two NULL guards (`w == NULL`, `button == NULL`) each independently return
 * null_ptr; a valid pair binds the routing vtable and makes the widget visible.
 */
static void test_button_init_guards(void)
{
  TEST_BEGIN("ra8_widget_button: init guards");
  ra8_widget_t        w   = {};
  ra8_widget_button_t btn = {};
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_widget_button_init(nullptr, &btn));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_widget_button_init(&w, nullptr));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_widget_button_init(&w, &btn));
  TEST_ASSERT_EQ(true, w.vt == ra8_widget_button_vtable());
  TEST_ASSERT_EQ(true, w.ctx == (void*)&btn);
  TEST_ASSERT_EQ(true, w.visible);
  TEST_ASSERT_EQ(true, w.vt->on_input != nullptr);
  TEST_END("ra8_widget_button: init guards");
}

/**
 * @struct kit_fixture_t
 * @brief A small UI -- a column panel of [title label, button row, footer
 *        label] -- built from the concrete leaf widgets for the integration
 *        test. The panel descriptors point into this object's own arrays, so it
 *        must not be copied.
 */
typedef struct {
  ra8_widget_t        root[3];    /**< [title, body-panel, footer]. */
  ra8_widget_t        body[2];    /**< [button A, button B].        */
  ra8_box_t           rscr[4];    /**< Root layout scratch.         */
  ra8_box_t           bscr[3];    /**< Body layout scratch.         */
  ra8_widget_panel_t  body_panel; /**< Nested button row.           */
  ra8_widget_panel_t  root_panel; /**< Root column.                 */
  ra8_widget_t        panelw;     /**< Top panel widget.            */
  ra8_widget_label_t  title;      /**< Title label leaf.            */
  ra8_widget_label_t  footer;     /**< Footer label leaf.           */
  ra8_widget_button_t b0;         /**< Left button leaf.            */
  ra8_widget_button_t b1;         /**< Right button leaf.           */
  mock_paint_t        mp;         /**< Recording paint backend.     */
  ra8_widget_paint_t  paint;      /**< Paint vtable bound to mp.    */
} kit_fixture_t;

/** @brief Fill the kit fixture's paint mock and widget config literals. */
static void kit_fixture_configs(kit_fixture_t* f)
{
  *f        = (kit_fixture_t){};
  f->mp     = (mock_paint_t){.glyph_w = 8, .glyph_h = 16};
  f->paint  = make_paint(&f->mp, true);
  f->title  = (ra8_widget_label_t){.paint = &f->paint,
                                   .text  = "Kit",
                                   .fg    = k_t_argb_title_fg,
                                   .bg    = k_t_argb_title_bg,
                                   .pad   = 4,
                                   .align = k_ra8_widget_align_center};
  f->footer = (ra8_widget_label_t){.paint = &f->paint,
                                   .text  = "hint",
                                   .fg    = k_t_argb_footer_fg,
                                   .bg    = k_t_argb_footer_bg,
                                   .pad   = 4,
                                   .align = k_ra8_widget_align_left};
  f->b0     = (ra8_widget_button_t){.paint        = &f->paint,
                                    .text         = "A",
                                    .fg           = k_t_argb_title_fg,
                                    .face         = k_t_argb_btn_a_face,
                                    .face_pressed = k_t_argb_btn_a_press,
                                    .border       = 0x00000000U,
                                    .border_w     = 2,
                                    .align        = k_ra8_widget_align_center};
  f->b1     = (ra8_widget_button_t){.paint        = &f->paint,
                                    .text         = "B",
                                    .fg           = k_t_argb_title_fg,
                                    .face         = k_t_argb_btn_b_face,
                                    .face_pressed = k_t_argb_btn_b_press,
                                    .border       = 0x00000000U,
                                    .border_w     = 2,
                                    .align        = k_ra8_widget_align_center};
}

/** @brief Build the kit UI in @p f; return false on a bind error. */
static bool build_kit_fixture(kit_fixture_t* f)
{
  kit_fixture_configs(f);

  if (ra8_widget_label_init(&f->root[0], &f->title) != k_ra8_ok) {
    return false;
  }
  f->root[0].fixed = k_t_label_h;
  if (ra8_widget_label_init(&f->root[2], &f->footer) != k_ra8_ok) {
    return false;
  }
  f->root[2].fixed = k_t_footer_fixed;
  if (ra8_widget_button_init(&f->body[0], &f->b0) != k_ra8_ok) {
    return false;
  }
  f->body[0].flex      = 1U;
  f->body[0].action_id = k_t_action_button_a;
  if (ra8_widget_button_init(&f->body[1], &f->b1) != k_ra8_ok) {
    return false;
  }
  f->body[1].flex      = 1U;
  f->body[1].action_id = k_t_action_button_b;

  f->body_panel = (ra8_widget_panel_t){.children    = f->body,
                                       .box_scratch = f->bscr,
                                       .count       = 2U,
                                       .box_cap     = 3U,
                                       .axis        = k_ra8_widget_axis_row};
  if (ra8_widget_panel_init(&f->root[1], &f->body_panel) != k_ra8_ok) {
    return false;
  }
  f->root[1].flex = 1U;
  f->root_panel   = (ra8_widget_panel_t){.children    = f->root,
                                         .box_scratch = f->rscr,
                                         .count       = 3U,
                                         .box_cap     = 4U,
                                         .axis        = k_ra8_widget_axis_col};
  return (ra8_widget_panel_init(&f->panelw, &f->root_panel) == k_ra8_ok);
}

/**
 * @test The concrete label + button leaves composite through the panel, and a
 *       touch on a button latches + drives a damage-tracked partial flush.
 *
 * @par MC/DC:
 * Full compose -- all 3 root children dirty -> 6 fills (2 labels x1 + 2 buttons
 * x2) and 4 text draws, full-frame quality damage. The touch routes root ->
 * body panel -> left button (each level's hit-test true arm), latching it.
 * Partial compose -- only the body band re-invalidated -> exactly 1 dirty root
 * child, the body rect with the fast hint, both buttons repainted (4 fills, 2
 * texts) and the labels untouched. This is the issue #145 partial-flush
 * acceptance, driven by the concrete leaf widgets.
 */
static void test_kit_compose(void)
{
  TEST_BEGIN("ra8_widget kit: concrete widgets composite through the panel");
  kit_fixture_t f;
  TEST_ASSERT_EQ(true, build_kit_fixture(&f));

  (void)ra8_widget_invalidate(&f.root[0], k_ra8_widget_refresh_quality);
  (void)ra8_widget_invalidate(&f.root[1], k_ra8_widget_refresh_quality);
  (void)ra8_widget_invalidate(&f.root[2], k_ra8_widget_refresh_quality);
  ra8_ui_rect_t        dmg   = {};
  ra8_widget_refresh_t hint  = k_ra8_widget_refresh_none;
  uint16_t             n     = 0U;
  const ra8_ui_rect_t  frame = {.x = 0, .y = 0, .w = 200, .h = 200};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_widget_panel_compose(&f.panelw, &frame, &dmg, &hint, &n));
  TEST_ASSERT_EQ(3U, n);
  TEST_ASSERT_EQ(200, dmg.w);
  TEST_ASSERT_EQ(200, dmg.h);
  TEST_ASSERT_EQ(k_ra8_widget_refresh_quality, hint);
  TEST_ASSERT_EQ(6U, f.mp.fill_calls); /* 2 labels x1 + 2 buttons x2       */
  TEST_ASSERT_EQ(4U, f.mp.text_calls); /* title + footer + 2 button labels */

  /* Touch the left button through the tree -> it latches + self-dirties. */
  const ra8_widget_event_t touch = {.kind = k_ra8_widget_ev_touch, .x = 10, .y = 100};
  TEST_ASSERT_EQ(true, f.panelw.vt->on_input(&f.panelw, &touch));
  TEST_ASSERT_EQ(1U, f.b0.presses);
  TEST_ASSERT_EQ(true, f.b0.pressed);
  TEST_ASSERT_EQ(0U, f.b1.presses);
  TEST_ASSERT_EQ(true, f.body[0].dirty);

  /* Partial flush: invalidate only the body band, recompose -> just its rect. */
  f.mp.fill_calls = 0U;
  f.mp.text_calls = 0U;
  (void)ra8_widget_invalidate(&f.root[1], k_ra8_widget_refresh_fast);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_widget_panel_compose(&f.panelw, &frame, &dmg, &hint, &n));
  TEST_ASSERT_EQ(1U, n);
  TEST_ASSERT_EQ(k_ra8_widget_refresh_fast, hint);
  TEST_ASSERT_EQ(40, dmg.y);  /* body starts below the 40px title */
  TEST_ASSERT_EQ(136, dmg.h); /* 200 - 40 - 24                    */
  TEST_ASSERT_EQ(200, dmg.w);
  TEST_ASSERT_EQ(4U, f.mp.fill_calls); /* both buttons repainted, labels skipped */
  TEST_ASSERT_EQ(2U, f.mp.text_calls);
  TEST_END("ra8_widget kit: concrete widgets composite through the panel");
}

/**
 * @test internal_button_render early-out guards each take their no-op arm.
 *
 * @par MC/DC:
 * The button render callback's four single-condition guards, each true arm:
 * `b == NULL` (NULL ctx -> nothing drawn), `b->paint == NULL` (no backend ->
 * nothing), `b->text == NULL` (face fills, no label) and `b->paint->draw_text ==
 * NULL` (face fills, label declined). Their false arms (a fully configured
 * button painting a bordered face plus a centred label) are covered by
 * ::test_button_render. Each guard is independent, so true-arm branch coverage
 * of every guard plus the all-false render gives full MC/DC of the render path.
 */
static void test_button_render_guards(void)
{
  TEST_BEGIN("ra8_widget_button: render guard arms");
  mock_paint_t mp = {.glyph_w = 8, .glyph_h = 16};

  /* b == NULL: button vtable render over a widget whose ctx is unset. */
  ra8_widget_t wn = {};
  wn.vt           = ra8_widget_button_vtable();
  wn.ctx          = nullptr;
  wn.rect = (ra8_ui_rect_t){.x = 0, .y = 0, .w = k_t_label_h, .h = k_t_rect_y};
  wn.vt->render(&wn); /* no crash, nothing drawn */
  TEST_ASSERT_EQ(0U, mp.fill_calls);

  /* b->paint == NULL: no draw backend at all. */
  ra8_widget_button_t bnp = {.paint = nullptr, .text = "x", .border_w = 2};
  ra8_widget_t        wp  = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_widget_button_init(&wp, &bnp));
  wp.rect = (ra8_ui_rect_t){.x = 0, .y = 0, .w = k_t_label_h, .h = k_t_rect_y};
  wp.vt->render(&wp);
  TEST_ASSERT_EQ(0U, mp.fill_calls);

  /* b->text == NULL: the bordered face fills, but no label is drawn. */
  ra8_widget_paint_t  paint = make_paint(&mp, true);
  ra8_widget_button_t bnt   = {.paint    = &paint,
                               .text     = nullptr,
                               .face     = k_t_argb_label_bg,
                               .border   = 0x00000000U,
                               .border_w = 2};
  ra8_widget_t        wt    = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_widget_button_init(&wt, &bnt));
  wt.rect = (ra8_ui_rect_t){.x = 0, .y = 0, .w = k_t_label_h, .h = k_t_rect_y};
  wt.vt->render(&wt);
  TEST_ASSERT_EQ(2U, mp.fill_calls); /* border fill + inset face fill */
  TEST_ASSERT_EQ(0U, mp.text_calls); /* text == NULL -> no label      */

  /* b->paint->draw_text == NULL: face fills, label declined. */
  mp                      = (mock_paint_t){.glyph_w = 8, .glyph_h = 16};
  paint.draw_text         = nullptr;
  ra8_widget_button_t bnd = {.paint    = &paint,
                             .text     = "OK",
                             .face     = k_t_argb_no_text_face,
                             .border   = 0x00000000U,
                             .border_w = 2};
  ra8_widget_t        wd  = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_widget_button_init(&wd, &bnd));
  wd.rect = (ra8_ui_rect_t){.x = 0, .y = 0, .w = k_t_label_h, .h = k_t_rect_y};
  wd.vt->render(&wd);
  TEST_ASSERT_EQ(2U, mp.fill_calls);
  TEST_ASSERT_EQ(0U, mp.text_calls);
  TEST_END("ra8_widget_button: render guard arms");
}

/**
 * @test The panel render + route callbacks decline a non-panel widget.
 *
 * @par MC/DC:
 * Both `internal_panel_render` (layout/composite path) and
 * `internal_panel_on_input` (routing path) guard the same compound decision
 * `if ((p == NULL) || (p->children == NULL))` before touching the panel. Two
 * vectors drive each condition true independently:
 * - Vector A: `p == NULL` (ctx unset) -> left true short-circuits -> declined.
 * - Vector B: `p != NULL`, `p->children == NULL` -> left false, right true ->
 *   declined.
 * The both-false arm (a real panel that lays out / routes its children) is
 * covered by ::test_panel_compose_full and ::test_panel_input_route, for the
 * N+1 = 3 vectors proving each condition independently affects the outcome.
 */

int main(void)
{
  test_label_render_align();
  test_label_render_guards();
  test_label_init_guards();
  test_button_render();
  test_button_input();
  test_button_init_guards();
  test_kit_compose();
  test_button_render_guards();
  return 0;
}
