/**
 * @file test_ra8_mipi_csi_events.c
 * @brief Unit tests for MIPI CSI-2 receiver status paths, FIFO and dispatch
 *
 * @details
 * Split sibling of the original test_ra8_mipi_csi.c suite covering
 * the event surface of ra8_mipi_csi.c against the host-side
 * simulated MMIO (``ra8_sim_mmap``):
 *
 * - per-data-lane / per-VC / power-management status get + clear
 * - short-packet FIFO configure, status, read, clear and re-enable
 * - full ISR-dispatch fan-out for the RX, DL, VC, PM and GST vectors
 * - the sweep-6 surface: virtual-channel filter, per-VC data format,
 *   and the error-report handler incl. null-safety
 *
 * Sibling suite: test_ra8_mipi_csi_init.c (bring-up + config setters
 * + power + the lane-validation MC/DC vector).
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
 * @enum t_csi_vc_t
 * @brief Virtual-channel indexing of the per-channel event counters.
 *
 * @details
 * CSI-2 defines 16 virtual channels; the fixture adds one more slot to hold
 * events reported with the broadcast channel, so every dispatch lands in
 * exactly one counter and none is silently dropped.
 */
typedef enum : uint8_t {
  k_t_vc_slots     = 17U,   /**< Counter slots: 16 channels plus broadcast.   */
  k_t_vc_broadcast = 0xFFU, /**< The broadcast channel id, mapped to slot 16. */
  k_t_mist_pattern = 0x21U, /**< Status pattern staged in MIST, chosen so two
                                 non-adjacent status bits are set at once.     */
} t_csi_vc_t;

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
static uint32_t s_vc_calls[k_t_vc_slots];

/**
 * @var s_vc_last_mask
 * @brief Last VCST value per VC.
 */
