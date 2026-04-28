/**
 * @file test_ra_mipi_phy.c
 * @brief Unit tests for ra_mipi_phy.c (MIPI D-PHY driver)
 *
 * @details
 * The host test fixture pre-seeds DPHYSFR with PWRSF | PLLSF before
 * calling ``ra_mipi_phy_init`` so the LDO and PLL stabilisation
 * polls complete on the first iteration. Real silicon needs ~us
 * for the LDO and ~tens of us for the PLL, both validated against
 * HUM Ch 64.3.1 p 3837.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8d2_mipi_phy_regs.h"
#include "ra_err.h"
#include "ra_mipi_phy.h"
#include "ra_sim_mmap.h"
#include "unity_minimal.h"

typedef enum : uint8_t {
  k_test_mipi_phy_pclka_mhz  = 100U, /**< PCLKA = 100 MHz (within 40..125). */
  k_test_mipi_phy_escdiv     = 4U,   /**< Arbitrary in-range escape divisor. */
  k_test_mipi_phy_escdiv_hi  = 31U,  /**< Upper bound for ESCDIV[4:0].       */
  k_test_mipi_phy_escdiv_bad = 32U,  /**< First out-of-range escape value.   */
  k_test_mipi_phy_pclka_lo   = 39U,  /**< Just below the 40 MHz floor.       */
  k_test_mipi_phy_pclka_hi   = 126U, /**< Just above the 125 MHz ceiling.    */
} test_mipi_phy_const_t;

typedef enum : uint16_t {
  k_test_mipi_phy_nmul_int    = 100U, /**< Mid-range NMUL (HUM allows 40..375). */
  k_test_mipi_phy_nmul_bad_lo = 39U,  /**< Below floor.                         */
  k_test_mipi_phy_nmul_bad_hi = 376U, /**< Above ceiling.                       */
  k_test_mipi_phy_rate_mbps   = 250U, /**< Sample lane rate.                    */
} test_mipi_phy_pll_t;

/* ---- Shared callback bookkeeping ----------------------------------------- */

static uint32_t s_phy_cb_count;
static uint32_t s_phy_cb_last_sfr;
static void*    s_phy_cb_last_ctx;
static uint8_t  s_phy_cb_last_event;

static void stub_phy_cb(void* ctx, ra_mipi_phy_event_t event, uint32_t sfr)
{
  ++s_phy_cb_count;
  s_phy_cb_last_sfr   = sfr;
  s_phy_cb_last_ctx   = ctx;
  s_phy_cb_last_event = (uint8_t)event;
}

/* ---- Fixture helpers ----------------------------------------------------- */

static void prep_fixture(void)
{
  ra_sim_mmap_reset();
  s_phy_cb_count      = 0U;
  s_phy_cb_last_sfr   = 0U;
  s_phy_cb_last_ctx   = nullptr;
  s_phy_cb_last_event = 0xFFU;
  /* Pre-seed DPHYSFR so the LDO + PLL polls return immediately. */
  *ra_mipi_phy_reg32(k_ra_mipi_phy_off_sfr) =
    (uint32_t)k_ra_mipi_phy_sfr_pwrsf | (uint32_t)k_ra_mipi_phy_sfr_pllsf;
  /* Detach any callback left over from a previous test. */
  (void)ra_mipi_phy_attach_handler(nullptr, nullptr);
  /* Reset module state via reset(). */
  (void)ra_mipi_phy_reset();
  /* Re-seed after reset wiped DPHYSFR. */
  *ra_mipi_phy_reg32(k_ra_mipi_phy_off_sfr) =
    (uint32_t)k_ra_mipi_phy_sfr_pwrsf | (uint32_t)k_ra_mipi_phy_sfr_pllsf;
}

static ra_mipi_phy_timing_t make_timing(void)
{
  /* Values cribbed from HUM Table 64.2 PCLKA 125 MHz / 250 Mbps row, p 3832,
   * but the unit test only checks that they are written verbatim. */
  const ra_mipi_phy_timing_t t = {
    .tinit    = 0x000124F9U,
    .tclkprep = 0x08U,
    .tclksett = 0x00U,
    .tclkmiss = 0x00U,
    .thsprep  = 0x0AU,
    .thssett  = 0x00U,
    .tclkzero = 0x21U,
    .tclkpre  = 0x04U,
    .tclkpost = 0x3AU,
    .tclktrl  = 0x08U,
    .thszero  = 0x0FU,
    .thstrl   = 0x09U,
    .thsexit  = 0x0EU,
    .tlpx     = 0x08U,
  };
  return t;
}

static ra_mipi_phy_pll_t make_pll(void)
{
  const ra_mipi_phy_pll_t pll = {
    .idiv     = k_ra_mipi_phy_idiv_2,
    .pmul     = k_ra_mipi_phy_pmul_1,
    .nfmul    = k_ra_mipi_phy_nfmul_0_50,
    .nmul_int = (uint16_t)k_test_mipi_phy_nmul_int,
  };
  return pll;
}

static ra_mipi_phy_config_t make_dsi_cfg(const ra_mipi_phy_timing_t* tim)
{
  const ra_mipi_phy_config_t cfg = {
    .mode           = k_ra_mipi_phy_mode_dsi_master,
    .pclka_mhz      = (uint8_t)k_test_mipi_phy_pclka_mhz,
    .line_rate_mbps = (uint16_t)k_test_mipi_phy_rate_mbps,
    .lane_count     = k_ra_mipi_phy_lane_count_2,
    .clk_mode       = k_ra_mipi_phy_clk_noncontinuous,
    .eotp           = k_ra_mipi_phy_eotp_disabled,
    .pll            = make_pll(),
    .escdiv         = (uint8_t)k_test_mipi_phy_escdiv,
    .p_timing       = tim,
  };
  return cfg;
}

