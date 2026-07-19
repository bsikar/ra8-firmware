/**
 * @file test_ra8_vin_config.c
 * @brief Unit tests for the VIN scaling / CSC / routing / status setters
 *
 * @details
 * Split sibling of the original test_ra8_vin.c suite covering the
 * configuration surface of ra8_vin.c against the host-side simulated
 * MMIO (``ra8_sim_mmap``):
 *
 * - pre-clip window + UDS scale / passband / clip / control and the
 *   scaling enable
 * - LUT programming and both CSC directions incl. dithering,
 *   YUV-444 and interlace modes
 * - data mode, CSI input routing, field detect, framebuffer + UV
 *   offset setters
 * - status get / clear, module status, line count, interrupt enable,
 *   scanline compare, IRQ dispatch fan-out and power transition
 *
 * Sibling suites: test_ra8_vin_capture.c (lifecycle + capture path)
 * and test_ra8_vin_mcdc.c (MC/DC vectors). Shared fixtures live in
 * support/vin_test_util.h.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_err.h"
#include "ra8_mstp.h"
#include "ra8_sim_mmap.h"
#include "ra8_vin.h"
#include "ra8_vin_regs.h"
#include "support/vin_test_util.h"
#include "unity_minimal.h"

/**
 * @enum t_vin_plane_t
 * @brief Synthetic YCbCr plane fixture parameters.
 *
 * @details
 * Each chroma plane is the byte index XORed with a distinct mask, so the two
 * planes never hold the same value at the same index -- a Cb/Cr swap in the
 * colour conversion is then visible rather than symmetric.
 */
typedef enum : uint16_t {
  k_t_plane_len = 256U,  /**< Samples per test plane.           */
  k_t_cb_mask   = 0x55U, /**< XOR mask generating the Cb plane. */
  k_t_cr_mask   = 0xAAU, /**< XOR mask generating the Cr plane. */
} t_vin_plane_t;

/**
 * @enum t_vin_bad_cfg_t
 * @brief Out-of-range configuration values the validator must reject.
 */
typedef enum : uint16_t {
  k_t_conv_mode_bad  = 7U,     /**< Conversion mode past the last defined one. */
  k_t_field_num_bad  = 5U,     /**< Even-field number past the legal range.    */
  k_t_vchannel_bad   = 99U,    /**< Virtual channel far past the MIPI CSI max. */
  k_t_line_count_reg = 0x123U, /**< A distinctive line count written straight to
                                    the LC register, to prove the read path.    */
} t_vin_bad_cfg_t;

/**
 * @enum ra8_vin_test_field_t
 * @brief Sample field values for the field-detect tests.
 */
typedef enum : uint8_t {
  k_ra8_vin_test_field_even_one = 1U, /**< RA8 vin test field even one. */
} ra8_vin_test_field_t;

static uint32_t s_vin_cb_count;
static uint32_t s_vin_cb_last_mask;
static void*    s_vin_cb_last_ctx;

/**
 * @brief Reset the host harness and zero shared callback counters.
 */
