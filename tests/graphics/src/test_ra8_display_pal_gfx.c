/**
 * @file test_ra8_display_pal_gfx.c
 * @brief Unit and integration tests for the display_bind_gfx() composition bridge.
 *
 * @details
 * Exercises ::display_bind_gfx() pixel-format translation (RGB565, RGB888,
 * unsupported greyscale formats, unrecognized enum values), null-pointer MC/DC,
 * surface dimension/stride validation forwarding, and the full LCD PAL bind ->
 * rasterise -> flush -> ::ra8_gfx_deinit() -> display_deinit() lifecycle.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <string.h>

#include "ra8_display_pal.h"
#include "ra8_display_pal_gfx.h"
#include "ra8_display_pal_lcd.h"
#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_fake_mmio.h"
#include "ra8_gfx.h"
#include "ra8_mstp.h"
#include "ra8_panel_timing.h"
#include "ra8_pin_validator.h"
#include "unity_minimal.h"

/**
 * @enum pal_gfx_test_const_t
 * @brief Buffer dimensions, alignment, strides, and pixel canaries for tests.
 */
typedef enum : uint32_t {
  k_pg_fb_align   = 64U,         /**< Cache-line framebuffer alignment (B). */
  k_pg_lcd_w      = 64U,         /**< LCD PAL test framebuffer width (px).  */
  k_pg_lcd_h      = 32U,         /**< LCD PAL test framebuffer height (px). */
  k_pg_lcd_pixels = 64U * 32U,   /**< LCD PAL test pixel count.             */
  k_pg_surf_w     = 4U,          /**< Standalone surface width (px).        */
  k_pg_surf_h     = 3U,          /**< Standalone surface height (px).       */
  k_pg_rgb565_bpp = 2U,          /**< RGB565 bytes per pixel.               */
  k_pg_rgb888_bpp = 3U,          /**< RGB888 bytes per pixel.               */
  k_pg_row_pad    = 4U,          /**< Trailing padding bytes per row.       */
  k_pg_canary     = 0x5AU,       /**< Guard byte outside active pixels.     */
  k_pg_white_xrgb = 0x00FFFFFFU, /**< White 32-bit colour value.            */
  k_pg_rgb565_max = 0xFFFFU,     /**< White 16-bit RGB565 pixel value.      */
  k_pg_bad_pixfmt = 99U,         /**< Out-of-range pixfmt sentinel.         */
  k_pg_bad_stride = 2U,          /**< Undersized stride for 4 px RGB565.    */
  k_pg_rect_w     = 4U,          /**< Filled rectangle width (px).          */
  k_pg_rect_h     = 2U,          /**< Filled rectangle height (px).         */
} pal_gfx_test_const_t;

/** @brief Padded row stride in bytes for standalone RGB565 tests. */
enum : uint32_t {
  k_pg_rgb565_sby = (k_pg_surf_w * k_pg_rgb565_bpp) + k_pg_row_pad,
  k_pg_rgb888_sby = (k_pg_surf_w * k_pg_rgb888_bpp) + k_pg_row_pad,
};

/** @brief 64-byte aligned backing buffer for the GLCDC PAL lifecycle test. */
alignas(k_pg_fb_align) static uint16_t s_lcd_fb[k_pg_lcd_pixels];

/** @brief Byte scratch framebuffer large enough for padded RGB565/RGB888 runs. */
static uint8_t s_raw_fb[k_pg_rgb888_sby * k_pg_surf_h];

/**
 * @brief Reset host fake MMIO/pin/MSTP state and ensure ra8_gfx starts unbound.
 */
static void reset_host_world(void)
{
  (void)ra8_gfx_deinit();
  ra8_fake_mmap_reset();
  ra8_fake_mmio_reset();
  ra8_pin_validator_reset();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mstp_init());
}

/**
 * @par MC/DC:
 * Decision in ::display_bind_gfx():
 * ``if ((fb == nullptr) || (fb->pixels == nullptr))`` -- 2 conditions.
 *
 * - V1: fb != nullptr, fb->pixels != nullptr -> false (covered by RGB565 test).
 * - V2: fb == nullptr                         -> true (short-circuits).
 * - V3: fb != nullptr, fb->pixels == nullptr -> true (exercises second leg).
 *
 * V1+V2 vary C1; V1+V3 vary C2. N+1 = 3 vectors for 2 conditions.
 */
