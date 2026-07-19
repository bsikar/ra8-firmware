/**
 * @file test_ra8_widget_keyboard.c
 * @brief Unit tests for the ra8_widget on-screen-keyboard leaf (#145 Phase 2).
 *
 * @details
 * The keyboard widget draws each key (a bordered face plus its glyph or label)
 * and routes a tap to a key, applies it, and fires a commit-edge callback,
 * driving the `ra8_keyboard` engine through the injected
 * ::ra8_widget_keyboard_ops_t seam. These tests bind a recording mock seam (no
 * `ra8_keyboard`, no framebuffer), so the per-key render and the hit / apply /
 * commit routing are asserted in-process with full MC/DC vectors.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>
#include <string.h>

#include "ra8_widget.h"
#include "ra8_widget_keyboard.h"
#include "unity_minimal.h"

/**
 * @enum widget_keyboard_uint8_const_t
 * @brief Named uint8_t constants used by this file.
 *
 * @details
 * Every literal this translation unit needs, named so the
 * value's role is visible at the point of use (CLAUDE.md
 * "No Magic Numbers").
 */
typedef enum : uint8_t {
  k_widget_keyboard_idx_20 = 20,
  k_widget_keyboard_w_80   = 80,
} widget_keyboard_uint8_const_t;

/**
 * @enum widget_keyboard_uint32_const_t
 * @brief Named uint32_t constants used by this file.
 *
 * @details
 * Every literal this translation unit needs, named so the
 * value's role is visible at the point of use (CLAUDE.md
 * "No Magic Numbers").
 */
typedef enum : uint32_t {
  k_widget_keyboard_bg_00202020         = 0x00202020U,
  k_widget_keyboard_key_border_00101010 = 0x00101010U,
  k_widget_keyboard_key_face_00404040   = 0x00404040U,
  k_widget_keyboard_key_fg_00ffffff     = 0x00FFFFFFU,
} widget_keyboard_uint32_const_t;

/* --- Recording mock keyboard seam ------------------------------------------- */

/**
 * @struct mock_kb_t
 * @brief A recording ::ra8_widget_keyboard_ops_t backend for the keyboard tests.
 *
 * @details
 * A tiny 3-key layout: key 0 is a character key ('a'), key 1 is a special key
 * (label "SP", no glyph), key 2 is a blank hit area (neither glyph nor label).
 * `hit_result` selects what a tap maps to; `apply_commits` selects whether
 * `apply` reports a committed query.
 */
typedef struct {
  uint8_t  nkeys;         /**< Keys reported by count().     */
  uint8_t  hit_result;    /**< Key index hit() returns.      */
  bool     apply_commits; /**< apply() return (commit edge). */
  uint32_t apply_calls;   /**< Times apply() ran.            */
  uint8_t  last_apply;    /**< Last key applied.             */
} mock_kb_t;

static uint8_t mock_kb_count(void* user)
{
  return ((mock_kb_t*)user)->nkeys;
}

static void mock_kb_info(void* user, uint8_t idx, ra8_widget_key_info_t* out)
{
  (void)user;
  out->rect = (ra8_ui_rect_t){.x = (int32_t)idx * k_widget_keyboard_idx_20,
                              .y = 0,
                              .w = k_widget_keyboard_idx_20,
                              .h = k_widget_keyboard_idx_20};
  if (idx == 0U) {
    out->glyph = 'a';
    out->label = nullptr;
  } else if (idx == 1U) {
    out->glyph = 0;
    out->label = "SP";
  } else {
    out->glyph = 0;
    out->label = nullptr; /* blank key: face only */
  }
}

static uint8_t mock_kb_hit(void* user, int32_t x, int32_t y)
{
  (void)x;
  (void)y;
  return ((mock_kb_t*)user)->hit_result;
}

static bool mock_kb_apply(void* user, uint8_t idx)
{
  mock_kb_t* m = (mock_kb_t*)user;
  m->apply_calls++;
  m->last_apply = idx;
  return m->apply_commits;
}

