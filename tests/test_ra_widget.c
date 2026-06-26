/**
 * @file test_ra_widget.c
 * @brief Unit tests for the ra_widget composable-UI layer (#145).
 *
 * @details
 * Pure logic -- layout (via ra_box), input routing (via ra_ui hit-test),
 * damage union + refresh-hint folding, and the render-dispatch selection
 * (with a recording mock vtable). No framebuffer, so this runs in-process.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra_widget.h"
#include "unity_minimal.h"

/* --- Recording mock widget -------------------------------------------------- */

typedef struct {
  uint32_t render_calls; /**< Times render() ran.    */
  uint32_t input_calls;  /**< Times on_input() ran.  */
  bool     consume;      /**< on_input return value. */
  uint16_t last_button;  /**< Last button id seen.   */
} mock_ctx_t;

static void mock_measure(ra_widget_t* w, int32_t aw, int32_t ah, int32_t* ow, int32_t* oh)
{
  (void)w;
  *ow = aw;
  *oh = ah;
}

static void mock_render(ra_widget_t* w)
{
  ((mock_ctx_t*)w->ctx)->render_calls++;
}

static bool mock_on_input(ra_widget_t* w, const ra_widget_event_t* ev)
{
  mock_ctx_t* c = (mock_ctx_t*)w->ctx;
  c->input_calls++;
  c->last_button = ev->button_id;
  return c->consume;
}

static const ra_widget_vtable_t k_mock_vt = {
  .measure  = mock_measure,
  .render   = mock_render,
  .on_input = mock_on_input,
};

static ra_widget_t make_widget(mock_ctx_t* ctx, int16_t fixed, uint16_t flex, uint16_t action)
{
  ra_widget_t w = {};
  w.vt          = &k_mock_vt;
  w.ctx         = ctx;
  w.fixed       = fixed;
  w.flex        = flex;
  w.action_id   = action;
  w.visible     = true;
  return w;
}

/**
 * @test ra_widget_layout_stack places fixed + flex children + skips invisible.
 *
 * @par MC/DC:
 * Visibility skip `if (!widgets[i].visible)` (1 condition) -- vectors: a
 * visible child gets a rect; an invisible child is skipped (keeps its rect).
 * Plus the box_cap guard `box_cap < visible+1`.
 */
static void test_layout_stack(void)
{
  TEST_BEGIN("ra_widget: layout_stack fixed+flex+invisible");
  mock_ctx_t  c0 = {}, c1 = {}, c2 = {};
  ra_widget_t ws[3] = {
    make_widget(&c0, 64, 0, 1), /* fixed 64 high   */
    make_widget(&c1, 0, 1, 2),  /* flex fills rest */
    make_widget(&c2, 48, 0, 3), /* fixed 48 high   */
  };
  ra_box_t           scratch[8];
  const ra_ui_rect_t frame = {.x = 0, .y = 0, .w = 100, .h = 300};
  TEST_ASSERT_EQ(
    (int)k_ra_ok,
    (int)ra_widget_layout_stack(ws, 3U, &frame, k_ra_widget_axis_col, 0, 0, scratch, 8U));
  /* col: w0 = top 64, w1 = middle (300-64-48=188), w2 = bottom 48. */
  TEST_ASSERT_EQ(0, ws[0].rect.y);
  TEST_ASSERT_EQ(64, ws[0].rect.h);
  TEST_ASSERT_EQ(64, ws[1].rect.y);
  TEST_ASSERT_EQ(188, ws[1].rect.h);
  TEST_ASSERT_EQ(252, ws[2].rect.y);
  TEST_ASSERT_EQ(48, ws[2].rect.h);

  /* Hide the middle widget: the two fixed ones now bracket the frame. */
  ws[1].visible = false;
  ws[1].rect.h  = 999; /* sentinel: must be left untouched */
  TEST_ASSERT_EQ(
    (int)k_ra_ok,
    (int)ra_widget_layout_stack(ws, 3U, &frame, k_ra_widget_axis_col, 0, 0, scratch, 8U));
  TEST_ASSERT_EQ(64, ws[0].rect.h);
  TEST_ASSERT_EQ(999, ws[1].rect.h); /* invisible: untouched */
  TEST_ASSERT_EQ(48, ws[2].rect.h);

  /* box_cap too small (need visible(2)+1 = 3, give 2). */
  TEST_ASSERT_EQ(
    (int)k_ra_err_invalid_arg,
    (int)ra_widget_layout_stack(ws, 3U, &frame, k_ra_widget_axis_col, 0, 0, scratch, 2U));
  TEST_END("ra_widget: layout_stack fixed+flex+invisible");
}

