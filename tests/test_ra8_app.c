/**
 * @file test_ra8_app.c
 * @brief Unit tests for the ra8_app app framework (#146).
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

#include "ra8_app.h"
#include "unity_minimal.h"

/**
 * @enum app_fixture_t
 * @brief Poison values written into out-parameters before a call, so one that fails without assigning is detectable.
 */
typedef enum : uint8_t {
  k_app_id_library = 10,  /**< Id of the library app in the three-app fixture. */
  k_app_id_mount_a = 100, /**< First app of the mount-ordering fixture.        */
  k_app_id_reader  = 20,  /**< Id of the reader app.                           */
  k_app_id_mount_b = 200, /**< Its second.                                     */
  k_app_id_settings =
    30, /**< Id of the settings app. The three are spaced apart so an off-by-one in a lookup cannot land on a neighbour. */
  k_app_id_freed       = 7, /**< App registered only to be freed again.   */
  k_app_id_init_fails  = 9, /**< App whose init deliberately fails.       */
  k_app_id_null_vtable = 5, /**< App registered with a null vtable entry. */
  k_app_poison_out =
    99U, /**< Poison written into a count or depth out-parameter, so a call that fails without setting it is detectable. */
  k_app_id_unregistered =
    77U, /**< An id no fixture app registers, so a back-trail entry naming it must be rejected. */
} app_fixture_t;

typedef struct {
  uint32_t init_calls;   /**< Init calls.                                */
  uint32_t enter_calls;  /**< Enter calls.                               */
  uint32_t leave_calls;  /**< Leave calls.                               */
  uint32_t input_calls;  /**< Input calls.                               */
  uint32_t tick_calls;   /**< Tick calls.                                */
  uint32_t render_calls; /**< Render calls.                              */
  uint32_t deinit_calls; /**< Times deinit() fired (uninstall teardown). */
  bool     init_fail;    /**< Make init() return an error.               */
  bool     consume;      /**< on_input return value.                     */
} app_ctx_t;

static ra8_err_t app_init(ra8_app_t* a)
{
  app_ctx_t* c = (app_ctx_t*)a->ctx;
  c->init_calls++;
  return c->init_fail ? k_ra8_err_hw_init_failed : k_ra8_ok;
}

static void app_enter(ra8_app_t* a)
{
  ((app_ctx_t*)a->ctx)->enter_calls++;
}

static void app_leave(ra8_app_t* a)
{
  ((app_ctx_t*)a->ctx)->leave_calls++;
}

static bool app_input(ra8_app_t* a, const ra8_widget_event_t* ev)
{
  app_ctx_t* c = (app_ctx_t*)a->ctx;
  (void)ev;
  c->input_calls++;
  return c->consume;
}

static void app_tick(ra8_app_t* a)
{
  ((app_ctx_t*)a->ctx)->tick_calls++;
}

static void app_render(const ra8_app_t* a)
{
  ((app_ctx_t*)a->ctx)->render_calls++;
}

static void app_deinit(ra8_app_t* a)
{
  ((app_ctx_t*)a->ctx)->deinit_calls++;
}

static const ra8_app_vtable_t k_app_vt = {
  .init     = app_init,
  .on_enter = app_enter,
  .tick     = app_tick,
  .render   = app_render,
  .on_input = app_input,
  .on_leave = app_leave,
  .deinit   = app_deinit,
};

/** @brief A vtable whose optional callbacks are all NULL (only init set). */
static const ra8_app_vtable_t k_app_vt_null = {
  .init = app_init,
};

static ra8_app_t
make_app_vt(app_ctx_t* ctx, uint16_t id, const char* name, const ra8_app_vtable_t* vt)
{
  ra8_app_t a = {};
  a.vt        = vt;
  a.ctx       = ctx;
  a.id        = id;
  a.name      = name;
  return a;
}

static ra8_app_t make_app(app_ctx_t* ctx, uint16_t id, const char* name)
{
  return make_app_vt(ctx, id, name, &k_app_vt);
}

/**
 * @test ra8_app_register: init-once, duplicate id, capacity, init failure.
 *
 * @par MC/DC:
 * - capacity guard `count >= cap`,
 * - duplicate guard `existing != none`,
 * - init-failure guard `ierr != ok` (leaves the app unregistered).
 */
static void test_register(void)
{
  TEST_BEGIN("ra8_app: register init/dup/cap/fail");
  app_ctx_t          c0    = {};
  app_ctx_t          c1    = {};
  app_ctx_t          cdup  = {};
  app_ctx_t          cfail = {.init_fail = true};
  ra8_app_t          a0    = make_app(&c0, 1, "lib");
  ra8_app_t          a1    = make_app(&c1, 2, "rdr");
  ra8_app_t          adup  = make_app(&cdup, 1, "dup");
  ra8_app_t          afail = make_app(&cfail, k_app_id_init_fails, "bad");
  ra8_app_t*         slots[2];
  ra8_app_registry_t reg = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_app_registry_init(&reg, slots, 2U));

  /* Register two: init fires once each. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_app_register(&reg, &a0));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_app_register(&reg, &a1));
  TEST_ASSERT_EQ(1U, c0.init_calls);
  TEST_ASSERT_EQ(1U, c1.init_calls);
  TEST_ASSERT_EQ(true, a0.initialized);

  /* Duplicate id -> conflict, unchanged. */
  TEST_ASSERT_EQ(k_ra8_err_conflict, ra8_app_register(&reg, &adup));
  /* Capacity full (cap 2) -> no_mem. */
  app_ctx_t c2 = {};
  ra8_app_t a2 = make_app(&c2, 3, "set");
  TEST_ASSERT_EQ(k_ra8_err_no_mem, ra8_app_register(&reg, &a2));
  TEST_ASSERT_EQ(0U, c2.init_calls); /* never initialised */

  /* init failure -> propagated, left unregistered (fresh registry, room). */
  ra8_app_t*         slots2[2];
  ra8_app_registry_t reg2 = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_app_registry_init(&reg2, slots2, 2U));
  TEST_ASSERT_EQ(k_ra8_err_hw_init_failed, ra8_app_register(&reg2, &afail));
  uint16_t n = k_app_poison_out;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_app_count(&reg2, &n));
  TEST_ASSERT_EQ(0U, n);
  TEST_ASSERT_EQ(false, afail.initialized);
  TEST_END("ra8_app: register init/dup/cap/fail");
}

