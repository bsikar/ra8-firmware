/**
 * @file test_ra_app.c
 * @brief Unit tests for the ra_app app framework (#146).
 *
 * @details
 * Pure logic -- registration (duplicate / capacity / init failure), the focus
 * lifecycle (on_leave -> on_enter, idempotent self-launch), and input routing.
 * A recording mock app vtable proves which callbacks fire. No framebuffer, so
 * this runs in-process.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra_app.h"
#include "unity_minimal.h"

typedef struct {
  uint32_t init_calls;
  uint32_t enter_calls;
  uint32_t leave_calls;
  uint32_t input_calls;
  uint32_t tick_calls;
  uint32_t render_calls;
  bool     init_fail; /**< Make init() return an error.   */
  bool     consume;   /**< on_input return value.          */
} app_ctx_t;

static ra_err_t app_init(ra_app_t* a)
{
  app_ctx_t* c = (app_ctx_t*)a->ctx;
  c->init_calls++;
  return c->init_fail ? k_ra_err_hw_init_failed : k_ra_ok;
}

static void app_enter(ra_app_t* a)
{
  ((app_ctx_t*)a->ctx)->enter_calls++;
}

static void app_leave(ra_app_t* a)
{
  ((app_ctx_t*)a->ctx)->leave_calls++;
}

static bool app_input(ra_app_t* a, const ra_widget_event_t* ev)
{
  app_ctx_t* c = (app_ctx_t*)a->ctx;
  (void)ev;
  c->input_calls++;
  return c->consume;
}

static void app_tick(ra_app_t* a)
{
  ((app_ctx_t*)a->ctx)->tick_calls++;
}

static void app_render(ra_app_t* a)
{
  ((app_ctx_t*)a->ctx)->render_calls++;
}

static const ra_app_vtable_t k_app_vt = {
  .init     = app_init,
  .on_enter = app_enter,
  .tick     = app_tick,
  .render   = app_render,
  .on_input = app_input,
  .on_leave = app_leave,
};

/** @brief A vtable whose optional callbacks are all NULL (only init set). */
static const ra_app_vtable_t k_app_vt_null = {
  .init = app_init,
};

static ra_app_t
make_app_vt(app_ctx_t* ctx, uint16_t id, const char* name, const ra_app_vtable_t* vt)
{
  ra_app_t a = {};
  a.vt       = vt;
  a.ctx      = ctx;
  a.id       = id;
  a.name     = name;
  return a;
}

static ra_app_t make_app(app_ctx_t* ctx, uint16_t id, const char* name)
{
  return make_app_vt(ctx, id, name, &k_app_vt);
}

/**
 * @test ra_app_register: init-once, duplicate id, capacity, init failure.
 *
 * @par MC/DC:
 * - capacity guard `count >= cap`,
 * - duplicate guard `existing != none`,
 * - init-failure guard `ierr != ok` (leaves the app unregistered).
 */
static void test_register(void)
{
  TEST_BEGIN("ra_app: register init/dup/cap/fail");
  app_ctx_t         c0 = {}, c1 = {}, cdup = {}, cfail = {.init_fail = true};
  ra_app_t          a0 = make_app(&c0, 1, "lib"), a1 = make_app(&c1, 2, "rdr");
  ra_app_t          adup = make_app(&cdup, 1, "dup"), afail = make_app(&cfail, 9, "bad");
  ra_app_t*         slots[2];
  ra_app_registry_t reg = {};
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_app_registry_init(&reg, slots, 2U));

  /* Register two: init fires once each. */
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_app_register(&reg, &a0));
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_app_register(&reg, &a1));
  TEST_ASSERT_EQ(1U, c0.init_calls);
  TEST_ASSERT_EQ(1U, c1.init_calls);
  TEST_ASSERT_EQ(true, a0.initialized);

  /* Duplicate id -> conflict, unchanged. */
  TEST_ASSERT_EQ((int)k_ra_err_conflict, (int)ra_app_register(&reg, &adup));
  /* Capacity full (cap 2) -> no_mem. */
  app_ctx_t c2 = {};
  ra_app_t  a2 = make_app(&c2, 3, "set");
  TEST_ASSERT_EQ((int)k_ra_err_no_mem, (int)ra_app_register(&reg, &a2));
  TEST_ASSERT_EQ(0U, c2.init_calls); /* never initialised */

  /* init failure -> propagated, left unregistered (fresh registry, room). */
  ra_app_t*         slots2[2];
  ra_app_registry_t reg2 = {};
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_app_registry_init(&reg2, slots2, 2U));
  TEST_ASSERT_EQ((int)k_ra_err_hw_init_failed, (int)ra_app_register(&reg2, &afail));
  uint16_t n = 99U;
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_app_count(&reg2, &n));
  TEST_ASSERT_EQ(0U, n);
  TEST_ASSERT_EQ(false, afail.initialized);
  TEST_END("ra_app: register init/dup/cap/fail");
}

/**
 * @test ra_app_launch: focus lifecycle (leave->enter) + idempotent self-launch.
 *
 * @par MC/DC:
 * - not-found guard `target == none`,
 * - self-launch guard `target == active` (no lifecycle calls),
 * - previous-focus guard `active != none` (fires on_leave).
 */
