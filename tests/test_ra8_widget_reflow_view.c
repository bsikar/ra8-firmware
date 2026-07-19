/**
 * @file test_ra8_widget_reflow_view.c
 * @brief Unit tests for the ra8_widget reflow-view leaf (#145 Phase 2).
 *
 * @details
 * The reflow view owns the reading-body paging state (current page, page count,
 * margins) and routes taps to a link-follow or a page turn, driving the reflow
 * engine through the injected ::ra8_widget_reflow_ops_t seam. These tests bind a
 * recording mock seam (no `ra8_reflow`, no framebuffer), so the page-turn
 * clamping and link routing are asserted in-process with full MC/DC vectors.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_widget.h"
#include "ra8_widget_reflow_view.h"
#include "unity_minimal.h"

/**
 * @enum widget_reflow_view_uint8_const_t
 * @brief Named uint8_t constants used by this file.
 *
 * @details
 * Every literal this translation unit needs, named so the
 * value's role is visible at the point of use (CLAUDE.md
 * "No Magic Numbers").
 */
typedef enum : uint8_t {
  k_widget_reflow_view_link_dest_5   = 5,
  k_widget_reflow_view_link_dest_99  = 99,
  k_widget_reflow_view_margin_x_24   = 24,
  k_widget_reflow_view_page_count_12 = 12,
  k_widget_reflow_view_w_200         = 200,
} widget_reflow_view_uint8_const_t;

/**
 * @enum widget_reflow_view_uint16_const_t
 * @brief Named uint16_t constants used by this file.
 *
 * @details
 * Every literal this translation unit needs, named so the
 * value's role is visible at the point of use (CLAUDE.md
 * "No Magic Numbers").
 */
typedef enum : uint16_t {
  k_widget_reflow_view_h_300 = 300,
} widget_reflow_view_uint16_const_t;

/**
 * @enum widget_reflow_view_uint32_const_t
 * @brief Named uint32_t constants used by this file.
 *
 * @details
 * Every literal this translation unit needs, named so the
 * value's role is visible at the point of use (CLAUDE.md
 * "No Magic Numbers").
 */
typedef enum : uint32_t {
  k_widget_reflow_view_bg_00ffffff = 0x00FFFFFFU,
} widget_reflow_view_uint32_const_t;

/* --- Recording mock reflow seam --------------------------------------------- */

/**
 * @struct mock_rv_t
 * @brief A recording ::ra8_widget_reflow_ops_t backend for the reflow-view tests.
 */
typedef struct {
  uint32_t      render_calls; /**< Times render_page() ran.         */
  uint32_t      link_calls;   /**< Times follow_link() ran.         */
  uint16_t      last_page;    /**< Last page passed to render_page. */
  ra8_ui_rect_t last_body;    /**< Last body rect painted.          */
  bool          link_return;  /**< follow_link return value.        */
  uint16_t      link_dest;    /**< follow_link destination page.    */
} mock_rv_t;

static void mock_rv_render(void* user, uint16_t page, const ra8_ui_rect_t* body)
{
  mock_rv_t* m = (mock_rv_t*)user;
  m->render_calls++;
  m->last_page = page;
  m->last_body = *body;
}

static bool mock_rv_follow(void* user, uint16_t page, int32_t x, int32_t y, uint16_t* out_page)
{
  mock_rv_t* m = (mock_rv_t*)user;
  (void)page;
  (void)x;
  (void)y;
  m->link_calls++;
  *out_page = m->link_dest;
  return m->link_return;
}

/** @brief A recording paint backend whose only role is to count bg clears. */
static uint32_t s_rv_fill_calls = 0U;
static void     rv_fill(void* u, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t c)
{
  (void)u;
  (void)x;
  (void)y;
  (void)w;
  (void)h;
  (void)c;
  s_rv_fill_calls++;
}

