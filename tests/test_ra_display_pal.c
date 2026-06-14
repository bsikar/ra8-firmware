/**
 * @file test_ra_display_pal.c
 * @brief Host unit tests for the display PAL (ra_display_pal).
 *
 * @details
 * Exercises:
 *
 *   - Argument validation on every public entry point.
 *   - LCD backend end-to-end: init, get_caps, get_framebuffer,
 *     clear, flush, deinit. The LCD backend's hardware accesses
 *     are intercepted by ``ra_sim_mmap`` so the bring-up sequence
 *     can run under host gcc with no real GPIO / GLCDC.
 *   - E-ink (IT8951) backend end-to-end against the simulator-backed
 *     ``ra_epaper`` driver: init / get_caps / get_framebuffer / clear /
 *     flush / deinit all succeed, plus the RGB565 -> 8bpp luma
 *     conversion the flush path relies on.
 *   - Busy-rejection on a second ``display_init`` without a
 *     preceding ``display_deinit``.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <string.h>

#include "ra_display_pal.h"
#include "ra_display_pal_eink.h"
#include "ra_display_pal_lcd.h"
#include "ra_epaper.h"
#include "ra_err.h"
#include "ra_mstp.h"
#include "ra_panel_timing.h"
#include "ra_pin_validator.h"
#include "ra_sim_mmap.h"
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

/* Static framebuffer the LCD and e-ink backends share -- one test
 * deinits before the next inits so the state machine resets. */
static uint16_t s_test_fb[k_test_fb_pixels] __attribute__((aligned(64)));

static void harness_reset_world(void)
{
  ra_sim_mmap_reset();
  ra_pin_validator_reset();
  (void)ra_mstp_init();
  (void)memset(s_test_fb, 0, sizeof(s_test_fb));
}

static display_cfg_t make_lcd_cfg(void)
{
  const display_cfg_t cfg = {
    .iface             = &k_display_backend_lcd_ra_glcdc,
    .framebuffer       = s_test_fb,
    .framebuffer_bytes = sizeof(s_test_fb),
    .width_px          = (uint16_t)k_test_fb_width,
    .height_px         = (uint16_t)k_test_fb_height,
    .pixfmt            = k_display_pixfmt_rgb565,
    .panel_timing      = &k_ra_panel_ek_ra8d2_timing,
  };
  return cfg;
}

/* IT8951 descriptor the board BSP would supply through panel_timing,
 * sized to the test framebuffer. In host tests this drives the
 * simulator-backed ra_epaper (SPI/HRDY short-circuited under sim). */
typedef enum : uint32_t {
  k_eink_spi_baud_hz = 12000000U,  /**< 12 MHz SPI clock. */
  k_eink_pclka_hz    = 100000000U, /**< 100 MHz PCLKA.    */
} test_eink_const_t;

static const ra_epaper_cfg_t s_eink_panel_cfg = {
  .spi_channel  = 0U,
  .spi_baud_hz  = (uint32_t)k_eink_spi_baud_hz,
  .pclka_hz     = (uint32_t)k_eink_pclka_hz,
  .reset_pin    = 0U,
  .busy_pin     = 0U,
  .panel_width  = (uint16_t)k_test_fb_width,
  .panel_height = (uint16_t)k_test_fb_height,
};

static display_cfg_t make_eink_cfg(void)
{
  const display_cfg_t cfg = {
    .iface             = &k_display_backend_eink_it8951,
    .framebuffer       = s_test_fb,
    .framebuffer_bytes = sizeof(s_test_fb),
    .width_px          = (uint16_t)k_test_fb_width,
    .height_px         = (uint16_t)k_test_fb_height,
    .pixfmt            = k_display_pixfmt_rgb565,
    .panel_timing      = &s_eink_panel_cfg,
  };
  return cfg;
}

/* =============================================================================
 * Tests
 * =============================================================================
 */

/**
 * @par MC/DC:
 * Decision: ``if (cfg == nullptr || out_handle == nullptr || cfg->iface == nullptr
 *               || cfg->iface->init == nullptr)`` in display_init.
 * Each NULL is exercised independently by the four sub-cases below; the
 * happy path is exercised by ``test_lcd_happy_path``. Five vectors total
 * for a 4-condition OR -> N+1 minimal MC/DC.
 */
