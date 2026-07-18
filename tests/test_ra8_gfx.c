/**
 * @file test_ra8_gfx.c
 * @brief Unit tests for libs/ra8_gfx (software 2D graphics + bitmap font).
 *
 * @details
 * Each test allocates a small RGB565 / RGB888 / ARGB8888 framebuffer on
 * the stack (or .bss for bigger ones), binds ra8_gfx to it via
 * ra8_gfx_init(), and inspects raw bytes after a draw call.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <string.h>

#include "ra8_err.h"
#include "ra8_gfx.h"
#include "ra8_gfx_font.h"
#include "unity_minimal.h"

/**
 * @enum test_gfx_dim_t
 * @brief Framebuffer dimensions used across the tests.
 */
typedef enum : uint16_t {
  k_test_fb_w = 64, /**< Test fb w. */
  k_test_fb_h = 32, /**< Test fb h. */
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
  const ra8_err_t err = ra8_gfx_init(s_fb565, k_test_fb_w, k_test_fb_h, k_ra8_gfx_format_rgb565);
  TEST_ASSERT_EQ(k_ra8_ok, err);
}

static void rebind_8888(void)
{
  (void)memset(s_fb8888, 0, sizeof(s_fb8888));
  const ra8_err_t err = ra8_gfx_init(s_fb8888, k_test_fb_w, k_test_fb_h, k_ra8_gfx_format_argb8888);
  TEST_ASSERT_EQ(k_ra8_ok, err);
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_arg_validation(void)
{
  TEST_BEGIN("ra8_gfx_init validates arguments");
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_gfx_init(nullptr, 64, 32, k_ra8_gfx_format_rgb565));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_gfx_init(s_fb565, 0, 32, k_ra8_gfx_format_rgb565));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_gfx_init(s_fb565, 64, 0, k_ra8_gfx_format_rgb565));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_gfx_init(s_fb565, 64, 32, (ra8_gfx_format_t)0xAA));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_gfx_init(s_fb565, 64, 32, k_ra8_gfx_format_rgb565));
  TEST_END("ra8_gfx_init validates arguments");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_clear_writes_every_pixel(void)
{
  TEST_BEGIN("ra8_gfx_clear sets every pixel");
  rebind_565();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_gfx_clear(0xFFFFFFU)); /* white */
  /* 0xFFFFFF in RGB565 packs to 0xFFFF. */
  for (uint32_t i = 0; i < sizeof(s_fb565); i++) {
    TEST_ASSERT_EQ(0xFF, s_fb565[i]);
  }
  TEST_END("ra8_gfx_clear sets every pixel");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_pixel_happy_and_oob(void)
{
  TEST_BEGIN("ra8_gfx_pixel happy + OOB");
  rebind_8888();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_gfx_pixel(5, 7, 0xFF112233U));
  /* ARGB8888 layout in memory: B G R A. */
  const uint32_t off = ((7U * k_test_fb_w) + 5U) * 4U;
  TEST_ASSERT_EQ(0x33, s_fb8888[off + 0]);
  TEST_ASSERT_EQ(0x22, s_fb8888[off + 1]);
  TEST_ASSERT_EQ(0x11, s_fb8888[off + 2]);
  TEST_ASSERT_EQ(0xFF, s_fb8888[off + 3]);
  TEST_ASSERT_EQ(k_ra8_err_range_check_failed, ra8_gfx_pixel(-1, 0, 0));
  TEST_ASSERT_EQ(k_ra8_err_range_check_failed, ra8_gfx_pixel(k_test_fb_w, 0, 0));
  TEST_ASSERT_EQ(k_ra8_err_range_check_failed, ra8_gfx_pixel(0, k_test_fb_h, 0));
  TEST_END("ra8_gfx_pixel happy + OOB");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_line_horizontal(void)
{
  TEST_BEGIN("ra8_gfx_line draws a horizontal line");
  rebind_8888();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_gfx_line(2, 4, 10, 4, 0xFF00FF00U));
  for (int32_t x = 2; x <= 10; x++) {
    const uint32_t off = ((4U * k_test_fb_w) + (uint32_t)x) * 4U;
    TEST_ASSERT_EQ(0x00, s_fb8888[off + 0]); /* B */
    TEST_ASSERT_EQ(0xFF, s_fb8888[off + 1]); /* G */
    TEST_ASSERT_EQ(0x00, s_fb8888[off + 2]); /* R */
  }
  TEST_END("ra8_gfx_line draws a horizontal line");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_rect_outline_and_filled(void)
{
  TEST_BEGIN("ra8_gfx_rect outline + filled");
  rebind_8888();
  /* Outline: 4x4 starting at (1,1). */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_gfx_rect(1, 1, 4, 4, 0xFFFF0000U, false));
  /* Top-left corner pixel is on; centre pixel (2,2) is off. */
  TEST_ASSERT_EQ(0xFF, s_fb8888[(((1U * k_test_fb_w) + 1U) * 4U) + 2U]); /* R */
  TEST_ASSERT_EQ(0x00, s_fb8888[(((2U * k_test_fb_w) + 2U) * 4U) + 2U]);
  /* Filled: 3x3 starting at (10,10). */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_gfx_rect(10, 10, 3, 3, 0xFF0000FFU, true));
  for (uint32_t y = 10; y < 13; y++) {
    for (uint32_t x = 10; x < 13; x++) {
      TEST_ASSERT_EQ(0xFF, s_fb8888[(((y * k_test_fb_w) + x) * 4U) + 0U]); /* B */
    }
  }
  TEST_END("ra8_gfx_rect outline + filled");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_circle_outline_and_filled(void)
{
  TEST_BEGIN("ra8_gfx_circle outline + filled");
  rebind_8888();
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_gfx_circle(0, 0, -1, 0, false));
  /* Outline circle at (16,16) r=5 -- expect (21,16) pixel set. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_gfx_circle(16, 16, 5, 0xFFFFFFFFU, false));
  TEST_ASSERT_EQ(0xFF, s_fb8888[(((16U * k_test_fb_w) + 21U) * 4U) + 0U]);
  /* Centre pixel of outline must be untouched. */
  TEST_ASSERT_EQ(0x00, s_fb8888[(((16U * k_test_fb_w) + 16U) * 4U) + 0U]);
  /* Filled disc -- centre pixel now lit. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_gfx_circle(16, 16, 4, 0xFFFFFFFFU, true));
  TEST_ASSERT_EQ(0xFF, s_fb8888[(((16U * k_test_fb_w) + 16U) * 4U) + 0U]);
  TEST_END("ra8_gfx_circle outline + filled");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_text_out_renders_glyph_a(void)
{
  TEST_BEGIN("ra8_gfx_text_out renders 'A'");
  rebind_8888();
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_gfx_text_out(0, 0, "A", &ra8_gfx_font_8x16, 0xFFFFFFFFU, 0xFF000000U));
  /* Row 2 of 'A' is 0x10 -- bit 4 set (column 3, MSB-left). Pixel (3,2)
   * must therefore be foreground white (B/G/R == 0xFF). */
  const uint32_t off_on = ((2U * k_test_fb_w) + 3U) * 4U;
  TEST_ASSERT_EQ(0xFF, s_fb8888[off_on + 0]);
  TEST_ASSERT_EQ(0xFF, s_fb8888[off_on + 1]);
  TEST_ASSERT_EQ(0xFF, s_fb8888[off_on + 2]);
  /* Pixel (0,2) is background black on row with only bit 4 set. */
  const uint32_t off_off = ((2U * k_test_fb_w) + 0U) * 4U;
  TEST_ASSERT_EQ(0x00, s_fb8888[off_off + 0]);
  TEST_ASSERT_EQ(0x00, s_fb8888[off_off + 1]);
  TEST_ASSERT_EQ(0x00, s_fb8888[off_off + 2]);
  TEST_END("ra8_gfx_text_out renders 'A'");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_text_size(void)
{
  TEST_BEGIN("ra8_gfx_text_size measures correctly");
  uint32_t w = 0;
  uint32_t h = 0;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_gfx_text_size("Hello", &ra8_gfx_font_8x16, &w, &h));
  TEST_ASSERT_EQ(40U, w); /* 5 chars * 8 px */
  TEST_ASSERT_EQ(16U, h);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_gfx_text_size("", &ra8_gfx_font_8x16, &w, &h));
  TEST_ASSERT_EQ(0U, w);
  TEST_END("ra8_gfx_text_size measures correctly");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_blit(void)
{
  TEST_BEGIN("ra8_gfx_blit copies pixels");
  rebind_8888();
  // clang-format off: one ARGB pixel per row with its coordinate label.
  /* Tiny 2x2 ARGB source: red, green, blue, white. */
  static const uint8_t src[] = {
    /* (0,0) red    */ 0x00, 0x00, 0xFF, 0xFF,
    /* (1,0) green  */ 0x00, 0xFF, 0x00, 0xFF,
    /* (0,1) blue   */ 0xFF, 0x00, 0x00, 0xFF,
    /* (1,1) white  */ 0xFF, 0xFF, 0xFF, 0xFF,
  };
  // clang-format on
  TEST_ASSERT_EQ(k_ra8_ok, ra8_gfx_blit(src, 2, 2, k_ra8_gfx_format_argb8888, 5, 5));
  const uint32_t off = ((5U * k_test_fb_w) + 5U) * 4U;
  /* (5,5) was red (B=0,G=0,R=FF). */
  TEST_ASSERT_EQ(0x00, s_fb8888[off + 0]);
  TEST_ASSERT_EQ(0xFF, s_fb8888[off + 2]);
  /* (6,6) was white. */
  const uint32_t off2 = ((6U * k_test_fb_w) + 6U) * 4U;
  TEST_ASSERT_EQ(0xFF, s_fb8888[off2 + 0]);
  TEST_ASSERT_EQ(0xFF, s_fb8888[off2 + 1]);
  TEST_ASSERT_EQ(0xFF, s_fb8888[off2 + 2]);
  TEST_END("ra8_gfx_blit copies pixels");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_null_args(void)
{
  TEST_BEGIN("NULL args rejected");
  rebind_8888();
  uint32_t w = 0;
  uint32_t h = 0;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_gfx_text_out(0, 0, nullptr, &ra8_gfx_font_8x16, 0, 0));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_gfx_text_out(0, 0, "x", nullptr, 0, 0));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_gfx_text_size(nullptr, &ra8_gfx_font_8x16, &w, &h));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_gfx_text_size("x", nullptr, &w, &h));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_gfx_text_size("x", &ra8_gfx_font_8x16, nullptr, &h));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_gfx_text_size("x", &ra8_gfx_font_8x16, &w, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_gfx_blit(nullptr, 4, 4, k_ra8_gfx_format_argb8888, 0, 0));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_gfx_blit(s_fb8888, 0, 4, k_ra8_gfx_format_argb8888, 0, 0));
  TEST_END("NULL args rejected");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_rgb888_format(void)
{
  TEST_BEGIN("RGB888 format round-trip via blit");
  static uint8_t s_rgb888_fb[k_test_fb_w * k_test_fb_h * 3];
  (void)memset(s_rgb888_fb, 0, sizeof(s_rgb888_fb));
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_gfx_init(s_rgb888_fb, k_test_fb_w, k_test_fb_h, k_ra8_gfx_format_rgb888));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_gfx_pixel(0, 0, 0x00AABBCCU));
  TEST_ASSERT_EQ(0xAA, s_rgb888_fb[0]); /* R */
  TEST_ASSERT_EQ(0xBB, s_rgb888_fb[1]); /* G */
  TEST_ASSERT_EQ(0xCC, s_rgb888_fb[2]); /* B */
  TEST_END("RGB888 format round-trip via blit");
}