/**
 * @test ra8_app_launch: focus lifecycle (leave->enter) + idempotent self-launch.
 *
 * @par MC/DC:
 * - not-found guard `target == none`,
 * - self-launch guard `target == active` (no lifecycle calls),
 * - previous-focus guard `active != none` (fires on_leave).
 */
static void test_launch_lifecycle(void)
{
  TEST_BEGIN("ra8_app: launch focus lifecycle MC/DC");
  app_ctx_t          c0 = {};
  app_ctx_t          c1 = {};
  ra8_app_t          a0 = make_app(&c0, 1, "lib");
  ra8_app_t          a1 = make_app(&c1, 2, "rdr");
  ra8_app_t*         slots[2];
  ra8_app_registry_t reg = {};
  (void)ra8_app_registry_init(&reg, slots, 2U);
  (void)ra8_app_register(&reg, &a0);
  (void)ra8_app_register(&reg, &a1);

  /* Launch a0 (no prior focus): on_enter only. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_app_launch(&reg, 1));
  TEST_ASSERT_EQ(1U, c0.enter_calls);
  TEST_ASSERT_EQ(0U, c0.leave_calls);

  /* Self-launch a0: idempotent, no new lifecycle calls. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_app_launch(&reg, 1));
  TEST_ASSERT_EQ(1U, c0.enter_calls);
  TEST_ASSERT_EQ(0U, c0.leave_calls);

  /* Switch to a1: a0.on_leave + a1.on_enter. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_app_launch(&reg, 2));
  TEST_ASSERT_EQ(1U, c0.leave_calls);
  TEST_ASSERT_EQ(1U, c1.enter_calls);
  ra8_app_t* act = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_app_active(&reg, &act));
  TEST_ASSERT_EQ(2, act->id);

  /* Unknown id -> not_found, focus unchanged. */
  TEST_ASSERT_EQ(k_ra8_err_not_found, ra8_app_launch(&reg, 99));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_app_active(&reg, &act));
  TEST_ASSERT_EQ(2, act->id);
  TEST_END("ra8_app: launch focus lifecycle MC/DC");
}

/**
 * @test ra8_app_route_input routes to the active app (or no-op when none).
 *
 * @par MC/DC:
 * - active guard `active == none` -> not handled,
 * - the active app's on_input result decides handled.
 */
static void test_route_input(void)
{
  TEST_BEGIN("ra8_app: route input to active");
  app_ctx_t          c0 = {.consume = true};
  ra8_app_t          a0 = make_app(&c0, 1, "lib");
  ra8_app_t*         slots[1];
  ra8_app_registry_t reg = {};
  (void)ra8_app_registry_init(&reg, slots, 1U);
  (void)ra8_app_register(&reg, &a0);

  bool                     handled = true;
  const ra8_widget_event_t ev      = {.kind = k_ra8_widget_ev_button, .button_id = 5};
  /* No focus yet -> not handled, on_input not called. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_app_route_input(&reg, &ev, &handled));
  TEST_ASSERT_EQ(false, handled);
  TEST_ASSERT_EQ(0U, c0.input_calls);

  /* Focus a0 -> routes + consumes. */
  (void)ra8_app_launch(&reg, 1);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_app_route_input(&reg, &ev, &handled));
  TEST_ASSERT_EQ(true, handled);
  TEST_ASSERT_EQ(1U, c0.input_calls);
  TEST_END("ra8_app: route input to active");
}

/**
 * @test build-time exclusion: an unregistered app is absent from the registry.
 *
 * @details Mirrors the `#if RA8_APP_SETTINGS` pattern -- simply not calling
 * register leaves the app out of count / find / launch.
 */
static void test_buildtime_exclusion(void)
{
  TEST_BEGIN("ra8_app: build-time exclusion (unregistered absent)");
  app_ctx_t          c0 = {};
  ra8_app_t          a0 = make_app(&c0, 1, "lib");
  ra8_app_t*         slots[2];
  ra8_app_registry_t reg = {};
  (void)ra8_app_registry_init(&reg, slots, 2U);
  (void)ra8_app_register(&reg, &a0);
  /* "settings" (id 3) deliberately NOT registered (excluded at build time). */

  int16_t idx = 0;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_app_find(&reg, 3, &idx));
  TEST_ASSERT_EQ(k_ra8_app_none, idx);
  TEST_ASSERT_EQ(k_ra8_err_not_found, ra8_app_launch(&reg, 3));
  uint16_t n = 0U;
  (void)ra8_app_count(&reg, &n);
  TEST_ASSERT_EQ(1U, n); /* only the library app shipped */
  TEST_END("ra8_app: build-time exclusion (unregistered absent)");
}

/**
 * @test ra8_app_tick / ra8_app_render route to the active app (or no-op).
 *
 * @par MC/DC:
 * Guard `(a != null) && (a->vt->tick != null)` (likewise render). Vectors:
 * - no active focus -> no-op (left side short-circuits via the active guard),
 * - active app with a non-null tick/render -> fires,
 * - active app with a NULL tick/render -> no-op (right condition false).
 */