/**
 * @test ra8_widget_reflow_view render clears the body + paints the page.
 *
 * @par MC/DC:
 * `internal_rv_render`: `paint != NULL` true arm (bg cleared) and, in the guards
 * test, its false arm (no clear); `ops == NULL || render_page == NULL` false arm
 * (page painted). The body rect equals the widget rect inset by the margins.
 */
static void test_reflow_view_render(void)
{
  TEST_BEGIN("ra8_widget_reflow_view: clear + paint page");
  s_rv_fill_calls                = 0U;
  mock_rv_t                m     = {};
  ra8_widget_paint_t       paint = {.user = nullptr, .fill_rect = rv_fill};
  ra8_widget_reflow_ops_t  ops   = {.user        = &m,
                                    .render_page = mock_rv_render,
                                    .follow_link = mock_rv_follow};
  ra8_widget_reflow_view_t rv    = {.paint      = &paint,
                                    .ops        = &ops,
                                    .bg         = k_widget_reflow_view_bg_00ffffff,
                                    .page       = 2,
                                    .page_count = k_widget_reflow_view_page_count_12,
                                    .margin_x   = k_widget_reflow_view_margin_x_24,
                                    .margin_y   = 8};
  ra8_widget_t             w     = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_widget_reflow_view_init(&w, &rv));
  w.rect = (ra8_ui_rect_t){.x = 0,
                           .y = 0,
                           .w = k_widget_reflow_view_w_200,
                           .h = k_widget_reflow_view_h_300};

  w.vt->render(&w);
  TEST_ASSERT_EQ(1U, s_rv_fill_calls);
  TEST_ASSERT_EQ(1U, m.render_calls);
  TEST_ASSERT_EQ(2U, m.last_page);
  /* body = rect inset by margins: {24, 8, 200-48, 300-16}. */
  TEST_ASSERT_EQ(24, m.last_body.x);
  TEST_ASSERT_EQ(8, m.last_body.y);
  TEST_ASSERT_EQ(152, m.last_body.w);
  TEST_ASSERT_EQ(284, m.last_body.h);
  TEST_END("ra8_widget_reflow_view: clear + paint page");
}

/**
 * @test ra8_widget_reflow_view turns pages by tap side, clamped at both ends.
 *
 * @par MC/DC:
 * `internal_rv_turn`: `px < mid` true (left tap -> back) and false (right tap ->
 * forward); `page == first` true (back at page 0 -> no change) and false (back
 * from page 1 -> page 0); `page + 1 >= page_count` true (forward at last page ->
 * no change) and false (forward -> next page).
 */
static void test_reflow_view_page_turn(void)
{
  TEST_BEGIN("ra8_widget_reflow_view: page-turn clamping");
  mock_rv_t                m   = {.link_return = false};
  ra8_widget_reflow_ops_t  ops = {.user        = &m,
                                  .render_page = mock_rv_render,
                                  .follow_link = mock_rv_follow};
  ra8_widget_reflow_view_t rv  = {.ops = &ops, .page = 1, .page_count = 3};
  ra8_widget_t             w   = {};
  (void)ra8_widget_reflow_view_init(&w, &rv);
  w.rect = (ra8_ui_rect_t){.x = 0,
                           .y = 0,
                           .w = k_widget_reflow_view_w_200,
                           .h = k_widget_reflow_view_h_300}; /* mid x = 100 */

  /* right tap (x=150) -> forward: page 1 -> 2. */
  const ra8_widget_event_t fwd = {.kind = k_ra8_widget_ev_touch, .x = 150, .y = 50};
  TEST_ASSERT_EQ(true, w.vt->on_input(&w, &fwd));
  TEST_ASSERT_EQ(2U, rv.page);
  TEST_ASSERT_EQ(k_ra8_widget_refresh_quality, w.refresh);

  /* forward at the last page (2 of 3) -> no change, still consumed. */
  w.refresh = k_ra8_widget_refresh_none;
  TEST_ASSERT_EQ(true, w.vt->on_input(&w, &fwd));
  TEST_ASSERT_EQ(2U, rv.page);
  TEST_ASSERT_EQ(k_ra8_widget_refresh_none, w.refresh); /* unchanged -> not re-invalidated */

  /* left tap (x=50) -> back: page 2 -> 1 -> 0. */
  const ra8_widget_event_t back = {.kind = k_ra8_widget_ev_touch, .x = 50, .y = 50};
  TEST_ASSERT_EQ(true, w.vt->on_input(&w, &back));
  TEST_ASSERT_EQ(1U, rv.page);
  TEST_ASSERT_EQ(true, w.vt->on_input(&w, &back));
  TEST_ASSERT_EQ(0U, rv.page);

  /* back at page 0 -> no change, still consumed. */
  w.refresh = k_ra8_widget_refresh_none;
  TEST_ASSERT_EQ(true, w.vt->on_input(&w, &back));
  TEST_ASSERT_EQ(0U, rv.page);
  TEST_ASSERT_EQ(k_ra8_widget_refresh_none, w.refresh);
  TEST_END("ra8_widget_reflow_view: page-turn clamping");
}

