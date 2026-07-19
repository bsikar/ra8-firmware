/**
 * @file test_ra8_widget.c
 * @brief Unit tests for the ra8_widget composable-UI layer (#145).
 *
 * @details
 * Pure logic -- layout (via ra8_box), input routing (via ra8_ui hit-test),
 * damage union + refresh-hint folding, and the render-dispatch selection
 * (with a recording mock vtable). No framebuffer, so this runs in-process.
 * This sibling owns the core layout / dispatch / damage / panel tests; the
 * concrete leaf-widget (label / button / kit) tests live in
 * test_ra8_widget_leaf.c.
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
 * @enum t_widget_geom_t
 * @brief Rectangles and track sizes the layout arms are laid out with.
 *
 * @details
 * Sizes are picked so every computed edge is unique: a stacked layout that
 * mis-attributes a track, or a hit test that lands one child off, produces a
 * coordinate that matches no other widget in the fixture.
 */
typedef enum : int16_t {
  k_t_pane_w        = 100, /**< Width of a full-width stacked child.            */
  k_t_pane_h        = 40,  /**< Its height.                                     */
  k_t_square_side   = 50,  /**< Edge of the two side-by-side square children.    */
  k_t_short_w       = 80,  /**< Width of the damage-intersect arm's widget.      */
  k_t_short_h       = 30,  /**< Its height.                                      */
  k_t_origin_x      = 10,  /**< Non-zero origin x proving offsets are honoured;
                                also the edge of the two adjacency arms.         */
  k_t_origin_y      = 20,  /**< Non-zero origin y.                               */
  k_t_second_pane_y = 260, /**< Origin y of the second pane, past the first.     */
  k_t_track_tall    = 64,  /**< Fixed track height of the tall stack child.      */
  k_t_track_mid     = 48,  /**< Fixed track height of the middle child.          */
  k_t_track_header  = 44,  /**< Fixed track height reserved for the header.      */
  k_t_track_footer  = 28,  /**< Fixed track height reserved for the footer.      */
  k_t_damage_inside = 7,   /**< Edge of a damage rect wholly inside the widget.  */
  k_t_damage_small  = 5,   /**< Edge of the smaller damage rect.                 */
  k_t_untouched_h   = 999, /**< Sentinel height a skipped layout must not rewrite. */
} t_widget_geom_t;

/**
 * @enum t_widget_count_t
 * @brief Out-parameter seed proving the callee always writes it.
 */
typedef enum : uint16_t {
  k_t_count_poison = 99U, /**< Pre-set count; a callee that returns without
                               writing leaves this value behind. */
} t_widget_count_t;

/* --- Recording mock widget -------------------------------------------------- */

typedef struct {
  uint32_t render_calls; /**< Times render() ran.    */
  uint32_t input_calls;  /**< Times on_input() ran.  */
  bool     consume;      /**< on_input return value. */
  uint16_t last_button;  /**< Last button id seen.   */
} mock_ctx_t;

static void mock_measure(ra8_widget_t* w, int32_t aw, int32_t ah, int32_t* ow, int32_t* oh)
{
  (void)w;
  *ow = aw;
  *oh = ah;
}

static void mock_render(ra8_widget_t* w)
{
  ((mock_ctx_t*)w->ctx)->render_calls++;
}

static bool mock_on_input(ra8_widget_t* w, const ra8_widget_event_t* ev)
{
  mock_ctx_t* c = (mock_ctx_t*)w->ctx;
  c->input_calls++;
  c->last_button = ev->button_id;
  return c->consume;
}

static const ra8_widget_vtable_t k_mock_vt = {
  .measure  = mock_measure,
  .render   = mock_render,
  .on_input = mock_on_input,
};

static ra8_widget_t make_widget(mock_ctx_t* ctx, int16_t fixed, uint16_t flex, uint16_t action)
{
  ra8_widget_t w = {};
  w.vt           = &k_mock_vt;
  w.ctx          = ctx;
  w.fixed        = fixed;
  w.flex         = flex;
  w.action_id    = action;
  w.visible      = true;
  return w;
}

/**
 * @test ra8_widget_layout_stack places fixed + flex children + skips invisible.
 *
 * @par MC/DC:
 * Visibility skip `if (!widgets[i].visible)` (1 condition) -- vectors: a
 * visible child gets a rect; an invisible child is skipped (keeps its rect).
 * Plus the box_cap guard `box_cap < visible+1`.
 */
