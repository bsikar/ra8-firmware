/**
 * @file ra8_app.h
 * @brief Zero-heap app framework: lifecycle + static registry + launcher (#146).
 * @ingroup grp_board
 *
 * @details
 * `ra8_app` makes each major function a first-class **app** with a lifecycle,
 * launched from the chrome (issue #146): opening a book launches the EPUB
 * reader app; the library organizer is an app; settings is an app. The
 * framework owns nothing but the routing -- it calls the active app's
 * lifecycle, forwards input + render, and tracks focus. Each app builds its UI
 * by composing `ra8_widget`s (issue #145), so an "app = a widget tree".
 *
 * Zero-heap (NASA Rule 3): apps are static instances registered into a
 * caller-owned pointer array. Nothing is allocated; the registry is a fixed
 * table of `ra8_app_t*`.
 *
 * **Core uninstallable.** Issue #146's headline rule -- "core functionality
 * should be able to be uninstalled" -- is enforced two ways:
 *   - **Build time:** a Kconfig-style guard around the registration call (e.g.
 *     `#if RA8_APP_SETTINGS`) excludes an app from the registry, so the firmware
 *     ships only the apps you want. The framework needs no special support: an
 *     unregistered app is simply absent, and `ra8_app_count` / `ra8_app_find`
 *     never see it.
 *   - **Run time:** ::ra8_app_uninstall **unmounts** a *removable* app (its
 *     `deinit` runs and it leaves the registry) but **refuses** a core app
 *     (`removable == false`). The `removable` flag thereby has teeth: a launcher
 *     offers "remove" only on removable apps, and the framework guarantees a
 *     core app can never be torn down at run time.
 *
 * The lifecycle / registration / routing logic is pure (no framebuffer), so it
 * is host-unit-tested; the per-app `render` is the only on-target callback.
 * ::ra8_app_state reports each app's place in the state machine below, *derived*
 * from the registry so it can never drift from the real focus / membership.
 *
 * @par State Machine
 * @dot
 * digraph ra8_app_states {
 *   bgcolor="transparent";
 *   rankdir=LR;
 *   node [shape=box, style="rounded,filled", fontname="Helvetica", fontsize=10,
 *         fillcolor="#e8eef7", color="#5a7ca6"];
 *   edge [fontname="Helvetica", fontsize=9, color="#5a7ca6"];
 *
 *   __start [shape=circle, width=0.18, label="", fillcolor="#5a7ca6", color="#5a7ca6"];
 *
 *   Unmounted [label="Unmounted"];
 *   Background [label="Background"];
 *   Foreground [label="Foreground"];
 *
 *   __start -> Unmounted;
 *   Unmounted -> Background [label="ra8_app_register (mount /\\ninit)"];
 *   Background -> Foreground [label="ra8_app_launch (focus /\\non_enter)"];
 *   Foreground -> Background [label="ra8_app_launch(other)\\n(suspend / on_leave)"];
 *   Background -> Unmounted [label="ra8_app_uninstall (unmount /\\ndeinit)"];
 * }
 * @enddot
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

#include "ra8_err.h"
#include "ra8_widget.h"

/**
 * @enum ra8_app_const_t
 * @brief Sentinels for the app registry.
 */
typedef enum : int16_t {
  k_ra8_app_none = -1, /**< No active app / not found. */
} ra8_app_const_t;

/**
 * @enum ra8_app_state_t
 * @brief Lifecycle state of one app within the framework's state machine.
 *
 * @details
 * Every app moves through a small, explicit state machine the framework drives:
 * an unregistered app is **mounted** by ::ra8_app_register (its `init` runs),
 * entering ::k_ra8_app_state_background; ::ra8_app_launch **focuses** it into
 * ::k_ra8_app_state_foreground (and **suspends** the outgoing app back to
 * ::k_ra8_app_state_background); ::ra8_app_uninstall **unmounts** a background app
 * (its `deinit` runs) back to ::k_ra8_app_state_unmounted. The state is not a
 * stored field: ::ra8_app_state derives it from the registry (membership +
 * focused index), so the single source of truth is the registry and the
 * reported state can never drift from the real focus / membership.
 *
 * @invariant At most one app is ::k_ra8_app_state_foreground at a time (the
 *            registry's `active` index).
 *
 * @par State table:
 * | Reported state              | Condition                                |
 * |-----------------------------|------------------------------------------|
 * | ::k_ra8_app_state_unmounted  | id not registered (or after uninstall)   |
 * | ::k_ra8_app_state_background  | registered, not the focused app          |
 * | ::k_ra8_app_state_foreground | registered and the focused (active) app  |
 *
 * @see ra8_app_state
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_ra8_app_state_unmounted  = 0U, /**< Not registered (initial / after uninstall). */
  k_ra8_app_state_background = 1U, /**< Mounted (init ran), not focused.            */
  k_ra8_app_state_foreground = 2U, /**< Mounted and focused (the active app).       */
} ra8_app_state_t;