static ra_mipi_phy_config_t make_csi_cfg(const ra_mipi_phy_timing_t* tim)
{
  const ra_mipi_phy_config_t cfg = {
    .mode           = k_ra_mipi_phy_mode_csi_slave,
    .pclka_mhz      = (uint8_t)k_test_mipi_phy_pclka_mhz,
    .line_rate_mbps = (uint16_t)k_test_mipi_phy_rate_mbps,
    .lane_count     = k_ra_mipi_phy_lane_count_2,
    .clk_mode       = k_ra_mipi_phy_clk_noncontinuous,
    .eotp           = k_ra_mipi_phy_eotp_disabled,
    .pll            = make_pll(),
    .escdiv         = (uint8_t)k_test_mipi_phy_escdiv,
    .p_timing       = tim,
  };
  return cfg;
}

/* ---- Tests --------------------------------------------------------------- */

static void test_init_happy_dsi_master(void)
{
  TEST_BEGIN("mipi_phy init dsi master happy path");
  prep_fixture();

  const ra_mipi_phy_timing_t tim = make_timing();
  const ra_mipi_phy_config_t cfg = make_dsi_cfg(&tim);

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_mipi_phy_init(&cfg));

  /* DPHYMDC should hold MASTEREN. */
  TEST_ASSERT_EQ((int32_t)k_ra_mipi_phy_mdc_masteren,
                 (int32_t)*ra_mipi_phy_reg32(k_ra_mipi_phy_off_mdc));

  /* DPHYREFCR should hold the PCLKA frequency in MHz. */
  TEST_ASSERT_EQ((int32_t)k_test_mipi_phy_pclka_mhz,
                 (int32_t)*ra_mipi_phy_reg32(k_ra_mipi_phy_off_refcr));

  /* DPHYPWRCR.PWRSEN should be set. */
  TEST_ASSERT_EQ((int32_t)k_ra_mipi_phy_pwrcr_pwrsen,
                 (int32_t)*ra_mipi_phy_reg32(k_ra_mipi_phy_off_pwrcr));

  /* DPHYPLOCR.PLLSTP cleared (PLL allowed to run). */
  TEST_ASSERT_EQ((int32_t)0, (int32_t)*ra_mipi_phy_reg32(k_ra_mipi_phy_off_plocr));

  /* DPHYESCCR holds the configured ESCDIV. */
  TEST_ASSERT_EQ((int32_t)k_test_mipi_phy_escdiv,
                 (int32_t)*ra_mipi_phy_reg32(k_ra_mipi_phy_off_esccr));

  /* DPHYOCR.DPHYEN should be set. */
  TEST_ASSERT_EQ((int32_t)k_ra_mipi_phy_ocr_dphyen,
                 (int32_t)*ra_mipi_phy_reg32(k_ra_mipi_phy_off_ocr));

  /* DPHYTIM1.TINIT verbatim. */
  TEST_ASSERT_EQ((int32_t)tim.tinit, (int32_t)*ra_mipi_phy_reg32(k_ra_mipi_phy_off_tim1));

  /* DPHYTIM6.TLPX verbatim. */
  TEST_ASSERT_EQ((int32_t)tim.tlpx, (int32_t)*ra_mipi_phy_reg32(k_ra_mipi_phy_off_tim6));

  TEST_END("mipi_phy init dsi master happy path");
}

static void test_init_csi_slave_skips_pll(void)
{
  TEST_BEGIN("mipi_phy init csi slave skips pll");
  prep_fixture();

  /* For CSI mode the PLL stabilisation poll must not run, so leave
   * DPHYSFR with only PWRSF set to prove the PLL path is skipped. */
  *ra_mipi_phy_reg32(k_ra_mipi_phy_off_sfr) = (uint32_t)k_ra_mipi_phy_sfr_pwrsf;

  const ra_mipi_phy_timing_t tim = make_timing();
  const ra_mipi_phy_config_t cfg = make_csi_cfg(&tim);

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_mipi_phy_init(&cfg));

  /* MASTEREN should be cleared in slave mode. */
  TEST_ASSERT_EQ((int32_t)0, (int32_t)*ra_mipi_phy_reg32(k_ra_mipi_phy_off_mdc));

  /* PLL/escape divider should NOT have been touched (still zero). */
  TEST_ASSERT_EQ((int32_t)0, (int32_t)*ra_mipi_phy_reg32(k_ra_mipi_phy_off_plfcr));
  TEST_ASSERT_EQ((int32_t)0, (int32_t)*ra_mipi_phy_reg32(k_ra_mipi_phy_off_esccr));
  TEST_ASSERT_EQ((int32_t)0, (int32_t)*ra_mipi_phy_reg32(k_ra_mipi_phy_off_plocr));

  /* Timing + DPHYEN should still be programmed. */
  TEST_ASSERT_EQ((int32_t)k_ra_mipi_phy_ocr_dphyen,
                 (int32_t)*ra_mipi_phy_reg32(k_ra_mipi_phy_off_ocr));

  TEST_END("mipi_phy init csi slave skips pll");
}

static void test_init_null_cfg_rejected(void)
{
  TEST_BEGIN("mipi_phy init rejects null cfg");
  prep_fixture();

  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_mipi_phy_init(nullptr));

  TEST_END("mipi_phy init rejects null cfg");
}

