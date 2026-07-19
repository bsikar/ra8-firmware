/**
 * @file test_ra8_mipi_csi_init.c
 * @brief Unit tests for MIPI CSI-2 receiver init, config setters and power
 *
 * @details
 * Split sibling of the original test_ra8_mipi_csi.c suite covering
 * bring-up and the configuration surface of ra8_mipi_csi.c against
 * the host-side simulated MMIO (``ra8_sim_mmap``):
 *
 * - init happy path incl. ECC / frame / data-type / EPCT-EMCT / IRQ
 *   mask programming, plus null-cfg, lane and bad-arg rejection
 * - start / stop / reset, status get + clear, RX IRQ enables,
 *   module IRQ status + info
 * - data-type filter, ECC mode, frame-error mode, EPD, LRTE setters
 * - power transition, deinit, and the lane-validation MC/DC vector
 *
 * Sibling suite: test_ra8_mipi_csi_events.c (per-lane / per-VC / PM
 * status paths, short-packet FIFO, ISR dispatch, error handler).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_err.h"
#include "ra8_mipi_csi.h"
#include "ra8_mipi_csi_regs.h"
#include "ra8_mstp.h"
#include "ra8_sim_mmap.h"
#include "unity_minimal.h"

/**
 * @enum mipi_csi_init_uint16_const_t
 * @brief Named uint16_t constants used by this file.
 *
 * @details
 * Every literal this translation unit needs, named so the
 * value's role is visible at the point of use (CLAUDE.md
 * "No Magic Numbers").
 */
typedef enum : uint16_t {
  k_mipi_csi_init_epd_long_spacer_100  = 0x100U,
  k_mipi_csi_init_epd_long_spacer_8000 = 0x8000U,
  k_mipi_csi_init_epd_short_spacer_200 = 0x200U,
} mipi_csi_init_uint16_const_t;

/**
 * @enum mipi_csi_init_uint32_const_t
 * @brief Named uint32_t constants used by this file.
 *
 * @details
 * Every literal this translation unit needs, named so the
 * value's role is visible at the point of use (CLAUDE.md
 * "No Magic Numbers").
 */
typedef enum : uint32_t {
  k_mipi_csi_init_sentinel_ffffffff = 0xFFFFFFFFUL,
} mipi_csi_init_uint32_const_t;

/**
 * @enum ra8_mipi_csi_test_const_t
 * @brief Constants used across multiple test cases.
 */
typedef enum : uint32_t {
  k_ra8_mipi_csi_test_frrclk         = 3U,           /**< vclk/hsclk = 125/90 example.      */
  k_ra8_mipi_csi_test_frrskw         = 5U,           /**< vclk/hsclk = 125/90 example.      */
  k_ra8_mipi_csi_test_ractdet_marker = 0x00020000UL, /**< RA8 mipi csi test ractdet marker. */
  k_ra8_mipi_csi_test_ctx_marker     = 0xCAFEU,      /**< RA8 mipi csi test ctx marker.     */
  k_ra8_mipi_csi_test_dl_marker      = 0xC0DEU,      /**< RA8 mipi csi test dl marker.      */
  k_ra8_mipi_csi_test_vc_marker      = 0xBEEFU,      /**< RA8 mipi csi test vc marker.      */
  k_ra8_mipi_csi_test_pm_marker      = 0xFACEU,      /**< RA8 mipi csi test pm marker.      */
  k_ra8_mipi_csi_test_gst_marker     = 0xBABEU,      /**< RA8 mipi csi test gst marker.     */
  k_ra8_mipi_csi_test_mcg_value      = 0x00100A02UL, /**< VER=2 SDLN=0xA STAGES=0x10.       */
  k_ra8_mipi_csi_test_dt_low_value   = 0xC0008010UL, /**< YUV422_8/10 + GSP1 + EOT.         */
  k_ra8_mipi_csi_test_dt_high_value  = 0x00000010UL, /**< RGB888.                           */
  k_ra8_mipi_csi_test_threshold      = 7U,           /**< RA8 mipi csi test threshold.      */
  k_ra8_mipi_csi_test_pkt_value      = 0x0A083412UL, /**< VC=0xA DT=0x08 SPDT=0x3412.       */
  k_ra8_mipi_csi_test_pmst_marker    = 0x00C00055UL, /**< Lower 8 W1C-able + RO bits.       */
} ra8_mipi_csi_test_const_t;

