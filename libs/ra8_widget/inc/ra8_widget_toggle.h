/**
 * @file ra8_widget_toggle.h
 * @brief Checkbox toggle leaf widget for the ra8_widget tree.
 * @ingroup grp_ereader
 *
 * @details
 * The descriptor is caller-owned and allocation-free. A touch toggles
 * `checked`, then invalidates the widget rectangle with the fast refresh hint.
 * Rendering uses only the injected widget paint backend.
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
 * @struct ra8_widget_toggle_t
 * @brief Caller-owned checkbox style and state.
 * @details `paint` and `label` may be NULL; state remains in `checked`.
 */
typedef struct ra8_widget_toggle {
  const ra8_widget_paint_t* paint; /**< Backend used for every draw operation. */
  const char*               label; /**< Optional text shown beside the box. */
  uint32_t                  fg; /**< Label foreground colour, 0xRRGGBB. */
  uint32_t                  bg; /**< Box and label background colour. */
  uint32_t                  border; /**< Unchecked box outline colour. */
  uint32_t                  mark; /**< Checked box fill colour. */
  uint16_t                  box_size; /**< Requested square size in pixels. */
  uint16_t                  gap; /**< Space between box and label in pixels. */
  bool                      checked; /**< Current checked state. */
  uint8_t                   reserved[3]; /**< ABI padding; initialize to zero. */
  ra8_widget_text_face_t text_face;    /**< Text family; zero keeps sans. */
  ra8_widget_text_weight_t text_weight; /**< Text weight; zero keeps regular. */
  ra8_widget_text_size_t text_size;    /**< Text size; zero keeps size three. */
} ra8_widget_toggle_t;

/** @brief Return the shared toggle vtable. @return Non-NULL static vtable. */
const ra8_widget_vtable_t* ra8_widget_toggle_vtable(void);

/**
 * @brief Bind a widget to a caller-owned checkbox toggle.
 * @param[in,out] w Widget instance.
 * @param[in] toggle Toggle descriptor that outlives the widget.
 * @return Error code; null pointers return k_ra8_err_null_ptr.
 * @pre w and toggle are non-NULL.
 * @pre toggle remains alive while w is rendered or receives input.
 * @post On success w is visible and points to toggle.
 * @post A touch changes checked and invalidates w->rect.
 * @note Caller serializes descriptor access with rendering and input.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_widget_toggle_init(ra8_widget_t* w, ra8_widget_toggle_t* toggle);

#ifdef __cplusplus
}
#endif