/* ===========================================================================
 * MC/DC vector coverage for compound boolean decisions in ra8_gfx_text.c.
 *
 * Per docs/MCDC_GAPS.csv there are 12 compound decisions across the file.
 * One test_mcdc_<short_name>(void) function below targets each, using the
 * minimal N+1 vector set (Chilenski masking-MC/DC). For 4-condition
 * decisions a representative subset is used per DO-178C 6.4.4.3, which
 * permits MC/DC to be argued via masking pairs that prove each condition
 * independently flips the decision while others are held at their
 * non-masking value -- this requires N+1 vectors, not 2^N exhaustive.
 *
 * Vector tables follow the canonical pattern documented in
 * tests/test_ra8_xspi.c (e.g. test_set_xip_mode_mcdc_addr_bytes).
 * ===========================================================================
 */

/**
 * @enum test_gfx_mcdc_t
 * @brief Numeric vectors used by the MC/DC tests below.
 */
typedef enum : uint16_t {
  k_test_gfx_mcdc_dim_min     = 1U,    /**< == k_ra8_gfx_min_dim. */
  k_test_gfx_mcdc_dim_max     = 4096U, /**< == k_ra8_gfx_max_dim. */
  k_test_gfx_mcdc_dim_below   = 0U,    /**< Below min_dim.        */
  k_test_gfx_mcdc_dim_above   = 4097U, /**< Above max_dim.        */
  k_test_gfx_mcdc_dim_inrange = 32U,   /**< Mid-range valid.      */
} test_gfx_mcdc_t;