static void test_layout_stack(void)
{
  TEST_BEGIN("ra8_widget: layout_stack fixed+flex+invisible");
  mock_ctx_t   c0    = {};
  mock_ctx_t   c1    = {};
  mock_ctx_t   c2    = {};
  ra8_widget_t ws[3] = {
    make_widget(&c0, k_t_track_tall, 0, 1), /* fixed 64 high   */
    make_widget(&c1, 0, 1, 2),                       /* flex fills rest */
    make_widget(&c2, k_t_track_mid, 0, 3), /* fixed 48 high   */
  };
  ra8_box_t           scratch[8];
  const ra8_ui_rect_t frame = {.x = 0, .y = 0, .w = 100, .h = 300};
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_widget_layout_stack(ws, 3U, &frame, k_ra8_widget_axis_col, 0, 0, scratch, 8U));
  /* col: w0 = top 64, w1 = middle (300-64-48=188), w2 = bottom 48. */
  TEST_ASSERT_EQ(0, ws[0].rect.y);
  TEST_ASSERT_EQ(64, ws[0].rect.h);
  TEST_ASSERT_EQ(64, ws[1].rect.y);
  TEST_ASSERT_EQ(188, ws[1].rect.h);
  TEST_ASSERT_EQ(252, ws[2].rect.y);
  TEST_ASSERT_EQ(48, ws[2].rect.h);

  /* Hide the middle widget: the two fixed ones now bracket the frame. */
  ws[1].visible = false;
  ws[1].rect.h  = k_t_untouched_h; /* sentinel: must be left untouched */
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_widget_layout_stack(ws, 3U, &frame, k_ra8_widget_axis_col, 0, 0, scratch, 8U));
  TEST_ASSERT_EQ(64, ws[0].rect.h);
  TEST_ASSERT_EQ(999, ws[1].rect.h); /* invisible: untouched */
  TEST_ASSERT_EQ(48, ws[2].rect.h);

  /* box_cap too small (need visible(2)+1 = 3, give 2). */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_widget_layout_stack(ws, 3U, &frame, k_ra8_widget_axis_col, 0, 0, scratch, 2U));
  TEST_END("ra8_widget: layout_stack fixed+flex+invisible");
}

/**
 * @test ra8_widget_dispatch routes touch by hit + skips ineligible widgets.
 *
 * @par MC/DC:
 * Eligibility `!visible || vt==NULL || on_input==NULL` (3 conditions). N+1
 * vectors: eligible+hit -> routed; invisible -> skipped; NULL on_input ->
 * skipped; plus a touch miss -> not handled.
 */
static void test_dispatch_touch(void)
{
  TEST_BEGIN("ra8_widget: dispatch touch routing MC/DC");
  mock_ctx_t   c0    = {.consume = true};
  mock_ctx_t   c1    = {.consume = true};
  ra8_widget_t ws[2] = {make_widget(&c0, 0, 0, 1), make_widget(&c1, 0, 0, 2)};
  ws[0].rect         = (ra8_ui_rect_t){.x = 0, .y = 0, .w = k_t_square_side, .h = k_t_square_side};
  ws[1].rect = (ra8_ui_rect_t){.x = k_t_square_side, .y = 0, .w = k_t_square_side, .h = k_t_square_side};

  bool                     handled = false;
  const ra8_widget_event_t touch1  = {.kind = k_ra8_widget_ev_touch, .x = 60, .y = 10};
  /* Vector: eligible widget under the point handles it. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_widget_dispatch(ws, 2U, &touch1, &handled));
  TEST_ASSERT_EQ(true, handled);
  TEST_ASSERT_EQ(1U, c1.input_calls);
  TEST_ASSERT_EQ(0U, c0.input_calls);

  /* Vector: touch miss -> not handled. */
  const ra8_widget_event_t miss = {.kind = k_ra8_widget_ev_touch, .x = 200, .y = 200};
  handled                       = true;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_widget_dispatch(ws, 2U, &miss, &handled));
  TEST_ASSERT_EQ(false, handled);

  /* Vector: invisible widget under the point is skipped. */
  ws[1].visible  = false;
  c1.input_calls = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_widget_dispatch(ws, 2U, &touch1, &handled));
  TEST_ASSERT_EQ(false, handled);
  TEST_ASSERT_EQ(0U, c1.input_calls);

  /* Vector: NULL on_input is skipped (eligible position, no handler). */
  ws[1].visible                               = true;
  static const ra8_widget_vtable_t k_no_input = {.measure  = mock_measure,
                                                 .render   = mock_render,
                                                 .on_input = nullptr};
  ws[1].vt                                    = &k_no_input;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_widget_dispatch(ws, 2U, &touch1, &handled));
  TEST_ASSERT_EQ(false, handled);
  TEST_END("ra8_widget: dispatch touch routing MC/DC");
}

/**
 * @test ra8_widget_dispatch offers a button to each widget until one consumes.
 *
 * @par MC/DC:
 * Button path: first widget declines (returns false) -> offered to the next,
 * which consumes. Vectors: none consumes -> not handled; second consumes.
 */
