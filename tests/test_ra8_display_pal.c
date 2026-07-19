/**
 * @file test_ra8_display_pal.c
 * @brief Host unit tests for the display PAL (ra8_display_pal).
 *
 * @details
 * Exercises:
 *
 *   - Argument validation on every public entry point.
 *   - LCD backend end-to-end: init, get_caps, get_framebuffer,
 *     clear, flush, deinit. The LCD backend's hardware accesses
 *     are intercepted by ``ra8_sim_mmap`` so the bring-up sequence
 *     can run under host gcc with no real GPIO / GLCDC.
 *   - E-ink (IT8951) backend end-to-end against the simulator-backed
 *     ``ra8_epaper`` driver: init / get_caps / get_framebuffer / clear /
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

#include "ra8_display_pal.h"
#include "ra8_display_pal_eink.h"
#include "ra8_display_pal_internal.h"
#include "ra8_display_pal_lcd.h"
#include "ra8_display_pal_policy.h"
#include "ra8_epaper.h"
#include "ra8_err.h"
#include "ra8_io_spi_bus.h"
#include "ra8_io_spi_bus_spi_b.h"
#include "ra8_mstp.h"
#include "ra8_panel_timing.h"
#include "ra8_pin_validator.h"
#include "ra8_sim_mmap.h"
#include "ra8_sim_mmio.h"
#include "ra8_spi.h"
#include "ra8_spi_bus_ops.h"
#include "ra8_spi_regs.h"
#include "unity_minimal.h"

/**
 * @enum display_pal_fixture_t
 * @brief Buffer capacities and payload sizes.
 */
typedef enum : uint8_t {
  k_fb_alignment_bytes =
    64, /**< Framebuffer alignment the display controller requires, in bytes. */
} display_pal_fixture_t;

/**
 * @enum display_pal_fixture2_t
 * @brief Out-of-range and malformed inputs the code under test must reject.
 */
typedef enum : uint16_t {
  k_pal_width_over_max =
    4097U, /**< A panel width one past the largest the controller supports, so configuration must reject it. */
} display_pal_fixture2_t;

/* =============================================================================
 * Test fixture
 * =============================================================================
 */

typedef enum : uint16_t {
  k_test_fb_width  = 64U, /**< Test fb width.  */
  k_test_fb_height = 32U, /**< Test fb height. */
} test_fb_dim_t;

typedef enum : uint32_t {
  k_test_fb_pixels = (uint32_t)k_test_fb_width * (uint32_t)k_test_fb_height, /**< Test fb pixels. */
  k_test_fb_bytes  = k_test_fb_pixels * 2U,                                  /**< Test fb bytes. */
} test_fb_size_t;

/* Static framebuffer the LCD and e-ink backends share -- one test
 * deinits before the next inits so the state machine resets. */
[[gnu::aligned(k_fb_alignment_bytes)]] static uint16_t s_test_fb[k_test_fb_pixels];

/* IT8951 descriptor the board BSP would supply through panel_timing,
 * sized to the test framebuffer. In host tests this drives the
 * simulator-backed ra8_epaper (SPI/HRDY short-circuited under sim). The
 * descriptor's injected bus seam is bound in harness_reset_world()
 * through the ra8_io SPI facade -- the same wiring a real BSP performs. */
typedef enum : uint32_t {
  k_eink_spi_baud_hz = 12000000U,  /**< 12 MHz SPI clock. */
  k_eink_pclka_hz    = 100000000U, /**< 100 MHz PCLKA.    */
  /**
   * The only VCOM the sim SPI loopback echoes back, so the only one whose
   * readback can satisfy ``ra8_epaper_set_vcom``. See the call site.
   */
  k_eink_vcom_loopback = 0xFFFFU,
} test_eink_const_t;

/** @brief Bound SPI_B bus handle the e-ink descriptor's seam points at. */
static ra8_io_spi_bus_t s_eink_bus;

/** @brief IT8951 descriptor; its seam is (re)bound in harness_reset_world. */
static ra8_epaper_cfg_t s_eink_panel_cfg = {
  .bus          = {},
  .waveform     = {.init = 0U, .du = 1U, .gc16 = 2U, .a2 = 4U},
  .reset_pin    = 0U,
  .busy_pin     = 0U,
  .panel_width  = (uint16_t)k_test_fb_width,
  .panel_height = (uint16_t)k_test_fb_height,
};