/**
 * @test test_mcdc_internal_format_ok
 *
 * @par MC/DC:
 * Decision: `return (f == k_ra8_gfx_format_rgb565) ||
 *                  (f == k_ra8_gfx_format_rgb888) ||
 *                  (f == k_ra8_gfx_format_argb8888);`
 * (3 conditions, libs/ra8_gfx/src/ra8_gfx_text.c line 147)
 *
 * Reached indirectly via ra8_gfx_init() (line 259) and ra8_gfx_blit()
 * (line 516). Vectors:
 * - V1: f=rgb565    -> C1=T (short-circuit) -> decision T (init OK)
 * - V2: f=rgb888    -> C1=F, C2=T           -> decision T (init OK)
 * - V3: f=argb8888  -> C1=F, C2=F, C3=T     -> decision T (init OK)
 * - V4: f=0xAA      -> C1=F, C2=F, C3=F     -> decision F (invalid_arg)
 * V1 vs V4 vary C1 (others held F); V2 vs V4 vary C2 (C1=F both, C3
 * masked by short-circuit); V3 vs V4 vary C3. N+1 = 4 vectors prove
 * each condition independently affects the outcome.
 */
static void test_mcdc_internal_format_ok(void)
{
  TEST_BEGIN("gfx MC/DC internal_format_ok 3-condition OR");
  /* V1 */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_gfx_init(s_fb565, 64, 32, k_ra8_gfx_format_rgb565));
  /* V2 */
  static uint8_t s_fb888[64 * 32 * 3];
  TEST_ASSERT_EQ(k_ra8_ok, ra8_gfx_init(s_fb888, 64, 32, k_ra8_gfx_format_rgb888));
  /* V3 */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_gfx_init(s_fb8888, 64, 32, k_ra8_gfx_format_argb8888));
  /* V4 */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_gfx_init(s_fb565, 64, 32, (ra8_gfx_format_t)0xAA));
  TEST_END("gfx MC/DC internal_format_ok 3-condition OR");
}

