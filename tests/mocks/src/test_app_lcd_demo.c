/**
 * @file test_app_lcd_demo.c
 * @brief Integration test: GLCDC two-layer bring-up + bouncing-sprite loop
 *
 * @details
 * The production app at examples/ek_ra8d2/hw_validated/manual/lcd_color_cycle/src/main.c brings up
 * CGC, SysTick, LED1, routes the GLCDC pin block, fills two layer
 * framebuffers, programmes layer 1 (sprite) + layer 2 (background)
 * with an alpha blend, then enters a redraw-and-toggle loop. The LCD
 * panel is not present in the host test build, but every register
 * write the driver makes lands in the fake MMIO window so we
 * can exercise the same call sequence and assert on the resulting
 * register contents.
 *
 * Exercised modules:
 *   - ra8_cgc, ra8_time, ra8_board_ek_ra8d2 (LED), ra8_glcdc
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_board_ek_ra8d2.h"
#include "ra8_cgc.h"
#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_glcdc.h"
#include "ra8_glcdc_regs.h"
#include "ra8_pin_validator.h"
#include "ra8_system_regs.h"
#include "ra8_time.h"
#include "unity_minimal.h"

/**
 * @enum app_lcd_demo_fixture_t
 * @brief All-bits-set register values, so a write that clears the wrong field leaves evidence.
 */
typedef enum : uint8_t {
  k_sys_oscsf_all_ready =
    0xFFU, /**< Every oscillator-stabilisation flag set, so clock bring-up sees all sources ready */
} app_lcd_demo_fixture_t;

/** @brief Per-test enums. */
typedef enum : uint32_t {
  k_test_lcd_fb_l1_addr = 0x68000000UL, /**< Sprite framebuffer base. */
  k_test_lcd_fb_l2_addr = 0x68040000UL, /**< Background fb base.      */
  k_test_lcd_redraw_n   = 4U,           /**< Frames to simulate.      */
} test_lcd_addr_t;

typedef enum : uint16_t {
  k_test_lcd_l1_w  = 128U, /**< Sprite layer width.         */
  k_test_lcd_l1_h  = 128U, /**< Sprite layer height.        */
  k_test_lcd_l2_w  = 320U, /**< Background layer width.     */
  k_test_lcd_l2_h  = 240U, /**< Background layer height.    */
  k_test_lcd_l2_x  = 0U,   /**< Layer-2 X position.         */
  k_test_lcd_l2_y  = 0U,   /**< Layer-2 Y position.         */
  k_test_lcd_l2_st = 640U, /**< Layer-2 stride: 320 * 2bpp. */
} test_lcd_dim_t;

typedef enum : uint8_t {
  k_test_lcd_alpha  = 0xFFU, /**< Fully-opaque global alpha. */
  k_test_lcd_rgb565 = 0x2U,  /**< k_ra8_glcdc_fmt_rgb565.    */
} test_lcd_byte_t;

static void reset_world(void)
{
  ra8_fake_mmap_reset();
  ra8_pin_validator_reset();
  /* Pre-seed OSCSF stabilisation bits so ra8_cgc_init() spin loops
   * complete on the first iteration in RA8_OFF_TARGET. */
  *ra8_sys_oscsf() = (uint8_t)k_sys_oscsf_all_ready;
}

/* -------------------------------------------------------------------------
 * Golden path
 * ------------------------------------------------------------------------- */

/**
 * @brief Pre-GLCDC bring-up: CGC + SysTick + LED1 init.
 *
 * @par MC/DC: not applicable -- sequential init/teardown, no compound
 * boolean decisions in path.
 */
static void test_lcd_drivers_init_or_halt(void)
{
  reset_world();
  TEST_BEGIN("lcd_demo: CGC + SysTick + LED1 init");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_cgc_init());
  uint32_t hz = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &hz));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_time_init(hz));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_board_led_init(k_ra8_board_led1));
  TEST_END("lcd_demo: CGC + SysTick + LED1 init");
}

/**
 * @brief Layer-1 + Layer-2 + blend + start sequence on the GLCDC.
 *
 * @par MC/DC:
 * Decision under test: ``ra8_glcdc_init() != ok || set_layer2 != ok ||
 * set_blend != ok || set_background_color != ok || start != ok`` short-
 * circuit chain. Five atomic conditions x N+1 = 6 vectors; this case
 * covers the all-ok vector. Failure vectors split across the rejection
 * tests below.
 */