static void prep(void)
{
  ra8_sim_mmap_reset();
  (void)ra8_mstp_init();
  s_vin_cb_count     = 0U;
  s_vin_cb_last_mask = 0U;
  s_vin_cb_last_ctx  = nullptr;
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
 * Pre-clip / scaling tests
  *
  * @par MC/DC:
  * (no compound decisions in this test -- exercises the public-API
  * happy path / error-rejection contract; no `&&` or `||` in the
  * code under test that this case touches)
 * --------------------------------------------------------------------------*/

static void test_set_preclip(void)
{
  TEST_BEGIN("vin set_preclip");
  prep();
  const ra8_vin_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vin_init(&cfg));

  const ra8_vin_preclip_t w = {.line_start  = 4U,
                               .line_end    = 595U,
                               .pixel_start = 0U,
                               .pixel_end   = 1023U};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vin_set_preclip(&w));
  TEST_ASSERT_EQ(4, *ra8_vin_reg32(k_ra8_vin_off_slprc));
  TEST_ASSERT_EQ(595, *ra8_vin_reg32(k_ra8_vin_off_elprc));
  TEST_ASSERT_EQ(0, *ra8_vin_reg32(k_ra8_vin_off_spprc));
  TEST_ASSERT_EQ(1023, *ra8_vin_reg32(k_ra8_vin_off_epprc));

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_vin_set_preclip(nullptr));

  const ra8_vin_preclip_t bad = {.line_start  = 100U,
                                 .line_end    = 50U,
                                 .pixel_start = 0U,
                                 .pixel_end   = 1023U};
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_vin_set_preclip(&bad));

  const ra8_vin_preclip_t huge = {.line_start  = 0U,
                                  .line_end    = 5000U,
                                  .pixel_start = 0U,
                                  .pixel_end   = 100U};
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_vin_set_preclip(&huge));
  TEST_END("vin set_preclip");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_set_uds_scale(void)
{
  TEST_BEGIN("vin set_uds_scale");
  prep();
  const ra8_vin_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vin_init(&cfg));

  /* 1.5x V scale, 2.0x H scale: V = 1.500 -> mant 1, frac 0x800; H = 2.0 mant 2. */
  const ra8_vin_uds_scale_t s = {
    .v_mantissa = 1U,
    .v_fraction = 0x800U,
    .h_mantissa = 2U,
    .h_fraction = 0U,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vin_set_uds_scale(&s));
  const uint32_t v = *ra8_vin_reg32(k_ra8_vin_off_uds_scale);
  TEST_ASSERT_EQ(0x800U, (v & (uint32_t)k_ra8_vin_uds_scale_vfrac));
  TEST_ASSERT_EQ((1U << 12), (v & (uint32_t)k_ra8_vin_uds_scale_vmant));
  TEST_ASSERT_EQ(0U, (v & (uint32_t)k_ra8_vin_uds_scale_hfrac));
  TEST_ASSERT_EQ((2U << 28), (v & (uint32_t)k_ra8_vin_uds_scale_hmant));

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_vin_set_uds_scale(nullptr));

  const ra8_vin_uds_scale_t bad = {.v_mantissa = 99U,
                                   .v_fraction = 0U,
                                   .h_mantissa = 0U,
                                   .h_fraction = 0U};
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_vin_set_uds_scale(&bad));
  TEST_END("vin set_uds_scale");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_set_uds_passband(void)
{
  TEST_BEGIN("vin set_uds_passband");
  prep();
  const ra8_vin_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vin_init(&cfg));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_vin_set_uds_passband(0x10U, 0x20U));
  const uint32_t v = *ra8_vin_reg32(k_ra8_vin_off_uds_pass_bwidth);
  TEST_ASSERT_EQ(0x10U, (v & (uint32_t)k_ra8_vin_uds_bwidth_v));
  TEST_ASSERT_EQ((0x20U << 16), (v & (uint32_t)k_ra8_vin_uds_bwidth_h));

  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_vin_set_uds_passband(0xFFU, 0x10U));
  TEST_END("vin set_uds_passband");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_set_uds_clip(void)
{
  TEST_BEGIN("vin set_uds_clip");
  prep();
  const ra8_vin_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vin_init(&cfg));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_vin_set_uds_clip(600U, 800U));
  const uint32_t v = *ra8_vin_reg32(k_ra8_vin_off_uds_clip_size);
  TEST_ASSERT_EQ(600U, (v & (uint32_t)k_ra8_vin_uds_clip_v));
  TEST_ASSERT_EQ((800U << 16), (v & (uint32_t)k_ra8_vin_uds_clip_h));

  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_vin_set_uds_clip(5000U, 100U));
  TEST_END("vin set_uds_clip");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_set_uds_ctrl(void)
{
  TEST_BEGIN("vin set_uds_ctrl");
  prep();
  const ra8_vin_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vin_init(&cfg));

  const ra8_vin_uds_ctrl_t c = {
    .b_cb_nearest = true,
    .g_y_nearest  = false,
    .r_cr_nearest = true,
    .multitap     = true,
    .advanced_bl  = false,
    .advanced_amd = true,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vin_set_uds_ctrl(&c));
  const uint32_t v = *ra8_vin_reg32(k_ra8_vin_off_uds_ctrl);
  TEST_ASSERT((v & (uint32_t)k_ra8_vin_uds_ctrl_ne_bcb) != 0U);
  TEST_ASSERT_EQ(0U, (v & (uint32_t)k_ra8_vin_uds_ctrl_ne_gy));
  TEST_ASSERT((v & (uint32_t)k_ra8_vin_uds_ctrl_ne_rcr) != 0U);
  TEST_ASSERT((v & (uint32_t)k_ra8_vin_uds_ctrl_bc) != 0U);
  TEST_ASSERT_EQ(0U, (v & (uint32_t)k_ra8_vin_uds_ctrl_bladv));
  TEST_ASSERT((v & (uint32_t)k_ra8_vin_uds_ctrl_amd) != 0U);

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_vin_set_uds_ctrl(nullptr));
  TEST_END("vin set_uds_ctrl");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_enable_scaling(void)
{
  TEST_BEGIN("vin enable_scaling");
  prep();
  const ra8_vin_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vin_init(&cfg));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_vin_enable_scaling(true));
  TEST_ASSERT(((*ra8_vin_reg32(k_ra8_vin_off_mc)) & (uint32_t)k_ra8_vin_mc_scle) != 0U);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_vin_enable_scaling(false));
  TEST_ASSERT_EQ(0U, ((*ra8_vin_reg32(k_ra8_vin_off_mc)) & (uint32_t)k_ra8_vin_mc_scle));
  TEST_END("vin enable_scaling");
}