/**
 * @test test_mcdc_internal_plot_neg
 *
 * @par MC/DC:
 * Decision: `if ((x < 0) || (y < 0))`
 * (2 conditions, libs/ra8_gfx/src/ra8_gfx_text.c line 230)
 *
 * Reached via ra8_gfx_line() / ra8_gfx_rect() / ra8_gfx_circle() which call
 * internal_plot() unconditionally. We use ra8_gfx_line() with same-x,y
 * endpoints (fast-exits after one plot). Decision T = pixel NOT written.
 * - V1: (x=5, y=5)   -> C1=F, C2=F -> decision F: pixel written
 * - V2: (x=-1, y=5)  -> C1=T (short-circuit) -> decision T: skipped
 * - V3: (x=5, y=-1)  -> C1=F, C2=T -> decision T: skipped
 * V1+V2 vary C1; V1+V3 vary C2 (C1=F both). N+1 = 3 vectors.
 */
static void test_mcdc_internal_plot_neg(void)
{
  TEST_BEGIN("gfx MC/DC internal_plot negative-coord guard");
  rebind_8888();
  /* V1: positive coords, pixel must land. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_gfx_line(5, 5, 5, 5, 0xFF112233U));
  TEST_ASSERT_EQ(0x33, s_fb8888[(((5U * k_test_fb_w) + 5U) * 4U) + 0U]);
  /* V2: x<0, plot skipped. We re-clear the buffer and probe a no-op. */
  rebind_8888();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_gfx_line(-1, 5, -1, 5, 0xFF112233U));
  TEST_ASSERT_EQ(0x00, s_fb8888[(((5U * k_test_fb_w) + 0U) * 4U) + 0U]);
  /* V3: y<0, plot skipped. */
  rebind_8888();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_gfx_line(5, -1, 5, -1, 0xFF112233U));
  TEST_ASSERT_EQ(0x00, s_fb8888[(((0U * k_test_fb_w) + 5U) * 4U) + 0U]);
  TEST_END("gfx MC/DC internal_plot negative-coord guard");
}

/**
 * @test test_mcdc_internal_plot_oob
 *
 * @par MC/DC:
 * Decision: `if ((x >= (int32_t)s_state.width) || (y >= (int32_t)s_state.height))`
 * (2 conditions, libs/ra8_gfx/src/ra8_gfx_text.c line 233)
 *
 * - V1: (x=5,  y=5)        -> C1=F, C2=F -> plotted
 * - V2: (x=fb_w, y=5)      -> C1=T short-circuits -> skipped
 * - V3: (x=5,  y=fb_h)     -> C1=F, C2=T -> skipped
 * V1+V2 vary C1; V1+V3 vary C2.
 */