static void test_dispatch_button(void)
{
  TEST_BEGIN("ra8_widget: dispatch button first-consumer");
  mock_ctx_t               c0      = {.consume = false};
  mock_ctx_t               c1      = {.consume = true};
  ra8_widget_t             ws[2]   = {make_widget(&c0, 0, 0, 1), make_widget(&c1, 0, 0, 2)};
  bool                     handled = false;
  const ra8_widget_event_t btn     = {.kind = k_ra8_widget_ev_button, .button_id = 7};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_widget_dispatch(ws, 2U, &btn, &handled));
  TEST_ASSERT_EQ(true, handled);
  TEST_ASSERT_EQ(1U, c0.input_calls); /* declined */
  TEST_ASSERT_EQ(1U, c1.input_calls); /* consumed */
  TEST_ASSERT_EQ(7U, c1.last_button);
  TEST_END("ra8_widget: dispatch button first-consumer");
}

/**
 * @test ra8_widget_invalidate + ra8_widget_damage fold dirty rects + hints.
 *
 * @par MC/DC:
 * Damage selector `!visible || !dirty` (2 conditions). Vectors: a clean tree
 * -> count 0, empty rect; one fast-dirty widget -> its rect + fast; adding a
 * quality-dirty widget -> union rect + quality (the strongest hint wins).
 */
static void test_invalidate_damage(void)
{
  TEST_BEGIN("ra8_widget: invalidate + damage union/hint");
  mock_ctx_t   c0    = {};
  mock_ctx_t   c1    = {};
  ra8_widget_t ws[2] = {make_widget(&c0, 0, 0, 1), make_widget(&c1, 0, 0, 2)};
  ws[0].rect         = (ra8_ui_rect_t){.x = 0, .y = 0, .w = k_t_pane_w, .h = k_t_pane_h};
  ws[1].rect =
    (ra8_ui_rect_t){.x = 0, .y = k_t_second_pane_y, .w = k_t_pane_w, .h = k_t_pane_h};

  ra8_ui_rect_t        rect = {};
  ra8_widget_refresh_t hint = k_ra8_widget_refresh_quality;
  uint16_t             n    = k_t_count_poison;
  /* Clean -> nothing dirty. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_widget_damage(ws, 2U, &rect, &hint, &n));
  TEST_ASSERT_EQ(0U, n);
  TEST_ASSERT_EQ(0, rect.w);
  TEST_ASSERT_EQ(k_ra8_widget_refresh_none, hint);

  /* One fast-dirty widget -> its own rect, fast hint. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_widget_invalidate(&ws[0], k_ra8_widget_refresh_fast));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_widget_damage(ws, 2U, &rect, &hint, &n));
  TEST_ASSERT_EQ(1U, n);
  TEST_ASSERT_EQ(40, rect.h);
  TEST_ASSERT_EQ(k_ra8_widget_refresh_fast, hint);

  /* Add a quality-dirty widget -> union spans both, quality wins. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_widget_invalidate(&ws[1], k_ra8_widget_refresh_quality));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_widget_damage(ws, 2U, &rect, &hint, &n));
  TEST_ASSERT_EQ(2U, n);
  TEST_ASSERT_EQ(0, rect.y);
  TEST_ASSERT_EQ(300, rect.h); /* 0..300 spans both */
  TEST_ASSERT_EQ(k_ra8_widget_refresh_quality, hint);
  TEST_END("ra8_widget: invalidate + damage union/hint");
}

/**
 * @test ra8_widget_render_dirty renders only visible+dirty + clears damage.
 *
 * @par MC/DC:
 * Selector `!visible || !dirty` (2 conditions). Vectors: a visible+dirty
 * widget renders + clears; a clean widget does not; an invisible dirty widget
 * does not.
 */
