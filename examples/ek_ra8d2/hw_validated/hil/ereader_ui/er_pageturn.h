/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file er_pageturn.h
 * @brief Pure page-turn + input-region decisions for the e-reader (#78).
 *
 * @details
 * The reading view turns pages from two inputs -- touch edge taps and the two
 * user buttons -- and crosses chapter boundaries. The DECISIONS (which way a tap
 * or button means, how a turn advances across pages and chapters, where it
 * clamps) are factored into the small pure functions here so they are unit- and
 * MC/DC-testable without the GLCDC / touch / GPIO stack. `main.c` calls them and
 * applies the result to its file-scope reading-location state; the host test
 * `tests/test_ereader_pageturn.c` exercises them directly.
 *
 * Header-only `static inline` (no TU to link): both the app and the test include
 * it and get their own copies. Pure logic -- no MMIO, no heap, no globals.
 *
 *
 * [Ring 7 / App] {World: NS}
 *
 * @since 0.1.0
 */

#pragma once

#include <stdint.h>

/**
 * @enum er_dir_t
 * @brief Page-turn direction produced by an input decision.
 * @since 0.1.0
 */
typedef enum : int8_t {
  k_er_dir_prev = -1, /**< Go to the previous page. */
  k_er_dir_none = 0,  /**< No page turn.            */
  k_er_dir_next = 1,  /**< Go to the next page.     */
} er_dir_t;

/**
 * @enum er_pageturn_const_t
 * @brief Constants for the page-turn decisions (no magic numbers).
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_er_pt_thirds = 3U,          /**< Screen split into thirds for edge taps. */
  k_er_page_last = 0xFFFFFFFFU, /**< Sentinel: "last page of this chapter".  */
} er_pageturn_const_t;

/**
 * @brief Decide a page-turn direction from a touch X coordinate (edge taps).
 *
 * @details Right third of the screen = next page, left third = previous, and the
 * middle third is neutral (reserved for in-content links / future menus). The
 * boundaries use integer thirds of @p width.
 *
 * @param[in] x     Tap X in panel pixels.
 * @param[in] width Panel width in pixels (> 0).
 * @return ::k_er_dir_next (right third), ::k_er_dir_prev (left third), or
 *         ::k_er_dir_none (middle third, or @p width == 0).
 * @pre @p width is the live framebuffer width.
 * @pre @p x is a panel-space coordinate.
 * @post The return value is one of the three ::er_dir_t members.
 * @note Pure; not stateful.
 * @since 0.1.0
 */
static inline er_dir_t er_tap_to_dir(int32_t x, int32_t width)
{
  if (width <= 0) {
    return k_er_dir_none;
  }
  const int32_t third = width / (int32_t)k_er_pt_thirds;
  if (x >= (width - third)) {
    return k_er_dir_next;
  }
  if (x < third) {
    return k_er_dir_prev;
  }
  return k_er_dir_none;
}

/**
 * @brief Decide a page-turn direction from the two user-button states.
 *
 * @details SW1 = previous page, SW2 = next page. If both are pressed, previous
 * wins (deterministic). Inputs are the logical "pressed" states (the caller maps
 * the active-low GPIO levels to booleans).
 *
 * @param[in] sw1_pressed true if SW1 (P009) is held.
 * @param[in] sw2_pressed true if SW2 (P008) is held.
 * @return ::k_er_dir_prev, ::k_er_dir_next, or ::k_er_dir_none.
 * @pre The booleans reflect a fresh (edge-triggered) press.
 * @pre Caller has already debounced / edge-detected.
 * @post The return value is one of the three ::er_dir_t members.
 * @note Pure; not stateful.
 * @since 0.1.0
 */
static inline er_dir_t er_buttons_to_dir(bool sw1_pressed, bool sw2_pressed)
{
  if (sw1_pressed) {
    return k_er_dir_prev;
  }
  if (sw2_pressed) {
    return k_er_dir_next;
  }
  return k_er_dir_none;
}

/**
 * @brief Advance/retreat the reading location by one page, crossing chapters.
 *
 * @details
 * Within a chapter, moves @p page by one in @p dir. At a chapter edge it crosses:
 * NEXT past the last page goes to page 0 of the next chapter; PREV before page 0
 * goes to ::k_er_page_last of the previous chapter (the caller's render clamps
 * that sentinel to the real last page once the new chapter is laid out, since the
 * page count is not known here). Clamps at the book ends (no change). A cross is
 * reported in @p out_crossed so the caller can pick a clean chapter-boundary
 * refresh.
 *
 * @param[in]  chapter   Current chapter index.
 * @param[in]  page      Current page within @p chapter.
 * @param[in]  pages     Page count of @p chapter (>= 1).
 * @param[in]  ch_count  Number of chapters in the spine (>= 1).
 * @param[in]  dir       ::er_dir_t direction.
 * @param[out] out_chap  Receives the new chapter index.
 * @param[out] out_page  Receives the new page (or ::k_er_page_last on PREV cross).
 * @param[out] out_crossed Receives true iff a chapter boundary was crossed.
 * @return true iff the location changed (caller re-renders); false at a book end
 *         or ::k_er_dir_none.
 * @pre @p pages >= 1 and @p ch_count >= 1.
 * @pre All out-params are non-null.
 * @post On true, (@p out_chap, @p out_page) is the new location.
 * @post On false, the out-params are still written to the unchanged location.
 * @note Pure; not stateful.
 * @since 0.1.0
 *
 * @par MC/DC:
 * Documented and exercised in tests/test_ereader_pageturn.c.
 */
static inline bool er_pageturn_step(uint32_t  chapter,
                                    uint32_t  page,
                                    uint32_t  pages,
                                    uint32_t  ch_count,
                                    er_dir_t  dir,
                                    uint32_t* out_chap,
                                    uint32_t* out_page,
                                    bool*     out_crossed)
{
  *out_chap    = chapter;
  *out_page    = page;
  *out_crossed = false;
  if (dir == k_er_dir_next) {
    if ((page + 1U) < pages) {
      *out_page = page + 1U;
      return true;
    }
    if ((chapter + 1U) < ch_count) {
      *out_chap    = chapter + 1U;
      *out_page    = 0U;
      *out_crossed = true;
      return true;
    }
    return false;
  }
  if (dir == k_er_dir_prev) {
    if (page > 0U) {
      *out_page = page - 1U;
      return true;
    }
    if (chapter > 0U) {
      *out_chap    = chapter - 1U;
      *out_page    = (uint32_t)k_er_page_last;
      *out_crossed = true;
      return true;
    }
    return false;
  }
  return false;
}