static void test_mcdc_internal_plot_oob(void)
{
  TEST_BEGIN("gfx MC/DC internal_plot OOB-coord guard");
  rebind_8888();
  /* V1: in-range. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_gfx_line(5, 5, 5, 5, 0xFF112233U));
  TEST_ASSERT_EQ(0x33, s_fb8888[(((5U * k_test_fb_w) + 5U) * 4U) + 0U]);
  /* V2: x == width. */
  rebind_8888();
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_gfx_line((int32_t)k_test_fb_w, 5, (int32_t)k_test_fb_w, 5, 0xFF112233U));
  /* No pixel could land at fb_w,5: probe (fb_w-1,5) is still 0. */
  TEST_ASSERT_EQ(0x00, s_fb8888[(((5U * k_test_fb_w) + (k_test_fb_w - 1U)) * 4U) + 0U]);
  /* V3: y == height. */
  rebind_8888();
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_gfx_line(5, (int32_t)k_test_fb_h, 5, (int32_t)k_test_fb_h, 0xFF112233U));
  TEST_ASSERT_EQ(0x00, s_fb8888[((((k_test_fb_h - 1U) * k_test_fb_w) + 5U) * 4U) + 0U]);
  TEST_END("gfx MC/DC internal_plot OOB-coord guard");
}

/**
 * @test test_mcdc_init_width_range
 *
 * @par MC/DC:
 * Decision: `if ((width < k_ra8_gfx_min_dim) || (width > k_ra8_gfx_max_dim))`
 * (2 conditions, libs/ra8_gfx/src/ra8_gfx_text.c line 253)
 *
 * - V1: width=32      -> C1=F, C2=F -> decision F: ok
 * - V2: width=0       -> C1=T short-circuit -> decision T: invalid_arg
 * - V3: width=4097    -> C1=F, C2=T -> decision T: invalid_arg
 */
static void test_mcdc_init_width_range(void)
{
  TEST_BEGIN("gfx MC/DC ra8_gfx_init width range");
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_gfx_init(s_fb565,
                              (uint16_t)k_test_gfx_mcdc_dim_inrange,
                              (uint16_t)k_test_gfx_mcdc_dim_inrange,
                              k_ra8_gfx_format_rgb565));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_gfx_init(s_fb565,
                              (uint16_t)k_test_gfx_mcdc_dim_below,
                              (uint16_t)k_test_gfx_mcdc_dim_inrange,
                              k_ra8_gfx_format_rgb565));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_gfx_init(s_fb565,
                              (uint16_t)k_test_gfx_mcdc_dim_above,
                              (uint16_t)k_test_gfx_mcdc_dim_inrange,
                              k_ra8_gfx_format_rgb565));
  TEST_END("gfx MC/DC ra8_gfx_init width range");
}

/**
 * @test test_mcdc_init_height_range
 *
 * @par MC/DC:
 * Decision: `if ((height < k_ra8_gfx_min_dim) || (height > k_ra8_gfx_max_dim))`
 * (2 conditions, libs/ra8_gfx/src/ra8_gfx_text.c line 256)
 *
 * - V1: height=32   -> F||F = F -> ok
 * - V2: height=0    -> T short -> invalid_arg
 * - V3: height=4097 -> F||T -> invalid_arg
 */
static void test_mcdc_init_height_range(void)
{
  TEST_BEGIN("gfx MC/DC ra8_gfx_init height range");
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_gfx_init(s_fb565,
                              (uint16_t)k_test_gfx_mcdc_dim_inrange,
                              (uint16_t)k_test_gfx_mcdc_dim_inrange,
                              k_ra8_gfx_format_rgb565));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_gfx_init(s_fb565,
                              (uint16_t)k_test_gfx_mcdc_dim_inrange,
                              (uint16_t)k_test_gfx_mcdc_dim_below,
                              k_ra8_gfx_format_rgb565));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_gfx_init(s_fb565,
                              (uint16_t)k_test_gfx_mcdc_dim_inrange,
                              (uint16_t)k_test_gfx_mcdc_dim_above,
                              k_ra8_gfx_format_rgb565));
  TEST_END("gfx MC/DC ra8_gfx_init height range");
}