static void test_bind_gfx_mcdc_null_guards(void)
{
  TEST_BEGIN("display_bind_gfx MC/DC: rejects null descriptor and null pixels");
  reset_host_world();

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, display_bind_gfx(nullptr));

  const display_fb_t null_pixels = {
    .pixels       = nullptr,
    .width_px     = (uint16_t)k_pg_surf_w,
    .height_px    = (uint16_t)k_pg_surf_h,
    .stride_bytes = k_pg_rgb565_sby,
    .pixfmt       = k_display_pixfmt_rgb565,
  };
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, display_bind_gfx(&null_pixels));
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_gfx_pixel(0, 0, k_pg_white_xrgb));

  TEST_END("display_bind_gfx MC/DC: rejects null descriptor and null pixels");
}

/**
 * @brief Verify RGB565 translation honours padded stride and deinit unbinds.
 */
static void test_bind_gfx_rgb565_padded_and_deinit(void)
{
  TEST_BEGIN("display_bind_gfx translates RGB565 stride and ra8_gfx_deinit unbinds");
  reset_host_world();
  (void)memset(s_raw_fb, (int)k_pg_canary, sizeof(s_raw_fb));

  const display_fb_t fb = {
    .pixels       = s_raw_fb,
    .width_px     = (uint16_t)k_pg_surf_w,
    .height_px    = (uint16_t)k_pg_surf_h,
    .stride_bytes = k_pg_rgb565_sby,
    .pixfmt       = k_display_pixfmt_rgb565,
  };
  TEST_ASSERT_EQ(k_ra8_ok, display_bind_gfx(&fb));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_gfx_clear(0U));

  /* Row-end padding bytes remain untouched at canary value. */
  const uint32_t active_row_bytes = k_pg_surf_w * k_pg_rgb565_bpp;
  for (uint32_t row = 0U; row < k_pg_surf_h; ++row) {
    const uint32_t row_pad_off = (row * k_pg_rgb565_sby) + active_row_bytes;
    for (uint32_t pad = 0U; pad < k_pg_row_pad; ++pad) {
      TEST_ASSERT_EQ((uint8_t)k_pg_canary, s_raw_fb[row_pad_off + pad]);
    }
  }

  /* Pixel (2, 1) writes 2 white bytes at row 1, col 2. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_gfx_pixel(2, 1, k_pg_white_xrgb));
  const uint32_t px_off = (1U * k_pg_rgb565_sby) + (2U * k_pg_rgb565_bpp);
  TEST_ASSERT_EQ(0xFFU, s_raw_fb[px_off]);
  TEST_ASSERT_EQ(0xFFU, s_raw_fb[px_off + 1U]);
  TEST_ASSERT_EQ(0x00U, s_raw_fb[px_off - 1U]);
  TEST_ASSERT_EQ(0x00U, s_raw_fb[px_off + 2U]);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_gfx_deinit());
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_gfx_pixel(0, 0, k_pg_white_xrgb));

  TEST_END("display_bind_gfx translates RGB565 stride and ra8_gfx_deinit unbinds");
}

/**
 * @brief Verify RGB888 translation maps to 3 bytes/pixel and respects padding.
 */
static void test_bind_gfx_rgb888_pixel_stride(void)
{
  TEST_BEGIN("display_bind_gfx translates RGB888 format and 3-bpp layout");
  reset_host_world();
  (void)memset(s_raw_fb, (int)k_pg_canary, sizeof(s_raw_fb));

  const display_fb_t fb = {
    .pixels       = s_raw_fb,
    .width_px     = (uint16_t)k_pg_surf_w,
    .height_px    = (uint16_t)k_pg_surf_h,
    .stride_bytes = k_pg_rgb888_sby,
    .pixfmt       = k_display_pixfmt_rgb888,
  };
  TEST_ASSERT_EQ(k_ra8_ok, display_bind_gfx(&fb));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_gfx_clear(0U));

  /* Trailing padding past 4 * 3 bytes stays untouched. */
  const uint32_t active_row_bytes = k_pg_surf_w * k_pg_rgb888_bpp;
  for (uint32_t pad = 0U; pad < k_pg_row_pad; ++pad) {
    TEST_ASSERT_EQ((uint8_t)k_pg_canary, s_raw_fb[active_row_bytes + pad]);
  }

  /* Pixel (1, 0) fills bytes [3..5] with 0xFF and leaves neighbours zero. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_gfx_pixel(1, 0, k_pg_white_xrgb));
  TEST_ASSERT_EQ(0x00U, s_raw_fb[2]);
  TEST_ASSERT_EQ(0xFFU, s_raw_fb[3]);
  TEST_ASSERT_EQ(0xFFU, s_raw_fb[4]);
  TEST_ASSERT_EQ(0xFFU, s_raw_fb[5]);
  TEST_ASSERT_EQ(0x00U, s_raw_fb[6]);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_gfx_deinit());

  TEST_END("display_bind_gfx translates RGB888 format and 3-bpp layout");
}

/**
 * @brief Verify greyscale formats, unrecognized enum values, and bad geometry fail.
 */