/**
 * @test ra_widget_dispatch routes touch by hit + skips ineligible widgets.
 *
 * @par MC/DC:
 * Eligibility `!visible || vt==NULL || on_input==NULL` (3 conditions). N+1
 * vectors: eligible+hit -> routed; invisible -> skipped; NULL on_input ->
 * skipped; plus a touch miss -> not handled.
 */
static void test_dispatch_touch(void)
{
  TEST_BEGIN("ra_widget: dispatch touch routing MC/DC");
  mock_ctx_t  c0 = {.consume = true}, c1 = {.consume = true};
  ra_widget_t ws[2] = {make_widget(&c0, 0, 0, 1), make_widget(&c1, 0, 0, 2)};
  ws[0].rect        = (ra_ui_rect_t){.x = 0, .y = 0, .w = 50, .h = 50};
  ws[1].rect        = (ra_ui_rect_t){.x = 50, .y = 0, .w = 50, .h = 50};

  bool                    handled = false;
  const ra_widget_event_t touch1  = {.kind = k_ra_widget_ev_touch, .x = 60, .y = 10};
  /* Vector: eligible widget under the point handles it. */
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_widget_dispatch(ws, 2U, &touch1, &handled));
  TEST_ASSERT_EQ(true, handled);
  TEST_ASSERT_EQ(1U, c1.input_calls);
  TEST_ASSERT_EQ(0U, c0.input_calls);

  /* Vector: touch miss -> not handled. */
  const ra_widget_event_t miss = {.kind = k_ra_widget_ev_touch, .x = 200, .y = 200};
  handled                      = true;
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_widget_dispatch(ws, 2U, &miss, &handled));
  TEST_ASSERT_EQ(false, handled);

  /* Vector: invisible widget under the point is skipped. */
  ws[1].visible  = false;
  c1.input_calls = 0U;
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_widget_dispatch(ws, 2U, &touch1, &handled));
  TEST_ASSERT_EQ(false, handled);
  TEST_ASSERT_EQ(0U, c1.input_calls);

  /* Vector: NULL on_input is skipped (eligible position, no handler). */
  ws[1].visible                              = true;
  static const ra_widget_vtable_t k_no_input = {.measure  = mock_measure,
                                                .render   = mock_render,
                                                .on_input = nullptr};
  ws[1].vt                                   = &k_no_input;
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_widget_dispatch(ws, 2U, &touch1, &handled));
  TEST_ASSERT_EQ(false, handled);
  TEST_END("ra_widget: dispatch touch routing MC/DC");
}

/**
 * @test ra_widget_dispatch offers a button to each widget until one consumes.
 *
 * @par MC/DC:
 * Button path: first widget declines (returns false) -> offered to the next,
 * which consumes. Vectors: none consumes -> not handled; second consumes.
 */
static void test_dispatch_button(void)
{
  TEST_BEGIN("ra_widget: dispatch button first-consumer");
  mock_ctx_t              c0 = {.consume = false}, c1 = {.consume = true};
  ra_widget_t             ws[2]   = {make_widget(&c0, 0, 0, 1), make_widget(&c1, 0, 0, 2)};
  bool                    handled = false;
  const ra_widget_event_t btn     = {.kind = k_ra_widget_ev_button, .button_id = 7};
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_widget_dispatch(ws, 2U, &btn, &handled));
  TEST_ASSERT_EQ(true, handled);
  TEST_ASSERT_EQ(1U, c0.input_calls); /* declined */
  TEST_ASSERT_EQ(1U, c1.input_calls); /* consumed */
  TEST_ASSERT_EQ(7U, c1.last_button);
  TEST_END("ra_widget: dispatch button first-consumer");
}