/* --- Recording mock paint --------------------------------------------------- */

typedef struct {
  uint32_t    fill_calls; /**< Times fill_rect() ran. */
  uint32_t    text_calls; /**< Times draw_text() ran. */
  uint32_t    last_fg;    /**< Last text colour.      */
  const char* last_text;  /**< Last drawn string.     */
} mock_kp_t;

static void kp_fill(void* u, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t c)
{
  (void)x;
  (void)y;
  (void)w;
  (void)h;
  (void)c;
  ((mock_kp_t*)u)->fill_calls++;
}

static void kp_text(void* u, int32_t x, int32_t y, const char* s, uint32_t fg, uint32_t bg)
{
  (void)x;
  (void)y;
  (void)bg;
  mock_kp_t* m = (mock_kp_t*)u;
  m->text_calls++;
  m->last_fg   = fg;
  m->last_text = s;
}

static void kp_size(void* u, const char* s, int32_t* ow, int32_t* oh)
{
  (void)u;
  *ow = (int32_t)strlen(s) * 8;
  *oh = 16;
}

/** @brief Records keyboard commit-callback invocations. */
static uint32_t s_kb_commit_calls = 0U;
static void     test_kb_on_commit(ra8_widget_t* w)
{
  (void)w;
  s_kb_commit_calls++;
}

/** @brief Build a keyboard fixture over @p ops + @p paint. */
static ra8_widget_keyboard_t make_kbd(ra8_widget_keyboard_ops_t* ops, ra8_widget_paint_t* paint)
{
  return (ra8_widget_keyboard_t){.paint      = paint,
                                 .ops        = ops,
                                 .on_commit  = test_kb_on_commit,
                                 .bg         = k_widget_keyboard_bg_00202020,
                                 .key_face   = k_widget_keyboard_key_face_00404040,
                                 .key_border = k_widget_keyboard_key_border_00101010,
                                 .key_fg     = k_widget_keyboard_key_fg_00ffffff,
                                 .border_w   = 1};
}

/**
 * @test ra8_widget_keyboard renders the background then every key.
 *
 * @par MC/DC:
 * `internal_kbd_render`: `ops == NULL || count == NULL || key_info == NULL`
 * false arm (keys drawn). `internal_kbd_key`: `glyph != 0` true arm (key 0 draws
 * its 'a' glyph) and false arm (key 1 draws its "SP" label); `text == NULL` true
 * arm (key 2, a blank key, draws face only). Each key face is a bordered
 * `priv_fill_box` (2 fills); bg(1) + 3 keys x 2 = 7 fills, glyph + label = 2
 * texts.
 */
static void test_keyboard_render(void)
{
  TEST_BEGIN("ra8_widget_keyboard: background + keys");
  mock_kb_t                 mk    = {.nkeys = 3};
  mock_kp_t                 mp    = {};
  ra8_widget_paint_t        paint = {.user      = &mp,
                                     .fill_rect = kp_fill,
                                     .draw_text = kp_text,
                                     .text_size = kp_size};
  ra8_widget_keyboard_ops_t ops   = {.user     = &mk,
                                     .count    = mock_kb_count,
                                     .key_info = mock_kb_info,
                                     .hit      = mock_kb_hit,
                                     .apply    = mock_kb_apply};
  ra8_widget_keyboard_t     kbd   = make_kbd(&ops, &paint);
  ra8_widget_t              w     = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_widget_keyboard_init(&w, &kbd));
  w.rect =
    (ra8_ui_rect_t){.x = 0, .y = 0, .w = k_widget_keyboard_w_80, .h = k_widget_keyboard_idx_20};

  w.vt->render(&w);
  TEST_ASSERT_EQ(7U, mp.fill_calls); /* bg + 3 keys x (border + face) */
  TEST_ASSERT_EQ(2U, mp.text_calls); /* 'a' glyph + "SP" label        */
  TEST_ASSERT(strcmp(mp.last_text, "SP") == 0);
  TEST_ASSERT_EQ(0x00FFFFFFU, mp.last_fg);
  TEST_END("ra8_widget_keyboard: background + keys");
}