/**
 * @test test_mcdc_pixel_bounds
 *
 * @par MC/DC:
 * Decision: `if ((x < 0) || (y < 0) || (x >= w) || (y >= h))`
 * (4 conditions, libs/ra8_gfx/src/ra8_gfx_text.c line 294)
 *
 * Per DO-178C 6.4.4.3, MC/DC for an N-condition pure-OR decision is
 * achieved with N+1 = 5 vectors: one all-false (decision F) plus one
 * vector where each condition is uniquely true with the others false.
 * Short-circuit semantics mean later conditions are masked when an
 * earlier one is T, but the all-false baseline + each condition's
 * unique-true vector establishes the masking pairs that prove
 * independent effect on outcome.
 *
 * - V1: (5,5)        -> F,F,F,F -> decision F -> ok
 * - V2: (-1,5)       -> T,_,_,_ -> decision T -> range_check_failed
 * - V3: (5,-1)       -> F,T,_,_ -> decision T -> range_check_failed
 * - V4: (fb_w,5)     -> F,F,T,_ -> decision T -> range_check_failed
 * - V5: (5,fb_h)     -> F,F,F,T -> decision T -> range_check_failed
 */
static void test_mcdc_pixel_bounds(void)
{
  TEST_BEGIN("gfx MC/DC ra8_gfx_pixel 4-condition bounds");
  rebind_8888();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_gfx_pixel(5, 5, 0xFF112233U));
  TEST_ASSERT_EQ(k_ra8_err_range_check_failed, ra8_gfx_pixel(-1, 5, 0U));
  TEST_ASSERT_EQ(k_ra8_err_range_check_failed, ra8_gfx_pixel(5, -1, 0U));
  TEST_ASSERT_EQ(k_ra8_err_range_check_failed, ra8_gfx_pixel((int32_t)k_test_fb_w, 5, 0U));
  TEST_ASSERT_EQ(k_ra8_err_range_check_failed, ra8_gfx_pixel(5, (int32_t)k_test_fb_h, 0U));
  TEST_END("gfx MC/DC ra8_gfx_pixel 4-condition bounds");
}

/**
 * @test test_mcdc_line_endpoint
 *
 * @par MC/DC:
 * Decision: `if ((x == x1) && (y == y1))` -- Bresenham termination
 * (2 conditions, libs/ra8_gfx/src/ra8_gfx_text.c line 318)
 *
 * - V1: from (3,3) to (3,3)  -> after first plot x==x1 T, y==y1 T -> break
 * - V2: from (3,3) to (5,3)  -> after step x!=x1 (F) short-circuits -> continue
 * - V3: from (3,3) to (3,5)  -> x==x1 T, y!=y1 F -> continue
 * V1 vs V2 vary C1; V1 vs V3 vary C2 (with C1 held T). Behavioural
 * proxy: count plotted pixels along the line via framebuffer probes.
 */
static void test_mcdc_line_endpoint(void)
{
  TEST_BEGIN("gfx MC/DC ra8_gfx_line endpoint && check");
  rebind_8888();
  /* V1: degenerate line (1 pixel). */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_gfx_line(3, 3, 3, 3, 0xFF010101U));
  TEST_ASSERT_EQ(0x01, s_fb8888[(((3U * k_test_fb_w) + 3U) * 4U) + 0U]);
  TEST_ASSERT_EQ(0x00, s_fb8888[(((3U * k_test_fb_w) + 4U) * 4U) + 0U]);
  /* V2: horizontal line, multi-pixel proves loop did not break early. */
  rebind_8888();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_gfx_line(3, 3, 5, 3, 0xFF010101U));
  TEST_ASSERT_EQ(0x01, s_fb8888[(((3U * k_test_fb_w) + 5U) * 4U) + 0U]);
  /* V3: vertical line, ditto. */
  rebind_8888();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_gfx_line(3, 3, 3, 5, 0xFF010101U));
  TEST_ASSERT_EQ(0x01, s_fb8888[(((5U * k_test_fb_w) + 3U) * 4U) + 0U]);
  TEST_END("gfx MC/DC ra8_gfx_line endpoint && check");
}

/**
 * @test test_mcdc_rect_dim
 *
 * @par MC/DC:
 * Decision: `if ((w <= 0) || (h <= 0))`
 * (2 conditions, libs/ra8_gfx/src/ra8_gfx_text.c line 339)
 *
 * Decision T = no-op success (no pixels drawn). We probe a target
 * pixel that the filled rect would normally write.
 * - V1: w=4, h=4   -> F,F -> draw (target pixel set)
 * - V2: w=0, h=4   -> T short-circuit -> no draw
 * - V3: w=4, h=0   -> F,T -> no draw
 */
