/**
 * @file test_ra_mipi_csi.c
 * @brief Unit tests for the MIPI CSI-2 receiver HAL driver
 *
 * @details
 * Exercises ra_mipi_csi.c against the host-side simulated MMIO
 * (``ra_sim_mmap``). Covers every public entry point with happy +
 * bad-arg + edge cases, plus full ISR-dispatch paths for RX, DL,
 * VC, PM, and GST vectors.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8d2_mipi_csi_regs.h"
#include "ra_err.h"
#include "ra_mipi_csi.h"
#include "ra_mstp.h"
#include "ra_sim_mmap.h"
#include "unity_minimal.h"

/**
 * @enum ra_mipi_csi_test_const_t
 * @brief Constants used across multiple test cases.
 */
typedef enum : uint32_t {
  k_ra_mipi_csi_test_frrclk         = 3U, /**< vclk/hsclk = 125/90 example.    */
  k_ra_mipi_csi_test_frrskw         = 5U, /**< vclk/hsclk = 125/90 example.    */
  k_ra_mipi_csi_test_ractdet_marker = 0x00020000UL,
  k_ra_mipi_csi_test_ctx_marker     = 0xCAFEU,
  k_ra_mipi_csi_test_dl_marker      = 0xC0DEU,
  k_ra_mipi_csi_test_vc_marker      = 0xBEEFU,
  k_ra_mipi_csi_test_pm_marker      = 0xFACEU,
  k_ra_mipi_csi_test_gst_marker     = 0xBABEU,
  k_ra_mipi_csi_test_mcg_value      = 0x00100A02UL, /**< VER=2 SDLN=0xA STAGES=0x10. */
  k_ra_mipi_csi_test_dt_low_value   = 0xC0008010UL, /**< YUV422_8/10 + GSP1 + EOT.   */
  k_ra_mipi_csi_test_dt_high_value  = 0x00000010UL, /**< RGB888.                     */
  k_ra_mipi_csi_test_threshold      = 7U,
  k_ra_mipi_csi_test_pkt_value      = 0x0A083412UL, /**< VC=0xA DT=0x08 SPDT=0x3412. */
  k_ra_mipi_csi_test_pmst_marker    = 0x00C00055UL, /**< Lower 8 W1C-able + RO bits. */
} ra_mipi_csi_test_const_t;

/**
 * @var s_cb_count
 * @brief Number of times the test receive callback has fired since prep.
 */
static uint32_t s_cb_count;

/**
 * @var s_cb_last_rxst
 * @brief Most recent RXST snapshot delivered to the test callback.
 */
static uint32_t s_cb_last_rxst;

/**
 * @var s_cb_last_ctx
 * @brief Most recent context pointer delivered to the test callback.
 */
static void* s_cb_last_ctx;

/**
 * @var s_dl_calls
 * @brief Per-lane call count for the data-lane callback.
 */
static uint32_t s_dl_calls[2];

/**
 * @var s_dl_last_mask
 * @brief Last DLST value forwarded to the data-lane callback per lane.
 */
static uint32_t s_dl_last_mask[2];

/**
 * @var s_vc_calls
 * @brief Per-VC call count for the VC callback (index 16 = generic).
 */
static uint32_t s_vc_calls[17];

/**
 * @var s_vc_last_mask
 * @brief Last VCST value per VC.
 */
static uint32_t s_vc_last_mask[17];

/**
 * @var s_pm_calls
 * @brief Number of PM dispatch firings.
 */
static uint32_t s_pm_calls;

/**
 * @var s_pm_last_mask
 * @brief Last PMST snapshot delivered.
 */
static uint32_t s_pm_last_mask;

/**
 * @var s_gst_calls
 * @brief Number of GST dispatch firings.
 */
static uint32_t s_gst_calls;

/**
 * @var s_gst_last_mask
 * @brief Last GSST snapshot delivered.
 */
static uint32_t s_gst_last_mask;

/**
 * @brief Test receive callback that records arguments for assertions.
 */
static void stub_event_cb(void* ctx, uint32_t rxst)
{
  ++s_cb_count;
  s_cb_last_rxst = rxst;
  s_cb_last_ctx  = ctx;
}

/**
 * @brief Test data-lane callback.
 */
static void stub_dl_cb(void* ctx, uint8_t lane, uint32_t mask)
{
  (void)ctx;
  if (lane < 2U) {
    ++s_dl_calls[lane];
    s_dl_last_mask[lane] = mask;
  }
}

/**
 * @brief Test VC callback.
 */
static void stub_vc_cb(void* ctx, uint8_t vc, uint32_t mask)
{
  (void)ctx;
  uint8_t idx = (vc == 0xFFU) ? 16U : vc;
  if (idx < 17U) {
    ++s_vc_calls[idx];
    s_vc_last_mask[idx] = mask;
  }
}

/**
 * @brief Test PM callback.
 */
static void stub_pm_cb(void* ctx, uint32_t mask)
{
  (void)ctx;
  ++s_pm_calls;
  s_pm_last_mask = mask;
}

/**
 * @brief Test GST callback.
 */
static void stub_gst_cb(void* ctx, uint32_t mask)
{
  (void)ctx;
  ++s_gst_calls;
  s_gst_last_mask = mask;
}

/**
 * @brief Reset sim mmap + ra_mstp + callback statistics.
 */
static void prep(void)
{
  ra_sim_mmap_reset();
  (void)ra_mstp_init();
  s_cb_count     = 0U;
  s_cb_last_rxst = 0U;
  s_cb_last_ctx  = nullptr;
  for (uint8_t i = 0U; i < 2U; ++i) {
    s_dl_calls[i]     = 0U;
    s_dl_last_mask[i] = 0U;
  }
  for (uint8_t i = 0U; i < 17U; ++i) {
    s_vc_calls[i]     = 0U;
    s_vc_last_mask[i] = 0U;
  }
  s_pm_calls      = 0U;
  s_pm_last_mask  = 0U;
  s_gst_calls     = 0U;
  s_gst_last_mask = 0U;
}

/**
 * @brief Build a default config (2 lanes, GRMD on, EOTPEN off, all IRQs off).
 */