static void test_init_null_timing_rejected(void)
{
  TEST_BEGIN("mipi_phy init rejects null timing");
  prep_fixture();

  ra_mipi_phy_config_t cfg = make_dsi_cfg(nullptr);

  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_mipi_phy_init(&cfg));

  TEST_END("mipi_phy init rejects null timing");
}

static void test_init_pclka_out_of_range(void)
{
  TEST_BEGIN("mipi_phy init rejects pclka out of range");
  prep_fixture();

  const ra_mipi_phy_timing_t tim = make_timing();
  ra_mipi_phy_config_t       cfg = make_dsi_cfg(&tim);

  cfg.pclka_mhz = (uint8_t)k_test_mipi_phy_pclka_lo;
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_mipi_phy_init(&cfg));

  cfg.pclka_mhz = (uint8_t)k_test_mipi_phy_pclka_hi;
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_mipi_phy_init(&cfg));

  TEST_END("mipi_phy init rejects pclka out of range");
}

static void test_init_escdiv_out_of_range(void)
{
  TEST_BEGIN("mipi_phy init rejects bad escdiv");
  prep_fixture();

  const ra_mipi_phy_timing_t tim = make_timing();
  ra_mipi_phy_config_t       cfg = make_dsi_cfg(&tim);
  cfg.escdiv                     = (uint8_t)k_test_mipi_phy_escdiv_bad;

  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_mipi_phy_init(&cfg));

  TEST_END("mipi_phy init rejects bad escdiv");
}

static void test_init_nmul_out_of_range(void)
{
  TEST_BEGIN("mipi_phy init rejects bad nmul");
  prep_fixture();

  const ra_mipi_phy_timing_t tim = make_timing();
  ra_mipi_phy_config_t       cfg = make_dsi_cfg(&tim);

  cfg.pll.nmul_int = (uint16_t)k_test_mipi_phy_nmul_bad_lo;
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_mipi_phy_init(&cfg));

  cfg.pll.nmul_int = (uint16_t)k_test_mipi_phy_nmul_bad_hi;
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_mipi_phy_init(&cfg));

  TEST_END("mipi_phy init rejects bad nmul");
}

static void test_init_lane_count_3_4_rejected(void)
{
  TEST_BEGIN("mipi_phy init rejects 3 / 4 lanes");
  prep_fixture();

  const ra_mipi_phy_timing_t tim = make_timing();
  ra_mipi_phy_config_t       cfg = make_dsi_cfg(&tim);

  cfg.lane_count = k_ra_mipi_phy_lane_count_3;
  TEST_ASSERT_EQ((int32_t)k_ra_err_not_supported, (int32_t)ra_mipi_phy_init(&cfg));

  cfg.lane_count = k_ra_mipi_phy_lane_count_4;
  TEST_ASSERT_EQ((int32_t)k_ra_err_not_supported, (int32_t)ra_mipi_phy_init(&cfg));

  TEST_END("mipi_phy init rejects 3 / 4 lanes");
}

static void test_init_lane_count_1_accepted(void)
{
  TEST_BEGIN("mipi_phy init accepts 1 lane");
  prep_fixture();

  const ra_mipi_phy_timing_t tim = make_timing();
  ra_mipi_phy_config_t       cfg = make_dsi_cfg(&tim);
  cfg.lane_count                 = k_ra_mipi_phy_lane_count_1;

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_mipi_phy_init(&cfg));
  TEST_ASSERT_EQ((int32_t)k_ra_mipi_phy_lane_count_1, (int32_t)ra_mipi_phy_get_lane_count());

  TEST_END("mipi_phy init accepts 1 lane");
}

static void test_init_ldo_timeout(void)
{
  TEST_BEGIN("mipi_phy init reports LDO timeout");
  ra_sim_mmap_reset(); /* zeros DPHYSFR -- LDO never asserts. */

  const ra_mipi_phy_timing_t tim = make_timing();
  const ra_mipi_phy_config_t cfg = make_dsi_cfg(&tim);

  TEST_ASSERT_EQ((int32_t)k_ra_err_hw_timeout, (int32_t)ra_mipi_phy_init(&cfg));

  TEST_END("mipi_phy init reports LDO timeout");
}

static void test_init_pll_timeout(void)
{
  TEST_BEGIN("mipi_phy init reports PLL timeout");
  ra_sim_mmap_reset();
  /* LDO ready, PLL never locks. */
  *ra_mipi_phy_reg32(k_ra_mipi_phy_off_sfr) = (uint32_t)k_ra_mipi_phy_sfr_pwrsf;

  const ra_mipi_phy_timing_t tim = make_timing();
  const ra_mipi_phy_config_t cfg = make_dsi_cfg(&tim);

  TEST_ASSERT_EQ((int32_t)k_ra_err_hw_timeout, (int32_t)ra_mipi_phy_init(&cfg));

  TEST_END("mipi_phy init reports PLL timeout");
}

static void test_deinit_clears_enables(void)
{
  TEST_BEGIN("mipi_phy deinit clears enables");
  prep_fixture();

  const ra_mipi_phy_timing_t tim = make_timing();
  const ra_mipi_phy_config_t cfg = make_dsi_cfg(&tim);
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_mipi_phy_init(&cfg));

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_mipi_phy_deinit());

  TEST_ASSERT_EQ((int32_t)0, (int32_t)*ra_mipi_phy_reg32(k_ra_mipi_phy_off_ocr));
  TEST_ASSERT_EQ((int32_t)k_ra_mipi_phy_plocr_pllstp,
                 (int32_t)*ra_mipi_phy_reg32(k_ra_mipi_phy_off_plocr));
  TEST_ASSERT_EQ((int32_t)0, (int32_t)*ra_mipi_phy_reg32(k_ra_mipi_phy_off_pwrcr));

  TEST_END("mipi_phy deinit clears enables");
}

