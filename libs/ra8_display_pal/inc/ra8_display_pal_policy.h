/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_display_pal_policy.h
 * @brief Pluggable page-turn refresh-cadence policy for bistable (e-ink) panels.
 * @ingroup grp_ereader
 *
 * @details
 * E-ink panels are bistable: every ``display_flush`` is a real, visible panel
 * update whose cost and ghosting depend on the chosen ``display_refresh_hint_t``
 * (fast A2 ghosts but is snappy; clean GC16 is slow but clears ghosting; INIT is
 * a full white reset). A reader must therefore decide, per page turn, WHICH
 * waveform and WHICH update extent (full vs partial rect) to use.
 *
 * This module isolates that decision behind a small, pure strategy object so the
 * application stays format- and panel-agnostic (Dependency Inversion): the app
 * feeds page-turn *events* in and gets a ::display_policy_decision_t out, then
 * applies it via ``display_flush``. The strategy is selectable at init
 * (::display_policy_kind_t), so the cadence can be chosen dynamically -- snappy
 * fast-only, always-clean quality, or the default fast-with-periodic-clean that
 * trades a periodic clean GC16 against A2 ghosting build-up.
 *
 * Pure logic: no MMIO, no heap, no I/O. Continuous-refresh (LCD) backends do not
 * need it -- it is inert there (every hint behaves the same on a scan-out panel).
 *
 *
 * [Ring 4 / Display] {World: NS}
 *
 * @since 0.1.0
 */

#pragma once

#include <stdint.h>

#include "ra8_display_pal.h"
#include "ra8_err.h"

/**
 * @enum display_policy_kind_t
 * @brief Selectable refresh-cadence strategies (chosen at ::display_policy_init).
 *
 * @details Picks the trade-off between turn latency and ghosting. The app may
 * switch strategy dynamically by re-initialising the policy.
 *
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_display_policy_fast_only  = 0U, /**< Always fast A2 partial -- snappiest, ghosts. */
  k_display_policy_quality    = 1U, /**< Always clean GC16 full -- no ghosting, slow. */
  k_display_policy_fast_clean = 2U, /**< A2 partial, periodic GC16 clean (default).   */
} display_policy_kind_t;

/**
 * @enum display_turn_event_t
 * @brief The page-transition event the policy reacts to.
 *
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_display_event_open    = 0U, /**< Book/first page opened -- wants a clean INIT.   */
  k_display_event_turn    = 1U, /**< Ordinary next/prev page turn.                   */
  k_display_event_chapter = 2U, /**< Chapter-boundary turn -- wants a clean refresh. */
} display_turn_event_t;

/**
 * @enum display_policy_const_t
 * @brief Bounds for the periodic-clean cadence (no magic numbers).
 *
 * @since 0.1.0
 */
typedef enum : uint16_t {
  k_display_policy_clean_every_default = 8U,   /**< Default GC16 every N fast turns.   */
  k_display_policy_clean_every_min     = 1U,   /**< Clamp floor (every turn is clean). */
  k_display_policy_clean_every_max     = 256U, /**< Clamp ceiling.                     */
} display_policy_const_t;

/**
 * @struct display_policy_t
 * @brief Caller-owned policy state (zero heap; init once, decide per turn).
 *
 * @invariant ``kind`` is a valid ::display_policy_kind_t.
 * @invariant ``clean_every`` is in [::k_display_policy_clean_every_min,
 *            ::k_display_policy_clean_every_max].
 *
 * @since 0.1.0
 */
typedef struct {
  uint8_t  kind;              /**< Active ::display_policy_kind_t.              */
  uint16_t clean_every;       /**< Fast turns between GC16 cleans (fast_clean). */
  uint16_t turns_since_clean; /**< Fast turns since the last clean refresh.     */
} display_policy_t;

/**
 * @struct display_policy_decision_t
 * @brief What the app should pass to ``display_flush`` for this event.
 *
 * @since 0.1.0
 */
typedef struct {
  display_refresh_hint_t hint;        /**< Waveform intent (fast/quality/init). */
  bool                   full_update; /**< true => flush the whole page rect.   */
} display_policy_decision_t;

/**
 * @brief Initialise a refresh-cadence policy.
 *
 * @details Selects the strategy and (for ::k_display_policy_fast_clean) the
 * clean cadence, then resets the turn counter. The policy is then driven one
 * event at a time via ::display_policy_decide.
 *
 * @param[out] p           Policy state to initialise.
 * @param[in]  kind        Strategy to run.
 * @param[in]  clean_every GC16 cadence for ::k_display_policy_fast_clean; clamped
 *                         to [min,max]; ignored by the other strategies.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok               Initialised.
 * @retval k_ra8_err_null_ptr     @p p is NULL.
 * @retval k_ra8_err_invalid_arg  @p kind out of range.
 *
 * @pre @p p points to writable storage.
 * @pre @p kind is a defined ::display_policy_kind_t.
 * @post On success ``turns_since_clean == 0`` and the struct invariants hold.
 * @post On failure @p p is unmodified.
 * @note Not thread-safe; the reader UI is single-threaded.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
display_policy_init(display_policy_t* p, display_policy_kind_t kind, uint16_t clean_every);

/**
 * @brief Decide the flush parameters for one page-transition event.
 *
 * @details
 * - ::k_display_event_open always yields a clean INIT full update and resets the
 *   counter (clears any boot ghosting).
 * - ::k_display_policy_fast_only: A2 partial for every non-open event.
 * - ::k_display_policy_quality: GC16 full for every event.
 * - ::k_display_policy_fast_clean: a clean GC16 full when the turn is a clean
 *   turn -- ``(turns_since_clean + 1 >= clean_every) OR event == chapter`` --
 *   else A2 partial. A clean turn resets the counter.
 *
 * @param[in,out] p     Policy state (counter updated).
 * @param[in]     event The page-transition that occurred.
 * @param[out]    out   Receives the waveform hint + full/partial extent.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok               Decision written.
 * @retval k_ra8_err_null_ptr     @p p or @p out is NULL.
 * @retval k_ra8_err_invalid_arg  @p event out of range.
 *
 * @pre @p p was initialised by ::display_policy_init.
 * @pre @p out points to writable storage.
 * @post On success @p out holds a valid hint and the counter reflects the event.
 * @post On failure neither @p p nor @p out is modified.
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t display_policy_decide(display_policy_t*          p,
                                              display_turn_event_t       event,
                                              display_policy_decision_t* out);

/**
 * @brief Build the full-page damage rectangle for a page turn.
 *
 * @details A page turn repaints the whole content area, so the partial-update
 * rect is the full framebuffer. Apps that repaint only a sub-region (e.g. a
 * status line) can build their own ::display_rect_t instead.
 *
 * @param[in]  w   Framebuffer width in pixels (> 0).
 * @param[in]  h   Framebuffer height in pixels (> 0).
 * @param[out] out Receives the {0,0,w,h} rectangle.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok              Rectangle written.
 * @retval k_ra8_err_null_ptr    @p out is NULL.
 * @retval k_ra8_err_invalid_arg @p w or @p h is 0.
 *
 * @pre @p out points to writable storage.
 * @pre @p w and @p h are non-zero.
 * @post On success @p out == {0,0,w,h}.
 * @post On failure @p out is unmodified.
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t display_policy_full_rect(uint16_t w, uint16_t h, display_rect_t* out);
