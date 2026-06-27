/**
 * @file ra_app.h
 * @brief Zero-heap app framework: lifecycle + static registry + launcher (#146).
 *
 * @details
 * `ra_app` makes each major function a first-class **app** with a lifecycle,
 * launched from the chrome (issue #146): opening a book launches the EPUB
 * reader app; the library organizer is an app; settings is an app. The
 * framework owns nothing but the routing -- it calls the active app's
 * lifecycle, forwards input + render, and tracks focus. Each app builds its UI
 * by composing `ra_widget`s (issue #145), so an "app = a widget tree".
 *
 * Zero-heap (NASA Rule 3): apps are static instances registered into a
 * caller-owned pointer array. Nothing is allocated; the registry is a fixed
 * table of `ra_app_t*`.
 *
 * **Build-time exclusion ("core uninstallable").** An app registers itself; a
 * Kconfig-style guard around the registration call (e.g. `#if RA_APP_SETTINGS`)
 * excludes it from the registry so the firmware ships only the apps you want.
 * The framework needs no special support -- an unregistered app is simply
 * absent, and `ra_app_count` / `ra_app_find` never see it.
 *
 * The lifecycle / registration / routing logic is pure (no framebuffer), so it
 * is host-unit-tested; the per-app `render` is the only on-target callback.
 *
 * @par State Machine
 * @startuml
 *  [*] --> Registered : ra_app_register (init once)
 *  Registered --> Foreground : ra_app_launch (on_enter)
 *  Foreground --> Background : ra_app_launch(other) (on_leave)
 *  Background --> Foreground : ra_app_launch(self) (on_enter)
 * @enduml
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * [Ring 5 / UI]
 * {World: NS}
 *
 * @since 0.1.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra_err.h"
#include "ra_widget.h"

/**
 * @enum ra_app_const_t
 * @brief Sentinels for the app registry.
 */
typedef enum : int16_t {
  k_ra_app_none = -1, /**< No active app / not found. */
} ra_app_const_t;

struct ra_app; /* fwd */

/**
 * @struct ra_app_vtable_t
 * @brief App lifecycle callbacks (shared by all instances of one app).
 *
 * @details
 * `init` runs once at registration; `on_enter` / `on_leave` bracket focus;
 * `tick` + `render` run each frame while foreground; `on_input` routes events
 * (returning whether the app consumed them); `deinit` tears down. Any callback
 * except `on_input` may be NULL (a no-op). `render` is the only on-target one.
 */
typedef struct {
  ra_err_t (*init)(struct ra_app* a);     /**< One-time setup (at register).  */
  void (*on_enter)(struct ra_app* a);     /**< Gained focus / foreground.     */
  void (*tick)(struct ra_app* a);         /**< Per-frame update (foreground). */
  void (*render)(const struct ra_app* a); /**< Draw (on-target, foreground).  */
  bool (*on_input)(struct ra_app* a, const ra_widget_event_t* ev); /**< Handle.                  */
  void (*on_leave)(struct ra_app* a);                              /**< Lost focus / background. */
  void (*deinit)(struct ra_app* a);                                /**< Teardown.                */
} ra_app_vtable_t;

/**
 * @struct ra_app_t
 * @brief One app instance + its metadata (caller-owned, static).
 */
typedef struct ra_app {
  const ra_app_vtable_t* vt;          /**< Lifecycle table (non-NULL).     */
  void*                  ctx;         /**< App-specific state.             */
  uint16_t               id;          /**< Unique app id (launch key).     */
  const char*            name;        /**< Display name for the launcher.  */
  bool                   removable;   /**< Optional/uninstallable core?    */
  bool                   initialized; /**< Set true once `init` succeeded. */
} ra_app_t;

/**
 * @struct ra_app_registry_t
 * @brief Fixed table of registered apps + the focused index.
 */
typedef struct {
  ra_app_t** apps;   /**< Caller storage (array of app pointers). */
  uint16_t   cap;    /**< Capacity of `apps`.                     */
  uint16_t   count;  /**< Registered app count.                   */
  int16_t    active; /**< Focused app index, or k_ra_app_none.    */
} ra_app_registry_t;

/* ===========================================================================
 * Registry lifecycle
 * ===========================================================================
 */

