/**
 * @file ra8_ui.h
 * @brief Bounded UI interaction core: hit-testing, screen stack, paging.
 * @ingroup grp_ereader
 *
 * @details
 * `ra8_ui` is the interaction / controller layer for the e-reader UI
 * (issue #80). The rendering engines (`ra8_reflow`, `ra8_gfx`) emit boxes
 * and glyphs; none of them model *interaction*. This library fills that
 * gap with three small, allocation-free, MC/DC-able units:
 *
 *   - **Hit-testing** -- given a flat list of tap targets (rectangles +
 *     an opaque action id) and a point, find the target the user tapped.
 *     This is how a tap on the framebuffer becomes an app action.
 *   - **Screen-stack navigation** -- a fixed-depth stack of screen ids
 *     with push / pop / replace, modelling modal + drill-down flows
 *     (Library -> Reading -> Table-of-Contents -> back).
 *   - **Paging** -- a clamped (current, total) page cursor for the
 *     reading view and any other paginated surface.
 *
 * Everything is caller-owned plain data (no globals, no allocation), so
 * the same logic runs identically on the host test harness and on the
 * RA8D2. It is deliberately renderer-agnostic: callers pair it with
 * `ra8_gfx` today and `ra8_reflow` box rectangles later.
 *
 *
 * [Ring 5 / UI]
 * {World: NS}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_err.h"

/* ===========================================================================
 * Compile-time limits
 * ===========================================================================
 */

/**
 * @enum ra8_ui_limits_t
 * @brief Static-allocation caps for the interaction core.
 */
typedef enum : uint16_t {
  k_ra8_ui_nav_max_depth = 8U, /**< Max screen-stack depth. */
} ra8_ui_limits_t;

/* ===========================================================================
 * Geometry + hit-testing
 * ===========================================================================
 */

/**
 * @struct ra8_ui_rect_t
 * @brief Axis-aligned rectangle in framebuffer pixel coordinates.
 *
 * @details
 * Inclusive on the top-left corner, exclusive on the bottom-right, so a
 * rect of `w = h = 0` contains no points and adjacent rects do not
 * overlap on their shared edge.
 */
typedef struct {
  int32_t x; /**< Left edge (pixels).    */
  int32_t y; /**< Top edge (pixels).     */
  int32_t w; /**< Width (pixels, >= 0).  */
  int32_t h; /**< Height (pixels, >= 0). */
} ra8_ui_rect_t;

/**
 * @struct ra8_ui_target_t
 * @brief One tap target: a rectangle bound to an opaque action id.
 */
typedef struct {
  ra8_ui_rect_t rect;      /**< Hit rectangle.                       */
  uint16_t      action_id; /**< Caller-defined action for this rect. */
  uint16_t      reserved;  /**< Padding to 4-byte align.             */
} ra8_ui_target_t;

/**
 * @brief Test whether a point lies inside a rectangle.
 *
 * @details
 * Top-left inclusive, bottom-right exclusive:
 * `(px >= x) && (px < x + w) && (py >= y) && (py < y + h)`.
 *
 * @param[in] r  Rectangle to test (NULL yields false).
 * @param[in] px Point X (pixels).
 * @param[in] py Point Y (pixels).
 *
 * @return true if the point is inside @p r, false otherwise.
 * @retval true  The point lies within @p r.
 * @retval false The point is outside @p r, or @p r is NULL.
 *
 * @pre None (NULL @p r is handled).
 * @pre None.
 * @post No state is modified.
 * @post Return value reflects containment only.
 *
 * @note Pure; thread-safe.
 * @since 0.1.0
 */
bool ra8_ui_rect_contains(const ra8_ui_rect_t* r, int32_t px, int32_t py);