struct ra8_app; /* fwd */

/**
 * @struct ra8_app_vtable_t
 * @brief App lifecycle callbacks (shared by all instances of one app).
 *
 * @details
 * `init` runs once at registration; `on_enter` / `on_leave` bracket focus;
 * `tick` + `render` run each frame while foreground; `on_input` routes events
 * (returning whether the app consumed them); `deinit` tears down. Any callback
 * except `on_input` may be NULL (a no-op). `render` is the only on-target one.
 */
typedef struct {
  ra8_err_t (*init)(struct ra8_app* a);    /**< One-time setup (at register).  */
  void (*on_enter)(struct ra8_app* a);     /**< Gained focus / foreground.     */
  void (*tick)(struct ra8_app* a);         /**< Per-frame update (foreground). */
  void (*render)(const struct ra8_app* a); /**< Draw (on-target, foreground).  */
  bool (*on_input)(struct ra8_app*           a,
                   const ra8_widget_event_t* ev); /**< Handle.                  */
  void (*on_leave)(struct ra8_app* a);            /**< Lost focus / background. */
  void (*deinit)(struct ra8_app* a);              /**< Teardown.                */
} ra8_app_vtable_t;

/**
 * @struct ra8_app_t
 * @brief One app instance + its metadata (caller-owned, static).
 */
typedef struct ra8_app {
  const ra8_app_vtable_t* vt;          /**< Lifecycle table (non-NULL).     */
  void*                   ctx;         /**< App-specific state.             */
  uint16_t                id;          /**< Unique app id (launch key).     */
  const char*             name;        /**< Display name for the launcher.  */
  bool                    removable;   /**< Optional/uninstallable core?    */
  bool                    initialized; /**< Set true once `init` succeeded. */
} ra8_app_t;

/**
 * @struct ra8_app_registry_t
 * @brief Fixed table of registered apps + the focused index.
 */
typedef struct {
  ra8_app_t** apps;   /**< Caller storage (array of app pointers). */
  uint16_t    cap;    /**< Capacity of `apps`.                     */
  uint16_t    count;  /**< Registered app count.                   */
  int16_t     active; /**< Focused app index, or k_ra8_app_none.   */
} ra8_app_registry_t;

/* ===========================================================================
 * Registry lifecycle
 * ===========================================================================
 */