static void test_reset_clears_all_regs(void)
{
  TEST_BEGIN("mipi_phy reset clears every register");
  prep_fixture();

  const ra_mipi_phy_timing_t tim = make_timing();
  const ra_mipi_phy_config_t cfg = make_dsi_cfg(&tim);
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_mipi_phy_init(&cfg));

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_mipi_phy_reset());

  TEST_ASSERT_EQ((int32_t)0, (int32_t)*ra_mipi_phy_reg32(k_ra_mipi_phy_off_ocr));
  TEST_ASSERT_EQ((int32_t)k_ra_mipi_phy_plocr_pllstp,
                 (int32_t)*ra_mipi_phy_reg32(k_ra_mipi_phy_off_plocr));
  TEST_ASSERT_EQ((int32_t)0, (int32_t)*ra_mipi_phy_reg32(k_ra_mipi_phy_off_pwrcr));
  TEST_ASSERT_EQ((int32_t)0, (int32_t)*ra_mipi_phy_reg32(k_ra_mipi_phy_off_plfcr));
  TEST_ASSERT_EQ((int32_t)0, (int32_t)*ra_mipi_phy_reg32(k_ra_mipi_phy_off_refcr));
  TEST_ASSERT_EQ((int32_t)0, (int32_t)*ra_mipi_phy_reg32(k_ra_mipi_phy_off_mdc));
  TEST_ASSERT_EQ((int32_t)0, (int32_t)*ra_mipi_phy_reg32(k_ra_mipi_phy_off_tim1));
  TEST_ASSERT_EQ((int32_t)0, (int32_t)*ra_mipi_phy_reg32(k_ra_mipi_phy_off_tim6));

  TEST_END("mipi_phy reset clears every register");
}

static void test_recover_from_error(void)
{
  TEST_BEGIN("mipi_phy recover_from_error re-runs init");
  prep_fixture();

  const ra_mipi_phy_timing_t tim = make_timing();
  const ra_mipi_phy_config_t cfg = make_dsi_cfg(&tim);

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_mipi_phy_init(&cfg));
  /* Re-seed flags before recover() (reset() will wipe them). */
  *ra_mipi_phy_reg32(k_ra_mipi_phy_off_sfr) =
    (uint32_t)k_ra_mipi_phy_sfr_pwrsf | (uint32_t)k_ra_mipi_phy_sfr_pllsf;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_mipi_phy_recover_from_error(&cfg));
  TEST_ASSERT_EQ((int32_t)k_ra_mipi_phy_ocr_dphyen,
                 (int32_t)*ra_mipi_phy_reg32(k_ra_mipi_phy_off_ocr));

  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_mipi_phy_recover_from_error(nullptr));

  TEST_END("mipi_phy recover_from_error re-runs init");
}

static void test_status_get_and_clear(void)
{
  TEST_BEGIN("mipi_phy status get + clear");
  prep_fixture();

  uint32_t mask = 0U;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_mipi_phy_get_status(&mask));
  TEST_ASSERT((mask & (uint32_t)k_ra_mipi_phy_sfr_pwrsf) != 0U);
  TEST_ASSERT((mask & (uint32_t)k_ra_mipi_phy_sfr_pllsf) != 0U);

  /* clear_status is a no-op (DPHYSFR is read-only on this part). */
  const uint32_t before = *ra_mipi_phy_reg32(k_ra_mipi_phy_off_sfr);
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_mipi_phy_clear_status((uint32_t)k_ra_mipi_phy_sfr_pwrsf));
  TEST_ASSERT_EQ((int32_t)before, (int32_t)*ra_mipi_phy_reg32(k_ra_mipi_phy_off_sfr));

  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_mipi_phy_get_status(nullptr));

  TEST_END("mipi_phy status get + clear");
}

static void test_is_helpers(void)
{
  TEST_BEGIN("mipi_phy is_pll_locked / is_ldo_stable / wait_ready");
  prep_fixture();

  TEST_ASSERT(ra_mipi_phy_is_ldo_stable());
  TEST_ASSERT(ra_mipi_phy_is_pll_locked());
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_mipi_phy_wait_ready());

  /* Clear flags -> is_* should report false and wait_ready should time out. */
  *ra_mipi_phy_reg32(k_ra_mipi_phy_off_sfr) = 0U;
  TEST_ASSERT(!ra_mipi_phy_is_ldo_stable());
  TEST_ASSERT(!ra_mipi_phy_is_pll_locked());
  TEST_ASSERT_EQ((int32_t)k_ra_err_hw_timeout, (int32_t)ra_mipi_phy_wait_ready());

  TEST_END("mipi_phy is_pll_locked / is_ldo_stable / wait_ready");
}