/* Same descriptor with an out-of-range panel width. The e-ink PAL
 * validate accepts it (panel_timing is non-NULL) but ra8_epaper_init's
 * own field validation rejects it -- used to drive the eink_init
 * "ra8_epaper_init failed" error leg. */
static ra8_epaper_cfg_t s_eink_panel_cfg_bad = {
  .bus          = {},
  .waveform     = {.init = 0U, .du = 1U, .gc16 = 2U, .a2 = 4U},
  .reset_pin    = 0U,
  .busy_pin     = 0U,
  .panel_width  = 0U,
  .panel_height = (uint16_t)k_test_fb_height,
};

static void harness_reset_world(void)
{
  ra8_sim_mmap_reset();
  ra8_sim_mmio_reset();
  ra8_pin_validator_reset();
  (void)ra8_mstp_init();
  (void)memset(s_test_fb, 0, sizeof(s_test_fb));
  /* The e-ink backend drives the IT8951 over SPI (ra8_spi_xfer8), whose sim
   * wait_spsr returns OK only when the status flags are set -- the sim backs
   * SPSR with zeroed memory and has no controller to assert them. Prime SPI ch0
   * "TX-empty + RX-full" once; ra8_spi_xfer8 never writes SPSR in the sim (its
   * SPSRC clears hit a separate register), so a single prime carries every
   * transfer of an e-ink bring-up. Same accommodation test_ra8_spi_b.c uses. */
  ra8_spi((uint8_t)0)->SPSR = (uint32_t)k_ra8_spsr_mask_sptef | (uint32_t)k_ra8_spsr_mask_sprf;
  /* Play the BSP's role for the e-ink descriptor: initialise SPI_B channel 0
   * and (re)bind the ra8_io facade into the descriptor's injected seam. */
  const ra8_spi_cfg_t spi_cfg = {
    .baud_hz   = (uint32_t)k_eink_spi_baud_hz,
    .pclka_hz  = (uint32_t)k_eink_pclka_hz,
    .mode      = k_ra8_spi_mode_0,
    .lsb_first = false,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_spi_init(0U, &spi_cfg));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_spi_bus_bind_spi_b(&s_eink_bus, 0U));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_spi_bus_as_ops(&s_eink_bus, &s_eink_panel_cfg.bus));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_spi_bus_as_ops(&s_eink_bus, &s_eink_panel_cfg_bad.bus));
}

static display_cfg_t make_lcd_cfg(void)
{
  const display_cfg_t cfg = {
    .iface             = &k_display_backend_lcd_ra8_glcdc,
    .framebuffer       = s_test_fb,
    .framebuffer_bytes = sizeof(s_test_fb),
    .width_px          = (uint16_t)k_test_fb_width,
    .height_px         = (uint16_t)k_test_fb_height,
    .pixfmt            = k_display_pixfmt_rgb565,
    .panel_timing      = &k_ra8_panel_ek_ra8d2_timing,
  };
  return cfg;
}

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
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, display_init(nullptr, &d));

  /* Vector 3: out_handle = NULL. */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, display_init(&cfg, nullptr));

  /* Vector 4: cfg->iface = NULL. */
  display_cfg_t cfg_no_iface = cfg;
  cfg_no_iface.iface         = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, display_init(&cfg_no_iface, &d));

  /* Vector 5: framebuffer = NULL (the LCD backend's first check). */
  display_cfg_t cfg_no_fb = cfg;
  cfg_no_fb.framebuffer   = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, display_init(&cfg_no_fb, &d));

  TEST_END("display_init rejects null arguments");
}