/* ----------------------------------------------------------------------------
 * LUT test
  *
  * @par MC/DC:
  * (no compound decisions in this test -- exercises the public-API
  * happy path / error-rejection contract; no `&&` or `||` in the
  * code under test that this case touches)
 * --------------------------------------------------------------------------*/

static void test_lut_program(void)
{
  TEST_BEGIN("vin lut_program");
  prep();
  const ra8_vin_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vin_init(&cfg));

  uint8_t y[k_t_plane_len];
  uint8_t cb[k_t_plane_len];
  uint8_t cr[k_t_plane_len];
  for (uint16_t i = 0U; i < k_t_plane_len; ++i) {
    y[i]  = (uint8_t)i;
    cb[i] = (uint8_t)(i ^ k_t_cb_mask);
    cr[i] = (uint8_t)(i ^ k_t_cr_mask);
  }
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vin_lut_program(y, cb, cr, true));

  /* Last write wins on the host shim -- pointer points at index 255. */
  const uint32_t lutp = *ra8_vin_reg32(k_ra8_vin_off_lutp);
  TEST_ASSERT_EQ(((255U) << 20), (lutp & (uint32_t)k_ra8_vin_lutp_ltypr));
  const uint32_t lutd = *ra8_vin_reg32(k_ra8_vin_off_lutd);
  TEST_ASSERT_EQ(((uint32_t)y[255] << 16), (lutd & (uint32_t)k_ra8_vin_lutd_ltydt));

  /* MC.LUTE should be set. */
  TEST_ASSERT(((*ra8_vin_reg32(k_ra8_vin_off_mc)) & (uint32_t)k_ra8_vin_mc_lute) != 0U);

  /* Disable LUT path. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vin_lut_program(y, nullptr, nullptr, false));
  TEST_ASSERT_EQ(0U, ((*ra8_vin_reg32(k_ra8_vin_off_mc)) & (uint32_t)k_ra8_vin_mc_lute));

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_vin_lut_program(nullptr, nullptr, nullptr, false));
  TEST_END("vin lut_program");
}

/* ----------------------------------------------------------------------------
 * CSC tests
  *
  * @par MC/DC:
  * (no compound decisions in this test -- exercises the public-API
  * happy path / error-rejection contract; no `&&` or `||` in the
  * code under test that this case touches)
 * --------------------------------------------------------------------------*/