static void test_attach_and_dispatch_status_chg(void)
{
  TEST_BEGIN("mipi_phy attach + dispatch (status_chg)");
  prep_fixture();

  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_mipi_phy_attach_handler(stub_phy_cb, (void*)(uintptr_t)0xCAFEU));

  /* Run dispatch once to load s_last_sfr with the seeded value. */
  ra_mipi_phy_dispatch();
  TEST_ASSERT_EQ((int32_t)1, (int32_t)s_phy_cb_count);

  /* Second dispatch with no edges => status_chg event. */
  ra_mipi_phy_dispatch();
  TEST_ASSERT_EQ((int32_t)2, (int32_t)s_phy_cb_count);
  TEST_ASSERT_EQ((int32_t)k_ra_mipi_phy_event_status_chg, (int32_t)s_phy_cb_last_event);
  TEST_ASSERT_EQ((int32_t)((uint32_t)k_ra_mipi_phy_sfr_pwrsf | (uint32_t)k_ra_mipi_phy_sfr_pllsf),
                 (int32_t)s_phy_cb_last_sfr);
  TEST_ASSERT(s_phy_cb_last_ctx == (void*)(uintptr_t)0xCAFEU);

  /* Detach -- next dispatch must not increment the count. */
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_mipi_phy_attach_handler(nullptr, nullptr));
  ra_mipi_phy_dispatch();
  TEST_ASSERT_EQ((int32_t)2, (int32_t)s_phy_cb_count);

  TEST_END("mipi_phy attach + dispatch (status_chg)");
}

static void test_dispatch_decodes_edges(void)
{
  TEST_BEGIN("mipi_phy dispatch decodes ldo / pll edges");
  prep_fixture();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_mipi_phy_attach_handler(stub_phy_cb, nullptr));

  /* Start with everything cleared and dispatch -- s_last_sfr seeds. */
  *ra_mipi_phy_reg32(k_ra_mipi_phy_off_sfr) = 0U;
  ra_mipi_phy_dispatch();

  /* PWRSF rising. */
  *ra_mipi_phy_reg32(k_ra_mipi_phy_off_sfr) = (uint32_t)k_ra_mipi_phy_sfr_pwrsf;
  ra_mipi_phy_dispatch();
  TEST_ASSERT_EQ((int32_t)k_ra_mipi_phy_event_ldo_ready, (int32_t)s_phy_cb_last_event);

  /* PLLSF rising. */
  *ra_mipi_phy_reg32(k_ra_mipi_phy_off_sfr) =
    (uint32_t)k_ra_mipi_phy_sfr_pwrsf | (uint32_t)k_ra_mipi_phy_sfr_pllsf;
  ra_mipi_phy_dispatch();
  TEST_ASSERT_EQ((int32_t)k_ra_mipi_phy_event_pll_locked, (int32_t)s_phy_cb_last_event);

  /* PLLSF falling. */
  *ra_mipi_phy_reg32(k_ra_mipi_phy_off_sfr) = (uint32_t)k_ra_mipi_phy_sfr_pwrsf;
  ra_mipi_phy_dispatch();
  TEST_ASSERT_EQ((int32_t)k_ra_mipi_phy_event_pll_lost, (int32_t)s_phy_cb_last_event);

  /* PWRSF falling. */
  *ra_mipi_phy_reg32(k_ra_mipi_phy_off_sfr) = 0U;
  ra_mipi_phy_dispatch();
  TEST_ASSERT_EQ((int32_t)k_ra_mipi_phy_event_ldo_lost, (int32_t)s_phy_cb_last_event);

  TEST_END("mipi_phy dispatch decodes ldo / pll edges");
}

static void test_power_transition(void)
{
  TEST_BEGIN("mipi_phy power transition");
  prep_fixture();

  const ra_mipi_phy_timing_t tim = make_timing();
  const ra_mipi_phy_config_t cfg = make_dsi_cfg(&tim);

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_mipi_phy_init(&cfg));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_mipi_phy_enter_stop());
  TEST_ASSERT_EQ((int32_t)0, (int32_t)*ra_mipi_phy_reg32(k_ra_mipi_phy_off_pwrcr));

  /* Restore the status flags before exit_stop reruns init(). */
  *ra_mipi_phy_reg32(k_ra_mipi_phy_off_sfr) =
    (uint32_t)k_ra_mipi_phy_sfr_pwrsf | (uint32_t)k_ra_mipi_phy_sfr_pllsf;

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_mipi_phy_exit_stop(&cfg));
  TEST_ASSERT_EQ((int32_t)k_ra_mipi_phy_ocr_dphyen,
                 (int32_t)*ra_mipi_phy_reg32(k_ra_mipi_phy_off_ocr));

  TEST_END("mipi_phy power transition");
}

static void test_set_lane_speed_happy(void)
{
  TEST_BEGIN("mipi_phy set_lane_speed happy");
  prep_fixture();

  const ra_mipi_phy_timing_t tim = make_timing();
  const ra_mipi_phy_config_t cfg = make_dsi_cfg(&tim);
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_mipi_phy_init(&cfg));

  ra_mipi_phy_pll_t pll = make_pll();
  pll.nmul_int          = 200U;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_mipi_phy_set_lane_speed(&pll));

  /* DPHYPLFCR should hold the new NMUL in [24:16]. */
  const uint32_t plfcr = *ra_mipi_phy_reg32(k_ra_mipi_phy_off_plfcr);
  const uint32_t nmul_rb =
    (plfcr >> (uint32_t)k_ra_mipi_phy_plfcr_shift_nmul) & (uint32_t)k_ra_mipi_phy_plfcr_mask_nmul;
  TEST_ASSERT_EQ((int32_t)200, (int32_t)nmul_rb);

  /* PLLSTP must be cleared after relock. */
  TEST_ASSERT_EQ((int32_t)0, (int32_t)*ra_mipi_phy_reg32(k_ra_mipi_phy_off_plocr));

  TEST_END("mipi_phy set_lane_speed happy");
}