static ra_mipi_csi_config_t make_cfg(void)
{
  const ra_mipi_csi_config_t cfg = {
    .lanes              = k_ra_mipi_csi_lanes_2,
    .generic_rule       = true,
    .eccv13             = false,
    .lfsren             = false,
    .zlmd               = false,
    .edmd               = false,
    .rvmd               = false,
    .frrclk             = (uint16_t)k_ra_mipi_csi_test_frrclk,
    .frrskw             = (uint16_t)k_ra_mipi_csi_test_frrskw,
    .epd_enable         = false,
    .epd_option_2       = false,
    .epd_long_spacer    = 0U,
    .epd_short_spacer   = 0U,
    .vlsien             = k_ra_mipi_csi_vlsien_fixed,
    .eotp_enable        = false,
    .dt_low_mask        = 0U,
    .dt_high_mask       = 0U,
    .rx_irq_mask        = 0U,
    .dl_irq_mask        = {0U, 0U},
    .vc_irq_mask        = {},
    .pm_irq_mask        = 0U,
    .short_irq_mask     = 0U,
    .short_threshold    = 0U,
    .short_store_enable = false,
  };
  return cfg;
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_happy(void)
{
  TEST_BEGIN("mipi_csi init happy");
  prep();

  const ra_mipi_csi_config_t cfg = make_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_mipi_csi_init(&cfg));

  /* MCT0: VDLN = 2, GRMD set. */
  const uint32_t mct0 = *ra_mipi_csi_reg32(k_ra_mipi_csi_off_mct0);
  TEST_ASSERT_EQ((int32_t)k_ra_mipi_csi_lanes_2,
                 (int32_t)(mct0 & (uint32_t)k_ra_mipi_csi_mct0_vdln_mask));
  TEST_ASSERT((mct0 & (uint32_t)k_ra_mipi_csi_mct0_grmd_mask) != 0UL);

  /* MCT2: FRRSKW @ [24:16] | FRRCLK @ [8:0]. */
  const uint32_t mct2     = *ra_mipi_csi_reg32(k_ra_mipi_csi_off_mct2);
  const uint32_t expected = (((uint32_t)k_ra_mipi_csi_test_frrskw & 0x1FFUL) << 16U) |
                            ((uint32_t)k_ra_mipi_csi_test_frrclk & 0x1FFUL);
  TEST_ASSERT_EQ((int64_t)expected, (int64_t)mct2);

  /* MCT3 must remain 0 (RXEN cleared post-init). */
  TEST_ASSERT_EQ((int32_t)0, (int32_t)*ra_mipi_csi_reg32(k_ra_mipi_csi_off_mct3));
  TEST_END("mipi_csi init happy");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_ecc_and_frame_bits(void)
{
  TEST_BEGIN("mipi_csi init ecc + frame bits");
  prep();

  ra_mipi_csi_config_t cfg = make_cfg();
  cfg.eccv13               = true;
  cfg.lfsren               = true;
  cfg.zlmd                 = true;
  cfg.edmd                 = true;
  cfg.rvmd                 = true;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_mipi_csi_init(&cfg));

  const uint32_t mct0 = *ra_mipi_csi_reg32(k_ra_mipi_csi_off_mct0);
  TEST_ASSERT((mct0 & (uint32_t)k_ra_mipi_csi_mct0_eccv13_mask) != 0UL);
  TEST_ASSERT((mct0 & (uint32_t)k_ra_mipi_csi_mct0_lfsren_mask) != 0UL);
  TEST_ASSERT((mct0 & (uint32_t)k_ra_mipi_csi_mct0_zlmd_mask) != 0UL);
  TEST_ASSERT((mct0 & (uint32_t)k_ra_mipi_csi_mct0_edmd_mask) != 0UL);
  TEST_ASSERT((mct0 & (uint32_t)k_ra_mipi_csi_mct0_rvmd_mask) != 0UL);
  TEST_END("mipi_csi init ecc + frame bits");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_dt_and_short_packet(void)
{
  TEST_BEGIN("mipi_csi init dt filter + short packet");
  prep();

  ra_mipi_csi_config_t cfg = make_cfg();
  cfg.dt_low_mask          = (uint32_t)k_ra_mipi_csi_test_dt_low_value;
  cfg.dt_high_mask         = (uint32_t)k_ra_mipi_csi_test_dt_high_value;
  cfg.short_threshold      = (uint8_t)k_ra_mipi_csi_test_threshold;
  cfg.short_store_enable   = true;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_mipi_csi_init(&cfg));

  TEST_ASSERT_EQ((int64_t)k_ra_mipi_csi_test_dt_low_value,
                 (int64_t)*ra_mipi_csi_reg32(k_ra_mipi_csi_off_dtel));
  TEST_ASSERT_EQ((int64_t)k_ra_mipi_csi_test_dt_high_value,
                 (int64_t)*ra_mipi_csi_reg32(k_ra_mipi_csi_off_dteh));

  const uint32_t gsct = *ra_mipi_csi_reg32(k_ra_mipi_csi_off_gsct);
  TEST_ASSERT_EQ((int32_t)k_ra_mipi_csi_test_threshold,
                 (int32_t)(gsct & (uint32_t)k_ra_mipi_csi_gsct_shth_mask));
  TEST_ASSERT((gsct & (uint32_t)k_ra_mipi_csi_gsct_gfif_mask) != 0UL);
  TEST_END("mipi_csi init dt filter + short packet");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_epct_emct(void)
{
  TEST_BEGIN("mipi_csi init epct + emct");
  prep();

  ra_mipi_csi_config_t cfg = make_cfg();
  cfg.epd_enable           = true;
  cfg.epd_option_2         = true;
  cfg.epd_long_spacer      = 0x100U;
  cfg.epd_short_spacer     = 0x200U;
  cfg.vlsien               = k_ra_mipi_csi_vlsien_x2;
  cfg.eotp_enable          = true;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_mipi_csi_init(&cfg));

  const uint32_t epct = *ra_mipi_csi_reg32(k_ra_mipi_csi_off_epct);
  TEST_ASSERT((epct & (uint32_t)k_ra_mipi_csi_epct_epden_mask) != 0UL);
  TEST_ASSERT((epct & (uint32_t)k_ra_mipi_csi_epct_epdop_mask) != 0UL);
  TEST_ASSERT_EQ((int32_t)0x100, (int32_t)(epct & (uint32_t)k_ra_mipi_csi_epct_slp_mask));
  TEST_ASSERT_EQ((int64_t)0x02000000ULL, (int64_t)(epct & (uint32_t)k_ra_mipi_csi_epct_ssp_mask));

  const uint32_t emct = *ra_mipi_csi_reg32(k_ra_mipi_csi_off_emct);
  TEST_ASSERT_EQ(
    (int32_t)((uint32_t)k_ra_mipi_csi_vlsien_x2 << (uint32_t)k_ra_mipi_csi_emct_vlsien_shift),
    (int32_t)(emct & (uint32_t)k_ra_mipi_csi_emct_vlsien_mask));
  TEST_ASSERT((emct & (uint32_t)k_ra_mipi_csi_emct_eotpen_mask) != 0UL);
  TEST_END("mipi_csi init epct + emct");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_irq_masks(void)
{
  TEST_BEGIN("mipi_csi init programs all irq masks");
  prep();

  ra_mipi_csi_config_t cfg = make_cfg();
  cfg.rx_irq_mask          = (uint32_t)k_ra_mipi_csi_rxie_ractdete_mask;
  cfg.dl_irq_mask[0]       = (uint32_t)k_ra_mipi_csi_dlie_eshe_mask;
  cfg.dl_irq_mask[1]       = (uint32_t)k_ra_mipi_csi_dlie_esse_mask;
  for (uint8_t vc = 0U; vc < 16U; ++vc) {
    cfg.vc_irq_mask[vc] = ((uint32_t)1U << vc);
  }
  cfg.pm_irq_mask    = (uint32_t)k_ra_mipi_csi_pmie_dsxe_mask;
  cfg.short_irq_mask = (uint32_t)k_ra_mipi_csi_gsie_gnee_mask;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_mipi_csi_init(&cfg));

  TEST_ASSERT_EQ((int64_t)cfg.rx_irq_mask, (int64_t)*ra_mipi_csi_reg32(k_ra_mipi_csi_off_rxie));
  TEST_ASSERT_EQ((int64_t)cfg.dl_irq_mask[0], (int64_t)*ra_mipi_csi_reg32(k_ra_mipi_csi_off_dlie0));
  TEST_ASSERT_EQ((int64_t)cfg.dl_irq_mask[1], (int64_t)*ra_mipi_csi_reg32(k_ra_mipi_csi_off_dlie1));
  for (uint8_t vc = 0U; vc < 16U; ++vc) {
    const ra_mipi_csi_off_t off = ra_mipi_csi_vc_off(k_ra_mipi_csi_off_vcie0, vc);
    TEST_ASSERT_EQ((int64_t)((uint32_t)1U << vc), (int64_t)*ra_mipi_csi_reg32(off));
  }
  TEST_ASSERT_EQ((int64_t)cfg.pm_irq_mask, (int64_t)*ra_mipi_csi_reg32(k_ra_mipi_csi_off_pmie));
  TEST_ASSERT_EQ((int64_t)cfg.short_irq_mask, (int64_t)*ra_mipi_csi_reg32(k_ra_mipi_csi_off_gsie));
  TEST_END("mipi_csi init programs all irq masks");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_null_cfg(void)
{
  TEST_BEGIN("mipi_csi init null cfg");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_mipi_csi_init(nullptr));
  TEST_END("mipi_csi init null cfg");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_bad_lanes(void)
{
  TEST_BEGIN("mipi_csi init bad lanes");
  prep();

  ra_mipi_csi_config_t cfg = make_cfg();
  cfg.lanes                = (ra_mipi_csi_lanes_t)0U;
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_mipi_csi_init(&cfg));

  cfg.lanes = (ra_mipi_csi_lanes_t)3U;
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_mipi_csi_init(&cfg));
  TEST_END("mipi_csi init bad lanes");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_bad_args(void)
{
  TEST_BEGIN("mipi_csi init misc bad args");
  prep();

  ra_mipi_csi_config_t cfg = make_cfg();
  cfg.vlsien               = (ra_mipi_csi_vlsien_t)4U;
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_mipi_csi_init(&cfg));

  cfg                 = make_cfg();
  cfg.epd_long_spacer = 0x8000U; /* 16-bit, overflows 15-bit field. */
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_mipi_csi_init(&cfg));

  cfg                  = make_cfg();
  cfg.epd_short_spacer = 0x8000U;
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_mipi_csi_init(&cfg));

  cfg                 = make_cfg();
  cfg.short_threshold = 0x10U;
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_mipi_csi_init(&cfg));
  TEST_END("mipi_csi init misc bad args");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_start_then_stop(void)
{
  TEST_BEGIN("mipi_csi start + stop");
  prep();

  const ra_mipi_csi_config_t cfg = make_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_mipi_csi_init(&cfg));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_mipi_csi_start_receive());

  TEST_ASSERT_EQ(
    (int32_t)k_ra_mipi_csi_mct3_rxen_mask,
    (int32_t)(*ra_mipi_csi_reg32(k_ra_mipi_csi_off_mct3) & (uint32_t)k_ra_mipi_csi_mct3_rxen_mask));

  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_state, (int32_t)ra_mipi_csi_start_receive());

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_mipi_csi_stop_receive());
  TEST_ASSERT_EQ((int32_t)0, (int32_t)*ra_mipi_csi_reg32(k_ra_mipi_csi_off_mct3));
  TEST_ASSERT_EQ((int32_t)k_ra_mipi_csi_rtct_vsrst_mask,
                 (int32_t)(*ra_mipi_csi_reg32(k_ra_mipi_csi_off_rtct) &
                           (uint32_t)k_ra_mipi_csi_rtct_vsrst_mask));
  TEST_END("mipi_csi start + stop");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_reset(void)
{
  TEST_BEGIN("mipi_csi explicit reset");
  prep();
  const ra_mipi_csi_config_t cfg = make_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_mipi_csi_init(&cfg));

  /* RTST defaults to 0 in sim, so reset should succeed quickly. */
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_mipi_csi_reset());
  TEST_ASSERT_EQ((int32_t)k_ra_mipi_csi_rtct_vsrst_mask,
                 (int32_t)(*ra_mipi_csi_reg32(k_ra_mipi_csi_off_rtct) &
                           (uint32_t)k_ra_mipi_csi_rtct_vsrst_mask));
  TEST_END("mipi_csi explicit reset");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_status_get_clear(void)
{
  TEST_BEGIN("mipi_csi status get + clear");
  prep();

  const ra_mipi_csi_config_t cfg = make_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_mipi_csi_init(&cfg));

  *ra_mipi_csi_reg32(k_ra_mipi_csi_off_rxst) = (uint32_t)k_ra_mipi_csi_test_ractdet_marker;

  uint32_t mask = 0U;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_mipi_csi_get_status(&mask));
  TEST_ASSERT_EQ((int64_t)k_ra_mipi_csi_test_ractdet_marker, (int64_t)mask);

  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_mipi_csi_clear_status((uint32_t)k_ra_mipi_csi_rxsc_ractdetc_mask));
  TEST_ASSERT_EQ((int64_t)k_ra_mipi_csi_rxsc_ractdetc_mask,
                 (int64_t)*ra_mipi_csi_reg32(k_ra_mipi_csi_off_rxsc));

  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_mipi_csi_get_status(nullptr));
  TEST_END("mipi_csi status get + clear");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_set_rx_irq_enable(void)
{
  TEST_BEGIN("mipi_csi set rx irq enable");
  prep();
  const ra_mipi_csi_config_t cfg = make_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_mipi_csi_init(&cfg));

  TEST_ASSERT_EQ(
    (int32_t)k_ra_ok,
    (int32_t)ra_mipi_csi_set_rx_irq_enable((uint32_t)k_ra_mipi_csi_rxie_ractdete_mask));
  TEST_ASSERT_EQ((int64_t)k_ra_mipi_csi_rxie_ractdete_mask,
                 (int64_t)*ra_mipi_csi_reg32(k_ra_mipi_csi_off_rxie));
  TEST_END("mipi_csi set rx irq enable");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_get_module_irq_status(void)
{
  TEST_BEGIN("mipi_csi get module irq status");
  prep();
  const ra_mipi_csi_config_t cfg = make_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_mipi_csi_init(&cfg));

  *ra_mipi_csi_reg32(k_ra_mipi_csi_off_mist) = (uint32_t)k_ra_mipi_csi_mist_rxs_mask;
  uint32_t out                               = 0U;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_mipi_csi_get_module_irq_status(&out));
  TEST_ASSERT_EQ((int64_t)k_ra_mipi_csi_mist_rxs_mask, (int64_t)out);
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_mipi_csi_get_module_irq_status(nullptr));
  TEST_END("mipi_csi get module irq status");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_get_module_info(void)
{
  TEST_BEGIN("mipi_csi get module info");
  prep();
  const ra_mipi_csi_config_t cfg = make_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_mipi_csi_init(&cfg));

  *ra_mipi_csi_reg32(k_ra_mipi_csi_off_mcg) = (uint32_t)k_ra_mipi_csi_test_mcg_value;

  ra_mipi_csi_module_info_t info = {};
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_mipi_csi_get_module_info(&info));
  TEST_ASSERT_EQ((int32_t)2, (int32_t)info.version);
  TEST_ASSERT_EQ((int32_t)0xA, (int32_t)info.lanes_max);
  TEST_ASSERT_EQ((int32_t)0x10, (int32_t)info.fifo_stages);
  TEST_ASSERT_EQ((int64_t)k_ra_mipi_csi_test_mcg_value, (int64_t)info.raw);

  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_mipi_csi_get_module_info(nullptr));
  TEST_END("mipi_csi get module info");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_set_data_type_filter(void)
{
  TEST_BEGIN("mipi_csi set data type filter");
  prep();
  const ra_mipi_csi_config_t cfg = make_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_mipi_csi_init(&cfg));

  TEST_ASSERT_EQ(
    (int32_t)k_ra_ok,
    (int32_t)ra_mipi_csi_set_data_type_filter((uint32_t)k_ra_mipi_csi_test_dt_low_value,
                                              (uint32_t)k_ra_mipi_csi_test_dt_high_value));
  TEST_ASSERT_EQ((int64_t)k_ra_mipi_csi_test_dt_low_value,
                 (int64_t)*ra_mipi_csi_reg32(k_ra_mipi_csi_off_dtel));
  TEST_ASSERT_EQ((int64_t)k_ra_mipi_csi_test_dt_high_value,
                 (int64_t)*ra_mipi_csi_reg32(k_ra_mipi_csi_off_dteh));
  TEST_END("mipi_csi set data type filter");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_set_ecc_mode(void)
{
  TEST_BEGIN("mipi_csi set ecc mode");
  prep();
  const ra_mipi_csi_config_t cfg = make_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_mipi_csi_init(&cfg));

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_mipi_csi_set_ecc_mode(true, true));
  uint32_t mct0 = *ra_mipi_csi_reg32(k_ra_mipi_csi_off_mct0);
  TEST_ASSERT((mct0 & (uint32_t)k_ra_mipi_csi_mct0_eccv13_mask) != 0UL);
  TEST_ASSERT((mct0 & (uint32_t)k_ra_mipi_csi_mct0_lfsren_mask) != 0UL);

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_mipi_csi_set_ecc_mode(false, false));
  mct0 = *ra_mipi_csi_reg32(k_ra_mipi_csi_off_mct0);
  TEST_ASSERT((mct0 & (uint32_t)k_ra_mipi_csi_mct0_eccv13_mask) == 0UL);
  TEST_ASSERT((mct0 & (uint32_t)k_ra_mipi_csi_mct0_lfsren_mask) == 0UL);

  /* Reject while running. */
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_mipi_csi_start_receive());
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_state, (int32_t)ra_mipi_csi_set_ecc_mode(true, true));
  TEST_END("mipi_csi set ecc mode");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_set_frame_error_mode(void)
{
  TEST_BEGIN("mipi_csi set frame error mode");
  prep();
  const ra_mipi_csi_config_t cfg = make_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_mipi_csi_init(&cfg));

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_mipi_csi_set_frame_error_mode(true, true, true));
  const uint32_t mct0 = *ra_mipi_csi_reg32(k_ra_mipi_csi_off_mct0);
  TEST_ASSERT((mct0 & (uint32_t)k_ra_mipi_csi_mct0_zlmd_mask) != 0UL);
  TEST_ASSERT((mct0 & (uint32_t)k_ra_mipi_csi_mct0_edmd_mask) != 0UL);
  TEST_ASSERT((mct0 & (uint32_t)k_ra_mipi_csi_mct0_rvmd_mask) != 0UL);

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_mipi_csi_start_receive());
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_state,
                 (int32_t)ra_mipi_csi_set_frame_error_mode(false, false, false));
  TEST_END("mipi_csi set frame error mode");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_set_epd(void)
{
  TEST_BEGIN("mipi_csi set epd");
  prep();
  const ra_mipi_csi_config_t cfg = make_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_mipi_csi_init(&cfg));

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_mipi_csi_set_epd(true, true, 0x123U, 0x456U));
  const uint32_t epct = *ra_mipi_csi_reg32(k_ra_mipi_csi_off_epct);
  TEST_ASSERT((epct & (uint32_t)k_ra_mipi_csi_epct_epden_mask) != 0UL);
  TEST_ASSERT((epct & (uint32_t)k_ra_mipi_csi_epct_epdop_mask) != 0UL);
  TEST_ASSERT_EQ((int32_t)0x123, (int32_t)(epct & (uint32_t)k_ra_mipi_csi_epct_slp_mask));

  /* Reject bad spacers. */
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_mipi_csi_set_epd(true, true, 0x8000U, 0U));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_mipi_csi_set_epd(true, true, 0U, 0x8000U));
  TEST_END("mipi_csi set epd");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_set_lrte(void)
{
  TEST_BEGIN("mipi_csi set lrte");
  prep();
  const ra_mipi_csi_config_t cfg = make_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_mipi_csi_init(&cfg));

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_mipi_csi_set_lrte(k_ra_mipi_csi_vlsien_x4, true));
  const uint32_t emct = *ra_mipi_csi_reg32(k_ra_mipi_csi_off_emct);
  TEST_ASSERT((emct & (uint32_t)k_ra_mipi_csi_emct_eotpen_mask) != 0UL);
  TEST_ASSERT_EQ(
    (int32_t)((uint32_t)k_ra_mipi_csi_vlsien_x4 << (uint32_t)k_ra_mipi_csi_emct_vlsien_shift),
    (int32_t)(emct & (uint32_t)k_ra_mipi_csi_emct_vlsien_mask));

  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_mipi_csi_set_lrte((ra_mipi_csi_vlsien_t)4U, false));
  TEST_END("mipi_csi set lrte");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_dl_status_paths(void)
{
  TEST_BEGIN("mipi_csi dl status get/clear/irq");
  prep();
  const ra_mipi_csi_config_t cfg = make_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_mipi_csi_init(&cfg));

  *ra_mipi_csi_reg32(k_ra_mipi_csi_off_dlst0) = (uint32_t)k_ra_mipi_csi_dlst_esh_mask;
  *ra_mipi_csi_reg32(k_ra_mipi_csi_off_dlst1) = (uint32_t)k_ra_mipi_csi_dlst_ess_mask;

  uint32_t v = 0U;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_mipi_csi_dl_get_status(0U, &v));
  TEST_ASSERT_EQ((int64_t)k_ra_mipi_csi_dlst_esh_mask, (int64_t)v);
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_mipi_csi_dl_get_status(1U, &v));
  TEST_ASSERT_EQ((int64_t)k_ra_mipi_csi_dlst_ess_mask, (int64_t)v);

  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_mipi_csi_dl_clear_status(0U, (uint32_t)k_ra_mipi_csi_dlsc_eshc_mask));
  TEST_ASSERT_EQ((int64_t)k_ra_mipi_csi_dlsc_eshc_mask,
                 (int64_t)*ra_mipi_csi_reg32(k_ra_mipi_csi_off_dlsc0));

  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_mipi_csi_dl_set_irq_enable(0U, (uint32_t)k_ra_mipi_csi_dlie_all_mask));
  TEST_ASSERT_EQ((int64_t)k_ra_mipi_csi_dlie_all_mask,
                 (int64_t)*ra_mipi_csi_reg32(k_ra_mipi_csi_off_dlie0));

  /* Bad lane index. */
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_mipi_csi_dl_get_status(2U, &v));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_mipi_csi_dl_clear_status(2U, 0U));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_mipi_csi_dl_set_irq_enable(2U, 0U));
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_mipi_csi_dl_get_status(0U, nullptr));
  TEST_END("mipi_csi dl status get/clear/irq");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_vc_status_paths(void)
{
  TEST_BEGIN("mipi_csi vc status get/clear/irq");
  prep();
  const ra_mipi_csi_config_t cfg = make_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_mipi_csi_init(&cfg));

  for (uint8_t vc = 0U; vc < 16U; ++vc) {
    const ra_mipi_csi_off_t st_off = ra_mipi_csi_vc_off(k_ra_mipi_csi_off_vcst0, vc);
    *ra_mipi_csi_reg32(st_off)     = ((uint32_t)1U << vc);

    uint32_t v = 0U;
    TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_mipi_csi_vc_get_status(vc, &v));
    TEST_ASSERT_EQ((int64_t)((uint32_t)1U << vc), (int64_t)v);

    TEST_ASSERT_EQ((int32_t)k_ra_ok,
                   (int32_t)ra_mipi_csi_vc_clear_status(vc, ((uint32_t)1U << vc)));
    const ra_mipi_csi_off_t clr_off = ra_mipi_csi_vc_off(k_ra_mipi_csi_off_vcsc0, vc);
    TEST_ASSERT_EQ((int64_t)((uint32_t)1U << vc), (int64_t)*ra_mipi_csi_reg32(clr_off));

    TEST_ASSERT_EQ((int32_t)k_ra_ok,
                   (int32_t)ra_mipi_csi_vc_set_irq_enable(vc, ((uint32_t)1U << vc)));
    const ra_mipi_csi_off_t ie_off = ra_mipi_csi_vc_off(k_ra_mipi_csi_off_vcie0, vc);
    TEST_ASSERT_EQ((int64_t)((uint32_t)1U << vc), (int64_t)*ra_mipi_csi_reg32(ie_off));
  }

  uint32_t v = 0U;
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_mipi_csi_vc_get_status(16U, &v));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_mipi_csi_vc_clear_status(16U, 0U));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_mipi_csi_vc_set_irq_enable(16U, 0U));
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_mipi_csi_vc_get_status(0U, nullptr));
  TEST_END("mipi_csi vc status get/clear/irq");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_pm_status_paths(void)
{
  TEST_BEGIN("mipi_csi pm status get/clear/irq");
  prep();
  const ra_mipi_csi_config_t cfg = make_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_mipi_csi_init(&cfg));

  *ra_mipi_csi_reg32(k_ra_mipi_csi_off_pmst) = (uint32_t)k_ra_mipi_csi_test_pmst_marker;

  uint32_t v = 0U;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_mipi_csi_pm_get_status(&v));
  TEST_ASSERT_EQ((int64_t)k_ra_mipi_csi_test_pmst_marker, (int64_t)v);

  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_mipi_csi_pm_clear_status((uint32_t)k_ra_mipi_csi_pmsc_all_mask));
  TEST_ASSERT_EQ((int64_t)k_ra_mipi_csi_pmsc_all_mask,
                 (int64_t)*ra_mipi_csi_reg32(k_ra_mipi_csi_off_pmsc));

  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_mipi_csi_pm_set_irq_enable((uint32_t)k_ra_mipi_csi_pmie_all_mask));
  TEST_ASSERT_EQ((int64_t)k_ra_mipi_csi_pmie_all_mask,
                 (int64_t)*ra_mipi_csi_reg32(k_ra_mipi_csi_off_pmie));

  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_mipi_csi_pm_get_status(nullptr));
  TEST_END("mipi_csi pm status get/clear/irq");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_short_packet_configure(void)
{
  TEST_BEGIN("mipi_csi short packet configure + irq");
  prep();
  const ra_mipi_csi_config_t cfg = make_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_mipi_csi_init(&cfg));

  TEST_ASSERT_EQ(
    (int32_t)k_ra_ok,
    (int32_t)ra_mipi_csi_short_packet_configure((uint8_t)k_ra_mipi_csi_test_threshold, true));
  const uint32_t gsct = *ra_mipi_csi_reg32(k_ra_mipi_csi_off_gsct);
  TEST_ASSERT_EQ((int32_t)k_ra_mipi_csi_test_threshold,
                 (int32_t)(gsct & (uint32_t)k_ra_mipi_csi_gsct_shth_mask));
  TEST_ASSERT((gsct & (uint32_t)k_ra_mipi_csi_gsct_gfif_mask) != 0UL);

  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_mipi_csi_short_packet_configure(0x10U, false));

  TEST_ASSERT_EQ(
    (int32_t)k_ra_ok,
    (int32_t)ra_mipi_csi_short_packet_set_irq_enable((uint32_t)k_ra_mipi_csi_gsie_all_mask));
  TEST_ASSERT_EQ((int64_t)k_ra_mipi_csi_gsie_all_mask,
                 (int64_t)*ra_mipi_csi_reg32(k_ra_mipi_csi_off_gsie));
  TEST_END("mipi_csi short packet configure + irq");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_short_packet_status(void)
{
  TEST_BEGIN("mipi_csi short packet status get/clear");
  prep();
  const ra_mipi_csi_config_t cfg = make_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_mipi_csi_init(&cfg));

  *ra_mipi_csi_reg32(k_ra_mipi_csi_off_gsst) =
    (uint32_t)k_ra_mipi_csi_gsst_gov_mask | (uint32_t)k_ra_mipi_csi_gsst_gne_mask;
  uint32_t v = 0U;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_mipi_csi_short_packet_get_status(&v));
  TEST_ASSERT((v & (uint32_t)k_ra_mipi_csi_gsst_gov_mask) != 0UL);

  TEST_ASSERT_EQ(
    (int32_t)k_ra_ok,
    (int32_t)ra_mipi_csi_short_packet_clear_status((uint32_t)k_ra_mipi_csi_gssc_govc_mask));
  TEST_ASSERT_EQ((int64_t)k_ra_mipi_csi_gssc_govc_mask,
                 (int64_t)*ra_mipi_csi_reg32(k_ra_mipi_csi_off_gssc));

  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_mipi_csi_short_packet_get_status(nullptr));
  TEST_END("mipi_csi short packet status get/clear");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_read_short_packet(void)
{
  TEST_BEGIN("mipi_csi read short packet");
  prep();
  const ra_mipi_csi_config_t cfg = make_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_mipi_csi_init(&cfg));

  /* Empty FIFO (PNUM = 0) -> empty error. */
  ra_mipi_csi_short_packet_t pkt = {};
  TEST_ASSERT_EQ((int32_t)k_ra_err_empty, (int32_t)ra_mipi_csi_read_short_packet(&pkt));

  /* PNUM = 1, GSHT loaded with a known value -> decoded fields match. */
  *ra_mipi_csi_reg32(k_ra_mipi_csi_off_gsst) =
    (uint32_t)(1U << (uint32_t)k_ra_mipi_csi_gsst_pnum_shift);
  *ra_mipi_csi_reg32(k_ra_mipi_csi_off_gsht) = (uint32_t)k_ra_mipi_csi_test_pkt_value;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_mipi_csi_read_short_packet(&pkt));
  TEST_ASSERT_EQ((int32_t)0x3412, (int32_t)pkt.payload);
  TEST_ASSERT_EQ((int32_t)0x08, (int32_t)pkt.data_type);
  TEST_ASSERT_EQ((int32_t)0x0A, (int32_t)pkt.vc);
  TEST_ASSERT_EQ((int64_t)k_ra_mipi_csi_test_pkt_value, (int64_t)pkt.raw);

  /* GSIU.FINC must have been pulsed. */
  TEST_ASSERT(
    (*ra_mipi_csi_reg32(k_ra_mipi_csi_off_gsiu) & (uint32_t)k_ra_mipi_csi_gsiu_finc_mask) != 0UL);

  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_mipi_csi_read_short_packet(nullptr));
  TEST_END("mipi_csi read short packet");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_short_packet_clear_fifo(void)
{
  TEST_BEGIN("mipi_csi short packet clear fifo");
  prep();
  const ra_mipi_csi_config_t cfg = make_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_mipi_csi_init(&cfg));

  /* Pre-load GCD in GSST so the spinner exits immediately. */
  *ra_mipi_csi_reg32(k_ra_mipi_csi_off_gsst) = (uint32_t)k_ra_mipi_csi_gsst_gcd_mask;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_mipi_csi_short_packet_clear_fifo());
  /* GSIU.GFCLR must have been released back to 0. */
  TEST_ASSERT_EQ((int64_t)0, (int64_t)*ra_mipi_csi_reg32(k_ra_mipi_csi_off_gsiu));

  /* Now without GCD set -> hw_timeout. */
  *ra_mipi_csi_reg32(k_ra_mipi_csi_off_gsst) = 0U;
  TEST_ASSERT_EQ((int32_t)k_ra_err_hw_timeout, (int32_t)ra_mipi_csi_short_packet_clear_fifo());
  TEST_ASSERT_EQ((int64_t)0, (int64_t)*ra_mipi_csi_reg32(k_ra_mipi_csi_off_gsiu));
  TEST_END("mipi_csi short packet clear fifo");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_short_packet_re_enable(void)
{
  TEST_BEGIN("mipi_csi short packet re-enable store");
  prep();
  const ra_mipi_csi_config_t cfg = make_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_mipi_csi_init(&cfg));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_mipi_csi_short_packet_re_enable_store());
  TEST_ASSERT(
    (*ra_mipi_csi_reg32(k_ra_mipi_csi_off_gsiu) & (uint32_t)k_ra_mipi_csi_gsiu_gfen_mask) != 0UL);
  TEST_END("mipi_csi short packet re-enable store");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_attach_and_dispatch(void)
{
  TEST_BEGIN("mipi_csi attach + dispatch");
  prep();

  const ra_mipi_csi_config_t cfg = make_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_mipi_csi_init(&cfg));
  TEST_ASSERT_EQ(
    (int32_t)k_ra_ok,
    (int32_t)ra_mipi_csi_attach_handler(stub_event_cb,
                                        (void*)(uintptr_t)k_ra_mipi_csi_test_ctx_marker));

  *ra_mipi_csi_reg32(k_ra_mipi_csi_off_rxst) = (uint32_t)k_ra_mipi_csi_test_ractdet_marker;
  ra_mipi_csi_dispatch();

  TEST_ASSERT_EQ((int32_t)1, (int32_t)s_cb_count);
  TEST_ASSERT_EQ((int64_t)k_ra_mipi_csi_test_ractdet_marker, (int64_t)s_cb_last_rxst);
  TEST_ASSERT_EQ((int64_t)(uintptr_t)k_ra_mipi_csi_test_ctx_marker,
                 (int64_t)(uintptr_t)s_cb_last_ctx);

  TEST_ASSERT_EQ((int64_t)k_ra_mipi_csi_rxsc_ractdetc_mask,
                 (int64_t)*ra_mipi_csi_reg32(k_ra_mipi_csi_off_rxsc));

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_mipi_csi_attach_handler(nullptr, nullptr));
  ra_mipi_csi_dispatch();
  TEST_ASSERT_EQ((int32_t)1, (int32_t)s_cb_count);
  TEST_END("mipi_csi attach + dispatch");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_dispatch_dl(void)
{
  TEST_BEGIN("mipi_csi dispatch dl per-lane");
  prep();
  const ra_mipi_csi_config_t cfg = make_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_mipi_csi_init(&cfg));
  TEST_ASSERT_EQ(
    (int32_t)k_ra_ok,
    (int32_t)ra_mipi_csi_attach_dl_handler(stub_dl_cb,
                                           (void*)(uintptr_t)k_ra_mipi_csi_test_dl_marker));

  /* Both lanes flagged in MIST, distinct DLST values. */
  *ra_mipi_csi_reg32(k_ra_mipi_csi_off_mist) =
    (uint32_t)k_ra_mipi_csi_mist_dl0s_mask | (uint32_t)k_ra_mipi_csi_mist_dl1s_mask;
  *ra_mipi_csi_reg32(k_ra_mipi_csi_off_dlst0) = (uint32_t)k_ra_mipi_csi_dlst_esh_mask;
  *ra_mipi_csi_reg32(k_ra_mipi_csi_off_dlst1) = (uint32_t)k_ra_mipi_csi_dlst_ess_mask;

  ra_mipi_csi_dispatch_dl();

  TEST_ASSERT_EQ((int32_t)1, (int32_t)s_dl_calls[0]);
  TEST_ASSERT_EQ((int32_t)1, (int32_t)s_dl_calls[1]);
  TEST_ASSERT_EQ((int64_t)k_ra_mipi_csi_dlst_esh_mask, (int64_t)s_dl_last_mask[0]);
  TEST_ASSERT_EQ((int64_t)k_ra_mipi_csi_dlst_ess_mask, (int64_t)s_dl_last_mask[1]);
  /* DLSC should have been written with the W1C-able subset. */
  TEST_ASSERT_EQ(
    (int64_t)((uint32_t)k_ra_mipi_csi_dlst_esh_mask & (uint32_t)k_ra_mipi_csi_dlsc_all_mask),
    (int64_t)*ra_mipi_csi_reg32(k_ra_mipi_csi_off_dlsc0));
  TEST_END("mipi_csi dispatch dl per-lane");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_dispatch_vc(void)
{
  TEST_BEGIN("mipi_csi dispatch vc + generic err");
  prep();
  const ra_mipi_csi_config_t cfg = make_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_mipi_csi_init(&cfg));
  TEST_ASSERT_EQ(
    (int32_t)k_ra_ok,
    (int32_t)ra_mipi_csi_attach_vc_handler(stub_vc_cb,
                                           (void*)(uintptr_t)k_ra_mipi_csi_test_vc_marker));

  /* Flag VC0 + VC5 in MIST. */
  *ra_mipi_csi_reg32(k_ra_mipi_csi_off_mist) = ((uint32_t)0x21U)
                                               << (uint32_t)k_ra_mipi_csi_mist_vc_shift;

  /* VC0 carries a CRC error (per-VC). */
  const ra_mipi_csi_off_t st0 = ra_mipi_csi_vc_off(k_ra_mipi_csi_off_vcst0, 0U);
  *ra_mipi_csi_reg32(st0)     = (uint32_t)k_ra_mipi_csi_vcst_crc_mask;
  /* VC5 carries an MLF (generic-err) + LSR (per-VC). */
  const ra_mipi_csi_off_t st5 = ra_mipi_csi_vc_off(k_ra_mipi_csi_off_vcst0, 5U);
  *ra_mipi_csi_reg32(st5) =
    (uint32_t)k_ra_mipi_csi_vcst_mlf_mask | (uint32_t)k_ra_mipi_csi_vcst_lsr_mask;

  ra_mipi_csi_dispatch_vc();

  TEST_ASSERT_EQ((int32_t)1, (int32_t)s_vc_calls[0]);
  TEST_ASSERT_EQ((int64_t)k_ra_mipi_csi_vcst_crc_mask, (int64_t)s_vc_last_mask[0]);
  TEST_ASSERT_EQ((int32_t)1, (int32_t)s_vc_calls[5]);
  TEST_ASSERT_EQ((int64_t)k_ra_mipi_csi_vcst_lsr_mask, (int64_t)s_vc_last_mask[5]);
  /* Generic summary callback (vc=0xFF -> idx 16) carries MLF only. */
  TEST_ASSERT_EQ((int32_t)1, (int32_t)s_vc_calls[16]);
  TEST_ASSERT_EQ((int64_t)k_ra_mipi_csi_vcst_mlf_mask, (int64_t)s_vc_last_mask[16]);

  /* VCSC for VC0 must reflect the per-vc clear mask. */
  const ra_mipi_csi_off_t clr0 = ra_mipi_csi_vc_off(k_ra_mipi_csi_off_vcsc0, 0U);
  TEST_ASSERT_EQ((int64_t)k_ra_mipi_csi_vcst_crc_mask, (int64_t)*ra_mipi_csi_reg32(clr0));
  TEST_END("mipi_csi dispatch vc + generic err");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_dispatch_pm(void)
{
  TEST_BEGIN("mipi_csi dispatch pm");
  prep();
  const ra_mipi_csi_config_t cfg = make_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_mipi_csi_init(&cfg));
  TEST_ASSERT_EQ(
    (int32_t)k_ra_ok,
    (int32_t)ra_mipi_csi_attach_pm_handler(stub_pm_cb,
                                           (void*)(uintptr_t)k_ra_mipi_csi_test_pm_marker));

  *ra_mipi_csi_reg32(k_ra_mipi_csi_off_pmst) = (uint32_t)k_ra_mipi_csi_test_pmst_marker;
  ra_mipi_csi_dispatch_pm();

  TEST_ASSERT_EQ((int32_t)1, (int32_t)s_pm_calls);
  TEST_ASSERT_EQ((int64_t)k_ra_mipi_csi_test_pmst_marker, (int64_t)s_pm_last_mask);
  /* PMSC must have been written with only the lower 8 bits. */
  TEST_ASSERT_EQ(
    (int64_t)((uint32_t)k_ra_mipi_csi_test_pmst_marker & (uint32_t)k_ra_mipi_csi_pmst_w1c_mask),
    (int64_t)*ra_mipi_csi_reg32(k_ra_mipi_csi_off_pmsc));
  TEST_END("mipi_csi dispatch pm");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_dispatch_short_packet(void)
{
  TEST_BEGIN("mipi_csi dispatch short packet");
  prep();
  const ra_mipi_csi_config_t cfg = make_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_mipi_csi_init(&cfg));
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_mipi_csi_attach_short_packet_handler(
                   stub_gst_cb,
                   (void*)(uintptr_t)k_ra_mipi_csi_test_gst_marker));

  *ra_mipi_csi_reg32(k_ra_mipi_csi_off_gsst) =
    (uint32_t)k_ra_mipi_csi_gsst_gov_mask | (uint32_t)k_ra_mipi_csi_gsst_gne_mask;
  ra_mipi_csi_dispatch_short_packet();

  TEST_ASSERT_EQ((int32_t)1, (int32_t)s_gst_calls);
  TEST_ASSERT((s_gst_last_mask & (uint32_t)k_ra_mipi_csi_gsst_gov_mask) != 0UL);
  /* GSSC should hold only the GOV-clear bit. */
  TEST_ASSERT_EQ((int64_t)k_ra_mipi_csi_gssc_govc_mask,
                 (int64_t)*ra_mipi_csi_reg32(k_ra_mipi_csi_off_gssc));
  TEST_END("mipi_csi dispatch short packet");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_power_transition(void)
{
  TEST_BEGIN("mipi_csi power transition");
  prep();

  const ra_mipi_csi_config_t cfg = make_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_mipi_csi_init(&cfg));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_mipi_csi_enter_stop());
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_mipi_csi_exit_stop());
  TEST_END("mipi_csi power transition");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_deinit(void)
{
  TEST_BEGIN("mipi_csi deinit");
  prep();

  const ra_mipi_csi_config_t cfg = make_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_mipi_csi_init(&cfg));
  /* Pre-set IRQ enables so we can verify they get cleared. */
  *ra_mipi_csi_reg32(k_ra_mipi_csi_off_rxie)  = 0xFFFFFFFFUL;
  *ra_mipi_csi_reg32(k_ra_mipi_csi_off_dlie0) = 0xFFFFFFFFUL;
  *ra_mipi_csi_reg32(k_ra_mipi_csi_off_pmie)  = 0xFFFFFFFFUL;
  *ra_mipi_csi_reg32(k_ra_mipi_csi_off_gsie)  = 0xFFFFFFFFUL;
  for (uint8_t vc = 0U; vc < 16U; ++vc) {
    const ra_mipi_csi_off_t off = ra_mipi_csi_vc_off(k_ra_mipi_csi_off_vcie0, vc);
    *ra_mipi_csi_reg32(off)     = 0xFFFFFFFFUL;
  }

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_mipi_csi_deinit());

  TEST_ASSERT_EQ((int32_t)0, (int32_t)*ra_mipi_csi_reg32(k_ra_mipi_csi_off_mct3));
  TEST_ASSERT_EQ((int32_t)0, (int32_t)*ra_mipi_csi_reg32(k_ra_mipi_csi_off_rxie));
  TEST_ASSERT_EQ((int32_t)0, (int32_t)*ra_mipi_csi_reg32(k_ra_mipi_csi_off_dlie0));
  TEST_ASSERT_EQ((int32_t)0, (int32_t)*ra_mipi_csi_reg32(k_ra_mipi_csi_off_dlie1));
  TEST_ASSERT_EQ((int32_t)0, (int32_t)*ra_mipi_csi_reg32(k_ra_mipi_csi_off_pmie));
  TEST_ASSERT_EQ((int32_t)0, (int32_t)*ra_mipi_csi_reg32(k_ra_mipi_csi_off_gsie));
  for (uint8_t vc = 0U; vc < 16U; ++vc) {
    const ra_mipi_csi_off_t off = ra_mipi_csi_vc_off(k_ra_mipi_csi_off_vcie0, vc);
    TEST_ASSERT_EQ((int32_t)0, (int32_t)*ra_mipi_csi_reg32(off));
  }
  TEST_END("mipi_csi deinit");
}