/**
 * @brief Reset sim mmap + ra8_mstp before each test.
 */
static void prep(void)
{
  ra8_sim_mmap_reset();
  (void)ra8_mstp_init();
}

/**
 * @brief Build a default config (2 lanes, GRMD on, EOTPEN off, all IRQs off).
 */
static ra8_mipi_csi_config_t make_cfg(void)
{
  const ra8_mipi_csi_config_t cfg = {
    .lanes              = k_ra8_mipi_csi_lanes_2,
    .generic_rule       = true,
    .eccv13             = false,
    .lfsren             = false,
    .zlmd               = false,
    .edmd               = false,
    .rvmd               = false,
    .frrclk             = (uint16_t)k_ra8_mipi_csi_test_frrclk,
    .frrskw             = (uint16_t)k_ra8_mipi_csi_test_frrskw,
    .epd_enable         = false,
    .epd_option_2       = false,
    .epd_long_spacer    = 0U,
    .epd_short_spacer   = 0U,
    .vlsien             = k_ra8_mipi_csi_vlsien_fixed,
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

  const ra8_mipi_csi_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_csi_init(&cfg));

  /* MCT0: VDLN = 2, GRMD set. */
  const uint32_t mct0 = *ra8_mipi_csi_reg32(k_ra8_mipi_csi_off_mct0);
  TEST_ASSERT_EQ(k_ra8_mipi_csi_lanes_2, (mct0 & (uint32_t)k_ra8_mipi_csi_mct0_vdln_mask));
  TEST_ASSERT((mct0 & (uint32_t)k_ra8_mipi_csi_mct0_grmd_mask) != 0UL);

  /* MCT2: FRRSKW @ [24:16] | FRRCLK @ [8:0]. */
  const uint32_t mct2     = *ra8_mipi_csi_reg32(k_ra8_mipi_csi_off_mct2);
  const uint32_t expected = (((uint32_t)k_ra8_mipi_csi_test_frrskw & 0x1FFUL) << 16U) |
                            ((uint32_t)k_ra8_mipi_csi_test_frrclk & 0x1FFUL);
  TEST_ASSERT_EQ(expected, mct2);

  /* MCT3 must remain 0 (RXEN cleared post-init). */
  TEST_ASSERT_EQ(0, *ra8_mipi_csi_reg32(k_ra8_mipi_csi_off_mct3));
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

  ra8_mipi_csi_config_t cfg = make_cfg();
  cfg.eccv13                = true;
  cfg.lfsren                = true;
  cfg.zlmd                  = true;
  cfg.edmd                  = true;
  cfg.rvmd                  = true;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_csi_init(&cfg));

  const uint32_t mct0 = *ra8_mipi_csi_reg32(k_ra8_mipi_csi_off_mct0);
  TEST_ASSERT((mct0 & (uint32_t)k_ra8_mipi_csi_mct0_eccv13_mask) != 0UL);
  TEST_ASSERT((mct0 & (uint32_t)k_ra8_mipi_csi_mct0_lfsren_mask) != 0UL);
  TEST_ASSERT((mct0 & (uint32_t)k_ra8_mipi_csi_mct0_zlmd_mask) != 0UL);
  TEST_ASSERT((mct0 & (uint32_t)k_ra8_mipi_csi_mct0_edmd_mask) != 0UL);
  TEST_ASSERT((mct0 & (uint32_t)k_ra8_mipi_csi_mct0_rvmd_mask) != 0UL);
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

  ra8_mipi_csi_config_t cfg = make_cfg();
  cfg.dt_low_mask           = (uint32_t)k_ra8_mipi_csi_test_dt_low_value;
  cfg.dt_high_mask          = (uint32_t)k_ra8_mipi_csi_test_dt_high_value;
  cfg.short_threshold       = (uint8_t)k_ra8_mipi_csi_test_threshold;
  cfg.short_store_enable    = true;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_csi_init(&cfg));

  TEST_ASSERT_EQ(k_ra8_mipi_csi_test_dt_low_value, *ra8_mipi_csi_reg32(k_ra8_mipi_csi_off_dtel));
  TEST_ASSERT_EQ(k_ra8_mipi_csi_test_dt_high_value, *ra8_mipi_csi_reg32(k_ra8_mipi_csi_off_dteh));

  const uint32_t gsct = *ra8_mipi_csi_reg32(k_ra8_mipi_csi_off_gsct);
  TEST_ASSERT_EQ(k_ra8_mipi_csi_test_threshold, (gsct & (uint32_t)k_ra8_mipi_csi_gsct_shth_mask));
  TEST_ASSERT((gsct & (uint32_t)k_ra8_mipi_csi_gsct_gfif_mask) != 0UL);
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

  ra8_mipi_csi_config_t cfg = make_cfg();
  cfg.epd_enable            = true;
  cfg.epd_option_2          = true;
  cfg.epd_long_spacer       = k_mipi_csi_init_epd_long_spacer_100;
  cfg.epd_short_spacer      = k_mipi_csi_init_epd_short_spacer_200;
  cfg.vlsien                = k_ra8_mipi_csi_vlsien_x2;
  cfg.eotp_enable           = true;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_csi_init(&cfg));

  const uint32_t epct = *ra8_mipi_csi_reg32(k_ra8_mipi_csi_off_epct);
  TEST_ASSERT((epct & (uint32_t)k_ra8_mipi_csi_epct_epden_mask) != 0UL);
  TEST_ASSERT((epct & (uint32_t)k_ra8_mipi_csi_epct_epdop_mask) != 0UL);
  TEST_ASSERT_EQ(0x100, (epct & (uint32_t)k_ra8_mipi_csi_epct_slp_mask));
  TEST_ASSERT_EQ(0x02000000ULL, (epct & (uint32_t)k_ra8_mipi_csi_epct_ssp_mask));

  const uint32_t emct = *ra8_mipi_csi_reg32(k_ra8_mipi_csi_off_emct);
  TEST_ASSERT_EQ(((uint32_t)k_ra8_mipi_csi_vlsien_x2 << (uint32_t)k_ra8_mipi_csi_emct_vlsien_shift),
                 (emct & (uint32_t)k_ra8_mipi_csi_emct_vlsien_mask));
  TEST_ASSERT((emct & (uint32_t)k_ra8_mipi_csi_emct_eotpen_mask) != 0UL);
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

  ra8_mipi_csi_config_t cfg = make_cfg();
  cfg.rx_irq_mask           = (uint32_t)k_ra8_mipi_csi_rxie_ractdete_mask;
  cfg.dl_irq_mask[0]        = (uint32_t)k_ra8_mipi_csi_dlie_eshe_mask;
  cfg.dl_irq_mask[1]        = (uint32_t)k_ra8_mipi_csi_dlie_esse_mask;
  for (uint8_t vc = 0U; vc < 16U; ++vc) {
    cfg.vc_irq_mask[vc] = ((uint32_t)1U << vc);
  }
  cfg.pm_irq_mask    = (uint32_t)k_ra8_mipi_csi_pmie_dsxe_mask;
  cfg.short_irq_mask = (uint32_t)k_ra8_mipi_csi_gsie_gnee_mask;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_csi_init(&cfg));

  TEST_ASSERT_EQ(cfg.rx_irq_mask, *ra8_mipi_csi_reg32(k_ra8_mipi_csi_off_rxie));
  TEST_ASSERT_EQ(cfg.dl_irq_mask[0], *ra8_mipi_csi_reg32(k_ra8_mipi_csi_off_dlie0));
  TEST_ASSERT_EQ(cfg.dl_irq_mask[1], *ra8_mipi_csi_reg32(k_ra8_mipi_csi_off_dlie1));
  for (uint8_t vc = 0U; vc < 16U; ++vc) {
    const ra8_mipi_csi_off_t off = ra8_mipi_csi_vc_off(k_ra8_mipi_csi_off_vcie0, vc);
    TEST_ASSERT_EQ(((uint32_t)1U << vc), *ra8_mipi_csi_reg32(off));
  }
  TEST_ASSERT_EQ(cfg.pm_irq_mask, *ra8_mipi_csi_reg32(k_ra8_mipi_csi_off_pmie));
  TEST_ASSERT_EQ(cfg.short_irq_mask, *ra8_mipi_csi_reg32(k_ra8_mipi_csi_off_gsie));
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
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_mipi_csi_init(nullptr));
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

  ra8_mipi_csi_config_t cfg = make_cfg();
  cfg.lanes                 = (ra8_mipi_csi_lanes_t)0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_mipi_csi_init(&cfg));

  cfg.lanes = (ra8_mipi_csi_lanes_t)3U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_mipi_csi_init(&cfg));
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

  ra8_mipi_csi_config_t cfg = make_cfg();
  cfg.vlsien                = (ra8_mipi_csi_vlsien_t)4U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_mipi_csi_init(&cfg));

  cfg                 = make_cfg();
  cfg.epd_long_spacer = k_mipi_csi_init_epd_long_spacer_8000; /* 16-bit, overflows 15-bit field. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_mipi_csi_init(&cfg));

  cfg                  = make_cfg();
  cfg.epd_short_spacer = k_mipi_csi_init_epd_long_spacer_8000;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_mipi_csi_init(&cfg));

  cfg                 = make_cfg();
  cfg.short_threshold = 0x10U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_mipi_csi_init(&cfg));
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

  const ra8_mipi_csi_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_csi_init(&cfg));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_csi_start_receive());

  TEST_ASSERT_EQ(
    k_ra8_mipi_csi_mct3_rxen_mask,
    (*ra8_mipi_csi_reg32(k_ra8_mipi_csi_off_mct3) & (uint32_t)k_ra8_mipi_csi_mct3_rxen_mask));

  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_mipi_csi_start_receive());

  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_csi_stop_receive());
  TEST_ASSERT_EQ(0, *ra8_mipi_csi_reg32(k_ra8_mipi_csi_off_mct3));
  TEST_ASSERT_EQ(
    k_ra8_mipi_csi_rtct_vsrst_mask,
    (*ra8_mipi_csi_reg32(k_ra8_mipi_csi_off_rtct) & (uint32_t)k_ra8_mipi_csi_rtct_vsrst_mask));
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
  const ra8_mipi_csi_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_csi_init(&cfg));

  /* RTST defaults to 0 in sim, so reset should succeed quickly. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_csi_reset());
  TEST_ASSERT_EQ(
    k_ra8_mipi_csi_rtct_vsrst_mask,
    (*ra8_mipi_csi_reg32(k_ra8_mipi_csi_off_rtct) & (uint32_t)k_ra8_mipi_csi_rtct_vsrst_mask));
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

  const ra8_mipi_csi_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_csi_init(&cfg));

  *ra8_mipi_csi_reg32(k_ra8_mipi_csi_off_rxst) = (uint32_t)k_ra8_mipi_csi_test_ractdet_marker;

  uint32_t mask = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_csi_get_status(&mask));
  TEST_ASSERT_EQ(k_ra8_mipi_csi_test_ractdet_marker, mask);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_csi_clear_status((uint32_t)k_ra8_mipi_csi_rxsc_ractdetc_mask));
  TEST_ASSERT_EQ(k_ra8_mipi_csi_rxsc_ractdetc_mask, *ra8_mipi_csi_reg32(k_ra8_mipi_csi_off_rxsc));

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_mipi_csi_get_status(nullptr));
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
  const ra8_mipi_csi_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_csi_init(&cfg));

  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_mipi_csi_set_rx_irq_enable((uint32_t)k_ra8_mipi_csi_rxie_ractdete_mask));
  TEST_ASSERT_EQ(k_ra8_mipi_csi_rxie_ractdete_mask, *ra8_mipi_csi_reg32(k_ra8_mipi_csi_off_rxie));
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
  const ra8_mipi_csi_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_csi_init(&cfg));

  *ra8_mipi_csi_reg32(k_ra8_mipi_csi_off_mist) = (uint32_t)k_ra8_mipi_csi_mist_rxs_mask;
  uint32_t out                                 = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_csi_get_module_irq_status(&out));
  TEST_ASSERT_EQ(k_ra8_mipi_csi_mist_rxs_mask, out);
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_mipi_csi_get_module_irq_status(nullptr));
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
  const ra8_mipi_csi_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_csi_init(&cfg));

  *ra8_mipi_csi_reg32(k_ra8_mipi_csi_off_mcg) = (uint32_t)k_ra8_mipi_csi_test_mcg_value;

  ra8_mipi_csi_module_info_t info = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_csi_get_module_info(&info));
  TEST_ASSERT_EQ(2, info.version);
  TEST_ASSERT_EQ(0xA, info.lanes_max);
  TEST_ASSERT_EQ(0x10, info.fifo_stages);
  TEST_ASSERT_EQ(k_ra8_mipi_csi_test_mcg_value, info.raw);

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_mipi_csi_get_module_info(nullptr));
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
  const ra8_mipi_csi_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_csi_init(&cfg));

  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_mipi_csi_set_data_type_filter((uint32_t)k_ra8_mipi_csi_test_dt_low_value,
                                                   (uint32_t)k_ra8_mipi_csi_test_dt_high_value));
  TEST_ASSERT_EQ(k_ra8_mipi_csi_test_dt_low_value, *ra8_mipi_csi_reg32(k_ra8_mipi_csi_off_dtel));
  TEST_ASSERT_EQ(k_ra8_mipi_csi_test_dt_high_value, *ra8_mipi_csi_reg32(k_ra8_mipi_csi_off_dteh));
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
  const ra8_mipi_csi_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_csi_init(&cfg));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_csi_set_ecc_mode(true, true));
  uint32_t mct0 = *ra8_mipi_csi_reg32(k_ra8_mipi_csi_off_mct0);
  TEST_ASSERT((mct0 & (uint32_t)k_ra8_mipi_csi_mct0_eccv13_mask) != 0UL);
  TEST_ASSERT((mct0 & (uint32_t)k_ra8_mipi_csi_mct0_lfsren_mask) != 0UL);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_csi_set_ecc_mode(false, false));
  mct0 = *ra8_mipi_csi_reg32(k_ra8_mipi_csi_off_mct0);
  TEST_ASSERT((mct0 & (uint32_t)k_ra8_mipi_csi_mct0_eccv13_mask) == 0UL);
  TEST_ASSERT((mct0 & (uint32_t)k_ra8_mipi_csi_mct0_lfsren_mask) == 0UL);

  /* Reject while running. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_csi_start_receive());
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_mipi_csi_set_ecc_mode(true, true));
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
  const ra8_mipi_csi_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_csi_init(&cfg));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_csi_set_frame_error_mode(true, true, true));
  const uint32_t mct0 = *ra8_mipi_csi_reg32(k_ra8_mipi_csi_off_mct0);
  TEST_ASSERT((mct0 & (uint32_t)k_ra8_mipi_csi_mct0_zlmd_mask) != 0UL);
  TEST_ASSERT((mct0 & (uint32_t)k_ra8_mipi_csi_mct0_edmd_mask) != 0UL);
  TEST_ASSERT((mct0 & (uint32_t)k_ra8_mipi_csi_mct0_rvmd_mask) != 0UL);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_csi_start_receive());
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_mipi_csi_set_frame_error_mode(false, false, false));
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
  const ra8_mipi_csi_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_csi_init(&cfg));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_csi_set_epd(true, true, 0x123U, 0x456U));
  const uint32_t epct = *ra8_mipi_csi_reg32(k_ra8_mipi_csi_off_epct);
  TEST_ASSERT((epct & (uint32_t)k_ra8_mipi_csi_epct_epden_mask) != 0UL);
  TEST_ASSERT((epct & (uint32_t)k_ra8_mipi_csi_epct_epdop_mask) != 0UL);
  TEST_ASSERT_EQ(0x123, (epct & (uint32_t)k_ra8_mipi_csi_epct_slp_mask));

  /* Reject bad spacers. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_mipi_csi_set_epd(true, true, 0x8000U, 0U));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_mipi_csi_set_epd(true, true, 0U, 0x8000U));
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
  const ra8_mipi_csi_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_csi_init(&cfg));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_csi_set_lrte(k_ra8_mipi_csi_vlsien_x4, true));
  const uint32_t emct = *ra8_mipi_csi_reg32(k_ra8_mipi_csi_off_emct);
  TEST_ASSERT((emct & (uint32_t)k_ra8_mipi_csi_emct_eotpen_mask) != 0UL);
  TEST_ASSERT_EQ(((uint32_t)k_ra8_mipi_csi_vlsien_x4 << (uint32_t)k_ra8_mipi_csi_emct_vlsien_shift),
                 (emct & (uint32_t)k_ra8_mipi_csi_emct_vlsien_mask));

  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_mipi_csi_set_lrte((ra8_mipi_csi_vlsien_t)4U, false));
  TEST_END("mipi_csi set lrte");
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

  const ra8_mipi_csi_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_csi_init(&cfg));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_csi_enter_stop());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_csi_exit_stop());
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

  const ra8_mipi_csi_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_csi_init(&cfg));
  /* Pre-set IRQ enables so we can verify they get cleared. */
  *ra8_mipi_csi_reg32(k_ra8_mipi_csi_off_rxie)  = k_mipi_csi_init_sentinel_ffffffff;
  *ra8_mipi_csi_reg32(k_ra8_mipi_csi_off_dlie0) = k_mipi_csi_init_sentinel_ffffffff;
  *ra8_mipi_csi_reg32(k_ra8_mipi_csi_off_pmie)  = k_mipi_csi_init_sentinel_ffffffff;
  *ra8_mipi_csi_reg32(k_ra8_mipi_csi_off_gsie)  = k_mipi_csi_init_sentinel_ffffffff;
  for (uint8_t vc = 0U; vc < 16U; ++vc) {
    const ra8_mipi_csi_off_t off = ra8_mipi_csi_vc_off(k_ra8_mipi_csi_off_vcie0, vc);
    *ra8_mipi_csi_reg32(off)     = k_mipi_csi_init_sentinel_ffffffff;
  }

  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_csi_deinit());

  TEST_ASSERT_EQ(0, *ra8_mipi_csi_reg32(k_ra8_mipi_csi_off_mct3));
  TEST_ASSERT_EQ(0, *ra8_mipi_csi_reg32(k_ra8_mipi_csi_off_rxie));
  TEST_ASSERT_EQ(0, *ra8_mipi_csi_reg32(k_ra8_mipi_csi_off_dlie0));
  TEST_ASSERT_EQ(0, *ra8_mipi_csi_reg32(k_ra8_mipi_csi_off_dlie1));
  TEST_ASSERT_EQ(0, *ra8_mipi_csi_reg32(k_ra8_mipi_csi_off_pmie));
  TEST_ASSERT_EQ(0, *ra8_mipi_csi_reg32(k_ra8_mipi_csi_off_gsie));
  for (uint8_t vc = 0U; vc < 16U; ++vc) {
    const ra8_mipi_csi_off_t off = ra8_mipi_csi_vc_off(k_ra8_mipi_csi_off_vcie0, vc);
    TEST_ASSERT_EQ(0, *ra8_mipi_csi_reg32(off));
  }
  TEST_END("mipi_csi deinit");
}

