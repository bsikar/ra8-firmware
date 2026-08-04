/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_app.c
 * @brief Implementation of the ra8_app app framework (#146).
 */

#include "ra8_app.h"

#include "ra8_check.h"

/** @brief Logging / check tag. */
static const char* s_tag = "ra8_app";

[[nodiscard]] ra8_err_t
ra8_app_registry_init(ra8_app_registry_t* reg, ra8_app_t** storage, uint16_t cap)
{
  RA8_CHECK_NULL_PTR(reg, s_tag, "reg must not be nullptr");
  RA8_CHECK_NULL_PTR(storage, s_tag, "storage must not be nullptr");
  if (cap == 0U) {
    return k_ra8_err_invalid_arg;
  }
  reg->apps   = storage;
  reg->cap    = cap;
  reg->count  = 0U;
  reg->active = (int16_t)k_ra8_app_none;
  return k_ra8_ok;
}

[[nodiscard]] ra8_err_t ra8_app_find(const ra8_app_registry_t* reg, uint16_t id, int16_t* out_idx)
{
  RA8_CHECK_NULL_PTR(reg, s_tag, "reg must not be nullptr");
  RA8_CHECK_NULL_PTR(out_idx, s_tag, "out_idx must not be nullptr");
  *out_idx = (int16_t)k_ra8_app_none;
  for (uint16_t i = 0U; i < reg->count; ++i) {
    if ((reg->apps[i] != nullptr) && (reg->apps[i]->id == id)) {
      *out_idx = (int16_t)i;
      return k_ra8_ok;
    }
  }
  return k_ra8_ok;
}

[[nodiscard]] ra8_err_t ra8_app_register(ra8_app_registry_t* reg, ra8_app_t* app)
{
  RA8_CHECK_NULL_PTR(reg, s_tag, "reg must not be nullptr");
  RA8_CHECK_NULL_PTR(app, s_tag, "app must not be nullptr");
  RA8_CHECK_NULL_PTR(app->vt, s_tag, "app->vt must not be nullptr");
  /* A duplicate id is a conflict regardless of capacity (check it first so a
   * full registry still reports the more precise error). */
  int16_t         existing = (int16_t)k_ra8_app_none;
  const ra8_err_t ferr     = ra8_app_find(reg, app->id, &existing);
  if (ferr != k_ra8_ok) {
    return ferr;
  }
  if (existing != (int16_t)k_ra8_app_none) {
    return k_ra8_err_conflict;
  }
  if (reg->count >= reg->cap) {
    return k_ra8_err_no_mem;
  }
  if (app->vt->init != nullptr) {
    const ra8_err_t ierr = app->vt->init(app);
    if (ierr != k_ra8_ok) {
      return ierr; /* leave unregistered */
    }
  }
  app->initialized      = true;
  reg->apps[reg->count] = app;
  reg->count            = (uint16_t)(reg->count + 1U);
  return k_ra8_ok;
}

[[nodiscard]] ra8_err_t ra8_app_launch(ra8_app_registry_t* reg, uint16_t id)
{
  RA8_CHECK_NULL_PTR(reg, s_tag, "reg must not be nullptr");
  int16_t         target = (int16_t)k_ra8_app_none;
  const ra8_err_t ferr   = ra8_app_find(reg, id, &target);
  if (ferr != k_ra8_ok) {
    return ferr;
  }
  if (target == (int16_t)k_ra8_app_none) {
    return k_ra8_err_not_found;
  }
  if (target == reg->active) {
    return k_ra8_ok; /* already focused: idempotent */
  }
  if (reg->active != (int16_t)k_ra8_app_none) {
    ra8_app_t* cur = reg->apps[reg->active];
    if ((cur != nullptr) && (cur->vt->on_leave != nullptr)) {
      cur->vt->on_leave(cur);
    }
  }
  reg->active     = target;
  ra8_app_t* next = reg->apps[target];
  /*
   * `target` is a live registry index returned by ra8_app_find() above (the
   * function already returned k_ra8_err_not_found for k_ra8_app_none), and
   * ra8_app_register() never stores a nullptr slot, so reg->apps[target] is
   * provably non-null here. The (next != nullptr) clause is a defensive guard
   * that cannot be driven false on any public-API path, so it cannot be given
   * independent MC/DC influence; only (next->vt->on_enter != nullptr) varies.
   */
  /* mcdc-deactivated: next=reg->apps[target] with target a validated ra8_app_find index; (next!=nullptr) is always true here. */
  if ((next != nullptr) && (next->vt->on_enter != nullptr)) {
    next->vt->on_enter(next);
  }
  return k_ra8_ok;
}

[[nodiscard]] ra8_err_t ra8_app_active(const ra8_app_registry_t* reg, ra8_app_t** out_app)
{
  RA8_CHECK_NULL_PTR(reg, s_tag, "reg must not be nullptr");
  RA8_CHECK_NULL_PTR(out_app, s_tag, "out_app must not be nullptr");
  *out_app = (reg->active != (int16_t)k_ra8_app_none) ? reg->apps[reg->active] : nullptr;
  return k_ra8_ok;
}

[[nodiscard]] ra8_err_t
ra8_app_route_input(ra8_app_registry_t* reg, const ra8_widget_event_t* ev, bool* out_handled)
{
  RA8_CHECK_NULL_PTR(reg, s_tag, "reg must not be nullptr");
  RA8_CHECK_NULL_PTR(ev, s_tag, "ev must not be nullptr");
  RA8_CHECK_NULL_PTR(out_handled, s_tag, "out_handled must not be nullptr");
  *out_handled = false;
  if (reg->active == (int16_t)k_ra8_app_none) {
    return k_ra8_ok;
  }
  ra8_app_t* a = reg->apps[reg->active];
  if ((a != nullptr) && (a->vt->on_input != nullptr)) {
    *out_handled = a->vt->on_input(a, ev);
  }
  return k_ra8_ok;
}

[[nodiscard]] ra8_err_t ra8_app_tick(ra8_app_registry_t* reg)
{
  RA8_CHECK_NULL_PTR(reg, s_tag, "reg must not be nullptr");
  if (reg->active == (int16_t)k_ra8_app_none) {
    return k_ra8_ok;
  }
  ra8_app_t* a = reg->apps[reg->active];
  if ((a != nullptr) && (a->vt->tick != nullptr)) {
    a->vt->tick(a);
  }
  return k_ra8_ok;
}

[[nodiscard]] ra8_err_t ra8_app_render(ra8_app_registry_t* reg)
{
  RA8_CHECK_NULL_PTR(reg, s_tag, "reg must not be nullptr");
  if (reg->active == (int16_t)k_ra8_app_none) {
    return k_ra8_ok;
  }
  ra8_app_t* a = reg->apps[reg->active];
  if ((a != nullptr) && (a->vt->render != nullptr)) {
    a->vt->render(a);
  }
  return k_ra8_ok;
}

[[nodiscard]] ra8_err_t ra8_app_count(const ra8_app_registry_t* reg, uint16_t* out_count)
{
  RA8_CHECK_NULL_PTR(reg, s_tag, "reg must not be nullptr");
  RA8_CHECK_NULL_PTR(out_count, s_tag, "out_count must not be nullptr");
  *out_count = reg->count;
  return k_ra8_ok;
}

[[nodiscard]] ra8_err_t ra8_app_at(const ra8_app_registry_t* reg, uint16_t idx, ra8_app_t** out_app)
{
  RA8_CHECK_NULL_PTR(reg, s_tag, "reg must not be nullptr");
  RA8_CHECK_NULL_PTR(out_app, s_tag, "out_app must not be nullptr");
  if (idx >= reg->count) {
    return k_ra8_err_out_of_range;
  }
  *out_app = reg->apps[idx];
  return k_ra8_ok;
}

[[nodiscard]] ra8_err_t
ra8_app_nav_init(ra8_app_nav_t* nav, ra8_app_registry_t* reg, uint16_t* storage, uint16_t cap)
{
  RA8_CHECK_NULL_PTR(nav, s_tag, "nav must not be nullptr");
  RA8_CHECK_NULL_PTR(reg, s_tag, "reg must not be nullptr");
  RA8_CHECK_NULL_PTR(storage, s_tag, "storage must not be nullptr");
  if (cap == 0U) {
    return k_ra8_err_invalid_arg;
  }
  nav->reg   = reg;
  nav->stack = storage;
  nav->cap   = cap;
  nav->depth = 0U;
  return k_ra8_ok;
}

[[nodiscard]] ra8_err_t ra8_app_nav_go(ra8_app_nav_t* nav, uint16_t id)
{
  RA8_CHECK_NULL_PTR(nav, s_tag, "nav must not be nullptr");
  RA8_CHECK_NULL_PTR(nav->reg, s_tag, "nav->reg must not be nullptr");
  ra8_app_t*      cur  = nullptr;
  const ra8_err_t aerr = ra8_app_active(nav->reg, &cur);
  if (aerr != k_ra8_ok) {
    return aerr;
  }
  /* Only a switch *away from* a different app records a trail entry. The first
   * launch (no prior focus) and an idempotent re-tap push nothing. The two
   * single-condition guards below avoid a compound decision (each is plain
   * branch coverage, not MC/DC). */
  bool     will_push = false;
  uint16_t prev_id   = 0U;
  if (cur != nullptr) {
    if (cur->id != id) {
      will_push = true;
      prev_id   = cur->id;
    }
  }
  if (will_push) {
    if (nav->depth >= nav->cap) {
      return k_ra8_err_no_mem;
    }
  }
  const ra8_err_t lerr = ra8_app_launch(nav->reg, id);
  if (lerr != k_ra8_ok) {
    return lerr;
  }
  if (will_push) {
    nav->stack[nav->depth] = prev_id;
    nav->depth             = (uint16_t)(nav->depth + 1U);
  }
  return k_ra8_ok;
}

[[nodiscard]] ra8_err_t ra8_app_nav_go_index(ra8_app_nav_t* nav, uint16_t idx)
{
  RA8_CHECK_NULL_PTR(nav, s_tag, "nav must not be nullptr");
  RA8_CHECK_NULL_PTR(nav->reg, s_tag, "nav->reg must not be nullptr");
  ra8_app_t*      app  = nullptr;
  const ra8_err_t aerr = ra8_app_at(nav->reg, idx, &app);
  if (aerr != k_ra8_ok) {
    return aerr; /* out_of_range for idx >= count */
  }
  RA8_CHECK_NULL_PTR(app, s_tag, "registry slot at idx must not be nullptr");
  return ra8_app_nav_go(nav, app->id);
}

[[nodiscard]] ra8_err_t ra8_app_nav_back(ra8_app_nav_t* nav, bool* out_popped)
{
  RA8_CHECK_NULL_PTR(nav, s_tag, "nav must not be nullptr");
  RA8_CHECK_NULL_PTR(nav->reg, s_tag, "nav->reg must not be nullptr");
  RA8_CHECK_NULL_PTR(out_popped, s_tag, "out_popped must not be nullptr");
  *out_popped = false;
  if (nav->depth == 0U) {
    return k_ra8_ok; /* at the root: nothing to go back to */
  }
  const uint16_t  prev_id = nav->stack[nav->depth - 1U];
  const ra8_err_t lerr    = ra8_app_launch(nav->reg, prev_id);
  if (lerr != k_ra8_ok) {
    return lerr;
  }
  nav->depth  = (uint16_t)(nav->depth - 1U);
  *out_popped = true;
  return k_ra8_ok;
}

[[nodiscard]] ra8_err_t ra8_app_nav_depth(const ra8_app_nav_t* nav, uint16_t* out_depth)
{
  RA8_CHECK_NULL_PTR(nav, s_tag, "nav must not be nullptr");
  RA8_CHECK_NULL_PTR(out_depth, s_tag, "out_depth must not be nullptr");
  *out_depth = nav->depth;
  return k_ra8_ok;
}

[[nodiscard]] ra8_err_t
ra8_app_state(const ra8_app_registry_t* reg, uint16_t id, ra8_app_state_t* out_state)
{
  RA8_CHECK_NULL_PTR(reg, s_tag, "reg must not be nullptr");
  RA8_CHECK_NULL_PTR(out_state, s_tag, "out_state must not be nullptr");
  int16_t         idx  = (int16_t)k_ra8_app_none;
  const ra8_err_t ferr = ra8_app_find(reg, id, &idx);
  if (ferr != k_ra8_ok) {
    return ferr;
  }
  if (idx == (int16_t)k_ra8_app_none) {
    *out_state = k_ra8_app_state_unmounted;
    return k_ra8_ok;
  }
  *out_state = (idx == reg->active) ? k_ra8_app_state_foreground : k_ra8_app_state_background;
  return k_ra8_ok;
}

/**
 * @brief Remove the registry slot at @p idx and compact the table down one place.
 *
 * @details
 * Shifts every slot after @p idx down by one (a forward sweep bounded by
 * `reg->count`, NASA Rule 2, mirroring ::ra8_app_find), decrements `count`, and --
 * if the focused app sat *after* the removed slot -- decrements `active` so it
 * keeps pointing at the same app. Factored out of ::ra8_app_uninstall so that
 * function stays within the size budget; the caller has already validated @p idx
 * and run the outgoing app's teardown.
 *
 * @param[in,out] reg Registry to compact (non-NULL; checked by the caller).
 * @param[in]     idx Slot to remove, in `[0, reg->count)` and not the focused app.
 *
 * @pre `reg` is non-NULL and `idx < reg->count`.
 * @pre `idx != reg->active` (the caller refuses to remove the focused app).
 * @post `reg->count` shrank by one and slot @p idx is overwritten.
 * @post `reg->active` still indexes the same app (adjusted if it shifted).
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void internal_app_remove_at(ra8_app_registry_t* reg, uint16_t idx)
{
  for (uint16_t i = idx; (uint16_t)(i + 1U) < reg->count; ++i) {
    reg->apps[i] = reg->apps[i + 1U];
  }
  reg->count = (uint16_t)(reg->count - 1U);
  if (reg->active > (int16_t)idx) {
    reg->active = (int16_t)(reg->active - 1);
  }
}

[[nodiscard]] ra8_err_t ra8_app_uninstall(ra8_app_registry_t* reg, uint16_t id)
{
  RA8_CHECK_NULL_PTR(reg, s_tag, "reg must not be nullptr");
  int16_t         idx  = (int16_t)k_ra8_app_none;
  const ra8_err_t ferr = ra8_app_find(reg, id, &idx);
  if (ferr != k_ra8_ok) {
    return ferr;
  }
  if (idx == (int16_t)k_ra8_app_none) {
    return k_ra8_err_not_found;
  }
  ra8_app_t* app = reg->apps[idx];
  RA8_CHECK_NULL_PTR(app, s_tag, "registry slot must not be nullptr");
  if (!app->removable) {
    return k_ra8_err_not_supported; /* core app: uninstall refused */
  }
  if (idx == reg->active) {
    return k_ra8_err_busy; /* the focused app cannot be unmounted */
  }
  if (app->vt->deinit != nullptr) {
    app->vt->deinit(app);
  }
  app->initialized = false;
  internal_app_remove_at(reg, (uint16_t)idx);
  return k_ra8_ok;
}