static void test_bind_gfx_rejects_unsupported_and_invalid_inputs(void)
{
  TEST_BEGIN("display_bind_gfx rejects grey4, grey1, invalid pixfmt, and bad geometry");
  reset_host_world();

  display_fb_t fb = {
    .pixels       = s_raw_fb,
    .width_px     = (uint16_t)k_pg_surf_w,
    .height_px    = (uint16_t)k_pg_surf_h,
    .stride_bytes = k_pg_rgb565_sby,
    .pixfmt       = k_display_pixfmt_grey4,
  };
  TEST_ASSERT_EQ(k_ra8_err_not_supported, display_bind_gfx(&fb));

  fb.pixfmt = k_display_pixfmt_grey1;
  TEST_ASSERT_EQ(k_ra8_err_not_supported, display_bind_gfx(&fb));

  fb.pixfmt = (display_pixfmt_t)k_pg_bad_pixfmt;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, display_bind_gfx(&fb));

  fb.pixfmt   = k_display_pixfmt_rgb565;
  fb.width_px = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, display_bind_gfx(&fb));

  fb.width_px     = (uint16_t)k_pg_surf_w;
  fb.stride_bytes = k_pg_bad_stride;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, display_bind_gfx(&fb));

  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_gfx_pixel(0, 0, k_pg_white_xrgb));

  TEST_END("display_bind_gfx rejects grey4, grey1, invalid pixfmt, and bad geometry");
}

/**
 * @brief Verify PAL GLCDC bind -> display_get_framebuffer -> display_bind_gfx -> draw -> deinit.
 */
static void test_bind_gfx_with_lcd_pal_lifecycle(void)
{
  TEST_BEGIN("LCD PAL bind -> get_framebuffer -> display_bind_gfx -> draw -> deinit");
  reset_host_world();
  (void)memset(s_lcd_fb, (int)k_pg_canary, sizeof(s_lcd_fb));

  display_handle_t*      disp   = nullptr;
  const display_fb_cfg_t fb_cfg = {
    .pixels    = s_lcd_fb,
    .bytes     = sizeof(s_lcd_fb),
    .width_px  = (uint16_t)k_pg_lcd_w,
    .height_px = (uint16_t)k_pg_lcd_h,
    .pixfmt    = k_display_pixfmt_rgb565,
  };
  TEST_ASSERT_EQ(k_ra8_ok,
                 display_pal_bind_glcdc(&disp, &fb_cfg, &s_ra8_panel_ek_ra8d2_timing));

  display_fb_t fb = {};
  TEST_ASSERT_EQ(k_ra8_ok, display_get_framebuffer(disp, &fb));
  TEST_ASSERT_EQ(k_ra8_ok, display_bind_gfx(&fb));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_gfx_clear(0U));
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_gfx_rect(0,
                              0,
                              (int32_t)k_pg_rect_w,
                              (int32_t)k_pg_rect_h,
                              k_pg_white_xrgb,
                              true));
  TEST_ASSERT_EQ((uint16_t)k_pg_rgb565_max, s_lcd_fb[0]);
  TEST_ASSERT_EQ((uint16_t)k_pg_rgb565_max, s_lcd_fb[k_pg_lcd_w + (k_pg_rect_w - 1U)]);
  TEST_ASSERT_EQ(0U, s_lcd_fb[(k_pg_rect_h * k_pg_lcd_w) + k_pg_rect_w]);

  TEST_ASSERT_EQ(k_ra8_ok, display_flush(disp, display_full_rect(disp), k_display_refresh_quality));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_gfx_deinit());
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_gfx_clear(0U));
  TEST_ASSERT_EQ(k_ra8_ok, display_deinit(disp));

  TEST_END("LCD PAL bind -> get_framebuffer -> display_bind_gfx -> draw -> deinit");
}

/** @brief Roster of every test case in this file, run in order by main(). */
static void (*const s_test_roster[])(void) = {
  test_bind_gfx_mcdc_null_guards,
  test_bind_gfx_rgb565_padded_and_deinit,
  test_bind_gfx_rgb888_pixel_stride,
  test_bind_gfx_rejects_unsupported_and_invalid_inputs,
  test_bind_gfx_with_lcd_pal_lifecycle,
};

int main(void)
{
  for (size_t i = 0U; i < (sizeof s_test_roster / sizeof s_test_roster[0]); ++i) {
    s_test_roster[i]();
  }
  return 0;
}
