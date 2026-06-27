/**
 * @file ra_app.c
 * @brief Implementation of the ra_app app framework (#146).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_app.h"

#include "ra_check.h"

/** @brief Logging / check tag. */
static const char* s_tag = "ra_app";

[[nodiscard]] ra_err_t
ra_app_registry_init(ra_app_registry_t* reg, ra_app_t** storage, uint16_t cap)
{
  RA_CHECK_NULL_PTR(reg, s_tag, "reg must not be nullptr");
  RA_CHECK_NULL_PTR(storage, s_tag, "storage must not be nullptr");
  if (cap == 0U) {
    return k_ra_err_invalid_arg;
  }
  reg->apps   = storage;
  reg->cap    = cap;
  reg->count  = 0U;
  reg->active = (int16_t)k_ra_app_none;
  return k_ra_ok;
}

[[nodiscard]] ra_err_t ra_app_find(const ra_app_registry_t* reg, uint16_t id, int16_t* out_idx)
{
  RA_CHECK_NULL_PTR(reg, s_tag, "reg must not be nullptr");
  RA_CHECK_NULL_PTR(out_idx, s_tag, "out_idx must not be nullptr");
  *out_idx = (int16_t)k_ra_app_none;
  for (uint16_t i = 0U; i < reg->count; ++i) {
    if ((reg->apps[i] != nullptr) && (reg->apps[i]->id == id)) {
      *out_idx = (int16_t)i;
      return k_ra_ok;
    }
  }
  return k_ra_ok;
}

[[nodiscard]] ra_err_t ra_app_register(ra_app_registry_t* reg, ra_app_t* app)
{
  RA_CHECK_NULL_PTR(reg, s_tag, "reg must not be nullptr");
  RA_CHECK_NULL_PTR(app, s_tag, "app must not be nullptr");
  RA_CHECK_NULL_PTR(app->vt, s_tag, "app->vt must not be nullptr");
  /* A duplicate id is a conflict regardless of capacity (check it first so a
   * full registry still reports the more precise error). */
  int16_t        existing = (int16_t)k_ra_app_none;
  const ra_err_t ferr     = ra_app_find(reg, app->id, &existing);
  if (ferr != k_ra_ok) {
    return ferr;
  }
  if (existing != (int16_t)k_ra_app_none) {
    return k_ra_err_conflict;
  }
  if (reg->count >= reg->cap) {
    return k_ra_err_no_mem;
  }
  if (app->vt->init != nullptr) {
    const ra_err_t ierr = app->vt->init(app);
    if (ierr != k_ra_ok) {
      return ierr; /* leave unregistered */
    }
  }
  app->initialized      = true;
  reg->apps[reg->count] = app;
  reg->count            = (uint16_t)(reg->count + 1U);
  return k_ra_ok;
}

[[nodiscard]] ra_err_t ra_app_launch(ra_app_registry_t* reg, uint16_t id)
{
  RA_CHECK_NULL_PTR(reg, s_tag, "reg must not be nullptr");
  int16_t        target = (int16_t)k_ra_app_none;
  const ra_err_t ferr   = ra_app_find(reg, id, &target);
  if (ferr != k_ra_ok) {
    return ferr;
  }
  if (target == (int16_t)k_ra_app_none) {
    return k_ra_err_not_found;
  }
  if (target == reg->active) {
    return k_ra_ok; /* already focused: idempotent */
  }
  if (reg->active != (int16_t)k_ra_app_none) {
    ra_app_t* cur = reg->apps[reg->active];
    if ((cur != nullptr) && (cur->vt->on_leave != nullptr)) {
      cur->vt->on_leave(cur);
    }
  }
  reg->active    = target;
  ra_app_t* next = reg->apps[target];
  if ((next != nullptr) && (next->vt->on_enter != nullptr)) {
    next->vt->on_enter(next);
  }
  return k_ra_ok;
}

[[nodiscard]] ra_err_t ra_app_active(const ra_app_registry_t* reg, ra_app_t** out_app)
{
  RA_CHECK_NULL_PTR(reg, s_tag, "reg must not be nullptr");
  RA_CHECK_NULL_PTR(out_app, s_tag, "out_app must not be nullptr");
  *out_app = (reg->active != (int16_t)k_ra_app_none) ? reg->apps[reg->active] : nullptr;
  return k_ra_ok;
}

[[nodiscard]] ra_err_t
ra_app_route_input(ra_app_registry_t* reg, const ra_widget_event_t* ev, bool* out_handled)
{
  RA_CHECK_NULL_PTR(reg, s_tag, "reg must not be nullptr");
  RA_CHECK_NULL_PTR(ev, s_tag, "ev must not be nullptr");
  RA_CHECK_NULL_PTR(out_handled, s_tag, "out_handled must not be nullptr");
  *out_handled = false;
  if (reg->active == (int16_t)k_ra_app_none) {
    return k_ra_ok;
  }
  ra_app_t* a = reg->apps[reg->active];
  if ((a != nullptr) && (a->vt->on_input != nullptr)) {
    *out_handled = a->vt->on_input(a, ev);
  }
  return k_ra_ok;
}