static void test_set_lane_speed_rejects(void)
{
  TEST_BEGIN("mipi_phy set_lane_speed rejects bad input");
  prep_fixture();

  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_mipi_phy_set_lane_speed(nullptr));

  ra_mipi_phy_pll_t pll = make_pll();
  pll.nmul_int          = (uint16_t)k_test_mipi_phy_nmul_bad_hi;
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_mipi_phy_set_lane_speed(&pll));

  TEST_END("mipi_phy set_lane_speed rejects bad input");
}

static void test_switch_mode(void)
{
  TEST_BEGIN("mipi_phy switch_mode toggles MASTEREN + clears DPHYEN");
  prep_fixture();

  const ra_mipi_phy_timing_t tim = make_timing();
  const ra_mipi_phy_config_t cfg = make_dsi_cfg(&tim);
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_mipi_phy_init(&cfg));

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_mipi_phy_switch_mode(k_ra_mipi_phy_mode_csi_slave));
  TEST_ASSERT_EQ((int32_t)0, (int32_t)*ra_mipi_phy_reg32(k_ra_mipi_phy_off_mdc));
  TEST_ASSERT_EQ((int32_t)0, (int32_t)*ra_mipi_phy_reg32(k_ra_mipi_phy_off_ocr));

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_mipi_phy_switch_mode(k_ra_mipi_phy_mode_dsi_master));
  TEST_ASSERT_EQ((int32_t)k_ra_mipi_phy_mdc_masteren,
                 (int32_t)*ra_mipi_phy_reg32(k_ra_mipi_phy_off_mdc));

  /* Bad arg. */
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_mipi_phy_switch_mode((ra_mipi_phy_mode_t)9U));

  TEST_END("mipi_phy switch_mode toggles MASTEREN + clears DPHYEN");
}

static void test_lane_count_get_set(void)
{
  TEST_BEGIN("mipi_phy lane count get/set");
  prep_fixture();

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_mipi_phy_set_lane_count(k_ra_mipi_phy_lane_count_1));
  TEST_ASSERT_EQ((int32_t)k_ra_mipi_phy_lane_count_1, (int32_t)ra_mipi_phy_get_lane_count());
  /* Lane 1 should now be auto-disabled. */
  TEST_ASSERT(!ra_mipi_phy_is_lane_enabled(k_ra_mipi_phy_lane_d1));

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_mipi_phy_set_lane_count(k_ra_mipi_phy_lane_count_2));
  TEST_ASSERT(ra_mipi_phy_is_lane_enabled(k_ra_mipi_phy_lane_d1));

  TEST_ASSERT_EQ((int32_t)k_ra_err_not_supported,
                 (int32_t)ra_mipi_phy_set_lane_count(k_ra_mipi_phy_lane_count_3));
  TEST_ASSERT_EQ((int32_t)k_ra_err_not_supported,
                 (int32_t)ra_mipi_phy_set_lane_count(k_ra_mipi_phy_lane_count_4));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_mipi_phy_set_lane_count((ra_mipi_phy_lane_count_t)0U));

  TEST_END("mipi_phy lane count get/set");
}

static void test_lane_enable_disable(void)
{
  TEST_BEGIN("mipi_phy per-lane enable / disable");
  prep_fixture();

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_mipi_phy_set_lane_count(k_ra_mipi_phy_lane_count_2));

  TEST_ASSERT(ra_mipi_phy_is_lane_enabled(k_ra_mipi_phy_lane_clk));
  TEST_ASSERT(ra_mipi_phy_is_lane_enabled(k_ra_mipi_phy_lane_d0));
  TEST_ASSERT(ra_mipi_phy_is_lane_enabled(k_ra_mipi_phy_lane_d1));

  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_mipi_phy_set_lane_enable(k_ra_mipi_phy_lane_d1, false));
  TEST_ASSERT(!ra_mipi_phy_is_lane_enabled(k_ra_mipi_phy_lane_d1));

  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_mipi_phy_set_lane_enable(k_ra_mipi_phy_lane_d1, true));
  TEST_ASSERT(ra_mipi_phy_is_lane_enabled(k_ra_mipi_phy_lane_d1));

  /* Cannot enable lane 1 when count == 1. */
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_mipi_phy_set_lane_count(k_ra_mipi_phy_lane_count_1));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_mipi_phy_set_lane_enable(k_ra_mipi_phy_lane_d1, true));

  /* Lanes 2 / 3 do not exist. */
  TEST_ASSERT_EQ((int32_t)k_ra_err_not_supported,
                 (int32_t)ra_mipi_phy_set_lane_enable(k_ra_mipi_phy_lane_d2, true));
  TEST_ASSERT_EQ((int32_t)k_ra_err_not_supported,
                 (int32_t)ra_mipi_phy_set_lane_enable(k_ra_mipi_phy_lane_d3, false));

  TEST_END("mipi_phy per-lane enable / disable");
}

static void test_clock_mode(void)
{
  TEST_BEGIN("mipi_phy clock_mode setter / getter");
  prep_fixture();

  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_mipi_phy_set_clock_mode(k_ra_mipi_phy_clk_continuous));
  TEST_ASSERT_EQ((int32_t)k_ra_mipi_phy_clk_continuous, (int32_t)ra_mipi_phy_get_clock_mode());

  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_mipi_phy_set_clock_mode(k_ra_mipi_phy_clk_noncontinuous));
  TEST_ASSERT_EQ((int32_t)k_ra_mipi_phy_clk_noncontinuous, (int32_t)ra_mipi_phy_get_clock_mode());

  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_mipi_phy_set_clock_mode((ra_mipi_phy_clk_mode_t)9U));

  TEST_END("mipi_phy clock_mode setter / getter");
}