static void test_tick_render(void)
{
  TEST_BEGIN("ra8_app: tick/render routing MC/DC");
  app_ctx_t          c0    = {};
  app_ctx_t          cnull = {};
  ra8_app_t          a0    = make_app(&c0, 1, "full");
  ra8_app_t          anull = make_app_vt(&cnull, 2, "null", &k_app_vt_null);
  ra8_app_t*         slots[2];
  ra8_app_registry_t reg = {};
  (void)ra8_app_registry_init(&reg, slots, 2U);
  (void)ra8_app_register(&reg, &a0);
  (void)ra8_app_register(&reg, &anull);

  /* Vector: no active focus -> no-op. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_app_tick(&reg));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_app_render(&reg));
  TEST_ASSERT_EQ(0U, c0.tick_calls);

  /* Vector: active app with non-null tick/render -> fires. */
  (void)ra8_app_launch(&reg, 1);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_app_tick(&reg));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_app_render(&reg));
  TEST_ASSERT_EQ(1U, c0.tick_calls);
  TEST_ASSERT_EQ(1U, c0.render_calls);

  /* Vector: active app with NULL tick/render -> no-op, no crash. */
  (void)ra8_app_launch(&reg, 2);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_app_tick(&reg));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_app_render(&reg));
  TEST_ASSERT_EQ(0U, cnull.tick_calls);
  TEST_END("ra8_app: tick/render routing MC/DC");
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
  TEST_BEGIN("ra8_app: null lifecycle callbacks skipped");
  app_ctx_t          c0    = {};
  app_ctx_t          cnull = {};
  ra8_app_t          a0    = make_app(&c0, 1, "full");
  ra8_app_t          anull = make_app_vt(&cnull, 2, "null", &k_app_vt_null);
  ra8_app_t*         slots[2];
  ra8_app_registry_t reg = {};
  (void)ra8_app_registry_init(&reg, slots, 2U);
  (void)ra8_app_register(&reg, &a0);
  (void)ra8_app_register(&reg, &anull);

  /* Launch the null-callback app: on_enter==NULL is skipped (no crash). */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_app_launch(&reg, 2));
  TEST_ASSERT_EQ(0U, cnull.enter_calls);

  /* Switch to a0: anull.on_leave==NULL skipped, a0.on_enter fires. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_app_launch(&reg, 1));
  TEST_ASSERT_EQ(0U, cnull.leave_calls);
  TEST_ASSERT_EQ(1U, c0.enter_calls);

  /* Route input to the null-on_input app -> not handled. */
  (void)ra8_app_launch(&reg, 2);
  bool                     handled = true;
  const ra8_widget_event_t ev      = {.kind = k_ra8_widget_ev_button, .button_id = 1};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_app_route_input(&reg, &ev, &handled));
  TEST_ASSERT_EQ(false, handled);
  TEST_END("ra8_app: null lifecycle callbacks skipped");
}

/**
 * @test Every public entry rejects a NULL required pointer (NASA Rule 5).
 */
static void test_null_guards(void)
{
  TEST_BEGIN("ra8_app: null-arg guards");
  ra8_app_registry_t reg = {};
  ra8_app_t*         slots[1];
  (void)ra8_app_registry_init(&reg, slots, 1U);
  int16_t                  idx     = 0;
  ra8_app_t*               app     = nullptr;
  bool                     handled = false;
  const ra8_widget_event_t ev      = {.kind = k_ra8_widget_ev_button};
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_app_registry_init(nullptr, slots, 1U));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_app_register(nullptr, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_app_find(nullptr, 1, &idx));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_app_launch(nullptr, 1));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_app_active(&reg, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_app_route_input(nullptr, &ev, &handled));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_app_tick(nullptr));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_app_render(nullptr));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_app_count(&reg, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_out_of_range, ra8_app_at(&reg, 5, &app));
  TEST_END("ra8_app: null-arg guards");
}

/**
 * @test Null registry-slot arms: a slot pointer that is NULL (left condition
 *       false) for find / launch-leave / launch-enter / route / tick / render.
 *
 * @details
 * A registry slot can legitimately hold NULL (a caller-provided storage array
 * entry that was never filled, or cleared). The framework defends every slot
 * dereference with `apps[i] != NULL`; the existing tests only ever populate
 * non-null slots, so the left-false arm of each guard is exercised here by
 * driving the public API against a hand-built registry whose `apps[]` entries
 * are NULL while `count` / `active` still index them.
 *
 * @par MC/DC:
 * - find guard `(apps[i] != NULL) && (apps[i]->id == id)` (ra8_app.c L37): the
 *   `apps[i] == NULL` left-false arm -- a NULL slot is skipped, so the id is not
 *   found (its both-true arm is covered by ::test_launch_lifecycle's hits).
 * - launch leave guard `(cur != NULL) && (cur->vt->on_leave != NULL)` (L91):
 *   the `cur == NULL` left-false arm -- a NULL previously-active slot is skipped
 *   (no on_leave call, no crash).
 * - launch enter guard `(next != NULL) && (next->vt->on_enter != NULL)` (L97):
 *   the `next == NULL` left-false arm is UNREACHABLE via the public API (see the
 *   inline note below) -- the launch target always resolves to a non-null slot.
 * - route guard `(a != NULL) && (a->vt->on_input != NULL)` (L122): the
 *   `a == NULL` left-false arm -- a NULL active slot routes nothing (not handled).
 * - tick guard `(a != NULL) && (a->vt->tick != NULL)` (L135): the `a == NULL`
 *   left-false arm -- a NULL active slot ticks nothing.
 * - render guard `(a != NULL) && (a->vt->render != NULL)` (L148): the
 *   `a == NULL` left-false arm -- a NULL active slot renders nothing.
 */