[[nodiscard]] ra_err_t ra_app_tick(ra_app_registry_t* reg)
{
  RA_CHECK_NULL_PTR(reg, s_tag, "reg must not be nullptr");
  if (reg->active == (int16_t)k_ra_app_none) {
    return k_ra_ok;
  }
  ra_app_t* a = reg->apps[reg->active];
  if ((a != nullptr) && (a->vt->tick != nullptr)) {
    a->vt->tick(a);
  }
  return k_ra_ok;
}

[[nodiscard]] ra_err_t ra_app_render(ra_app_registry_t* reg)
{
  RA_CHECK_NULL_PTR(reg, s_tag, "reg must not be nullptr");
  if (reg->active == (int16_t)k_ra_app_none) {
    return k_ra_ok;
  }
  ra_app_t* a = reg->apps[reg->active];
  if ((a != nullptr) && (a->vt->render != nullptr)) {
    a->vt->render(a);
  }
  return k_ra_ok;
}

[[nodiscard]] ra_err_t ra_app_count(const ra_app_registry_t* reg, uint16_t* out_count)
{
  RA_CHECK_NULL_PTR(reg, s_tag, "reg must not be nullptr");
  RA_CHECK_NULL_PTR(out_count, s_tag, "out_count must not be nullptr");
  *out_count = reg->count;
  return k_ra_ok;
}

[[nodiscard]] ra_err_t ra_app_at(const ra_app_registry_t* reg, uint16_t idx, ra_app_t** out_app)
{
  RA_CHECK_NULL_PTR(reg, s_tag, "reg must not be nullptr");
  RA_CHECK_NULL_PTR(out_app, s_tag, "out_app must not be nullptr");
  if (idx >= reg->count) {
    return k_ra_err_out_of_range;
  }
  *out_app = reg->apps[idx];
  return k_ra_ok;
}

[[nodiscard]] ra_err_t
ra_app_nav_init(ra_app_nav_t* nav, ra_app_registry_t* reg, uint16_t* storage, uint16_t cap)
{
  RA_CHECK_NULL_PTR(nav, s_tag, "nav must not be nullptr");
  RA_CHECK_NULL_PTR(reg, s_tag, "reg must not be nullptr");
  RA_CHECK_NULL_PTR(storage, s_tag, "storage must not be nullptr");
  if (cap == 0U) {
    return k_ra_err_invalid_arg;
  }
  nav->reg   = reg;
  nav->stack = storage;
  nav->cap   = cap;
  nav->depth = 0U;
  return k_ra_ok;
}

[[nodiscard]] ra_err_t ra_app_nav_go(ra_app_nav_t* nav, uint16_t id)
{
  RA_CHECK_NULL_PTR(nav, s_tag, "nav must not be nullptr");
  RA_CHECK_NULL_PTR(nav->reg, s_tag, "nav->reg must not be nullptr");
  ra_app_t*      cur  = nullptr;
  const ra_err_t aerr = ra_app_active(nav->reg, &cur);
  if (aerr != k_ra_ok) {
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
      return k_ra_err_no_mem;
    }
  }
  const ra_err_t lerr = ra_app_launch(nav->reg, id);
  if (lerr != k_ra_ok) {
    return lerr;
  }
  if (will_push) {
    nav->stack[nav->depth] = prev_id;
    nav->depth             = (uint16_t)(nav->depth + 1U);
  }
  return k_ra_ok;
}

[[nodiscard]] ra_err_t ra_app_nav_go_index(ra_app_nav_t* nav, uint16_t idx)
{
  RA_CHECK_NULL_PTR(nav, s_tag, "nav must not be nullptr");
  RA_CHECK_NULL_PTR(nav->reg, s_tag, "nav->reg must not be nullptr");
  ra_app_t*      app  = nullptr;
  const ra_err_t aerr = ra_app_at(nav->reg, idx, &app);
  if (aerr != k_ra_ok) {
    return aerr; /* out_of_range for idx >= count */
  }
  RA_CHECK_NULL_PTR(app, s_tag, "registry slot at idx must not be nullptr");
  return ra_app_nav_go(nav, app->id);
}

[[nodiscard]] ra_err_t ra_app_nav_back(ra_app_nav_t* nav, bool* out_popped)
{
  RA_CHECK_NULL_PTR(nav, s_tag, "nav must not be nullptr");
  RA_CHECK_NULL_PTR(nav->reg, s_tag, "nav->reg must not be nullptr");
  RA_CHECK_NULL_PTR(out_popped, s_tag, "out_popped must not be nullptr");
  *out_popped = false;
  if (nav->depth == 0U) {
    return k_ra_ok; /* at the root: nothing to go back to */
  }
  const uint16_t prev_id = nav->stack[nav->depth - 1U];
  const ra_err_t lerr    = ra_app_launch(nav->reg, prev_id);
  if (lerr != k_ra_ok) {
    return lerr;
  }
  nav->depth  = (uint16_t)(nav->depth - 1U);
  *out_popped = true;
  return k_ra_ok;
}

[[nodiscard]] ra_err_t ra_app_nav_depth(const ra_app_nav_t* nav, uint16_t* out_depth)
{
  RA_CHECK_NULL_PTR(nav, s_tag, "nav must not be nullptr");
  RA_CHECK_NULL_PTR(out_depth, s_tag, "out_depth must not be nullptr");
  *out_depth = nav->depth;
  return k_ra_ok;
}
