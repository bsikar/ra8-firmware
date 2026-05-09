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

static uint8_t s_fb[64U * 64U * 4U];

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

int32_t main(void)
{
  test_mcdc_gfx_init_width_range();
  test_mcdc_gfx_init_height_range();
  (void)fprintf(stderr, "[OK ] test_ra_gfx_text.c\n");
  return 0;
}
