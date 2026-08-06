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
 * @since 0.1.0
 *
 *

 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#include "ra8_host_arena.h"

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
 * @details Creates a native scrolling window that reads the document as one
 *          continuous vertical strip, pulling and scaling tiles lazily as they
 *          scroll into view. The view borrows @p reader and renders on demand; a
 *          headless host with no display returns NULL.
 * @param[in] reader Open reader whose tiles the window renders (non-NULL).
 * @param[in] title  Window title (NUL-terminated, may be NULL).
 * @return New view handle, or NULL if a window could not be created (e.g. a
 *         headless host with no display).
 * @retval NULL No window could be created on this host.
 * @pre @p reader is an open document that outlives the view.
 * @pre A window system is available.
 * @post On success a window is open and borrows @p reader.
 * @post On failure no window or view is created.
 * @note The view borrows @p reader; the reader must outlive the view. Not
 *       thread-safe; call on the main thread.
 * @since 0.1.0
 */
ra8_viewer_view_t*
ra8_viewer_view_open(ra8_viewer_reader_t* reader, const char* title, ra8_arena_t* arena);

/**
 * @brief Drain pending window events cooperatively.
 * @details Processes the events queued since the last call and returns without
 *          blocking, so the caller keeps its own run loop rather than handing
 *          control to the platform's blocking run call. Returns true once the
 *          user has closed the window.
 * @param[in] view View handle (NULL treated as closed).
 * @return true when the window has been closed (the caller should exit).
 * @retval true  The window has closed, or @p view is NULL.
 * @retval false The window is still open; keep pumping.
 * @pre @p view was returned by ::ra8_viewer_view_open, or is NULL.
 * @pre Called from the thread that created the view.
 * @post Events queued before the call have been processed.
 * @post The view is unchanged except for consumed events.
 * @note Not thread-safe; call on the main thread only.
 * @since 0.1.0
 */
bool ra8_viewer_view_pump(ra8_viewer_view_t* view);

/**
 * @brief Close the window and release the view handle.
 * @details Tears down the window and frees the view; the borrowed reader is not
 *          touched. Safe to call with NULL, which is a no-op.
 * @param[in] view View handle (NULL is ignored).
 * @pre @p view was returned by ::ra8_viewer_view_open, or is NULL.
 * @pre The view is not pumped again after this call.
 * @post The window is closed and @p view is freed.
 * @post The borrowed reader is left untouched.
 * @note Not thread-safe; call on the main thread only.
 * @since 0.1.0
 */
void ra8_viewer_view_close(ra8_viewer_view_t* view);

#ifdef __cplusplus
}
#endif
