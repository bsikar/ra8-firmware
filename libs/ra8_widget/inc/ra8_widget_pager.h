/**
 * @file ra8_widget_pager.h
 * @brief Previous/Next paging control for fixed-size list or grid pages.
 * @ingroup grp_ereader
 *
 * @details
 * The pager shows Previous, Page X of Y, and Next. Its page is zero-based in
 * caller-owned state while its label is one-based. Touches in the left and
 * right thirds request one page backward or forward. A successful page change
 * uses the fast refresh hint and reports the widget rectangle as damage.
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
 * @struct ra8_widget_pager_t
 * @brief Caller-owned state for a page-count navigation control.
 *
 * @details
 * `item_count` rows/items are split into pages of `page_capacity`. A zero
 * item count or capacity has zero pages and displays "Page 0 of 0". The current
 * page is zero-based; initialization clamps it into the available range.
 * Render and input use only the injected paint backend and widget rectangle.
 *
 * @invariant The descriptor outlives every render or input dispatch.
 * @since 0.1.0
 */
typedef struct ra8_widget_pager {
  const ra8_widget_paint_t* paint;         /**< Draw backend (NULL -> draws nothing). */
  uint16_t                  item_count;    /**< Total rows or items across all pages. */
  uint16_t                  page_capacity; /**< Items shown on each page. */
  uint16_t                  page;          /**< Current zero-based page. */
  uint32_t                  bg;            /**< Background color, 0xRRGGBB. */
  uint32_t                  fg;            /**< Available control and label color. */
  uint32_t                  fg_disabled;   /**< Color for unavailable directions. */
  ra8_widget_text_face_t text_face;    /**< Text family; zero keeps sans. */
  ra8_widget_text_weight_t text_weight; /**< Text weight; zero keeps regular. */
  ra8_widget_text_size_t text_size;    /**< Text size; zero keeps size three. */
} ra8_widget_pager_t;

/**
 * @brief Return the shared vtable backing every pager.
 *
 * @return Non-NULL pointer to static vtable storage.
 * @since 0.1.0
 */
const ra8_widget_vtable_t* ra8_widget_pager_vtable(void);

/**
 * @brief Bind a widget to a caller-owned pager descriptor.
 *
 * @param[in,out] w Widget to bind.
 * @param[in,out] pager Pager state; initialization clamps its page.
 * @return ra8_err_t; null arguments return `k_ra8_err_null_ptr`.
 * @pre Both arguments are non-NULL and `pager` outlives the widget use.
 * @post On success, the widget is visible and bound to the pager.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_widget_pager_init(ra8_widget_t* w, ra8_widget_pager_t* pager);

#ifdef __cplusplus
}
#endif
