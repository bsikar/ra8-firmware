/**
 * @file ra8_viewer_view.h
 * @brief Plain-C ABI for the viewer's desktop window (Cocoa backend on macOS).
 *
 * @details
 * Mirrors the board_sim board_view pattern: a layer-backed NSView whose CALayer
 * contents is a CGImage built from an RGB565 framebuffer, driven cooperatively so
 * the caller keeps ownership of the run loop. The window records navigation input
 * (Left/Right arrows for page turns, Up/Down + scroll wheel for vertical scroll)
 * and the caller drains it each frame. The implementation is Objective-C
 * (ra8_viewer_view.m); this header keeps the interface pure C so the C reader and
 * `main` need no Objective-C.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * @since 0.1.0
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @struct ra8_viewer_view
 * @brief Opaque handle to the desktop window and its content view.
 * @since 0.1.0
 */
typedef struct ra8_viewer_view ra8_viewer_view_t;

/**
 * @brief Open a titled window whose content view shows an RGB565 framebuffer.
 * @param[in] width_px  Window/content width in pixels (> 0).
 * @param[in] height_px Window/content height in pixels (> 0).
 * @param[in] title     Window title (NUL-terminated, may be NULL).
 * @return New view handle, or NULL if a window could not be created (e.g. no
 *         display / headless host).
 * @since 0.1.0
 */
ra8_viewer_view_t* ra8_viewer_view_open(uint16_t width_px, uint16_t height_px, const char* title);

/**
 * @brief Present one RGB565 frame into the window's layer.
 * @param[in] view      View handle (NULL is ignored).
 * @param[in] rgb565    Row-major RGB565 pixels, little-endian.
 * @param[in] width_px  Frame width in pixels.
 * @param[in] height_px Frame height in pixels.
 * @since 0.1.0
 */
void ra8_viewer_view_present(ra8_viewer_view_t* view,
                             const uint16_t*    rgb565,
                             uint16_t           width_px,
                             uint16_t           height_px);

/**
 * @brief Drain pending window events cooperatively.
 * @param[in] view View handle (NULL treated as closed).
 * @return true when the window has been closed (the caller should exit).
 * @since 0.1.0
 */
bool ra8_viewer_view_pump(ra8_viewer_view_t* view);

/**
 * @brief Consume the accumulated page-navigation delta since the last poll.
 * @param[in] view View handle (NULL yields 0).
 * @return Net page delta: negative for previous pages (Left), positive for next
 *         (Right); 0 when nothing was pressed.
 * @since 0.1.0
 */
int32_t ra8_viewer_view_poll_nav(ra8_viewer_view_t* view);

/**
 * @brief Consume the accumulated vertical-scroll delta since the last poll.
 * @param[in] view View handle (NULL yields 0).
 * @return Net scroll notches: positive downward (Down / wheel down), negative up.
 * @since 0.1.0
 */
int32_t ra8_viewer_view_poll_scroll(ra8_viewer_view_t* view);

/**
 * @brief Save the frame currently shown in the window to a PNG file.
 * @param[in] view View handle (non-NULL).
 * @param[in] path Output PNG path (non-NULL).
 * @return true on success; false if there is no frame yet or the write failed.
 * @since 0.1.0
 */
bool ra8_viewer_view_save_png(ra8_viewer_view_t* view, const char* path);

/**
 * @brief Close the window and release the view handle.
 * @param[in] view View handle (NULL is ignored).
 * @since 0.1.0
 */
void ra8_viewer_view_close(ra8_viewer_view_t* view);

#ifdef __cplusplus
}
#endif