static void test_null_slots(void)
{
  TEST_BEGIN("ra8_app: null registry-slot guards MC/DC");
  app_ctx_t c0 = {};
  ra8_app_t a0 = make_app(&c0, 1, "lib");

  /* L37: a NULL slot inside [0, count) is skipped during find. */
  ra8_app_t*         slots_find[2] = {nullptr, &a0};
  ra8_app_registry_t rfind         = {.apps = slots_find, .cap = 2U, .count = 2U, .active = -1};
  int16_t            idx           = 0;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_app_find(&rfind, 1, &idx));
  TEST_ASSERT_EQ(1, idx); /* slot 0 (NULL) skipped, slot 1 matched */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_app_find(&rfind, 7, &idx));
  TEST_ASSERT_EQ(k_ra8_app_none, idx); /* NULL slot never matches */

  /* L91: previously-active slot is NULL -> leave guard left-false, skipped.
   * active = 0 (a NULL slot), launch id 1 at slot 1 (valid) -> on_enter fires,
   * but the NULL outgoing slot's on_leave is safely skipped. */
  ra8_app_t*         slots_leave[2] = {nullptr, &a0};
  ra8_app_registry_t rleave         = {.apps = slots_leave, .cap = 2U, .count = 2U, .active = 0};
  c0.enter_calls                    = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_app_launch(&rleave, 1));
  TEST_ASSERT_EQ(1, rleave.active);   /* switched to slot 1      */
  TEST_ASSERT_EQ(1U, c0.enter_calls); /* incoming on_enter fired */

  /* L97 (`next == NULL` enter-guard arm) is UNREACHABLE through the public API:
   * the launch target comes from ra8_app_find(), which only returns an index
   * whose `apps[target]` is non-NULL (its own L37 guard filters NULL slots).
   * After `reg->active = target`, `next = reg->apps[target]` is therefore always
   * non-null, so the `next == NULL` branch is dead defensive code. It is left in
   * place as belt-and-braces and cannot be covered by a public-API test. */

  /* L122: active slot is NULL -> route guard left-false, nothing handled. */
  ra8_app_t*               slots_act[1] = {nullptr};
  ra8_app_registry_t       ract         = {.apps = slots_act, .cap = 1U, .count = 1U, .active = 0};
  bool                     handled      = true;
  const ra8_widget_event_t ev           = {.kind = k_ra8_widget_ev_button, .button_id = 3};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_app_route_input(&ract, &ev, &handled));
  TEST_ASSERT_EQ(false, handled);

  /* L135: active slot is NULL -> tick guard left-false, no-op. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_app_tick(&ract));

  /* L148: active slot is NULL -> render guard left-false, no-op. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_app_render(&ract));
  TEST_END("ra8_app: null registry-slot guards MC/DC");
}

/**
 * @brief Drive the go-by-index / back-stack MC/DC sequence over three apps.
 * @param[in,out] nav Initialised navigator over @p reg.
 * @param[in]     reg Registry holding the library/reader/settings apps.
 * @param[in]     c0  Context of app 0 (library) for enter/leave counters.
 * @param[in]     c1  Context of app 1 (reader) for enter/leave counters.
 * @pre @p nav and @p reg are initialised with three registered apps.
 * @post Every go_index / nav_back decision vector held.
 * @note Not thread-safe; single-threaded host-test helper.
 * @since 0.1.0
 */
static void
nav_check_back_stack(ra8_app_nav_t* nav, ra8_app_registry_t* reg, app_ctx_t* c0, app_ctx_t* c1)
{
  /* go_index(0): Decision A false (no prior app) -> nothing pushed, depth 0. */
  ra8_app_t* act   = nullptr;
  uint16_t   depth = k_app_poison_out;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_app_nav_go_index(nav, 0U));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_app_active(reg, &act));
  TEST_ASSERT_EQ(10, act->id);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_app_nav_depth(nav, &depth));
  TEST_ASSERT_EQ(0U, depth);

  /* go_index(1): Decision A true + B true + C false -> push library, depth 1. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_app_nav_go_index(nav, 1U));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_app_active(reg, &act));
  TEST_ASSERT_EQ(20, act->id);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_app_nav_depth(nav, &depth));
  TEST_ASSERT_EQ(1U, depth);
  TEST_ASSERT_EQ(1U, c0->leave_calls);
  TEST_ASSERT_EQ(1U, c1->enter_calls);

  /* nav_back: depth-guard false -> pop library, reader leaves, library re-enters. */
  bool popped = false;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_app_nav_back(nav, &popped));
  TEST_ASSERT_EQ(true, popped);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_app_active(reg, &act));
  TEST_ASSERT_EQ(10, act->id);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_app_nav_depth(nav, &depth));
  TEST_ASSERT_EQ(0U, depth);
  TEST_ASSERT_EQ(2U, c0->enter_calls);

  /* go_index(0) re-tap: Decision A true + B false -> idempotent, no push, no
   * second on_enter (enter count stays 2), depth still 0. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_app_nav_go_index(nav, 0U));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_app_nav_depth(nav, &depth));
  TEST_ASSERT_EQ(0U, depth);
  TEST_ASSERT_EQ(2U, c0->enter_calls);

  /* nav_back: depth-guard true (root) -> nothing popped, not an error. */
  popped = true;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_app_nav_back(nav, &popped));
  TEST_ASSERT_EQ(false, popped);

  /* go_index range guard true: out-of-range index -> out_of_range, focus kept. */
  TEST_ASSERT_EQ(k_ra8_err_out_of_range, ra8_app_nav_go_index(nav, 9U));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_app_active(reg, &act));
  TEST_ASSERT_EQ(10, act->id);
}