/**
 * @brief Bind a registry to caller-owned pointer storage (empty, no focus).
 *
 * @param[out] reg     Registry to initialise.
 * @param[in]  storage Array of `ra8_app_t*` the registry fills.
 * @param[in]  cap     Capacity of `storage` (>= 1).
 *
 * @return ra8_err_t
 * @retval k_ra8_ok              Initialised.
 * @retval k_ra8_err_null_ptr    `reg` or `storage` is NULL.
 * @retval k_ra8_err_invalid_arg `cap` is 0.
 *
 * @pre `reg` and `storage` non-NULL; `cap >= 1`.
 * @post `reg->count == 0` and `reg->active == k_ra8_app_none`.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_app_registry_init(ra8_app_registry_t* reg, ra8_app_t** storage, uint16_t cap);

/**
 * @brief Register an app (calls its `init` once) and add it to the registry.
 *
 * @details
 * Rejects a duplicate `id`. On success the app's `init` (if any) runs; a
 * non-ok `init` leaves the app **unregistered** and returns its error. This is
 * the point a build-time guard (`#if RA8_APP_X`) wraps to exclude an app.
 *
 * @param[in,out] reg Registry.
 * @param[in]     app App instance (non-NULL, `vt` non-NULL).
 *
 * @return ra8_err_t
 * @retval k_ra8_ok                Registered + initialised.
 * @retval k_ra8_err_null_ptr      `reg` or `app` (or `app->vt`) is NULL.
 * @retval k_ra8_err_no_mem        Registry already at `cap`.
 * @retval k_ra8_err_conflict      An app with the same `id` is registered.
 * @retval <init's error>         `app->vt->init` returned non-ok.
 *
 * @pre `app->vt` non-NULL.
 * @post On success `reg->count` grew by one and `app->initialized == true`.
 * @post On any failure the registry is unchanged.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_app_register(ra8_app_registry_t* reg, ra8_app_t* app);

/**
 * @brief Find a registered app's index by id.
 *
 * @param[in]  reg     Registry.
 * @param[in]  id      App id to find.
 * @param[out] out_idx Receives the index in `[0, count)`, or k_ra8_app_none.
 *
 * @return ra8_err_t
 * @retval k_ra8_ok           Search done (see `*out_idx`).
 * @retval k_ra8_err_null_ptr `reg` or `out_idx` is NULL.
 *
 * @pre `reg` and `out_idx` non-NULL.
 * @post `*out_idx == k_ra8_app_none` iff no app has `id`.
 *
 * @note Pure; not thread-safe vs concurrent mutation.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_app_find(const ra8_app_registry_t* reg, uint16_t id, int16_t* out_idx);

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
 * @return ra8_err_t
 * @retval k_ra8_ok            Focused (or already focused).
 * @retval k_ra8_err_null_ptr  `reg` is NULL.
 * @retval k_ra8_err_not_found No app has `id`.
 *
 * @pre `reg` non-NULL.
 * @post On success `reg->active` indexes the app whose id is `id`.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_app_launch(ra8_app_registry_t* reg, uint16_t id);

/**
 * @brief Get the currently focused app.
 *
 * @param[in]  reg     Registry.
 * @param[out] out_app Receives the active app pointer, or NULL if none.
 *
 * @return ra8_err_t
 * @retval k_ra8_ok           Reported (see `*out_app`).
 * @retval k_ra8_err_null_ptr `reg` or `out_app` is NULL.
 *
 * @pre `reg` and `out_app` non-NULL.
 * @post `*out_app == NULL` iff no app is focused.
 *
 * @note Pure read; not thread-safe vs concurrent mutation.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_app_active(const ra8_app_registry_t* reg, ra8_app_t** out_app);

/**
 * @brief Route an input event to the focused app.
 *
 * @param[in,out] reg         Registry.
 * @param[in]     ev          The event.
 * @param[out]    out_handled Receives true if the active app consumed it.
 *
 * @return ra8_err_t
 * @retval k_ra8_ok           Routed (see `*out_handled`); false if no focus.
 * @retval k_ra8_err_null_ptr `reg`, `ev`, or `out_handled` is NULL.
 *
 * @pre `ev` and `out_handled` non-NULL.
 * @post `*out_handled` is the active app's `on_input` result, or false.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_app_route_input(ra8_app_registry_t* reg, const ra8_widget_event_t* ev, bool* out_handled);

/**
 * @brief Run the focused app's per-frame `tick` (no-op if none / NULL).
 *
 * @param[in,out] reg Registry.
 * @return ra8_err_t k_ra8_ok, or k_ra8_err_null_ptr if `reg` is NULL.
 * @pre `reg` non-NULL.
 * @post The active app's `tick` ran (if any).
 * @post No app state other than what `tick` itself mutates is touched.
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_app_tick(ra8_app_registry_t* reg);

/**
 * @brief Run the focused app's `render` (on-target; no-op if none / NULL).
 *
 * @param[in,out] reg Registry.
 * @return ra8_err_t k_ra8_ok, or k_ra8_err_null_ptr if `reg` is NULL.
 * @pre `reg` non-NULL.
 * @post The active app's `render` ran (if any).
 * @post The registry's focus / membership state is unchanged.
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_app_render(ra8_app_registry_t* reg);

/**
 * @brief Number of registered apps (for the launcher to list).
 *
 * @param[in]  reg       Registry.
 * @param[out] out_count Receives `reg->count`.
 * @return ra8_err_t k_ra8_ok, or k_ra8_err_null_ptr.
 * @pre `reg` and `out_count` non-NULL.
 * @post `*out_count == reg->count`.
 * @note Pure read.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_app_count(const ra8_app_registry_t* reg, uint16_t* out_count);

/**
 * @brief Get the app at registry index `idx` (launcher enumeration).
 *
 * @param[in]  reg     Registry.
 * @param[in]  idx     Index in `[0, count)`.
 * @param[out] out_app Receives the app pointer.
 * @return ra8_err_t
 * @retval k_ra8_ok              Reported.
 * @retval k_ra8_err_null_ptr    `reg` or `out_app` is NULL.
 * @retval k_ra8_err_out_of_range `idx >= count`.
 * @pre `reg` and `out_app` non-NULL.
 * @post On success `*out_app` is `reg->apps[idx]`.
 * @note Pure read.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_app_at(const ra8_app_registry_t* reg, uint16_t idx, ra8_app_t** out_app);

/* ===========================================================================
 * Install / uninstall + lifecycle state
 * ===========================================================================
 */