/**
 * @test ra_widget_invalidate + ra_widget_damage fold dirty rects + hints.
 *
 * @par MC/DC:
 * Damage selector `!visible || !dirty` (2 conditions). Vectors: a clean tree
 * -> count 0, empty rect; one fast-dirty widget -> its rect + fast; adding a
 * quality-dirty widget -> union rect + quality (the strongest hint wins).
 */
static void test_invalidate_damage(void)
{
  TEST_BEGIN("ra_widget: invalidate + damage union/hint");
  mock_ctx_t  c0 = {}, c1 = {};
  ra_widget_t ws[2] = {make_widget(&c0, 0, 0, 1), make_widget(&c1, 0, 0, 2)};
  ws[0].rect        = (ra_ui_rect_t){.x = 0, .y = 0, .w = 100, .h = 40};
  ws[1].rect        = (ra_ui_rect_t){.x = 0, .y = 260, .w = 100, .h = 40};

  ra_ui_rect_t        rect = {};
  ra_widget_refresh_t hint = k_ra_widget_refresh_quality;
  uint16_t            n    = 99U;
  /* Clean -> nothing dirty. */
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_widget_damage(ws, 2U, &rect, &hint, &n));
  TEST_ASSERT_EQ(0U, n);
  TEST_ASSERT_EQ(0, rect.w);
  TEST_ASSERT_EQ((int)k_ra_widget_refresh_none, (int)hint);

  /* One fast-dirty widget -> its own rect, fast hint. */
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_widget_invalidate(&ws[0], k_ra_widget_refresh_fast));
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_widget_damage(ws, 2U, &rect, &hint, &n));
  TEST_ASSERT_EQ(1U, n);
  TEST_ASSERT_EQ(40, rect.h);
  TEST_ASSERT_EQ((int)k_ra_widget_refresh_fast, (int)hint);

  /* Add a quality-dirty widget -> union spans both, quality wins. */
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_widget_invalidate(&ws[1], k_ra_widget_refresh_quality));
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_widget_damage(ws, 2U, &rect, &hint, &n));
  TEST_ASSERT_EQ(2U, n);
  TEST_ASSERT_EQ(0, rect.y);
  TEST_ASSERT_EQ(300, rect.h); /* 0..300 spans both */
  TEST_ASSERT_EQ((int)k_ra_widget_refresh_quality, (int)hint);
  TEST_END("ra_widget: invalidate + damage union/hint");
}

/**
 * @test ra_widget_render_dirty renders only visible+dirty + clears damage.
 *
 * @par MC/DC:
 * Selector `!visible || !dirty` (2 conditions). Vectors: a visible+dirty
 * widget renders + clears; a clean widget does not; an invisible dirty widget
 * does not.
 */
static void test_render_dirty(void)
{
  TEST_BEGIN("ra_widget: render_dirty selection");
  mock_ctx_t  c0 = {}, c1 = {}, c2 = {};
  ra_widget_t ws[3] = {make_widget(&c0, 0, 0, 1),
                       make_widget(&c1, 0, 0, 2),
                       make_widget(&c2, 0, 0, 3)};
  (void)ra_widget_invalidate(&ws[0], k_ra_widget_refresh_fast); /* visible + dirty */
  /* ws[1] stays clean */
  (void)ra_widget_invalidate(&ws[2], k_ra_widget_refresh_quality); /* dirty but invisible */
  ws[2].visible = false;

  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_widget_render_dirty(ws, 3U));
  TEST_ASSERT_EQ(1U, c0.render_calls); /* rendered  */
  TEST_ASSERT_EQ(0U, c1.render_calls); /* clean     */
  TEST_ASSERT_EQ(0U, c2.render_calls); /* invisible */
  TEST_ASSERT_EQ(false, ws[0].dirty);  /* cleared   */
  TEST_ASSERT_EQ((int)k_ra_widget_refresh_none, (int)ws[0].refresh);
  TEST_END("ra_widget: render_dirty selection");
}

/**
 * @test render_dirty skips null-render + null-vt widgets; damage folds a
 *       half-empty rect; every entry rejects a NULL widget array.
 *
 * @par MC/DC:
 * - render guard `(vt != null) && (vt->render != null)`: a null-vt widget
 *   (left false) and a null-render widget (right false) are both skipped, yet
 *   their dirty flag is cleared.
 * - `internal_rect_empty` `(w <= 0) && (h <= 0)`: a half-empty rect (w == 0,
 *   h > 0 -> left true, right false) is folded as non-empty by the union.
 * - the `(count > 0) && (widgets == NULL)` precondition guard returns null_ptr.
 */