/**
 * @test ra8_widget_keyboard routes a tap: hit, apply, and the commit edge.
 *
 * @par MC/DC:
 * `internal_kbd_on_input`: `kind != touch` false (touch) + true (button
 * declined); `key == no_hit` false (a real key applied) + true (a gap tap ->
 * consumed, no apply); `committed && on_commit != NULL` -- `committed` true
 * (RETURN -> callback fires) + false (a plain key -> no callback).
 */
static void test_keyboard_input(void)
{
  TEST_BEGIN("ra8_widget_keyboard: hit + apply + commit");
  s_kb_commit_calls               = 0U;
  mock_kb_t                 mk    = {.nkeys = 3, .hit_result = 0, .apply_commits = false};
  mock_kp_t                 mp    = {};
  ra8_widget_paint_t        paint = {.user = &mp, .fill_rect = kp_fill, .draw_text = kp_text};
  ra8_widget_keyboard_ops_t ops   = {.user     = &mk,
                                     .count    = mock_kb_count,
                                     .key_info = mock_kb_info,
                                     .hit      = mock_kb_hit,
                                     .apply    = mock_kb_apply};
  ra8_widget_keyboard_t     kbd   = make_kbd(&ops, &paint);
  ra8_widget_t              w     = {};
  (void)ra8_widget_keyboard_init(&w, &kbd);
  w.rect =
    (ra8_ui_rect_t){.x = 0, .y = 0, .w = k_widget_keyboard_w_80, .h = k_widget_keyboard_idx_20};

  /* a touch hits key 0, apply does NOT commit -> applied, dirty, no callback. */
  const ra8_widget_event_t tap = {.kind = k_ra8_widget_ev_touch, .x = 5, .y = 5};
  TEST_ASSERT_EQ(true, w.vt->on_input(&w, &tap));
  TEST_ASSERT_EQ(1U, mk.apply_calls);
  TEST_ASSERT_EQ(0U, mk.last_apply);
  TEST_ASSERT_EQ(true, w.dirty);
  TEST_ASSERT_EQ(0U, s_kb_commit_calls);

  /* a touch that commits (RETURN) -> callback fires on the edge. */
  mk.hit_result    = 1;
  mk.apply_commits = true;
  TEST_ASSERT_EQ(true, w.vt->on_input(&w, &tap));
  TEST_ASSERT_EQ(2U, mk.apply_calls);
  TEST_ASSERT_EQ(1U, s_kb_commit_calls);

  /* a touch that hits no key -> consumed, no apply. */
  mk.hit_result = (uint8_t)k_ra8_widget_key_no_hit;
  TEST_ASSERT_EQ(true, w.vt->on_input(&w, &tap));
  TEST_ASSERT_EQ(2U, mk.apply_calls); /* unchanged */

  /* a button event is declined. */
  const ra8_widget_event_t bev = {.kind = k_ra8_widget_ev_button};
  TEST_ASSERT_EQ(false, w.vt->on_input(&w, &bev));
  TEST_END("ra8_widget_keyboard: hit + apply + commit");
}

/**
 * @brief Exercise the render draw_text / OR-guard / paint-ctx guard vectors.
 * @param[in,out] w     Widget bound to @p kbd.
 * @param[in,out] kbd   Keyboard widget under test.
 * @param[in,out] paint Paint sink (draw_text toggled).
 * @param[in]     ops   The valid ops table to restore between vectors.
 * @param[in]     mk    Mock keyboard model backing the ops tables.
 * @param[in,out] mp    Paint-call counters asserted per vector.
 * @return None.
 * @pre @p w is initialised over @p kbd.
 * @post Every render guard vector produced the expected fill/text counts; the
 *       valid ops/paint/ctx are restored on exit.
 * @note Not thread-safe; single-threaded host-test helper.
 * @since 0.1.0
 */