/**
 * @brief go_index NULL-slot guard: a valid index whose slot is NULL rejects.
 * @pre None.
 * @post `ra8_app_nav_go_index` returned k_ra8_err_null_ptr.
 * @note Not thread-safe; single-threaded host-test helper.
 * @since 0.1.0
 */
static void nav_check_null_slot(void)
{
  ra8_app_t*         nslots[1] = {nullptr};
  ra8_app_registry_t nreg      = {.apps = nslots, .cap = 1U, .count = 1U, .active = -1};
  ra8_app_nav_t      nnav      = {};
  uint16_t           ntrail[1];
  TEST_ASSERT_EQ(k_ra8_ok, ra8_app_nav_init(&nnav, &nreg, ntrail, 1U));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_app_nav_go_index(&nnav, 0U));
}

/**
 * @brief nav_go Decision C true: a full push stack rejects the next switch.
 * @pre None.
 * @post The over-capacity push returned k_ra8_err_no_mem with focus unchanged.
 * @note Not thread-safe; single-threaded host-test helper.
 * @since 0.1.0
 */
static void nav_check_push_capacity(void)
{
  app_ctx_t          mc0 = {};
  app_ctx_t          mc1 = {};
  ra8_app_t          m0  = make_app(&mc0, k_app_id_mount_a, "m0");
  ra8_app_t          m1  = make_app(&mc1, k_app_id_mount_b, "m1");
  ra8_app_t*         mslots[2];
  ra8_app_registry_t mreg = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_app_registry_init(&mreg, mslots, 2U));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_app_register(&mreg, &m0));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_app_register(&mreg, &m1));
  uint16_t      mtrail[1];
  ra8_app_nav_t mnav = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_app_nav_init(&mnav, &mreg, mtrail, 1U));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_app_nav_go_index(&mnav, 0U)); /* depth 0   */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_app_nav_go_index(&mnav, 1U)); /* push, d=1 */
  TEST_ASSERT_EQ(k_ra8_err_no_mem, ra8_app_nav_go_index(&mnav, 0U));
  ra8_app_t* mact = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_app_active(&mreg, &mact));
  TEST_ASSERT_EQ(200, mact->id); /* still on m1: the full-stack push was rejected */
}

/**
 * @brief go_index null-argument guards: NULL nav, then NULL nav->reg.
 * @pre None.
 * @post Both null arguments returned k_ra8_err_null_ptr.
 * @note Not thread-safe; single-threaded host-test helper.
 * @since 0.1.0
 */
static void nav_check_null_args(void)
{
  ra8_app_nav_t bad = {.reg = nullptr};
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_app_nav_go_index(nullptr, 0U));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_app_nav_go_index(&bad, 0U));
}

/**
 * @test ra8_app_nav_go_index: launcher select-by-position + back-stack MC/DC.
 *
 * @details
 * Exercises the by-index launcher bridge plus the nav layer it composes:
 * `go_index` resolves a registry position to an app id and focuses it through
 * `ra8_app_nav_go` (pushing the outgoing app), and `ra8_app_nav_back` pops it.
 * This is the first coverage of the navigation back-stack, so it drives the
 * first-focus (no push), switch (push), idempotent re-tap (no push), stack-full
 * (no_mem), pop, and root-no-op paths in addition to `go_index`'s own guards.
 *
 * @par MC/DC:
 * Each decision below is single-condition; minimal MC/DC is its two branch
 * outcomes (N=1 -> N+1=2 vectors).
 * - `go_index` range guard `aerr != k_ra8_ok` (from ra8_app_at): false = in-range
 *   index 0/1 -> launch; true = out-of-range index 9 -> out_of_range, focus kept.
 * - `go_index` NULL-slot guard `app == NULL`: true = valid index whose slot is
 *   NULL -> null_ptr; false = the populated slots above -> launch proceeds.
 * - `nav_go` Decision A `cur != nullptr` (was an app focused?): false = the very
 *   first focus (no prior app, nothing pushed); true = every later switch.
 * - `nav_go` Decision B `cur->id != id` (a *different* app?): true = switch to a
 *   different index (push); false = idempotent re-tap of the focused index (no
 *   push, no on_enter).
 * - `nav_go` Decision C `nav->depth >= nav->cap` (push capacity): false = room on
 *   the trail -> push; true = trail full -> k_ra8_err_no_mem, focus unchanged.
 * - `nav_back` depth guard `depth == 0`: false = a populated stack pops; true =
 *   the emptied stack reports not-popped at the root.
 */
static void test_nav_go_index(void)
{
  TEST_BEGIN("ra8_app: nav go-by-index + back-stack MC/DC");
  app_ctx_t          c0 = {};
  app_ctx_t          c1 = {};
  app_ctx_t          c2 = {};
  ra8_app_t          a0 = make_app(&c0, k_app_id_library, "library");
  ra8_app_t          a1 = make_app(&c1, k_app_id_reader, "reader");
  ra8_app_t          a2 = make_app(&c2, k_app_id_settings, "settings");
  ra8_app_t*         slots[3];
  ra8_app_registry_t reg = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_app_registry_init(&reg, slots, 3U));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_app_register(&reg, &a0));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_app_register(&reg, &a1));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_app_register(&reg, &a2));

  uint16_t      trail[4];
  ra8_app_nav_t nav = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_app_nav_init(&nav, &reg, trail, 4U));

  nav_check_back_stack(&nav, &reg, &c0, &c1);
  nav_check_null_slot();
  nav_check_push_capacity();
  nav_check_null_args();
  TEST_END("ra8_app: nav go-by-index + back-stack MC/DC");
}