static void test_set_yc_to_rgb(void)
{
  TEST_BEGIN("vin set_yc_to_rgb");
  prep();
  const ra8_vin_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vin_init(&cfg));

  const ra8_vin_yc_to_rgb_t c = {
    .y_multiplier = 0x129FU,
    .enable_round = true,
    .y_sub        = 0x100U,
    .cbcr_sub     = 0x800U,
    .cr_mul_lo    = 0x1234U,
    .cr_mul_hi    = 0x05A6U,
    .cb_mul_lo    = 0x0789U,
    .cb_mul_hi    = 0x0ABCU,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vin_set_yc_to_rgb(&c));

  const uint32_t csce1 = *ra8_vin_reg32(k_ra8_vin_off_csce1);
  TEST_ASSERT_EQ(0x129FU, (csce1 & (uint32_t)k_ra8_vin_csce1_ymul2));
  TEST_ASSERT((csce1 & (uint32_t)k_ra8_vin_csce1_round) != 0U);

  const uint32_t csce2 = *ra8_vin_reg32(k_ra8_vin_off_csce2);
  TEST_ASSERT_EQ(0x800U, (csce2 & (uint32_t)k_ra8_vin_csce2_csub2));
  TEST_ASSERT_EQ((0x100U << 16), (csce2 & (uint32_t)k_ra8_vin_csce2_ysub2));

  const uint32_t csce3 = *ra8_vin_reg32(k_ra8_vin_off_csce3);
  TEST_ASSERT_EQ(0x1234U, (csce3 & (uint32_t)k_ra8_vin_csce_mul_lo));
  TEST_ASSERT_EQ((0x05A6U << 16), (csce3 & (uint32_t)k_ra8_vin_csce_mul_hi));

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_vin_set_yc_to_rgb(nullptr));
  TEST_END("vin set_yc_to_rgb");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_set_rgb_to_yc(void)
{
  TEST_BEGIN("vin set_rgb_to_yc");
  prep();
  const ra8_vin_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vin_init(&cfg));

  const ra8_vin_rgb_to_yc_chan_t y_row = {
    .r_coeff    = 0x4DU,
    .g_coeff    = 0x96U,
    .b_coeff    = 0x1DU,
    .add_offset = 0x010U,
    .enable_hen = true,
    .shift_down = 8U,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vin_set_rgb_to_yc(0U, &y_row));

  TEST_ASSERT_EQ(0x4DU, (*ra8_vin_reg32(k_ra8_vin_off_yccr1) & (uint32_t)k_ra8_vin_yc1_rp));
  const uint32_t yccr2 = *ra8_vin_reg32(k_ra8_vin_off_yccr2);
  TEST_ASSERT_EQ(0x96U, (yccr2 & (uint32_t)k_ra8_vin_yc2_gp));
  TEST_ASSERT_EQ((0x1DU << 16), (yccr2 & (uint32_t)k_ra8_vin_yc2_bp));
  const uint32_t yccr3 = *ra8_vin_reg32(k_ra8_vin_off_yccr3);
  TEST_ASSERT_EQ(0x010U, (yccr3 & (uint32_t)k_ra8_vin_yc3_ap));
  TEST_ASSERT((yccr3 & (uint32_t)k_ra8_vin_yc3_hen) != 0U);
  TEST_ASSERT_EQ((8U << 24), (yccr3 & (uint32_t)k_ra8_vin_yc3_sft));

  /* Channel 1 (Cb) writes the CBCCR triple, leaves YCCR untouched. */
  const ra8_vin_rgb_to_yc_chan_t cb_row = {
    .r_coeff    = 0x12U,
    .g_coeff    = 0x34U,
    .b_coeff    = 0x56U,
    .add_offset = 0x020U,
    .enable_hen = false,
    .shift_down = 4U,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vin_set_rgb_to_yc(1U, &cb_row));
  TEST_ASSERT_EQ(0x12U, (*ra8_vin_reg32(k_ra8_vin_off_cbccr1) & (uint32_t)k_ra8_vin_yc1_rp));

  /* Channel 2 (Cr). */
  const ra8_vin_rgb_to_yc_chan_t cr_row = {
    .r_coeff    = 0x78U,
    .g_coeff    = 0x9AU,
    .b_coeff    = 0xBCU,
    .add_offset = 0x000U,
    .enable_hen = true,
    .shift_down = 0U,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vin_set_rgb_to_yc(2U, &cr_row));
  TEST_ASSERT_EQ(0x78U, (*ra8_vin_reg32(k_ra8_vin_off_crccr1) & (uint32_t)k_ra8_vin_yc1_rp));
  TEST_END("vin set_rgb_to_yc");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_set_rgb_to_yc_rejects(void)
{
  TEST_BEGIN("vin set_rgb_to_yc rejects");
  prep();
  const ra8_vin_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vin_init(&cfg));

  const ra8_vin_rgb_to_yc_chan_t y_row = {
    .r_coeff    = 0x4DU,
    .g_coeff    = 0x96U,
    .b_coeff    = 0x1DU,
    .add_offset = 0x010U,
    .enable_hen = true,
    .shift_down = 8U,
  };
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_vin_set_rgb_to_yc(0U, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_vin_set_rgb_to_yc(7U, &y_row));

  const ra8_vin_rgb_to_yc_chan_t bad_shift = {
    .r_coeff    = 0U,
    .g_coeff    = 0U,
    .b_coeff    = 0U,
    .add_offset = 0U,
    .enable_hen = false,
    .shift_down = 99U,
  };
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_vin_set_rgb_to_yc(0U, &bad_shift));
  TEST_END("vin set_rgb_to_yc rejects");
}