static void test_eotp(void)
{
  TEST_BEGIN("mipi_phy eotp setter / getter");
  prep_fixture();

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_mipi_phy_set_eotp(k_ra_mipi_phy_eotp_enabled));
  TEST_ASSERT_EQ((int32_t)k_ra_mipi_phy_eotp_enabled, (int32_t)ra_mipi_phy_get_eotp());

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_mipi_phy_set_eotp(k_ra_mipi_phy_eotp_disabled));
  TEST_ASSERT_EQ((int32_t)k_ra_mipi_phy_eotp_disabled, (int32_t)ra_mipi_phy_get_eotp());

  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_mipi_phy_set_eotp((ra_mipi_phy_eotp_t)9U));

  TEST_END("mipi_phy eotp setter / getter");
}

static void test_set_pclka_freq(void)
{
  TEST_BEGIN("mipi_phy set_pclka_freq");
  prep_fixture();

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_mipi_phy_set_pclka_freq(80U));
  TEST_ASSERT_EQ((int32_t)80, (int32_t)*ra_mipi_phy_reg32(k_ra_mipi_phy_off_refcr));

  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_mipi_phy_set_pclka_freq(39U));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_mipi_phy_set_pclka_freq(126U));

  TEST_END("mipi_phy set_pclka_freq");
}

static void test_set_escape_divisor(void)
{
  TEST_BEGIN("mipi_phy set_escape_divisor");
  prep_fixture();

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_mipi_phy_set_escape_divisor(7U));
  TEST_ASSERT_EQ((int32_t)7, (int32_t)*ra_mipi_phy_reg32(k_ra_mipi_phy_off_esccr));
  TEST_ASSERT_EQ((int32_t)0, (int32_t)*ra_mipi_phy_reg32(k_ra_mipi_phy_off_plocr));

  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_mipi_phy_set_escape_divisor(32U));

  TEST_END("mipi_phy set_escape_divisor");
}

static void test_pll_start_stop(void)
{
  TEST_BEGIN("mipi_phy pll start / stop / ldo enable / disable");
  prep_fixture();

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_mipi_phy_ldo_enable());
  TEST_ASSERT_EQ((int32_t)k_ra_mipi_phy_pwrcr_pwrsen,
                 (int32_t)*ra_mipi_phy_reg32(k_ra_mipi_phy_off_pwrcr));

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_mipi_phy_pll_start());
  TEST_ASSERT_EQ((int32_t)0, (int32_t)*ra_mipi_phy_reg32(k_ra_mipi_phy_off_plocr));

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_mipi_phy_pll_stop());
  TEST_ASSERT_EQ((int32_t)k_ra_mipi_phy_plocr_pllstp,
                 (int32_t)*ra_mipi_phy_reg32(k_ra_mipi_phy_off_plocr));

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_mipi_phy_ldo_disable());
  TEST_ASSERT_EQ((int32_t)0, (int32_t)*ra_mipi_phy_reg32(k_ra_mipi_phy_off_pwrcr));

  /* Timeout paths. */
  *ra_mipi_phy_reg32(k_ra_mipi_phy_off_sfr) = 0U;
  TEST_ASSERT_EQ((int32_t)k_ra_err_hw_timeout, (int32_t)ra_mipi_phy_ldo_enable());
  TEST_ASSERT_EQ((int32_t)k_ra_err_hw_timeout, (int32_t)ra_mipi_phy_pll_start());

  TEST_END("mipi_phy pll start / stop / ldo enable / disable");
}

static void test_select_timing_dsi_125_250(void)
{
  TEST_BEGIN("mipi_phy select_timing DSI 125 MHz 250 Mbps");
  prep_fixture();

  ra_mipi_phy_timing_t out;
  TEST_ASSERT_EQ(
    (int32_t)k_ra_ok,
    (int32_t)ra_mipi_phy_select_timing(k_ra_mipi_phy_mode_dsi_master, 125U, 250U, &out));
  /* HUM Table 64.2 PCLKA 125 MHz / 150-250 column row, p 3832. */
  TEST_ASSERT_EQ((int32_t)0x000124F9U, (int32_t)out.tinit);
  TEST_ASSERT_EQ((int32_t)0x21U, (int32_t)out.tclkzero);
  TEST_ASSERT_EQ((int32_t)0x08U, (int32_t)out.tlpx);

  TEST_END("mipi_phy select_timing DSI 125 MHz 250 Mbps");
}

static void test_select_timing_csi_100_300(void)
{
  TEST_BEGIN("mipi_phy select_timing CSI 100 MHz 300 Mbps");
  prep_fixture();

  ra_mipi_phy_timing_t out;
  TEST_ASSERT_EQ(
    (int32_t)k_ra_ok,
    (int32_t)ra_mipi_phy_select_timing(k_ra_mipi_phy_mode_csi_slave, 100U, 300U, &out));
  /* HUM Table 64.3 PCLKA 100 MHz / 200-300 column row, p 3835. */
  TEST_ASSERT_EQ((int32_t)0x0000EA61U, (int32_t)out.tinit);
  TEST_ASSERT_EQ((int32_t)0x0DU, (int32_t)out.tclkprep);

  TEST_END("mipi_phy select_timing CSI 100 MHz 300 Mbps");
}

