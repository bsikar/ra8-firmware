/**
 * @file test_app_drw_fill_demo.c
 * @brief Integration test: DRW fill-rect demo logic (pure)
 *
 * @details
 * Mirrors examples/ek_ra8d2/drw_fill_demo/main.c. The DRW register
 * sequencing is owned + covered by ra8_drw's own unit tests; this test
 * pins down the app-level pure logic:
 *
 *  - The framebuffer pixel-index arithmetic.
 *  - The fill verdict ``ok = centre==green && tl==0 && br==0`` with full
 *    MC/DC, using a host-side software fill to model the expected result.
 *
 * No ra8_fake_mmap MMIO is required, so this test runs in-process.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>
#include <string.h>

#include "unity_minimal.h"

typedef enum : uint32_t {
  k_t_drw_fill_argb = 0xFF00FF00U, /**< T DRW fill argb. */
} t_drw_color_t;

typedef enum : uint16_t {
  k_t_drw_fb_dim  = 32U, /**< T DRW fb dim.  */
  k_t_drw_rect_xy = 8U,  /**< T DRW rect xy. */
  k_t_drw_rect_wh = 16U, /**< T DRW rect wh. */
  k_t_drw_center  = 16U, /**< T DRW center.  */
} t_drw_geom_t;

static uint32_t s_fb[(uint32_t)k_t_drw_fb_dim * (uint32_t)k_t_drw_fb_dim];

/** @brief Mirror of drw_demo_px(). */
static uint32_t px(uint16_t x, uint16_t y)
{
  return ((uint32_t)y * (uint32_t)k_t_drw_fb_dim) + (uint32_t)x;
}

/** @brief Software model of the DRW rectangle fill (what HW should produce). */
static void sw_fill_rect(void)
{
  (void)memset(s_fb, 0, sizeof(s_fb));
  for (uint16_t y = (uint16_t)k_t_drw_rect_xy; y < (uint16_t)(k_t_drw_rect_xy + k_t_drw_rect_wh);
       ++y) {
    for (uint16_t x = (uint16_t)k_t_drw_rect_xy; x < (uint16_t)(k_t_drw_rect_xy + k_t_drw_rect_wh);
         ++x) {
      s_fb[px(x, y)] = (uint32_t)k_t_drw_fill_argb;
    }
  }
}

/** @brief Mirror of the demo's verdict. */
static uint8_t compute_ok(uint32_t centre, uint32_t tl, uint32_t br)
{
  return (centre == (uint32_t)k_t_drw_fill_argb && tl == 0U && br == 0U) ? 1U : 0U;
}

/**
 * @brief The pixel-index arithmetic and the modelled fill agree.
 *
 * @par MC/DC:
 * No compound decision; arithmetic + a software fill. Vectors confirm the
 * centre is filled and the corners are clear.
 */
static void test_drw_app_fill_model(void)
{
  TEST_BEGIN("drw_fill_demo: software fill matches expectation");
  TEST_ASSERT_EQ(0U, px(0U, 0U));
  TEST_ASSERT_EQ((((uint32_t)k_t_drw_center * (uint32_t)k_t_drw_fb_dim) + (uint32_t)k_t_drw_center),
                 px(k_t_drw_center, k_t_drw_center));
  sw_fill_rect();
  TEST_ASSERT_EQ(k_t_drw_fill_argb, s_fb[px(k_t_drw_center, k_t_drw_center)]);
  TEST_ASSERT_EQ(0U, s_fb[px(0U, 0U)]);
  TEST_ASSERT_EQ(0U, s_fb[px((uint16_t)(k_t_drw_fb_dim - 1U), (uint16_t)(k_t_drw_fb_dim - 1U))]);
  TEST_END("drw_fill_demo: software fill matches expectation");
}

/**
 * @brief Verdict ``centre==green && tl==0 && br==0`` exercised for MC/DC.
 *
 * @par MC/DC:
 * 3 conditions. N+1 = 4 vectors:
 * - green,0,0 -> 1   (control)
 * - 0,0,0     -> 0   (varies centre)
 * - green,1,0 -> 0   (varies tl)
 * - green,0,1 -> 0   (varies br)
 */
static void test_drw_app_verdict_mcdc(void)
{
  TEST_BEGIN("drw_fill_demo: verdict MC/DC");
  TEST_ASSERT_EQ(1U, compute_ok((uint32_t)k_t_drw_fill_argb, 0U, 0U)); /* control     */
  TEST_ASSERT_EQ(0U, compute_ok(0U, 0U, 0U));                          /* vary centre */
  TEST_ASSERT_EQ(0U, compute_ok((uint32_t)k_t_drw_fill_argb, 1U, 0U)); /* vary tl     */
  TEST_ASSERT_EQ(0U, compute_ok((uint32_t)k_t_drw_fill_argb, 0U, 1U)); /* vary br     */
  TEST_END("drw_fill_demo: verdict MC/DC");
}

int main(void)
{
  test_drw_app_fill_model();
  test_drw_app_verdict_mcdc();
  return 0;
}