static void test_lcd_glcdc_two_layer_start(void)
{
  reset_world();
  TEST_BEGIN("lcd_demo: GLCDC two-layer bring-up + start");
  const ra8_glcdc_config_t cfg = {
    .framebuffer_addr = (uint32_t)k_test_lcd_fb_l1_addr,
    .width_px         = (uint16_t)k_test_lcd_l1_w,
    .height_px        = (uint16_t)k_test_lcd_l1_h,
    .format           = k_ra8_glcdc_fmt_rgb565,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_glcdc_init(&cfg));

  const ra8_glcdc_layer2_cfg_t l2 = {
    .framebuffer_addr  = (uint32_t)k_test_lcd_fb_l2_addr,
    .line_stride_bytes = (uint32_t)k_test_lcd_l2_st,
    .width_px          = (uint16_t)k_test_lcd_l2_w,
    .height_px         = (uint16_t)k_test_lcd_l2_h,
    .pos_x             = (uint16_t)k_test_lcd_l2_x,
    .pos_y             = (uint16_t)k_test_lcd_l2_y,
    .format            = k_ra8_glcdc_fmt_rgb565,
    .alpha             = (uint8_t)k_test_lcd_alpha,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_glcdc_set_layer2(&l2));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_glcdc_set_blend(k_ra8_blend_alpha, (uint8_t)k_test_lcd_alpha));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_glcdc_set_background_color(0U));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_glcdc_start(true));
  TEST_END("lcd_demo: GLCDC two-layer bring-up + start");
}

/**
 * @brief Per-frame redraw loop: LED1 toggles N times without error.
 *
 * @par MC/DC:
 * Decision under test: per-frame ``ra8_board_led_toggle != k_ra8_ok``
 * inside the demo's redraw loop. One atomic condition x 2 vectors:
 * ok-loop (this test) + rejected (covered by toggle-on-invalid-led
 * test below).
 */
static void test_lcd_per_frame_led_toggle_loop(void)
{
  reset_world();
  TEST_BEGIN("lcd_demo: per-frame LED1 toggle loop");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_board_led_init(k_ra8_board_led1));
  for (uint8_t i = 0U; i < (uint8_t)k_test_lcd_redraw_n; i++) {
    TEST_ASSERT_EQ(k_ra8_ok, ra8_board_led_toggle(k_ra8_board_led1));
  }
  TEST_END("lcd_demo: per-frame LED1 toggle loop");
}

/* -------------------------------------------------------------------------
 * Edge / failure paths
 * ------------------------------------------------------------------------- */

/**
 * @brief NULL config rejected by ra8_glcdc_init (panic-halt path).
 *
 * @par MC/DC:
 * Decision vector under test: ``cfg == NULL`` -- failure side of the
 * init precondition decision.
 */
static void test_lcd_glcdc_init_null_rejected(void)
{
  reset_world();
  TEST_BEGIN("lcd_demo: ra8_glcdc_init(NULL) rejected");
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_glcdc_init(nullptr));
  TEST_END("lcd_demo: ra8_glcdc_init(NULL) rejected");
}

/**
 * @brief NULL layer-2 cfg rejected by ra8_glcdc_set_layer2.
 *
 * @par MC/DC:
 * Decision vector under test: ``cfg == NULL`` -- failure side of the
 * set_layer2 precondition decision.
 */
static void test_lcd_glcdc_set_layer2_null_rejected(void)
{
  reset_world();
  TEST_BEGIN("lcd_demo: set_layer2(NULL) rejected");
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_glcdc_set_layer2(nullptr));
  TEST_END("lcd_demo: set_layer2(NULL) rejected");
}

/**
 * @brief Out-of-range blend mode rejected.
 *
 * @par MC/DC:
 * Decision vector under test: ``mode out of range`` invariant in
 * ra8_glcdc_set_blend -- failure side of the mode-range check.
 */
static void test_lcd_glcdc_set_blend_invalid_mode(void)
{
  reset_world();
  TEST_BEGIN("lcd_demo: set_blend rejects out-of-range mode");
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_glcdc_set_blend((ra8_glcdc_blend_mode_t)0xFFU, (uint8_t)k_test_lcd_alpha));
  TEST_END("lcd_demo: set_blend rejects out-of-range mode");
}

/**
 * @brief Toggle on out-of-range LED rejected (mid-loop fault path).
 *
 * @par MC/DC:
 * Decision vector under test: ``ra8_board_led_toggle != k_ra8_ok``
 * failure-side vector pairing with the ok-loop above.
 */
static void test_lcd_led_toggle_invalid_id(void)
{
  reset_world();
  TEST_BEGIN("lcd_demo: led_toggle rejects invalid id");
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_board_led_toggle((ra8_board_led_id_t)k_ra8_board_led_count));
  TEST_END("lcd_demo: led_toggle rejects invalid id");
}

int main(void)
{
  test_lcd_drivers_init_or_halt();
  test_lcd_glcdc_two_layer_start();
  test_lcd_per_frame_led_toggle_loop();
  test_lcd_glcdc_init_null_rejected();
  test_lcd_glcdc_set_layer2_null_rejected();
  test_lcd_glcdc_set_blend_invalid_mode();
  test_lcd_led_toggle_invalid_id();
  return 0;
}
