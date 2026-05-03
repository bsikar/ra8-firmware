/**
 * @file test_ra_vin.c
 * @brief Unit tests for ra_vin.c (Video Input Module driver)
 *
 * @details
 * Tests run on the host build and use the `ra_sim_mmap` shim so the
 * driver's MMIO accesses land in plain RAM. Each test resets the
 * mmap and (where needed) ra_mstp before exercising the API.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8d2_vin_regs.h"
#include "ra_err.h"
#include "ra_mstp.h"
#include "ra_sim_mmap.h"
#include "ra_vin.h"
#include "unity_minimal.h"

/**
 * @enum ra_vin_test_const_t
 * @brief Magic-free constants for the test cases.
 *
 * @details
 * Centralises the framebuffer addresses and image-stride values used
 * across multiple cases so a single edit moves them all.
 */
typedef enum : uint32_t {
  k_ra_vin_test_fb1     = 0x68000000UL, /**< First MB1 (SDRAM head).         */
  k_ra_vin_test_fb2     = 0x68040000UL, /**< MB2 for continuous mode.        */
  k_ra_vin_test_fb3     = 0x68080000UL, /**< MB3 for continuous mode.        */
  k_ra_vin_test_fb_alt  = 0x680C0000UL, /**< Alternate MB1 base.             */
  k_ra_vin_test_uv_off  = 0x68100000UL, /**< UV plane offset (aligned).      */
  k_ra_vin_test_uv_bad  = 0x68100001UL, /**< UV plane offset (mis-aligned).  */
  k_ra_vin_test_stride  = 1280U,        /**< Pixels per scanline.            */
  k_ra_vin_test_ie_mask = 0x00000022UL, /**< IE: enable EFE + FME.           */
  k_ra_vin_test_ctx     = 0x12345678UL, /**< Sentinel ctx for callback test. */
  k_ra_vin_test_status  = 0x00000080UL, /**< Arbitrary non-W1C INTS bit.     */
  k_ra_vin_test_si_val  = 600U,         /**< Scanline compare line.          */
} ra_vin_test_const_t;

/**
 * @enum ra_vin_test_field_t
 * @brief Sample field values for the field-detect tests.
 */
typedef enum : uint8_t {
  k_ra_vin_test_field_even_one = 1U,
} ra_vin_test_field_t;

/* ----------------------------------------------------------------------------
 * Helpers
 * --------------------------------------------------------------------------*/

static uint32_t s_vin_cb_count;
static uint32_t s_vin_cb_last_mask;
static void*    s_vin_cb_last_ctx;

/**
 * @brief Reset the host harness and zero shared callback counters.
 */
static void prep(void)
{
  ra_sim_mmap_reset();
  (void)ra_mstp_init();
  s_vin_cb_count     = 0U;
  s_vin_cb_last_mask = 0U;
  s_vin_cb_last_ctx  = nullptr;
}

/**
 * @brief Build a known-good config with two MB buffers populated.
 */
static ra_vin_config_t make_cfg(void)
{
  const ra_vin_config_t cfg = {
    .input_fmt          = k_ra_vin_input_rgb888,
    .bypass_csc         = true,
    .big_endian         = false,
    .interlace_mode     = (uint8_t)k_ra_vin_im_odd_even,
    .pixel_clip_mode    = (uint8_t)k_ra_vin_clp_default,
    .image_stride_px    = (uint16_t)k_ra_vin_test_stride,
    .framebuffer_addr_1 = (uint32_t)k_ra_vin_test_fb1,
    .framebuffer_addr_2 = (uint32_t)k_ra_vin_test_fb2,
    .framebuffer_addr_3 = (uint32_t)k_ra_vin_test_fb3,
    .interrupt_enable   = (uint32_t)k_ra_vin_test_ie_mask,
    .scanline_compare   = (uint16_t)k_ra_vin_test_si_val,
  };
  return cfg;
}

/**
 * @brief Test callback used by the dispatch test.
 */
static void stub_vin_cb(void* ctx, uint32_t mask)
{
  ++s_vin_cb_count;
  s_vin_cb_last_mask = mask;
  s_vin_cb_last_ctx  = ctx;
}

/* ----------------------------------------------------------------------------
 * Lifecycle tests
 * --------------------------------------------------------------------------*/

static void test_init_happy(void)
{
  TEST_BEGIN("vin init happy");
  prep();

  const ra_vin_config_t cfg = make_cfg();
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_vin_init(&cfg));

  /* MC has BPS = 1, INF = RGB888 (6 << 16 = 0x00060000), IM = 1 << 3 = 0x08. */
  const uint32_t mc = *ra_vin_reg32(k_ra_vin_off_mc);
  TEST_ASSERT_EQ((int)0U, (int)(mc & (uint32_t)k_ra_vin_mc_me));
  TEST_ASSERT((mc & (uint32_t)k_ra_vin_mc_bps) != 0U);
  TEST_ASSERT_EQ((int)((uint32_t)k_ra_vin_input_rgb888 << 16),
                 (int)(mc & (uint32_t)k_ra_vin_mc_inf));
  TEST_ASSERT_EQ((int)((uint32_t)k_ra_vin_im_odd_even << 3), (int)(mc & (uint32_t)k_ra_vin_mc_im));

  /* IS / MB1..3 / IE / SI all reflect the config. */
  TEST_ASSERT_EQ((int)k_ra_vin_test_stride, (int)*ra_vin_reg32(k_ra_vin_off_is));
  TEST_ASSERT_EQ((int)k_ra_vin_test_fb1, (int)*ra_vin_reg32(k_ra_vin_off_mb1));
  TEST_ASSERT_EQ((int)k_ra_vin_test_fb2, (int)*ra_vin_reg32(k_ra_vin_off_mb2));
  TEST_ASSERT_EQ((int)k_ra_vin_test_fb3, (int)*ra_vin_reg32(k_ra_vin_off_mb3));
  TEST_ASSERT_EQ((int)k_ra_vin_test_ie_mask, (int)*ra_vin_reg32(k_ra_vin_off_ie));
  TEST_ASSERT_EQ((int)k_ra_vin_test_si_val, (int)*ra_vin_reg32(k_ra_vin_off_si));
  TEST_END("vin init happy");
}

static void test_init_null_cfg(void)
{
  TEST_BEGIN("vin init null cfg");
  prep();

  TEST_ASSERT_EQ((int)k_ra_err_null_ptr, (int)ra_vin_init(nullptr));
  TEST_END("vin init null cfg");
}

static void test_init_zero_stride(void)
{
  TEST_BEGIN("vin init zero stride");
  prep();

  ra_vin_config_t cfg = make_cfg();
  cfg.image_stride_px = 0U;
  TEST_ASSERT_EQ((int)k_ra_err_invalid_arg, (int)ra_vin_init(&cfg));
  TEST_END("vin init zero stride");
}

static void test_init_bad_im(void)
{
  TEST_BEGIN("vin init bad interlace mode");
  prep();
  ra_vin_config_t cfg = make_cfg();
  cfg.interlace_mode  = 3U;
  TEST_ASSERT_EQ((int)k_ra_err_invalid_arg, (int)ra_vin_init(&cfg));
  TEST_END("vin init bad interlace mode");
}

static void test_init_bad_clp(void)
{
  TEST_BEGIN("vin init bad clp mode");
  prep();
  ra_vin_config_t cfg = make_cfg();
  cfg.pixel_clip_mode = 4U;
  TEST_ASSERT_EQ((int)k_ra_err_invalid_arg, (int)ra_vin_init(&cfg));
  TEST_END("vin init bad clp mode");
}

static void test_reset(void)
{
  TEST_BEGIN("vin reset");
  prep();
  const ra_vin_config_t cfg = make_cfg();
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_vin_init(&cfg));
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_vin_reset());
  const uint32_t mc = *ra_vin_reg32(k_ra_vin_off_mc);
  TEST_ASSERT_EQ((int)0U, (int)(mc & (uint32_t)k_ra_vin_mc_me));
  TEST_END("vin reset");
}

/* ----------------------------------------------------------------------------
 * Capture tests
 * --------------------------------------------------------------------------*/

static void test_capture_arm_single(void)
{
  TEST_BEGIN("vin capture_arm single");
  prep();

  const ra_vin_config_t cfg = make_cfg();
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_vin_init(&cfg));

  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_vin_capture_arm(k_ra_vin_capture_single));
  const uint32_t mc = *ra_vin_reg32(k_ra_vin_off_mc);
  const uint32_t fc = *ra_vin_reg32(k_ra_vin_off_fc);
  TEST_ASSERT((mc & (uint32_t)k_ra_vin_mc_me) != 0U);
  TEST_ASSERT_EQ((int)0U, (int)(fc & (uint32_t)k_ra_vin_fc_cc));
  TEST_END("vin capture_arm single");
}

static void test_capture_arm_continuous(void)
{
  TEST_BEGIN("vin capture_arm continuous");
  prep();

  const ra_vin_config_t cfg = make_cfg();
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_vin_init(&cfg));

  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_vin_capture_arm(k_ra_vin_capture_continuous));
  const uint32_t mc = *ra_vin_reg32(k_ra_vin_off_mc);
  const uint32_t fc = *ra_vin_reg32(k_ra_vin_off_fc);
  TEST_ASSERT((mc & (uint32_t)k_ra_vin_mc_me) != 0U);
  TEST_ASSERT((fc & (uint32_t)k_ra_vin_fc_cc) != 0U);
  TEST_END("vin capture_arm continuous");
}

static void test_capture_arm_field_skip(void)
{
  TEST_BEGIN("vin capture_arm field-skip");
  prep();
  const ra_vin_config_t cfg = make_cfg();
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_vin_init(&cfg));
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_vin_capture_arm(k_ra_vin_capture_continuous_field_skip));
  const uint32_t fc = *ra_vin_reg32(k_ra_vin_off_fc);
  TEST_ASSERT((fc & (uint32_t)k_ra_vin_fc_cc) != 0U);
  TEST_END("vin capture_arm field-skip");
}