/* ===========================================================================
 * Sweep 6: virtual-channel filter, per-VC data-format selection, error
 * handler. NULL-arg coverage included.
 * ===========================================================================
 */

typedef enum : uint32_t {
  k_ra_mipi_csi_test_vc_keep_mask = 0x0005U,  /**< Keep VC0 + VC2.       */
  k_ra_mipi_csi_test_vc_seed      = 0xABCDUL, /**< Pre-seeded VCIE val. */
  k_ra_mipi_csi_test_err_marker   = 0xDEC0DE0U,
  k_ra_mipi_csi_test_err_ctx_val  = 0xCAFEU,
} ra_mipi_csi_sw6_const_t;

static uint32_t                   s_err_calls;
static ra_mipi_csi_error_report_t s_err_last;
static void*                      s_err_last_ctx;

static void stub_err_cb(void* ctx, const ra_mipi_csi_error_report_t* report)
{
  ++s_err_calls;
  s_err_last_ctx = ctx;
  if (report != nullptr) {
    s_err_last = *report;
  }
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_set_virtual_channels(void)
{
  TEST_BEGIN("mipi_csi set_virtual_channels masks unselected VCIE");
  prep();
  const ra_mipi_csi_config_t cfg = make_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_mipi_csi_init(&cfg));

  /* Seed VCIE for every channel so we can confirm the masking. */
  for (uint8_t vc = 0U; vc < 16U; ++vc) {
    const ra_mipi_csi_off_t off = ra_mipi_csi_vc_off(k_ra_mipi_csi_off_vcie0, vc);
    *ra_mipi_csi_reg32(off)     = (uint32_t)k_ra_mipi_csi_test_vc_seed;
  }

  TEST_ASSERT_EQ(
    (int32_t)k_ra_ok,
    (int32_t)ra_mipi_csi_set_virtual_channels((uint16_t)k_ra_mipi_csi_test_vc_keep_mask));

  /* VC0 + VC2 retain (saved was zero, so default mask was restored). */
  TEST_ASSERT_EQ((int32_t)k_ra_mipi_csi_test_vc_seed,
                 (int32_t)*ra_mipi_csi_reg32(ra_mipi_csi_vc_off(k_ra_mipi_csi_off_vcie0, 0U)));
  /* VC1, VC3..VC15 zeroed. */
  TEST_ASSERT_EQ((int32_t)0,
                 (int32_t)*ra_mipi_csi_reg32(ra_mipi_csi_vc_off(k_ra_mipi_csi_off_vcie0, 1U)));
  TEST_ASSERT_EQ((int32_t)0,
                 (int32_t)*ra_mipi_csi_reg32(ra_mipi_csi_vc_off(k_ra_mipi_csi_off_vcie0, 5U)));

  /* Re-enabling VC1 restores the saved value. */
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_mipi_csi_set_virtual_channels((uint16_t)0xFFFFU));
  TEST_ASSERT_EQ((int32_t)k_ra_mipi_csi_test_vc_seed,
                 (int32_t)*ra_mipi_csi_reg32(ra_mipi_csi_vc_off(k_ra_mipi_csi_off_vcie0, 1U)));
  TEST_END("mipi_csi set_virtual_channels masks unselected VCIE");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_set_virtual_channels_empty_mask(void)
{
  TEST_BEGIN("mipi_csi set_virtual_channels rejects empty mask");
  prep();
  const ra_mipi_csi_config_t cfg = make_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_mipi_csi_init(&cfg));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_mipi_csi_set_virtual_channels(0U));
  TEST_END("mipi_csi set_virtual_channels rejects empty mask");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_set_data_format_per_vc(void)
{
  TEST_BEGIN("mipi_csi set_data_format programs DTEL/DTEH per VC");
  prep();
  const ra_mipi_csi_config_t cfg = make_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_mipi_csi_init(&cfg));

  /* VC0 -> RGB888 (DTEH bit). */
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_mipi_csi_set_data_format(0U, k_ra_mipi_csi_format_rgb888));
  TEST_ASSERT_EQ((int32_t)k_ra_mipi_csi_dteh_rgb888_mask,
                 (int32_t)*ra_mipi_csi_reg32(k_ra_mipi_csi_off_dteh));
  TEST_ASSERT_EQ((int32_t)0, (int32_t)*ra_mipi_csi_reg32(k_ra_mipi_csi_off_dtel));

  /* VC1 -> YUV422_8 (DTEL bit). DTEH stays set. */
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_mipi_csi_set_data_format(1U, k_ra_mipi_csi_format_yuv422_8));
  TEST_ASSERT_EQ((int32_t)k_ra_mipi_csi_dtel_yuv422_8_mask,
                 (int32_t)*ra_mipi_csi_reg32(k_ra_mipi_csi_off_dtel));
  TEST_ASSERT_EQ((int32_t)k_ra_mipi_csi_dteh_rgb888_mask,
                 (int32_t)*ra_mipi_csi_reg32(k_ra_mipi_csi_off_dteh));

  /* Disable VC0 -- DTEH clears. */
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_mipi_csi_set_data_format(0U, k_ra_mipi_csi_format_off));
  TEST_ASSERT_EQ((int32_t)0, (int32_t)*ra_mipi_csi_reg32(k_ra_mipi_csi_off_dteh));
  TEST_ASSERT_EQ((int32_t)k_ra_mipi_csi_dtel_yuv422_8_mask,
                 (int32_t)*ra_mipi_csi_reg32(k_ra_mipi_csi_off_dtel));
  TEST_END("mipi_csi set_data_format programs DTEL/DTEH per VC");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_set_data_format_bad_args(void)
{
  TEST_BEGIN("mipi_csi set_data_format rejects bad inputs");
  prep();
  const ra_mipi_csi_config_t cfg = make_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_mipi_csi_init(&cfg));

  /* VC out of range. */
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_mipi_csi_set_data_format(16U, k_ra_mipi_csi_format_rgb888));

  /* Format unsupported on RA8D2 DTEL/DTEH (RAW10). */
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_mipi_csi_set_data_format(0U, k_ra_mipi_csi_format_raw10));
  TEST_END("mipi_csi set_data_format rejects bad inputs");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_attach_error_handler(void)
{
  TEST_BEGIN("mipi_csi attach_error_handler decodes ECC/CRC reports");
  prep();
  const ra_mipi_csi_config_t cfg = make_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_mipi_csi_init(&cfg));

  s_err_calls           = 0U;
  s_err_last_ctx        = nullptr;
  void* const ctx_token = (void*)(uintptr_t)k_ra_mipi_csi_test_err_ctx_val;
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_mipi_csi_attach_error_handler(stub_err_cb, ctx_token));

  /* Drive a CRC + ECC-corrected event on VC0 via VCST seeding + MIST set.
   * MIST.VC[31:16] -- bit 16 = VC0 source flag. */
  *ra_mipi_csi_reg32(k_ra_mipi_csi_off_mist) =
    (uint32_t)((uint32_t)1U << (uint32_t)k_ra_mipi_csi_mist_vc_shift);
  const ra_mipi_csi_off_t st_off = ra_mipi_csi_vc_off(k_ra_mipi_csi_off_vcst0, 0U);
  *ra_mipi_csi_reg32(st_off) =
    (uint32_t)k_ra_mipi_csi_vcst_crc_mask | (uint32_t)k_ra_mipi_csi_vcst_ecc_mask;

  ra_mipi_csi_dispatch_vc();
  TEST_ASSERT_EQ((int32_t)1, (int32_t)s_err_calls);
  TEST_ASSERT(s_err_last_ctx == ctx_token);
  TEST_ASSERT(s_err_last.crc_error);
  TEST_ASSERT(s_err_last.ecc_corrected);
  TEST_ASSERT(!s_err_last.ecc_two_bit_error);
  TEST_ASSERT_EQ((int32_t)0, (int32_t)s_err_last.vc);

  /* Detach -- subsequent dispatches must be silent. */
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_mipi_csi_attach_error_handler(nullptr, nullptr));
  *ra_mipi_csi_reg32(k_ra_mipi_csi_off_mist) =
    (uint32_t)((uint32_t)1U << (uint32_t)k_ra_mipi_csi_mist_vc_shift);
  *ra_mipi_csi_reg32(st_off) = (uint32_t)k_ra_mipi_csi_vcst_crc_mask;
  ra_mipi_csi_dispatch_vc();
  TEST_ASSERT_EQ((int32_t)1, (int32_t)s_err_calls);
  TEST_END("mipi_csi attach_error_handler decodes ECC/CRC reports");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_error_handler_no_errors_silent(void)
{
  TEST_BEGIN("mipi_csi error handler silent on non-error VCST snapshot");
  prep();
  const ra_mipi_csi_config_t cfg = make_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_mipi_csi_init(&cfg));

  s_err_calls = 0U;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_mipi_csi_attach_error_handler(stub_err_cb, nullptr));

  /* Seed a frame-start event (not an error). */
  *ra_mipi_csi_reg32(k_ra_mipi_csi_off_mist) =
    (uint32_t)((uint32_t)1U << (uint32_t)k_ra_mipi_csi_mist_vc_shift);
  const ra_mipi_csi_off_t st_off = ra_mipi_csi_vc_off(k_ra_mipi_csi_off_vcst0, 0U);
  *ra_mipi_csi_reg32(st_off)     = (uint32_t)k_ra_mipi_csi_vcst_fsr_mask;
  ra_mipi_csi_dispatch_vc();
  TEST_ASSERT_EQ((int32_t)0, (int32_t)s_err_calls);
  TEST_END("mipi_csi error handler silent on non-error VCST snapshot");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_attach_error_handler_null_safe(void)
{
  TEST_BEGIN("mipi_csi attach_error_handler accepts NULL detach");
  prep();
  const ra_mipi_csi_config_t cfg = make_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_mipi_csi_init(&cfg));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_mipi_csi_attach_error_handler(nullptr, nullptr));
  /* Dispatch with no handler is a no-op. */
  *ra_mipi_csi_reg32(k_ra_mipi_csi_off_mist) =
    (uint32_t)((uint32_t)1U << (uint32_t)k_ra_mipi_csi_mist_vc_shift);
  const ra_mipi_csi_off_t st_off = ra_mipi_csi_vc_off(k_ra_mipi_csi_off_vcst0, 0U);
  *ra_mipi_csi_reg32(st_off)     = (uint32_t)k_ra_mipi_csi_vcst_crc_mask;
  ra_mipi_csi_dispatch_vc();
  TEST_END("mipi_csi attach_error_handler accepts NULL detach");
}

