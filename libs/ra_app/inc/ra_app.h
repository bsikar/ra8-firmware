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
  void (*render)(const struct ra_app* a); /**< Draw (on-target, foreground). */
  bool (*on_input)(struct ra_app* a, const ra_widget_event_t* ev); /**< Handle. */
  void (*on_leave)(struct ra_app* a); /**< Lost focus / background.       */
  void (*deinit)(struct ra_app* a);   /**< Teardown.                      */
} ra_app_vtable_t;

/**
 * @struct ra_app_t
 * @brief One app instance + its metadata (caller-owned, static).
 */
typedef struct ra_app {
  const ra_app_vtable_t* vt;          /**< Lifecycle table (non-NULL).      */
  void*                  ctx;         /**< App-specific state.              */
  uint16_t               id;          /**< Unique app id (launch key).      */
  const char*            name;        /**< Display name for the launcher.   */
  bool                   removable;   /**< Optional/uninstallable core?     */
  bool                   initialized; /**< Set true once `init` succeeded.  */
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

#ifdef __cplusplus
}
#endif