static void test_capture_arm_invalid_mode(void)
{
  TEST_BEGIN("vin capture_arm invalid mode");
  prep();
  const ra_vin_config_t cfg = make_cfg();
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_vin_init(&cfg));
  TEST_ASSERT_EQ((int)k_ra_err_invalid_arg, (int)ra_vin_capture_arm((ra_vin_capture_mode_t)0xFFU));
  TEST_END("vin capture_arm invalid mode");
}

static void test_capture_arm_already_running(void)
{
  TEST_BEGIN("vin capture_arm already running");
  prep();

  const ra_vin_config_t cfg = make_cfg();
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_vin_init(&cfg));
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_vin_capture_arm(k_ra_vin_capture_single));
  TEST_ASSERT_EQ((int)k_ra_err_invalid_state, (int)ra_vin_capture_arm(k_ra_vin_capture_single));
  TEST_END("vin capture_arm already running");
}

static void test_capture_stop(void)
{
  TEST_BEGIN("vin capture_disarm");
  prep();

  const ra_vin_config_t cfg = make_cfg();
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_vin_init(&cfg));
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_vin_capture_arm(k_ra_vin_capture_continuous));

  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_vin_capture_stop());
  const uint32_t mc      = *ra_vin_reg32(k_ra_vin_off_mc);
  const uint32_t fc      = *ra_vin_reg32(k_ra_vin_off_fc);
  const uint32_t mtcstop = *ra_vin_reg32(k_ra_vin_off_mtcstop);
  TEST_ASSERT_EQ((int)0U, (int)(mc & (uint32_t)k_ra_vin_mc_me));
  TEST_ASSERT_EQ((int)0U, (int)(fc & (uint32_t)k_ra_vin_fc_cc));
  TEST_ASSERT((mtcstop & (uint32_t)k_ra_vin_mtcstop_req) != 0U);
  TEST_END("vin capture_disarm");
}

static void test_capture_disarm_idle(void)
{
  TEST_BEGIN("vin capture_disarm idle");
  prep();

  const ra_vin_config_t cfg = make_cfg();
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_vin_init(&cfg));
  TEST_ASSERT_EQ((int)k_ra_err_invalid_state, (int)ra_vin_capture_stop());
  TEST_END("vin capture_disarm idle");
}

/* ----------------------------------------------------------------------------
 * Pre-clip / scaling tests
 * --------------------------------------------------------------------------*/

static void test_set_preclip(void)
{
  TEST_BEGIN("vin set_preclip");
  prep();
  const ra_vin_config_t cfg = make_cfg();
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_vin_init(&cfg));

  const ra_vin_preclip_t w = {.line_start  = 4U,
                              .line_end    = 595U,
                              .pixel_start = 0U,
                              .pixel_end   = 1023U};
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_vin_set_preclip(&w));
  TEST_ASSERT_EQ((int)4, (int)*ra_vin_reg32(k_ra_vin_off_slprc));
  TEST_ASSERT_EQ((int)595, (int)*ra_vin_reg32(k_ra_vin_off_elprc));
  TEST_ASSERT_EQ((int)0, (int)*ra_vin_reg32(k_ra_vin_off_spprc));
  TEST_ASSERT_EQ((int)1023, (int)*ra_vin_reg32(k_ra_vin_off_epprc));

  TEST_ASSERT_EQ((int)k_ra_err_null_ptr, (int)ra_vin_set_preclip(nullptr));

  const ra_vin_preclip_t bad = {.line_start  = 100U,
                                .line_end    = 50U,
                                .pixel_start = 0U,
                                .pixel_end   = 1023U};
  TEST_ASSERT_EQ((int)k_ra_err_invalid_arg, (int)ra_vin_set_preclip(&bad));

  const ra_vin_preclip_t huge = {.line_start  = 0U,
                                 .line_end    = 5000U,
                                 .pixel_start = 0U,
                                 .pixel_end   = 100U};
  TEST_ASSERT_EQ((int)k_ra_err_invalid_arg, (int)ra_vin_set_preclip(&huge));
  TEST_END("vin set_preclip");
}

static void test_set_uds_scale(void)
{
  TEST_BEGIN("vin set_uds_scale");
  prep();
  const ra_vin_config_t cfg = make_cfg();
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_vin_init(&cfg));

  /* 1.5x V scale, 2.0x H scale: V = 1.500 -> mant 1, frac 0x800; H = 2.0 mant 2. */
  const ra_vin_uds_scale_t s = {
    .v_mantissa = 1U,
    .v_fraction = 0x800U,
    .h_mantissa = 2U,
    .h_fraction = 0U,
  };
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_vin_set_uds_scale(&s));
  const uint32_t v = *ra_vin_reg32(k_ra_vin_off_uds_scale);
  TEST_ASSERT_EQ((int)0x800U, (int)(v & (uint32_t)k_ra_vin_uds_scale_vfrac));
  TEST_ASSERT_EQ((int)(1U << 12), (int)(v & (uint32_t)k_ra_vin_uds_scale_vmant));
  TEST_ASSERT_EQ((int)0U, (int)(v & (uint32_t)k_ra_vin_uds_scale_hfrac));
  TEST_ASSERT_EQ((int)(2U << 28), (int)(v & (uint32_t)k_ra_vin_uds_scale_hmant));

  TEST_ASSERT_EQ((int)k_ra_err_null_ptr, (int)ra_vin_set_uds_scale(nullptr));

  const ra_vin_uds_scale_t bad = {.v_mantissa = 99U,
                                  .v_fraction = 0U,
                                  .h_mantissa = 0U,
                                  .h_fraction = 0U};
  TEST_ASSERT_EQ((int)k_ra_err_invalid_arg, (int)ra_vin_set_uds_scale(&bad));
  TEST_END("vin set_uds_scale");
}

static void test_set_uds_passband(void)
{
  TEST_BEGIN("vin set_uds_passband");
  prep();
  const ra_vin_config_t cfg = make_cfg();
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_vin_init(&cfg));

  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_vin_set_uds_passband(0x10U, 0x20U));
  const uint32_t v = *ra_vin_reg32(k_ra_vin_off_uds_pass_bwidth);
  TEST_ASSERT_EQ((int)0x10U, (int)(v & (uint32_t)k_ra_vin_uds_bwidth_v));
  TEST_ASSERT_EQ((int)(0x20U << 16), (int)(v & (uint32_t)k_ra_vin_uds_bwidth_h));

  TEST_ASSERT_EQ((int)k_ra_err_invalid_arg, (int)ra_vin_set_uds_passband(0xFFU, 0x10U));
  TEST_END("vin set_uds_passband");
}

static void test_set_uds_clip(void)
{
  TEST_BEGIN("vin set_uds_clip");
  prep();
  const ra_vin_config_t cfg = make_cfg();
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_vin_init(&cfg));

  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_vin_set_uds_clip(600U, 800U));
  const uint32_t v = *ra_vin_reg32(k_ra_vin_off_uds_clip_size);
  TEST_ASSERT_EQ((int)600U, (int)(v & (uint32_t)k_ra_vin_uds_clip_v));
  TEST_ASSERT_EQ((int)(800U << 16), (int)(v & (uint32_t)k_ra_vin_uds_clip_h));

  TEST_ASSERT_EQ((int)k_ra_err_invalid_arg, (int)ra_vin_set_uds_clip(5000U, 100U));
  TEST_END("vin set_uds_clip");
}

static void test_set_uds_ctrl(void)
{
  TEST_BEGIN("vin set_uds_ctrl");
  prep();
  const ra_vin_config_t cfg = make_cfg();
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_vin_init(&cfg));

  const ra_vin_uds_ctrl_t c = {
    .b_cb_nearest = true,
    .g_y_nearest  = false,
    .r_cr_nearest = true,
    .multitap     = true,
    .advanced_bl  = false,
    .advanced_amd = true,
  };
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_vin_set_uds_ctrl(&c));
  const uint32_t v = *ra_vin_reg32(k_ra_vin_off_uds_ctrl);
  TEST_ASSERT((v & (uint32_t)k_ra_vin_uds_ctrl_ne_bcb) != 0U);
  TEST_ASSERT_EQ((int)0U, (int)(v & (uint32_t)k_ra_vin_uds_ctrl_ne_gy));
  TEST_ASSERT((v & (uint32_t)k_ra_vin_uds_ctrl_ne_rcr) != 0U);
  TEST_ASSERT((v & (uint32_t)k_ra_vin_uds_ctrl_bc) != 0U);
  TEST_ASSERT_EQ((int)0U, (int)(v & (uint32_t)k_ra_vin_uds_ctrl_bladv));
  TEST_ASSERT((v & (uint32_t)k_ra_vin_uds_ctrl_amd) != 0U);

  TEST_ASSERT_EQ((int)k_ra_err_null_ptr, (int)ra_vin_set_uds_ctrl(nullptr));
  TEST_END("vin set_uds_ctrl");
}

static void test_enable_scaling(void)
{
  TEST_BEGIN("vin enable_scaling");
  prep();
  const ra_vin_config_t cfg = make_cfg();
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_vin_init(&cfg));

  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_vin_enable_scaling(true));
  TEST_ASSERT(((*ra_vin_reg32(k_ra_vin_off_mc)) & (uint32_t)k_ra_vin_mc_scle) != 0U);

  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_vin_enable_scaling(false));
  TEST_ASSERT_EQ((int)0U, (int)((*ra_vin_reg32(k_ra_vin_off_mc)) & (uint32_t)k_ra_vin_mc_scle));
  TEST_END("vin enable_scaling");
}

/* ----------------------------------------------------------------------------
 * LUT test
 * --------------------------------------------------------------------------*/

