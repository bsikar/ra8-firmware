/**
 * @file test_ra_gfx.c
 * @brief Unit tests for libs/ra_gfx (software 2D graphics + bitmap font).
 *
 * @details
 * Each test allocates a small RGB565 / RGB888 / ARGB8888 framebuffer on
 * the stack (or .bss for bigger ones), binds ra_gfx to it via
 * ra_gfx_init(), and inspects raw bytes after a draw call.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <string.h>

#include "ra_err.h"
#include "ra_gfx.h"
#include "ra_gfx_font.h"
#include "unity_minimal.h"

/**
 * @enum test_gfx_dim_t
 * @brief Framebuffer dimensions used across the tests.
 */
typedef enum : uint16_t {
  k_test_fb_w = 64,
  k_test_fb_h = 32,
} test_gfx_dim_t;

/** @brief Bytes for a 64x32 RGB565 buffer (4096 B). */
static uint8_t s_fb565[k_test_fb_w * k_test_fb_h * 2];
/** @brief Bytes for a 64x32 ARGB8888 buffer (8192 B). */
static uint8_t s_fb8888[k_test_fb_w * k_test_fb_h * 4];

/**
 * @brief Fully reset s_state by re-binding to a fresh framebuffer.
 */
static void rebind_565(void)
{
  (void)memset(s_fb565, 0, sizeof(s_fb565));
  const ra_err_t err = ra_gfx_init(s_fb565, k_test_fb_w, k_test_fb_h, k_ra_gfx_format_rgb565);
  TEST_ASSERT_EQ(k_ra_ok, err);
}

static void rebind_8888(void)
{
  (void)memset(s_fb8888, 0, sizeof(s_fb8888));
  const ra_err_t err = ra_gfx_init(s_fb8888, k_test_fb_w, k_test_fb_h, k_ra_gfx_format_argb8888);
  TEST_ASSERT_EQ(k_ra_ok, err);
}

static void test_init_arg_validation(void)
{
  TEST_BEGIN("ra_gfx_init validates arguments");
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_gfx_init(NULL, 64, 32, k_ra_gfx_format_rgb565));
  TEST_ASSERT_EQ(k_ra_err_invalid_arg, ra_gfx_init(s_fb565, 0, 32, k_ra_gfx_format_rgb565));
  TEST_ASSERT_EQ(k_ra_err_invalid_arg, ra_gfx_init(s_fb565, 64, 0, k_ra_gfx_format_rgb565));
  TEST_ASSERT_EQ(k_ra_err_invalid_arg, ra_gfx_init(s_fb565, 64, 32, (ra_gfx_format_t)0xAA));
  TEST_ASSERT_EQ(k_ra_ok, ra_gfx_init(s_fb565, 64, 32, k_ra_gfx_format_rgb565));
  TEST_END("ra_gfx_init validates arguments");
}

static void test_clear_writes_every_pixel(void)
{
  TEST_BEGIN("ra_gfx_clear sets every pixel");
  rebind_565();
  TEST_ASSERT_EQ(k_ra_ok, ra_gfx_clear(0xFFFFFFu)); /* white */
  /* 0xFFFFFF in RGB565 packs to 0xFFFF. */
  for (uint32_t i = 0; i < sizeof(s_fb565); i++) {
    TEST_ASSERT_EQ(0xFF, s_fb565[i]);
  }
  TEST_END("ra_gfx_clear sets every pixel");
}

static void test_pixel_happy_and_oob(void)
{
  TEST_BEGIN("ra_gfx_pixel happy + OOB");
  rebind_8888();
  TEST_ASSERT_EQ(k_ra_ok, ra_gfx_pixel(5, 7, 0xFF112233u));
  /* ARGB8888 layout in memory: B G R A. */
  const uint32_t off = (7u * k_test_fb_w + 5u) * 4u;
  TEST_ASSERT_EQ(0x33, s_fb8888[off + 0]);
  TEST_ASSERT_EQ(0x22, s_fb8888[off + 1]);
  TEST_ASSERT_EQ(0x11, s_fb8888[off + 2]);
  TEST_ASSERT_EQ(0xFF, s_fb8888[off + 3]);
  TEST_ASSERT_EQ(k_ra_err_range_check_failed, ra_gfx_pixel(-1, 0, 0));
  TEST_ASSERT_EQ(k_ra_err_range_check_failed, ra_gfx_pixel(k_test_fb_w, 0, 0));
  TEST_ASSERT_EQ(k_ra_err_range_check_failed, ra_gfx_pixel(0, k_test_fb_h, 0));
  TEST_END("ra_gfx_pixel happy + OOB");
}

