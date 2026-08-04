/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file board_view_stub.c
 * @brief Headless no-op board view for non-Apple builds (Linux CI)
 *
 * @details
 * ra8_emulator's live window (board_view.m) is Cocoa, so it only builds on macOS.
 * Off the APPLE path -- a Linux CI runner with no window server -- this file is
 * compiled in its place to satisfy the exact same C-ABI declared in
 * board_view.h, with zero AppKit/CoreGraphics references. Every entry point is a
 * headless no-op: board_view_open reports failure (returns nullptr) so main.c
 * takes its existing "could not open window; continuing headless" branch and the
 * run proceeds without a window. The --ppm snapshot path and the smoke harness,
 * which never open a window, are unaffected. The CMakeLists selects exactly one
 * of board_view.m (APPLE) or this file (non-APPLE), so the two never coexist in
 * a link.
 *
 *
 * @since 0.1.0
 */

#include <stdint.h>

#include "board_view.h"

/**
 * @brief Report that no desktop window is available on this build.
 *
 * @param[in] width_px  Content width in pixels (unused, headless).
 * @param[in] height_px Content height in pixels (unused, headless).
 * @param[in] title     Window title (unused, headless).
 * @return Always nullptr -- there is no window server off the APPLE path.
 */
board_view_t* board_view_open(uint16_t width_px, uint16_t height_px, const char* title)
{
  (void)width_px;
  (void)height_px;
  (void)title;
  return nullptr;
}

/**
 * @brief No-op frame upload (no window to present to).
 *
 * @param[in] view      Always nullptr from board_view_open (unused).
 * @param[in] rgb565    Framebuffer base (unused, headless).
 * @param[in] width_px  Framebuffer width in pixels (unused, headless).
 * @param[in] height_px Framebuffer height in pixels (unused, headless).
 */
void board_view_present(board_view_t*   view,
                        const uint16_t* rgb565,
                        uint16_t        width_px,
                        uint16_t        height_px)
{
  (void)view;
  (void)rgb565;
  (void)width_px;
  (void)height_px;
}

/**
 * @brief No-op event pump; report the (absent) window as closed.
 *
 * @param[in] view Always nullptr from board_view_open (unused).
 * @return Always true -- there is no live window to keep a loop running.
 */
bool board_view_pump(board_view_t* view)
{
  (void)view;
  return true;
}

/**
 * @brief No-op click poll (no window can report a mouse-down).
 *
 * @param[in]  view Always nullptr from board_view_open (unused).
 * @param[out] x    Click column (unused, never written).
 * @param[out] y    Click row (unused, never written).
 * @return Always false -- a headless build reports no clicks.
 */
/* x/y are OUT parameters of the board_view.h contract: this headless backend
 * never writes them, but the Cocoa backend (board_view.m) does, so the
 * signature is fixed by the header both must satisfy. */
// NOLINTNEXTLINE(readability-non-const-parameter)
bool board_view_poll_click(board_view_t* view, uint16_t* x, uint16_t* y)
{
  (void)view;
  (void)x;
  (void)y;
  return false;
}

/**
 * @brief No-op drag poll (headless: no window to drag).
 *
 * @param[in]  view Always nullptr from board_view_open (unused).
 * @param[out] x    Drag column (unused, never written).
 * @param[out] y    Drag row (unused, never written).
 * @return Always false -- a headless build reports no drags.
 */
/* x/y are OUT parameters of the board_view.h contract: this headless backend
 * never writes them, but the Cocoa backend (board_view.m) does, so the
 * signature is fixed by the header both must satisfy. */
// NOLINTNEXTLINE(readability-non-const-parameter)
bool board_view_poll_drag(board_view_t* view, uint16_t* x, uint16_t* y)
{
  (void)view;
  (void)x;
  (void)y;
  return false;
}

/**
 * @brief No-op release poll (headless: no window, no mouse-up events).
 *
 * @param[in] view Always nullptr from board_view_open (unused).
 * @return Always false -- a headless build reports no releases.
 */
bool board_view_poll_release(board_view_t* view)
{
  (void)view;
  return false;
}

/**
 * @brief No-op scroll poll (headless: no window to scroll).
 *
 * @param[in] view Always nullptr from board_view_open (unused).
 * @return Always 0.
 */
int32_t board_view_poll_scroll(board_view_t* view)
{
  (void)view;
  return 0;
}

/**
 * @brief No-op window close (nothing was opened).
 *
 * @param[in] view Always nullptr from board_view_open (unused).
 */
void board_view_close(board_view_t* view)
{
  (void)view;
}
