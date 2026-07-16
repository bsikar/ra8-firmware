/**
 * @file ra8_widget_internal.h
 * @brief Module-private paint helpers shared by the concrete leaf-widget TUs.
 * @ingroup grp_ereader
 *
 * @details
 * The concrete leaf widgets (::ra8_widget_label_t in `ra8_widget_label.c` and
 * ::ra8_widget_button_t in `ra8_widget_button.c`) both place aligned text and
 * fill a (optionally bordered) box through the injected ::ra8_widget_paint_t
 * backend. Rather than duplicate that geometry in each TU -- which would also
 * duplicate the branch-coverage obligation -- the two small helpers live in
 * `ra8_widget_paint.c` and are declared here as `RA8_PRIV` (module-private,
 * shared across TUs of this one library). They remain pure compute + callback
 * dispatch: they carry no `ra8_gfx` dependency, so the library stays graphics
 * free and the helpers are exercised on the host through the public render
 * callbacks with a recording mock paint.
 *
 * Not part of the public surface: production callers use the widget vtables;
 * the only out-of-TU consumers are the sibling widget TUs and the host tests.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * [Ring 5 / UI]
 * {World: NS}
 *
 * @since 0.1.0
 */

#pragma once

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_widget.h"

/**
 * @brief Compute where a leaf widget's text should start within its rect.
 *
 * @details
 * Resolves the top-left pen position for a string drawn inside @p rect. The
 * horizontal coordinate follows @p align (left/centre/right); centre and right
 * need the string's pixel width, so they require a `text_size` callback on
 * @p paint and otherwise fall back to the left inset. When the width is
 * measurable the vertical coordinate centres the glyph cell in the rect, else
 * it uses the top inset. Each guard is a single condition (no compound
 * decision), so MC/DC reduces to branch coverage of each early-out. The widget
 * render callbacks guard @p paint and @p text non-NULL before delegating here,
 * so neither is re-checked.
 *
 * @param[in]  paint Draw backend (non-NULL); its `text_size` measures the text.
 * @param[in]  rect  The widget rectangle the text is placed inside (non-NULL).
 * @param[in]  text  NUL-terminated string (non-NULL; read only for centre/right).
 * @param[in]  pad   Inner inset from the rect edges, pixels.
 * @param[in]  align Horizontal alignment selector.
 * @param[out] out_x Receives the pen X (left edge of the first glyph).
 * @param[out] out_y Receives the pen Y (top edge of the glyph row).
 *
 * @return Nothing.
 *
 * @pre @p paint, @p rect, @p text, @p out_x and @p out_y are non-NULL.
 * @pre @p paint->text_size, if set, fills both out dimensions.
 * @post `*out_x` / `*out_y` hold a pen position inside or at the rect inset.
 * @post No backend draw call is issued (measurement only).
 *
 * @note Pure compute; not thread-safe vs concurrent backend mutation.
 *
 * @par MC/DC:
 * Single-condition early-outs (`align == left`, `text_size == NULL`,
 * `align == right`) -- each driven both true and false through the label /
 * button render with a mock paint that does / does not provide `text_size`.
 *
 * @since 0.1.0
 */
RA8_PRIV void ra8_widget_priv_text_pos(const ra8_widget_paint_t* paint,
                                       const ra8_ui_rect_t*      rect,
                                       const char*               text,
                                       int16_t                   pad,
                                       ra8_widget_align_t        align,
                                       int32_t*                  out_x,
                                       int32_t*                  out_y);

/**
 * @brief Fill a widget rect with an optional 1-or-more-pixel border.
 *
 * @details
 * With @p border_w <= 0 this is a single `fill_rect` of @p fill over @p rect.
 * Otherwise it paints @p border over the whole rect then @p fill over the rect
 * inset by @p border_w on every side, yielding a framed face in two fills. A
 * NULL `fill_rect` callback is a no-op, so a widget with a non-drawing backend
 * is safe. The widget render callbacks guard @p paint non-NULL before
 * delegating here, so it is not re-checked.
 *
 * @param[in] paint    Draw backend (non-NULL; NULL `fill_rect` -> no-op).
 * @param[in] rect     Rectangle to fill (non-NULL).
 * @param[in] fill     Interior fill colour, 0xRRGGBB.
 * @param[in] border   Border colour, 0xRRGGBB (used only when border_w > 0).
 * @param[in] border_w Border thickness in pixels; <= 0 means no border.
 *
 * @return Nothing.
 *
 * @pre @p paint and @p rect are non-NULL.
 * @pre @p border_w is the configured thickness (>= 0 by widget invariant).
 * @post At most two `fill_rect` calls are issued on @p paint.
 * @post No pixels are touched when @p paint->fill_rect is NULL.
 *
 * @note Not thread-safe vs concurrent backend mutation.
 *
 * @par MC/DC:
 * Single-condition guards (`fill_rect == NULL`, `border_w <= 0`) -- each
 * exercised true and false via the label (border_w 0) and button (border_w > 0)
 * renders plus a NULL-`fill_rect` backend vector.
 *
 * @since 0.1.0
 */
RA8_PRIV void ra8_widget_priv_fill_box(const ra8_widget_paint_t* paint,
                                       const ra8_ui_rect_t*      rect,
                                       uint32_t                  fill,
                                       uint32_t                  border,
                                       int16_t                   border_w);

/**
 * @brief Filled width in pixels for a `value / total` bar spanning @p width.
 *
 * @details
 * The proportional-fill maths shared by the progress-bar leaf and the book-card
 * progress indicator: clamp @p value into `[0, total]` and guard the degenerate
 * inputs so no caller divides by zero or paints past the rect. A zero @p total or
 * a non-positive @p width fills nothing; a `value >= total` fills the whole
 * @p width; otherwise the fill is `width * value / total`. Pure integer compute,
 * no backend call, so it is exercised on the host through both widgets' renders.
 *
 * @param[in] value Current progress value.
 * @param[in] total Full-scale value; 0 means empty (no fill).
 * @param[in] width Rect width in pixels the fill spans.
 *
 * @return Filled width in pixels, in `[0, width]`.
 * @retval 0     When @p total is 0, @p width is non-positive, or @p value is 0.
 * @retval width When @p value is at or above @p total (a full bar).
 *
 * @pre None.
 * @pre None.
 * @post The result is >= 0 and <= @p width.
 * @post No state is modified.
 *
 * @note Pure; thread-safe.
 *
 * @par MC/DC:
 * Single-condition guards (`total == 0`, `width <= 0`, `value > total`) -- each
 * driven both true and false through the progress-bar render (empty / partial /
 * full / zero-width vectors) and the book-card render.
 *
 * @since 0.1.0
 */
RA8_PRIV int32_t ra8_widget_priv_fill_frac(uint32_t value, uint32_t total, int32_t width);