static void test_line_horizontal(void)
{
  TEST_BEGIN("ra_gfx_line draws a horizontal line");
  rebind_8888();
  TEST_ASSERT_EQ(k_ra_ok, ra_gfx_line(2, 4, 10, 4, 0xFF00FF00u));
  for (int32_t x = 2; x <= 10; x++) {
    const uint32_t off = (4u * k_test_fb_w + (uint32_t)x) * 4u;
    TEST_ASSERT_EQ(0x00, s_fb8888[off + 0]); /* B */
    TEST_ASSERT_EQ(0xFF, s_fb8888[off + 1]); /* G */
    TEST_ASSERT_EQ(0x00, s_fb8888[off + 2]); /* R */
  }
  TEST_END("ra_gfx_line draws a horizontal line");
}

static void test_rect_outline_and_filled(void)
{
  TEST_BEGIN("ra_gfx_rect outline + filled");
  rebind_8888();
  /* Outline: 4x4 starting at (1,1). */
  TEST_ASSERT_EQ(k_ra_ok, ra_gfx_rect(1, 1, 4, 4, 0xFFFF0000u, false));
  /* Top-left corner pixel is on; centre pixel (2,2) is off. */
  TEST_ASSERT_EQ(0xFF, s_fb8888[(1u * k_test_fb_w + 1u) * 4u + 2u]); /* R */
  TEST_ASSERT_EQ(0x00, s_fb8888[(2u * k_test_fb_w + 2u) * 4u + 2u]);
  /* Filled: 3x3 starting at (10,10). */
  TEST_ASSERT_EQ(k_ra_ok, ra_gfx_rect(10, 10, 3, 3, 0xFF0000FFu, true));
  for (uint32_t y = 10; y < 13; y++) {
    for (uint32_t x = 10; x < 13; x++) {
      TEST_ASSERT_EQ(0xFF, s_fb8888[(y * k_test_fb_w + x) * 4u + 0u]); /* B */
    }
  }
  TEST_END("ra_gfx_rect outline + filled");
}

static void test_circle_outline_and_filled(void)
{
  TEST_BEGIN("ra_gfx_circle outline + filled");
  rebind_8888();
  TEST_ASSERT_EQ(k_ra_err_invalid_arg, ra_gfx_circle(0, 0, -1, 0, false));
  /* Outline circle at (16,16) r=5 -- expect (21,16) pixel set. */
  TEST_ASSERT_EQ(k_ra_ok, ra_gfx_circle(16, 16, 5, 0xFFFFFFFFu, false));
  TEST_ASSERT_EQ(0xFF, s_fb8888[(16u * k_test_fb_w + 21u) * 4u + 0u]);
  /* Centre pixel of outline must be untouched. */
  TEST_ASSERT_EQ(0x00, s_fb8888[(16u * k_test_fb_w + 16u) * 4u + 0u]);
  /* Filled disc -- centre pixel now lit. */
  TEST_ASSERT_EQ(k_ra_ok, ra_gfx_circle(16, 16, 4, 0xFFFFFFFFu, true));
  TEST_ASSERT_EQ(0xFF, s_fb8888[(16u * k_test_fb_w + 16u) * 4u + 0u]);
  TEST_END("ra_gfx_circle outline + filled");
}

static void test_text_out_renders_A(void)
{
  TEST_BEGIN("ra_gfx_text_out renders 'A'");
  rebind_8888();
  TEST_ASSERT_EQ(k_ra_ok, ra_gfx_text_out(0, 0, "A", &ra_gfx_font_8x16, 0xFFFFFFFFu, 0xFF000000u));
  /* Row 2 of 'A' is 0x10 -- bit 4 set (column 3, MSB-left). Pixel (3,2)
   * must therefore be foreground white (B/G/R == 0xFF). */
  const uint32_t off_on = (2u * k_test_fb_w + 3u) * 4u;
  TEST_ASSERT_EQ(0xFF, s_fb8888[off_on + 0]);
  TEST_ASSERT_EQ(0xFF, s_fb8888[off_on + 1]);
  TEST_ASSERT_EQ(0xFF, s_fb8888[off_on + 2]);
  /* Pixel (0,2) is background black on row with only bit 4 set. */
  const uint32_t off_off = (2u * k_test_fb_w + 0u) * 4u;
  TEST_ASSERT_EQ(0x00, s_fb8888[off_off + 0]);
  TEST_ASSERT_EQ(0x00, s_fb8888[off_off + 1]);
  TEST_ASSERT_EQ(0x00, s_fb8888[off_off + 2]);
  TEST_END("ra_gfx_text_out renders 'A'");
}