/**
 * @test test_mcdc_validate_lanes
 *
 * @par MC/DC:
 * Decision: `if ((cfg->lanes != k_ra8_mipi_csi_lanes_1) &&
 *               (cfg->lanes != k_ra8_mipi_csi_lanes_2))`
 * (2 conditions, libs/ra8_hal/src/ra8_mipi_csi.c line 283)
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

  ra8_mipi_csi_config_t cfg = make_cfg();

  /* Vector 1: lanes=1 -> decision F -> ok. */
  cfg.lanes = k_ra8_mipi_csi_lanes_1;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_csi_init(&cfg));
  prep();

  /* Vector 2: lanes=2 -> decision F -> ok. */
  cfg       = make_cfg();
  cfg.lanes = k_ra8_mipi_csi_lanes_2;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_csi_init(&cfg));
  prep();

  /* Vector 3: lanes=3 -> decision T -> invalid_arg. */
  cfg       = make_cfg();
  cfg.lanes = (ra8_mipi_csi_lanes_t)3U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_mipi_csi_init(&cfg));

  TEST_END("mipi_csi init MC/DC: lanes!=1 && lanes!=2");
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
  test_init_happy,
  test_init_ecc_and_frame_bits,
  test_init_dt_and_short_packet,
  test_init_epct_emct,
  test_init_irq_masks,
  test_init_null_cfg,
  test_init_bad_lanes,
  test_init_bad_args,
  test_start_then_stop,
  test_reset,
  test_status_get_clear,
  test_set_rx_irq_enable,
  test_get_module_irq_status,
  test_get_module_info,
  test_set_data_type_filter,
  test_set_ecc_mode,
  test_set_frame_error_mode,
  test_set_epd,
  test_set_lrte,
  test_power_transition,
  test_deinit,
  test_mcdc_validate_lanes,
};

int32_t main(void)
{
  for (size_t i = 0U; i < (sizeof s_test_roster / sizeof s_test_roster[0]); ++i) {
    s_test_roster[i]();
  }
  (void)fprintf(stderr, "[OK ] test_ra8_mipi_csi_init.c\n");
  return 0;
}