/**
 * @test Zero-capacity init guards and navigation launch-error propagation.
 *
 * @details
 * Closes the remaining edge branches the lifecycle / nav tests above never
 * reach: the `cap == 0` rejections in ::ra8_app_registry_init (ra8_app.c L22)
 * and ::ra8_app_nav_init (L180), and the two navigation paths that forward a
 * failing ::ra8_app_launch result -- a forward ::ra8_app_nav_go to an
 * unregistered id (L217) and a ::ra8_app_nav_back whose trail entry no longer
 * resolves to a live app (L251). The launch failure is provoked with a
 * genuinely absent id rather than by corrupting registry state, so the
 * forwarded code is the real `k_ra8_err_not_found` a caller would observe.
 *
 * The sibling find / active propagation guards (L55 in ::ra8_app_register, L81
 * in ::ra8_app_launch, L196 in ::ra8_app_nav_go) are intentionally NOT exercised
 * here: each forwards a ::ra8_app_find / ::ra8_app_active result whose only
 * failure mode is a NULL argument, yet every call site passes an
 * already-null-checked registry pointer and the address of a stack local. They
 * are dead defensive branches, unreachable through the public API (the same
 * status ::test_null_slots records for the L97 enter-guard arm).
 *
 * @par MC/DC:
 * Every decision exercised here is single-condition; minimal coverage is its
 * two branch outcomes (N=1 -> N+1=2 vectors), the false arm of each already
 * being covered by the tests above.
 * - ::ra8_app_registry_init guard `cap == 0` (L21): true = cap 0 ->
 *   `k_ra8_err_invalid_arg` (L22); false = the cap >= 1 inits used everywhere.
 * - ::ra8_app_nav_init guard `cap == 0` (L179): true = cap 0 ->
 *   `k_ra8_err_invalid_arg` (L180); false = the cap >= 1 inits above.
 * - ::ra8_app_nav_go forward-launch guard `lerr != k_ra8_ok` (L216): true = the
 *   launch of an unregistered id returns `k_ra8_err_not_found`, forwarded at
 *   L217 with nothing pushed; false = the successful switches in
 *   ::test_nav_go_index.
 * - ::ra8_app_nav_back pop-launch guard `lerr != k_ra8_ok` (L250): true = a trail
 *   entry whose id is absent from the registry makes the relaunch fail
 *   (`k_ra8_err_not_found` forwarded at L251, depth / out_popped unchanged);
 *   false = the successful pop in ::test_nav_go_index.
 */
static void test_zero_cap_and_nav_launch_errors(void)
{
  TEST_BEGIN("ra8_app: zero-cap guards + nav launch-error propagation");

  /* L21/L22: registry_init rejects a zero capacity. */
  ra8_app_registry_t reg0      = {};
  ra8_app_t*         slots0[1] = {nullptr};
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_app_registry_init(&reg0, slots0, 0U));

  /* A small populated registry (only id 1) for the nav cases below. */
  app_ctx_t          c0 = {};
  ra8_app_t          a0 = make_app(&c0, 1, "lib");
  ra8_app_t*         slots[2];
  ra8_app_registry_t reg = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_app_registry_init(&reg, slots, 2U));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_app_register(&reg, &a0));

  /* L179/L180: nav_init rejects a zero capacity. */
  ra8_app_nav_t nav0      = {};
  uint16_t      trail0[1] = {};
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_app_nav_init(&nav0, &reg, trail0, 0U));

  /* L216/L217: forward nav_go to an unregistered id. Nothing is focused yet, so
   * Decision A (`cur != nullptr`) is false -> `will_push` stays false -> the
   * failing launch is forwarded directly, leaving depth 0 and focus at none. */
  ra8_app_nav_t nav      = {};
  uint16_t      trail[2] = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_app_nav_init(&nav, &reg, trail, 2U));
  TEST_ASSERT_EQ(k_ra8_err_not_found, ra8_app_nav_go(&nav, 99U));
  uint16_t depth = k_app_poison_out;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_app_nav_depth(&nav, &depth));
  TEST_ASSERT_EQ(0U, depth); /* failing launch pushed nothing */
  ra8_app_t* act = &a0;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_app_active(&reg, &act));
  TEST_ASSERT_NULL(act); /* focus untouched: still none */

  /* L250/L251: nav_back whose trail entry no longer resolves. A hand-built nav
   * with depth 1 and a stack id absent from the registry makes the relaunch
   * return `k_ra8_err_not_found`; it is forwarded and the stack is left intact. */
  uint16_t      btrail[1] = {k_app_id_unregistered}; /* id 77 was never registered */
  ra8_app_nav_t bnav      = {.reg = &reg, .stack = btrail, .cap = 1U, .depth = 1U};
  bool          popped    = true;
  TEST_ASSERT_EQ(k_ra8_err_not_found, ra8_app_nav_back(&bnav, &popped));
  TEST_ASSERT_EQ(false, popped);  /* pop did not complete       */
  TEST_ASSERT_EQ(1U, bnav.depth); /* trail untouched on failure */

  TEST_END("ra8_app: zero-cap guards + nav launch-error propagation");
}