/* ----------------------------------------------------------------------------
 * Dithering / YUV-444 / IM tests
  *
  * @par MC/DC:
  * (no compound decisions in this test -- exercises the public-API
  * happy path / error-rejection contract; no `&&` or `||` in the
  * code under test that this case touches)
 * --------------------------------------------------------------------------*/

static void test_set_dithering(void)
{
  TEST_BEGIN("vin set_dithering");
  prep();
  const ra8_vin_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vin_init(&cfg));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_vin_set_dithering((uint8_t)k_ra8_vin_dc_ordered_2, true));
  const uint32_t mc = *ra8_vin_reg32(k_ra8_vin_off_mc);
  TEST_ASSERT_EQ(((uint32_t)k_ra8_vin_dc_ordered_2 << 14), (mc & (uint32_t)k_ra8_vin_mc_dc));
  TEST_ASSERT((mc & (uint32_t)k_ra8_vin_mc_dc2) != 0U);

  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_vin_set_dithering(7U, false));
  TEST_END("vin set_dithering");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_set_yuv444_mode(void)
{
  TEST_BEGIN("vin set_yuv444_mode");
  prep();
  const ra8_vin_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vin_init(&cfg));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_vin_set_yuv444_mode((uint8_t)k_ra8_vin_yuv444_interpolate));
  TEST_ASSERT(((*ra8_vin_reg32(k_ra8_vin_off_mc)) & (uint32_t)k_ra8_vin_mc_yuv444) != 0U);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_vin_set_yuv444_mode((uint8_t)k_ra8_vin_yuv444_data_extend));
  TEST_ASSERT_EQ(0U, ((*ra8_vin_reg32(k_ra8_vin_off_mc)) & (uint32_t)k_ra8_vin_mc_yuv444));

  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_vin_set_yuv444_mode(2U));
  TEST_END("vin set_yuv444_mode");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_set_interlace_mode(void)
{
  TEST_BEGIN("vin set_interlace_mode");
  prep();
  const ra8_vin_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vin_init(&cfg));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_vin_set_interlace_mode((uint8_t)k_ra8_vin_im_even_only));
  TEST_ASSERT_EQ(((uint32_t)k_ra8_vin_im_even_only << 3),
                 ((*ra8_vin_reg32(k_ra8_vin_off_mc)) & (uint32_t)k_ra8_vin_mc_im));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_vin_set_interlace_mode(5U));
  TEST_END("vin set_interlace_mode");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_set_data_mode(void)
{
  TEST_BEGIN("vin set_data_mode");
  prep();
  const ra8_vin_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vin_init(&cfg));

  const ra8_vin_data_mode_t m = {
    .conv_mode  = (uint8_t)k_ra8_vin_dtmd_yc_separated,
    .alpha_bit  = true,
    .byte_swap  = true,
    .extend_rgb = true,
    .yc_through = false,
    .y_mode     = (uint8_t)k_ra8_vin_ymode_y10_cbcr,
    .alpha_byte = 0xC3U,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vin_set_data_mode(&m));
  const uint32_t v = *ra8_vin_reg32(k_ra8_vin_off_dmr);
  TEST_ASSERT_EQ(k_ra8_vin_dtmd_yc_separated, (v & (uint32_t)k_ra8_vin_dmr_dtmd));
  TEST_ASSERT((v & (uint32_t)k_ra8_vin_dmr_abit) != 0U);
  TEST_ASSERT((v & (uint32_t)k_ra8_vin_dmr_bpsm) != 0U);
  TEST_ASSERT((v & (uint32_t)k_ra8_vin_dmr_exrgb) != 0U);
  TEST_ASSERT_EQ(0U, (v & (uint32_t)k_ra8_vin_dmr_yc_thr));
  TEST_ASSERT_EQ(((uint32_t)k_ra8_vin_ymode_y10_cbcr << 12), (v & (uint32_t)k_ra8_vin_dmr_ymode));
  TEST_ASSERT_EQ(((uint32_t)0xC3U << 24), (v & (uint32_t)k_ra8_vin_dmr_a8bit));

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_vin_set_data_mode(nullptr));

  ra8_vin_data_mode_t bad = m;
  bad.conv_mode           = k_t_conv_mode_bad;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_vin_set_data_mode(&bad));
  TEST_END("vin set_data_mode");
}