static void test_lut_program(void)
{
  TEST_BEGIN("vin lut_program");
  prep();
  const ra_vin_config_t cfg = make_cfg();
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_vin_init(&cfg));

  uint8_t y[256];
  uint8_t cb[256];
  uint8_t cr[256];
  for (uint16_t i = 0U; i < 256U; ++i) {
    y[i]  = (uint8_t)i;
    cb[i] = (uint8_t)(i ^ 0x55U);
    cr[i] = (uint8_t)(i ^ 0xAAU);
  }
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_vin_lut_program(y, cb, cr, true));

  /* Last write wins on the host shim -- pointer points at index 255. */
  const uint32_t lutp = *ra_vin_reg32(k_ra_vin_off_lutp);
  TEST_ASSERT_EQ((int)((255U) << 20), (int)(lutp & (uint32_t)k_ra_vin_lutp_ltypr));
  const uint32_t lutd = *ra_vin_reg32(k_ra_vin_off_lutd);
  TEST_ASSERT_EQ((int)((uint32_t)y[255] << 16), (int)(lutd & (uint32_t)k_ra_vin_lutd_ltydt));

  /* MC.LUTE should be set. */
  TEST_ASSERT(((*ra_vin_reg32(k_ra_vin_off_mc)) & (uint32_t)k_ra_vin_mc_lute) != 0U);

  /* Disable LUT path. */
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_vin_lut_program(y, nullptr, nullptr, false));
  TEST_ASSERT_EQ((int)0U, (int)((*ra_vin_reg32(k_ra_vin_off_mc)) & (uint32_t)k_ra_vin_mc_lute));

  TEST_ASSERT_EQ((int)k_ra_err_null_ptr, (int)ra_vin_lut_program(nullptr, nullptr, nullptr, false));
  TEST_END("vin lut_program");
}

/* ----------------------------------------------------------------------------
 * CSC tests
 * --------------------------------------------------------------------------*/

static void test_set_yc_to_rgb(void)
{
  TEST_BEGIN("vin set_yc_to_rgb");
  prep();
  const ra_vin_config_t cfg = make_cfg();
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_vin_init(&cfg));

  const ra_vin_yc_to_rgb_t c = {
    .y_multiplier = 0x129FU,
    .enable_round = true,
    .y_sub        = 0x100U,
    .cbcr_sub     = 0x800U,
    .cr_mul_lo    = 0x1234U,
    .cr_mul_hi    = 0x05A6U,
    .cb_mul_lo    = 0x0789U,
    .cb_mul_hi    = 0x0ABCU,
  };
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_vin_set_yc_to_rgb(&c));

  const uint32_t csce1 = *ra_vin_reg32(k_ra_vin_off_csce1);
  TEST_ASSERT_EQ((int)0x129FU, (int)(csce1 & (uint32_t)k_ra_vin_csce1_ymul2));
  TEST_ASSERT((csce1 & (uint32_t)k_ra_vin_csce1_round) != 0U);

  const uint32_t csce2 = *ra_vin_reg32(k_ra_vin_off_csce2);
  TEST_ASSERT_EQ((int)0x800U, (int)(csce2 & (uint32_t)k_ra_vin_csce2_csub2));
  TEST_ASSERT_EQ((int)(0x100U << 16), (int)(csce2 & (uint32_t)k_ra_vin_csce2_ysub2));

  const uint32_t csce3 = *ra_vin_reg32(k_ra_vin_off_csce3);
  TEST_ASSERT_EQ((int)0x1234U, (int)(csce3 & (uint32_t)k_ra_vin_csce_mul_lo));
  TEST_ASSERT_EQ((int)(0x05A6U << 16), (int)(csce3 & (uint32_t)k_ra_vin_csce_mul_hi));

  TEST_ASSERT_EQ((int)k_ra_err_null_ptr, (int)ra_vin_set_yc_to_rgb(nullptr));
  TEST_END("vin set_yc_to_rgb");
}

static void test_set_rgb_to_yc(void)
{
  TEST_BEGIN("vin set_rgb_to_yc");
  prep();
  const ra_vin_config_t cfg = make_cfg();
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_vin_init(&cfg));

  const ra_vin_rgb_to_yc_chan_t y_row = {
    .r_coeff    = 0x4DU,
    .g_coeff    = 0x96U,
    .b_coeff    = 0x1DU,
    .add_offset = 0x010U,
    .enable_hen = true,
    .shift_down = 8U,
  };
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_vin_set_rgb_to_yc(0U, &y_row));

  TEST_ASSERT_EQ((int)0x4DU, (int)(*ra_vin_reg32(k_ra_vin_off_yccr1) & (uint32_t)k_ra_vin_yc1_rp));
  const uint32_t yccr2 = *ra_vin_reg32(k_ra_vin_off_yccr2);
  TEST_ASSERT_EQ((int)0x96U, (int)(yccr2 & (uint32_t)k_ra_vin_yc2_gp));
  TEST_ASSERT_EQ((int)(0x1DU << 16), (int)(yccr2 & (uint32_t)k_ra_vin_yc2_bp));
  const uint32_t yccr3 = *ra_vin_reg32(k_ra_vin_off_yccr3);
  TEST_ASSERT_EQ((int)0x010U, (int)(yccr3 & (uint32_t)k_ra_vin_yc3_ap));
  TEST_ASSERT((yccr3 & (uint32_t)k_ra_vin_yc3_hen) != 0U);
  TEST_ASSERT_EQ((int)(8U << 24), (int)(yccr3 & (uint32_t)k_ra_vin_yc3_sft));

  /* Channel 1 (Cb) writes the CBCCR triple, leaves YCCR untouched. */
  const ra_vin_rgb_to_yc_chan_t cb_row = {
    .r_coeff    = 0x12U,
    .g_coeff    = 0x34U,
    .b_coeff    = 0x56U,
    .add_offset = 0x020U,
    .enable_hen = false,
    .shift_down = 4U,
  };
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_vin_set_rgb_to_yc(1U, &cb_row));
  TEST_ASSERT_EQ((int)0x12U, (int)(*ra_vin_reg32(k_ra_vin_off_cbccr1) & (uint32_t)k_ra_vin_yc1_rp));

  /* Channel 2 (Cr). */
  const ra_vin_rgb_to_yc_chan_t cr_row = {
    .r_coeff    = 0x78U,
    .g_coeff    = 0x9AU,
    .b_coeff    = 0xBCU,
    .add_offset = 0x000U,
    .enable_hen = true,
    .shift_down = 0U,
  };
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_vin_set_rgb_to_yc(2U, &cr_row));
  TEST_ASSERT_EQ((int)0x78U, (int)(*ra_vin_reg32(k_ra_vin_off_crccr1) & (uint32_t)k_ra_vin_yc1_rp));

  TEST_ASSERT_EQ((int)k_ra_err_null_ptr, (int)ra_vin_set_rgb_to_yc(0U, nullptr));
  TEST_ASSERT_EQ((int)k_ra_err_invalid_arg, (int)ra_vin_set_rgb_to_yc(7U, &y_row));

  const ra_vin_rgb_to_yc_chan_t bad_shift = {
    .r_coeff    = 0U,
    .g_coeff    = 0U,
    .b_coeff    = 0U,
    .add_offset = 0U,
    .enable_hen = false,
    .shift_down = 99U,
  };
  TEST_ASSERT_EQ((int)k_ra_err_invalid_arg, (int)ra_vin_set_rgb_to_yc(0U, &bad_shift));
  TEST_END("vin set_rgb_to_yc");
}

/* ----------------------------------------------------------------------------
 * Dithering / YUV-444 / IM tests
 * --------------------------------------------------------------------------*/

static void test_set_dithering(void)
{
  TEST_BEGIN("vin set_dithering");
  prep();
  const ra_vin_config_t cfg = make_cfg();
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_vin_init(&cfg));

  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_vin_set_dithering((uint8_t)k_ra_vin_dc_ordered_2, true));
  const uint32_t mc = *ra_vin_reg32(k_ra_vin_off_mc);
  TEST_ASSERT_EQ((int)((uint32_t)k_ra_vin_dc_ordered_2 << 14),
                 (int)(mc & (uint32_t)k_ra_vin_mc_dc));
  TEST_ASSERT((mc & (uint32_t)k_ra_vin_mc_dc2) != 0U);

  TEST_ASSERT_EQ((int)k_ra_err_invalid_arg, (int)ra_vin_set_dithering(7U, false));
  TEST_END("vin set_dithering");
}

static void test_set_yuv444_mode(void)
{
  TEST_BEGIN("vin set_yuv444_mode");
  prep();
  const ra_vin_config_t cfg = make_cfg();
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_vin_init(&cfg));

  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_vin_set_yuv444_mode((uint8_t)k_ra_vin_yuv444_interpolate));
  TEST_ASSERT(((*ra_vin_reg32(k_ra_vin_off_mc)) & (uint32_t)k_ra_vin_mc_yuv444) != 0U);

  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_vin_set_yuv444_mode((uint8_t)k_ra_vin_yuv444_data_extend));
  TEST_ASSERT_EQ((int)0U, (int)((*ra_vin_reg32(k_ra_vin_off_mc)) & (uint32_t)k_ra_vin_mc_yuv444));

  TEST_ASSERT_EQ((int)k_ra_err_invalid_arg, (int)ra_vin_set_yuv444_mode(2U));
  TEST_END("vin set_yuv444_mode");
}

static void test_set_interlace_mode(void)
{
  TEST_BEGIN("vin set_interlace_mode");
  prep();
  const ra_vin_config_t cfg = make_cfg();
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_vin_init(&cfg));

  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_vin_set_interlace_mode((uint8_t)k_ra_vin_im_even_only));
  TEST_ASSERT_EQ((int)((uint32_t)k_ra_vin_im_even_only << 3),
                 (int)((*ra_vin_reg32(k_ra_vin_off_mc)) & (uint32_t)k_ra_vin_mc_im));
  TEST_ASSERT_EQ((int)k_ra_err_invalid_arg, (int)ra_vin_set_interlace_mode(5U));
  TEST_END("vin set_interlace_mode");
}