/**
 * @brief Bind a registry to caller-owned pointer storage (empty, no focus).
 *
 * @param[out] reg     Registry to initialise.
 * @param[in]  storage Array of `ra_app_t*` the registry fills.
 * @param[in]  cap     Capacity of `storage` (>= 1).
 *
 * @return ra_err_t
 * @retval k_ra_ok              Initialised.
 * @retval k_ra_err_null_ptr    `reg` or `storage` is NULL.
 * @retval k_ra_err_invalid_arg `cap` is 0.
 *
 * @pre `reg` and `storage` non-NULL; `cap >= 1`.
 * @post `reg->count == 0` and `reg->active == k_ra_app_none`.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t
ra_app_registry_init(ra_app_registry_t* reg, ra_app_t** storage, uint16_t cap);

/**
 * @brief Register an app (calls its `init` once) and add it to the registry.
 *
 * @details
 * Rejects a duplicate `id`. On success the app's `init` (if any) runs; a
 * non-ok `init` leaves the app **unregistered** and returns its error. This is
 * the point a build-time guard (`#if RA_APP_X`) wraps to exclude an app.
 *
 * @param[in,out] reg Registry.
 * @param[in]     app App instance (non-NULL, `vt` non-NULL).
 *
 * @return ra_err_t
 * @retval k_ra_ok                Registered + initialised.
 * @retval k_ra_err_null_ptr      `reg` or `app` (or `app->vt`) is NULL.
 * @retval k_ra_err_no_mem        Registry already at `cap`.
 * @retval k_ra_err_conflict      An app with the same `id` is registered.
 * @retval <init's error>         `app->vt->init` returned non-ok.
 *
 * @pre `app->vt` non-NULL.
 * @post On success `reg->count` grew by one and `app->initialized == true`.
 * @post On any failure the registry is unchanged.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_app_register(ra_app_registry_t* reg, ra_app_t* app);

/**
 * @brief Find a registered app's index by id.
 *
 * @param[in]  reg     Registry.
 * @param[in]  id      App id to find.
 * @param[out] out_idx Receives the index in `[0, count)`, or k_ra_app_none.
 *
 * @return ra_err_t
 * @retval k_ra_ok           Search done (see `*out_idx`).
 * @retval k_ra_err_null_ptr `reg` or `out_idx` is NULL.
 *
 * @pre `reg` and `out_idx` non-NULL.
 * @post `*out_idx == k_ra_app_none` iff no app has `id`.
 *
 * @note Pure; not thread-safe vs concurrent mutation.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_app_find(const ra_app_registry_t* reg, uint16_t id, int16_t* out_idx);

/* ===========================================================================
 * Focus + routing
 * ===========================================================================
 */

