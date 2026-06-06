/**
 * @file ra_display_pal_host_macos_window_sim.c
 * @brief Headless window shim for the host macOS display backend
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details
 * No-op implementation of ``ra_display_pal_host_macos_window.h``. It is
 * compiled into the host unit-test build (and any non-macOS build) so
 * the portable backend stays linkable and testable without AppKit. It
 * validates arguments exactly like the real shim and reports a window
 * that never closes, so backend logic (init / caps / flush bounds /
 * clear / deinit) can be exercised end-to-end on a CI runner.
 *
 * The real Cocoa implementation lives in
 * ``ra_display_pal_host_macos_window.m`` and is linked only by the
 * desktop demo, which excludes this file.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>

#include "ra_check.h"
#include "ra_display_pal_host_macos_window.h"
#include "ra_err.h"

/** @brief Module log tag. */
static const char* const s_tag = "ra_display_pal_host_macos_window_sim";

/**
 * @struct ra_macos_window
 * @brief Headless window record -- carries only the last geometry seen.
 *
 * @since 0.1.0
 */
struct ra_macos_window {
  uint16_t width_px;  /**< Width passed to open.  */
  uint16_t height_px; /**< Height passed to open. */
};

/** @brief Single headless window instance (one per process). */
static struct ra_macos_window s_sim_window;

ra_err_t ra_macos_window_open(uint16_t            width_px,
                              uint16_t            height_px,
                              const char*         title,
                              ra_macos_window_t** out_win)
{
  RA_CHECK_NULL_PTR(title, s_tag, "title");
  RA_CHECK_NULL_PTR(out_win, s_tag, "out_win");
  if (width_px == 0U || height_px == 0U) {
    return k_ra_err_invalid_arg;
  }
  s_sim_window.width_px  = width_px;
  s_sim_window.height_px = height_px;
  *out_win               = &s_sim_window;
  return k_ra_ok;
}

ra_err_t ra_macos_window_present(ra_macos_window_t* win,
                                 const void*        rgb565,
                                 uint16_t           width_px,
                                 uint16_t           height_px,
                                 uint32_t           stride_bytes)
{
  RA_CHECK_NULL_PTR(win, s_tag, "win");
  RA_CHECK_NULL_PTR(rgb565, s_tag, "rgb565");
  if (width_px == 0U || height_px == 0U || stride_bytes == 0U) {
    return k_ra_err_invalid_arg;
  }
  /* Headless: nothing is drawn. The arguments are validated so callers
   * see the same contract the AppKit shim enforces. */
  return k_ra_ok;
}

ra_err_t ra_macos_window_pump(ra_macos_window_t* win, bool* out_should_close)
{
  RA_CHECK_NULL_PTR(win, s_tag, "win");
  RA_CHECK_NULL_PTR(out_should_close, s_tag, "out_should_close");
  /* A headless window never asks to close. */
  *out_should_close = false;
  return k_ra_ok;
}

ra_err_t ra_macos_window_poll_click(ra_macos_window_t* win,
                                    bool*              out_has_click,
                                    uint16_t*          out_x,
                                    uint16_t*          out_y)
{
  RA_CHECK_NULL_PTR(win, s_tag, "win");
  RA_CHECK_NULL_PTR(out_has_click, s_tag, "out_has_click");
  RA_CHECK_NULL_PTR(out_x, s_tag, "out_x");
  RA_CHECK_NULL_PTR(out_y, s_tag, "out_y");
  /* Headless: no input device, so never a click. */
  *out_has_click = false;
  *out_x         = 0U;
  *out_y         = 0U;
  return k_ra_ok;
}

ra_err_t ra_macos_window_close(ra_macos_window_t* win)
{
  RA_CHECK_NULL_PTR(win, s_tag, "win");
  s_sim_window.width_px  = 0U;
  s_sim_window.height_px = 0U;
  return k_ra_ok;
}