/**
 * @test test_mcdc_validate_lanes
 *
 * @par MC/DC:
 * Decision: `if ((cfg->lanes != k_ra_mipi_csi_lanes_1) &&
 *               (cfg->lanes != k_ra_mipi_csi_lanes_2))`
 * (2 conditions, libs/ra_hal/src/ra_mipi_csi.c line 283)
 * - Vector 1: lanes=1 -> C1=(1!=1)=F short-circuits. Decision F -> ok.
 * - Vector 2: lanes=2 -> C1=(2!=1)=T, C2=(2!=2)=F. Decision F -> ok.
 * - Vector 3: lanes=3 -> C1=(3!=1)=T, C2=(3!=2)=T. Decision T -> invalid_arg.
 * MC/DC pair for C1: V1(F,_)->F vs V3(T,T)->T (decision flips, C2
 * masked: not evaluated in V1, T in V3 so the masking pair is the
 * effective short-circuit pair). MC/DC pair for C2: V2(T,F)->F vs
 * V3(T,T)->T (decision flips, C1 held T). N+1 = 3 vectors for N=2
 * conditions: minimal MC/DC.
 */
static void test_mcdc_validate_lanes(void)
{
  TEST_BEGIN("mipi_csi init MC/DC: lanes!=1 && lanes!=2");
  prep();

  ra_mipi_csi_config_t cfg = make_cfg();

  /* Vector 1: lanes=1 -> decision F -> ok. */
  cfg.lanes = k_ra_mipi_csi_lanes_1;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_mipi_csi_init(&cfg));
  prep();

  /* Vector 2: lanes=2 -> decision F -> ok. */
  cfg       = make_cfg();
  cfg.lanes = k_ra_mipi_csi_lanes_2;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_mipi_csi_init(&cfg));
  prep();

  /* Vector 3: lanes=3 -> decision T -> invalid_arg. */
  cfg       = make_cfg();
  cfg.lanes = (ra_mipi_csi_lanes_t)3U;
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_mipi_csi_init(&cfg));

  TEST_END("mipi_csi init MC/DC: lanes!=1 && lanes!=2");
}