static void test_render_dirty(void)
{
  TEST_BEGIN("ra8_widget: render_dirty selection");
  mock_ctx_t   c0    = {};
  mock_ctx_t   c1    = {};
  mock_ctx_t   c2    = {};
  ra8_widget_t ws[3] = {make_widget(&c0, 0, 0, 1),
                        make_widget(&c1, 0, 0, 2),
                        make_widget(&c2, 0, 0, 3)};
  (void)ra8_widget_invalidate(&ws[0], k_ra8_widget_refresh_fast); /* visible + dirty */
  /* ws[1] stays clean */
  (void)ra8_widget_invalidate(&ws[2], k_ra8_widget_refresh_quality); /* dirty but invisible */
  ws[2].visible = false;

  TEST_ASSERT_EQ(k_ra8_ok, ra8_widget_render_dirty(ws, 3U));
  TEST_ASSERT_EQ(1U, c0.render_calls); /* rendered  */
  TEST_ASSERT_EQ(0U, c1.render_calls); /* clean     */
  TEST_ASSERT_EQ(0U, c2.render_calls); /* invisible */
  TEST_ASSERT_EQ(false, ws[0].dirty);  /* cleared   */
  TEST_ASSERT_EQ(k_ra8_widget_refresh_none, ws[0].refresh);
  TEST_END("ra8_widget: render_dirty selection");
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
  TEST_BEGIN("ra8_widget: edge guards + half-empty damage");
  static const ra8_widget_vtable_t k_no_render = {.measure  = mock_measure,
                                                  .render   = nullptr,
                                                  .on_input = mock_on_input};

  /* H-2: null-render widget + null-vt widget are skipped but cleared. */
  mock_ctx_t   c0    = {};
  mock_ctx_t   c1    = {};
  ra8_widget_t ws[2] = {make_widget(&c0, 0, 0, 1), make_widget(&c1, 0, 0, 2)};
  ws[0].vt           = &k_no_render; /* render == null */
  ws[1].vt           = nullptr;      /* vt == null     */
  (void)ra8_widget_invalidate(&ws[0], k_ra8_widget_refresh_fast);
  (void)ra8_widget_invalidate(&ws[1], k_ra8_widget_refresh_fast);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_widget_render_dirty(ws, 2U));
  TEST_ASSERT_EQ(0U, c0.render_calls);
  TEST_ASSERT_EQ(false, ws[0].dirty);
  TEST_ASSERT_EQ(false, ws[1].dirty);

  /* H-1: a half-empty rect (w == 0, h > 0) folds as non-empty. */
  mock_ctx_t   c2    = {};
  mock_ctx_t   c3    = {};
  ra8_widget_t wd[2] = {make_widget(&c2, 0, 0, 1), make_widget(&c3, 0, 0, 2)};
  wd[0].rect         = (ra8_ui_rect_t){.x = 0, .y = 0, .w = k_t_pane_w, .h = k_t_pane_h};
  wd[1].rect         = (ra8_ui_rect_t){.x = 0, .y = 0, .w = 0, .h = k_t_pane_h};
  (void)ra8_widget_invalidate(&wd[0], k_ra8_widget_refresh_fast);
  (void)ra8_widget_invalidate(&wd[1], k_ra8_widget_refresh_fast);
  ra8_ui_rect_t        dmg  = {};
  ra8_widget_refresh_t hint = k_ra8_widget_refresh_none;
  uint16_t             n    = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_widget_damage(wd, 2U, &dmg, &hint, &n));
  TEST_ASSERT_EQ(2U, n);
  TEST_ASSERT_EQ(100, dmg.w);
  TEST_ASSERT_EQ(40, dmg.h);

  /* Precondition guards: NULL widget array / NULL widget. */
  bool                     h  = false;
  const ra8_widget_event_t ev = {.kind = k_ra8_widget_ev_touch};
  ra8_box_t                scr[2];
  const ra8_ui_rect_t      fr = {.x = 0, .y = 0, .w = 10, .h = 10};
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_widget_dispatch(nullptr, 1U, &ev, &h));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_widget_damage(nullptr, 1U, &dmg, &hint, &n));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_widget_render_dirty(nullptr, 1U));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_widget_invalidate(nullptr, k_ra8_widget_refresh_fast));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_widget_invalidate(&wd[0], k_ra8_widget_refresh_none));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_widget_layout_stack(nullptr, 1U, &fr, k_ra8_widget_axis_col, 0, 0, scr, 2U));
  TEST_END("ra8_widget: edge guards + half-empty damage");
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
 *   accepted and the call returns k_ra8_ok (its true arm -- count>0, widgets
 *   NULL -- is covered by ::test_widget_edge_guards).
 */
static void test_widget_remaining_mcdc(void)
{
  TEST_BEGIN("ra8_widget: remaining MC/DC arms");

  /* L257 middle arm: a visible widget with on_input set but vt == NULL is
   * skipped during dispatch (the `vt == NULL` condition is true). */
  mock_ctx_t   cnv = {.consume = true};
  ra8_widget_t wnv = make_widget(&cnv, 0, 0, 1);
  wnv.rect         = (ra8_ui_rect_t){.x = 0, .y = 0, .w = k_t_square_side, .h = k_t_square_side};
  wnv.vt           = nullptr; /* vt == NULL: middle condition true */
  bool                     handled = true;
  const ra8_widget_event_t touch   = {.kind = k_ra8_widget_ev_touch, .x = 10, .y = 10};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_widget_dispatch(&wnv, 1U, &touch, &handled));
  TEST_ASSERT_EQ(false, handled);
  TEST_ASSERT_EQ(0U, cnv.input_calls);

  /* L307 `!visible` true arm: a dirty BUT invisible widget is skipped, so it
   * never folds into the damage union or the dirty count. */
  mock_ctx_t   cdi = {};
  ra8_widget_t wdi = make_widget(&cdi, 0, 0, 1);
  wdi.rect         = (ra8_ui_rect_t){.x = 0, .y = 0, .w = k_t_short_w, .h = k_t_short_h};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_widget_invalidate(&wdi, k_ra8_widget_refresh_quality));
  wdi.visible       = false; /* dirty == true, visible == false */
  ra8_ui_rect_t dmg = {.x = k_t_damage_inside, .y = k_t_damage_inside, .w = k_t_damage_inside, .h = k_t_damage_inside};
  ra8_widget_refresh_t hint = k_ra8_widget_refresh_quality;
  uint16_t             n    = k_t_count_poison;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_widget_damage(&wdi, 1U, &dmg, &hint, &n));
  TEST_ASSERT_EQ(0U, n);    /* invisible -> not counted   */
  TEST_ASSERT_EQ(0, dmg.w); /* empty accumulator returned */
  TEST_ASSERT_EQ(0, dmg.h);
  TEST_ASSERT_EQ(k_ra8_widget_refresh_none, hint);

  /* L250 / L299 / L324 left-false arm: count == 0 short-circuits the guard, so
   * a NULL array with zero widgets is accepted and the call returns ok. */
  const ra8_widget_event_t ev = {.kind = k_ra8_widget_ev_button, .button_id = 1};
  handled                     = true;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_widget_dispatch(nullptr, 0U, &ev, &handled));
  TEST_ASSERT_EQ(false, handled);
  dmg = (ra8_ui_rect_t){.x = k_t_damage_small, .y = k_t_damage_small, .w = k_t_damage_small, .h = k_t_damage_small};
  hint = k_ra8_widget_refresh_quality;
  n    = k_t_count_poison;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_widget_damage(nullptr, 0U, &dmg, &hint, &n));
  TEST_ASSERT_EQ(0U, n);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_widget_render_dirty(nullptr, 0U));

  TEST_END("ra8_widget: remaining MC/DC arms");
}

