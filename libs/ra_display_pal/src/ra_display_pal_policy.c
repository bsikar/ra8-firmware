/**
 * @file ra_display_pal_policy.c
 * @brief Implementation of the e-ink page-turn refresh-cadence policy.
 *
 * @details
 * Pure decision logic (no MMIO/heap/I/O) implementing ra_display_pal_policy.h.
 * The application owns a ::display_policy_t, feeds page-transition events in, and
 * applies the returned ::display_policy_decision_t through ``display_flush``.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * [Ring 4 / Display] {World: NS}
 *
 * @since 0.1.0
 */

#include "ra_display_pal_policy.h"

#include <stdint.h>

#include "ra_check.h"
#include "ra_err.h"

/** @brief Log tag for the refresh-cadence policy module. */
static const char* const s_tag_policy = "disp_policy";

/** @brief Clamp a requested clean cadence to the documented bounds. */
static uint16_t priv_clamp_clean_every(uint16_t n)
{
  if (n < (uint16_t)k_display_policy_clean_every_min) {
    return (uint16_t)k_display_policy_clean_every_min;
  }
  if (n > (uint16_t)k_display_policy_clean_every_max) {
    return (uint16_t)k_display_policy_clean_every_max;
  }
  return n;
}

/**
 * @brief Resolve the ::k_display_policy_fast_clean decision for a non-open turn.
 *
 * @details A2 partial by default; a clean GC16 full when this turn is a "clean
 * turn" -- the MC/DC decision `(turns_since_clean + 1 >= clean_every) ||
 * (event == chapter)` -- which also resets the counter.
 *
 * @param[in,out] p     Policy state (counter updated).
 * @param[in]     event The non-open page-transition.
 * @param[out]    out   Receives the hint + extent.
 */
static void priv_decide_fast_clean(display_policy_t*          p,
                                   display_turn_event_t       event,
                                   display_policy_decision_t* out)
{
  const uint16_t next  = (uint16_t)(p->turns_since_clean + 1U);
  const bool     clean = (next >= p->clean_every) || (event == k_display_event_chapter);
  if (clean) {
    out->hint            = k_display_refresh_quality;
    out->full_update     = true;
    p->turns_since_clean = 0U;
  } else {
    out->hint            = k_display_refresh_fast;
    out->full_update     = false;
    p->turns_since_clean = next;
  }
}

ra_err_t display_policy_init(display_policy_t* p, display_policy_kind_t kind, uint16_t clean_every)
{
  RA_CHECK_NULL_PTR(p, s_tag_policy, "init: null policy");
  if (kind > k_display_policy_fast_clean) {
    ra_log_error(s_tag_policy, "init: kind out of range");
    return k_ra_err_invalid_arg;
  }
  p->kind              = (uint8_t)kind;
  p->clean_every       = priv_clamp_clean_every(clean_every);
  p->turns_since_clean = 0U;
  return k_ra_ok;
}

ra_err_t display_policy_decide(display_policy_t*          p,
                               display_turn_event_t       event,
                               display_policy_decision_t* out)
{
  RA_CHECK_NULL_PTR(p, s_tag_policy, "decide: null policy");
  RA_CHECK_NULL_PTR(out, s_tag_policy, "decide: null out");
  if (event > k_display_event_chapter) {
    ra_log_error(s_tag_policy, "decide: event out of range");
    return k_ra_err_invalid_arg;
  }

  /* A fresh open always clears the panel (INIT) regardless of strategy. */
  if (event == k_display_event_open) {
    out->hint            = k_display_refresh_init;
    out->full_update     = true;
    p->turns_since_clean = 0U;
    return k_ra_ok;
  }

  switch (p->kind) {
    case (uint8_t)k_display_policy_fast_only:
      out->hint        = k_display_refresh_fast;
      out->full_update = false;
      break;
    case (uint8_t)k_display_policy_quality:
      out->hint        = k_display_refresh_quality;
      out->full_update = true;
      break;
    case (uint8_t)k_display_policy_fast_clean:
    default:
      priv_decide_fast_clean(p, event, out);
      break;
  }
  return k_ra_ok;
}

ra_err_t display_policy_full_rect(uint16_t w, uint16_t h, display_rect_t* out)
{
  RA_CHECK_NULL_PTR(out, s_tag_policy, "full_rect: null out");
  if ((w == 0U) || (h == 0U)) {
    ra_log_error(s_tag_policy, "full_rect: zero dimension");
    return k_ra_err_invalid_arg;
  }
  out->x = 0U;
  out->y = 0U;
  out->w = w;
  out->h = h;
  return k_ra_ok;
}