/**
 * @brief Report an app's lifecycle state, derived from the registry.
 *
 * @details
 * Maps @p id to a ::ra8_app_state_t: an id that is not registered reports
 * ::k_ra8_app_state_unmounted; a registered app reports ::k_ra8_app_state_foreground
 * when it is the registry's focused (active) app and ::k_ra8_app_state_background
 * otherwise. Nothing is stored per app -- the state is computed from membership
 * plus the focused index, so it cannot disagree with ::ra8_app_active or
 * ::ra8_app_find. A launcher uses this to draw the focused tile differently and to
 * decide whether a "remove" affordance applies (only background apps can be
 * uninstalled).
 *
 * @param[in]  reg       Registry.
 * @param[in]  id        App id to query.
 * @param[out] out_state Receives the derived ::ra8_app_state_t.
 *
 * @return ra8_err_t
 * @retval k_ra8_ok           Reported (see @p out_state).
 * @retval k_ra8_err_null_ptr `reg` or `out_state` is NULL.
 *
 * @pre `reg` and `out_state` non-NULL.
 * @post `*out_state == k_ra8_app_state_unmounted` iff no app has `id`.
 * @post `*out_state == k_ra8_app_state_foreground` iff `id` is the active app.
 *
 * @note Pure read; not thread-safe vs concurrent mutation.
 * @see ra8_app_launch
 * @see ra8_app_uninstall
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_app_state(const ra8_app_registry_t* reg, uint16_t id, ra8_app_state_t* out_state);

/**
 * @brief Uninstall (unmount) a removable, non-focused app from the registry.
 *
 * @details
 * The run-time half of #146's "core uninstallable" rule. Resolves @p id and:
 *   - **refuses a core app** (`removable == false`) with k_ra8_err_not_supported
 *     -- the framework guarantees a core app can never be torn down at run time;
 *   - **refuses the focused app** with k_ra8_err_busy -- the chrome must navigate
 *     away first, so the active-app and back-stack invariants stay intact;
 *   - otherwise **unmounts** it: runs its `deinit` (if any), then removes it from
 *     the registry table by compacting the later slots down one place and
 *     decrementing `count`. If the focused app sat *after* the removed slot, its
 *     `active` index is adjusted so it keeps pointing at the same app.
 *
 * After a successful uninstall the app is absent (::ra8_app_find / ::ra8_app_count
 * no longer see it) and ::ra8_app_state reports ::k_ra8_app_state_unmounted for its
 * id. The app instance itself is caller-owned static storage and is left intact
 * apart from `initialized` being cleared, so it may be re-registered later.
 *
 * @warning If the uninstalled app's id still sits on an ::ra8_app_nav_t back-stack,
 *          a later ::ra8_app_nav_back to it forwards k_ra8_err_not_found (the id no
 *          longer resolves); the chrome should not uninstall an app it can still
 *          navigate back to.
 *
 * @param[in,out] reg Registry.
 * @param[in]     id  App id to uninstall.
 *
 * @return ra8_err_t
 * @retval k_ra8_ok               Unmounted; the app left the registry.
 * @retval k_ra8_err_null_ptr     `reg` (or the resolved registry slot) is NULL.
 * @retval k_ra8_err_not_found    No app has `id`.
 * @retval k_ra8_err_not_supported `id` is a core app (`removable == false`).
 * @retval k_ra8_err_busy         `id` is the focused app (navigate away first).
 *
 * @pre `reg` non-NULL.
 * @post On k_ra8_ok `reg->count` shrank by one and `id` is no longer registered.
 * @post On any failure the registry is unchanged.
 *
 * @note Not thread-safe.
 * @see ra8_app_register
 * @see ra8_app_state
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_app_uninstall(ra8_app_registry_t* reg, uint16_t id);

/* ===========================================================================
 * Navigation back-stack (history of focused apps)
 * ===========================================================================
 */

