/**
 * @file ra8_widget_nav_bar.h
 * @brief Navigation-strip leaf widget for the ra8_widget tree model (#145 Phase 2).
 * @ingroup grp_ereader
 *
 * @details
 * The e-reader Library screen ends in a bottom navigation strip: a row of equal
 * destinations (Library / Store / Settings ...) with the active one inked and the
 * rest muted, each a tap target. ::ra8_widget_nav_bar_t extracts that into a
 * reusable ::ra8_widget leaf. `render` fills the strip and centres each item's
 * label in its equal-width cell (active vs muted colour) through the injected
 * ::ra8_widget_paint_t backend, so the widget carries no `ra8_gfx` dependency.
 * `on_input` maps a touch to the cell it landed in, records it in `selected`, and
 * fires the optional `on_select` callback; a button event is declined.
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
#include "ra8_widget.h"

/**
 * @struct ra8_widget_nav_bar_t
 * @brief An equal-cell navigation strip: N labels, one active, each tappable.
 *
 * @details
 * Caller-owned plain data bound to a ::ra8_widget_t with
 * ::ra8_widget_nav_bar_init. `items` is a caller-owned array of `count`
 * NUL-terminated labels; `active` is the highlighted (inked) index; `selected`
 * records the last cell a touch hit. `render` splits the rect into `count`
 * equal-width cells and centres each label (active -> `fg_active`, else
 * `fg_muted`). `on_input` maps a touch X to a cell index, stores it in
 * `selected`, and fires `on_select` -- the caller decides whether to move
 * `active` in response.
 *
 * @invariant `active < count` (else no item is inked).
 * @invariant `selected` holds a valid cell index after any touch is consumed.
 *
 * @par Example:
 * @code
 * static const char* const items[] = {"Library", "Store", "Settings"};
 * ra8_widget_nav_bar_t nav = {.paint = &paint, .items = items, .count = 3,
 *                             .active = 0, .bg = 0x00FFFFFFU,
 *                             .fg_active = 0x00101010U, .fg_muted = 0x00A0A0A0U};
 * ra8_widget_t w = {};
 * (void)ra8_widget_nav_bar_init(&w, &nav);
 * @endcode
 *
 * @see ra8_widget_nav_bar_init
 * @since 0.1.0
 */
typedef struct ra8_widget_nav_bar {
  const ra8_widget_paint_t* paint; /**< Draw backend (NULL -> draws nothing). */
  const char* const*        items; /**< Array of `count` label strings.       */
  /** Tap callback (opt/NULL). */
  void (*on_select)(struct ra8_widget* w, uint16_t index);
  uint32_t bg;        /**< Strip background colour, 0xRRGGBB.   */
  uint32_t fg_active; /**< Active-item text colour, 0xRRGGBB.   */
  uint32_t fg_muted;  /**< Inactive-item text colour, 0xRRGGBB. */
  uint16_t count;     /**< Number of items / cells.             */
  uint16_t active;    /**< Highlighted item index.              */
  uint16_t selected;  /**< Last-tapped item index (observable). */
} ra8_widget_nav_bar_t;

/**
 * @brief Return the shared vtable backing every navigation-strip widget.
 *
 * @details
 * One immutable vtable serves all nav strips: `render` fills the strip and
 * centres each label in its cell through the strip's ::ra8_widget_paint_t;
 * `on_input` maps a touch to a cell and fires `on_select`; `measure` is NULL.
 * ::ra8_widget_nav_bar_init binds it.
 *
 * @return Non-NULL pointer to the static nav-strip vtable.
 *
 * @pre None.
 * @pre None.
 * @post The returned pointer is non-NULL and references static storage.
 * @post No state is modified.
 *
 * @note Pure; thread-safe (returns a pointer to immutable static data).
 * @see ra8_widget_nav_bar_init
 * @since 0.1.0
 */
const ra8_widget_vtable_t* ra8_widget_nav_bar_vtable(void);

/**
 * @brief Bind a widget instance to a navigation strip.
 *
 * @details
 * Wires @p w to render + route @p nav: sets its vtable to
 * ::ra8_widget_nav_bar_vtable, points its `ctx` at @p nav, and makes it visible.
 * The caller still sets @p w's `fixed` / `flex` for its parent's layout.
 *
 * @param[in,out] w   Widget to turn into a nav strip (non-NULL).
 * @param[in]     nav Nav-strip descriptor (non-NULL; outlives @p w's use).
 *
 * @return ra8_err_t
 * @retval k_ra8_ok           Bound; @p w renders + routes @p nav.
 * @retval k_ra8_err_null_ptr @p w or @p nav is NULL.
 *
 * @pre @p w and @p nav are non-NULL.
 * @pre @p nav outlives every render / dispatch of @p w.
 * @post On success `w->vt == ra8_widget_nav_bar_vtable()`, `w->ctx == nav`,
 *       `w->visible == true`.
 * @post On failure @p w is left unchanged.
 *
 * @note Not thread-safe.
 * @see ra8_widget_nav_bar_vtable
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_widget_nav_bar_init(ra8_widget_t* w, ra8_widget_nav_bar_t* nav);

#ifdef __cplusplus
}
#endif