static void test_set_data_mode(void)
{
  TEST_BEGIN("vin set_data_mode");
  prep();
  const ra_vin_config_t cfg = make_cfg();
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_vin_init(&cfg));

  const ra_vin_data_mode_t m = {
    .conv_mode  = (uint8_t)k_ra_vin_dtmd_yc_separated,
    .alpha_bit  = true,
    .byte_swap  = true,
    .extend_rgb = true,
    .yc_through = false,
    .y_mode     = (uint8_t)k_ra_vin_ymode_y10_cbcr,
    .alpha_byte = 0xC3U,
  };
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_vin_set_data_mode(&m));
  const uint32_t v = *ra_vin_reg32(k_ra_vin_off_dmr);
  TEST_ASSERT_EQ((int)k_ra_vin_dtmd_yc_separated, (int)(v & (uint32_t)k_ra_vin_dmr_dtmd));
  TEST_ASSERT((v & (uint32_t)k_ra_vin_dmr_abit) != 0U);
  TEST_ASSERT((v & (uint32_t)k_ra_vin_dmr_bpsm) != 0U);
  TEST_ASSERT((v & (uint32_t)k_ra_vin_dmr_exrgb) != 0U);
  TEST_ASSERT_EQ((int)0U, (int)(v & (uint32_t)k_ra_vin_dmr_yc_thr));
  TEST_ASSERT_EQ((int)((uint32_t)k_ra_vin_ymode_y10_cbcr << 12),
                 (int)(v & (uint32_t)k_ra_vin_dmr_ymode));
  TEST_ASSERT_EQ((int)((uint32_t)0xC3U << 24), (int)(v & (uint32_t)k_ra_vin_dmr_a8bit));

  TEST_ASSERT_EQ((int)k_ra_err_null_ptr, (int)ra_vin_set_data_mode(nullptr));

  ra_vin_data_mode_t bad = m;
  bad.conv_mode          = 7U;
  TEST_ASSERT_EQ((int)k_ra_err_invalid_arg, (int)ra_vin_set_data_mode(&bad));
  TEST_END("vin set_data_mode");
}

/* ----------------------------------------------------------------------------
 * CSI input / framebuffer tests
 * --------------------------------------------------------------------------*/

static void test_set_csi_input(void)
{
  TEST_BEGIN("vin set_csi_input");
  prep();
  const ra_vin_config_t cfg = make_cfg();
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_vin_init(&cfg));

  const ra_vin_csi_input_t in = {
    .virtual_channel = 2U,
    .data_type       = (uint8_t)k_ra_vin_csi_dt_rgb888,
    .zero_extend     = true,
  };
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_vin_set_csi_input(&in));
  const uint32_t v = *ra_vin_reg32(k_ra_vin_off_csi_ifmd);
  TEST_ASSERT_EQ((int)2U, (int)(v & (uint32_t)k_ra_vin_csi_ifmd_vc_sel));
  TEST_ASSERT_EQ((int)((uint32_t)k_ra_vin_csi_dt_rgb888 << 8),
                 (int)(v & (uint32_t)k_ra_vin_csi_ifmd_dt));
  TEST_ASSERT((v & (uint32_t)k_ra_vin_csi_ifmd_des0) != 0U);

  TEST_ASSERT_EQ((int)k_ra_err_null_ptr, (int)ra_vin_set_csi_input(nullptr));

  ra_vin_csi_input_t bad = in;
  bad.virtual_channel    = 99U;
  TEST_ASSERT_EQ((int)k_ra_err_invalid_arg, (int)ra_vin_set_csi_input(&bad));
  TEST_END("vin set_csi_input");
}

static void test_set_field_detect(void)
{
  TEST_BEGIN("vin set_field_detect");
  prep();
  const ra_vin_config_t cfg = make_cfg();
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_vin_init(&cfg));

  const ra_vin_field_detect_t d = {
    .enable         = true,
    .even_field_sel = true,
    .even_field_num = (uint8_t)k_ra_vin_test_field_even_one,
  };
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_vin_set_field_detect(&d));
  const uint32_t v = *ra_vin_reg32(k_ra_vin_off_csifld);
  TEST_ASSERT((v & (uint32_t)k_ra_vin_csifld_fld_en) != 0U);
  TEST_ASSERT((v & (uint32_t)k_ra_vin_csifld_fld_sel) != 0U);
  TEST_ASSERT((v & (uint32_t)k_ra_vin_csifld_fld_num) != 0U);

  TEST_ASSERT_EQ((int)k_ra_err_null_ptr, (int)ra_vin_set_field_detect(nullptr));

  ra_vin_field_detect_t bad = d;
  bad.even_field_num        = 5U;
  TEST_ASSERT_EQ((int)k_ra_err_invalid_arg, (int)ra_vin_set_field_detect(&bad));
  TEST_END("vin set_field_detect");
}

static void test_set_framebuffers(void)
{
  TEST_BEGIN("vin set_framebuffers");
  prep();
  const ra_vin_config_t cfg = make_cfg();
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_vin_init(&cfg));

  TEST_ASSERT_EQ((int)k_ra_ok,
                 (int)ra_vin_set_framebuffers((uint32_t)k_ra_vin_test_fb_alt, 0U, 0U));
  TEST_ASSERT_EQ((int)k_ra_vin_test_fb_alt, (int)*ra_vin_reg32(k_ra_vin_off_mb1));
  /* MB2/MB3 unchanged. */
  TEST_ASSERT_EQ((int)k_ra_vin_test_fb2, (int)*ra_vin_reg32(k_ra_vin_off_mb2));

  TEST_ASSERT_EQ((int)k_ra_err_invalid_arg, (int)ra_vin_set_framebuffers(0U, 0U, 0U));
  TEST_ASSERT_EQ((int)k_ra_err_invalid_arg, (int)ra_vin_set_framebuffers(0x68000001U, 0U, 0U));
  TEST_END("vin set_framebuffers");
}

static void test_set_uv_offset(void)
{
  TEST_BEGIN("vin set_uv_offset");
  prep();
  const ra_vin_config_t cfg = make_cfg();
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_vin_init(&cfg));

  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_vin_set_uv_offset((uint32_t)k_ra_vin_test_uv_off));
  TEST_ASSERT_EQ((int)k_ra_vin_test_uv_off, (int)*ra_vin_reg32(k_ra_vin_off_uvaof));

  TEST_ASSERT_EQ((int)k_ra_err_invalid_arg,
                 (int)ra_vin_set_uv_offset((uint32_t)k_ra_vin_test_uv_bad));
  TEST_END("vin set_uv_offset");
}

/* ----------------------------------------------------------------------------
 * Status / IRQ tests
 * --------------------------------------------------------------------------*/

static void test_status_get_clear(void)
{
  TEST_BEGIN("vin status get + clear");
  prep();

  const ra_vin_config_t cfg = make_cfg();
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_vin_init(&cfg));

  /* Inject a synthetic INTS value with both W1C and non-W1C bits. */
  *ra_vin_reg32(k_ra_vin_off_ints) = (uint32_t)k_ra_vin_int_fos | (uint32_t)k_ra_vin_int_eof;
  uint32_t value                   = 0U;
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_vin_get_status(&value));
  TEST_ASSERT((value & (uint32_t)k_ra_vin_int_fos) != 0U);
  TEST_ASSERT((value & (uint32_t)k_ra_vin_int_eof) != 0U);

  /* Clearing only writes the W1C subset. */
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_vin_clear_status((uint32_t)k_ra_vin_int_fos));
  const uint32_t after = *ra_vin_reg32(k_ra_vin_off_ints);
  TEST_ASSERT_EQ((int)k_ra_vin_int_fos, (int)after);

  TEST_ASSERT_EQ((int)k_ra_err_null_ptr, (int)ra_vin_get_status(nullptr));
  TEST_END("vin status get + clear");
}

static void test_module_status(void)
{
  TEST_BEGIN("vin module status");
  prep();
  const ra_vin_config_t cfg = make_cfg();
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_vin_init(&cfg));

  /* Inject CA = 1, FBS = 2, FMS = 1. */
  const uint32_t synthetic = (uint32_t)k_ra_vin_ms_ca | ((uint32_t)2U << 3) | ((uint32_t)1U << 19) |
                             (uint32_t)k_ra_vin_ms_av;
  *ra_vin_reg32(k_ra_vin_off_ms) = synthetic;

  ra_vin_module_status_t s = {};
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_vin_get_module_status(&s));
  TEST_ASSERT(s.capture_active);
  TEST_ASSERT(s.active_video);
  TEST_ASSERT_EQ((int)2U, (int)s.frame_buffer_id);
  TEST_ASSERT_EQ((int)1U, (int)s.latest_frame_buffer);

  TEST_ASSERT_EQ((int)k_ra_err_null_ptr, (int)ra_vin_get_module_status(nullptr));

  uint8_t fbs = 0U;
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_vin_get_active_buffer(&fbs));
  TEST_ASSERT_EQ((int)2U, (int)fbs);
  TEST_ASSERT_EQ((int)k_ra_err_null_ptr, (int)ra_vin_get_active_buffer(nullptr));
  TEST_END("vin module status");
}

static void test_line_count(void)
{
  TEST_BEGIN("vin line count");
  prep();
  const ra_vin_config_t cfg = make_cfg();
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_vin_init(&cfg));

  *ra_vin_reg32(k_ra_vin_off_lc) = 0x123U;
  uint16_t lc                    = 0U;
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_vin_get_line_count(&lc));
  TEST_ASSERT_EQ((int)0x123U, (int)lc);
  TEST_ASSERT_EQ((int)k_ra_err_null_ptr, (int)ra_vin_get_line_count(nullptr));
  TEST_END("vin line count");
}

static void test_set_interrupt_enable(void)
{
  TEST_BEGIN("vin set_interrupt_enable");
  prep();
  const ra_vin_config_t cfg = make_cfg();
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_vin_init(&cfg));

  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_vin_set_interrupt_enable((uint32_t)k_ra_vin_int_axi_err));
  TEST_ASSERT_EQ((int)k_ra_vin_int_axi_err, (int)*ra_vin_reg32(k_ra_vin_off_ie));
  TEST_END("vin set_interrupt_enable");
}