/* --- Container-panel (ra8_widget_panel) fixture ------------------------------ */

/**
 * @struct compose_fixture_t
 * @brief A nested widget tree built in one stack object for the panel tests.
 *
 * @details
 * `panelw` is a ::ra8_widget_t bound (via ::ra8_widget_panel_init) to `root_panel`,
 * whose children are `root[0]` (status leaf, fixed), `root[1]` (the `body_panel`
 * nested panel) and `root[2]` (footer leaf, fixed). `body_panel`'s children are
 * the two flex tile leaves. The leaves use the recording mock vtable so a render
 * or input call is observable. The panel descriptors point into this object's
 * own arrays, so it must not be copied.
 */
typedef struct {
  ra8_widget_t       root[3];    /**< [status, body-panel, footer]. */
  ra8_widget_t       body[2];    /**< [left tile, right tile].      */
  ra8_box_t          rscr[4];    /**< Root layout scratch.          */
  ra8_box_t          bscr[3];    /**< Body layout scratch.          */
  ra8_widget_panel_t body_panel; /**< Nested row panel.             */
  ra8_widget_panel_t root_panel; /**< Root column panel.            */
  ra8_widget_t       panelw;     /**< Top panel widget.             */
  mock_ctx_t         cs;         /**< Status leaf ctx.              */
  mock_ctx_t         cl;         /**< Left tile ctx.                */
  mock_ctx_t         cr;         /**< Right tile ctx.               */
  mock_ctx_t         cf;         /**< Footer leaf ctx.              */
} compose_fixture_t;

/** @brief Build the nested tree in @p f; return false on a panel-bind error. */
static bool build_compose_fixture(compose_fixture_t* f)
{
  *f         = (compose_fixture_t){};
  f->root[0] = make_widget(&f->cs, k_t_track_header, 0, 0);
  f->root[2] = make_widget(&f->cf, k_t_track_footer, 0, 0);
  f->body[0] = make_widget(&f->cl, 0, 1, 0);
  f->body[1] = make_widget(&f->cr, 0, 1, 0);

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
 * @test ra8_widget_panel_init binds the vtable + rejects bad descriptors.
 *
 * @par MC/DC:
 * - NULL guards `w == NULL` / `panel == NULL` (each independently -> null_ptr).
 * - `count > 0` guard with `children == NULL` (true arm -> invalid_arg) and a
 *   too-small scratch `box_cap < count + 1` (true arm -> invalid_arg); the
 *   `count == 0` false arm + a valid descriptor bind successfully.
 */
static void test_panel_init_guards(void)
{
  TEST_BEGIN("ra8_widget_panel: init guards");
  ra8_widget_t       w = {};
  ra8_widget_t       kids[1];
  ra8_box_t          scr[2];
  ra8_widget_panel_t ok = {.children    = kids,
                           .box_scratch = scr,
                           .count       = 1U,
                           .box_cap     = 2U,
                           .axis        = k_ra8_widget_axis_col};

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_widget_panel_init(nullptr, &ok));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_widget_panel_init(&w, nullptr));

  ra8_widget_panel_t no_kids = {.children    = nullptr,
                                .box_scratch = scr,
                                .count       = 1U,
                                .box_cap     = 2U};
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_widget_panel_init(&w, &no_kids));

  ra8_widget_panel_t small = {.children = kids, .box_scratch = scr, .count = 3U, .box_cap = 3U};
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_widget_panel_init(&w, &small));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_widget_panel_init(&w, &ok));
  TEST_ASSERT_EQ(true, w.vt == ra8_widget_panel_vtable());
  TEST_ASSERT_EQ(true, w.ctx == (void*)&ok);
  TEST_ASSERT_EQ(true, w.visible);
  TEST_END("ra8_widget_panel: init guards");
}