static void test_launch_lifecycle(void)
{
  TEST_BEGIN("ra_app: launch focus lifecycle MC/DC");
  app_ctx_t         c0 = {}, c1 = {};
  ra_app_t          a0 = make_app(&c0, 1, "lib"), a1 = make_app(&c1, 2, "rdr");
  ra_app_t*         slots[2];
  ra_app_registry_t reg = {};
  (void)ra_app_registry_init(&reg, slots, 2U);
  (void)ra_app_register(&reg, &a0);
  (void)ra_app_register(&reg, &a1);

  /* Launch a0 (no prior focus): on_enter only. */
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_app_launch(&reg, 1));
  TEST_ASSERT_EQ(1U, c0.enter_calls);
  TEST_ASSERT_EQ(0U, c0.leave_calls);

  /* Self-launch a0: idempotent, no new lifecycle calls. */
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_app_launch(&reg, 1));
  TEST_ASSERT_EQ(1U, c0.enter_calls);
  TEST_ASSERT_EQ(0U, c0.leave_calls);

  /* Switch to a1: a0.on_leave + a1.on_enter. */
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_app_launch(&reg, 2));
  TEST_ASSERT_EQ(1U, c0.leave_calls);
  TEST_ASSERT_EQ(1U, c1.enter_calls);
  ra_app_t* act = nullptr;
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_app_active(&reg, &act));
  TEST_ASSERT_EQ(2, (int)act->id);

  /* Unknown id -> not_found, focus unchanged. */
  TEST_ASSERT_EQ((int)k_ra_err_not_found, (int)ra_app_launch(&reg, 99));
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_app_active(&reg, &act));
  TEST_ASSERT_EQ(2, (int)act->id);
  TEST_END("ra_app: launch focus lifecycle MC/DC");
}

/**
 * @test ra_app_route_input routes to the active app (or no-op when none).
 *
 * @par MC/DC:
 * - active guard `active == none` -> not handled,
 * - the active app's on_input result decides handled.
 */
static void test_route_input(void)
{
  TEST_BEGIN("ra_app: route input to active");
  app_ctx_t         c0 = {.consume = true};
  ra_app_t          a0 = make_app(&c0, 1, "lib");
  ra_app_t*         slots[1];
  ra_app_registry_t reg = {};
  (void)ra_app_registry_init(&reg, slots, 1U);
  (void)ra_app_register(&reg, &a0);

  bool                    handled = true;
  const ra_widget_event_t ev      = {.kind = k_ra_widget_ev_button, .button_id = 5};
  /* No focus yet -> not handled, on_input not called. */
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_app_route_input(&reg, &ev, &handled));
  TEST_ASSERT_EQ(false, handled);
  TEST_ASSERT_EQ(0U, c0.input_calls);

  /* Focus a0 -> routes + consumes. */
  (void)ra_app_launch(&reg, 1);
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_app_route_input(&reg, &ev, &handled));
  TEST_ASSERT_EQ(true, handled);
  TEST_ASSERT_EQ(1U, c0.input_calls);
  TEST_END("ra_app: route input to active");
}

/**
 * @test build-time exclusion: an unregistered app is absent from the registry.
 *
 * @details Mirrors the `#if RA_APP_SETTINGS` pattern -- simply not calling
 * register leaves the app out of count / find / launch.
 */
static void test_buildtime_exclusion(void)
{
  TEST_BEGIN("ra_app: build-time exclusion (unregistered absent)");
  app_ctx_t         c0 = {};
  ra_app_t          a0 = make_app(&c0, 1, "lib");
  ra_app_t*         slots[2];
  ra_app_registry_t reg = {};
  (void)ra_app_registry_init(&reg, slots, 2U);
  (void)ra_app_register(&reg, &a0);
  /* "settings" (id 3) deliberately NOT registered (excluded at build time). */

  int16_t idx = 0;
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_app_find(&reg, 3, &idx));
  TEST_ASSERT_EQ((int)k_ra_app_none, (int)idx);
  TEST_ASSERT_EQ((int)k_ra_err_not_found, (int)ra_app_launch(&reg, 3));
  uint16_t n = 0U;
  (void)ra_app_count(&reg, &n);
  TEST_ASSERT_EQ(1U, n); /* only the library app shipped */
  TEST_END("ra_app: build-time exclusion (unregistered absent)");
}

/**
 * @test ra_app_tick / ra_app_render route to the active app (or no-op).
 *
 * @par MC/DC:
 * Guard `(a != null) && (a->vt->tick != null)` (likewise render). Vectors:
 * - no active focus -> no-op (left side short-circuits via the active guard),
 * - active app with a non-null tick/render -> fires,
 * - active app with a NULL tick/render -> no-op (right condition false).
 */