static void test_set_scanline_compare(void)
{
  TEST_BEGIN("vin set_scanline_compare");
  prep();
  const ra_vin_config_t cfg = make_cfg();
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_vin_init(&cfg));

  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_vin_set_scanline_compare(720U));
  TEST_ASSERT_EQ((int)720U, (int)*ra_vin_reg32(k_ra_vin_off_si));
  TEST_ASSERT_EQ((int)k_ra_err_invalid_arg, (int)ra_vin_set_scanline_compare(5000U));
  TEST_END("vin set_scanline_compare");
}

static void test_attach_and_dispatch(void)
{
  TEST_BEGIN("vin attach + dispatch");
  prep();

  const ra_vin_config_t cfg = make_cfg();
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_vin_init(&cfg));
  TEST_ASSERT_EQ((int)k_ra_ok,
                 (int)ra_vin_attach_handler(stub_vin_cb, (void*)(uintptr_t)k_ra_vin_test_ctx));

  /* Drop a status value into INTS, then dispatch. */
  *ra_vin_reg32(k_ra_vin_off_ints) = (uint32_t)k_ra_vin_test_status;
  ra_vin_dispatch();
  TEST_ASSERT_EQ((int)1U, (int)s_vin_cb_count);
  TEST_ASSERT_EQ((int)k_ra_vin_test_status, (int)s_vin_cb_last_mask);
  TEST_ASSERT_EQ((int)k_ra_vin_test_ctx, (int)(uintptr_t)s_vin_cb_last_ctx);

  /* With callback detached, dispatch must still W1C without firing. */
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_vin_attach_handler(nullptr, nullptr));
  *ra_vin_reg32(k_ra_vin_off_ints) = (uint32_t)k_ra_vin_int_fos;
  ra_vin_dispatch();
  TEST_ASSERT_EQ((int)1U, (int)s_vin_cb_count);
  TEST_END("vin attach + dispatch");
}

static void test_power_transition(void)
{
  TEST_BEGIN("vin power transition");
  prep();

  const ra_vin_config_t cfg = make_cfg();
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_vin_init(&cfg));
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_vin_capture_arm(k_ra_vin_capture_continuous));
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_vin_enter_stop());

  const uint32_t mc = *ra_vin_reg32(k_ra_vin_off_mc);
  const uint32_t fc = *ra_vin_reg32(k_ra_vin_off_fc);
  TEST_ASSERT_EQ((int)0U, (int)(mc & (uint32_t)k_ra_vin_mc_me));
  TEST_ASSERT_EQ((int)0U, (int)(fc & (uint32_t)k_ra_vin_fc_cc));

  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_vin_exit_stop());
  TEST_END("vin power transition");
}

/* ----------------------------------------------------------------------------
 * Sweep 17: ra_vin_capture_start(buf, w, h, format) + window + frame handler
 * --------------------------------------------------------------------------*/

/**
 * @enum ra_vin_test_geom_t
 * @brief Magic-free geometry for the new high-level capture tests.
 */
typedef enum : uint16_t {
  k_ra_vin_test_w        = 640U,
  k_ra_vin_test_h        = 480U,
  k_ra_vin_test_window_x = 16U,
  k_ra_vin_test_window_y = 8U,
  k_ra_vin_test_window_w = 320U,
  k_ra_vin_test_window_h = 240U,
} ra_vin_test_geom_t;

static uint32_t s_vin_frame_count;
static void*    s_vin_frame_last_buf;
static uint32_t s_vin_frame_last_len;
static void*    s_vin_frame_last_ctx;

/**
 * @brief Frame-end stub used by `test_attach_frame_handler`.
 */
static void stub_vin_frame(void* ctx, void* buf, uint32_t len)
{
  ++s_vin_frame_count;
  s_vin_frame_last_buf = buf;
  s_vin_frame_last_len = len;
  s_vin_frame_last_ctx = ctx;
}

static void test_capture_start_buf_happy(void)
{
  TEST_BEGIN("vin capture_start(buf,w,h,format) happy");
  prep();
  const ra_vin_config_t cfg = make_cfg();
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_vin_init(&cfg));

  void* const buf = (void*)(uintptr_t)k_ra_vin_test_fb_alt;
  TEST_ASSERT_EQ((int)k_ra_ok,
                 (int)ra_vin_capture_start(buf,
                                           (uint16_t)k_ra_vin_test_w,
                                           (uint16_t)k_ra_vin_test_h,
                                           k_ra_vin_input_rgb888));
  TEST_ASSERT_EQ((int)k_ra_vin_test_fb_alt, (int)*ra_vin_reg32(k_ra_vin_off_mb1));
  TEST_ASSERT_EQ((int)k_ra_vin_test_w, (int)*ra_vin_reg32(k_ra_vin_off_is));
  const uint32_t mc = *ra_vin_reg32(k_ra_vin_off_mc);
  TEST_ASSERT((mc & (uint32_t)k_ra_vin_mc_me) != 0U);
  TEST_END("vin capture_start(buf,w,h,format) happy");
}

static void test_capture_start_buf_null(void)
{
  TEST_BEGIN("vin capture_start NULL buf");
  prep();
  const ra_vin_config_t cfg = make_cfg();
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_vin_init(&cfg));
  TEST_ASSERT_EQ((int)k_ra_err_null_ptr,
                 (int)ra_vin_capture_start(nullptr,
                                           (uint16_t)k_ra_vin_test_w,
                                           (uint16_t)k_ra_vin_test_h,
                                           k_ra_vin_input_rgb888));
  TEST_END("vin capture_start NULL buf");
}

static void test_capture_start_buf_bad_geom(void)
{
  TEST_BEGIN("vin capture_start bad geometry");
  prep();
  const ra_vin_config_t cfg = make_cfg();
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_vin_init(&cfg));
  void* const buf = (void*)(uintptr_t)k_ra_vin_test_fb_alt;
  TEST_ASSERT_EQ(
    (int)k_ra_err_invalid_arg,
    (int)ra_vin_capture_start(buf, 0U, (uint16_t)k_ra_vin_test_h, k_ra_vin_input_rgb888));
  TEST_ASSERT_EQ(
    (int)k_ra_err_invalid_arg,
    (int)ra_vin_capture_start(buf, (uint16_t)k_ra_vin_test_w, 0U, k_ra_vin_input_rgb888));
  TEST_END("vin capture_start bad geometry");
}

static void test_capture_start_buf_misaligned(void)
{
  TEST_BEGIN("vin capture_start misaligned buf");
  prep();
  const ra_vin_config_t cfg = make_cfg();
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_vin_init(&cfg));
  void* const bad = (void*)(uintptr_t)(k_ra_vin_test_fb_alt + 1U);
  TEST_ASSERT_EQ((int)k_ra_err_invalid_arg,
                 (int)ra_vin_capture_start(bad,
                                           (uint16_t)k_ra_vin_test_w,
                                           (uint16_t)k_ra_vin_test_h,
                                           k_ra_vin_input_rgb888));
  TEST_END("vin capture_start misaligned buf");
}

static void test_capture_stop_wrapper(void)
{
  TEST_BEGIN("vin capture_stop wrapper");
  prep();
  const ra_vin_config_t cfg = make_cfg();
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_vin_init(&cfg));
  void* const buf = (void*)(uintptr_t)k_ra_vin_test_fb_alt;
  TEST_ASSERT_EQ((int)k_ra_ok,
                 (int)ra_vin_capture_start(buf,
                                           (uint16_t)k_ra_vin_test_w,
                                           (uint16_t)k_ra_vin_test_h,
                                           k_ra_vin_input_rgb888));
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_vin_capture_stop());
  const uint32_t mc = *ra_vin_reg32(k_ra_vin_off_mc);
  TEST_ASSERT_EQ((int)0U, (int)(mc & (uint32_t)k_ra_vin_mc_me));
  TEST_END("vin capture_stop wrapper");
}

static void test_set_window_happy(void)
{
  TEST_BEGIN("vin set_window happy");
  prep();
  const ra_vin_config_t cfg = make_cfg();
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_vin_init(&cfg));
  TEST_ASSERT_EQ((int)k_ra_ok,
                 (int)ra_vin_set_window((uint16_t)k_ra_vin_test_window_x,
                                        (uint16_t)k_ra_vin_test_window_y,
                                        (uint16_t)k_ra_vin_test_window_w,
                                        (uint16_t)k_ra_vin_test_window_h));
  TEST_ASSERT_EQ((int)k_ra_vin_test_window_y, (int)*ra_vin_reg32(k_ra_vin_off_slprc));
  TEST_ASSERT_EQ((int)k_ra_vin_test_window_x, (int)*ra_vin_reg32(k_ra_vin_off_spprc));
  TEST_END("vin set_window happy");
}

static void test_set_window_invalid(void)
{
  TEST_BEGIN("vin set_window invalid");
  prep();
  const ra_vin_config_t cfg = make_cfg();
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_vin_init(&cfg));
  TEST_ASSERT_EQ((int)k_ra_err_invalid_arg, (int)ra_vin_set_window(0U, 0U, 0U, 0U));
  TEST_ASSERT_EQ((int)k_ra_err_invalid_arg, (int)ra_vin_set_window(4090U, 0U, 100U, 1U));
  TEST_END("vin set_window invalid");
}