/**
 * @test ra8_widget_panel_compose lays out + composites the whole nested tree.
 *
 * @par MC/DC:
 * Full-tree compose: all 3 root children dirty -> damage covers the frame with
 * the quality hint, the nested `body` panel composites BOTH tiles (the
 * `child.visible` true arm of internal_panel_render's loop, proven by the tile
 * render counters), and every leaf renders exactly once.
 */
static void test_panel_compose_full(void)
{
  TEST_BEGIN("ra8_widget_panel: compose full tree");
  compose_fixture_t f;
  TEST_ASSERT_EQ(true, build_compose_fixture(&f));
  (void)ra8_widget_invalidate(&f.root[0], k_ra8_widget_refresh_quality);
  (void)ra8_widget_invalidate(&f.root[1], k_ra8_widget_refresh_quality);
  (void)ra8_widget_invalidate(&f.root[2], k_ra8_widget_refresh_quality);

  ra8_ui_rect_t        dmg   = {};
  ra8_widget_refresh_t hint  = k_ra8_widget_refresh_none;
  uint16_t             n     = 0U;
  const ra8_ui_rect_t  frame = {.x = 0, .y = 0, .w = 100, .h = 300};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_widget_panel_compose(&f.panelw, &frame, &dmg, &hint, &n));
  TEST_ASSERT_EQ(3U, n);
  TEST_ASSERT_EQ(100, dmg.w);
  TEST_ASSERT_EQ(300, dmg.h);
  TEST_ASSERT_EQ(k_ra8_widget_refresh_quality, hint);
  TEST_ASSERT_EQ(1U, f.cs.render_calls); /* status   */
  TEST_ASSERT_EQ(1U, f.cl.render_calls); /* nested L */
  TEST_ASSERT_EQ(1U, f.cr.render_calls); /* nested R */
  TEST_ASSERT_EQ(1U, f.cf.render_calls); /* footer   */
  TEST_END("ra8_widget_panel: compose full tree");
}

/**
 * @test A status-only invalidate flushes only the status rect, fast.
 *
 * @par MC/DC:
 * Damage selection after only the status leaf is re-invalidated: exactly 1
 * dirty child, the damage rect is the 100x44 status band with the fast hint,
 * and the nested body panel is NOT re-rendered (its tiles' render counters stay
 * at zero -- the `child.dirty` false arm of render_dirty for the body panel).
 * This is the issue #145 partial-flush acceptance.
 */
static void test_panel_compose_partial(void)
{
  TEST_BEGIN("ra8_widget_panel: status-only partial flush");
  compose_fixture_t f;
  TEST_ASSERT_EQ(true, build_compose_fixture(&f));
  ra8_ui_rect_t        dmg   = {};
  ra8_widget_refresh_t hint  = k_ra8_widget_refresh_none;
  uint16_t             n     = 0U;
  const ra8_ui_rect_t  frame = {.x = 0, .y = 0, .w = 100, .h = 300};
  (void)ra8_widget_invalidate(&f.root[0], k_ra8_widget_refresh_quality);
  (void)ra8_widget_invalidate(&f.root[1], k_ra8_widget_refresh_quality);
  (void)ra8_widget_invalidate(&f.root[2], k_ra8_widget_refresh_quality);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_widget_panel_compose(&f.panelw, &frame, &dmg, &hint, &n));
  f.cs.render_calls = 0U;
  f.cl.render_calls = 0U;
  f.cr.render_calls = 0U;
  f.cf.render_calls = 0U;

  (void)ra8_widget_invalidate(&f.root[0], k_ra8_widget_refresh_fast); /* status only */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_widget_panel_compose(&f.panelw, &frame, &dmg, &hint, &n));
  TEST_ASSERT_EQ(1U, n);
  TEST_ASSERT_EQ(100, dmg.w);
  TEST_ASSERT_EQ(44, dmg.h);
  TEST_ASSERT_EQ(0, dmg.y);
  TEST_ASSERT_EQ(k_ra8_widget_refresh_fast, hint);
  TEST_ASSERT_EQ(1U, f.cs.render_calls); /* status redrawn */
  TEST_ASSERT_EQ(0U, f.cl.render_calls); /* body untouched */
  TEST_ASSERT_EQ(0U, f.cr.render_calls);
  TEST_ASSERT_EQ(0U, f.cf.render_calls);
  TEST_END("ra8_widget_panel: status-only partial flush");
}

/**
 * @test The panel vtable routes a touch down through a nested panel to a leaf.
 *
 * @par MC/DC:
 * A touch in the body region is routed by the root panel's on_input to the body
 * panel (hit-test true arm), whose on_input routes it to the left tile, which
 * consumes it -- so `handled` is true and only the left tile's on_input ran
 * (the `child.rect contains point` arm at each level).
 */
