/**
 * @file bedroom_ui.h
 * @brief Shared GUIX 3-tab bedroom UI (host + EK-RA8D2 panel)
 *
 * @details
 * Platform-independent GUIX widget tree: a tab bar of three buttons
 * (Primary / Secondary 1 / Secondary 2 Bedroom) over three swappable
 * screens. The same source builds for the macOS host preview (generic
 * RTOS bind + host 565rgb driver) and the panel (ThreadX + GLCDC) -- the
 * caller supplies a created ``GX_DISPLAY`` + root window, this module
 * populates it. Tab switching works through GUIX's own click events, so
 * it behaves identically under the host loop and the on-target loop.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * @since 0.1.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "gx_api.h"
#include "ra_err.h"

/**
 * @brief Build the tab UI into an existing display + root window.
 *
 * @details Installs a colour + font table on @p display, then creates a
 *          full-screen background window, the three tab buttons, and the
 *          three screen panels as descendants of @p root. Call
 *          ``gx_widget_show(root)`` afterwards.
 *
 * @param[in] display Created GUIX display (from gx_display_create).
 * @param[in] root    Created root window (from gx_window_root_create).
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok           UI built; tab 0 active.
 * @retval k_ra_err_null_ptr @p display or @p root was NULL.
 * @retval k_ra_fail         A GUIX widget call failed.
 *
 * @pre gx_system_initialize / gx_display_create / gx_canvas_create /
 *      gx_window_root_create have all succeeded.
 * @post The widget tree exists; tab 0 is the active screen.
 *
 * @since 0.1.0
 */
ra_err_t bedroom_ui_create(GX_DISPLAY* display, GX_WINDOW_ROOT* root);

/**
 * @brief Select the active tab / screen (also called from tab clicks).
 *
 * @param[in] tab Tab index in [0, bedroom_ui_tab_count()).
 *
 * @pre bedroom_ui_create has succeeded.
 * @post The chosen screen is shown, the others hidden, tab styling
 *       updated, and the affected widgets marked dirty.
 *
 * @since 0.1.0
 */
void bedroom_ui_set_active_tab(uint16_t tab);

/**
 * @brief Number of tabs / screens.
 *
 * @return uint16_t Tab count (3).
 *
 * @since 0.1.0
 */
uint16_t bedroom_ui_tab_count(void);

#ifdef __cplusplus
}
#endif
