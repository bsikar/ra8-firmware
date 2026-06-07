/**
 * @file board_view.h
 * @brief Minimal self-contained macOS window for the board emulator
 *
 * @details
 * A tiny C-ABI over Cocoa so board_sim can SHOW what the emulated GLCDC is
 * driving -- present an RGB565 framebuffer in a desktop window and report when
 * the user closes it. Deliberately standalone (no firmware display-PAL
 * dependency) to keep board_sim a self-contained tool. The real Objective-C
 * lives in board_view.m; this header is plain C so main.c needs no AppKit.
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

/** @brief Opaque desktop-window handle (defined in board_view.m). */
typedef struct board_view board_view_t;

/**
 * @brief Open a desktop window of the given pixel size.
 *
 * @param[in] width_px  Content width in pixels (>= 1).
 * @param[in] height_px Content height in pixels (>= 1).
 * @param[in] title     NUL-terminated window title (ASCII).
 * @return Window handle, or nullptr on failure.
 */
board_view_t* board_view_open(uint16_t width_px, uint16_t height_px, const char* title);

/**
 * @brief Upload an RGB565 framebuffer to the window and request a redraw.
 *
 * @param[in] view   Handle from board_view_open (nullptr is a no-op).
 * @param[in] rgb565 Base of the RGB565 pixel buffer (width_px * height_px).
 * @param[in] width_px  Framebuffer width in pixels.
 * @param[in] height_px Framebuffer height in pixels.
 */
void board_view_present(board_view_t* view,
                        const uint16_t* rgb565,
                        uint16_t        width_px,
                        uint16_t        height_px);

/**
 * @brief Drain pending UI events; report whether the window was closed.
 *
 * @param[in] view Handle from board_view_open (nullptr returns true).
 * @return true once the user has closed the window (stop the run loop).
 */
bool board_view_pump(board_view_t* view);

/**
 * @brief Close the window and release its resources.
 *
 * @param[in] view Handle from board_view_open (nullptr is a no-op).
 */
void board_view_close(board_view_t* view);

#ifdef __cplusplus
}
#endif