static void test_panel_input_route(void)
{
  TEST_BEGIN("ra8_widget_panel: nested touch routing");
  compose_fixture_t f;
  TEST_ASSERT_EQ(true, build_compose_fixture(&f));
  f.cl.consume               = true; /* the left tile will consume a touch */
  ra8_ui_rect_t        dmg   = {};
  ra8_widget_refresh_t hint  = k_ra8_widget_refresh_none;
  uint16_t             n     = 0U;
  const ra8_ui_rect_t  frame = {.x = 0, .y = 0, .w = 100, .h = 300};
  (void)ra8_widget_invalidate(&f.root[1], k_ra8_widget_refresh_quality);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_widget_panel_compose(&f.panelw, &frame, &dmg, &hint, &n));

  /* body spans y in [44, 272); the left tile is its left half (x in [0,50)). */
  const ra8_widget_event_t touch   = {.kind = k_ra8_widget_ev_touch, .x = 10, .y = 100};
  const bool               handled = f.panelw.vt->on_input(&f.panelw, &touch);
  TEST_ASSERT_EQ(true, handled);
  TEST_ASSERT_EQ(1U, f.cl.input_calls);
  TEST_ASSERT_EQ(0U, f.cr.input_calls);
  TEST_END("ra8_widget_panel: nested touch routing");
}

/**
 * @test ra8_widget_panel_compose rejects NULL args + non-panel widgets.
 *
 * @par MC/DC:
 * - The five NULL-pointer guards each independently return null_ptr.
 * - The `(p == NULL) || (p->children == NULL)` panel check: a widget whose ctx
 *   is NULL (left true) and a widget bound to a panel with a NULL child array
 *   (left false, right true) both return invalid_arg.
 */
static void test_panel_compose_guards(void)
{
  TEST_BEGIN("ra8_widget_panel: compose guards");
  ra8_widget_t         w     = {}; /* ctx == NULL -> not a panel */
  ra8_ui_rect_t        dmg   = {};
  ra8_widget_refresh_t hint  = k_ra8_widget_refresh_none;
  uint16_t             n     = 0U;
  const ra8_ui_rect_t  frame = {.x = 0, .y = 0, .w = 10, .h = 10};

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_widget_panel_compose(nullptr, &frame, &dmg, &hint, &n));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_widget_panel_compose(&w, nullptr, &dmg, &hint, &n));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_widget_panel_compose(&w, &frame, nullptr, &hint, &n));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_widget_panel_compose(&w, &frame, &dmg, nullptr, &n));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_widget_panel_compose(&w, &frame, &dmg, &hint, nullptr));
  /* ctx == NULL: the (p == NULL) arm. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_widget_panel_compose(&w, &frame, &dmg, &hint, &n));
  /* bound to a panel whose child array is NULL: the (p->children == NULL) arm. */
  ra8_widget_panel_t empty = {.children = nullptr, .box_scratch = nullptr, .count = 0U};
  w.ctx                    = &empty;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_widget_panel_compose(&w, &frame, &dmg, &hint, &n));
  TEST_END("ra8_widget_panel: compose guards");
}
/**
 * @test ra8_widget_damage folds a fully-empty dirty rect as the union identity.
 *
 * @par MC/DC:
 * `internal_rect_union`'s second single-condition guard `if
 * (internal_rect_empty(&r))` (reached through ::ra8_widget_damage), true arm:
 * with a non-empty accumulator already built from the first dirty widget,
 * folding a second dirty widget whose rect covers no pixels (`w <= 0 && h <= 0`,
 * so both conditions of `internal_rect_empty` are true) returns the accumulator
 * unchanged. The false arm (a non-empty second rect taking the bounding-union
 * path) is covered by ::test_invalidate_damage and ::test_widget_edge_guards;
 * the first guard's `internal_rect_empty(&acc)` true arm (an empty accumulator
 * returns the first rect) by every single-dirty-widget vector. The empty rect is
 * still visible + dirty, so it is counted even though it grows the union by
 * nothing.
 */