static uint32_t s_vc_last_mask[k_t_vc_slots];

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
  uint8_t idx = (vc == k_t_vc_broadcast) ? 16U : vc;
  if (idx < k_t_vc_slots) {
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
 * @brief Reset sim mmap + ra8_mstp + callback statistics.
 */
static void prep(void)
{
  ra8_sim_mmap_reset();
  (void)ra8_mstp_init();
  s_cb_count     = 0U;
  s_cb_last_rxst = 0U;
  s_cb_last_ctx  = nullptr;
  for (uint8_t i = 0U; i < 2U; ++i) {
    s_dl_calls[i]     = 0U;
    s_dl_last_mask[i] = 0U;
  }
  for (uint8_t i = 0U; i < k_t_vc_slots; ++i) {
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
static void test_dl_status_paths(void)
{
  TEST_BEGIN("mipi_csi dl status get/clear/irq");
  prep();
  const ra8_mipi_csi_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_csi_init(&cfg));

  *ra8_mipi_csi_reg32(k_ra8_mipi_csi_off_dlst0) = (uint32_t)k_ra8_mipi_csi_dlst_esh_mask;
  *ra8_mipi_csi_reg32(k_ra8_mipi_csi_off_dlst1) = (uint32_t)k_ra8_mipi_csi_dlst_ess_mask;

  uint32_t v = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_csi_dl_get_status(0U, &v));
  TEST_ASSERT_EQ(k_ra8_mipi_csi_dlst_esh_mask, v);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_csi_dl_get_status(1U, &v));
  TEST_ASSERT_EQ(k_ra8_mipi_csi_dlst_ess_mask, v);

  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_mipi_csi_dl_clear_status(0U, (uint32_t)k_ra8_mipi_csi_dlsc_eshc_mask));
  TEST_ASSERT_EQ(k_ra8_mipi_csi_dlsc_eshc_mask, *ra8_mipi_csi_reg32(k_ra8_mipi_csi_off_dlsc0));

  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_mipi_csi_dl_set_irq_enable(0U, (uint32_t)k_ra8_mipi_csi_dlie_all_mask));
  TEST_ASSERT_EQ(k_ra8_mipi_csi_dlie_all_mask, *ra8_mipi_csi_reg32(k_ra8_mipi_csi_off_dlie0));

  /* Bad lane index. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_mipi_csi_dl_get_status(2U, &v));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_mipi_csi_dl_clear_status(2U, 0U));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_mipi_csi_dl_set_irq_enable(2U, 0U));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_mipi_csi_dl_get_status(0U, nullptr));
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
  const ra8_mipi_csi_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_csi_init(&cfg));

  for (uint8_t vc = 0U; vc < 16U; ++vc) {
    const ra8_mipi_csi_off_t st_off = ra8_mipi_csi_vc_off(k_ra8_mipi_csi_off_vcst0, vc);
    *ra8_mipi_csi_reg32(st_off)     = ((uint32_t)1U << vc);

    uint32_t v = 0U;
    TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_csi_vc_get_status(vc, &v));
    TEST_ASSERT_EQ(((uint32_t)1U << vc), v);

    TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_csi_vc_clear_status(vc, ((uint32_t)1U << vc)));
    const ra8_mipi_csi_off_t clr_off = ra8_mipi_csi_vc_off(k_ra8_mipi_csi_off_vcsc0, vc);
    TEST_ASSERT_EQ(((uint32_t)1U << vc), *ra8_mipi_csi_reg32(clr_off));

    TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_csi_vc_set_irq_enable(vc, ((uint32_t)1U << vc)));
    const ra8_mipi_csi_off_t ie_off = ra8_mipi_csi_vc_off(k_ra8_mipi_csi_off_vcie0, vc);
    TEST_ASSERT_EQ(((uint32_t)1U << vc), *ra8_mipi_csi_reg32(ie_off));
  }

  uint32_t v = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_mipi_csi_vc_get_status(16U, &v));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_mipi_csi_vc_clear_status(16U, 0U));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_mipi_csi_vc_set_irq_enable(16U, 0U));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_mipi_csi_vc_get_status(0U, nullptr));
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
  const ra8_mipi_csi_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_csi_init(&cfg));

  *ra8_mipi_csi_reg32(k_ra8_mipi_csi_off_pmst) = (uint32_t)k_ra8_mipi_csi_test_pmst_marker;

  uint32_t v = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_csi_pm_get_status(&v));
  TEST_ASSERT_EQ(k_ra8_mipi_csi_test_pmst_marker, v);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_csi_pm_clear_status((uint32_t)k_ra8_mipi_csi_pmsc_all_mask));
  TEST_ASSERT_EQ(k_ra8_mipi_csi_pmsc_all_mask, *ra8_mipi_csi_reg32(k_ra8_mipi_csi_off_pmsc));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_csi_pm_set_irq_enable((uint32_t)k_ra8_mipi_csi_pmie_all_mask));
  TEST_ASSERT_EQ(k_ra8_mipi_csi_pmie_all_mask, *ra8_mipi_csi_reg32(k_ra8_mipi_csi_off_pmie));

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_mipi_csi_pm_get_status(nullptr));
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
  const ra8_mipi_csi_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_csi_init(&cfg));

  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_mipi_csi_short_packet_configure((uint8_t)k_ra8_mipi_csi_test_threshold, true));
  const uint32_t gsct = *ra8_mipi_csi_reg32(k_ra8_mipi_csi_off_gsct);
  TEST_ASSERT_EQ(k_ra8_mipi_csi_test_threshold, (gsct & (uint32_t)k_ra8_mipi_csi_gsct_shth_mask));
  TEST_ASSERT((gsct & (uint32_t)k_ra8_mipi_csi_gsct_gfif_mask) != 0UL);

  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_mipi_csi_short_packet_configure(0x10U, false));

  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_mipi_csi_short_packet_set_irq_enable((uint32_t)k_ra8_mipi_csi_gsie_all_mask));
  TEST_ASSERT_EQ(k_ra8_mipi_csi_gsie_all_mask, *ra8_mipi_csi_reg32(k_ra8_mipi_csi_off_gsie));
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
  const ra8_mipi_csi_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_csi_init(&cfg));

  *ra8_mipi_csi_reg32(k_ra8_mipi_csi_off_gsst) =
    (uint32_t)k_ra8_mipi_csi_gsst_gov_mask | (uint32_t)k_ra8_mipi_csi_gsst_gne_mask;
  uint32_t v = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_csi_short_packet_get_status(&v));
  TEST_ASSERT((v & (uint32_t)k_ra8_mipi_csi_gsst_gov_mask) != 0UL);

  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_mipi_csi_short_packet_clear_status((uint32_t)k_ra8_mipi_csi_gssc_govc_mask));
  TEST_ASSERT_EQ(k_ra8_mipi_csi_gssc_govc_mask, *ra8_mipi_csi_reg32(k_ra8_mipi_csi_off_gssc));

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_mipi_csi_short_packet_get_status(nullptr));
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
  const ra8_mipi_csi_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_csi_init(&cfg));

  /* Empty FIFO (PNUM = 0) -> empty error. */
  ra8_mipi_csi_short_packet_t pkt = {};
  TEST_ASSERT_EQ(k_ra8_err_empty, ra8_mipi_csi_read_short_packet(&pkt));

  /* PNUM = 1, GSHT loaded with a known value -> decoded fields match. */
  *ra8_mipi_csi_reg32(k_ra8_mipi_csi_off_gsst) =
    (uint32_t)(1U << (uint32_t)k_ra8_mipi_csi_gsst_pnum_shift);
  *ra8_mipi_csi_reg32(k_ra8_mipi_csi_off_gsht) = (uint32_t)k_ra8_mipi_csi_test_pkt_value;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_csi_read_short_packet(&pkt));
  TEST_ASSERT_EQ(0x3412, pkt.payload);
  TEST_ASSERT_EQ(0x08, pkt.data_type);
  TEST_ASSERT_EQ(0x0A, pkt.vc);
  TEST_ASSERT_EQ(k_ra8_mipi_csi_test_pkt_value, pkt.raw);

  /* GSIU.FINC must have been pulsed. */
  TEST_ASSERT((*ra8_mipi_csi_reg32(k_ra8_mipi_csi_off_gsiu) &
               (uint32_t)k_ra8_mipi_csi_gsiu_finc_mask) != 0UL);

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_mipi_csi_read_short_packet(nullptr));
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
  const ra8_mipi_csi_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_csi_init(&cfg));

  /* Pre-load GCD in GSST so the spinner exits immediately. */
  *ra8_mipi_csi_reg32(k_ra8_mipi_csi_off_gsst) = (uint32_t)k_ra8_mipi_csi_gsst_gcd_mask;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_csi_short_packet_clear_fifo());
  /* GSIU.GFCLR must have been released back to 0. */
  TEST_ASSERT_EQ(0, *ra8_mipi_csi_reg32(k_ra8_mipi_csi_off_gsiu));

  /* Now without GCD set -> hw_timeout. */
  *ra8_mipi_csi_reg32(k_ra8_mipi_csi_off_gsst) = 0U;
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout, ra8_mipi_csi_short_packet_clear_fifo());
  TEST_ASSERT_EQ(0, *ra8_mipi_csi_reg32(k_ra8_mipi_csi_off_gsiu));
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
  const ra8_mipi_csi_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_csi_init(&cfg));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_csi_short_packet_re_enable_store());
  TEST_ASSERT((*ra8_mipi_csi_reg32(k_ra8_mipi_csi_off_gsiu) &
               (uint32_t)k_ra8_mipi_csi_gsiu_gfen_mask) != 0UL);
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

  const ra8_mipi_csi_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_csi_init(&cfg));
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_mipi_csi_attach_handler(stub_event_cb, (void*)(uintptr_t)k_ra8_mipi_csi_test_ctx_marker));

  *ra8_mipi_csi_reg32(k_ra8_mipi_csi_off_rxst) = (uint32_t)k_ra8_mipi_csi_test_ractdet_marker;
  ra8_mipi_csi_dispatch();

  TEST_ASSERT_EQ(1, s_cb_count);
  TEST_ASSERT_EQ(k_ra8_mipi_csi_test_ractdet_marker, s_cb_last_rxst);
  TEST_ASSERT_EQ((uintptr_t)k_ra8_mipi_csi_test_ctx_marker, (uintptr_t)s_cb_last_ctx);

  TEST_ASSERT_EQ(k_ra8_mipi_csi_rxsc_ractdetc_mask, *ra8_mipi_csi_reg32(k_ra8_mipi_csi_off_rxsc));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_csi_attach_handler(nullptr, nullptr));
  ra8_mipi_csi_dispatch();
  TEST_ASSERT_EQ(1, s_cb_count);
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
  const ra8_mipi_csi_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_csi_init(&cfg));
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_mipi_csi_attach_dl_handler(stub_dl_cb, (void*)(uintptr_t)k_ra8_mipi_csi_test_dl_marker));

  /* Both lanes flagged in MIST, distinct DLST values. */
  *ra8_mipi_csi_reg32(k_ra8_mipi_csi_off_mist) =
    (uint32_t)k_ra8_mipi_csi_mist_dl0s_mask | (uint32_t)k_ra8_mipi_csi_mist_dl1s_mask;
  *ra8_mipi_csi_reg32(k_ra8_mipi_csi_off_dlst0) = (uint32_t)k_ra8_mipi_csi_dlst_esh_mask;
  *ra8_mipi_csi_reg32(k_ra8_mipi_csi_off_dlst1) = (uint32_t)k_ra8_mipi_csi_dlst_ess_mask;

  ra8_mipi_csi_dispatch_dl();

  TEST_ASSERT_EQ(1, s_dl_calls[0]);
  TEST_ASSERT_EQ(1, s_dl_calls[1]);
  TEST_ASSERT_EQ(k_ra8_mipi_csi_dlst_esh_mask, s_dl_last_mask[0]);
  TEST_ASSERT_EQ(k_ra8_mipi_csi_dlst_ess_mask, s_dl_last_mask[1]);
  /* DLSC should have been written with the W1C-able subset. */
  TEST_ASSERT_EQ(((uint32_t)k_ra8_mipi_csi_dlst_esh_mask & (uint32_t)k_ra8_mipi_csi_dlsc_all_mask),
                 *ra8_mipi_csi_reg32(k_ra8_mipi_csi_off_dlsc0));
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
  const ra8_mipi_csi_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_csi_init(&cfg));
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_mipi_csi_attach_vc_handler(stub_vc_cb, (void*)(uintptr_t)k_ra8_mipi_csi_test_vc_marker));

  /* Flag VC0 + VC5 in MIST. */
  *ra8_mipi_csi_reg32(k_ra8_mipi_csi_off_mist) = ((uint32_t)k_t_mist_pattern)
                                                 << (uint32_t)k_ra8_mipi_csi_mist_vc_shift;

  /* VC0 carries a CRC error (per-VC). */
  const ra8_mipi_csi_off_t st0 = ra8_mipi_csi_vc_off(k_ra8_mipi_csi_off_vcst0, 0U);
  *ra8_mipi_csi_reg32(st0)     = (uint32_t)k_ra8_mipi_csi_vcst_crc_mask;
  /* VC5 carries an MLF (generic-err) + LSR (per-VC). */
  const ra8_mipi_csi_off_t st5 = ra8_mipi_csi_vc_off(k_ra8_mipi_csi_off_vcst0, 5U);
  *ra8_mipi_csi_reg32(st5) =
    (uint32_t)k_ra8_mipi_csi_vcst_mlf_mask | (uint32_t)k_ra8_mipi_csi_vcst_lsr_mask;

  ra8_mipi_csi_dispatch_vc();

  TEST_ASSERT_EQ(1, s_vc_calls[0]);
  TEST_ASSERT_EQ(k_ra8_mipi_csi_vcst_crc_mask, s_vc_last_mask[0]);
  TEST_ASSERT_EQ(1, s_vc_calls[5]);
  TEST_ASSERT_EQ(k_ra8_mipi_csi_vcst_lsr_mask, s_vc_last_mask[5]);
  /* Generic summary callback (vc=0xFF -> idx 16) carries MLF only. */
  TEST_ASSERT_EQ(1, s_vc_calls[16]);
  TEST_ASSERT_EQ(k_ra8_mipi_csi_vcst_mlf_mask, s_vc_last_mask[16]);

  /* VCSC for VC0 must reflect the per-vc clear mask. */
  const ra8_mipi_csi_off_t clr0 = ra8_mipi_csi_vc_off(k_ra8_mipi_csi_off_vcsc0, 0U);
  TEST_ASSERT_EQ(k_ra8_mipi_csi_vcst_crc_mask, *ra8_mipi_csi_reg32(clr0));
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
  const ra8_mipi_csi_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_csi_init(&cfg));
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_mipi_csi_attach_pm_handler(stub_pm_cb, (void*)(uintptr_t)k_ra8_mipi_csi_test_pm_marker));

  *ra8_mipi_csi_reg32(k_ra8_mipi_csi_off_pmst) = (uint32_t)k_ra8_mipi_csi_test_pmst_marker;
  ra8_mipi_csi_dispatch_pm();

  TEST_ASSERT_EQ(1, s_pm_calls);
  TEST_ASSERT_EQ(k_ra8_mipi_csi_test_pmst_marker, s_pm_last_mask);
  /* PMSC must have been written with only the lower 8 bits. */
  TEST_ASSERT_EQ(
    ((uint32_t)k_ra8_mipi_csi_test_pmst_marker & (uint32_t)k_ra8_mipi_csi_pmst_w1c_mask),
    *ra8_mipi_csi_reg32(k_ra8_mipi_csi_off_pmsc));
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
  const ra8_mipi_csi_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_csi_init(&cfg));
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_mipi_csi_attach_short_packet_handler(stub_gst_cb,
                                             (void*)(uintptr_t)k_ra8_mipi_csi_test_gst_marker));

  *ra8_mipi_csi_reg32(k_ra8_mipi_csi_off_gsst) =
    (uint32_t)k_ra8_mipi_csi_gsst_gov_mask | (uint32_t)k_ra8_mipi_csi_gsst_gne_mask;
  ra8_mipi_csi_dispatch_short_packet();

  TEST_ASSERT_EQ(1, s_gst_calls);
  TEST_ASSERT((s_gst_last_mask & (uint32_t)k_ra8_mipi_csi_gsst_gov_mask) != 0UL);
  /* GSSC should hold only the GOV-clear bit. */
  TEST_ASSERT_EQ(k_ra8_mipi_csi_gssc_govc_mask, *ra8_mipi_csi_reg32(k_ra8_mipi_csi_off_gssc));
  TEST_END("mipi_csi dispatch short packet");
}