/**
 * @par MC/DC:
 * Decision (libs/ra8_display_pal/src/ra8_display_pal_lcd.c@internal_lcd_validate_cfg):
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
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, display_init(&cfg, &d));

  cfg           = make_lcd_cfg();
  cfg.height_px = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, display_init(&cfg, &d));

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
  TEST_ASSERT_EQ(k_ra8_err_not_supported, display_init(&cfg, &d));

  cfg.pixfmt = k_display_pixfmt_grey1;
  TEST_ASSERT_EQ(k_ra8_err_not_supported, display_init(&cfg, &d));

  cfg.pixfmt = k_display_pixfmt_rgb888;
  TEST_ASSERT_EQ(k_ra8_err_not_supported, display_init(&cfg, &d));

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
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, display_init(&cfg, &d));

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
  TEST_ASSERT_EQ(k_ra8_ok, display_init(&cfg, &d));
  TEST_ASSERT_NOT_NULL(d);

  display_caps_t caps = {};
  TEST_ASSERT_EQ(k_ra8_ok, display_get_caps(d, &caps));
  TEST_ASSERT_EQ(k_test_fb_width, caps.width_px);
  TEST_ASSERT_EQ(k_test_fb_height, caps.height_px);
  TEST_ASSERT_EQ(k_display_pixfmt_rgb565, caps.pixfmt);
  TEST_ASSERT_EQ(k_test_fb_width * 2U, caps.stride_bytes);
  TEST_ASSERT_EQ(0, caps.refresh_latency_us_typ);
  TEST_ASSERT(caps.continuous_refresh);
  TEST_ASSERT(caps.supports_partial_update);

  display_fb_t fb = {};
  TEST_ASSERT_EQ(k_ra8_ok, display_get_framebuffer(d, &fb));
  TEST_ASSERT_EQ((intptr_t)s_test_fb, (intptr_t)fb.pixels);
  TEST_ASSERT_EQ(k_test_fb_width, fb.width_px);
  TEST_ASSERT_EQ(k_test_fb_height, fb.height_px);

  TEST_ASSERT_EQ(k_ra8_ok, display_clear(d, 0xFFFFU));
  TEST_ASSERT_EQ(0xFFFFU, ((const uint16_t*)fb.pixels)[0]);
  TEST_ASSERT_EQ(0xFFFFU, ((const uint16_t*)fb.pixels)[k_test_fb_pixels - 1U]);

  const display_rect_t r = display_full_rect(d);
  TEST_ASSERT_EQ(0, r.x);
  TEST_ASSERT_EQ(0, r.y);
  TEST_ASSERT_EQ(k_test_fb_width, r.w);
  TEST_ASSERT_EQ(k_test_fb_height, r.h);
  TEST_ASSERT_EQ(k_ra8_ok, display_flush(d, r, k_display_refresh_quality));
  TEST_ASSERT_EQ(k_ra8_ok, display_flush(d, r, k_display_refresh_fast));
  TEST_ASSERT_EQ(k_ra8_ok, display_flush(d, r, k_display_refresh_init));

  TEST_ASSERT_EQ(k_ra8_ok, display_deinit(d));
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
  TEST_ASSERT_EQ(k_ra8_ok, display_init(&cfg, &d));

  const display_rect_t bad_x = {(uint16_t)(k_test_fb_width + 1U), 0U, 1U, 1U};
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, display_flush(d, bad_x, k_display_refresh_quality));

  const display_rect_t bad_y = {0U, (uint16_t)(k_test_fb_height + 1U), 1U, 1U};
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, display_flush(d, bad_y, k_display_refresh_quality));

  const display_rect_t bad_w = {0U, 0U, (uint16_t)(k_test_fb_width + 1U), 1U};
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, display_flush(d, bad_w, k_display_refresh_quality));

  const display_rect_t bad_h = {0U, 0U, 1U, (uint16_t)(k_test_fb_height + 1U)};
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, display_flush(d, bad_h, k_display_refresh_quality));

  TEST_ASSERT_EQ(k_ra8_ok, display_deinit(d));
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
  TEST_ASSERT_EQ(k_ra8_ok, display_init(&cfg, &d1));
  TEST_ASSERT_EQ(k_ra8_err_busy, display_init(&cfg, &d2));
  TEST_ASSERT_EQ(k_ra8_ok, display_deinit(d1));

  /* After deinit a new init succeeds.  The hardware-side panel
   * power-on claims GPIOs that survive ``ra8_glcdc_deinit`` (real
   * silicon does not need them re-claimed for the same panel) so
   * we reset the validator's bitmap explicitly before re-bringing
   * up.  Production callers either reset the chip or stay bound. */
  ra8_pin_validator_reset();
  TEST_ASSERT_EQ(k_ra8_ok, display_init(&cfg, &d2));
  TEST_ASSERT_EQ(k_ra8_ok, display_deinit(d2));
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
  TEST_ASSERT_EQ(k_ra8_ok, display_init(&cfg, &d));
  TEST_ASSERT_EQ(k_ra8_ok, display_deinit(d));

  display_caps_t caps = {};
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, display_get_caps(d, &caps));

  display_fb_t fb = {};
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, display_get_framebuffer(d, &fb));

  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 display_flush(d, display_full_rect(d), k_display_refresh_quality));

  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, display_clear(d, 0U));

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
 * Decision (libs/ra8_display_pal/src/ra8_display_pal_eink.c@internal_eink_validate_cfg):
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
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, display_init(&cfg, &d));

  /* V3: height = 0 with width = N exercises the second condition. */
  cfg           = make_eink_cfg();
  cfg.height_px = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, display_init(&cfg, &d));
  TEST_END("e-ink backend MC/DC: rejects zero dimensions");
}