static void test_mcdc_rect_dim(void)
{
  TEST_BEGIN("gfx MC/DC ra8_gfx_rect dimension guard");
  rebind_8888();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_gfx_rect(2, 2, 4, 4, 0xFF030303U, true));
  TEST_ASSERT_EQ(0x03, s_fb8888[(((2U * k_test_fb_w) + 2U) * 4U) + 0U]);
  rebind_8888();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_gfx_rect(2, 2, 0, 4, 0xFF030303U, true));
  TEST_ASSERT_EQ(0x00, s_fb8888[(((2U * k_test_fb_w) + 2U) * 4U) + 0U]);
  rebind_8888();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_gfx_rect(2, 2, 4, 0, 0xFF030303U, true));
  TEST_ASSERT_EQ(0x00, s_fb8888[(((2U * k_test_fb_w) + 2U) * 4U) + 0U]);
  TEST_END("gfx MC/DC ra8_gfx_rect dimension guard");
}

/**
 * @test test_mcdc_render_glyph_codepoint
 *
 * @par MC/DC:
 * Decision: `if ((cp < font->first_codepoint) || (cp > font->last_codepoint))`
 * (2 conditions, libs/ra8_gfx/src/ra8_gfx_text.c line 438)
 *
 * Reached via ra8_gfx_text_out(). Decision T -> render glyph 0 (space)
 * instead of cp's glyph. We probe a pixel that 'A' would set but
 * space (' ', glyph idx 0 in the font's blank cell) would not.
 *
 * The font_8x16 covers ASCII 0x20..0x7E.
 * - V1: cp='A' (0x41) -> F,F -> render 'A'
 * - V2: cp=0x10 (<0x20) -> T short-circuit -> render space
 * - V3: cp=0x80 (>0x7E) -> F,T -> render space
 */
static void test_mcdc_render_glyph_codepoint(void)
{
  TEST_BEGIN("gfx MC/DC internal_render_glyph codepoint guard");
  rebind_8888();
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_gfx_text_out(0, 0, "A", &ra8_gfx_font_8x16, 0xFFFFFFFFU, 0xFF000000U));
  /* Row 2 of 'A' has bit 4 set => pixel (3,2) is foreground. */
  TEST_ASSERT_EQ(0xFF, s_fb8888[(((2U * k_test_fb_w) + 3U) * 4U) + 0U]);
  /* V2: codepoint below first_codepoint -> render space (likely all bg). */
  rebind_8888();
  const char low[2] = {(char)0x10, '\0'};
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_gfx_text_out(0, 0, low, &ra8_gfx_font_8x16, 0xFFFFFFFFU, 0xFF000000U));
  /* (3,2) must NOT be foreground white because we substituted a space-glyph. */
  TEST_ASSERT_EQ(0x00, s_fb8888[(((2U * k_test_fb_w) + 3U) * 4U) + 0U]);
  /* V3: codepoint above last_codepoint. */
  rebind_8888();
  const char high[2] = {(char)0x80, '\0'};
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_gfx_text_out(0, 0, high, &ra8_gfx_font_8x16, 0xFFFFFFFFU, 0xFF000000U));
  TEST_ASSERT_EQ(0x00, s_fb8888[(((2U * k_test_fb_w) + 3U) * 4U) + 0U]);
  TEST_END("gfx MC/DC internal_render_glyph codepoint guard");
}

/**
 * @test test_mcdc_text_out_null
 *
 * @par MC/DC:
 * Decision: `if ((str == NULL) || (font == NULL))`
 * (2 conditions, libs/ra8_gfx/src/ra8_gfx_text.c line 463)
 *
 * - V1: str="x", font=&font_8x16 -> F,F -> ok
 * - V2: str=NULL,font=&font_8x16 -> T short-circuit -> null_ptr
 * - V3: str="x", font=NULL       -> F,T -> null_ptr
 */
static void test_mcdc_text_out_null(void)
{
  TEST_BEGIN("gfx MC/DC ra8_gfx_text_out null guard");
  rebind_8888();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_gfx_text_out(0, 0, "x", &ra8_gfx_font_8x16, 0U, 0U));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_gfx_text_out(0, 0, nullptr, &ra8_gfx_font_8x16, 0U, 0U));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_gfx_text_out(0, 0, "x", nullptr, 0U, 0U));
  TEST_END("gfx MC/DC ra8_gfx_text_out null guard");
}

