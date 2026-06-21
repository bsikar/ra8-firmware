/**
 * @file test_ra_gfx_text.c
 * @brief MC/DC unit tests for libs/ra_gfx/src/ra_gfx_text.c
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra_err.h"
#include "ra_gfx.h"
#include "unity_minimal.h"

typedef enum : uint16_t {
  k_test_gfx_dim_zero   = 0U,
  k_test_gfx_dim_normal = 64U,
  k_test_gfx_dim_over   = 4097U,
} test_gfx_dim_t;

typedef enum : int32_t {
  k_test_rect_pos = 2, /**< On-screen rectangle origin (x and y).        */
  k_test_rect_dim = 4, /**< On-screen rectangle width and height.        */
} test_rect_geom_t;

typedef enum : uint32_t {
  k_test_col_clear = 0x000000FFU, /**< Fill A: blue (nonzero in RGB565).  */
  k_test_col_rect  = 0x00FF0000U, /**< Fill B: red  (distinct in RGB565). */
} test_rect_col_t;

typedef enum : uint32_t {
  k_test_bpp_565  = 2U, /**< RGB565 bytes per pixel.   */
  k_test_bpp_8888 = 4U, /**< ARGB8888 bytes per pixel. */
} test_bpp_t;

static uint8_t s_fb[64U * 64U * 4U];

/**
 * @brief Byte offset of pixel (k_test_rect_pos, k_test_rect_pos) for @p bpp.
 *
 * @param[in] bpp Bytes per pixel of the bound format.
 * @return Byte index into s_fb of that pixel's first byte.
 * @retval 0 Only when the position and stride are zero (never for these tests).
 * @pre The framebuffer is bound at k_test_gfx_dim_normal width.
 * @pre bpp is the bound format's bytes per pixel.
 * @post The returned offset is within s_fb.
 * @post The returned offset addresses the pixel's first byte.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static size_t test_pixel_off(size_t bpp)
{
  return (((size_t)k_test_rect_pos * (size_t)k_test_gfx_dim_normal) + (size_t)k_test_rect_pos) *
         bpp;
}

/**
 * @test test_mcdc_gfx_init_width_range
 *
 * @par MC/DC:
 * Decision: ``if ((width < k_ra_gfx_min_dim) || (width > k_ra_gfx_max_dim))``
 * (2 conditions, libs/ra_gfx/src/ra_gfx_text.c around line 387)
 * Per DO-178C 6.4.4.3 N+1 = 3 vectors.
 */
static void test_mcdc_gfx_init_width_range(void)
{
  TEST_BEGIN("gfx_init MC/DC: (width<min)||(width>max)");
  TEST_ASSERT_EQ(k_ra_ok,
                 ra_gfx_init(s_fb,
                             (uint16_t)k_test_gfx_dim_normal,
                             (uint16_t)k_test_gfx_dim_normal,
                             k_ra_gfx_format_rgb565));
  TEST_ASSERT_EQ(k_ra_err_invalid_arg,
                 ra_gfx_init(s_fb,
                             (uint16_t)k_test_gfx_dim_zero,
                             (uint16_t)k_test_gfx_dim_normal,
                             k_ra_gfx_format_rgb565));
  TEST_ASSERT_EQ(k_ra_err_invalid_arg,
                 ra_gfx_init(s_fb,
                             (uint16_t)k_test_gfx_dim_over,
                             (uint16_t)k_test_gfx_dim_normal,
                             k_ra_gfx_format_rgb565));
  TEST_END("gfx_init MC/DC: (width<min)||(width>max)");
}

/**
 * @test test_mcdc_gfx_init_height_range
 *
 * @par MC/DC:
 * Decision: ``if ((height < k_ra_gfx_min_dim) || (height > k_ra_gfx_max_dim))``
 * (2 conditions, libs/ra_gfx/src/ra_gfx_text.c around line 390)
 * Per DO-178C 6.4.4.3 N+1 = 3 vectors.
 */
static void test_mcdc_gfx_init_height_range(void)
{
  TEST_BEGIN("gfx_init MC/DC: (height<min)||(height>max)");
  TEST_ASSERT_EQ(k_ra_ok,
                 ra_gfx_init(s_fb,
                             (uint16_t)k_test_gfx_dim_normal,
                             (uint16_t)k_test_gfx_dim_normal,
                             k_ra_gfx_format_rgb565));
  TEST_ASSERT_EQ(k_ra_err_invalid_arg,
                 ra_gfx_init(s_fb,
                             (uint16_t)k_test_gfx_dim_normal,
                             (uint16_t)k_test_gfx_dim_zero,
                             k_ra_gfx_format_rgb565));
  TEST_ASSERT_EQ(k_ra_err_invalid_arg,
                 ra_gfx_init(s_fb,
                             (uint16_t)k_test_gfx_dim_normal,
                             (uint16_t)k_test_gfx_dim_over,
                             k_ra_gfx_format_rgb565));
  TEST_END("gfx_init MC/DC: (height<min)||(height>max)");
}