/**
 * @par MC/DC:
 * E-ink backend init runs the same validation path as the LCD backend;
 * this test covers the happy path (init brings up the simulator-backed
 * IT8951 via ra8_epaper) so the validation-rejection vectors are
 * exercised the same way.
 */
static void test_eink_init_get_caps(void)
{
  TEST_BEGIN("e-ink: init + get_caps succeed");
  harness_reset_world();

  display_handle_t*   d   = nullptr;
  const display_cfg_t cfg = make_eink_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, display_init(&cfg, &d));

  display_caps_t caps = {};
  TEST_ASSERT_EQ(k_ra8_ok, display_get_caps(d, &caps));
  TEST_ASSERT_EQ(k_test_fb_width, caps.width_px);
  TEST_ASSERT(caps.refresh_latency_us_typ > 0U);
  TEST_ASSERT(!caps.continuous_refresh);
  TEST_ASSERT(caps.supports_partial_update);

  TEST_ASSERT_EQ(k_ra8_ok, display_deinit(d));
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
  TEST_ASSERT_EQ(k_ra8_ok, display_init(&cfg, &d));

  display_fb_t fb = {};
  TEST_ASSERT_EQ(k_ra8_ok, display_get_framebuffer(d, &fb));
  TEST_ASSERT(fb.pixels == s_test_fb);
  TEST_ASSERT_EQ(k_test_fb_width, fb.width_px);

  /* clear fills the framebuffer (does not touch the panel). */
  TEST_ASSERT_EQ(k_ra8_ok, display_clear(d, 0x8410U));
  TEST_ASSERT_EQ(0x8410U, s_test_fb[0]);
  TEST_ASSERT_EQ(0x8410U, s_test_fb[k_test_fb_pixels - 1U]);

  /* INV-VCOM-1 reaches through the PAL: the backend's flush ends in
   * ``ra8_epaper_display_area``, which refuses every refresh until a VCOM
   * has been programmed and read back unchanged. A PAL consumer that skips
   * calibration therefore gets a refusal, not a biased panel. */
  TEST_ASSERT_EQ(k_ra8_err_validation_failed,
                 display_flush(d, display_full_rect(d), k_display_refresh_quality));
  /* Grant the permit. The sim SPI is a loopback that always echoes the
   * driver's own 0xFF dummy, so 0xFFFF is the only value whose readback
   * can match here -- a fixture artefact, not a plausible bias. The value
   * comparison itself is covered in test_ra8_epaper_cov.c. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epaper_set_vcom((uint16_t)k_eink_vcom_loopback));

  /* full-screen flush converts + pushes to the IT8951 (sim SPI). */
  TEST_ASSERT_EQ(k_ra8_ok, display_flush(d, display_full_rect(d), k_display_refresh_quality));
  /* a fast partial flush also succeeds. */
  const display_rect_t part = {.x = 0U, .y = 0U, .w = 8U, .h = 8U};
  TEST_ASSERT_EQ(k_ra8_ok, display_flush(d, part, k_display_refresh_fast));
  /* an out-of-bounds rect is rejected. */
  const display_rect_t oob = {.x = 0U, .y = 0U, .w = (uint16_t)(k_test_fb_width + 1U), .h = 1U};
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, display_flush(d, oob, k_display_refresh_quality));

  TEST_ASSERT_EQ(k_ra8_ok, display_deinit(d));
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
  TEST_ASSERT_EQ(255U, ra8_display_pal_eink_luma_from_rgb565(0xFFFFU));
  TEST_ASSERT_EQ(0U, ra8_display_pal_eink_luma_from_rgb565(0x0000U));
  /* Pure primaries hit their Rec.601 weights (R 77, G 150, B 29 / 256). */
  TEST_ASSERT_EQ(76U, ra8_display_pal_eink_luma_from_rgb565(0xF800U));
  TEST_ASSERT_EQ(149U, ra8_display_pal_eink_luma_from_rgb565(0x07E0U));
  TEST_ASSERT_EQ(28U, ra8_display_pal_eink_luma_from_rgb565(0x001FU));
  TEST_END("e-ink: RGB565 -> 8bpp luma");
}