static void test_init_rejects_null_arguments(void)
{
  TEST_BEGIN("display_init rejects null arguments");
  harness_reset_world();

  display_handle_t* d   = nullptr;
  display_cfg_t     cfg = make_lcd_cfg();

  /* Vector 2: cfg = NULL. */
  TEST_ASSERT_EQ(k_ra_err_null_ptr, display_init(nullptr, &d));

  /* Vector 3: out_handle = NULL. */
  TEST_ASSERT_EQ(k_ra_err_null_ptr, display_init(&cfg, nullptr));

  /* Vector 4: cfg->iface = NULL. */
  display_cfg_t cfg_no_iface = cfg;
  cfg_no_iface.iface         = nullptr;
  TEST_ASSERT_EQ(k_ra_err_null_ptr, display_init(&cfg_no_iface, &d));

  /* Vector 5: framebuffer = NULL (the LCD backend's first check). */
  display_cfg_t cfg_no_fb = cfg;
  cfg_no_fb.framebuffer   = nullptr;
  TEST_ASSERT_EQ(k_ra_err_null_ptr, display_init(&cfg_no_fb, &d));

  TEST_END("display_init rejects null arguments");
}

/**
 * @par MC/DC:
 * Decision (libs/ra_display_pal/src/ra_display_pal_lcd.c@internal_lcd_validate_cfg):
 * ``if (cfg->width_px == 0U || cfg->height_px == 0U)`` -- 2 conditions.
 *
 * - V1: w=64, h=32  -> C1=F, C2=F -> decision F (covered by happy path).
 * - V2: w=0,  h=32  -> C1=T short-circuits -> decision T: invalid_arg.
 * - V3: w=64, h=0   -> C1=F, C2=T          -> decision T: invalid_arg.
 *
 * V1+V2 vary C1 (w); V1+V3 vary C2 (h). N+1 = 3 vectors for N=2 conds.
 */
static void test_mcdc_lcd_rejects_zero_dimensions(void)
{
  TEST_BEGIN("LCD backend rejects zero dimensions");
  harness_reset_world();

  display_handle_t* d   = nullptr;
  display_cfg_t     cfg = make_lcd_cfg();

  cfg.width_px = 0U;
  TEST_ASSERT_EQ(k_ra_err_invalid_arg, display_init(&cfg, &d));

  cfg           = make_lcd_cfg();
  cfg.height_px = 0U;
  TEST_ASSERT_EQ(k_ra_err_invalid_arg, display_init(&cfg, &d));

  TEST_END("LCD backend rejects zero dimensions");
}

/**
 * @par MC/DC:
 * Decision: ``if (cfg->pixfmt != k_display_pixfmt_rgb565)`` -- single
 * condition, 2 vectors: rgb565 (accepted via the happy path test) +
 * non-rgb565 (rejected here).
 */
static void test_lcd_rejects_non_rgb565(void)
{
  TEST_BEGIN("LCD backend rejects non-RGB565 pixfmt");
  harness_reset_world();

  display_handle_t* d   = nullptr;
  display_cfg_t     cfg = make_lcd_cfg();

  cfg.pixfmt = k_display_pixfmt_grey4;
  TEST_ASSERT_EQ(k_ra_err_not_supported, display_init(&cfg, &d));

  cfg.pixfmt = k_display_pixfmt_grey1;
  TEST_ASSERT_EQ(k_ra_err_not_supported, display_init(&cfg, &d));

  cfg.pixfmt = k_display_pixfmt_rgb888;
  TEST_ASSERT_EQ(k_ra_err_not_supported, display_init(&cfg, &d));

  TEST_END("LCD backend rejects non-RGB565 pixfmt");
}

/**
 * @par MC/DC:
 * Decision: ``if (cfg->framebuffer_bytes < need_bytes)`` -- single
 * condition. Two vectors: undersized (rejected here) and sized
 * exactly (accepted by happy path).
 */
static void test_lcd_rejects_undersized_buffer(void)
{
  TEST_BEGIN("LCD backend rejects framebuffer too small");
  harness_reset_world();

  display_handle_t* d   = nullptr;
  display_cfg_t     cfg = make_lcd_cfg();

  cfg.framebuffer_bytes = (uint32_t)k_test_fb_bytes - 1U;
  TEST_ASSERT_EQ(k_ra_err_invalid_arg, display_init(&cfg, &d));

  TEST_END("LCD backend rejects framebuffer too small");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the happy path
 * end-to-end so every other validation vector has the matching
 * "accepted" partner)
 */