int32_t main(void)
{
  test_init_happy();
  test_init_ecc_and_frame_bits();
  test_init_dt_and_short_packet();
  test_init_epct_emct();
  test_init_irq_masks();
  test_init_null_cfg();
  test_init_bad_lanes();
  test_init_bad_args();
  test_start_then_stop();
  test_reset();
  test_status_get_clear();
  test_set_rx_irq_enable();
  test_get_module_irq_status();
  test_get_module_info();
  test_set_data_type_filter();
  test_set_ecc_mode();
  test_set_frame_error_mode();
  test_set_epd();
  test_set_lrte();
  test_dl_status_paths();
  test_vc_status_paths();
  test_pm_status_paths();
  test_short_packet_configure();
  test_short_packet_status();
  test_read_short_packet();
  test_short_packet_clear_fifo();
  test_short_packet_re_enable();
  test_attach_and_dispatch();
  test_dispatch_dl();
  test_dispatch_vc();
  test_dispatch_pm();
  test_dispatch_short_packet();
  test_power_transition();
  test_deinit();
  test_set_virtual_channels();
  test_set_virtual_channels_empty_mask();
  test_set_data_format_per_vc();
  test_set_data_format_bad_args();
  test_attach_error_handler();
  test_error_handler_no_errors_silent();
  test_attach_error_handler_null_safe();
  test_mcdc_validate_lanes();
  (void)fprintf(stderr, "[OK  ] test_ra_mipi_csi.c\n");
  return 0;
}