/* ----------------------------------------------------------------------------
 * CSI input / framebuffer tests
  *
  * @par MC/DC:
  * (no compound decisions in this test -- exercises the public-API
  * happy path / error-rejection contract; no `&&` or `||` in the
  * code under test that this case touches)
 * --------------------------------------------------------------------------*/

static void test_set_csi_input(void)
{
  TEST_BEGIN("vin set_csi_input");
  prep();
  const ra8_vin_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vin_init(&cfg));

  const ra8_vin_csi_input_t in = {
    .virtual_channel = 2U,
    .data_type       = (uint8_t)k_ra8_vin_csi_dt_rgb888,
    .zero_extend     = true,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vin_set_csi_input(&in));
  const uint32_t v = *ra8_vin_reg32(k_ra8_vin_off_csi_ifmd);
  TEST_ASSERT_EQ(2U, (v & (uint32_t)k_ra8_vin_csi_ifmd_vc_sel));
  TEST_ASSERT_EQ(((uint32_t)k_ra8_vin_csi_dt_rgb888 << 8), (v & (uint32_t)k_ra8_vin_csi_ifmd_dt));
  TEST_ASSERT((v & (uint32_t)k_ra8_vin_csi_ifmd_des0) != 0U);

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_vin_set_csi_input(nullptr));

  ra8_vin_csi_input_t bad = in;
  bad.virtual_channel     = k_t_vchannel_bad;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_vin_set_csi_input(&bad));
  TEST_END("vin set_csi_input");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_set_field_detect(void)
{
  TEST_BEGIN("vin set_field_detect");
  prep();
  const ra8_vin_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vin_init(&cfg));

  const ra8_vin_field_detect_t d = {
    .enable         = true,
    .even_field_sel = true,
    .even_field_num = (uint8_t)k_ra8_vin_test_field_even_one,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vin_set_field_detect(&d));
  const uint32_t v = *ra8_vin_reg32(k_ra8_vin_off_csifld);
  TEST_ASSERT((v & (uint32_t)k_ra8_vin_csifld_fld_en) != 0U);
  TEST_ASSERT((v & (uint32_t)k_ra8_vin_csifld_fld_sel) != 0U);
  TEST_ASSERT((v & (uint32_t)k_ra8_vin_csifld_fld_num) != 0U);

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_vin_set_field_detect(nullptr));

  ra8_vin_field_detect_t bad = d;
  bad.even_field_num         = k_t_field_num_bad;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_vin_set_field_detect(&bad));
  TEST_END("vin set_field_detect");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_set_framebuffers(void)
{
  TEST_BEGIN("vin set_framebuffers");
  prep();
  const ra8_vin_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vin_init(&cfg));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_vin_set_framebuffers((uint32_t)k_ra8_vin_test_fb_alt, 0U, 0U));
  TEST_ASSERT_EQ(k_ra8_vin_test_fb_alt, *ra8_vin_reg32(k_ra8_vin_off_mb1));
  /* MB2/MB3 unchanged. */
  TEST_ASSERT_EQ(k_ra8_vin_test_fb2, *ra8_vin_reg32(k_ra8_vin_off_mb2));

  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_vin_set_framebuffers(0U, 0U, 0U));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_vin_set_framebuffers(0x68000001U, 0U, 0U));
  TEST_END("vin set_framebuffers");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_set_uv_offset(void)
{
  TEST_BEGIN("vin set_uv_offset");
  prep();
  const ra8_vin_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vin_init(&cfg));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_vin_set_uv_offset((uint32_t)k_ra8_vin_test_uv_off));
  TEST_ASSERT_EQ(k_ra8_vin_test_uv_off, *ra8_vin_reg32(k_ra8_vin_off_uvaof));

  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_vin_set_uv_offset((uint32_t)k_ra8_vin_test_uv_bad));
  TEST_END("vin set_uv_offset");
}