/**
 * @brief Launch (focus) the app with `id`, running the focus lifecycle.
 *
 * @details
 * If a different app is focused, its `on_leave` runs first; then the target
 * becomes active and its `on_enter` runs. Launching the already-active app is
 * an idempotent no-op (no lifecycle calls) so a re-tap on the current app does
 * not flicker.
 *
 * @param[in,out] reg Registry.
 * @param[in]     id  App id to focus.
 *
 * @return ra_err_t
 * @retval k_ra_ok            Focused (or already focused).
 * @retval k_ra_err_null_ptr  `reg` is NULL.
 * @retval k_ra_err_not_found No app has `id`.
 *
 * @pre `reg` non-NULL.
 * @post On success `reg->active` indexes the app whose id is `id`.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_app_launch(ra_app_registry_t* reg, uint16_t id);

/**
 * @brief Get the currently focused app.
 *
 * @param[in]  reg     Registry.
 * @param[out] out_app Receives the active app pointer, or NULL if none.
 *
 * @return ra_err_t
 * @retval k_ra_ok           Reported (see `*out_app`).
 * @retval k_ra_err_null_ptr `reg` or `out_app` is NULL.
 *
 * @pre `reg` and `out_app` non-NULL.
 * @post `*out_app == NULL` iff no app is focused.
 *
 * @note Pure read; not thread-safe vs concurrent mutation.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_app_active(const ra_app_registry_t* reg, ra_app_t** out_app);

/**
 * @brief Route an input event to the focused app.
 *
 * @param[in,out] reg         Registry.
 * @param[in]     ev          The event.
 * @param[out]    out_handled Receives true if the active app consumed it.
 *
 * @return ra_err_t
 * @retval k_ra_ok           Routed (see `*out_handled`); false if no focus.
 * @retval k_ra_err_null_ptr `reg`, `ev`, or `out_handled` is NULL.
 *
 * @pre `ev` and `out_handled` non-NULL.
 * @post `*out_handled` is the active app's `on_input` result, or false.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t
ra_app_route_input(ra_app_registry_t* reg, const ra_widget_event_t* ev, bool* out_handled);

/**
 * @brief Run the focused app's per-frame `tick` (no-op if none / NULL).
 *
 * @param[in,out] reg Registry.
 * @return ra_err_t k_ra_ok, or k_ra_err_null_ptr if `reg` is NULL.
 * @pre `reg` non-NULL.
 * @post The active app's `tick` ran (if any).
 * @post No app state other than what `tick` itself mutates is touched.
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_app_tick(ra_app_registry_t* reg);

/**
 * @brief Run the focused app's `render` (on-target; no-op if none / NULL).
 *
 * @param[in,out] reg Registry.
 * @return ra_err_t k_ra_ok, or k_ra_err_null_ptr if `reg` is NULL.
 * @pre `reg` non-NULL.
 * @post The active app's `render` ran (if any).
 * @post The registry's focus / membership state is unchanged.
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_app_render(ra_app_registry_t* reg);

/**
 * @brief Number of registered apps (for the launcher to list).
 *
 * @param[in]  reg       Registry.
 * @param[out] out_count Receives `reg->count`.
 * @return ra_err_t k_ra_ok, or k_ra_err_null_ptr.
 * @pre `reg` and `out_count` non-NULL.
 * @post `*out_count == reg->count`.
 * @note Pure read.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_app_count(const ra_app_registry_t* reg, uint16_t* out_count);

/**
 * @brief Get the app at registry index `idx` (launcher enumeration).
 *
 * @param[in]  reg     Registry.
 * @param[in]  idx     Index in `[0, count)`.
 * @param[out] out_app Receives the app pointer.
 * @return ra_err_t
 * @retval k_ra_ok              Reported.
 * @retval k_ra_err_null_ptr    `reg` or `out_app` is NULL.
 * @retval k_ra_err_out_of_range `idx >= count`.
 * @pre `reg` and `out_app` non-NULL.
 * @post On success `*out_app` is `reg->apps[idx]`.
 * @note Pure read.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_app_at(const ra_app_registry_t* reg, uint16_t idx, ra_app_t** out_app);

/* ===========================================================================
 * Navigation back-stack (history of focused apps)
 * ===========================================================================
 */

/**
 * @struct ra_app_nav_t
 * @brief A bounded back-stack of app ids layered over a registry (zero-heap).
 *
 * @details
 * The registry tracks only the *currently* focused app. A chrome/shell that
 * lets the user drill in (library -> reader -> settings) and then press "back"
 * needs the *trail* it came from. `ra_app_nav_t` is that trail: ::ra_app_nav_go
 * focuses an app and remembers the one it left, and ::ra_app_nav_back returns
 * to the most recently remembered app. The history is a caller-owned `uint16_t`
 * array of app ids -- nothing is allocated (NASA Rule 3). The registry's own
 * focus lifecycle (`on_leave` -> `on_enter`) still fires under every move, so a
 * navigation layer composes ::ra_app_launch rather than reimplementing it.
 *
 * @invariant `depth <= cap`.
 * @invariant `stack[0 .. depth)` hold app ids the user can go back through,
 *            oldest at index 0, most recent at `depth - 1`.
 *
 * @par Example:
 * @code
 * ra_app_nav_t nav;
 * uint16_t     trail[4];
 * ra_app_nav_init(&nav, &reg, trail, 4U);
 * ra_app_nav_go(&nav, k_app_reader);   // focus reader (trail empty)
 * ra_app_nav_go(&nav, k_app_settings); // focus settings (reader pushed)
 * bool popped = false;
 * ra_app_nav_back(&nav, &popped);      // back to reader (trail empty again)
 * @endcode
 *
 * @see ra_app_launch  The lower-level focus switch this layer composes.
 * @since 0.1.0
 */
typedef struct {
  ra_app_registry_t* reg;   /**< Registry the navigation drives (non-NULL). */
  uint16_t*          stack; /**< Caller storage: back-stack of app ids.     */
  uint16_t           cap;   /**< Capacity of `stack`.                       */
  uint16_t           depth; /**< Number of ids currently on the back-stack. */
} ra_app_nav_t;