/* ===========================================================================
 * Sweep 6: virtual-channel filter, per-VC data-format selection, error
 * handler. NULL-arg coverage included.
 * ===========================================================================
 */

typedef enum : uint32_t {
  k_ra8_mipi_csi_test_vc_keep_mask = 0x0005U,    /**< Keep VC0 + VC2.                  */
  k_ra8_mipi_csi_test_vc_seed      = 0xABCDUL,   /**< Pre-seeded VCIE val.             */
  k_ra8_mipi_csi_test_err_marker   = 0xDEC0DE0U, /**< RA8 mipi csi test error marker.  */
  k_ra8_mipi_csi_test_err_ctx_val  = 0xCAFEU,    /**< RA8 mipi csi test error ctx val. */
} ra8_mipi_csi_sw6_const_t;

static uint32_t                    s_err_calls;
static ra8_mipi_csi_error_report_t s_err_last;
static void*                       s_err_last_ctx;

static void stub_err_cb(void* ctx, const ra8_mipi_csi_error_report_t* report)
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
  const ra8_mipi_csi_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_csi_init(&cfg));

  /* Seed VCIE for every channel so we can confirm the masking. */
  for (uint8_t vc = 0U; vc < 16U; ++vc) {
    const ra8_mipi_csi_off_t off = ra8_mipi_csi_vc_off(k_ra8_mipi_csi_off_vcie0, vc);
    *ra8_mipi_csi_reg32(off)     = (uint32_t)k_ra8_mipi_csi_test_vc_seed;
  }

  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_mipi_csi_set_virtual_channels((uint16_t)k_ra8_mipi_csi_test_vc_keep_mask));

  /* VC0 + VC2 retain (saved was zero, so default mask was restored). */
  TEST_ASSERT_EQ(k_ra8_mipi_csi_test_vc_seed,
                 *ra8_mipi_csi_reg32(ra8_mipi_csi_vc_off(k_ra8_mipi_csi_off_vcie0, 0U)));
  /* VC1, VC3..VC15 zeroed. */
  TEST_ASSERT_EQ(0, *ra8_mipi_csi_reg32(ra8_mipi_csi_vc_off(k_ra8_mipi_csi_off_vcie0, 1U)));
  TEST_ASSERT_EQ(0, *ra8_mipi_csi_reg32(ra8_mipi_csi_vc_off(k_ra8_mipi_csi_off_vcie0, 5U)));

  /* Re-enabling VC1 restores the saved value. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_csi_set_virtual_channels((uint16_t)0xFFFFU));
  TEST_ASSERT_EQ(k_ra8_mipi_csi_test_vc_seed,
                 *ra8_mipi_csi_reg32(ra8_mipi_csi_vc_off(k_ra8_mipi_csi_off_vcie0, 1U)));
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
  const ra8_mipi_csi_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_csi_init(&cfg));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_mipi_csi_set_virtual_channels(0U));
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
  const ra8_mipi_csi_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_csi_init(&cfg));

  /* VC0 -> RGB888 (DTEH bit). */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_csi_set_data_format(0U, k_ra8_mipi_csi_format_rgb888));
  TEST_ASSERT_EQ(k_ra8_mipi_csi_dteh_rgb888_mask, *ra8_mipi_csi_reg32(k_ra8_mipi_csi_off_dteh));
  TEST_ASSERT_EQ(0, *ra8_mipi_csi_reg32(k_ra8_mipi_csi_off_dtel));

  /* VC1 -> YUV422_8 (DTEL bit). DTEH stays set. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_csi_set_data_format(1U, k_ra8_mipi_csi_format_yuv422_8));
  TEST_ASSERT_EQ(k_ra8_mipi_csi_dtel_yuv422_8_mask, *ra8_mipi_csi_reg32(k_ra8_mipi_csi_off_dtel));
  TEST_ASSERT_EQ(k_ra8_mipi_csi_dteh_rgb888_mask, *ra8_mipi_csi_reg32(k_ra8_mipi_csi_off_dteh));

  /* Disable VC0 -- DTEH clears. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_csi_set_data_format(0U, k_ra8_mipi_csi_format_off));
  TEST_ASSERT_EQ(0, *ra8_mipi_csi_reg32(k_ra8_mipi_csi_off_dteh));
  TEST_ASSERT_EQ(k_ra8_mipi_csi_dtel_yuv422_8_mask, *ra8_mipi_csi_reg32(k_ra8_mipi_csi_off_dtel));
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
  const ra8_mipi_csi_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_csi_init(&cfg));

  /* VC out of range. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_mipi_csi_set_data_format(16U, k_ra8_mipi_csi_format_rgb888));

  /* Format unsupported on RA8D2 DTEL/DTEH (RAW10). */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_mipi_csi_set_data_format(0U, k_ra8_mipi_csi_format_raw10));
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
  const ra8_mipi_csi_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_csi_init(&cfg));

  s_err_calls           = 0U;
  s_err_last_ctx        = nullptr;
  void* const ctx_token = (void*)(uintptr_t)k_ra8_mipi_csi_test_err_ctx_val;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_csi_attach_error_handler(stub_err_cb, ctx_token));

  /* Drive a CRC + ECC-corrected event on VC0 via VCST seeding + MIST set.
   * MIST.VC[31:16] -- bit 16 = VC0 source flag. */
  *ra8_mipi_csi_reg32(k_ra8_mipi_csi_off_mist) =
    (uint32_t)((uint32_t)1U << (uint32_t)k_ra8_mipi_csi_mist_vc_shift);
  const ra8_mipi_csi_off_t st_off = ra8_mipi_csi_vc_off(k_ra8_mipi_csi_off_vcst0, 0U);
  *ra8_mipi_csi_reg32(st_off) =
    (uint32_t)k_ra8_mipi_csi_vcst_crc_mask | (uint32_t)k_ra8_mipi_csi_vcst_ecc_mask;

  ra8_mipi_csi_dispatch_vc();
  TEST_ASSERT_EQ(1, s_err_calls);
  TEST_ASSERT(s_err_last_ctx == ctx_token);
  TEST_ASSERT(s_err_last.crc_error);
  TEST_ASSERT(s_err_last.ecc_corrected);
  TEST_ASSERT(!s_err_last.ecc_two_bit_error);
  TEST_ASSERT_EQ(0, s_err_last.vc);

  /* Detach -- subsequent dispatches must be silent. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_csi_attach_error_handler(nullptr, nullptr));
  *ra8_mipi_csi_reg32(k_ra8_mipi_csi_off_mist) =
    (uint32_t)((uint32_t)1U << (uint32_t)k_ra8_mipi_csi_mist_vc_shift);
  *ra8_mipi_csi_reg32(st_off) = (uint32_t)k_ra8_mipi_csi_vcst_crc_mask;
  ra8_mipi_csi_dispatch_vc();
  TEST_ASSERT_EQ(1, s_err_calls);
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
  const ra8_mipi_csi_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_csi_init(&cfg));

  s_err_calls = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_csi_attach_error_handler(stub_err_cb, nullptr));

  /* Seed a frame-start event (not an error). */
  *ra8_mipi_csi_reg32(k_ra8_mipi_csi_off_mist) =
    (uint32_t)((uint32_t)1U << (uint32_t)k_ra8_mipi_csi_mist_vc_shift);
  const ra8_mipi_csi_off_t st_off = ra8_mipi_csi_vc_off(k_ra8_mipi_csi_off_vcst0, 0U);
  *ra8_mipi_csi_reg32(st_off)     = (uint32_t)k_ra8_mipi_csi_vcst_fsr_mask;
  ra8_mipi_csi_dispatch_vc();
  TEST_ASSERT_EQ(0, s_err_calls);
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
  const ra8_mipi_csi_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_csi_init(&cfg));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_csi_attach_error_handler(nullptr, nullptr));
  /* Dispatch with no handler is a no-op. */
  *ra8_mipi_csi_reg32(k_ra8_mipi_csi_off_mist) =
    (uint32_t)((uint32_t)1U << (uint32_t)k_ra8_mipi_csi_mist_vc_shift);
  const ra8_mipi_csi_off_t st_off = ra8_mipi_csi_vc_off(k_ra8_mipi_csi_off_vcst0, 0U);
  *ra8_mipi_csi_reg32(st_off)     = (uint32_t)k_ra8_mipi_csi_vcst_crc_mask;
  ra8_mipi_csi_dispatch_vc();
  TEST_END("mipi_csi attach_error_handler accepts NULL detach");
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
  test_dl_status_paths,
  test_vc_status_paths,
  test_pm_status_paths,
  test_short_packet_configure,
  test_short_packet_status,
  test_read_short_packet,
  test_short_packet_clear_fifo,
  test_short_packet_re_enable,
  test_attach_and_dispatch,
  test_dispatch_dl,
  test_dispatch_vc,
  test_dispatch_pm,
  test_dispatch_short_packet,
  test_set_virtual_channels,
  test_set_virtual_channels_empty_mask,
  test_set_data_format_per_vc,
  test_set_data_format_bad_args,
  test_attach_error_handler,
  test_error_handler_no_errors_silent,
  test_attach_error_handler_null_safe,
};

int32_t main(void)
{
  for (size_t i = 0U; i < (sizeof s_test_roster / sizeof s_test_roster[0]); ++i) {
    s_test_roster[i]();
  }
  (void)fprintf(stderr, "[OK ] test_ra8_mipi_csi_events.c\n");
  return 0;
}