static void test_select_timing_rejects(void)
{
  TEST_BEGIN("mipi_phy select_timing rejects bad input");
  prep_fixture();

  /* Out-of-range rate. */
  TEST_ASSERT_EQ(
    (int32_t)k_ra_err_invalid_arg,
    (int32_t)ra_mipi_phy_select_timing(k_ra_mipi_phy_mode_dsi_master, 125U, 79U, nullptr));
  TEST_ASSERT_EQ(
    (int32_t)k_ra_err_invalid_arg,
    (int32_t)ra_mipi_phy_select_timing(k_ra_mipi_phy_mode_dsi_master, 125U, 721U, nullptr));
  /* Bad mode. */
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_mipi_phy_select_timing((ra_mipi_phy_mode_t)9U, 125U, 250U, nullptr));
  /* PCLKA not in the table. */
  TEST_ASSERT_EQ(
    (int32_t)k_ra_err_not_supported,
    (int32_t)ra_mipi_phy_select_timing(k_ra_mipi_phy_mode_dsi_master, 99U, 250U, nullptr));

  TEST_END("mipi_phy select_timing rejects bad input");
}

static void test_validate_pll_band(void)
{
  TEST_BEGIN("mipi_phy validate_pll_band");
  prep_fixture();

  /* HUM 64.2.2 example p 3823: fMAIN=24.576 (round to 24), IDIV=1/2,
   * NMUL=100, NFMUL=0.5, PMUL=1 -> ~1212 MHz, lands in P=1 band. */
  ra_mipi_phy_pll_t pll = {
    .idiv     = k_ra_mipi_phy_idiv_2,
    .pmul     = k_ra_mipi_phy_pmul_1,
    .nfmul    = k_ra_mipi_phy_nfmul_0_50,
    .nmul_int = 100U,
  };
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_mipi_phy_validate_pll_band(&pll, 24U));

  /* Force out-of-band: PMUL=1/8 with high N -> exceeds 375 MHz ceiling. */
  pll.pmul     = k_ra_mipi_phy_pmul_8;
  pll.nmul_int = 300U;
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_mipi_phy_validate_pll_band(&pll, 24U));

  /* Bad MOSC. */
  pll.pmul     = k_ra_mipi_phy_pmul_1;
  pll.nmul_int = 100U;
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_mipi_phy_validate_pll_band(&pll, 7U));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_mipi_phy_validate_pll_band(&pll, 49U));

  /* NULL pll. */
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_mipi_phy_validate_pll_band(nullptr, 24U));

  TEST_END("mipi_phy validate_pll_band");
}

static void test_init_writes_all_timings(void)
{
  TEST_BEGIN("mipi_phy init writes every DPHYTIMx register");
  prep_fixture();

  const ra_mipi_phy_timing_t tim = make_timing();
  const ra_mipi_phy_config_t cfg = make_dsi_cfg(&tim);
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_mipi_phy_init(&cfg));

  /* TIM2: TCLKPREP in byte 0. */
  TEST_ASSERT_EQ((int32_t)tim.tclkprep,
                 (int32_t)(*ra_mipi_phy_reg32(k_ra_mipi_phy_off_tim2) & 0xFFU));
  /* TIM3: THSPREP in byte 0, THSSETT in byte 1. */
  const uint32_t tim3 = *ra_mipi_phy_reg32(k_ra_mipi_phy_off_tim3);
  TEST_ASSERT_EQ((int32_t)tim.thsprep, (int32_t)(tim3 & 0xFFU));
  TEST_ASSERT_EQ((int32_t)tim.thssett, (int32_t)((tim3 >> 8U) & 0xFFU));
  /* TIM4: TCLKZERO in byte 0, TCLKTRL in byte 3. */
  const uint32_t tim4 = *ra_mipi_phy_reg32(k_ra_mipi_phy_off_tim4);
  TEST_ASSERT_EQ((int32_t)tim.tclkzero, (int32_t)(tim4 & 0xFFU));
  TEST_ASSERT_EQ((int32_t)tim.tclktrl, (int32_t)((tim4 >> 24U) & 0xFFU));
  /* TIM5: THSZERO in byte 0, THSEXIT in byte 2. */
  const uint32_t tim5 = *ra_mipi_phy_reg32(k_ra_mipi_phy_off_tim5);
  TEST_ASSERT_EQ((int32_t)tim.thszero, (int32_t)(tim5 & 0xFFU));
  TEST_ASSERT_EQ((int32_t)tim.thsexit, (int32_t)((tim5 >> 16U) & 0xFFU));

  TEST_END("mipi_phy init writes every DPHYTIMx register");
}

int32_t main(void)
{
  test_init_happy_dsi_master();
  test_init_csi_slave_skips_pll();
  test_init_null_cfg_rejected();
  test_init_null_timing_rejected();
  test_init_pclka_out_of_range();
  test_init_escdiv_out_of_range();
  test_init_nmul_out_of_range();
  test_init_lane_count_3_4_rejected();
  test_init_lane_count_1_accepted();
  test_init_ldo_timeout();
  test_init_pll_timeout();
  test_deinit_clears_enables();
  test_reset_clears_all_regs();
  test_recover_from_error();
  test_status_get_and_clear();
  test_is_helpers();
  test_attach_and_dispatch_status_chg();
  test_dispatch_decodes_edges();
  test_power_transition();
  test_set_lane_speed_happy();
  test_set_lane_speed_rejects();
  test_switch_mode();
  test_lane_count_get_set();
  test_lane_enable_disable();
  test_clock_mode();
  test_eotp();
  test_set_pclka_freq();
  test_set_escape_divisor();
  test_pll_start_stop();
  test_select_timing_dsi_125_250();
  test_select_timing_csi_100_300();
  test_select_timing_rejects();
  test_validate_pll_band();
  test_init_writes_all_timings();
  (void)fprintf(stderr, "[OK  ] test_ra_mipi_phy.c\n");
  return 0;
}