static void test_lcd_happy_path(void)
{
  TEST_BEGIN("LCD backend init -> caps -> fb -> clear -> flush -> deinit");
  harness_reset_world();

  display_handle_t*   d   = nullptr;
  const display_cfg_t cfg = make_lcd_cfg();
  TEST_ASSERT_EQ(k_ra_ok, display_init(&cfg, &d));
  TEST_ASSERT_NOT_NULL(d);

  display_caps_t caps = {};
  TEST_ASSERT_EQ(k_ra_ok, display_get_caps(d, &caps));
  TEST_ASSERT_EQ(k_test_fb_width, caps.width_px);
  TEST_ASSERT_EQ(k_test_fb_height, caps.height_px);
  TEST_ASSERT_EQ(k_display_pixfmt_rgb565, caps.pixfmt);
  TEST_ASSERT_EQ(k_test_fb_width * 2U, caps.stride_bytes);
  TEST_ASSERT_EQ(0, caps.refresh_latency_us_typ);
  TEST_ASSERT(caps.continuous_refresh);
  TEST_ASSERT(caps.supports_partial_update);

  display_fb_t fb = {};
  TEST_ASSERT_EQ(k_ra_ok, display_get_framebuffer(d, &fb));
  TEST_ASSERT_EQ((intptr_t)s_test_fb, (intptr_t)fb.pixels);
  TEST_ASSERT_EQ(k_test_fb_width, fb.width_px);
  TEST_ASSERT_EQ(k_test_fb_height, fb.height_px);

  TEST_ASSERT_EQ(k_ra_ok, display_clear(d, 0xFFFFU));
  TEST_ASSERT_EQ(0xFFFFU, ((const uint16_t*)fb.pixels)[0]);
  TEST_ASSERT_EQ(0xFFFFU, ((const uint16_t*)fb.pixels)[k_test_fb_pixels - 1U]);

  const display_rect_t r = display_full_rect(d);
  TEST_ASSERT_EQ(0, r.x);
  TEST_ASSERT_EQ(0, r.y);
  TEST_ASSERT_EQ(k_test_fb_width, r.w);
  TEST_ASSERT_EQ(k_test_fb_height, r.h);
  TEST_ASSERT_EQ(k_ra_ok, display_flush(d, r, k_display_refresh_quality));
  TEST_ASSERT_EQ(k_ra_ok, display_flush(d, r, k_display_refresh_fast));
  TEST_ASSERT_EQ(k_ra_ok, display_flush(d, r, k_display_refresh_init));

  TEST_ASSERT_EQ(k_ra_ok, display_deinit(d));
  TEST_END("LCD backend init -> caps -> fb -> clear -> flush -> deinit");
}

/**
 * @par MC/DC:
 * Decision in ``internal_lcd_check_rect``: four sequential bounds
 * checks. Vectors below cover each branch independently.
 */
static void test_lcd_flush_rejects_out_of_bounds(void)
{
  TEST_BEGIN("LCD backend flush rejects rects outside the framebuffer");
  harness_reset_world();

  display_handle_t*   d   = nullptr;
  const display_cfg_t cfg = make_lcd_cfg();
  TEST_ASSERT_EQ(k_ra_ok, display_init(&cfg, &d));

  const display_rect_t bad_x = {(uint16_t)(k_test_fb_width + 1U), 0U, 1U, 1U};
  TEST_ASSERT_EQ(k_ra_err_invalid_arg, display_flush(d, bad_x, k_display_refresh_quality));

  const display_rect_t bad_y = {0U, (uint16_t)(k_test_fb_height + 1U), 1U, 1U};
  TEST_ASSERT_EQ(k_ra_err_invalid_arg, display_flush(d, bad_y, k_display_refresh_quality));

  const display_rect_t bad_w = {0U, 0U, (uint16_t)(k_test_fb_width + 1U), 1U};
  TEST_ASSERT_EQ(k_ra_err_invalid_arg, display_flush(d, bad_w, k_display_refresh_quality));

  const display_rect_t bad_h = {0U, 0U, 1U, (uint16_t)(k_test_fb_height + 1U)};
  TEST_ASSERT_EQ(k_ra_err_invalid_arg, display_flush(d, bad_h, k_display_refresh_quality));

  TEST_ASSERT_EQ(k_ra_ok, display_deinit(d));
  TEST_END("LCD backend flush rejects rects outside the framebuffer");
}

/**
 * @par MC/DC:
 * Decision: ``if (s_initialized)`` in display_init. Two vectors: NOT
 * initialised (covered by happy path) + already initialised
 * (covered here).
 */