/**
 * @test ra8_widget_reflow_view follows a link before turning the page.
 *
 * @par MC/DC:
 * `internal_rv_link`: `ops == NULL || follow_link == NULL` false arm (seam
 * present); `follow_link()` true arm (a link jump adopts the page) and false arm
 * (no link -> falls through to a page turn). `internal_rv_clamp`: `page >= count`
 * true (an out-of-range destination clamps to the last page) and `count == 0`
 * true (an empty book clamps to 0).
 */
static void test_reflow_view_link(void)
{
  TEST_BEGIN("ra8_widget_reflow_view: link follow + clamp");
  mock_rv_t                m = {.link_return = true, .link_dest = k_widget_reflow_view_link_dest_5};
  ra8_widget_reflow_ops_t  ops = {.user        = &m,
                                  .render_page = mock_rv_render,
                                  .follow_link = mock_rv_follow};
  ra8_widget_reflow_view_t rv  = {.ops        = &ops,
                                  .page       = 0,
                                  .page_count = k_widget_reflow_view_page_count_12};
  ra8_widget_t             w   = {};
  (void)ra8_widget_reflow_view_init(&w, &rv);
  w.rect = (ra8_ui_rect_t){.x = 0,
                           .y = 0,
                           .w = k_widget_reflow_view_w_200,
                           .h = k_widget_reflow_view_h_300};

  /* link followed -> page adopts dest 5. */
  const ra8_widget_event_t tap = {.kind = k_ra8_widget_ev_touch, .x = 100, .y = 50};
  TEST_ASSERT_EQ(true, w.vt->on_input(&w, &tap));
  TEST_ASSERT_EQ(5U, rv.page);
  TEST_ASSERT_EQ(1U, m.link_calls);

  /* out-of-range destination clamps to the last page (11 of 12). */
  m.link_dest = k_widget_reflow_view_link_dest_99;
  TEST_ASSERT_EQ(true, w.vt->on_input(&w, &tap));
  TEST_ASSERT_EQ(11U, rv.page);

  /* empty book (page_count 0): destination clamps to 0. */
  rv.page_count = 0;
  m.link_dest   = 3;
  TEST_ASSERT_EQ(true, w.vt->on_input(&w, &tap));
  TEST_ASSERT_EQ(0U, rv.page);

  /* follow_link returns false -> falls through to a (boundary) page turn. */
  rv.page_count                  = 3;
  rv.page                        = 0;
  m.link_return                  = false;
  const ra8_widget_event_t right = {.kind = k_ra8_widget_ev_touch, .x = 150, .y = 50};
  TEST_ASSERT_EQ(true, w.vt->on_input(&w, &right));
  TEST_ASSERT_EQ(1U, rv.page); /* no link -> forward turn */
  TEST_END("ra8_widget_reflow_view: link follow + clamp");
}