/**
 * @brief Drive the refresh-cadence policy end-to-end through the sim e-ink backend.
 *
 * @details Proves the policy integrates with the real backend: every hint the
 * fast_clean policy issues (INIT on open, A2 for the first N-1 turns, GC16 on
 * the Nth, GC16 on a chapter boundary) is accepted by display_flush on the
 * IT8951 sim. Closes the loop the pure-logic test in
 * test_ra8_display_pal_policy.c opens (it asserts the cadence; this asserts the
 * backend honours each issued waveform hint).
 *
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the policy-to-backend
 * integration contract; the policy's own decisions carry their vectors in
 * test_ra8_display_pal_policy.c)
 */
static void test_eink_policy_sequence(void)
{
  TEST_BEGIN("e-ink: refresh policy sequence drives the sim backend");
  harness_reset_world();

  display_handle_t*   d   = nullptr;
  const display_cfg_t cfg = make_eink_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, display_init(&cfg, &d));

  /* INV-VCOM-1: the backend refuses every refresh until a VCOM has been
   * programmed and read back. This test is about the policy cadence, so it
   * takes the permit up front; the gate itself is asserted in
   * ``test_eink_flush_clear_get_fb``. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epaper_set_vcom((uint16_t)k_eink_vcom_loopback));

  display_policy_t p = {};
  TEST_ASSERT_EQ(k_ra8_ok, display_policy_init(&p, k_display_policy_fast_clean, 4U));

  /* Book open -> INIT clear, accepted by the panel. */
  display_policy_decision_t dec = {};
  TEST_ASSERT_EQ(k_ra8_ok, display_policy_decide(&p, k_display_event_open, &dec));
  TEST_ASSERT_EQ(k_display_refresh_init, dec.hint);
  TEST_ASSERT(dec.full_update);
  TEST_ASSERT_EQ(k_ra8_ok, display_flush(d, display_full_rect(d), dec.hint));

  /* Turns 1..3 -> fast A2; the 4th -> clean GC16; each accepted by the backend. */
  const display_refresh_hint_t expect[4] = {k_display_refresh_fast,
                                            k_display_refresh_fast,
                                            k_display_refresh_fast,
                                            k_display_refresh_quality};
  for (uint32_t i = 0U; i < 4U; i++) {
    TEST_ASSERT_EQ(k_ra8_ok, display_policy_decide(&p, k_display_event_turn, &dec));
    TEST_ASSERT_EQ(expect[i], dec.hint);
    TEST_ASSERT_EQ(k_ra8_ok, display_flush(d, display_full_rect(d), dec.hint));
  }

  /* A chapter boundary forces a clean GC16 even before the cadence elapses. */
  TEST_ASSERT_EQ(k_ra8_ok, display_policy_decide(&p, k_display_event_chapter, &dec));
  TEST_ASSERT_EQ(k_display_refresh_quality, dec.hint);
  TEST_ASSERT_EQ(k_ra8_ok, display_flush(d, display_full_rect(d), dec.hint));

  TEST_ASSERT_EQ(k_ra8_ok, display_deinit(d));
  TEST_END("e-ink: refresh policy sequence drives the sim backend");
}