/**
 * @brief Bind a navigation back-stack to a registry + caller-owned storage.
 *
 * @param[out] nav     Navigation state to initialise.
 * @param[in]  reg     Registry the navigation focuses apps in (non-NULL).
 * @param[in]  storage Array of `uint16_t` the back-stack fills with app ids.
 * @param[in]  cap     Capacity of `storage` (>= 1).
 *
 * @return ra_err_t
 * @retval k_ra_ok              Initialised.
 * @retval k_ra_err_null_ptr    `nav`, `reg`, or `storage` is NULL.
 * @retval k_ra_err_invalid_arg `cap` is 0.
 *
 * @pre `nav`, `reg`, and `storage` non-NULL; `cap >= 1`.
 * @post `nav->depth == 0` (empty trail).
 * @post `nav->reg == reg` and `nav->cap == cap`.
 *
 * @note Not thread-safe.
 * @see ra_app_nav_go
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t
ra_app_nav_init(ra_app_nav_t* nav, ra_app_registry_t* reg, uint16_t* storage, uint16_t cap);

/**
 * @brief Focus app `id`, pushing the outgoing app onto the back-stack.
 *
 * @details
 * Composes ::ra_app_launch (so the focus lifecycle fires) and remembers the
 * app that lost focus so ::ra_app_nav_back can return to it. The outgoing app
 * is pushed only when a *different* app was focused: launching the already
 * focused app, or the very first launch (no prior focus), pushes nothing -- the
 * trail records real navigation, not idempotent re-taps. Capacity is checked
 * before the launch so a full back-stack leaves the focus unchanged.
 *
 * @param[in,out] nav Navigation state.
 * @param[in]     id  App id to focus.
 *
 * @return ra_err_t
 * @retval k_ra_ok            Focused (and pushed if a different app left).
 * @retval k_ra_err_null_ptr  `nav` or `nav->reg` is NULL.
 * @retval k_ra_err_no_mem    A push was required but the back-stack is full.
 * @retval k_ra_err_not_found No app has `id` (focus + trail unchanged).
 *
 * @pre `nav` and `nav->reg` non-NULL.
 * @post On success the registry's active app is the one whose id is `id`.
 * @post `nav->depth` grew by one iff a different app was previously focused.
 *
 * @note Not thread-safe.
 * @see ra_app_nav_back
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_app_nav_go(ra_app_nav_t* nav, uint16_t id);

/**
 * @brief Return to the most recently pushed app (pop the back-stack).
 *
 * @details
 * Pops the top of the back-stack and focuses that app through ::ra_app_launch,
 * firing the focus lifecycle. An empty back-stack is not an error -- it reports
 * `*out_popped == false` so a chrome can decide what "back" means at the root
 * (exit, no-op, etc.). The pop is committed only after the launch succeeds.
 *
 * @param[in,out] nav        Navigation state.
 * @param[out]    out_popped Receives true if an app was popped + focused.
 *
 * @return ra_err_t
 * @retval k_ra_ok            Done (see @p out_popped); false at the root.
 * @retval k_ra_err_null_ptr  `nav`, `nav->reg`, or `out_popped` is NULL.
 * @retval k_ra_err_not_found The popped id is no longer registered.
 *
 * @pre `nav`, `nav->reg`, and `out_popped` non-NULL.
 * @post On `*out_popped == true` the registry's active app is the popped id and
 *       `nav->depth` shrank by one; otherwise both are unchanged.
 *
 * @note Not thread-safe.
 * @see ra_app_nav_go
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_app_nav_back(ra_app_nav_t* nav, bool* out_popped);

/**
 * @brief Current back-stack depth (apps the user can still go back through).
 *
 * @param[in]  nav       Navigation state.
 * @param[out] out_depth Receives `nav->depth`.
 *
 * @return ra_err_t
 * @retval k_ra_ok           Reported.
 * @retval k_ra_err_null_ptr `nav` or `out_depth` is NULL.
 *
 * @pre `nav` and `out_depth` non-NULL.
 * @post `*out_depth == nav->depth`.
 * @post No navigation state is modified.
 *
 * @note Pure read; not thread-safe vs concurrent mutation.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_app_nav_depth(const ra_app_nav_t* nav, uint16_t* out_depth);

#ifdef __cplusplus
}
#endif
