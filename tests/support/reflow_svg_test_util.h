/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file reflow_svg_test_util.h
 * @brief Shared framebuffer fixture for the test_ra8_reflow_svg_*_mcdc.c siblings.
 *
 * @details
 * Header-only test fixture providing the host ARGB8888 framebuffer, the
 * pixel probe, the bind-and-clear helper, and the render-into-full-box
 * wrapper shared by test_ra8_reflow_svg_scan_mcdc.c,
 * test_ra8_reflow_svg_shape_mcdc.c, and test_ra8_reflow_svg_paint_mcdc.c.
 * Everything here has internal linkage, so each including test executable
 * owns a private framebuffer.
 */
#pragma once

#include <stdint.h>
#include <string.h>

#include "ra8_err.h"
#include "ra8_gfx.h"
#include "ra8_reflow_svg.h"
#include "unity_minimal.h"

/** @brief Framebuffer + colour dimensions for the render tests. */
enum : int32_t {
  k_w        = 200,        /**< Framebuffer / render-box width.  */
  k_h        = 200,        /**< Framebuffer / render-box height. */
  k_white    = 0x00FFFFFF, /**< Cleared-background RGB.          */
  k_red      = 0x00FF0000, /**< A primary used for fills.        */
  k_green    = 0x0000FF00, /**< A primary used for fills.        */
  k_blue     = 0x000000FF, /**< A primary used for fills.        */
  k_black    = 0x00000000, /**< SVG default fill.                */
  k_rgb_mask = 0x00FFFFFF, /**< Low-24-bit RGB extraction mask.  */
};

/** @brief Host framebuffer (ARGB8888) bound by the render tests. */
static uint32_t s_fb[k_w * k_h];

/** @brief RGB (low 24 bits) of the framebuffer pixel at (x, y). */
static inline uint32_t px(int32_t x, int32_t y)
{
  return s_fb[(y * k_w) + x] & (uint32_t)k_rgb_mask;
}

/** @brief Bind + clear the framebuffer to white. */
static inline void fb_reset(void)
{
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_gfx_init(s_fb, (uint16_t)k_w, (uint16_t)k_h, k_ra8_gfx_format_argb8888));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_gfx_clear((uint32_t)k_white));
}

/** @brief Render a NUL-terminated SVG string into the full framebuffer box. */
static inline ra8_err_t render(const char* svg)
{
  return ra8_svg_render((const uint8_t*)svg, strlen(svg), 0, 0, k_w, k_h);
}