static void test_widget_edge_guards(void)
{
  TEST_BEGIN("ra_widget: edge guards + half-empty damage");
  static const ra_widget_vtable_t k_no_render = {.measure  = mock_measure,
                                                 .render   = nullptr,
                                                 .on_input = mock_on_input};

  /* H-2: null-render widget + null-vt widget are skipped but cleared. */
  mock_ctx_t  c0 = {}, c1 = {};
  ra_widget_t ws[2] = {make_widget(&c0, 0, 0, 1), make_widget(&c1, 0, 0, 2)};
  ws[0].vt          = &k_no_render; /* render == null */
  ws[1].vt          = nullptr;      /* vt == null     */
  (void)ra_widget_invalidate(&ws[0], k_ra_widget_refresh_fast);
  (void)ra_widget_invalidate(&ws[1], k_ra_widget_refresh_fast);
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_widget_render_dirty(ws, 2U));
  TEST_ASSERT_EQ(0U, c0.render_calls);
  TEST_ASSERT_EQ(false, ws[0].dirty);
  TEST_ASSERT_EQ(false, ws[1].dirty);

  /* H-1: a half-empty rect (w == 0, h > 0) folds as non-empty. */
  mock_ctx_t  c2 = {}, c3 = {};
  ra_widget_t wd[2] = {make_widget(&c2, 0, 0, 1), make_widget(&c3, 0, 0, 2)};
  wd[0].rect        = (ra_ui_rect_t){.x = 0, .y = 0, .w = 100, .h = 40};
  wd[1].rect        = (ra_ui_rect_t){.x = 0, .y = 0, .w = 0, .h = 40};
  (void)ra_widget_invalidate(&wd[0], k_ra_widget_refresh_fast);
  (void)ra_widget_invalidate(&wd[1], k_ra_widget_refresh_fast);
  ra_ui_rect_t        dmg  = {};
  ra_widget_refresh_t hint = k_ra_widget_refresh_none;
  uint16_t            n    = 0U;
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_widget_damage(wd, 2U, &dmg, &hint, &n));
  TEST_ASSERT_EQ(2U, n);
  TEST_ASSERT_EQ(100, dmg.w);
  TEST_ASSERT_EQ(40, dmg.h);

  /* Precondition guards: NULL widget array / NULL widget. */
  bool                    h  = false;
  const ra_widget_event_t ev = {.kind = k_ra_widget_ev_touch};
  ra_box_t                scr[2];
  const ra_ui_rect_t      fr = {.x = 0, .y = 0, .w = 10, .h = 10};
  TEST_ASSERT_EQ((int)k_ra_err_null_ptr, (int)ra_widget_dispatch(nullptr, 1U, &ev, &h));
  TEST_ASSERT_EQ((int)k_ra_err_null_ptr, (int)ra_widget_damage(nullptr, 1U, &dmg, &hint, &n));
  TEST_ASSERT_EQ((int)k_ra_err_null_ptr, (int)ra_widget_render_dirty(nullptr, 1U));
  TEST_ASSERT_EQ((int)k_ra_err_null_ptr,
                 (int)ra_widget_invalidate(nullptr, k_ra_widget_refresh_fast));
  TEST_ASSERT_EQ((int)k_ra_err_invalid_arg,
                 (int)ra_widget_invalidate(&wd[0], k_ra_widget_refresh_none));
  TEST_ASSERT_EQ(
    (int)k_ra_err_null_ptr,
    (int)ra_widget_layout_stack(nullptr, 1U, &fr, k_ra_widget_axis_col, 0, 0, scr, 2U));
  TEST_END("ra_widget: edge guards + half-empty damage");
}

