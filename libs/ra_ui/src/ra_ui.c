/**
 * @file ra_ui.c
 * @brief Implementation of the bounded UI interaction core.
 *
 * @par Tag
 * [Ring 5 / UI] {World: NS}
 *
 * @details
 * Allocation-free hit-testing, screen-stack navigation, and page-cursor
 * logic. See ra_ui.h for the contract. Every entry point validates its
 * pointer arguments and leaves caller state untouched on error.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include "ra_ui.h"

#include <stddef.h>
#include <stdint.h>

#include "ra_check.h"
#include "ra_err.h"

/** @brief Component tag for diagnostic logging. */
static const char* const s_tag = "ra_ui";

/* ===========================================================================
 * Geometry + hit-testing
 * ===========================================================================
 */

bool ra_ui_rect_contains(const ra_ui_rect_t* r, int32_t px, int32_t py)
{
  if (r == nullptr) {
    return false;
  }
  return ((px >= r->x) && (px < (r->x + r->w)) && (py >= r->y) && (py < (r->y + r->h)));
}

ra_err_t ra_ui_hit_test(const ra_ui_target_t* targets,
                        uint16_t              count,
                        int32_t               px,
                        int32_t               py,
                        uint16_t*             out_action,
                        bool*                 out_hit)
{
  RA_CHECK_NULL_PTR(out_action, s_tag, "out_action must not be nullptr");
  RA_CHECK_NULL_PTR(out_hit, s_tag, "out_hit must not be nullptr");
  if (count > 0U) {
    RA_CHECK_NULL_PTR(targets, s_tag, "targets must not be nullptr when count > 0");
  }

  *out_hit = false;
  for (uint16_t i = 0U; i < count; ++i) {
    if (ra_ui_rect_contains(&targets[i].rect, px, py)) {
      *out_action = targets[i].action_id;
      *out_hit    = true;
      return k_ra_ok;
    }
  }
  return k_ra_ok;
}

/* ===========================================================================
 * Screen-stack navigation
 * ===========================================================================
 */

ra_err_t ra_ui_nav_init(ra_ui_nav_t* nav, uint16_t root_screen)
{
  RA_CHECK_NULL_PTR(nav, s_tag, "nav must not be nullptr");
  nav->stack[0] = root_screen;
  nav->depth    = 1U;
  return k_ra_ok;
}

ra_err_t ra_ui_nav_push(ra_ui_nav_t* nav, uint16_t screen)
{
  RA_CHECK_NULL_PTR(nav, s_tag, "nav must not be nullptr");
  if (nav->depth == 0U) {
    return k_ra_err_invalid_state;
  }
  if (nav->depth >= (uint8_t)k_ra_ui_nav_max_depth) {
    return k_ra_err_no_mem;
  }
  nav->stack[nav->depth] = screen;
  nav->depth++;
  return k_ra_ok;
}

ra_err_t ra_ui_nav_pop(ra_ui_nav_t* nav, uint16_t* out_screen)
{
  RA_CHECK_NULL_PTR(nav, s_tag, "nav must not be nullptr");
  RA_CHECK_NULL_PTR(out_screen, s_tag, "out_screen must not be nullptr");
  if (nav->depth <= 1U) {
    return k_ra_err_invalid_state;
  }
  nav->depth--;
  *out_screen = nav->stack[nav->depth - 1U];
  return k_ra_ok;
}

ra_err_t ra_ui_nav_replace(ra_ui_nav_t* nav, uint16_t screen)
{
  RA_CHECK_NULL_PTR(nav, s_tag, "nav must not be nullptr");
  if (nav->depth == 0U) {
    return k_ra_err_invalid_state;
  }
  nav->stack[nav->depth - 1U] = screen;
  return k_ra_ok;
}

ra_err_t ra_ui_nav_top(const ra_ui_nav_t* nav, uint16_t* out_screen)
{
  RA_CHECK_NULL_PTR(nav, s_tag, "nav must not be nullptr");
  RA_CHECK_NULL_PTR(out_screen, s_tag, "out_screen must not be nullptr");
  if (nav->depth == 0U) {
    return k_ra_err_invalid_state;
  }
  *out_screen = nav->stack[nav->depth - 1U];
  return k_ra_ok;
}

/* ===========================================================================
 * Paging
 * ===========================================================================
 */

ra_err_t ra_ui_pager_init(ra_ui_pager_t* p, uint16_t total)
{
  RA_CHECK_NULL_PTR(p, s_tag, "p must not be nullptr");
  if (total == 0U) {
    return k_ra_err_invalid_arg;
  }
  p->current = 0U;
  p->total   = total;
  return k_ra_ok;
}

ra_err_t ra_ui_pager_next(ra_ui_pager_t* p, bool* out_changed)
{
  RA_CHECK_NULL_PTR(p, s_tag, "p must not be nullptr");
  RA_CHECK_NULL_PTR(out_changed, s_tag, "out_changed must not be nullptr");
  if ((p->total > 0U) && ((uint32_t)p->current < ((uint32_t)p->total - 1U))) {
    p->current++;
    *out_changed = true;
  } else {
    *out_changed = false;
  }
  return k_ra_ok;
}

ra_err_t ra_ui_pager_prev(ra_ui_pager_t* p, bool* out_changed)
{
  RA_CHECK_NULL_PTR(p, s_tag, "p must not be nullptr");
  RA_CHECK_NULL_PTR(out_changed, s_tag, "out_changed must not be nullptr");
  if (p->current > 0U) {
    p->current--;
    *out_changed = true;
  } else {
    *out_changed = false;
  }
  return k_ra_ok;
}

ra_err_t ra_ui_pager_goto(ra_ui_pager_t* p, uint16_t page, bool* out_changed)
{
  RA_CHECK_NULL_PTR(p, s_tag, "p must not be nullptr");
  RA_CHECK_NULL_PTR(out_changed, s_tag, "out_changed must not be nullptr");
  uint16_t target = page;
  if ((p->total > 0U) && (target > (uint16_t)(p->total - 1U))) {
    target = (uint16_t)(p->total - 1U);
  }
  *out_changed = (target != p->current);
  p->current   = target;
  return k_ra_ok;
}
