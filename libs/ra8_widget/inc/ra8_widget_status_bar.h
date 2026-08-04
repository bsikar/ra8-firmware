/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_widget_status_bar.h
 * @brief Status-bar leaf widget for the ra8_widget tree model (#145 Phase 2).
 * @ingroup grp_ereader
 *
 * @details
 * Both e-reader screens open with the same top band: a heading / wordmark hugging
 * the left, a status string (clock, battery) hugging the right, and a hairline
 * rule under the band. ::ra8_widget_status_bar_t extracts that into a reusable
 * ::ra8_widget leaf: it fills its rect with `bg`, draws the left-aligned `left`
 * text and the right-aligned `right` text (each vertically centred when the paint
 * backend can measure a string), then paints a `rule` hairline `rule_h` pixels
 * tall along its bottom edge. Painting goes through the injected
 * ::ra8_widget_paint_t backend, so the widget carries no `ra8_gfx` dependency and
 * is host-testable. It is display-only, so its vtable leaves `on_input` NULL.
 *
 *
 * [Ring 5 / UI]
 * {World: NS}
 *
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
 * @struct ra8_widget_status_bar_t
 * @brief A status band: a fill, a left + right label, and a bottom hairline.
 *
 * @details
 * Caller-owned plain data bound to a ::ra8_widget_t with
 * ::ra8_widget_status_bar_init. `render` fills the rect with `bg`, places `left`
 * at the left inset and `right` at the right inset (right alignment needs the
 * backend's `text_size`; without it the right text falls back to the left inset),
 * then fills a `rule`-coloured strip `rule_h` pixels tall at the band's bottom.
 * A NULL `left` / `right` draws no text; a `rule_h <= 0` draws no rule.
 *
 * @invariant `rule_h <= 0` disables the bottom hairline.
 * @invariant A NULL label string draws no text for that side.
 *
 * @par Example:
 * @code
 * ra8_widget_status_bar_t sb = {.paint = &paint, .left = "Library",
 *                               .right = "12:00", .bg = 0x00FFFFFFU,
 *                               .fg = 0x00101010U, .fg_right = 0x00808080U,
 *                               .rule = 0x00C0C0C0U, .pad = 8, .rule_h = 1};
 * ra8_widget_t w = {};
 * (void)ra8_widget_status_bar_init(&w, &sb);
 * @endcode
 *
 * @see ra8_widget_status_bar_init
 * @since 0.1.0
 */
typedef struct ra8_widget_status_bar {
  const ra8_widget_paint_t* paint;    /**< Draw backend (NULL -> draws nothing).    */
  const char*               left;     /**< Left-aligned text (NULL -> none).        */
  const char*               right;    /**< Right-aligned text (NULL -> none).       */
  uint32_t                  bg;       /**< Background fill colour, 0xRRGGBB.        */
  uint32_t                  fg;       /**< Left text colour, 0xRRGGBB.              */
  uint32_t                  fg_right; /**< Right text colour, 0xRRGGBB.             */
  uint32_t                  rule;     /**< Bottom hairline colour, 0xRRGGBB.        */
  int16_t                   pad;      /**< Inner text inset, pixels.                */
  int16_t                   rule_h;   /**< Hairline thickness in px (<= 0 -> none). */
} ra8_widget_status_bar_t;

/**
 * @brief Return the shared vtable backing every status-bar widget.
 *
 * @details
 * One immutable vtable serves all status bars: `render` fills the background,
 * draws the left + right labels and the bottom hairline through the bar's
 * ::ra8_widget_paint_t; `measure` and `on_input` are NULL (a status bar sizes
 * from its parent's fixed/flex and never consumes input).
 * ::ra8_widget_status_bar_init binds it, so callers rarely need it directly.
 *
 * @return Non-NULL pointer to the static status-bar vtable.
 *
 * @pre None.
 * @pre None.
 * @post The returned pointer is non-NULL and references static storage.
 * @post No state is modified.
 *
 * @note Pure; thread-safe (returns a pointer to immutable static data).
 * @see ra8_widget_status_bar_init
 * @since 0.1.0
 */
const ra8_widget_vtable_t* ra8_widget_status_bar_vtable(void);

/**
 * @brief Bind a widget instance to a status bar.
 *
 * @details
 * Wires @p w to render @p bar: sets its vtable to ::ra8_widget_status_bar_vtable,
 * points its `ctx` at @p bar, and makes it visible. The caller still sets @p w's
 * `fixed` / `flex` for its parent's layout.
 *
 * @param[in,out] w   Widget to turn into a status bar (non-NULL).
 * @param[in]     bar Status-bar descriptor (non-NULL; outlives @p w's use).
 *
 * @return ra8_err_t
 * @retval k_ra8_ok           Bound; @p w renders @p bar.
 * @retval k_ra8_err_null_ptr @p w or @p bar is NULL.
 *
 * @pre @p w and @p bar are non-NULL.
 * @pre @p bar outlives every render of @p w.
 * @post On success `w->vt == ra8_widget_status_bar_vtable()`, `w->ctx == bar`,
 *       `w->visible == true`.
 * @post On failure @p w is left unchanged.
 *
 * @note Not thread-safe.
 * @see ra8_widget_status_bar_vtable
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_widget_status_bar_init(ra8_widget_t* w, ra8_widget_status_bar_t* bar);

#ifdef __cplusplus
}
#endif