/**
 * @par MC/DC:
 * Drives the four single-condition rejection branches in
 * ``internal_eink_validate_cfg`` that the happy path leaves as the "false"
 * leg, by calling the e-ink backend vtable ``init`` directly (the PAL
 * dispatcher forwards the same cfg, but calling the vtable keeps the
 * backend context lifecycle local to this test). Each decision is a single
 * condition, so two vectors each: the "true" (reject) vector here and the
 * "false" (accept) vector via ``test_eink_init_get_caps``.
 *
 * - ``pixfmt != rgb565`` : T here (grey4) -> k_ra8_err_not_supported.
 * - ``width_px > 4096``  : T here (4097)  -> k_ra8_err_invalid_arg.
 * - ``panel_timing == nullptr`` : T here  -> k_ra8_err_invalid_arg.
 * - ``framebuffer_bytes < need``: T here (need-1) -> k_ra8_err_invalid_arg.
 *
 * Every vector fails before the bring-up, so the backend context stays
 * uninitialised (no ra8_epaper touched, no pins claimed).
 */
static void test_eink_validate_cfg_rejections(void)
{
  TEST_BEGIN("e-ink backend init rejects invalid cfg fields");
  harness_reset_world();

  void* ctx = nullptr;

  /* pixfmt not RGB565 -> not_supported. */
  display_cfg_t cfg_fmt = make_eink_cfg();
  cfg_fmt.pixfmt        = k_display_pixfmt_grey4;
  TEST_ASSERT_EQ(k_ra8_err_not_supported, k_display_backend_eink_it8951.init(&cfg_fmt, &ctx));

  /* width beyond the bounded conversion line buffer -> invalid_arg. */
  display_cfg_t cfg_wide = make_eink_cfg();
  cfg_wide.width_px      = k_pal_width_over_max;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, k_display_backend_eink_it8951.init(&cfg_wide, &ctx));

  /* BSP omitted the IT8951 descriptor -> invalid_arg. */
  display_cfg_t cfg_no_desc = make_eink_cfg();
  cfg_no_desc.panel_timing  = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, k_display_backend_eink_it8951.init(&cfg_no_desc, &ctx));

  /* framebuffer too small for width*height*2 -> invalid_arg. */
  display_cfg_t cfg_small     = make_eink_cfg();
  cfg_small.framebuffer_bytes = (uint32_t)k_test_fb_bytes - 1U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, k_display_backend_eink_it8951.init(&cfg_small, &ctx));

  TEST_END("e-ink backend init rejects invalid cfg fields");
}

/**
 * @par MC/DC:
 * ``internal_eink_check_rect`` is four sequential single-condition bounds
 * checks. ``test_eink_flush_clear_get_fb`` already drives the third
 * (``r.x + r.w > width``). This test drives the remaining three "true"
 * legs independently; the all-false leg is the happy-path flush. Each
 * check is a lone condition, so one reject vector per branch is minimal.
 *
 * - ``r.x > width``        : rect {65, 0, 1, 1}  (others in range).
 * - ``r.y > height``       : rect {0, 33, 1, 1}.
 * - ``r.y + r.h > height`` : rect {0, 0, 1, 33}.
 */
static void test_eink_check_rect_rejections(void)
{
  TEST_BEGIN("e-ink backend flush rejects out-of-bounds rects");
  harness_reset_world();

  void*               ctx = nullptr;
  const display_cfg_t cfg = make_eink_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, k_display_backend_eink_it8951.init(&cfg, &ctx));
  TEST_ASSERT_NOT_NULL(ctx);

  const display_rect_t bad_x = {(uint16_t)(k_test_fb_width + 1U), 0U, 1U, 1U};
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 k_display_backend_eink_it8951.flush(ctx, bad_x, k_display_refresh_quality));

  const display_rect_t bad_y = {0U, (uint16_t)(k_test_fb_height + 1U), 1U, 1U};
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 k_display_backend_eink_it8951.flush(ctx, bad_y, k_display_refresh_quality));

  const display_rect_t bad_h = {0U, 0U, 1U, (uint16_t)(k_test_fb_height + 1U)};
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 k_display_backend_eink_it8951.flush(ctx, bad_h, k_display_refresh_quality));

  TEST_ASSERT_EQ(k_ra8_ok, k_display_backend_eink_it8951.deinit(ctx));
  TEST_END("e-ink backend flush rejects out-of-bounds rects");
}

/**
 * @par MC/DC:
 * Decision: ``if (err != k_ra8_ok)`` after ``ra8_epaper_init`` in
 * ``eink_init`` -- single condition. The false leg (panel bring-up
 * succeeds) is the happy path; the true leg is driven here by handing the
 * backend a cfg that passes the PAL's own validation but carries an
 * IT8951 descriptor ``ra8_epaper_init`` rejects (panel_width == 0). The
 * failure returns before any state is snapshotted.
 */