/**
 * @struct ra8_app_nav_t
 * @brief A bounded back-stack of app ids layered over a registry (zero-heap).
 *
 * @details
 * The registry tracks only the *currently* focused app. A chrome/shell that
 * lets the user drill in (library -> reader -> settings) and then press "back"
 * needs the *trail* it came from. `ra8_app_nav_t` is that trail: ::ra8_app_nav_go
 * focuses an app and remembers the one it left, and ::ra8_app_nav_back returns
 * to the most recently remembered app. The history is a caller-owned `uint16_t`
 * array of app ids -- nothing is allocated (NASA Rule 3). The registry's own
 * focus lifecycle (`on_leave` -> `on_enter`) still fires under every move, so a
 * navigation layer composes ::ra8_app_launch rather than reimplementing it.
 *
 * @invariant `depth <= cap`.
 * @invariant `stack[0 .. depth)` hold app ids the user can go back through,
 *            oldest at index 0, most recent at `depth - 1`.
 *
 * @par Example:
 * @code
 * ra8_app_nav_t nav;
 * uint16_t     trail[4];
 * ra8_app_nav_init(&nav, &reg, trail, 4U);
 * ra8_app_nav_go(&nav, k_app_reader);   // focus reader (trail empty)
 * ra8_app_nav_go(&nav, k_app_settings); // focus settings (reader pushed)
 * bool popped = false;
 * ra8_app_nav_back(&nav, &popped);      // back to reader (trail empty again)
 * @endcode
 *
 * @see ra8_app_launch  The lower-level focus switch this layer composes.
 * @since 0.1.0
 */
typedef struct {
  ra8_app_registry_t* reg;   /**< Registry the navigation drives (non-NULL). */
  uint16_t*           stack; /**< Caller storage: back-stack of app ids.     */
  uint16_t            cap;   /**< Capacity of `stack`.                       */
  uint16_t            depth; /**< Number of ids currently on the back-stack. */
} ra8_app_nav_t;