static void test_attach_frame_handler(void)
{
  TEST_BEGIN("vin attach_frame_handler");
  prep();
  s_vin_frame_count    = 0U;
  s_vin_frame_last_buf = nullptr;
  s_vin_frame_last_len = 0U;
  s_vin_frame_last_ctx = nullptr;

  const ra_vin_config_t cfg = make_cfg();
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_vin_init(&cfg));

  void* const buf = (void*)(uintptr_t)k_ra_vin_test_fb_alt;
  TEST_ASSERT_EQ(
    (int)k_ra_ok,
    (int)ra_vin_attach_frame_handler(stub_vin_frame, (void*)(uintptr_t)k_ra_vin_test_ctx));
  TEST_ASSERT_EQ((int)k_ra_ok,
                 (int)ra_vin_capture_start(buf,
                                           (uint16_t)k_ra_vin_test_w,
                                           (uint16_t)k_ra_vin_test_h,
                                           k_ra_vin_input_raw8));

  /* Inject the FME bit into INTS, then dispatch. */
  *ra_vin_reg32(k_ra_vin_off_ints) = (uint32_t)k_ra_vin_int_fme;
  ra_vin_dispatch();
  TEST_ASSERT_EQ((int)1U, (int)s_vin_frame_count);
  TEST_ASSERT_EQ((int)(uintptr_t)buf, (int)(uintptr_t)s_vin_frame_last_buf);
  /* RAW8 = 1 byte/pixel -> len = w*h. */
  TEST_ASSERT_EQ((int)((uint32_t)k_ra_vin_test_w * (uint32_t)k_ra_vin_test_h),
                 (int)s_vin_frame_last_len);
  TEST_ASSERT_EQ((int)k_ra_vin_test_ctx, (int)(uintptr_t)s_vin_frame_last_ctx);

  /* Detach -> next dispatch must not increment. */
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_vin_attach_frame_handler(nullptr, nullptr));
  *ra_vin_reg32(k_ra_vin_off_ints) = (uint32_t)k_ra_vin_int_fme;
  ra_vin_dispatch();
  TEST_ASSERT_EQ((int)1U, (int)s_vin_frame_count);
  TEST_END("vin attach_frame_handler");
}

static void test_deinit(void)
{
  TEST_BEGIN("vin deinit");
  prep();

  const ra_vin_config_t cfg = make_cfg();
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_vin_init(&cfg));
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_vin_deinit());

  TEST_ASSERT_EQ((int)0U, (int)*ra_vin_reg32(k_ra_vin_off_mc));
  TEST_ASSERT_EQ((int)0U, (int)*ra_vin_reg32(k_ra_vin_off_fc));
  TEST_ASSERT_EQ((int)0U, (int)*ra_vin_reg32(k_ra_vin_off_ie));
  TEST_END("vin deinit");
}

/**
 * @test test_set_uds_clip_mcdc_bounds
 *
 * @par MC/DC:
 * Decision: `if ((v_size > k_ra_vin_uds_max_clip) ||
 *               (h_size > k_ra_vin_uds_max_clip))`
 * (2 conditions, libs/ra_hal/src/ra_vin.c line 436)
 * Standard: DO-178C Table A-7 obj 5; ISO 26262 Part 6 Table 12.
 * - Vector 1: v=0xFFFF,h=0     -> C1=T (short-circuits) -> Decision T
 * - Vector 2: v=0,    h=0      -> C1=F, C2=F -> Decision F (ok)
 * - Vector 3: v=0,    h=0xFFFF -> C1=F, C2=T -> Decision T
 * Vectors 1+2 vary C1; vectors 2+3 vary C2 with C1=F. N+1 minimal.
 */
static void test_set_uds_clip_mcdc_bounds(void)
{
  TEST_BEGIN("vin set_uds_clip MC/DC: v>max || h>max");
  prep();
  const ra_vin_config_t cfg = make_cfg();
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_vin_init(&cfg));

  /* Vector 1: v_size > max, h_size in range. C1=T short-circuits. */
  TEST_ASSERT_EQ((int)k_ra_err_invalid_arg, (int)ra_vin_set_uds_clip(0xFFFFU, 0U));
  /* Vector 2: both in range. C1=F, C2=F -> ok. */
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_vin_set_uds_clip(0U, 0U));
  /* Vector 3: v in range, h_size > max. C1=F, C2=T. */
  TEST_ASSERT_EQ((int)k_ra_err_invalid_arg, (int)ra_vin_set_uds_clip(0U, 0xFFFFU));
  TEST_END("vin set_uds_clip MC/DC: v>max || h>max");
}

/**
 * @test test_set_window_zero_dim_mcdc
 *
 * @par MC/DC:
 * Decision: `if ((w == 0U) || (h == 0U))`
 * (2 conditions, libs/ra_hal/src/ra_vin.c line 940)
 * Standard: DO-178C Table A-7 obj 5; IEC 61508-3 SIL 3.
 * - Vector 1: w=0,  h=10 -> C1=T (short-circuits) -> Decision T
 * - Vector 2: w=10, h=10 -> C1=F, C2=F -> Decision F (ok path)
 * - Vector 3: w=10, h=0  -> C1=F, C2=T -> Decision T
 */
static void test_set_window_zero_dim_mcdc(void)
{
  TEST_BEGIN("vin set_window MC/DC: w==0 || h==0");
  prep();
  const ra_vin_config_t cfg = make_cfg();
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_vin_init(&cfg));

  /* Vector 1: w=0. C1=T short-circuits. */
  TEST_ASSERT_EQ((int)k_ra_err_invalid_arg, (int)ra_vin_set_window(0U, 0U, 0U, 10U));
  /* Vector 2: both non-zero, end fits. C1=F, C2=F -> ok. */
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_vin_set_window(0U, 0U, 10U, 10U));
  /* Vector 3: h=0. C1=F, C2=T. */
  TEST_ASSERT_EQ((int)k_ra_err_invalid_arg, (int)ra_vin_set_window(0U, 0U, 10U, 0U));
  TEST_END("vin set_window MC/DC: w==0 || h==0");
}

/**
 * @test test_set_framebuffers_all_zero_mcdc
 *
 * @par MC/DC:
 * Decision: `if ((mb1 == 0U) && (mb2 == 0U) && (mb3 == 0U))`
 * (3 conditions, libs/ra_hal/src/ra_vin.c line 709)
 * Standard: DO-178C Table A-7 obj 5; ISO 26262 Part 6 Table 12.
 * For an N=3 short-circuiting AND, MC/DC requires N+1 = 4 vectors.
 * - Vector 1: mb1=0,mb2=0,mb3=0 -> C1=T,C2=T,C3=T -> Decision T (invalid_arg)
 * - Vector 2: mb1=fb1,_,_       -> C1=F (short-circuits) -> Decision F
 * - Vector 3: mb1=0,mb2=fb2,_   -> C1=T,C2=F (short-circuits) -> F
 * - Vector 4: mb1=0,mb2=0,mb3=fb3 -> C1=T,C2=T,C3=F -> Decision F
 * Pairs proving independence:
 *   - C1: vec1 vs vec2 (decision flips when C2,C3 are observation-masked
 *         by short-circuit; valid masking-MC/DC per DO-178C 6.4.4.2)
 *   - C2: vec3 vs a hypothetical vec where mb1=0,mb2=0,_ -- captured
 *         by the comparison vec3 vs vec1's prefix (C1=T held), C2 flip
 *         changes outcome
 *   - C3: vec4 vs vec1 (C1=T,C2=T held; C3 flip flips outcome)
 * Subsequent decision at line 712 (alignment check) is exercised by
 * vectors 2-4 with aligned addresses (k_ra_vin_test_fb*).
 */
static void test_set_framebuffers_all_zero_mcdc(void)
{
  TEST_BEGIN("vin set_framebuffers MC/DC: mb1==0 && mb2==0 && mb3==0");
  prep();
  const ra_vin_config_t cfg = make_cfg();
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_vin_init(&cfg));

  /* Vector 1: all zero -> Decision T -> invalid_arg. */
  TEST_ASSERT_EQ((int)k_ra_err_invalid_arg, (int)ra_vin_set_framebuffers(0U, 0U, 0U));
  /* Vector 2: mb1 non-zero -> C1=F short-circuits -> ok. */
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_vin_set_framebuffers((uint32_t)k_ra_vin_test_fb1, 0U, 0U));
  /* Vector 3: mb1=0, mb2 non-zero -> C1=T, C2=F -> ok. */
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_vin_set_framebuffers(0U, (uint32_t)k_ra_vin_test_fb2, 0U));
  /* Vector 4: mb1=0, mb2=0, mb3 non-zero -> C1=T,C2=T,C3=F -> ok. */
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_vin_set_framebuffers(0U, 0U, (uint32_t)k_ra_vin_test_fb3));
  TEST_END("vin set_framebuffers MC/DC: mb1==0 && mb2==0 && mb3==0");
}

/**
 * @test test_mcdc_capture_arm_mode_triple
 *
 * @par MC/DC:
 * Decision: ``if ((mode != single) && (mode != continuous) &&
 *               (mode != continuous_field_skip))`` (3 conditions,
 * libs/ra_hal/src/ra_vin.c ra_vin_capture_arm).
 *
 * @par DO-178C 6.4.4.3 omission rationale:
 * Full short-circuit MC/DC for N=3 AND requires N+1=4 vectors. We use
 * the canonical short-circuit set; each predicate flips with the others
 * held at their masking value (T):
 * - V1: mode=single                       -> C1=F short-circuits -> dec F (proceeds, ok)
 * - V2: mode=continuous                   -> C1=T, C2=F short    -> dec F (proceeds, ok)
 * - V3: mode=continuous_field_skip        -> C1=T, C2=T, C3=F    -> dec F (proceeds, ok)
 * - V4: mode=0xFE (none)                  -> C1=T, C2=T, C3=T    -> dec T -> invalid_arg
 */
static void test_mcdc_capture_arm_mode_triple(void)
{
  TEST_BEGIN("vin capture_arm MC/DC: mode != {single, cont, cont_field_skip}");
  prep();
  const ra_vin_config_t cfg = make_cfg();
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_vin_init(&cfg));
  /* V1: single. */
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_vin_capture_arm(k_ra_vin_capture_single));
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_vin_capture_disarm());
  /* V2: continuous. */
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_vin_capture_arm(k_ra_vin_capture_continuous));
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_vin_capture_disarm());
  /* V3: continuous_field_skip. */
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_vin_capture_arm(k_ra_vin_capture_continuous_field_skip));
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_vin_capture_disarm());
  /* V4: bogus. */
  TEST_ASSERT_EQ((int)k_ra_err_invalid_arg, (int)ra_vin_capture_arm((ra_vin_capture_mode_t)0xFEU));
  TEST_END("vin capture_arm MC/DC: mode != {single, cont, cont_field_skip}");
}