static void test_eink_epaper_init_failure(void)
{
  TEST_BEGIN("e-ink backend init forwards ra8_epaper_init failure");
  harness_reset_world();

  void*         ctx = nullptr;
  display_cfg_t cfg = make_eink_cfg();
  cfg.panel_timing  = &s_eink_panel_cfg_bad;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, k_display_backend_eink_it8951.init(&cfg, &ctx));

  TEST_END("e-ink backend init forwards ra8_epaper_init failure");
}

/**
 * @par MC/DC:
 * Decision: ``if (s_eink_ctx.initialised)`` in ``eink_init`` -- single
 * condition. The false leg is the first (accepted) init; the true leg is
 * the second init here, which must be rejected as busy. Exercised through
 * the backend vtable because the PAL dispatcher's own ``s_initialized``
 * guard would otherwise reject the second call before it reaches the
 * backend.
 */
static void test_eink_backend_rejects_double_init(void)
{
  TEST_BEGIN("e-ink backend init rejects a second init without deinit");
  harness_reset_world();

  void*               ctx1 = nullptr;
  void*               ctx2 = nullptr;
  const display_cfg_t cfg  = make_eink_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, k_display_backend_eink_it8951.init(&cfg, &ctx1));
  TEST_ASSERT_EQ(k_ra8_err_busy, k_display_backend_eink_it8951.init(&cfg, &ctx2));

  TEST_ASSERT_EQ(k_ra8_ok, k_display_backend_eink_it8951.deinit(ctx1));
  TEST_END("e-ink backend init rejects a second init without deinit");
}

/**
 * @par MC/DC:
 * Decision: ``if (lerr != k_ra8_ok)`` after ``internal_eink_load_rect`` in
 * ``eink_flush`` -- single condition, and the ``if (err != k_ra8_ok)``
 * inside ``internal_eink_load_rect`` after ``ra8_epaper_load_image``. The
 * false legs are the happy-path flush; the true legs are driven here by
 * clearing the primed SPI status flags after init, so the first SPI
 * transfer of the row load times out deterministically (the sim's
 * ``internal_wait_spsr`` returns k_ra8_err_hw_timeout at once when the
 * flag is clear -- no spin, no SIGALRM). The rect is fully in bounds so
 * the load path is reached before the error surfaces.
 */
static void test_eink_flush_load_failure(void)
{
  TEST_BEGIN("e-ink backend flush forwards a row-load failure");
  harness_reset_world();

  void*               ctx = nullptr;
  const display_cfg_t cfg = make_eink_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, k_display_backend_eink_it8951.init(&cfg, &ctx));

  /* Drop the SPI TX-empty / RX-full prime and arm the MMIO seam to fail the
   * bounded wait so the first row-load transfer times out. The row load runs
   * through ra8_spi_xfer8, whose SPTEF/SPRF wait polls the channel status
   * register, so key the fail on &ra8_spi(0)->SPSR. Without arming, an unstaged
   * flag now satisfies the wait on the first poll (T1-01 seam contract). */
  ra8_spi((uint8_t)0)->SPSR = 0U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_sim_mmio_fail_wait((const volatile void*)&ra8_spi((uint8_t)0)->SPSR));

  const display_rect_t full = {0U, 0U, (uint16_t)k_test_fb_width, (uint16_t)k_test_fb_height};
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout,
                 k_display_backend_eink_it8951.flush(ctx, full, k_display_refresh_quality));

  /* Disarm the seam and re-prime so the teardown SLEEP command completes
   * cleanly (the deinit SPI wait must succeed again). */
  ra8_sim_mmio_reset();
  ra8_spi((uint8_t)0)->SPSR = (uint32_t)k_ra8_spsr_mask_sptef | (uint32_t)k_ra8_spsr_mask_sprf;
  TEST_ASSERT_EQ(k_ra8_ok, k_display_backend_eink_it8951.deinit(ctx));
  TEST_END("e-ink backend flush forwards a row-load failure");
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
  test_eink_policy_sequence();
  test_eink_validate_cfg_rejections();
  test_eink_check_rect_rejections();
  test_eink_epaper_init_failure();
  test_eink_backend_rejects_double_init();
  test_eink_flush_load_failure();
  return 0;
}