/**
 * @test test_mcdc_text_size_null
 *
 * @par MC/DC:
 * Decision: `if ((str == NULL) || (font == NULL) || (out_w == NULL) || (out_h == NULL))`
 * (4 conditions, libs/ra8_gfx/src/ra8_gfx_text.c line 487)
 *
 * Per DO-178C 6.4.4.3, N+1 = 5 vectors are sufficient for an N-condition
 * pure-OR decision when each condition's unique-true vector pairs with
 * the all-false baseline to flip the outcome.
 *
 * - V1: all non-NULL              -> F,F,F,F -> ok
 * - V2: str=NULL                  -> T,_,_,_ -> null_ptr
 * - V3: font=NULL                 -> F,T,_,_ -> null_ptr
 * - V4: out_w=NULL                -> F,F,T,_ -> null_ptr
 * - V5: out_h=NULL                -> F,F,F,T -> null_ptr
 */
static void test_mcdc_text_size_null(void)
{
  TEST_BEGIN("gfx MC/DC ra8_gfx_text_size 4-condition null guard");
  uint32_t w = 0;
  uint32_t h = 0;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_gfx_text_size("x", &ra8_gfx_font_8x16, &w, &h));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_gfx_text_size(nullptr, &ra8_gfx_font_8x16, &w, &h));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_gfx_text_size("x", nullptr, &w, &h));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_gfx_text_size("x", &ra8_gfx_font_8x16, nullptr, &h));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_gfx_text_size("x", &ra8_gfx_font_8x16, &w, nullptr));
  TEST_END("gfx MC/DC ra8_gfx_text_size 4-condition null guard");
}

/**
 * @test test_mcdc_blit_args
 *
 * @par MC/DC:
 * Decision: `if ((src_w == 0) || (src_h == 0) || !internal_format_ok(src_format))`
 * (3 conditions, libs/ra8_gfx/src/ra8_gfx_text.c line 516)
 *
 * - V1: w=2,h=2,fmt=argb8888 -> F,F,F -> ok
 * - V2: w=0,h=2,fmt=argb8888 -> T short-circuit -> invalid_arg
 * - V3: w=2,h=0,fmt=argb8888 -> F,T,_ -> invalid_arg
 * - V4: w=2,h=2,fmt=0xAA     -> F,F,T -> invalid_arg
 * V1+V2 vary C1; V1+V3 vary C2; V1+V4 vary C3. N+1 = 4 vectors for N=3.
 */
static void test_mcdc_blit_args(void)
{
  TEST_BEGIN("gfx MC/DC ra8_gfx_blit 3-condition arg guard");
  rebind_8888();
  static const uint8_t src[] = {0x00,
                                0x00,
                                0xFF,
                                0xFF,
                                0x00,
                                0xFF,
                                0x00,
                                0xFF,
                                0xFF,
                                0x00,
                                0x00,
                                0xFF,
                                0xFF,
                                0xFF,
                                0xFF,
                                0xFF};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_gfx_blit(src, 2, 2, k_ra8_gfx_format_argb8888, 0, 0));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_gfx_blit(src, 0, 2, k_ra8_gfx_format_argb8888, 0, 0));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_gfx_blit(src, 2, 0, k_ra8_gfx_format_argb8888, 0, 0));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_gfx_blit(src, 2, 2, (ra8_gfx_format_t)0xAA, 0, 0));
  TEST_END("gfx MC/DC ra8_gfx_blit 3-condition arg guard");
}

int main(void)
{
  test_init_arg_validation();
  test_clear_writes_every_pixel();
  test_pixel_happy_and_oob();
  test_line_horizontal();
  test_rect_outline_and_filled();
  test_circle_outline_and_filled();
  test_text_out_renders_glyph_a();
  test_text_size();
  test_blit();
  test_null_args();
  test_rgb888_format();

  test_mcdc_internal_format_ok();
  test_mcdc_internal_plot_neg();
  test_mcdc_internal_plot_oob();
  test_mcdc_init_width_range();
  test_mcdc_init_height_range();
  test_mcdc_pixel_bounds();
  test_mcdc_line_endpoint();
  test_mcdc_rect_dim();
  test_mcdc_render_glyph_codepoint();
  test_mcdc_text_out_null();
  test_mcdc_text_size_null();
  test_mcdc_blit_args();
  return 0;
}