static void kbd_render_guards(ra8_widget_t*              w,
                              ra8_widget_keyboard_t*     kbd,
                              ra8_widget_paint_t*        paint,
                              ra8_widget_keyboard_ops_t* ops,
                              mock_kb_t*                 mk,
                              mock_kp_t*                 mp)
{
  /* draw_text == NULL -> key faces only, no glyphs (bg + 3 x 2 = 7 fills). */
  paint->draw_text = nullptr;
  w->vt->render(w);
  TEST_ASSERT_EQ(7U, mp->fill_calls);
  TEST_ASSERT_EQ(0U, mp->text_calls);

  /* render OR-guard, each condition true independently -> bg only. */
  *mp              = (mock_kp_t){};
  paint->draw_text = kp_text;
  kbd->ops         = nullptr; /* ops == NULL (left) */
  w->vt->render(w);
  TEST_ASSERT_EQ(1U, mp->fill_calls);
  *mp                                = (mock_kp_t){};
  ra8_widget_keyboard_ops_t no_count = {.user = mk, .count = nullptr, .key_info = mock_kb_info};
  kbd->ops                           = &no_count; /* count == NULL (middle) */
  w->vt->render(w);
  TEST_ASSERT_EQ(1U, mp->fill_calls);
  *mp                               = (mock_kp_t){};
  ra8_widget_keyboard_ops_t no_info = {.user = mk, .count = mock_kb_count, .key_info = nullptr};
  kbd->ops                          = &no_info; /* key_info == NULL (right) */
  w->vt->render(w);
  TEST_ASSERT_EQ(1U, mp->fill_calls);

  /* paint == NULL / ctx == NULL. */
  *mp        = (mock_kp_t){};
  kbd->ops   = ops;
  kbd->paint = nullptr;
  w->vt->render(w);
  TEST_ASSERT_EQ(0U, mp->fill_calls);
  w->ctx = nullptr;
  w->vt->render(w);
  w->ctx     = kbd;
  kbd->paint = paint;
}

/**
 * @brief Exercise the input OR-guard, commit-without-callback and ctx guards.
 * @param[in,out] w   Widget bound to @p kbd.
 * @param[in,out] kbd Keyboard widget under test.
 * @param[in]     ops The valid ops table to restore for the commit vector.
 * @param[in]     mk  Mock keyboard model (apply counters).
 * @return None.
 * @pre @p w is initialised over @p kbd with valid paint/ctx.
 * @post Each input guard vector consumed the touch without applying, the commit
 *       vector applied once with no callback, and the null-ctx vector declined.
 * @note Not thread-safe; single-threaded host-test helper.
 * @since 0.1.0
 */
static void kbd_input_guards(ra8_widget_t*              w,
                             ra8_widget_keyboard_t*     kbd,
                             ra8_widget_keyboard_ops_t* ops,
                             mock_kb_t*                 mk)
{
  /* input OR-guard, each condition true independently -> touch consumed, no
   * apply. ops == NULL (left); hit == NULL (middle); apply == NULL (right). */
  const ra8_widget_event_t tap = {.kind = k_ra8_widget_ev_touch, .x = 5, .y = 5};
  kbd->ops                     = nullptr;
  TEST_ASSERT_EQ(true, w->vt->on_input(w, &tap));
  ra8_widget_keyboard_ops_t no_hit = {.user = mk, .hit = nullptr, .apply = mock_kb_apply};
  kbd->ops                         = &no_hit;
  TEST_ASSERT_EQ(true, w->vt->on_input(w, &tap));
  ra8_widget_keyboard_ops_t no_apply = {.user = mk, .hit = mock_kb_hit, .apply = nullptr};
  kbd->ops                           = &no_apply;
  TEST_ASSERT_EQ(true, w->vt->on_input(w, &tap));
  TEST_ASSERT_EQ(0U, mk->apply_calls);

  /* a commit with no on_commit callback: applies, no crash, no callback. */
  kbd->ops          = ops;
  kbd->on_commit    = nullptr;
  s_kb_commit_calls = 0U;
  TEST_ASSERT_EQ(true, w->vt->on_input(w, &tap));
  TEST_ASSERT_EQ(1U, mk->apply_calls);
  TEST_ASSERT_EQ(0U, s_kb_commit_calls);

  /* ctx == NULL declines. */
  w->ctx = nullptr;
  TEST_ASSERT_EQ(false, w->vt->on_input(w, &tap));
}

