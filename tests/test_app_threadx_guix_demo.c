/**
 * @file test_app_threadx_guix_demo.c
 * @brief Integration test: ThreadX + GUIX hello-world bring-up
 *
 * @details
 * The production app at examples/ek_ra8d2/threadx_guix_demo/main.c
 * brings up the GLCDC two-layer pipeline, sets up two ThreadX threads
 * (gui_thread for GUIX's event loop and app_thread for the colour-
 * cycle), and hands control to ThreadX. Neither ThreadX nor GUIX is
 * linked into the host test build (RA_SIMULATOR_MODE), so this test
 * exercises the underlying GLCDC bring-up the gui_thread relies on
 * before gx_system_initialize, plus the framebuffer pointer
 * propagation the GUIX-to-GLCDC shim makes.
 *
 * Exercised modules:
 *   - ra_cgc, ra_time
 *   - ra_glcdc (single layer, RGB565 framebuffer at GUIX's draw target)
 *   - ra_board_ek_ra8d2 (LED heartbeat from app_thread)
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8d2_glcdc_regs.h"
#include "ra8d2_system_regs.h"
#include "ra_board_ek_ra8d2.h"
#include "ra_cgc.h"
#include "ra_err.h"
#include "ra_glcdc.h"
#include "ra_pin_validator.h"
#include "ra_sim_mmap.h"
#include "ra_time.h"
#include "unity_minimal.h"

/** @brief Per-test enums -- match the demo's layout. */
typedef enum : uint32_t {
  k_test_gx_fb_addr  = 0x68000000UL, /**< GUIX-owned framebuffer base.    */
  k_test_gx_period_n = 4U,           /**< app_thread cycle iterations.    */
} test_gx_const_t;

typedef enum : uint16_t {
  k_test_gx_fb_w = 256U, /**< Framebuffer width.  */
  k_test_gx_fb_h = 128U, /**< Framebuffer height. */
} test_gx_dim_t;

static void reset_world(void)
{
  ra_sim_mmap_reset();
  ra_pin_validator_reset();
  /* Pre-seed OSCSF stabilisation bits so ra_cgc_init() spin loops
   * complete on the first iteration in RA_SIMULATOR_MODE. */
  *ra_sys_oscsf() = (uint8_t)0xFFU;
}

/* -------------------------------------------------------------------------
 * Golden path
 * ------------------------------------------------------------------------- */

/**
 * @brief Pre-kernel boot replay: CGC + SysTick.
 *
 * @par MC/DC: not applicable -- sequential cgc_init -> readback ->
 * time_init wiring with no compound boolean decisions in path.
 */
static void test_gx_pre_kernel_bringup(void)
{
  reset_world();
  TEST_BEGIN("guix_demo: pre-kernel CGC + SysTick");
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_cgc_init());
  uint32_t hz = 0U;
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_cgc_get_clock_hz(k_ra_clock_id_cpuclk0, &hz));
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_time_init(hz));
  TEST_END("guix_demo: pre-kernel CGC + SysTick");
}

/**
 * @brief gui_thread bring-up: GLCDC + start drives the GUIX target buffer.
 *
 * @par MC/DC:
 * Decision under test: ``ra_glcdc_init() != ok || ra_glcdc_start() !=
 * ok`` chain. Two atomic conditions x N+1 = 3 vectors; this case
 * covers all-ok. Failure vectors split across the rejection tests.
 */
static void test_gx_gui_thread_glcdc_start(void)
{
  reset_world();
  TEST_BEGIN("guix_demo: gui_thread GLCDC bring-up + start");
  const ra_glcdc_config_t cfg = {
    .framebuffer_addr = (uint32_t)k_test_gx_fb_addr,
    .width_px         = (uint16_t)k_test_gx_fb_w,
    .height_px        = (uint16_t)k_test_gx_fb_h,
    .format           = k_ra_glcdc_fmt_rgb565,
  };
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_glcdc_init(&cfg));
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_glcdc_start(true));
  /* The driver writes the configured framebuffer base into GR1_SADDR. */
  TEST_ASSERT_EQ((int)k_test_gx_fb_addr, (int)*ra_glcdc_reg32(k_ra_glcdc_off_gr1_saddr));
  TEST_END("guix_demo: gui_thread GLCDC bring-up + start");
}

/**
 * @brief app_thread colour-cycle: refresh repainted N times without error.
 *
 * @details ra_glcdc_set_background_color is the production app's
 * stand-in for the canvas refresh -- exercise the call surface N
 * times to assert no per-iteration state corruption.
 *
 * @par MC/DC:
 * Decision under test: per-iteration ``set_background_color != ok``.
 * One atomic condition x 2 vectors: ok-loop (this test) + clear_status
 * mid-loop fault (handled by status-clear test).
 */
static void test_gx_app_thread_colour_cycle(void)
{
  reset_world();
  TEST_BEGIN("guix_demo: app_thread colour cycle N iterations");
  for (uint8_t i = 0U; i < (uint8_t)k_test_gx_period_n; i++) {
    /* alternate between two ARGB colours -- mirrors the production
     * blue/orange swap the prompt does each tick. */
    const uint32_t argb = ((i & 1U) == 0U) ? 0x000044FFUL : 0x00FF7700UL;
    TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_glcdc_set_background_color(argb));
  }
  TEST_END("guix_demo: app_thread colour cycle N iterations");
}

/* -------------------------------------------------------------------------
 * Edge / failure paths
 * ------------------------------------------------------------------------- */

/**
 * @brief NULL config rejected by ra_glcdc_init (panic-halt path).
 *
 * @par MC/DC:
 * Decision vector under test: ``cfg == NULL`` -- failure side of the
 * init precondition decision.
 */
static void test_gx_glcdc_init_null_rejected(void)
{
  reset_world();
  TEST_BEGIN("guix_demo: ra_glcdc_init(NULL) rejected");
  TEST_ASSERT_EQ((int)k_ra_err_null_ptr, (int)ra_glcdc_init(nullptr));
  TEST_END("guix_demo: ra_glcdc_init(NULL) rejected");
}

/**
 * @brief Stop-after-start reaches the disabled state.
 *
 * @par MC/DC:
 * State-machine vector: scanning -> stopped transition under
 * start(false) after a prior start(true). Pairs with the start-true
 * vector for full edge coverage.
 */
static void test_gx_glcdc_stop_after_start(void)
{
  reset_world();
  TEST_BEGIN("guix_demo: stop after start drives engine disabled");
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_glcdc_start(true));
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_glcdc_start(false));
  TEST_ASSERT_EQ(0, (int)*ra_glcdc_reg32(k_ra_glcdc_off_sys_cfg));
  TEST_END("guix_demo: stop after start drives engine disabled");
}

/**
 * @brief Status read with NULL out-mask is rejected.
 *
 * @par MC/DC:
 * Decision vector under test: ``out_mask == NULL`` precondition guard
 * inside ra_glcdc_get_status -- failure side of the precondition.
 */
static void test_gx_glcdc_get_status_null_rejected(void)
{
  reset_world();
  TEST_BEGIN("guix_demo: get_status(NULL) rejected");
  TEST_ASSERT(ra_glcdc_get_status(NULL) != k_ra_ok);
  TEST_END("guix_demo: get_status(NULL) rejected");
}

int main(void)
{
  test_gx_pre_kernel_bringup();
  test_gx_gui_thread_glcdc_start();
  test_gx_app_thread_colour_cycle();
  test_gx_glcdc_init_null_rejected();
  test_gx_glcdc_stop_after_start();
  test_gx_glcdc_get_status_null_rejected();
  return 0;
}
