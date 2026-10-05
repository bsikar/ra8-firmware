/**
 * @file ra8_widget_segmented.h
 * @brief One-of-N segmented control leaf widget for ra8_widget.
 * @ingroup grp_ereader
 *
 * @details
 * Labels, label storage, and selection are caller-owned. Segment widths divide
 * the widget width evenly, with remainder pixels assigned from the left. A
 * touch selects its segment and invalidates the widget rectangle.
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
 * @struct ra8_widget_segmented_t
 * @brief Caller-owned labels, colours, and selection for one-of-N options.
 * @details `labels` contains `count` NUL-terminated strings. `selected` must be
 * less than `count`; `count` is in the range 1 through 255.
 */
typedef struct ra8_widget_segmented {
  const ra8_widget_paint_t* paint; /**< Backend used for every draw operation. */
  const char* const*        labels; /**< `count` caller-owned labels. */
  uint32_t                  fg; /**< Unselected label foreground colour. */
  uint32_t                  selected_fg; /**< Selected label foreground colour. */
  uint32_t                  bg; /**< Unselected segment fill colour. */
  uint32_t                  selected_bg; /**< Selected segment fill colour. */
  uint32_t                  border; /**< Segment border colour. */
  uint8_t                   count; /**< Number of options, from 1 to 255. */
  uint8_t                   selected; /**< Selected option index. */
  uint16_t                  pad; /**< Text inset in pixels. */
} ra8_widget_segmented_t;

/** @brief Return the shared segmented-control vtable. @return Static vtable. */
const ra8_widget_vtable_t* ra8_widget_segmented_vtable(void);

/**
 * @brief Bind a widget to a caller-owned segmented control.
 * @param[in,out] w Widget instance.
 * @param[in] control Descriptor with labels and a valid selected index.
 * @return k_ra8_ok, k_ra8_err_null_ptr, or k_ra8_err_invalid_arg.
 * @pre w and control are non-NULL.
 * @pre control->labels is non-NULL and selected is less than count.
 * @post On success w is visible and points to control.
 * @post A changed selection invalidates w->rect.
 * @note Caller serializes descriptor access with rendering and input.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_widget_segmented_init(ra8_widget_t* w, ra8_widget_segmented_t* control);

#ifdef __cplusplus
}
#endif