static void test_tick_render(void)
{
  TEST_BEGIN("ra_app: tick/render routing MC/DC");
  app_ctx_t         c0 = {}, cnull = {};
  ra_app_t          a0    = make_app(&c0, 1, "full");
  ra_app_t          anull = make_app_vt(&cnull, 2, "null", &k_app_vt_null);
  ra_app_t*         slots[2];
  ra_app_registry_t reg = {};
  (void)ra_app_registry_init(&reg, slots, 2U);
  (void)ra_app_register(&reg, &a0);
  (void)ra_app_register(&reg, &anull);

  /* Vector: no active focus -> no-op. */
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_app_tick(&reg));
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_app_render(&reg));
  TEST_ASSERT_EQ(0U, c0.tick_calls);

  /* Vector: active app with non-null tick/render -> fires. */
  (void)ra_app_launch(&reg, 1);
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_app_tick(&reg));
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_app_render(&reg));
  TEST_ASSERT_EQ(1U, c0.tick_calls);
  TEST_ASSERT_EQ(1U, c0.render_calls);

  /* Vector: active app with NULL tick/render -> no-op, no crash. */
  (void)ra_app_launch(&reg, 2);
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_app_tick(&reg));
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_app_render(&reg));
  TEST_ASSERT_EQ(0U, cnull.tick_calls);
  TEST_END("ra_app: tick/render routing MC/DC");
}

/**
 * @test Null lifecycle callbacks are skipped (no crash), not consumed.
 *
 * @par MC/DC:
 * The launch leave/enter guards `(cur != null) && (cur->vt->on_leave != null)`
 * and the route guard `(a != null) && (a->vt->on_input != null)`: a registered
 * app with NULL on_enter / on_leave / on_input exercises the right-condition-
 * false branch of each (the callback is skipped; input is not handled).
 */
static void test_null_callbacks(void)
{
  TEST_BEGIN("ra_app: null lifecycle callbacks skipped");
  app_ctx_t         c0 = {}, cnull = {};
  ra_app_t          a0    = make_app(&c0, 1, "full");
  ra_app_t          anull = make_app_vt(&cnull, 2, "null", &k_app_vt_null);
  ra_app_t*         slots[2];
  ra_app_registry_t reg = {};
  (void)ra_app_registry_init(&reg, slots, 2U);
  (void)ra_app_register(&reg, &a0);
  (void)ra_app_register(&reg, &anull);

  /* Launch the null-callback app: on_enter==NULL is skipped (no crash). */
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_app_launch(&reg, 2));
  TEST_ASSERT_EQ(0U, cnull.enter_calls);

  /* Switch to a0: anull.on_leave==NULL skipped, a0.on_enter fires. */
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_app_launch(&reg, 1));
  TEST_ASSERT_EQ(0U, cnull.leave_calls);
  TEST_ASSERT_EQ(1U, c0.enter_calls);

  /* Route input to the null-on_input app -> not handled. */
  (void)ra_app_launch(&reg, 2);
  bool                    handled = true;
  const ra_widget_event_t ev      = {.kind = k_ra_widget_ev_button, .button_id = 1};
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_app_route_input(&reg, &ev, &handled));
  TEST_ASSERT_EQ(false, handled);
  TEST_END("ra_app: null lifecycle callbacks skipped");
}

/**
 * @test Every public entry rejects a NULL required pointer (NASA Rule 5).
 */
static void test_null_guards(void)
{
  TEST_BEGIN("ra_app: null-arg guards");
  ra_app_registry_t reg = {};
  ra_app_t*         slots[1];
  (void)ra_app_registry_init(&reg, slots, 1U);
  int16_t                 idx     = 0;
  ra_app_t*               app     = nullptr;
  bool                    handled = false;
  const ra_widget_event_t ev      = {.kind = k_ra_widget_ev_button};
  TEST_ASSERT_EQ((int)k_ra_err_null_ptr, (int)ra_app_registry_init(nullptr, slots, 1U));
  TEST_ASSERT_EQ((int)k_ra_err_null_ptr, (int)ra_app_register(nullptr, nullptr));
  TEST_ASSERT_EQ((int)k_ra_err_null_ptr, (int)ra_app_find(nullptr, 1, &idx));
  TEST_ASSERT_EQ((int)k_ra_err_null_ptr, (int)ra_app_launch(nullptr, 1));
  TEST_ASSERT_EQ((int)k_ra_err_null_ptr, (int)ra_app_active(&reg, nullptr));
  TEST_ASSERT_EQ((int)k_ra_err_null_ptr, (int)ra_app_route_input(nullptr, &ev, &handled));
  TEST_ASSERT_EQ((int)k_ra_err_null_ptr, (int)ra_app_tick(nullptr));
  TEST_ASSERT_EQ((int)k_ra_err_null_ptr, (int)ra_app_render(nullptr));
  TEST_ASSERT_EQ((int)k_ra_err_null_ptr, (int)ra_app_count(&reg, nullptr));
  TEST_ASSERT_EQ((int)k_ra_err_out_of_range, (int)ra_app_at(&reg, 5, &app));
  TEST_END("ra_app: null-arg guards");
}

int main(void)
{
  test_register();
  test_launch_lifecycle();
  test_route_input();
  test_buildtime_exclusion();
  test_tick_render();
  test_null_callbacks();
  test_null_guards();
  return 0;
}