/**
 * @test ra8_widget_reflow_view render + input + init guard arms.
 *
 * @par MC/DC:
 * `internal_rv_render`: `paint == NULL` false-of-clear arm (no bg clear); the
 * render guard `ops == NULL || render_page == NULL` both true arms (render_page
 * NULL, then ops NULL); `v == NULL` true arm (nothing). `internal_rv_link` guard
 * `ops == NULL || follow_link == NULL` both true arms (ops NULL, then
 * follow_link NULL) -- each falls through to a page turn. `internal_rv_on_input`:
 * `kind != touch` true (button declined); `v == NULL` true (declined). Plus init
 * NULL guards.
 */
static void test_reflow_view_guards(void)
{
  TEST_BEGIN("ra8_widget_reflow_view: render + input + init guards");
  s_rv_fill_calls = 0U;
  mock_rv_t m     = {};

  /* paint == NULL -> no clear; ops with NULL render_page -> no page paint. */
  ra8_widget_reflow_ops_t  no_render = {.user        = &m,
                                        .render_page = nullptr,
                                        .follow_link = mock_rv_follow};
  ra8_widget_reflow_view_t rv = {.paint = nullptr, .ops = &no_render, .page = 0, .page_count = 3};
  ra8_widget_t             w  = {};
  (void)ra8_widget_reflow_view_init(&w, &rv);
  w.rect = (ra8_ui_rect_t){.x = 0,
                           .y = 0,
                           .w = k_widget_reflow_view_w_200,
                           .h = k_widget_reflow_view_h_300};
  w.vt->render(&w);
  TEST_ASSERT_EQ(0U, s_rv_fill_calls);
  TEST_ASSERT_EQ(0U, m.render_calls);

  /* ops == NULL render arm. */
  rv.ops = nullptr;
  w.vt->render(&w);
  TEST_ASSERT_EQ(0U, m.render_calls);

  /* ctx == NULL render arm. */
  w.ctx = nullptr;
  w.vt->render(&w);
  w.ctx = &rv;

  /* input: ops == NULL -> page turn only (left arm of the link OR-guard). */
  const ra8_widget_event_t right = {.kind = k_ra8_widget_ev_touch, .x = 150, .y = 50};
  TEST_ASSERT_EQ(true, w.vt->on_input(&w, &right));
  TEST_ASSERT_EQ(1U, rv.page); /* forward turn */

  /* follow_link == NULL -> link seam declines, page turn taken (right arm). */
  ra8_widget_reflow_ops_t no_link = {.user        = &m,
                                     .render_page = mock_rv_render,
                                     .follow_link = nullptr};
  rv.ops                          = &no_link;
  rv.page                         = 0;
  TEST_ASSERT_EQ(true, w.vt->on_input(&w, &right));
  TEST_ASSERT_EQ(1U, rv.page); /* forward turn, no link followed */
  rv.ops = nullptr;

  /* a button event is declined. */
  const ra8_widget_event_t bev = {.kind = k_ra8_widget_ev_button};
  TEST_ASSERT_EQ(false, w.vt->on_input(&w, &bev));

  /* ctx == NULL declines. */
  w.ctx = nullptr;
  TEST_ASSERT_EQ(false, w.vt->on_input(&w, &right));

  /* init guards. */
  ra8_widget_reflow_view_t any = {};
  ra8_widget_t             ww  = {};
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_widget_reflow_view_init(nullptr, &any));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_widget_reflow_view_init(&ww, nullptr));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_widget_reflow_view_init(&ww, &any));
  TEST_ASSERT_EQ(true, ww.vt == ra8_widget_reflow_view_vtable());
  TEST_ASSERT_EQ(true, ww.visible);
  TEST_END("ra8_widget_reflow_view: render + input + init guards");
}

int main(void)
{
  test_reflow_view_render();
  test_reflow_view_page_turn();
  test_reflow_view_link();
  test_reflow_view_guards();
  return 0;
}