/**
 * @test ra8_app_state derives unmounted / background / foreground from the registry.
 *
 * @details
 * The lifecycle state is not a stored field; ra8_app_state computes it from
 * membership plus the focused index. This drives every reachable branch: an
 * unregistered id reports unmounted, a registered-but-not-active app reports
 * background, and the focused app reports foreground -- across a first focus and
 * a switch (which suspends the outgoing app back to background).
 *
 * @par MC/DC:
 * Each decision in ra8_app_state is single-condition (N=1 -> N+1=2 vectors):
 * - find-forward guard `ferr != k_ra8_ok`: false on every call here (ra8_app_find
 *   only fails on a NULL arg, already guarded), so the true arm is dead defensive
 *   -- unreachable through the public API, the same status the other find/active
 *   forwards in this suite record.
 * - membership guard `idx == none`: true = an unregistered id (99) -> unmounted;
 *   false = a registered id is classified by focus.
 * - focus classifier `idx == reg->active`: true = the active app -> foreground;
 *   false = a registered-but-not-active app -> background.
 */
static void test_app_state(void)
{
  TEST_BEGIN("ra8_app: derived lifecycle state");
  app_ctx_t          c0 = {};
  app_ctx_t          c1 = {};
  ra8_app_t          a0 = make_app(&c0, 1, "lib");
  ra8_app_t          a1 = make_app(&c1, 2, "rdr");
  ra8_app_t*         slots[2];
  ra8_app_registry_t reg = {};
  (void)ra8_app_registry_init(&reg, slots, 2U);
  (void)ra8_app_register(&reg, &a0);
  (void)ra8_app_register(&reg, &a1);

  ra8_app_state_t st = k_ra8_app_state_foreground;
  /* Unregistered id -> unmounted (membership guard true). */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_app_state(&reg, 99, &st));
  TEST_ASSERT_EQ(k_ra8_app_state_unmounted, st);

  /* Registered, no focus yet -> background (classifier false: active == none). */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_app_state(&reg, 1, &st));
  TEST_ASSERT_EQ(k_ra8_app_state_background, st);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_app_state(&reg, 2, &st));
  TEST_ASSERT_EQ(k_ra8_app_state_background, st);

  /* Focus a0 -> a0 foreground (classifier true), a1 still background. */
  (void)ra8_app_launch(&reg, 1);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_app_state(&reg, 1, &st));
  TEST_ASSERT_EQ(k_ra8_app_state_foreground, st);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_app_state(&reg, 2, &st));
  TEST_ASSERT_EQ(k_ra8_app_state_background, st);

  /* Switch to a1 -> a1 foreground, a0 suspended back to background. */
  (void)ra8_app_launch(&reg, 2);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_app_state(&reg, 2, &st));
  TEST_ASSERT_EQ(k_ra8_app_state_foreground, st);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_app_state(&reg, 1, &st));
  TEST_ASSERT_EQ(k_ra8_app_state_background, st);

  /* Null guards. */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_app_state(nullptr, 1, &st));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_app_state(&reg, 1, nullptr));
  TEST_END("ra8_app: derived lifecycle state");
}

/**
 * @brief Drive the core-invariant, busy, compaction and active-fix-up arms.
 * @param[in,out] reg Registry seeded with library/settings/notes/weather.
 * @param[in]     c0  Library context (core, deinit counter).
 * @param[in]     c1  Settings context (removable, deinit counter).
 * @param[in]     c3  Weather context (removable, deinit counter).
 * @pre @p reg holds the four registered apps.
 * @post Every not_found / core / busy / compaction / tail-remove vector held.
 * @note Not thread-safe; single-threaded host-test helper.
 * @since 0.1.0
 */
static void
uninstall_check_core_arms(ra8_app_registry_t* reg, app_ctx_t* c0, app_ctx_t* c1, app_ctx_t* c3)
{
  uint16_t        n   = 0U;
  ra8_app_state_t st  = k_ra8_app_state_unmounted;
  ra8_app_t*      act = nullptr;

  /* not_found: an unknown id (membership guard true). */
  TEST_ASSERT_EQ(k_ra8_err_not_found, ra8_app_uninstall(reg, 99));

  /* Core-uninstallable invariant: library is core -> not_supported, nothing torn
   * down, registry unchanged. */
  TEST_ASSERT_EQ(k_ra8_err_not_supported, ra8_app_uninstall(reg, 1));
  TEST_ASSERT_EQ(0U, c0->deinit_calls);
  (void)ra8_app_count(reg, &n);
  TEST_ASSERT_EQ(4U, n);

  /* Busy: focus settings (removable), then try to uninstall the focused app. The
   * removable guard passes but the focus guard refuses it. */
  (void)ra8_app_launch(reg, 2); /* active = idx 1 (settings) */
  TEST_ASSERT_EQ(k_ra8_err_busy, ra8_app_uninstall(reg, 2));
  TEST_ASSERT_EQ(0U, c1->deinit_calls);
  (void)ra8_app_count(reg, &n);
  TEST_ASSERT_EQ(4U, n);

  /* Focus weather (idx 3), then uninstall settings (idx 1): a removable, NON-
   * focused app sitting *before* the focused one. Exercises the compaction-loop
   * entered arm and the active-fix-up TRUE arm. */
  (void)ra8_app_launch(reg, 4); /* active = idx 3 (weather) */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_app_uninstall(reg, 2));
  TEST_ASSERT_EQ(1U, c1->deinit_calls); /* deinit guard true */
  (void)ra8_app_count(reg, &n);
  TEST_ASSERT_EQ(3U, n); /* compacted: library, notes, weather */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_app_state(reg, 2, &st));
  TEST_ASSERT_EQ(k_ra8_app_state_unmounted, st); /* settings gone */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_app_active(reg, &act));
  TEST_ASSERT_EQ(4, act->id); /* weather kept (active index fixed up) */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_app_state(reg, 4, &st));
  TEST_ASSERT_EQ(k_ra8_app_state_foreground, st);

  /* Switch focus to notes, then uninstall the tail (weather): removing the last
   * slot skips the compaction-loop body, and active (before the removed slot)
   * exercises the active-fix-up FALSE arm. */
  (void)ra8_app_launch(reg, 3); /* active = idx 1 (notes) */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_app_uninstall(reg, 4));
  TEST_ASSERT_EQ(1U, c3->deinit_calls);
  (void)ra8_app_count(reg, &n);
  TEST_ASSERT_EQ(2U, n); /* library, notes */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_app_active(reg, &act));
  TEST_ASSERT_EQ(3, act->id); /* notes unchanged (active before removed) */
}