/**
 * @brief Find the first tap target containing a point.
 *
 * @details
 * Walks @p targets in order and returns the action id of the first
 * rectangle that contains `(px, py)`. Earlier entries win on overlap, so
 * callers should order more-specific targets first.
 *
 * @param[in]  targets    Target array (may be NULL only if count == 0).
 * @param[in]  count      Number of targets.
 * @param[in]  px         Point X (pixels).
 * @param[in]  py         Point Y (pixels).
 * @param[out] out_action Receives the hit target's action id.
 * @param[out] out_hit    Receives true if a target was hit, else false.
 *
 * @return ra8_err_t
 * @retval k_ra8_ok           Search completed (see @p out_hit).
 * @retval k_ra8_err_null_ptr @p out_action / @p out_hit NULL, or
 *                           @p targets NULL while @p count > 0.
 *
 * @pre @p out_action and @p out_hit are non-NULL.
 * @pre @p targets covers @p count entries.
 * @post On a hit, `*out_hit == true` and `*out_action` is the hit id.
 * @post On a miss, `*out_hit == false` and `*out_action` is unchanged.
 *
 * @note Pure; thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_ui_hit_test(const ra8_ui_target_t* targets,
                                        uint16_t               count,
                                        int32_t                px,
                                        int32_t                py,
                                        uint16_t*              out_action,
                                        bool*                  out_hit);

/* ===========================================================================
 * Screen-stack navigation
 * ===========================================================================
 */

/**
 * @struct ra8_ui_nav_t
 * @brief Fixed-depth screen-id stack (caller-owned).
 *
 * @details
 * `stack[0]` is the root screen (never popped). `depth` is the number of
 * occupied slots (always >= 1 once initialised). Screen ids are opaque
 * `uint16_t` values the caller assigns.
 */
typedef struct {
  uint16_t stack[k_ra8_ui_nav_max_depth]; /**< Screen-id stack.         */
  uint8_t  depth;                         /**< Occupied slots (1..max). */
  uint8_t  reserved[3];                   /**< Padding.                 */
} ra8_ui_nav_t;

/**
 * @brief Initialise a navigation stack with a root screen.
 *
 * @param[out] nav         Stack to initialise.
 * @param[in]  root_screen Screen id placed at the stack base.
 *
 * @return ra8_err_t
 * @retval k_ra8_ok           Initialised (`depth == 1`).
 * @retval k_ra8_err_null_ptr @p nav is NULL.
 *
 * @pre @p nav is non-NULL.
 * @pre None.
 * @post `nav->depth == 1` and `nav->stack[0] == root_screen`.
 * @post No other slots are read.
 *
 * @note Not thread-safe (caller-owned mutable state).
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_ui_nav_init(ra8_ui_nav_t* nav, uint16_t root_screen);

/**
 * @brief Push a new screen onto the stack.
 *
 * @param[in,out] nav    Initialised stack.
 * @param[in]     screen Screen id to push.
 *
 * @return ra8_err_t
 * @retval k_ra8_ok              Pushed (`depth` incremented).
 * @retval k_ra8_err_null_ptr    @p nav is NULL.
 * @retval k_ra8_err_no_mem      Stack already at `k_ra8_ui_nav_max_depth`.
 * @retval k_ra8_err_invalid_state Stack not initialised (`depth == 0`).
 *
 * @pre @p nav is non-NULL and initialised.
 * @pre `nav->depth < k_ra8_ui_nav_max_depth` for success.
 * @post On success the new screen is on top and `depth` grew by one.
 * @post On overflow the stack is unchanged.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_ui_nav_push(ra8_ui_nav_t* nav, uint16_t screen);

/**
 * @brief Pop the top screen, revealing the one beneath.
 *
 * @details The root screen is never popped (a pop at `depth == 1` fails).
 *
 * @param[in,out] nav        Initialised stack.
 * @param[out]    out_screen Receives the new top screen after the pop.
 *
 * @return ra8_err_t
 * @retval k_ra8_ok              Popped; `*out_screen` is the new top.
 * @retval k_ra8_err_null_ptr    @p nav or @p out_screen is NULL.
 * @retval k_ra8_err_invalid_state Already at the root (`depth <= 1`).
 *
 * @pre @p nav and @p out_screen are non-NULL.
 * @pre `nav->depth > 1` for success.
 * @post On success `depth` shrank by one and `*out_screen` is the top.
 * @post At the root the stack is unchanged.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_ui_nav_pop(ra8_ui_nav_t* nav, uint16_t* out_screen);

/**
 * @brief Replace the top screen in place (no depth change).
 *
 * @param[in,out] nav    Initialised stack.
 * @param[in]     screen Screen id to install on top.
 *
 * @return ra8_err_t
 * @retval k_ra8_ok              Replaced.
 * @retval k_ra8_err_null_ptr    @p nav is NULL.
 * @retval k_ra8_err_invalid_state Stack not initialised (`depth == 0`).
 *
 * @pre @p nav is non-NULL and initialised.
 * @pre None.
 * @post `nav->stack[depth-1] == screen` and `depth` is unchanged.
 * @post No other slots are touched.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_ui_nav_replace(ra8_ui_nav_t* nav, uint16_t screen);

/**
 * @brief Read the current (top) screen id.
 *
 * @param[in]  nav        Initialised stack.
 * @param[out] out_screen Receives the top screen id.
 *
 * @return ra8_err_t
 * @retval k_ra8_ok              Reported.
 * @retval k_ra8_err_null_ptr    @p nav or @p out_screen is NULL.
 * @retval k_ra8_err_invalid_state Stack not initialised (`depth == 0`).
 *
 * @pre @p nav and @p out_screen are non-NULL.
 * @pre `nav->depth >= 1`.
 * @post `*out_screen` is `nav->stack[depth-1]`.
 * @post No state is modified.
 *
 * @note Pure read; not thread-safe vs concurrent mutation.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_ui_nav_top(const ra8_ui_nav_t* nav, uint16_t* out_screen);

/* ===========================================================================
 * Paging
 * ===========================================================================
 */