/**
 * @test Remaining MC/DC arms: null-vt skip, dirty-but-invisible damage skip,
 *       and the `count == 0` (left-false) branch of every array guard.
 *
 * @par MC/DC:
 * - dispatch eligibility `!visible || vt==NULL || on_input==NULL` (L257, 3
 *   conditions): the middle `vt == NULL` true arm -- a visible widget with a
 *   non-null on_input but a NULL `vt` is skipped (no crash, not handled). This
 *   completes the trio whose `!visible` and `on_input==NULL` arms are covered
 *   by ::test_dispatch_touch.
 * - damage selector `!visible || !dirty` (L307, 2 conditions): the `!visible`
 *   true / `!dirty` false arm -- a dirty but invisible widget is skipped, so it
 *   contributes nothing to the union or count (proving `visible` independently
 *   affects the outcome vs. the visible+dirty vector in ::test_invalidate_damage).
 * - array-precondition guard `(count > 0) && (widgets == NULL)` for dispatch
 *   (L250), damage (L299), and render_dirty (L324): the left-false arm
 *   `count == 0` short-circuits the guard so a NULL array with zero widgets is
 *   accepted and the call returns k_ra_ok (its true arm -- count>0, widgets
 *   NULL -- is covered by ::test_widget_edge_guards).
 */
static void test_widget_remaining_mcdc(void)
{
  TEST_BEGIN("ra_widget: remaining MC/DC arms");

  /* L257 middle arm: a visible widget with on_input set but vt == NULL is
   * skipped during dispatch (the `vt == NULL` condition is true). */
  mock_ctx_t  cnv                 = {.consume = true};
  ra_widget_t wnv                 = make_widget(&cnv, 0, 0, 1);
  wnv.rect                        = (ra_ui_rect_t){.x = 0, .y = 0, .w = 50, .h = 50};
  wnv.vt                          = nullptr; /* vt == NULL: middle condition true */
  bool                    handled = true;
  const ra_widget_event_t touch   = {.kind = k_ra_widget_ev_touch, .x = 10, .y = 10};
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_widget_dispatch(&wnv, 1U, &touch, &handled));
  TEST_ASSERT_EQ(false, handled);
  TEST_ASSERT_EQ(0U, cnv.input_calls);

  /* L307 `!visible` true arm: a dirty BUT invisible widget is skipped, so it
   * never folds into the damage union or the dirty count. */
  mock_ctx_t  cdi = {};
  ra_widget_t wdi = make_widget(&cdi, 0, 0, 1);
  wdi.rect        = (ra_ui_rect_t){.x = 0, .y = 0, .w = 80, .h = 30};
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_widget_invalidate(&wdi, k_ra_widget_refresh_quality));
  wdi.visible              = false; /* dirty == true, visible == false */
  ra_ui_rect_t        dmg  = {.x = 7, .y = 7, .w = 7, .h = 7};
  ra_widget_refresh_t hint = k_ra_widget_refresh_quality;
  uint16_t            n    = 99U;
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_widget_damage(&wdi, 1U, &dmg, &hint, &n));
  TEST_ASSERT_EQ(0U, n);    /* invisible -> not counted   */
  TEST_ASSERT_EQ(0, dmg.w); /* empty accumulator returned */
  TEST_ASSERT_EQ(0, dmg.h);
  TEST_ASSERT_EQ((int)k_ra_widget_refresh_none, (int)hint);

  /* L250 / L299 / L324 left-false arm: count == 0 short-circuits the guard, so
   * a NULL array with zero widgets is accepted and the call returns ok. */
  const ra_widget_event_t ev = {.kind = k_ra_widget_ev_button, .button_id = 1};
  handled                    = true;
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_widget_dispatch(nullptr, 0U, &ev, &handled));
  TEST_ASSERT_EQ(false, handled);
  dmg  = (ra_ui_rect_t){.x = 5, .y = 5, .w = 5, .h = 5};
  hint = k_ra_widget_refresh_quality;
  n    = 99U;
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_widget_damage(nullptr, 0U, &dmg, &hint, &n));
  TEST_ASSERT_EQ(0U, n);
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_widget_render_dirty(nullptr, 0U));

  TEST_END("ra_widget: remaining MC/DC arms");
}

int main(void)
{
  test_layout_stack();
  test_dispatch_touch();
  test_dispatch_button();
  test_invalidate_damage();
  test_render_dirty();
  test_widget_edge_guards();
  test_widget_remaining_mcdc();
  return 0;
}
