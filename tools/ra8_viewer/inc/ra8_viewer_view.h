/**
 * @file ra8_viewer_view.h
 * @brief Plain-C ABI for the viewer's desktop reader window (Cocoa on macOS).
 *
 * @details
 * The window is a native `NSScrollView` that reads the document as a continuous
 * vertical strip: it pulls tiles from the reader (::ra8_viewer_tile_count /
 * ::ra8_viewer_render_tile565), scales each to the window width, and stacks them
 * so the whole series scrolls smoothly and endlessly with the trackpad, wheel,
 * scrollbar, or Page/Home/End keys. Tiles are rendered lazily as they scroll into
 * view and dropped when far away, so a long chapter stays within a bounded memory
 * budget. Resizing the window re-fits every tile to the new width.
 *
 * The window runs cooperatively: ::ra8_viewer_view_pump drains pending events from
 * the caller's loop rather than taking over with `[NSApp run]`. The implementation
 * is Objective-C (ra8_viewer_view.m); this header stays pure C so the C reader and
 * `main` need no Objective-C.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * @since 0.1.0
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Opaque reader handle (defined in ra8_viewer_reader.h). */
typedef struct ra8_viewer_reader ra8_viewer_reader_t;

/**
 * @struct ra8_viewer_view
 * @brief Opaque handle to the desktop reader window.
 * @since 0.1.0
 */
typedef struct ra8_viewer_view ra8_viewer_view_t;

/**
 * @brief Open a resizable, fit-to-width scrolling reader window over @p reader.
 * @param[in] reader Open reader whose tiles the window renders (non-NULL).
 * @param[in] title  Window title (NUL-terminated, may be NULL).
 * @return New view handle, or NULL if a window could not be created (e.g. a
 *         headless host with no display).
 * @note The view borrows @p reader; the reader must outlive the view.
 * @since 0.1.0
 */
ra8_viewer_view_t* ra8_viewer_view_open(ra8_viewer_reader_t* reader, const char* title);

/**
 * @brief Drain pending window events cooperatively.
 * @param[in] view View handle (NULL treated as closed).
 * @return true when the window has been closed (the caller should exit).
 * @since 0.1.0
 */
bool ra8_viewer_view_pump(ra8_viewer_view_t* view);

/**
 * @brief Close the window and release the view handle.
 * @param[in] view View handle (NULL is ignored).
 * @since 0.1.0
 */
void ra8_viewer_view_close(ra8_viewer_view_t* view);

#ifdef __cplusplus
}
#endif
