/**
 * @file test_app_display_pal_animation.c
 * @brief Integration test mirroring the display_pal_animation example's main()
 *
 * @details
 * The production app at
 * ``examples/ek_ra8d2/hw_validated/manual/display_pal_animation/main.c``
 * drives the display PAL through ``display_init`` -> ``get_caps``
 * -> ``get_framebuffer`` -> ``clear`` -> paint -> ``flush`` (with a
 * caps-driven refresh hint).  This host integration test re-plays
 * that exact call sequence against the simulator-mocked HAL so a
 * regression in the bring-up order surfaces here long before it
 * reaches the bench.
 *
 * Exercised modules:
 *   - ra8_display_pal (LCD backend)
 *   - ra8_glcdc, ra8_board_ek_ra8d2 (reached through the LCD backend)
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>
#include <string.h>

#include "ra8_display_pal.h"
#include "ra8_display_pal_lcd.h"
#include "ra8_err.h"
#include "ra8_mstp.h"
#include "ra8_panel_timing.h"
#include "ra8_pin_validator.h"
#include "ra8_sim_mmap.h"
#include "unity_minimal.h"

/**
 * @enum display_pal_animation_uint8_const_t
 * @brief Named uint8_t constants used by this file.
 *
 * @details
 * Every literal this translation unit needs, named so the
 * value's role is visible at the point of use (CLAUDE.md
 * "No Magic Numbers").
 */
typedef enum : uint8_t {
  k_fb_alignment_bytes =
    64, /**< Framebuffer alignment the display controller requires, in bytes. */
} display_pal_animation_uint8_const_t;

typedef enum : uint16_t {
  k_test_app_fb_w = 64U, /**< Same shape as the production app, smaller. */
  k_test_app_fb_h = 32U, /**< Test app fb h.                             */
} test_app_dim_t;

typedef enum : uint32_t {
  k_test_app_fb_pixels =
    (uint32_t)k_test_app_fb_w * (uint32_t)k_test_app_fb_h, /**< Test app fb pixels. */
  k_test_app_fb_bytes = k_test_app_fb_pixels * 2U,         /**< Test app fb bytes.  */
} test_app_size_t;

[[gnu::aligned(k_fb_alignment_bytes)]] static uint16_t s_test_fb[k_test_app_fb_pixels];

static void reset_world(void)
{
  ra8_sim_mmap_reset();
  ra8_pin_validator_reset();
  (void)ra8_mstp_init();
  (void)memset(s_test_fb, 0, sizeof(s_test_fb));
}

static display_cfg_t make_cfg(void)
{
  const display_cfg_t cfg = {
    .iface             = &k_display_backend_lcd_ra8_glcdc,
    .framebuffer       = s_test_fb,
    .framebuffer_bytes = sizeof(s_test_fb),
    .width_px          = (uint16_t)k_test_app_fb_w,
    .height_px         = (uint16_t)k_test_app_fb_h,
    .pixfmt            = k_display_pixfmt_rgb565,
    .panel_timing      = &k_ra8_panel_ek_ra8d2_timing,
  };
  return cfg;
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the production
 * app's bring-up call sequence sequentially)
 */
static void test_app_dpa_full_bringup(void)
{
  TEST_BEGIN("display_pal_animation: full main() bring-up sequence");
  reset_world();

  display_handle_t*   d   = nullptr;
  const display_cfg_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, display_init(&cfg, &d));

  display_caps_t caps = {};
  TEST_ASSERT_EQ(k_ra8_ok, display_get_caps(d, &caps));
  TEST_ASSERT(caps.continuous_refresh);
  TEST_ASSERT_EQ(0, caps.refresh_latency_us_typ);

  display_fb_t fb = {};
  TEST_ASSERT_EQ(k_ra8_ok, display_get_framebuffer(d, &fb));
  TEST_ASSERT_EQ((intptr_t)s_test_fb, (intptr_t)fb.pixels);

  /* The app's first call after bring-up: clear to black + init refresh. */
  TEST_ASSERT_EQ(k_ra8_ok, display_clear(d, 0x0000U));
  TEST_ASSERT_EQ(k_ra8_ok, display_flush(d, display_full_rect(d), k_display_refresh_init));
  TEST_ASSERT_EQ(0x0000U, ((const uint16_t*)fb.pixels)[0]);

  TEST_ASSERT_EQ(k_ra8_ok, display_deinit(d));
  TEST_END("display_pal_animation: full main() bring-up sequence");
}

/**
 * @par MC/DC:
 * Decision under test (in the production app):
 * ``s_caps.continuous_refresh ? k_display_refresh_fast :
 *   k_display_refresh_quality``. Two vectors: continuous-refresh
 * panel (LCD) selects fast; bistable would select quality. This
 * test pins the LCD branch.
 */
static void test_app_dpa_frame_loop(void)
{
  TEST_BEGIN("display_pal_animation: per-frame paint + flush");
  reset_world();

  display_handle_t*   d   = nullptr;
  const display_cfg_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, display_init(&cfg, &d));

  display_caps_t caps = {};
  TEST_ASSERT_EQ(k_ra8_ok, display_get_caps(d, &caps));
  const display_refresh_hint_t hint =
    caps.continuous_refresh ? k_display_refresh_fast : k_display_refresh_quality;
  TEST_ASSERT_EQ(k_display_refresh_fast, hint);

  /* Mirror three frame iterations of the app's `while (1)` body. */
  for (uint8_t k = 0U; k < 3U; ++k) {
    TEST_ASSERT_EQ(k_ra8_ok, display_clear(d, (uint32_t)(0x07E0U + k)));
    TEST_ASSERT_EQ(k_ra8_ok, display_flush(d, display_full_rect(d), hint));
  }

  TEST_ASSERT_EQ(k_ra8_ok, display_deinit(d));
  TEST_END("display_pal_animation: per-frame paint + flush");
}

int main(void)
{
  test_app_dpa_full_bringup();
  test_app_dpa_frame_loop();
  return 0;
}