/**
 * @struct ra8_ui_pager_t
 * @brief Clamped (current, total) page cursor.
 */
typedef struct {
  uint16_t current; /**< Current 0-based page index. */
  uint16_t total;   /**< Total page count (>= 1).    */
} ra8_ui_pager_t;

/**
 * @brief Initialise a pager over @p total pages at page 0.
 *
 * @param[out] p     Pager to initialise.
 * @param[in]  total Total page count (must be >= 1).
 *
 * @return ra8_err_t
 * @retval k_ra8_ok            Initialised at page 0.
 * @retval k_ra8_err_null_ptr  @p p is NULL.
 * @retval k_ra8_err_invalid_arg @p total is 0.
 *
 * @pre @p p is non-NULL and @p total >= 1.
 * @pre None.
 * @post `p->current == 0` and `p->total == total`.
 * @post No other state exists.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_ui_pager_init(ra8_ui_pager_t* p, uint16_t total);

/**
 * @brief Advance to the next page, clamping at the last page.
 *
 * @param[in,out] p           Initialised pager.
 * @param[out]    out_changed Receives true if the page actually moved.
 *
 * @return ra8_err_t
 * @retval k_ra8_ok           Done (see @p out_changed).
 * @retval k_ra8_err_null_ptr @p p or @p out_changed is NULL.
 *
 * @pre @p p and @p out_changed are non-NULL; `p->total >= 1`.
 * @pre None.
 * @post `p->current` is `min(current+1, total-1)`.
 * @post `*out_changed` is true iff `current` moved.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_ui_pager_next(ra8_ui_pager_t* p, bool* out_changed);

/**
 * @brief Step to the previous page, clamping at page 0.
 *
 * @param[in,out] p           Initialised pager.
 * @param[out]    out_changed Receives true if the page actually moved.
 *
 * @return ra8_err_t
 * @retval k_ra8_ok           Done (see @p out_changed).
 * @retval k_ra8_err_null_ptr @p p or @p out_changed is NULL.
 *
 * @pre @p p and @p out_changed are non-NULL.
 * @pre None.
 * @post `p->current` is `max(current-1, 0)`.
 * @post `*out_changed` is true iff `current` moved.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_ui_pager_prev(ra8_ui_pager_t* p, bool* out_changed);

/**
 * @brief Jump to an absolute page, clamping into `[0, total-1]`.
 *
 * @param[in,out] p           Initialised pager.
 * @param[in]     page        Requested 0-based page index.
 * @param[out]    out_changed Receives true if the page actually moved.
 *
 * @return ra8_err_t
 * @retval k_ra8_ok           Done (see @p out_changed).
 * @retval k_ra8_err_null_ptr @p p or @p out_changed is NULL.
 *
 * @pre @p p and @p out_changed are non-NULL; `p->total >= 1`.
 * @pre None.
 * @post `p->current` is @p page clamped to `[0, total-1]`.
 * @post `*out_changed` is true iff `current` moved.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_ui_pager_goto(ra8_ui_pager_t* p, uint16_t page, bool* out_changed);

#ifdef __cplusplus
}
#endif