/**
 * @test test_mcdc_fill_rect_565_clip
 *
 * @par MC/DC:
 * Decision: ``if ((x1 <= x0) || (y1 <= y0))`` in internal_fill_rect_565 (the
 * RGB565 span-fill clip guard, 2 conditions). Reached through ra_gfx_rect.
 * Per DO-178C 6.4.4.3 N+1 = 3 vectors:
 * - V1: rect (2,2,4,4) fully on a 64x64 fb -> x1>x0, y1>y0 (F,F) -> covered
 *       pixel changes from the clear colour.
 * - V2: x at the right edge (x=64) -> x1<=x0 (T, short-circuit) -> no write.
 * - V3: y at the bottom edge (y=64) -> x1>x0, y1<=y0 (F,T) -> no write.
 * V1+V2 prove (x1<=x0) independently flips the outcome; V1+V3 prove (y1<=y0)
 * does likewise.
 */
static void test_mcdc_fill_rect_565_clip(void)
{
  TEST_BEGIN("fill_rect_565 MC/DC: (x1<=x0)||(y1<=y0)");
  const int32_t dim = (int32_t)k_test_gfx_dim_normal;
  const size_t  off = test_pixel_off((size_t)k_test_bpp_565);
  TEST_ASSERT_EQ(k_ra_ok, ra_gfx_init(s_fb, (uint16_t)dim, (uint16_t)dim, k_ra_gfx_format_rgb565));

  /* V1: fully on-screen (F,F) -> decision false -> the covered pixel changes. */
  TEST_ASSERT_EQ(k_ra_ok, ra_gfx_clear(k_test_col_clear));
  const uint8_t cleared = s_fb[off];
  TEST_ASSERT_EQ(k_ra_ok,
                 ra_gfx_rect(k_test_rect_pos,
                             k_test_rect_pos,
                             k_test_rect_dim,
                             k_test_rect_dim,
                             k_test_col_rect,
                             true));
  TEST_ASSERT(s_fb[off] != cleared);

  /* V2: x at the right edge (T, short-circuit) -> decision true -> no write. */
  TEST_ASSERT_EQ(k_ra_ok, ra_gfx_clear(k_test_col_clear));
  TEST_ASSERT_EQ(
    k_ra_ok,
    ra_gfx_rect(dim, k_test_rect_pos, k_test_rect_dim, k_test_rect_dim, k_test_col_rect, true));
  TEST_ASSERT_EQ(cleared, s_fb[off]);

  /* V3: y at the bottom edge (F,T) -> decision true -> no write. */
  TEST_ASSERT_EQ(k_ra_ok, ra_gfx_clear(k_test_col_clear));
  TEST_ASSERT_EQ(
    k_ra_ok,
    ra_gfx_rect(k_test_rect_pos, dim, k_test_rect_dim, k_test_rect_dim, k_test_col_rect, true));
  TEST_ASSERT_EQ(cleared, s_fb[off]);
  TEST_END("fill_rect_565 MC/DC: (x1<=x0)||(y1<=y0)");
}

/**
 * @test test_fill_rect_dispatch
 *
 * @par Branch coverage:
 * Decision: ``if (s_state.format == k_ra_gfx_format_rgb565)`` in
 * internal_fill_rect (single condition). The RGB565 fast path is covered by
 * test_mcdc_fill_rect_565_clip; this exercises the false branch (the per-pixel
 * fallback) by filling a rectangle in ARGB8888 and confirming the covered pixel
 * changes.
 */
static void test_fill_rect_dispatch(void)
{
  TEST_BEGIN("fill_rect dispatch: argb8888 per-pixel fallback");
  const int32_t dim = (int32_t)k_test_gfx_dim_normal;
  const size_t  off = test_pixel_off((size_t)k_test_bpp_8888);
  TEST_ASSERT_EQ(k_ra_ok,
                 ra_gfx_init(s_fb, (uint16_t)dim, (uint16_t)dim, k_ra_gfx_format_argb8888));
  TEST_ASSERT_EQ(k_ra_ok, ra_gfx_clear(k_test_col_clear));
  const uint8_t before = s_fb[off];
  TEST_ASSERT_EQ(k_ra_ok,
                 ra_gfx_rect(k_test_rect_pos,
                             k_test_rect_pos,
                             k_test_rect_dim,
                             k_test_rect_dim,
                             k_test_col_rect,
                             true));
  TEST_ASSERT(s_fb[off] != before);
  TEST_END("fill_rect dispatch: argb8888 per-pixel fallback");
}

int32_t main(void)
{
  test_mcdc_gfx_init_width_range();
  test_mcdc_gfx_init_height_range();
  test_mcdc_fill_rect_565_clip();
  test_fill_rect_dispatch();
  (void)fprintf(stderr, "[OK ] test_ra_gfx_text.c\n");
  return 0;
}