/**
 * @brief Bind a navigation back-stack to a registry + caller-owned storage.
 *
 * @param[out] nav     Navigation state to initialise.
 * @param[in]  reg     Registry the navigation focuses apps in (non-NULL).
 * @param[in]  storage Array of `uint16_t` the back-stack fills with app ids.
 * @param[in]  cap     Capacity of `storage` (>= 1).
 *
 * @return ra8_err_t
 * @retval k_ra8_ok              Initialised.
 * @retval k_ra8_err_null_ptr    `nav`, `reg`, or `storage` is NULL.
 * @retval k_ra8_err_invalid_arg `cap` is 0.
 *
 * @pre `nav`, `reg`, and `storage` non-NULL; `cap >= 1`.
 * @post `nav->depth == 0` (empty trail).
 * @post `nav->reg == reg` and `nav->cap == cap`.
 *
 * @note Not thread-safe.
 * @see ra8_app_nav_go
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_app_nav_init(ra8_app_nav_t* nav, ra8_app_registry_t* reg, uint16_t* storage, uint16_t cap);

/**
 * @brief Focus app `id`, pushing the outgoing app onto the back-stack.
 *
 * @details
 * Composes ::ra8_app_launch (so the focus lifecycle fires) and remembers the
 * app that lost focus so ::ra8_app_nav_back can return to it. The outgoing app
 * is pushed only when a *different* app was focused: launching the already
 * focused app, or the very first launch (no prior focus), pushes nothing -- the
 * trail records real navigation, not idempotent re-taps. Capacity is checked
 * before the launch so a full back-stack leaves the focus unchanged.
 *
 * @param[in,out] nav Navigation state.
 * @param[in]     id  App id to focus.
 *
 * @return ra8_err_t
 * @retval k_ra8_ok            Focused (and pushed if a different app left).
 * @retval k_ra8_err_null_ptr  `nav` or `nav->reg` is NULL.
 * @retval k_ra8_err_no_mem    A push was required but the back-stack is full.
 * @retval k_ra8_err_not_found No app has `id` (focus + trail unchanged).
 *
 * @pre `nav` and `nav->reg` non-NULL.
 * @post On success the registry's active app is the one whose id is `id`.
 * @post `nav->depth` grew by one iff a different app was previously focused.
 *
 * @note Not thread-safe.
 * @see ra8_app_nav_back
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_app_nav_go(ra8_app_nav_t* nav, uint16_t id);

/**
 * @brief Focus the app at registry index @p idx (launcher select-by-position).
 *
 * @details
 * A home/launcher (the chrome) enumerates apps by *position* -- ::ra8_app_count
 * gives the tile count and ::ra8_app_at maps a tile back to its app -- then lets
 * the user pick the n-th tile. This is the bridge that resolves that position to
 * the app's id and focuses it through ::ra8_app_nav_go, so the outgoing app is
 * pushed onto the back-stack exactly as a by-id navigation would. It is the
 * by-index sibling of ::ra8_app_nav_go: the launcher thinks in indices, the
 * back-stack records ids, and this couples the two. A NULL slot at @p idx (a
 * caller-cleared storage entry) is rejected rather than focused.
 *
 * @param[in,out] nav Navigation state.
 * @param[in]     idx Registry index in `[0, count)` of the app to focus.
 *
 * @return ra8_err_t
 * @retval k_ra8_ok               Focused (and pushed if a different app left).
 * @retval k_ra8_err_null_ptr     `nav`, `nav->reg`, or the slot at `idx` is NULL.
 * @retval k_ra8_err_out_of_range `idx >= count`.
 * @retval k_ra8_err_no_mem       A push was required but the back-stack is full.
 * @retval k_ra8_err_not_found    The resolved id is no longer registered.
 *
 * @pre `nav` is non-NULL.
 * @pre `nav->reg` is non-NULL (the navigation is bound to a registry).
 * @post On success the registry's active app is `reg->apps[idx]`.
 * @post `nav->depth` grew by one iff a different app was previously focused.
 *
 * @note Not thread-safe.
 * @see ra8_app_nav_go  The by-id navigation this composes.
 * @see ra8_app_at      The index enumeration a launcher pairs with this.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_app_nav_go_index(ra8_app_nav_t* nav, uint16_t idx);

/**
 * @brief Return to the most recently pushed app (pop the back-stack).
 *
 * @details
 * Pops the top of the back-stack and focuses that app through ::ra8_app_launch,
 * firing the focus lifecycle. An empty back-stack is not an error -- it reports
 * `*out_popped == false` so a chrome can decide what "back" means at the root
 * (exit, no-op, etc.). The pop is committed only after the launch succeeds.
 *
 * @param[in,out] nav        Navigation state.
 * @param[out]    out_popped Receives true if an app was popped + focused.
 *
 * @return ra8_err_t
 * @retval k_ra8_ok            Done (see @p out_popped); false at the root.
 * @retval k_ra8_err_null_ptr  `nav`, `nav->reg`, or `out_popped` is NULL.
 * @retval k_ra8_err_not_found The popped id is no longer registered.
 *
 * @pre `nav`, `nav->reg`, and `out_popped` non-NULL.
 * @post On `*out_popped == true` the registry's active app is the popped id and
 *       `nav->depth` shrank by one; otherwise both are unchanged.
 *
 * @note Not thread-safe.
 * @see ra8_app_nav_go
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_app_nav_back(ra8_app_nav_t* nav, bool* out_popped);

/**
 * @brief Current back-stack depth (apps the user can still go back through).
 *
 * @param[in]  nav       Navigation state.
 * @param[out] out_depth Receives `nav->depth`.
 *
 * @return ra8_err_t
 * @retval k_ra8_ok           Reported.
 * @retval k_ra8_err_null_ptr `nav` or `out_depth` is NULL.
 *
 * @pre `nav` and `out_depth` non-NULL.
 * @post `*out_depth == nav->depth`.
 * @post No navigation state is modified.
 *
 * @note Pure read; not thread-safe vs concurrent mutation.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_app_nav_depth(const ra8_app_nav_t* nav, uint16_t* out_depth);

#ifdef __cplusplus
}
#endif