/* ----------------------------------------------------------------------------
 * Status / IRQ tests
  *
  * @par MC/DC:
  * (no compound decisions in this test -- exercises the public-API
  * happy path / error-rejection contract; no `&&` or `||` in the
  * code under test that this case touches)
 * --------------------------------------------------------------------------*/

static void test_status_get_clear(void)
{
  TEST_BEGIN("vin status get + clear");
  prep();

  const ra8_vin_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vin_init(&cfg));

  /* Inject a synthetic INTS value with both W1C and non-W1C bits. */
  *ra8_vin_reg32(k_ra8_vin_off_ints) = (uint32_t)k_ra8_vin_int_fos | (uint32_t)k_ra8_vin_int_eof;
  uint32_t value                     = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vin_get_status(&value));
  TEST_ASSERT((value & (uint32_t)k_ra8_vin_int_fos) != 0U);
  TEST_ASSERT((value & (uint32_t)k_ra8_vin_int_eof) != 0U);

  /* Clearing only writes the W1C subset. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vin_clear_status((uint32_t)k_ra8_vin_int_fos));
  const uint32_t after = *ra8_vin_reg32(k_ra8_vin_off_ints);
  TEST_ASSERT_EQ(k_ra8_vin_int_fos, after);

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_vin_get_status(nullptr));
  TEST_END("vin status get + clear");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_module_status(void)
{
  TEST_BEGIN("vin module status");
  prep();
  const ra8_vin_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vin_init(&cfg));

  /* Inject CA = 1, FBS = 2, FMS = 1. */
  const uint32_t synthetic         = (uint32_t)k_ra8_vin_ms_ca | ((uint32_t)2U << 3) |
                                     ((uint32_t)1U << 19) | (uint32_t)k_ra8_vin_ms_av;
  *ra8_vin_reg32(k_ra8_vin_off_ms) = synthetic;

  ra8_vin_module_status_t s = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vin_get_module_status(&s));
  TEST_ASSERT(s.capture_active);
  TEST_ASSERT(s.active_video);
  TEST_ASSERT_EQ(2U, s.frame_buffer_id);
  TEST_ASSERT_EQ(1U, s.latest_frame_buffer);

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_vin_get_module_status(nullptr));

  uint8_t fbs = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vin_get_active_buffer(&fbs));
  TEST_ASSERT_EQ(2U, fbs);
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_vin_get_active_buffer(nullptr));
  TEST_END("vin module status");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_line_count(void)
{
  TEST_BEGIN("vin line count");
  prep();
  const ra8_vin_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vin_init(&cfg));

  *ra8_vin_reg32(k_ra8_vin_off_lc) = k_t_line_count_reg;
  uint16_t lc                      = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vin_get_line_count(&lc));
  TEST_ASSERT_EQ(0x123U, lc);
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_vin_get_line_count(nullptr));
  TEST_END("vin line count");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_set_interrupt_enable(void)
{
  TEST_BEGIN("vin set_interrupt_enable");
  prep();
  const ra8_vin_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vin_init(&cfg));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_vin_set_interrupt_enable((uint32_t)k_ra8_vin_int_axi_err));
  TEST_ASSERT_EQ(k_ra8_vin_int_axi_err, *ra8_vin_reg32(k_ra8_vin_off_ie));
  TEST_END("vin set_interrupt_enable");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_set_scanline_compare(void)
{
  TEST_BEGIN("vin set_scanline_compare");
  prep();
  const ra8_vin_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vin_init(&cfg));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_vin_set_scanline_compare(720U));
  TEST_ASSERT_EQ(720U, *ra8_vin_reg32(k_ra8_vin_off_si));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_vin_set_scanline_compare(5000U));
  TEST_END("vin set_scanline_compare");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_attach_and_dispatch(void)
{
  TEST_BEGIN("vin attach + dispatch");
  prep();

  const ra8_vin_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vin_init(&cfg));
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_vin_attach_handler(stub_vin_cb, (void*)(uintptr_t)k_ra8_vin_test_ctx));

  /* Drop a status value into INTS, then dispatch. */
  *ra8_vin_reg32(k_ra8_vin_off_ints) = (uint32_t)k_ra8_vin_test_status;
  ra8_vin_dispatch();
  TEST_ASSERT_EQ(1U, s_vin_cb_count);
  TEST_ASSERT_EQ(k_ra8_vin_test_status, s_vin_cb_last_mask);
  TEST_ASSERT_EQ(k_ra8_vin_test_ctx, (uintptr_t)s_vin_cb_last_ctx);

  /* With callback detached, dispatch must still W1C without firing. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vin_attach_handler(nullptr, nullptr));
  *ra8_vin_reg32(k_ra8_vin_off_ints) = (uint32_t)k_ra8_vin_int_fos;
  ra8_vin_dispatch();
  TEST_ASSERT_EQ(1U, s_vin_cb_count);
  TEST_END("vin attach + dispatch");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_power_transition(void)
{
  TEST_BEGIN("vin power transition");
  prep();

  const ra8_vin_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vin_init(&cfg));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vin_capture_arm(k_ra8_vin_capture_continuous));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vin_enter_stop());

  const uint32_t mc = *ra8_vin_reg32(k_ra8_vin_off_mc);
  const uint32_t fc = *ra8_vin_reg32(k_ra8_vin_off_fc);
  TEST_ASSERT_EQ(0U, (mc & (uint32_t)k_ra8_vin_mc_me));
  TEST_ASSERT_EQ(0U, (fc & (uint32_t)k_ra8_vin_fc_cc));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_vin_exit_stop());
  TEST_END("vin power transition");
}

/**
 * @var s_test_roster
 * @brief Fixed-order roster of every test case in this translation unit.
 *
 * @details
 * main() walks this table instead of naming each case, so its size does not
 * grow with the number of tests and adding a case is a one-line edit.
 *
 * @note Order is significant: cases run top to bottom, exactly as before.
 */
static void (*const s_test_roster[])(void) = {
  test_set_preclip,           test_set_uds_scale,        test_set_uds_passband,
  test_set_uds_clip,          test_set_uds_ctrl,         test_enable_scaling,
  test_lut_program,           test_set_yc_to_rgb,        test_set_rgb_to_yc,
  test_set_rgb_to_yc_rejects, test_set_dithering,        test_set_yuv444_mode,
  test_set_interlace_mode,    test_set_data_mode,        test_set_csi_input,
  test_set_field_detect,      test_set_framebuffers,     test_set_uv_offset,
  test_status_get_clear,      test_module_status,        test_line_count,
  test_set_interrupt_enable,  test_set_scanline_compare, test_attach_and_dispatch,
  test_power_transition,
};

int32_t main(void)
{
  for (size_t i = 0U; i < (sizeof s_test_roster / sizeof s_test_roster[0]); ++i) {
    s_test_roster[i]();
  }
  (void)fprintf(stderr, "[OK ] test_ra8_vin_config.c\n");
  return 0;
}
