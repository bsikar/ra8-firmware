/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file board_view.h
 * @brief Minimal self-contained macOS window for the board emulator
 *
 * @details
 * A tiny C-ABI over Cocoa so ra8_emulator can SHOW what the emulated GLCDC is
 * driving -- present an RGB565 framebuffer in a desktop window and report when
 * the user closes it. Deliberately standalone (no firmware display-PAL
 * dependency) to keep ra8_emulator a self-contained tool. The real Objective-C
 * lives in board_view.m; this header is plain C so main.c needs no AppKit.
 *
 *
 * @since 0.1.0
 */

#pragma once

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
void board_view_present(board_view_t*   view,
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
 * @brief Report the most recent unprocessed left mouse-down, once.
 *
 * @details
 * Latches the framebuffer-pixel coordinate of the last left-button press on
 * the content view (recorded by the view's mouseDown handler) and returns it
 * to the caller exactly once -- the latch is cleared on read, so a held button
 * or repeated polls do not re-report the same press. Coordinates use the
 * framebuffer's top-left origin (row 0 at the top), matching how
 * board_view_present uploads pixels; the view's flipped-Y window space is
 * converted here so the caller never sees AppKit's bottom-left origin. Pump
 * the window (board_view_pump) first so AppKit has delivered the click.
 *
 * @param[in]  view Handle from board_view_open (nullptr reports no click).
 * @param[out] x    Click column in framebuffer pixels (top-left origin).
 * @param[out] y    Click row in framebuffer pixels (top-left origin).
 * @return true if a fresh click was reported (and consumed); false otherwise.
 */
bool board_view_poll_click(board_view_t* view, uint16_t* x, uint16_t* y);

/**
 * @brief Report the current position of a held-button drag, once.
 *
 * @details
 * Latches the framebuffer-pixel coordinate published by the view's mouseDragged
 * handler while the primary button is held and moved, returning it to the caller
 * once (the latch clears on read). This is independent of board_view_poll_click:
 * the initial press is still reported once via that latch (so a click is routed
 * once), while continued motion updates only the drag latch -- letting the run
 * loop follow the cursor for a grabbed control (the battery slider) without
 * re-firing the one-shot press. Coordinates use the framebuffer's top-left
 * origin, matching board_view_poll_click. Pump the window first.
 *
 * @param[in]  view Handle from board_view_open (nullptr reports no drag).
 * @param[out] x    Drag column in framebuffer pixels (top-left origin).
 * @param[out] y    Drag row in framebuffer pixels (top-left origin).
 * @return true if a fresh drag position was reported (and consumed); false otherwise.
 * @since 0.1.0
 */
bool board_view_poll_drag(board_view_t* view, uint16_t* x, uint16_t* y);

/**
 * @brief Report whether the primary mouse button was released since the last poll.
 *
 * @details
 * Latches the view's mouseUp event (left button released anywhere over the
 * content view) and returns it to the caller exactly once -- the latch clears on
 * read. The run loop uses this to release a momentary on-screen push-button
 * (SW1/SW2) that a prior board_view_poll_click pressed, so the buttons behave as
 * real push-buttons (held only while the mouse is down) rather than latching
 * switches, and to drop a battery-slider grab when the drag ends. Unlike
 * board_view_poll_click/_drag this carries no coordinate -- a release is a pure
 * edge. Pump the window (board_view_pump) first so AppKit has delivered the up
 * event.
 *
 * @param[in] view Handle from board_view_open (nullptr reports no release).
 * @return true if a fresh mouse-up was reported (and consumed); false otherwise.
 * @since 0.1.0
 */
bool board_view_poll_release(board_view_t* view);

/**
 * @brief Drain the accumulated console scroll-wheel notches since the last poll.
 *
 * @details
 * The content view's scrollWheel handler accumulates one notch per wheel /
 * two-finger scroll step (positive scrolling up into older console history,
 * negative scrolling back down toward the newest line). This returns the net
 * accumulated notches and clears the accumulator, so the run loop can advance a
 * console scrollback offset. Independent of board_view_poll_click and of the
 * keyDown path (which feeds the firmware UART RX), so scrolling the console
 * never collides with clicking buttons or typing into the running app.
 *
 * @param[in] view Handle from board_view_open (nullptr returns 0).
 * @return Net scroll notches since the last poll (+up / -down), 0 if none.
 * @since 0.1.0
 */
int32_t board_view_poll_scroll(board_view_t* view);

/**
 * @brief Close the window and release its resources.
 *
 * @param[in] view Handle from board_view_open (nullptr is a no-op).
 */
void board_view_close(board_view_t* view);

#ifdef __cplusplus
}
#endif