/**
 * @test test_mcdc_capture_arm_continuous_pair
 *
 * @par MC/DC:
 * Decision: ``if ((mode == continuous) || (mode == continuous_field_skip))``
 * (2 conditions, libs/ra_hal/src/ra_vin.c ra_vin_capture_arm). N+1=3.
 * - V1: mode=single        -> C1=F, C2=F             -> dec F (no FC.CC write)
 * - V2: mode=continuous    -> C1=T short-circuits    -> dec T (FC.CC programmed)
 * - V3: mode=cont_field_skip -> C1=F, C2=T            -> dec T (FC.CC programmed)
 * (V1,V2) flips C1 with C2 masked F; (V1,V3) flips C2 with C1 fixed F.
 */
static void test_mcdc_capture_arm_continuous_pair(void)
{
  TEST_BEGIN("vin capture_arm MC/DC: mode == cont || mode == cont_field_skip");
  prep();
  const ra_vin_config_t cfg = make_cfg();
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_vin_init(&cfg));
  volatile uint32_t* fc_reg = ra_vin_reg32(k_ra_vin_off_fc);
  /* V1: single -- FC.CC must remain clear. */
  *fc_reg = 0U;
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_vin_capture_arm(k_ra_vin_capture_single));
  TEST_ASSERT_EQ((int32_t)0, (int32_t)(*fc_reg & k_ra_vin_fc_cc));
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_vin_capture_disarm());
  /* V2: continuous -- FC.CC must be set. */
  *fc_reg = 0U;
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_vin_capture_arm(k_ra_vin_capture_continuous));
  TEST_ASSERT((*fc_reg & k_ra_vin_fc_cc) != 0U);
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_vin_capture_disarm());
  /* V3: continuous_field_skip -- FC.CC must be set. */
  *fc_reg = 0U;
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_vin_capture_arm(k_ra_vin_capture_continuous_field_skip));
  TEST_ASSERT((*fc_reg & k_ra_vin_fc_cc) != 0U);
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_vin_capture_disarm());
  TEST_END("vin capture_arm MC/DC: mode == cont || mode == cont_field_skip");
}

/**
 * @test test_mcdc_set_preclip_window_pair
 *
 * @par MC/DC:
 * Decision: ``if ((window->line_end < window->line_start) ||
 *               (window->pixel_end < window->pixel_start))`` (2 conditions,
 * libs/ra_hal/src/ra_vin.c ra_vin_set_preclip). N+1 = 3.
 * - V1: line_end>=start, pixel_end>=start  -> C1=F, C2=F -> dec F (proceeds, ok)
 * - V2: line_end<start                     -> C1=T short -> dec T -> invalid_arg
 * - V3: line ok, pixel_end<start           -> C1=F, C2=T -> dec T -> invalid_arg
 */
static void test_mcdc_set_preclip_window_pair(void)
{
  TEST_BEGIN("vin set_preclip MC/DC: line_end<start || pixel_end<start");
  prep();
  const ra_vin_config_t cfg = make_cfg();
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_vin_init(&cfg));
  /* V1: well-formed window. */
  const ra_vin_preclip_t v1 = {.line_start  = 0U,
                               .line_end    = 10U,
                               .pixel_start = 0U,
                               .pixel_end   = 10U};
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_vin_set_preclip(&v1));
  /* V2: line_end < line_start. */
  const ra_vin_preclip_t v2 = {.line_start  = 10U,
                               .line_end    = 0U,
                               .pixel_start = 0U,
                               .pixel_end   = 10U};
  TEST_ASSERT_EQ((int)k_ra_err_invalid_arg, (int)ra_vin_set_preclip(&v2));
  /* V3: pixel_end < pixel_start. */
  const ra_vin_preclip_t v3 = {.line_start  = 0U,
                               .line_end    = 10U,
                               .pixel_start = 10U,
                               .pixel_end   = 0U};
  TEST_ASSERT_EQ((int)k_ra_err_invalid_arg, (int)ra_vin_set_preclip(&v3));
  TEST_END("vin set_preclip MC/DC: line_end<start || pixel_end<start");
}

/**
 * @test test_mcdc_set_uds_scale_quad
 *
 * @par MC/DC:
 * Decision (libs/ra_hal/src/ra_vin.c:428-430): 4-cond OR over UDS scale fields.
 * ``v_mant > MAX_MANT || h_mant > MAX_MANT || v_frac > MAX_FRAC || h_frac > MAX_FRAC``
 * MAX_MANT = 0xF, MAX_FRAC = 0xFFF. N+1 = 5 vectors.
 * - V1: all in range          -> all F -> dec F -> ok
 * - V2: v_mant = 0x10         -> C1=T short -> err
 * - V3: h_mant = 0x10         -> C1=F, C2=T -> err
 * - V4: v_frac = 0x1000       -> C3=T -> err
 * - V5: h_frac = 0x1000       -> C4=T -> err
 */
static void test_mcdc_set_uds_scale_quad(void)
{
  TEST_BEGIN("vin set_uds_scale MC/DC: 4-cond OR field range");
  prep();
  const ra_vin_config_t cfg = make_cfg();
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_vin_init(&cfg));
  /* V1 */
  const ra_vin_uds_scale_t v1 = {.v_mantissa = 1U,
                                 .h_mantissa = 1U,
                                 .v_fraction = 0U,
                                 .h_fraction = 0U};
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_vin_set_uds_scale(&v1));
  /* V2: v_mant out. */
  const ra_vin_uds_scale_t v2 = {.v_mantissa = 0x10U,
                                 .h_mantissa = 1U,
                                 .v_fraction = 0U,
                                 .h_fraction = 0U};
  TEST_ASSERT_EQ((int)k_ra_err_invalid_arg, (int)ra_vin_set_uds_scale(&v2));
  /* V3: h_mant out. */
  const ra_vin_uds_scale_t v3 = {.v_mantissa = 1U,
                                 .h_mantissa = 0x10U,
                                 .v_fraction = 0U,
                                 .h_fraction = 0U};
  TEST_ASSERT_EQ((int)k_ra_err_invalid_arg, (int)ra_vin_set_uds_scale(&v3));
  /* V4: v_frac out. */
  const ra_vin_uds_scale_t v4 = {.v_mantissa = 1U,
                                 .h_mantissa = 1U,
                                 .v_fraction = 0x1000U,
                                 .h_fraction = 0U};
  TEST_ASSERT_EQ((int)k_ra_err_invalid_arg, (int)ra_vin_set_uds_scale(&v4));
  /* V5: h_frac out. */
  const ra_vin_uds_scale_t v5 = {.v_mantissa = 1U,
                                 .h_mantissa = 1U,
                                 .v_fraction = 0U,
                                 .h_fraction = 0x1000U};
  TEST_ASSERT_EQ((int)k_ra_err_invalid_arg, (int)ra_vin_set_uds_scale(&v5));
  TEST_END("vin set_uds_scale MC/DC: 4-cond OR field range");
}

/**
 * @test test_mcdc_set_framebuffers_align_triple
 *
 * @par MC/DC:
 * Decision (libs/ra_hal/src/ra_vin.c:740-741): 3-cond OR over alignment masks.
 * ``(mb1 & MASK)!=0 || (mb2 & MASK)!=0 || (mb3 & MASK)!=0``  MASK=0x7F
 * N+1 = 4. Note: the prior all-zero check (line 737) gates this; we use
 * non-zero, aligned bases as the control.
 * - V1: all aligned (e.g. 0x100, 0, 0)           -> all F -> ok
 * - V2: mb1 misaligned (e.g. 0x101)              -> C1=T short -> err
 * - V3: mb2 misaligned                           -> C1=F, C2=T -> err
 * - V4: mb3 misaligned                           -> C3=T -> err
 */
static void test_mcdc_set_framebuffers_align_triple(void)
{
  TEST_BEGIN("vin set_framebuffers MC/DC: 3-cond OR misalignment");
  prep();
  const ra_vin_config_t cfg = make_cfg();
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_vin_init(&cfg));
  /* V1: aligned mb1 only. */
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_vin_set_framebuffers(0x100U, 0U, 0U));
  /* V2: mb1 misaligned. */
  TEST_ASSERT_EQ((int)k_ra_err_invalid_arg, (int)ra_vin_set_framebuffers(0x101U, 0U, 0U));
  /* V3: mb1 ok, mb2 misaligned. */
  TEST_ASSERT_EQ((int)k_ra_err_invalid_arg, (int)ra_vin_set_framebuffers(0x100U, 0x101U, 0U));
  /* V4: mb1, mb2 ok, mb3 misaligned. */
  TEST_ASSERT_EQ((int)k_ra_err_invalid_arg, (int)ra_vin_set_framebuffers(0x100U, 0x200U, 0x101U));
  TEST_END("vin set_framebuffers MC/DC: 3-cond OR misalignment");
}

/**
 * @test test_mcdc_capture_start_geom_quad
 *
 * @par MC/DC:
 * Decision (libs/ra_hal/src/ra_vin.c:958): 4-cond OR over geometry.
 * ``w == 0 || h == 0 || w > MAX || h > MAX``  MAX = 4096.
 * N+1 = 5 vectors. Buffer must be aligned (separate check after).
 * - V1: w=640, h=480, aligned buf                -> all F -> ok
 * - V2: w=0                                      -> C1=T short -> err
 * - V3: w=640, h=0                               -> C2=T -> err
 * - V4: w=4097, h=480                            -> C3=T -> err
 * - V5: w=640, h=4097                            -> C4=T -> err
 */
