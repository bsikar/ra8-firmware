/**
 * @file ra8_widget_toolbar.h
 * @brief Toolbar leaf widget for the ra8_widget tree model (#145 Phase 2).
 * @ingroup grp_ereader
 *
 * @details
 * The e-reader Library screen carries a toolbar: a wide search field on the left
 * (a bordered box showing a placeholder hint) and a fixed-width count chip on the
 * right (e.g. "12 books"). Tapping the search field opens the on-screen keyboard.
 * ::ra8_widget_toolbar_t extracts that into a reusable ::ra8_widget leaf. `render`
 * fills the band, draws the bordered search field with its hint and the
 * right-aligned count text, all through the injected ::ra8_widget_paint_t backend
 * (no `ra8_gfx` dependency). `on_input` treats the search field as a tap target:
 * a touch inside it bumps `searches`, self-invalidates fast, and fires the
 * optional `on_search` callback; a tap on the count chip (or a button event) is
 * declined so it keeps routing.
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
 * @struct ra8_widget_toolbar_t
 * @brief A search-field + count-chip toolbar band with a tappable field.
 *
 * @details
 * Caller-owned plain data bound to a ::ra8_widget_t with
 * ::ra8_widget_toolbar_init. The band is inset by `pad`; the count chip takes a
 * fixed `count_w` on the right and the search field flexes to fill the rest.
 * `render` fills `bg`, draws the `border`-framed `field` box with the `hint`
 * placeholder, then the right-aligned `count` text. `on_input` consumes a touch
 * that lands inside the search-field rect (incrementing `searches` and firing
 * `on_search`); everything else is declined.
 *
 * @invariant `searches` only ever increases.
 * @invariant `border_w >= 0`; `count_w >= 0`.
 *
 * @par Example:
 * @code
 * ra8_widget_toolbar_t tb = {.paint = &paint, .hint = "Search",
 *                            .count = "12 books", .on_search = open_kbd,
 *                            .bg = 0x00FFFFFFU, .field = 0x00F0F0F0U,
 *                            .border = 0x00C0C0C0U, .hint_fg = 0x00909090U,
 *                            .count_fg = 0x00909090U, .pad = 8, .border_w = 1,
 *                            .count_w = 72};
 * ra8_widget_t w = {};
 * (void)ra8_widget_toolbar_init(&w, &tb);
 * @endcode
 *
 * @see ra8_widget_toolbar_init
 * @since 0.1.0
 */
typedef struct ra8_widget_toolbar {
  const ra8_widget_paint_t* paint;         /**< Draw backend (NULL -> draws nothing).     */
  const char*               hint;          /**< Search-field placeholder (NULL -> none).  */
  const char*               count;         /**< Right count chip text (NULL -> none).     */
  void (*on_search)(struct ra8_widget* w); /**< Tap-in-field callback (optional/NULL).    */
  uint32_t bg;                             /**< Band background colour, 0xRRGGBB.         */
  uint32_t field;                          /**< Search-field fill colour, 0xRRGGBB.       */
  uint32_t border;                         /**< Search-field border colour, 0xRRGGBB.     */
  uint32_t hint_fg;                        /**< Placeholder text colour, 0xRRGGBB.        */
  uint32_t count_fg;                       /**< Count chip text colour, 0xRRGGBB.         */
  uint32_t searches;                       /**< Count of search-field taps (observable).  */
  int16_t  pad;                            /**< Inner padding inset, pixels.              */
  int16_t  border_w;                       /**< Search-field border thickness, px (>= 0). */
  int16_t  count_w;                        /**< Fixed count-chip width, px (>= 0).        */
} ra8_widget_toolbar_t;

/**
 * @brief Return the shared vtable backing every toolbar widget.
 *
 * @details
 * One immutable vtable serves all toolbars: `render` fills the band, the bordered
 * search field + hint, and the count chip through the toolbar's
 * ::ra8_widget_paint_t; `on_input` latches a touch inside the search field and
 * declines everything else; `measure` is NULL. ::ra8_widget_toolbar_init binds
 * it.
 *
 * @return Non-NULL pointer to the static toolbar vtable.
 *
 * @pre None.
 * @pre None.
 * @post The returned pointer is non-NULL and references static storage.
 * @post No state is modified.
 *
 * @note Pure; thread-safe (returns a pointer to immutable static data).
 * @see ra8_widget_toolbar_init
 * @since 0.1.0
 */
const ra8_widget_vtable_t* ra8_widget_toolbar_vtable(void);

/**
 * @brief Bind a widget instance to a toolbar.
 *
 * @details
 * Wires @p w to render + route @p bar: sets its vtable to
 * ::ra8_widget_toolbar_vtable, points its `ctx` at @p bar, and makes it visible.
 * The caller still sets @p w's `fixed` / `flex` for its parent's layout.
 *
 * @param[in,out] w   Widget to turn into a toolbar (non-NULL).
 * @param[in]     bar Toolbar descriptor (non-NULL; outlives @p w's use).
 *
 * @return ra8_err_t
 * @retval k_ra8_ok           Bound; @p w renders + routes @p bar.
 * @retval k_ra8_err_null_ptr @p w or @p bar is NULL.
 *
 * @pre @p w and @p bar are non-NULL.
 * @pre @p bar outlives every render / dispatch of @p w.
 * @post On success `w->vt == ra8_widget_toolbar_vtable()`, `w->ctx == bar`,
 *       `w->visible == true`.
 * @post On failure @p w is left unchanged.
 *
 * @note Not thread-safe.
 * @see ra8_widget_toolbar_vtable
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_widget_toolbar_init(ra8_widget_t* w, ra8_widget_toolbar_t* bar);

#ifdef __cplusplus
}
#endif