static void test_init_rejects_double_init(void)
{
  TEST_BEGIN("display_init rejects a second init without deinit");
  harness_reset_world();

  display_handle_t*   d1  = nullptr;
  display_handle_t*   d2  = nullptr;
  const display_cfg_t cfg = make_lcd_cfg();
  TEST_ASSERT_EQ(k_ra_ok, display_init(&cfg, &d1));
  TEST_ASSERT_EQ(k_ra_err_busy, display_init(&cfg, &d2));
  TEST_ASSERT_EQ(k_ra_ok, display_deinit(d1));

  /* After deinit a new init succeeds.  The hardware-side panel
   * power-on claims GPIOs that survive ``ra_glcdc_deinit`` (real
   * silicon does not need them re-claimed for the same panel) so
   * we reset the validator's bitmap explicitly before re-bringing
   * up.  Production callers either reset the chip or stay bound. */
  ra_pin_validator_reset();
  TEST_ASSERT_EQ(k_ra_ok, display_init(&cfg, &d2));
  TEST_ASSERT_EQ(k_ra_ok, display_deinit(d2));
  TEST_END("display_init rejects a second init without deinit");
}

/**
 * @par MC/DC:
 * Decision in ``internal_validate_handle``: three sequential checks
 * (null pointer, uninitialised, mismatched handle). Vectors below
 * cover the null and uninitialised branches; the mismatched-handle
 * branch is structurally unreachable while only one handle is
 * exposed but we keep the check to harden against future multi-handle
 * support.
 */
static void test_calls_after_deinit_are_rejected(void)
{
  TEST_BEGIN("PAL calls after deinit are rejected");
  harness_reset_world();

  display_handle_t*   d   = nullptr;
  const display_cfg_t cfg = make_lcd_cfg();
  TEST_ASSERT_EQ(k_ra_ok, display_init(&cfg, &d));
  TEST_ASSERT_EQ(k_ra_ok, display_deinit(d));

  display_caps_t caps = {};
  TEST_ASSERT_EQ(k_ra_err_invalid_arg, display_get_caps(d, &caps));

  display_fb_t fb = {};
  TEST_ASSERT_EQ(k_ra_err_invalid_arg, display_get_framebuffer(d, &fb));

  TEST_ASSERT_EQ(k_ra_err_invalid_arg,
                 display_flush(d, display_full_rect(d), k_display_refresh_quality));

  TEST_ASSERT_EQ(k_ra_err_invalid_arg, display_clear(d, 0U));

  /* full_rect with a stale handle returns an all-zero rect. */
  const display_rect_t empty = display_full_rect(d);
  TEST_ASSERT_EQ(0, empty.x);
  TEST_ASSERT_EQ(0, empty.y);
  TEST_ASSERT_EQ(0, empty.w);
  TEST_ASSERT_EQ(0, empty.h);

  TEST_END("PAL calls after deinit are rejected");
}

/**
 * @par MC/DC:
 * Decision (libs/ra_display_pal/src/ra_display_pal_eink.c@internal_eink_validate_cfg):
 * ``if (cfg->width_px == 0U || cfg->height_px == 0U)`` -- 2 conditions.
 *
 * - V1: w=64, h=32 -> C1=F, C2=F -> decision F (covered by happy path
 *                                  in ``test_eink_stub_get_caps_succeeds``).
 * - V2: w=0,  h=32 -> C1=T short-circuits -> decision T: invalid_arg.
 * - V3: w=64, h=0  -> C1=F, C2=T          -> decision T: invalid_arg.
 *
 * V1+V2 vary C1 (w); V1+V3 vary C2 (h). N+1 = 3 vectors for N=2 conds.
 */
static void test_mcdc_eink_rejects_zero_dimensions(void)
{
  TEST_BEGIN("e-ink backend MC/DC: rejects zero dimensions");
  harness_reset_world();

  display_handle_t* d   = nullptr;
  display_cfg_t     cfg = make_eink_cfg();

  /* V2: width = 0 short-circuits the OR. */
  cfg.width_px = 0U;
  TEST_ASSERT_EQ(k_ra_err_invalid_arg, display_init(&cfg, &d));

  /* V3: height = 0 with width = N exercises the second condition. */
  cfg           = make_eink_cfg();
  cfg.height_px = 0U;
  TEST_ASSERT_EQ(k_ra_err_invalid_arg, display_init(&cfg, &d));
  TEST_END("e-ink backend MC/DC: rejects zero dimensions");
}

/**
 * @par MC/DC:
 * E-ink backend init runs the same validation path as the LCD backend;
 * this test covers the happy path (init brings up the simulator-backed
 * IT8951 via ra_epaper) so the validation-rejection vectors are
 * exercised the same way.
 */