/**
 * @brief Exercise the ra8_widget_keyboard_init null guards and success path.
 * @return None.
 * @pre None.
 * @post Both null arguments rejected and a valid init wired the vtable.
 * @note Not thread-safe; single-threaded host-test helper.
 * @since 0.1.0
 */
static void kbd_init_guards(void)
{
  ra8_widget_keyboard_t any = {};
  ra8_widget_t          ww  = {};
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_widget_keyboard_init(nullptr, &any));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_widget_keyboard_init(&ww, nullptr));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_widget_keyboard_init(&ww, &any));
  TEST_ASSERT_EQ(true, ww.vt == ra8_widget_keyboard_vtable());
  TEST_ASSERT_EQ(true, ww.visible);
}

/**
 * @test ra8_widget_keyboard render + input + init guard arms.
 *
 * @par MC/DC:
 * `internal_kbd_render`: `paint == NULL` true; the 3-condition guard `ops ==
 * NULL || count == NULL || key_info == NULL` -- each condition driven true
 * independently (ops NULL; count NULL; key_info NULL), all three yielding bg
 * only, versus the all-false render in ::test_keyboard_render; `kbd == NULL`
 * true. `internal_kbd_key` `draw_text == NULL` true (faces only, no glyphs).
 * `internal_kbd_on_input`: the 3-condition guard `ops == NULL || hit == NULL ||
 * apply == NULL` -- each condition driven true independently (ops NULL; hit
 * NULL; apply NULL), all consuming the touch without an apply, versus the
 * all-false apply in ::test_keyboard_input; `kbd == NULL` true (declined); a
 * commit with `on_commit == NULL` (no crash). Plus init NULL guards.
 */
static void test_keyboard_guards(void)
{
  TEST_BEGIN("ra8_widget_keyboard: render + input + init guards");
  mock_kb_t                 mk    = {.nkeys = 3, .hit_result = 0, .apply_commits = true};
  mock_kp_t                 mp    = {};
  ra8_widget_paint_t        paint = {.user = &mp, .fill_rect = kp_fill, .draw_text = kp_text};
  ra8_widget_keyboard_ops_t ops   = {.user     = &mk,
                                     .count    = mock_kb_count,
                                     .key_info = mock_kb_info,
                                     .hit      = mock_kb_hit,
                                     .apply    = mock_kb_apply};
  ra8_widget_keyboard_t     kbd   = make_kbd(&ops, &paint);
  ra8_widget_t              w     = {};
  (void)ra8_widget_keyboard_init(&w, &kbd);
  w.rect =
    (ra8_ui_rect_t){.x = 0, .y = 0, .w = k_widget_keyboard_w_80, .h = k_widget_keyboard_idx_20};

  kbd_render_guards(&w, &kbd, &paint, &ops, &mk, &mp);
  kbd_input_guards(&w, &kbd, &ops, &mk);
  kbd_init_guards();
  TEST_END("ra8_widget_keyboard: render + input + init guards");
}

int main(void)
{
  test_keyboard_render();
  test_keyboard_input();
  test_keyboard_guards();
  return 0;
}
