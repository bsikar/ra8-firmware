/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_display_pal_policy.c
 * @brief Implementation of the e-ink page-turn refresh-cadence policy.
 *
 * @details
 * Pure decision logic (no MMIO/heap/I/O) implementing ra8_display_pal_policy.h.
 * The application owns a ::display_policy_t, feeds page-transition events in, and
 * applies the returned ::display_policy_decision_t through ``display_flush``.
 *
 *
 * [Ring 4 / Display] {World: NS}
 *
 * @since 0.1.0
 */

#include "ra8_display_pal_policy.h"

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_err.h"

/** @brief Log tag for the refresh-cadence policy module. */
static const char* const s_tag_policy = "disp_policy";

/**
 * @brief Clamp a requested clean cadence to the documented bounds.
 *
 * @details
 * Applies a symmetric min/max clamp to @p n using the constants
 * ::k_display_policy_clean_every_min and ::k_display_policy_clean_every_max.
 * Values below the floor are raised to the floor; values above the ceiling are
 * lowered to the ceiling; in-range values are returned unchanged.  The function
 * is branchless-equivalent for the common in-range case.
 *
 * @param[in] n Requested cadence (fast turns between GC16 cleans).
 *
 * @return uint16_t Clamped cadence in
 *         [::k_display_policy_clean_every_min, ::k_display_policy_clean_every_max].
 * @retval k_display_policy_clean_every_min  Returned when @p n is below the floor.
 * @retval k_display_policy_clean_every_max  Returned when @p n exceeds the ceiling.
 * @retval n                                 Returned unchanged when in range.
 *
 * @pre @p n is a uint16_t (caller is responsible for providing a valid value).
 * @pre ::k_display_policy_clean_every_min <= ::k_display_policy_clean_every_max.
 * @post The returned value is in
 *       [::k_display_policy_clean_every_min, ::k_display_policy_clean_every_max].
 * @post The policy state is not modified (pure, side-effect-free).
 *
 * @note Not thread-safe; called only from ::display_policy_init which is
 *       single-threaded in the reader UI.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
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
 * (event == chapter)` -- which also resets the counter.  On a clean turn
 * ``p->turns_since_clean`` is reset to zero and the full-update flag is set
 * with ::k_display_refresh_quality; on a fast turn the counter is incremented
 * by one and ::k_display_refresh_fast is selected with a partial update.
 *
 * @param[in,out] p     Policy state (counter updated on every call).
 * @param[in]     event The non-open page-transition (must not be
 *                      ::k_display_event_open; caller is responsible).
 * @param[out]    out   Receives the waveform hint and full/partial extent.
 *
 * @return Nothing.
 *
 * @pre @p p was initialised by ::display_policy_init with
 *      ::k_display_policy_fast_clean as the kind.
 * @pre @p out points to writable ::display_policy_decision_t storage.
 * @post On a clean turn ``p->turns_since_clean == 0`` and
 *       ``out->hint == k_display_refresh_quality``.
 * @post On a fast turn ``p->turns_since_clean`` is one greater than on entry
 *       and ``out->hint == k_display_refresh_fast``.
 *
 * @note Not thread-safe; called only from ::display_policy_decide which is
 *       single-threaded in the reader UI.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
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

ra8_err_t display_policy_init(display_policy_t* p, display_policy_kind_t kind, uint16_t clean_every)
{
  RA8_CHECK_NULL_PTR(p, s_tag_policy, "init: null policy");
  if (kind > k_display_policy_fast_clean) {
    ra8_log_error(s_tag_policy, "init: kind out of range");
    return k_ra8_err_invalid_arg;
  }
  p->kind              = (uint8_t)kind;
  p->clean_every       = priv_clamp_clean_every(clean_every);
  p->turns_since_clean = 0U;
  return k_ra8_ok;
}

ra8_err_t display_policy_decide(display_policy_t*          p,
                                display_turn_event_t       event,
                                display_policy_decision_t* out)
{
  RA8_CHECK_NULL_PTR(p, s_tag_policy, "decide: null policy");
  RA8_CHECK_NULL_PTR(out, s_tag_policy, "decide: null out");
  if (event > k_display_event_chapter) {
    ra8_log_error(s_tag_policy, "decide: event out of range");
    return k_ra8_err_invalid_arg;
  }

  /* A fresh open always clears the panel (INIT) regardless of strategy. */
  if (event == k_display_event_open) {
    out->hint            = k_display_refresh_init;
    out->full_update     = true;
    p->turns_since_clean = 0U;
    return k_ra8_ok;
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
  return k_ra8_ok;
}

ra8_err_t display_policy_full_rect(uint16_t w, uint16_t h, display_rect_t* out)
{
  RA8_CHECK_NULL_PTR(out, s_tag_policy, "full_rect: null out");
  if ((w == 0U) || (h == 0U)) {
    ra8_log_error(s_tag_policy, "full_rect: zero dimension");
    return k_ra8_err_invalid_arg;
  }
  out->x = 0U;
  out->y = 0U;
  out->w = w;
  out->h = h;
  return k_ra8_ok;
}
