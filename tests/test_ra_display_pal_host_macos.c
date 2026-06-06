/**
 * @file test_ra_display_pal_host_macos.c
 * @brief Host unit tests for the macOS display PAL backend.
 *
 * @details
 * Exercises the portable backend logic of ``k_display_backend_host_macos``
 * through the display PAL. In the host test build the backend links the
 * headless window shim (``ra_display_pal_host_macos_window_sim.c``), so
 * ``init`` / ``flush`` / ``deinit`` succeed without AppKit and the
 * validation, snapshot, bounds-check, clear, and event-pump paths run
 * on a CI runner exactly as they would on macOS.
 *
 * The real Cocoa rendering path lives in the ``.m`` shim and is covered
 * by manual on-desktop runs of the ``examples/host/floorplan`` demo, not
 * by these host tests.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <string.h>

#include "ra_display_pal.h"
#include "ra_display_pal_host_macos.h"
#include "ra_display_pal_host_macos_pixels.h"
#include "ra_err.h"
#include "unity_minimal.h"

/* =============================================================================
 * Test fixture
 * =============================================================================
 */

typedef enum : uint16_t {
  k_test_fb_width  = 64U,
  k_test_fb_height = 32U,
} test_fb_dim_t;

typedef enum : uint32_t {
  k_test_fb_pixels = (uint32_t)k_test_fb_width * (uint32_t)k_test_fb_height,
  k_test_fb_bytes  = k_test_fb_pixels * 2U,
} test_fb_size_t;

/** @brief Framebuffer the host backend presents (one window per process). */
static uint16_t s_test_fb[k_test_fb_pixels];

static display_cfg_t make_macos_cfg(void)
{
  const display_cfg_t cfg = {
    .iface             = &k_display_backend_host_macos,
    .framebuffer       = s_test_fb,
    .framebuffer_bytes = sizeof(s_test_fb),
    .width_px          = (uint16_t)k_test_fb_width,
    .height_px         = (uint16_t)k_test_fb_height,
    .pixfmt            = k_display_pixfmt_rgb565,
  };
  return cfg;
}

/* =============================================================================
 * Tests
 * =============================================================================
 */

static void test_macos_happy_path(void)
{
  TEST_BEGIN("host macOS backend: init/caps/get_fb/clear/flush/deinit");
  (void)memset(s_test_fb, 0, sizeof(s_test_fb));

  display_handle_t*   d   = nullptr;
  const display_cfg_t cfg = make_macos_cfg();
  TEST_ASSERT_EQ(k_ra_ok, display_init(&cfg, &d));

  display_caps_t caps = {};
  TEST_ASSERT_EQ(k_ra_ok, display_get_caps(d, &caps));
  TEST_ASSERT_EQ((uint16_t)k_test_fb_width, caps.width_px);
  TEST_ASSERT_EQ((uint16_t)k_test_fb_height, caps.height_px);
  TEST_ASSERT_EQ(k_display_pixfmt_rgb565, caps.pixfmt);

  display_fb_t fb = {};
  TEST_ASSERT_EQ(k_ra_ok, display_get_framebuffer(d, &fb));
  TEST_ASSERT(fb.pixels == (void*)s_test_fb);

  TEST_ASSERT_EQ(k_ra_ok, display_clear(d, 0xABCDU));
  TEST_ASSERT_EQ((uint16_t)0xABCDU, s_test_fb[0]);
  TEST_ASSERT_EQ((uint16_t)0xABCDU, s_test_fb[k_test_fb_pixels - 1U]);

  TEST_ASSERT_EQ(k_ra_ok, display_flush(d, display_full_rect(d), k_display_refresh_quality));
  TEST_ASSERT_EQ(k_ra_ok, display_deinit(d));
  TEST_END("host macOS backend: init/caps/get_fb/clear/flush/deinit");
}

static void test_macos_rejects_bad_cfg(void)
{
  TEST_BEGIN("host macOS backend: cfg validation");
  (void)memset(s_test_fb, 0, sizeof(s_test_fb));
  display_handle_t* d = nullptr;

  /* Non-RGB565 pixel format. */
  display_cfg_t bad_fmt = make_macos_cfg();
  bad_fmt.pixfmt        = k_display_pixfmt_grey1;
  TEST_ASSERT_EQ(k_ra_err_not_supported, display_init(&bad_fmt, &d));

  /* Zero dimension. */
  display_cfg_t bad_dim = make_macos_cfg();
  bad_dim.width_px      = 0U;
  TEST_ASSERT_EQ(k_ra_err_invalid_arg, display_init(&bad_dim, &d));

  /* Buffer too small for the geometry. */
  display_cfg_t bad_buf     = make_macos_cfg();
  bad_buf.framebuffer_bytes = 8U;
  TEST_ASSERT_EQ(k_ra_err_invalid_arg, display_init(&bad_buf, &d));
  TEST_END("host macOS backend: cfg validation");
}