static void test_text_size(void)
{
  TEST_BEGIN("ra_gfx_text_size measures correctly");
  uint32_t w = 0;
  uint32_t h = 0;
  TEST_ASSERT_EQ(k_ra_ok, ra_gfx_text_size("Hello", &ra_gfx_font_8x16, &w, &h));
  TEST_ASSERT_EQ(40u, w); /* 5 chars * 8 px */
  TEST_ASSERT_EQ(16u, h);
  TEST_ASSERT_EQ(k_ra_ok, ra_gfx_text_size("", &ra_gfx_font_8x16, &w, &h));
  TEST_ASSERT_EQ(0u, w);
  TEST_END("ra_gfx_text_size measures correctly");
}

static void test_blit(void)
{
  TEST_BEGIN("ra_gfx_blit copies pixels");
  rebind_8888();
  /* clang-format off */
  /* Tiny 2x2 ARGB source: red, green, blue, white. */
  static const uint8_t src[] = {
    /* (0,0) red    */ 0x00, 0x00, 0xFF, 0xFF,
    /* (1,0) green  */ 0x00, 0xFF, 0x00, 0xFF,
    /* (0,1) blue   */ 0xFF, 0x00, 0x00, 0xFF,
    /* (1,1) white  */ 0xFF, 0xFF, 0xFF, 0xFF,
  };
  /* clang-format on */
  TEST_ASSERT_EQ(k_ra_ok, ra_gfx_blit(src, 2, 2, k_ra_gfx_format_argb8888, 5, 5));
  const uint32_t off = (5u * k_test_fb_w + 5u) * 4u;
  /* (5,5) was red (B=0,G=0,R=FF). */
  TEST_ASSERT_EQ(0x00, s_fb8888[off + 0]);
  TEST_ASSERT_EQ(0xFF, s_fb8888[off + 2]);
  /* (6,6) was white. */
  const uint32_t off2 = (6u * k_test_fb_w + 6u) * 4u;
  TEST_ASSERT_EQ(0xFF, s_fb8888[off2 + 0]);
  TEST_ASSERT_EQ(0xFF, s_fb8888[off2 + 1]);
  TEST_ASSERT_EQ(0xFF, s_fb8888[off2 + 2]);
  TEST_END("ra_gfx_blit copies pixels");
}

static void test_null_args(void)
{
  TEST_BEGIN("NULL args rejected");
  rebind_8888();
  uint32_t w = 0;
  uint32_t h = 0;
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_gfx_text_out(0, 0, NULL, &ra_gfx_font_8x16, 0, 0));
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_gfx_text_out(0, 0, "x", NULL, 0, 0));
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_gfx_text_size(NULL, &ra_gfx_font_8x16, &w, &h));
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_gfx_text_size("x", NULL, &w, &h));
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_gfx_text_size("x", &ra_gfx_font_8x16, NULL, &h));
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_gfx_text_size("x", &ra_gfx_font_8x16, &w, NULL));
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_gfx_blit(NULL, 4, 4, k_ra_gfx_format_argb8888, 0, 0));
  TEST_ASSERT_EQ(k_ra_err_invalid_arg, ra_gfx_blit(s_fb8888, 0, 4, k_ra_gfx_format_argb8888, 0, 0));
  TEST_END("NULL args rejected");
}

static void test_rgb888_format(void)
{
  TEST_BEGIN("RGB888 format round-trip via blit");
  static uint8_t rgb888_fb[k_test_fb_w * k_test_fb_h * 3];
  (void)memset(rgb888_fb, 0, sizeof(rgb888_fb));
  TEST_ASSERT_EQ(k_ra_ok, ra_gfx_init(rgb888_fb, k_test_fb_w, k_test_fb_h, k_ra_gfx_format_rgb888));
  TEST_ASSERT_EQ(k_ra_ok, ra_gfx_pixel(0, 0, 0x00AABBCCu));
  TEST_ASSERT_EQ(0xAA, rgb888_fb[0]); /* R */
  TEST_ASSERT_EQ(0xBB, rgb888_fb[1]); /* G */
  TEST_ASSERT_EQ(0xCC, rgb888_fb[2]); /* B */
  TEST_END("RGB888 format round-trip via blit");
}

int main(void)
{
  test_init_arg_validation();
  test_clear_writes_every_pixel();
  test_pixel_happy_and_oob();
  test_line_horizontal();
  test_rect_outline_and_filled();
  test_circle_outline_and_filled();
  test_text_out_renders_A();
  test_text_size();
  test_blit();
  test_null_args();
  test_rgb888_format();
  return 0;
}