static void test_widget_damage_empty_union(void)
{
  TEST_BEGIN("ra8_widget: damage folds a fully-empty dirty rect");
  mock_ctx_t   c0    = {};
  mock_ctx_t   c1    = {};
  ra8_widget_t ws[2] = {make_widget(&c0, 0, 0, 1), make_widget(&c1, 0, 0, 2)};
  ws[0].rect         = (ra8_ui_rect_t){.x = k_t_origin_x,
                                       .y = k_t_origin_y,
                                       .w = k_t_pane_w,
                                       .h = k_t_pane_h};
  ws[1].rect         = (ra8_ui_rect_t){.x = 0, .y = 0, .w = 0, .h = 0}; /* covers no pixels */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_widget_invalidate(&ws[0], k_ra8_widget_refresh_fast));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_widget_invalidate(&ws[1], k_ra8_widget_refresh_fast));

  ra8_ui_rect_t        rect = {};
  ra8_widget_refresh_t hint = k_ra8_widget_refresh_none;
  uint16_t             n    = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_widget_damage(ws, 2U, &rect, &hint, &n));
  TEST_ASSERT_EQ(2U, n);      /* both dirty; the empty one still counts */
  TEST_ASSERT_EQ(10, rect.x); /* union == the first (non-empty) rect    */
  TEST_ASSERT_EQ(20, rect.y);
  TEST_ASSERT_EQ(100, rect.w);
  TEST_ASSERT_EQ(40, rect.h);
  TEST_END("ra8_widget: damage folds a fully-empty dirty rect");
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
static void test_panel_render_route_guards(void)
{
  TEST_BEGIN("ra8_widget_panel: render + route non-panel guards");
  const ra8_widget_event_t ev = {.kind = k_ra8_widget_ev_touch, .x = 1, .y = 1};

  /* Vector A: ctx == NULL (p == NULL). */
  ra8_widget_t wa = {};
  wa.vt           = ra8_widget_panel_vtable();
  wa.ctx          = nullptr;
  wa.rect         = (ra8_ui_rect_t){.x = 0, .y = 0, .w = k_t_origin_x, .h = k_t_origin_x};
  wa.vt->render(&wa); /* no crash */
  TEST_ASSERT_EQ(false, wa.vt->on_input(&wa, &ev));

  /* Vector B: p != NULL but children == NULL. */
  ra8_widget_panel_t no_kids = {.children = nullptr, .box_scratch = nullptr, .count = 0U};
  ra8_widget_t       wb      = {};
  wb.vt                      = ra8_widget_panel_vtable();
  wb.ctx                     = &no_kids;
  wb.rect = (ra8_ui_rect_t){.x = 0, .y = 0, .w = k_t_origin_x, .h = k_t_origin_x};
  wb.vt->render(&wb); /* no crash */
  TEST_ASSERT_EQ(false, wb.vt->on_input(&wb, &ev));
  TEST_END("ra8_widget_panel: render + route non-panel guards");
}

/**
 * @test A panel with an undersized scratch fails layout on compose and render.
 *
 * @par MC/DC:
 * The layout-result guard `if (... != k_ra8_ok)` (a single condition) on its true
 * arm, reached two ways for the SAME undersized panel (two visible children but
 * `box_cap == 1 < count + 1`):
 * - ::ra8_widget_panel_compose forwards the ::ra8_widget_layout_stack failure (its
 *   `lerr != k_ra8_ok` true arm -> k_ra8_err_invalid_arg, before the damage /
 *   render steps run).
 * - ::internal_panel_render (the panel vtable render) bails on the same failure
 *   (its `!= k_ra8_ok` true arm), leaving both children unrendered.
 * The false arm (a sufficiently sized panel laying out cleanly) is covered by
 * ::test_panel_compose_full. The widget is hand-bound to the panel vtable to
 * reach the layout step, since ::ra8_widget_panel_init rejects the small scratch.
 */
static void test_panel_layout_fail(void)
{
  TEST_BEGIN("ra8_widget_panel: undersized scratch fails layout");
  mock_ctx_t         c0      = {};
  mock_ctx_t         c1      = {};
  ra8_widget_t       kids[2] = {make_widget(&c0, 0, 1, 1), make_widget(&c1, 0, 1, 2)};
  ra8_box_t          scr[1]; /* too small: layout needs count(2) + 1 = 3 nodes */
  ra8_widget_panel_t small = {.children    = kids,
                              .box_scratch = scr,
                              .count       = 2U,
                              .box_cap     = 1U,
                              .axis        = k_ra8_widget_axis_row};
  ra8_widget_t       w     = {};
  w.vt                     = ra8_widget_panel_vtable(); /* hand-bound: init would reject box_cap */
  w.ctx                    = &small;
  w.rect = (ra8_ui_rect_t){.x = 0, .y = 0, .w = k_t_pane_w, .h = k_t_square_side};

  /* compose forwards the layout failure (damage / render never run). */
  ra8_ui_rect_t        dmg   = {};
  ra8_widget_refresh_t hint  = k_ra8_widget_refresh_none;
  uint16_t             n     = 0U;
  const ra8_ui_rect_t  frame = {.x = 0, .y = 0, .w = 100, .h = 50};
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_widget_panel_compose(&w, &frame, &dmg, &hint, &n));

  /* the render callback bails on the same failure: both children stay unrendered
   * even though they are dirty (so the bail is observable, not just a no-op). */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_widget_invalidate(&kids[0], k_ra8_widget_refresh_fast));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_widget_invalidate(&kids[1], k_ra8_widget_refresh_fast));
  w.vt->render(&w);
  TEST_ASSERT_EQ(0U, c0.render_calls);
  TEST_ASSERT_EQ(0U, c1.render_calls);
  TEST_END("ra8_widget_panel: undersized scratch fails layout");
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
  test_panel_init_guards();
  test_panel_compose_full();
  test_panel_compose_partial();
  test_panel_input_route();
  test_panel_compose_guards();
  test_widget_damage_empty_union();
  test_panel_render_route_guards();
  test_panel_layout_fail();
  return 0;
}