/**
 * @brief deinit-NULL arm: a removable app with no deinit unmounts cleanly.
 * @param[in,out] reg Registry (post core-arms: library + notes remain).
 * @pre @p reg has room for one more app.
 * @post The no-deinit app uninstalled with no crash and count unchanged.
 * @note Not thread-safe; single-threaded host-test helper.
 * @since 0.1.0
 */
static void uninstall_check_deinit_null(ra8_app_registry_t* reg)
{
  uint16_t  n    = 0U;
  app_ctx_t cnd  = {};
  ra8_app_t andn = make_app_vt(&cnd, k_app_id_null_vtable, "nodeinit", &k_app_vt_null);
  andn.removable = true;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_app_register(reg, &andn)); /* idx 2              */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_app_uninstall(reg, 5));    /* deinit guard false */
  (void)ra8_app_count(reg, &n);
  TEST_ASSERT_EQ(2U, n);
}

/**
 * @brief active == none arm: uninstalling an unfocused removable app.
 * @pre None.
 * @post The active-fix-up comparison took its false (active == none) arm.
 * @note Not thread-safe; single-threaded host-test helper.
 * @since 0.1.0
 */
static void uninstall_check_active_none(void)
{
  uint16_t  n  = 0U;
  app_ctx_t fc = {};
  ra8_app_t fa = make_app(&fc, k_app_id_freed, "free");
  fa.removable = true;
  ra8_app_t*         fslots[1];
  ra8_app_registry_t freg = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_app_registry_init(&freg, fslots, 1U));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_app_register(&freg, &fa));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_app_uninstall(&freg, 7));
  (void)ra8_app_count(&freg, &n);
  TEST_ASSERT_EQ(0U, n);
}

/**
 * @test ra8_app_uninstall: core-uninstallable invariant + removable unmount.
 *
 * @details
 * The run-time half of #146's "core uninstallable" rule. Drives every reachable
 * branch of ra8_app_uninstall: an unknown id is not_found, a core app is refused
 * (not_supported, nothing torn down), the focused app is refused (busy), and a
 * removable background app is unmounted -- its `deinit` fires, it leaves the
 * registry, the table compacts, and a focused index sitting after the removed
 * slot is fixed up so it keeps pointing at the same app. Both the deinit-present
 * and deinit-NULL arms, and both compaction-loop / active-fix-up arms, are
 * covered. After each unmount ::ra8_app_state reports the gone app as unmounted.
 *
 * @par MC/DC:
 * Every decision in ra8_app_uninstall is single-condition (N=1 -> N+1=2 vectors):
 * - find-forward `ferr != k_ra8_ok`: false here (find fails only on a NULL arg,
 *   already guarded) -- the true arm is dead defensive, unreachable via the API.
 * - membership `idx == none`: true = unknown id (99) -> not_found; false = found.
 * - slot guard `app == NULL` (RA8_CHECK_NULL_PTR): defensive -- ra8_app_find never
 *   yields a NULL-slot index, so the true arm is unreachable via the API.
 * - core guard `!app->removable`: true = core (library) -> not_supported;
 *   false = a removable app proceeds.
 * - focus guard `idx == reg->active`: true = the focused app -> busy; false = a
 *   background app proceeds.
 * - deinit guard `deinit != NULL`: true = settings/weather deinit fires;
 *   false = the NULL-deinit app unmounts with no deinit call.
 * - compaction loop `(i + 1) < count`: entered when later slots exist (removing
 *   settings shifts notes + weather down); skipped when removing the tail.
 * - active fix-up `reg->active > idx`: true = focus sat after the removed slot
 *   (active index decremented); false = focus before the slot or no focus at all.
 */
static void test_uninstall(void)
{
  TEST_BEGIN("ra8_app: uninstall core-invariant + removable unmount MC/DC");
  app_ctx_t c0 = {};
  app_ctx_t c1 = {};
  app_ctx_t c2 = {};
  app_ctx_t c3 = {};
  ra8_app_t a0 = make_app(&c0, 1, "library");  /* core: removable stays false */
  ra8_app_t a1 = make_app(&c1, 2, "settings"); /* removable below             */
  ra8_app_t a2 = make_app(&c2, 3, "notes");    /* removable below             */
  ra8_app_t a3 = make_app(&c3, 4, "weather");  /* removable below             */
  a1.removable = true;
  a2.removable = true;
  a3.removable = true;
  ra8_app_t*         slots[4];
  ra8_app_registry_t reg = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_app_registry_init(&reg, slots, 4U));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_app_register(&reg, &a0)); /* idx 0 */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_app_register(&reg, &a1)); /* idx 1 */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_app_register(&reg, &a2)); /* idx 2 */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_app_register(&reg, &a3)); /* idx 3 */

  uninstall_check_core_arms(&reg, &c0, &c1, &c3);
  uninstall_check_deinit_null(&reg);
  uninstall_check_active_none();

  /* Null guard. */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_app_uninstall(nullptr, 1));
  TEST_END("ra8_app: uninstall core-invariant + removable unmount MC/DC");
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
  test_null_slots();
  test_nav_go_index();
  test_zero_cap_and_nav_launch_errors();
  test_app_state();
  test_uninstall();
  return 0;
}