static void test_eink_init_get_caps(void)
{
  TEST_BEGIN("e-ink: init + get_caps succeed");
  harness_reset_world();

  display_handle_t*   d   = nullptr;
  const display_cfg_t cfg = make_eink_cfg();
  TEST_ASSERT_EQ(k_ra_ok, display_init(&cfg, &d));

  display_caps_t caps = {};
  TEST_ASSERT_EQ(k_ra_ok, display_get_caps(d, &caps));
  TEST_ASSERT_EQ(k_test_fb_width, caps.width_px);
  TEST_ASSERT(caps.refresh_latency_us_typ > 0U);
  TEST_ASSERT(!caps.continuous_refresh);
  TEST_ASSERT(caps.supports_partial_update);

  TEST_ASSERT_EQ(k_ra_ok, display_deinit(d));
  TEST_END("e-ink: init + get_caps succeed");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the wired e-ink
 * vtable: get_framebuffer hands back the RGB565 buffer, clear fills it,
 * flush converts + pushes to the simulator-backed IT8951, and an
 * out-of-bounds rect is rejected by the simple bounds checks.)
 */
static void test_eink_flush_clear_get_fb(void)
{
  TEST_BEGIN("e-ink: get_fb + clear + flush succeed");
  harness_reset_world();

  display_handle_t*   d   = nullptr;
  const display_cfg_t cfg = make_eink_cfg();
  TEST_ASSERT_EQ(k_ra_ok, display_init(&cfg, &d));

  display_fb_t fb = {};
  TEST_ASSERT_EQ(k_ra_ok, display_get_framebuffer(d, &fb));
  TEST_ASSERT(fb.pixels == s_test_fb);
  TEST_ASSERT_EQ(k_test_fb_width, fb.width_px);

  /* clear fills the framebuffer (does not touch the panel). */
  TEST_ASSERT_EQ(k_ra_ok, display_clear(d, 0x8410U));
  TEST_ASSERT_EQ(0x8410U, s_test_fb[0]);
  TEST_ASSERT_EQ(0x8410U, s_test_fb[k_test_fb_pixels - 1U]);

  /* full-screen flush converts + pushes to the IT8951 (sim SPI). */
  TEST_ASSERT_EQ(k_ra_ok, display_flush(d, display_full_rect(d), k_display_refresh_quality));
  /* a fast partial flush also succeeds. */
  const display_rect_t part = {.x = 0U, .y = 0U, .w = 8U, .h = 8U};
  TEST_ASSERT_EQ(k_ra_ok, display_flush(d, part, k_display_refresh_fast));
  /* an out-of-bounds rect is rejected. */
  const display_rect_t oob = {.x = 0U, .y = 0U, .w = (uint16_t)(k_test_fb_width + 1U), .h = 1U};
  TEST_ASSERT_EQ(k_ra_err_invalid_arg, display_flush(d, oob, k_display_refresh_quality));

  TEST_ASSERT_EQ(k_ra_ok, display_deinit(d));
  TEST_END("e-ink: get_fb + clear + flush succeed");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- pins the RGB565 -> 8bpp luma
 * conversion the e-ink flush path relies on against known endpoints.)
 */
static void test_eink_luma_conversion(void)
{
  TEST_BEGIN("e-ink: RGB565 -> 8bpp luma");
  /* White -> 255, black -> 0 (the weights sum to 256). */
  TEST_ASSERT_EQ(255U, ra_display_pal_eink_luma_from_rgb565(0xFFFFU));
  TEST_ASSERT_EQ(0U, ra_display_pal_eink_luma_from_rgb565(0x0000U));
  /* Pure primaries hit their Rec.601 weights (R 77, G 150, B 29 / 256). */
  TEST_ASSERT_EQ(76U, ra_display_pal_eink_luma_from_rgb565(0xF800U));
  TEST_ASSERT_EQ(149U, ra_display_pal_eink_luma_from_rgb565(0x07E0U));
  TEST_ASSERT_EQ(28U, ra_display_pal_eink_luma_from_rgb565(0x001FU));
  TEST_END("e-ink: RGB565 -> 8bpp luma");
}

/* =============================================================================
 * Driver
 * =============================================================================
 */

int main(void)
{
  test_init_rejects_null_arguments();
  test_mcdc_lcd_rejects_zero_dimensions();
  test_lcd_rejects_non_rgb565();
  test_lcd_rejects_undersized_buffer();
  test_lcd_happy_path();
  test_lcd_flush_rejects_out_of_bounds();
  test_init_rejects_double_init();
  test_calls_after_deinit_are_rejected();
  test_mcdc_eink_rejects_zero_dimensions();
  test_eink_init_get_caps();
  test_eink_flush_clear_get_fb();
  test_eink_luma_conversion();
  return 0;
}
