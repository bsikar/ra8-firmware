/**
 * @file ra8_widget_progress_bar.h
 * @brief Progress-bar leaf widget for the ra8_widget tree model (#145 Phase 2).
 * @ingroup grp_ereader
 *
 * @details
 * The reading footer and every book card in the e-reader chrome draw the same
 * shape: a soft track rectangle with a proportional filled portion showing how
 * far through a book (or chapter) the reader is. ::ra8_widget_progress_bar_t
 * extracts that into a reusable ::ra8_widget leaf: it fills its rect with the
 * `track` colour, then overpaints the leftmost `value / total` fraction with the
 * `fill` colour. Like every leaf in this library it paints through the injected
 * ::ra8_widget_paint_t backend, so the widget carries no `ra8_gfx` dependency and
 * the fill-fraction maths is host-testable with a recording mock. It is
 * display-only, so its vtable leaves `on_input` NULL.
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
 * @struct ra8_widget_progress_bar_t
 * @brief A proportional progress bar: a track plus a `value / total` fill.
 *
 * @details
 * Caller-owned plain data bound to a ::ra8_widget_t with
 * ::ra8_widget_progress_bar_init. `render` fills the whole rect with `track`,
 * then fills the leftmost `clamp(value, 0, total) / total` fraction with `fill`.
 * A `total` of 0 or a zero-width rect paints the track only (no divide, no fill);
 * a `value >= total` paints a full bar. The bar is horizontal (fills left to
 * right); rotate the containing panel for a vertical gauge.
 *
 * @invariant `total == 0` is treated as "empty" (0% fill), never a divide.
 * @invariant The filled portion never exceeds the widget rect width.
 *
 * @par Example:
 * @code
 * static const ra8_widget_paint_t paint = { ... };
 * ra8_widget_progress_bar_t bar = {.paint = &paint, .track = 0x00303030U,
 *                                  .fill = 0x00E0A020U, .value = 3, .total = 10};
 * ra8_widget_t w = {};
 * (void)ra8_widget_progress_bar_init(&w, &bar);  // 30% bar
 * @endcode
 *
 * @see ra8_widget_progress_bar_init
 * @since 0.1.0
 */
typedef struct ra8_widget_progress_bar {
  const ra8_widget_paint_t* paint; /**< Draw backend (NULL -> draws nothing).      */
  uint32_t                  track; /**< Track (unfilled) fill colour, 0xRRGGBB.    */
  uint32_t                  fill;  /**< Filled-portion colour, 0xRRGGBB.           */
  uint16_t                  value; /**< Current progress value (clamped to total). */
  uint16_t                  total; /**< Full-scale value; 0 -> empty (no fill).    */
} ra8_widget_progress_bar_t;

/**
 * @brief Return the shared vtable backing every progress-bar widget.
 *
 * @details
 * One immutable vtable serves all progress bars: `render` fills the track then
 * the proportional fill through the bar's ::ra8_widget_paint_t; `measure` and
 * `on_input` are NULL (a progress bar sizes from its parent's fixed/flex and
 * never consumes input). ::ra8_widget_progress_bar_init binds it, so callers
 * rarely need it directly -- it is exposed for tests and hand-built widgets.
 *
 * @return Non-NULL pointer to the static progress-bar vtable.
 *
 * @pre None.
 * @pre None.
 * @post The returned pointer is non-NULL and references static storage.
 * @post No state is modified.
 *
 * @note Pure; thread-safe (returns a pointer to immutable static data).
 * @see ra8_widget_progress_bar_init
 * @since 0.1.0
 */
const ra8_widget_vtable_t* ra8_widget_progress_bar_vtable(void);

/**
 * @brief Bind a widget instance to a progress bar.
 *
 * @details
 * Wires @p w to render @p bar: sets its vtable to
 * ::ra8_widget_progress_bar_vtable, points its `ctx` at @p bar, and makes it
 * visible. The caller still sets @p w's `fixed` / `flex` for its parent's
 * layout.
 *
 * @param[in,out] w   Widget to turn into a progress bar (non-NULL).
 * @param[in]     bar Progress-bar descriptor (non-NULL; outlives @p w's use).
 *
 * @return ra8_err_t
 * @retval k_ra8_ok           Bound; @p w renders @p bar.
 * @retval k_ra8_err_null_ptr @p w or @p bar is NULL.
 *
 * @pre @p w and @p bar are non-NULL.
 * @pre @p bar outlives every render of @p w.
 * @post On success `w->vt == ra8_widget_progress_bar_vtable()`, `w->ctx == bar`,
 *       `w->visible == true`.
 * @post On failure @p w is left unchanged.
 *
 * @note Not thread-safe.
 * @see ra8_widget_progress_bar_vtable
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_widget_progress_bar_init(ra8_widget_t*              w,
                                                     ra8_widget_progress_bar_t* bar);

#ifdef __cplusplus
}
#endif