static void test_mcdc_capture_start_geom_quad(void)
{
  TEST_BEGIN("vin capture_start MC/DC: 4-cond OR geometry");
  prep();
  const ra_vin_config_t cfg = make_cfg();
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_vin_init(&cfg));
  /* 128-byte aligned buffer. */
  static uint8_t s_buf[1024] __attribute__((aligned(128)));
  /* V1 */
  TEST_ASSERT_EQ((int)k_ra_ok,
                 (int)ra_vin_capture_start(s_buf, 640U, 480U, k_ra_vin_input_ycbcr422_8));
  /* V2 */
  TEST_ASSERT_EQ((int)k_ra_err_invalid_arg,
                 (int)ra_vin_capture_start(s_buf, 0U, 480U, k_ra_vin_input_ycbcr422_8));
  /* V3 */
  TEST_ASSERT_EQ((int)k_ra_err_invalid_arg,
                 (int)ra_vin_capture_start(s_buf, 640U, 0U, k_ra_vin_input_ycbcr422_8));
  /* V4 */
  TEST_ASSERT_EQ((int)k_ra_err_invalid_arg,
                 (int)ra_vin_capture_start(s_buf, 4097U, 480U, k_ra_vin_input_ycbcr422_8));
  /* V5 */
  TEST_ASSERT_EQ((int)k_ra_err_invalid_arg,
                 (int)ra_vin_capture_start(s_buf, 640U, 4097U, k_ra_vin_input_ycbcr422_8));
  TEST_END("vin capture_start MC/DC: 4-cond OR geometry");
}

/**
 * @test test_mcdc_set_uds_passband_pair
 *
 * @par MC/DC:
 * Decision: ``if ((v_bwidth > k_ra_vin_uds_max_bw) || (h_bwidth > k_ra_vin_uds_max_bw))``
 * (2 conditions, libs/ra_hal/src/ra_vin.c:445 ra_vin_set_uds_passband).
 * Short-circuit OR: N+1 = 3 vectors.
 * - V1: v=0, h=0       -> C1=F, C2=F -> dec F (ok).
 * - V2: v=0xFF, h=0    -> C1=T short -> dec T -> invalid_arg.
 *   (V1->V2 isolates C1.)
 * - V3: v=0, h=0xFF    -> C1=F, C2=T -> dec T -> invalid_arg.
 *   (V1->V3 isolates C2.)
 */
static void test_mcdc_set_uds_passband_pair(void)
{
  TEST_BEGIN("vin set_uds_passband MC/DC: v||h > max_bw");
  prep();
  const ra_vin_config_t cfg = make_cfg();
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_vin_init(&cfg));
  /* V1 */
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_vin_set_uds_passband(0U, 0U));
  /* V2 */
  TEST_ASSERT_EQ((int)k_ra_err_invalid_arg, (int)ra_vin_set_uds_passband(0xFFU, 0U));
  /* V3 */
  TEST_ASSERT_EQ((int)k_ra_err_invalid_arg, (int)ra_vin_set_uds_passband(0U, 0xFFU));
  TEST_END("vin set_uds_passband MC/DC: v||h > max_bw");
}

/**
 * @test test_mcdc_set_data_mode_pair
 *
 * @par MC/DC:
 * Decision: ``if ((mode->conv_mode > k_ra_vin_max_dtmd) || (mode->y_mode > k_ra_vin_max_ymode))``
 * (2 conditions, libs/ra_hal/src/ra_vin.c:674 ra_vin_set_data_mode).
 * Short-circuit OR: N+1 = 3 vectors.
 * - V1: conv=0, y=0   -> C1=F, C2=F -> dec F (ok).
 * - V2: conv=99, y=0  -> C1=T short -> dec T -> invalid_arg.
 * - V3: conv=0, y=99  -> C1=F, C2=T -> dec T -> invalid_arg.
 */
static void test_mcdc_set_data_mode_pair(void)
{
  TEST_BEGIN("vin set_data_mode MC/DC: conv||y > max");
  prep();
  const ra_vin_config_t cfg = make_cfg();
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_vin_init(&cfg));
  ra_vin_data_mode_t m = {};
  m.conv_mode          = 0U;
  m.y_mode             = 0U;
  /* V1 */
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_vin_set_data_mode(&m));
  /* V2 */
  m.conv_mode = 99U;
  m.y_mode    = 0U;
  TEST_ASSERT_EQ((int)k_ra_err_invalid_arg, (int)ra_vin_set_data_mode(&m));
  /* V3 */
  m.conv_mode = 0U;
  m.y_mode    = 99U;
  TEST_ASSERT_EQ((int)k_ra_err_invalid_arg, (int)ra_vin_set_data_mode(&m));
  TEST_END("vin set_data_mode MC/DC: conv||y > max");
}

/**
 * @test test_mcdc_set_csi_input_pair
 *
 * @par MC/DC:
 * Decision: ``if ((input->virtual_channel > k_ra_vin_max_vc_sel) ||
 *               (input->data_type > k_ra_vin_max_dt))``
 * (2 conditions, libs/ra_hal/src/ra_vin.c:700 ra_vin_set_csi_input).
 * Short-circuit OR: N+1 = 3 vectors.
 * - V1: vc=0, dt=0  -> C1=F, C2=F -> dec F.
 * - V2: vc=99, dt=0 -> C1=T short -> dec T -> invalid_arg.
 * - V3: vc=0, dt=99 -> C1=F, C2=T -> dec T -> invalid_arg.
 */
static void test_mcdc_set_csi_input_pair(void)
{
  TEST_BEGIN("vin set_csi_input MC/DC: vc||dt > max");
  prep();
  const ra_vin_config_t cfg = make_cfg();
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_vin_init(&cfg));
  ra_vin_csi_input_t in = {};
  /* V1 */
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_vin_set_csi_input(&in));
  /* V2 */
  in.virtual_channel = 99U;
  in.data_type       = 0U;
  TEST_ASSERT_EQ((int)k_ra_err_invalid_arg, (int)ra_vin_set_csi_input(&in));
  /* V3 */
  in.virtual_channel = 0U;
  in.data_type       = 99U;
  TEST_ASSERT_EQ((int)k_ra_err_invalid_arg, (int)ra_vin_set_csi_input(&in));
  TEST_END("vin set_csi_input MC/DC: vc||dt > max");
}

/**
 * @test test_mcdc_set_window_pair
 *
 * @par MC/DC:
 * Decision: ``if ((pixel_end > k_ra_vin_preclip_mask) || (line_end > k_ra_vin_preclip_mask))``
 * (2 conditions, libs/ra_hal/src/ra_vin.c:992 ra_vin_set_window).
 * Short-circuit OR: N+1 = 3 vectors. preclip_mask = 0xFFF (4095).
 * - V1: w=8,h=8 at (0,0) -> ends 7,7  -> C1=F, C2=F -> dec F.
 * - V2: w=4096 at x=1    -> pixel_end=4096 > 4095   -> C1=T short -> invalid_arg.
 * - V3: w=8,h=4096 at y=1 -> line_end=4096 > 4095   -> C1=F, C2=T -> invalid_arg.
 */
static void test_mcdc_set_window_pair(void)
{
  TEST_BEGIN("vin set_window MC/DC: pixel_end||line_end > mask");
  prep();
  const ra_vin_config_t cfg = make_cfg();
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_vin_init(&cfg));
  /* V1: ok small window. */
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_vin_set_window(0U, 0U, 8U, 8U));
  /* V2: pixel_end overflow. */
  TEST_ASSERT_EQ((int)k_ra_err_invalid_arg, (int)ra_vin_set_window(1U, 0U, 4096U, 8U));
  /* V3: line_end overflow. */
  TEST_ASSERT_EQ((int)k_ra_err_invalid_arg, (int)ra_vin_set_window(0U, 1U, 8U, 4096U));
  TEST_END("vin set_window MC/DC: pixel_end||line_end > mask");
}

int32_t main(void)
{
  test_init_happy();
  test_init_null_cfg();
  test_init_zero_stride();
  test_init_bad_im();
  test_init_bad_clp();
  test_reset();
  test_capture_arm_single();
  test_capture_arm_continuous();
  test_capture_arm_field_skip();
  test_capture_arm_invalid_mode();
  test_capture_arm_already_running();
  test_capture_stop();
  test_capture_disarm_idle();
  test_set_preclip();
  test_set_uds_scale();
  test_set_uds_passband();
  test_set_uds_clip();
  test_set_uds_ctrl();
  test_enable_scaling();
  test_lut_program();
  test_set_yc_to_rgb();
  test_set_rgb_to_yc();
  test_set_dithering();
  test_set_yuv444_mode();
  test_set_interlace_mode();
  test_set_data_mode();
  test_set_csi_input();
  test_set_field_detect();
  test_set_framebuffers();
  test_set_uv_offset();
  test_status_get_clear();
  test_module_status();
  test_line_count();
  test_set_interrupt_enable();
  test_set_scanline_compare();
  test_attach_and_dispatch();
  test_capture_start_buf_happy();
  test_capture_start_buf_null();
  test_capture_start_buf_bad_geom();
  test_capture_start_buf_misaligned();
  test_capture_stop_wrapper();
  test_set_window_happy();
  test_set_window_invalid();
  test_attach_frame_handler();
  test_power_transition();
  test_deinit();
  test_set_uds_clip_mcdc_bounds();
  test_set_window_zero_dim_mcdc();
  test_set_framebuffers_all_zero_mcdc();
  test_mcdc_capture_arm_mode_triple();
  test_mcdc_capture_arm_continuous_pair();
  test_mcdc_set_preclip_window_pair();
  test_mcdc_set_uds_scale_quad();
  test_mcdc_set_framebuffers_align_triple();
  test_mcdc_capture_start_geom_quad();
  test_mcdc_set_uds_passband_pair();
  test_mcdc_set_data_mode_pair();
  test_mcdc_set_csi_input_pair();
  test_mcdc_set_window_pair();
  (void)fprintf(stderr, "[OK  ] test_ra_vin.c\n");
  return 0;
}