static void test_macos_flush_rejects_out_of_bounds(void)
{
  TEST_BEGIN("host macOS backend: flush rejects out-of-bounds rect");
  (void)memset(s_test_fb, 0, sizeof(s_test_fb));

  display_handle_t*   d   = nullptr;
  const display_cfg_t cfg = make_macos_cfg();
  TEST_ASSERT_EQ(k_ra_ok, display_init(&cfg, &d));

  const display_rect_t oob = {0U, 0U, (uint16_t)(k_test_fb_width + 1U), (uint16_t)k_test_fb_height};
  TEST_ASSERT_EQ(k_ra_err_invalid_arg, display_flush(d, oob, k_display_refresh_fast));

  TEST_ASSERT_EQ(k_ra_ok, display_deinit(d));
  TEST_END("host macOS backend: flush rejects out-of-bounds rect");
}

static void test_macos_rejects_double_init(void)
{
  TEST_BEGIN("host macOS backend: second init is busy");
  (void)memset(s_test_fb, 0, sizeof(s_test_fb));

  display_handle_t*   d   = nullptr;
  const display_cfg_t cfg = make_macos_cfg();
  TEST_ASSERT_EQ(k_ra_ok, display_init(&cfg, &d));

  display_handle_t* d2 = nullptr;
  TEST_ASSERT_EQ(k_ra_err_busy, display_init(&cfg, &d2));

  TEST_ASSERT_EQ(k_ra_ok, display_deinit(d));
  TEST_END("host macOS backend: second init is busy");
}

static void test_macos_pump_contract(void)
{
  TEST_BEGIN("host macOS backend: pump null + not-initialized contract");
  (void)memset(s_test_fb, 0, sizeof(s_test_fb));

  /* No window open yet (previous tests all deinit) -> not initialized. */
  bool should_close = true;
  TEST_ASSERT_EQ(k_ra_err_not_initialized, ra_display_pal_host_macos_pump(&should_close));
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_display_pal_host_macos_pump(nullptr));

  display_handle_t*   d   = nullptr;
  const display_cfg_t cfg = make_macos_cfg();
  TEST_ASSERT_EQ(k_ra_ok, display_init(&cfg, &d));

  should_close = true;
  TEST_ASSERT_EQ(k_ra_ok, ra_display_pal_host_macos_pump(&should_close));
  TEST_ASSERT(!should_close); /* headless window never closes. */

  TEST_ASSERT_EQ(k_ra_ok, display_deinit(d));
  TEST_END("host macOS backend: pump null + not-initialized contract");
}

static void test_macos_pixels_conversion(void)
{
  TEST_BEGIN("host macOS backend: RGB565 -> ARGB8888 conversion");

  /* Pure red (0xF800) and pure blue (0x001F), 2x1 row. */
  const uint16_t rb[2]  = {0xF800U, 0x001FU};
  uint32_t       out[2] = {0U, 0U};
  TEST_ASSERT_EQ(k_ra_ok, ra_macos_rgb565_to_argb8888(out, rb, 2U, 1U, (uint32_t)sizeof(rb)));
  TEST_ASSERT_EQ((uint32_t)0xFFFF0000U, out[0]);
  TEST_ASSERT_EQ((uint32_t)0xFF0000FFU, out[1]);

  /* Black (0x0000) and white (0xFFFF) -- full-scale must map to 0xFF. */
  const uint16_t bw[2]   = {0x0000U, 0xFFFFU};
  uint32_t       out2[2] = {0U, 0U};
  TEST_ASSERT_EQ(k_ra_ok, ra_macos_rgb565_to_argb8888(out2, bw, 2U, 1U, (uint32_t)sizeof(bw)));
  TEST_ASSERT_EQ((uint32_t)0xFF000000U, out2[0]);
  TEST_ASSERT_EQ((uint32_t)0xFFFFFFFFU, out2[1]);

  /* Argument validation. */
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_macos_rgb565_to_argb8888(nullptr, rb, 2U, 1U, 4U));
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_macos_rgb565_to_argb8888(out, nullptr, 2U, 1U, 4U));
  TEST_ASSERT_EQ(k_ra_err_invalid_arg, ra_macos_rgb565_to_argb8888(out, rb, 0U, 1U, 4U));
  TEST_END("host macOS backend: RGB565 -> ARGB8888 conversion");
}

/* =============================================================================
 * Driver
 * =============================================================================
 */

int main(void)
{
  test_macos_happy_path();
  test_macos_rejects_bad_cfg();
  test_macos_flush_rejects_out_of_bounds();
  test_macos_rejects_double_init();
  test_macos_pump_contract();
  test_macos_pixels_conversion();
  return 0;
}
