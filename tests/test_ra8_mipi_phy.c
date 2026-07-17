/**
 * @file test_ra8_mipi_phy.c
 * @brief Unit tests for ra8_mipi_phy.c (MIPI D-PHY driver)
 *
 * @details
 * The host test fixture pre-seeds DPHYSFR with PWRSF | PLLSF before
 * calling ``ra8_mipi_phy_init`` so the LDO and PLL stabilisation
 * polls complete on the first iteration. Real silicon needs ~us
 * for the LDO and ~tens of us for the PLL, both validated against
 * HUM Ch 64.3.1 p 3837.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_mipi_phy.h"
#include "ra8_mipi_phy_regs.h"
#include "ra8_sim_mmap.h"
#include "unity_minimal.h"

typedef enum : uint8_t {
  k_test_mipi_phy_pclka_mhz  = 100U, /**< PCLKA = 100 MHz (within 40..125).  */
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

static void stub_phy_cb(void* ctx, ra8_mipi_phy_event_t event, uint32_t sfr)
{
  ++s_phy_cb_count;
  s_phy_cb_last_sfr   = sfr;
  s_phy_cb_last_ctx   = ctx;
  s_phy_cb_last_event = (uint8_t)event;
}

/* ---- Fixture helpers ----------------------------------------------------- */

static void prep_fixture(void)
{
  ra8_sim_mmap_reset();
  s_phy_cb_count      = 0U;
  s_phy_cb_last_sfr   = 0U;
  s_phy_cb_last_ctx   = nullptr;
  s_phy_cb_last_event = 0xFFU;
  /* Pre-seed DPHYSFR so the LDO + PLL polls return immediately. */
  *ra8_mipi_phy_reg32(k_ra8_mipi_phy_off_sfr) =
    (uint32_t)k_ra8_mipi_phy_sfr_pwrsf | (uint32_t)k_ra8_mipi_phy_sfr_pllsf;
  /* Detach any callback left over from a previous test. */
  (void)ra8_mipi_phy_attach_handler(nullptr, nullptr);
  /* Reset module state via reset(). */
  (void)ra8_mipi_phy_reset();
  /* Re-seed after reset wiped DPHYSFR. */
  *ra8_mipi_phy_reg32(k_ra8_mipi_phy_off_sfr) =
    (uint32_t)k_ra8_mipi_phy_sfr_pwrsf | (uint32_t)k_ra8_mipi_phy_sfr_pllsf;
}

static ra8_mipi_phy_timing_t make_timing(void)
{
  /* Values cribbed from HUM Table 64.2 PCLKA 125 MHz / 250 Mbps row, p 3832,
   * but the unit test only checks that they are written verbatim. */
  const ra8_mipi_phy_timing_t t = {
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

static ra8_mipi_phy_pll_t make_pll(void)
{
  const ra8_mipi_phy_pll_t pll = {
    .idiv     = k_ra8_mipi_phy_idiv_2,
    .pmul     = k_ra8_mipi_phy_pmul_1,
    .nfmul    = k_ra8_mipi_phy_nfmul_0_50,
    .nmul_int = (uint16_t)k_test_mipi_phy_nmul_int,
  };
  return pll;
}

static ra8_mipi_phy_config_t make_dsi_cfg(const ra8_mipi_phy_timing_t* tim)
{
  const ra8_mipi_phy_config_t cfg = {
    .mode           = k_ra8_mipi_phy_mode_dsi_host,
    .pclka_mhz      = (uint8_t)k_test_mipi_phy_pclka_mhz,
    .line_rate_mbps = (uint16_t)k_test_mipi_phy_rate_mbps,
    .lane_count     = k_ra8_mipi_phy_lane_count_2,
    .clk_mode       = k_ra8_mipi_phy_clk_noncontinuous,
    .eotp           = k_ra8_mipi_phy_eotp_disabled,
    .pll            = make_pll(),
    .escdiv         = (uint8_t)k_test_mipi_phy_escdiv,
    .p_timing       = tim,
  };
  return cfg;
}

static ra8_mipi_phy_config_t make_csi_cfg(const ra8_mipi_phy_timing_t* tim)
{
  const ra8_mipi_phy_config_t cfg = {
    .mode           = k_ra8_mipi_phy_mode_csi_device,
    .pclka_mhz      = (uint8_t)k_test_mipi_phy_pclka_mhz,
    .line_rate_mbps = (uint16_t)k_test_mipi_phy_rate_mbps,
    .lane_count     = k_ra8_mipi_phy_lane_count_2,
    .clk_mode       = k_ra8_mipi_phy_clk_noncontinuous,
    .eotp           = k_ra8_mipi_phy_eotp_disabled,
    .pll            = make_pll(),
    .escdiv         = (uint8_t)k_test_mipi_phy_escdiv,
    .p_timing       = tim,
  };
  return cfg;
}

/* ---- Tests --------------------------------------------------------------- */
/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */

static void test_init_happy_dsi_host(void)
{
  TEST_BEGIN("mipi_phy init dsi host happy path");
  prep_fixture();

  const ra8_mipi_phy_timing_t tim = make_timing();
  const ra8_mipi_phy_config_t cfg = make_dsi_cfg(&tim);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_phy_init(&cfg));

  /* DPHYMDC should hold MASTEREN. */
  TEST_ASSERT_EQ(k_ra8_mipi_phy_mdc_hosten, *ra8_mipi_phy_reg32(k_ra8_mipi_phy_off_mdc));

  /* DPHYREFCR.RFREQ encodes (MHz - 1) per HUM Ch 64.2.1 p 3822
   * (matches FSP r_mipi_phy.c which writes ``(pclka_hz / 1MHz) - 1``). */
  TEST_ASSERT_EQ(((uint32_t)k_test_mipi_phy_pclka_mhz - (uint32_t)k_ra8_mipi_phy_refcr_rfreq_bias),
                 *ra8_mipi_phy_reg32(k_ra8_mipi_phy_off_refcr));

  /* DPHYPWRCR.PWRSEN should be set. */
  TEST_ASSERT_EQ(k_ra8_mipi_phy_pwrcr_pwrsen, *ra8_mipi_phy_reg32(k_ra8_mipi_phy_off_pwrcr));

  /* DPHYPLOCR.PLLSTP cleared (PLL allowed to run). */
  TEST_ASSERT_EQ(0, *ra8_mipi_phy_reg32(k_ra8_mipi_phy_off_plocr));

  /* DPHYESCCR holds the configured ESCDIV. */
  TEST_ASSERT_EQ(k_test_mipi_phy_escdiv, *ra8_mipi_phy_reg32(k_ra8_mipi_phy_off_esccr));

  /* DPHYOCR.DPHYEN should be set. */
  TEST_ASSERT_EQ(k_ra8_mipi_phy_ocr_dphyen, *ra8_mipi_phy_reg32(k_ra8_mipi_phy_off_ocr));

  /* DPHYTIM1.TINIT verbatim. */
  TEST_ASSERT_EQ(tim.tinit, *ra8_mipi_phy_reg32(k_ra8_mipi_phy_off_tim1));

  /* DPHYTIM6.TLPX verbatim. */
  TEST_ASSERT_EQ(tim.tlpx, *ra8_mipi_phy_reg32(k_ra8_mipi_phy_off_tim6));

  TEST_END("mipi_phy init dsi host happy path");
}
/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */

static void test_init_csi_device_skips_pll(void)
{
  TEST_BEGIN("mipi_phy init csi device skips pll");
  prep_fixture();

  /* For CSI mode the PLL stabilisation poll must not run, so leave
   * DPHYSFR with only PWRSF set to prove the PLL path is skipped. */
  *ra8_mipi_phy_reg32(k_ra8_mipi_phy_off_sfr) = (uint32_t)k_ra8_mipi_phy_sfr_pwrsf;

  const ra8_mipi_phy_timing_t tim = make_timing();
  const ra8_mipi_phy_config_t cfg = make_csi_cfg(&tim);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_phy_init(&cfg));

  /* MASTEREN should be cleared in device mode. */
  TEST_ASSERT_EQ(0, *ra8_mipi_phy_reg32(k_ra8_mipi_phy_off_mdc));

  /* PLL/escape divider should NOT have been touched (still zero). */
  TEST_ASSERT_EQ(0, *ra8_mipi_phy_reg32(k_ra8_mipi_phy_off_plfcr));
  TEST_ASSERT_EQ(0, *ra8_mipi_phy_reg32(k_ra8_mipi_phy_off_esccr));
  TEST_ASSERT_EQ(0, *ra8_mipi_phy_reg32(k_ra8_mipi_phy_off_plocr));

  /* Timing + DPHYEN should still be programmed. */
  TEST_ASSERT_EQ(k_ra8_mipi_phy_ocr_dphyen, *ra8_mipi_phy_reg32(k_ra8_mipi_phy_off_ocr));

  TEST_END("mipi_phy init csi device skips pll");
}
/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */

static void test_init_null_cfg_rejected(void)
{
  TEST_BEGIN("mipi_phy init rejects null cfg");
  prep_fixture();

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_mipi_phy_init(nullptr));

  TEST_END("mipi_phy init rejects null cfg");
}
/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */

static void test_init_null_timing_rejected(void)
{
  TEST_BEGIN("mipi_phy init rejects null timing");
  prep_fixture();

  ra8_mipi_phy_config_t cfg = make_dsi_cfg(nullptr);

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_mipi_phy_init(&cfg));

  TEST_END("mipi_phy init rejects null timing");
}
/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */

static void test_init_pclka_out_of_range(void)
{
  TEST_BEGIN("mipi_phy init rejects pclka out of range");
  prep_fixture();

  const ra8_mipi_phy_timing_t tim = make_timing();
  ra8_mipi_phy_config_t       cfg = make_dsi_cfg(&tim);

  cfg.pclka_mhz = (uint8_t)k_test_mipi_phy_pclka_lo;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_mipi_phy_init(&cfg));

  cfg.pclka_mhz = (uint8_t)k_test_mipi_phy_pclka_hi;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_mipi_phy_init(&cfg));

  TEST_END("mipi_phy init rejects pclka out of range");
}
/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */

static void test_init_escdiv_out_of_range(void)
{
  TEST_BEGIN("mipi_phy init rejects bad escdiv");
  prep_fixture();

  const ra8_mipi_phy_timing_t tim = make_timing();
  ra8_mipi_phy_config_t       cfg = make_dsi_cfg(&tim);
  cfg.escdiv                      = (uint8_t)k_test_mipi_phy_escdiv_bad;

  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_mipi_phy_init(&cfg));

  TEST_END("mipi_phy init rejects bad escdiv");
}
/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */

static void test_init_nmul_out_of_range(void)
{
  TEST_BEGIN("mipi_phy init rejects bad nmul");
  prep_fixture();

  const ra8_mipi_phy_timing_t tim = make_timing();
  ra8_mipi_phy_config_t       cfg = make_dsi_cfg(&tim);

  cfg.pll.nmul_int = (uint16_t)k_test_mipi_phy_nmul_bad_lo;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_mipi_phy_init(&cfg));

  cfg.pll.nmul_int = (uint16_t)k_test_mipi_phy_nmul_bad_hi;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_mipi_phy_init(&cfg));

  TEST_END("mipi_phy init rejects bad nmul");
}
/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */

static void test_init_lane_count_3_4_rejected(void)
{
  TEST_BEGIN("mipi_phy init rejects 3 / 4 lanes");
  prep_fixture();

  const ra8_mipi_phy_timing_t tim = make_timing();
  ra8_mipi_phy_config_t       cfg = make_dsi_cfg(&tim);

  cfg.lane_count = k_ra8_mipi_phy_lane_count_3;
  TEST_ASSERT_EQ(k_ra8_err_not_supported, ra8_mipi_phy_init(&cfg));

  cfg.lane_count = k_ra8_mipi_phy_lane_count_4;
  TEST_ASSERT_EQ(k_ra8_err_not_supported, ra8_mipi_phy_init(&cfg));

  TEST_END("mipi_phy init rejects 3 / 4 lanes");
}
/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */

static void test_init_lane_count_1_accepted(void)
{
  TEST_BEGIN("mipi_phy init accepts 1 lane");
  prep_fixture();

  const ra8_mipi_phy_timing_t tim = make_timing();
  ra8_mipi_phy_config_t       cfg = make_dsi_cfg(&tim);
  cfg.lane_count                  = k_ra8_mipi_phy_lane_count_1;

  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_phy_init(&cfg));
  TEST_ASSERT_EQ(k_ra8_mipi_phy_lane_count_1, ra8_mipi_phy_get_lane_count());

  TEST_END("mipi_phy init accepts 1 lane");
}
/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */

static void test_init_ldo_timeout(void)
{
  TEST_BEGIN("mipi_phy init reports LDO timeout");
  ra8_sim_mmap_reset(); /* zeros DPHYSFR -- LDO never asserts. */

  const ra8_mipi_phy_timing_t tim = make_timing();
  const ra8_mipi_phy_config_t cfg = make_dsi_cfg(&tim);

  TEST_ASSERT_EQ(k_ra8_err_hw_timeout, ra8_mipi_phy_init(&cfg));

  TEST_END("mipi_phy init reports LDO timeout");
}
/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */

static void test_init_pll_timeout(void)
{
  TEST_BEGIN("mipi_phy init reports PLL timeout");
  ra8_sim_mmap_reset();
  /* LDO ready, PLL never locks. */
  *ra8_mipi_phy_reg32(k_ra8_mipi_phy_off_sfr) = (uint32_t)k_ra8_mipi_phy_sfr_pwrsf;

  const ra8_mipi_phy_timing_t tim = make_timing();
  const ra8_mipi_phy_config_t cfg = make_dsi_cfg(&tim);

  TEST_ASSERT_EQ(k_ra8_err_hw_timeout, ra8_mipi_phy_init(&cfg));

  TEST_END("mipi_phy init reports PLL timeout");
}
/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */

static void test_deinit_clears_enables(void)
{
  TEST_BEGIN("mipi_phy deinit clears enables");
  prep_fixture();

  const ra8_mipi_phy_timing_t tim = make_timing();
  const ra8_mipi_phy_config_t cfg = make_dsi_cfg(&tim);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_phy_init(&cfg));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_phy_deinit());

  TEST_ASSERT_EQ(0, *ra8_mipi_phy_reg32(k_ra8_mipi_phy_off_ocr));
  TEST_ASSERT_EQ(k_ra8_mipi_phy_plocr_pllstp, *ra8_mipi_phy_reg32(k_ra8_mipi_phy_off_plocr));
  TEST_ASSERT_EQ(0, *ra8_mipi_phy_reg32(k_ra8_mipi_phy_off_pwrcr));

  TEST_END("mipi_phy deinit clears enables");
}
/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */

static void test_reset_clears_all_regs(void)
{
  TEST_BEGIN("mipi_phy reset clears every register");
  prep_fixture();

  const ra8_mipi_phy_timing_t tim = make_timing();
  const ra8_mipi_phy_config_t cfg = make_dsi_cfg(&tim);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_phy_init(&cfg));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_phy_reset());

  TEST_ASSERT_EQ(0, *ra8_mipi_phy_reg32(k_ra8_mipi_phy_off_ocr));
  TEST_ASSERT_EQ(k_ra8_mipi_phy_plocr_pllstp, *ra8_mipi_phy_reg32(k_ra8_mipi_phy_off_plocr));
  TEST_ASSERT_EQ(0, *ra8_mipi_phy_reg32(k_ra8_mipi_phy_off_pwrcr));
  TEST_ASSERT_EQ(0, *ra8_mipi_phy_reg32(k_ra8_mipi_phy_off_plfcr));
  TEST_ASSERT_EQ(0, *ra8_mipi_phy_reg32(k_ra8_mipi_phy_off_refcr));
  TEST_ASSERT_EQ(0, *ra8_mipi_phy_reg32(k_ra8_mipi_phy_off_mdc));
  TEST_ASSERT_EQ(0, *ra8_mipi_phy_reg32(k_ra8_mipi_phy_off_tim1));
  TEST_ASSERT_EQ(0, *ra8_mipi_phy_reg32(k_ra8_mipi_phy_off_tim6));

  TEST_END("mipi_phy reset clears every register");
}
/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */

static void test_recover_from_error(void)
{
  TEST_BEGIN("mipi_phy recover_from_error re-runs init");
  prep_fixture();

  const ra8_mipi_phy_timing_t tim = make_timing();
  const ra8_mipi_phy_config_t cfg = make_dsi_cfg(&tim);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_phy_init(&cfg));
  /* Re-seed flags before recover() (reset() will wipe them). */
  *ra8_mipi_phy_reg32(k_ra8_mipi_phy_off_sfr) =
    (uint32_t)k_ra8_mipi_phy_sfr_pwrsf | (uint32_t)k_ra8_mipi_phy_sfr_pllsf;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_phy_recover_from_error(&cfg));
  TEST_ASSERT_EQ(k_ra8_mipi_phy_ocr_dphyen, *ra8_mipi_phy_reg32(k_ra8_mipi_phy_off_ocr));

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_mipi_phy_recover_from_error(nullptr));

  TEST_END("mipi_phy recover_from_error re-runs init");
}
/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */

static void test_status_get_and_clear(void)
{
  TEST_BEGIN("mipi_phy status get + clear");
  prep_fixture();

  uint32_t mask = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_phy_get_status(&mask));
  TEST_ASSERT((mask & (uint32_t)k_ra8_mipi_phy_sfr_pwrsf) != 0U);
  TEST_ASSERT((mask & (uint32_t)k_ra8_mipi_phy_sfr_pllsf) != 0U);

  /* clear_status is a no-op (DPHYSFR is read-only on this part). */
  const uint32_t before = *ra8_mipi_phy_reg32(k_ra8_mipi_phy_off_sfr);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_phy_clear_status((uint32_t)k_ra8_mipi_phy_sfr_pwrsf));
  TEST_ASSERT_EQ(before, *ra8_mipi_phy_reg32(k_ra8_mipi_phy_off_sfr));

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_mipi_phy_get_status(nullptr));

  TEST_END("mipi_phy status get + clear");
}
/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */

static void test_is_helpers(void)
{
  TEST_BEGIN("mipi_phy is_pll_locked / is_ldo_stable / wait_ready");
  prep_fixture();

  TEST_ASSERT(ra8_mipi_phy_is_ldo_stable());
  TEST_ASSERT(ra8_mipi_phy_is_pll_locked());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_phy_wait_ready());

  /* Clear flags -> is_* should report false and wait_ready should time out. */
  *ra8_mipi_phy_reg32(k_ra8_mipi_phy_off_sfr) = 0U;
  TEST_ASSERT(!ra8_mipi_phy_is_ldo_stable());
  TEST_ASSERT(!ra8_mipi_phy_is_pll_locked());
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout, ra8_mipi_phy_wait_ready());

  TEST_END("mipi_phy is_pll_locked / is_ldo_stable / wait_ready");
}
/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */

static void test_attach_and_dispatch_status_chg(void)
{
  TEST_BEGIN("mipi_phy attach + dispatch (status_chg)");
  prep_fixture();

  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_phy_attach_handler(stub_phy_cb, (void*)(uintptr_t)0xCAFEU));

  /* Run dispatch once to load s_last_sfr with the seeded value. */
  ra8_mipi_phy_dispatch();
  TEST_ASSERT_EQ(1, s_phy_cb_count);

  /* Second dispatch with no edges => status_chg event. */
  ra8_mipi_phy_dispatch();
  TEST_ASSERT_EQ(2, s_phy_cb_count);
  TEST_ASSERT_EQ(k_ra8_mipi_phy_event_status_chg, s_phy_cb_last_event);
  TEST_ASSERT_EQ(((uint32_t)k_ra8_mipi_phy_sfr_pwrsf | (uint32_t)k_ra8_mipi_phy_sfr_pllsf),
                 s_phy_cb_last_sfr);
  TEST_ASSERT(s_phy_cb_last_ctx == (void*)(uintptr_t)0xCAFEU);

  /* Detach -- next dispatch must not increment the count. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_phy_attach_handler(nullptr, nullptr));
  ra8_mipi_phy_dispatch();
  TEST_ASSERT_EQ(2, s_phy_cb_count);

  TEST_END("mipi_phy attach + dispatch (status_chg)");
}
/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */

static void test_dispatch_decodes_edges(void)
{
  TEST_BEGIN("mipi_phy dispatch decodes ldo / pll edges");
  prep_fixture();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_phy_attach_handler(stub_phy_cb, nullptr));

  /* Start with everything cleared and dispatch -- s_last_sfr seeds. */
  *ra8_mipi_phy_reg32(k_ra8_mipi_phy_off_sfr) = 0U;
  ra8_mipi_phy_dispatch();

  /* PWRSF rising. */
  *ra8_mipi_phy_reg32(k_ra8_mipi_phy_off_sfr) = (uint32_t)k_ra8_mipi_phy_sfr_pwrsf;
  ra8_mipi_phy_dispatch();
  TEST_ASSERT_EQ(k_ra8_mipi_phy_event_ldo_ready, s_phy_cb_last_event);

  /* PLLSF rising. */
  *ra8_mipi_phy_reg32(k_ra8_mipi_phy_off_sfr) =
    (uint32_t)k_ra8_mipi_phy_sfr_pwrsf | (uint32_t)k_ra8_mipi_phy_sfr_pllsf;
  ra8_mipi_phy_dispatch();
  TEST_ASSERT_EQ(k_ra8_mipi_phy_event_pll_locked, s_phy_cb_last_event);

  /* PLLSF falling. */
  *ra8_mipi_phy_reg32(k_ra8_mipi_phy_off_sfr) = (uint32_t)k_ra8_mipi_phy_sfr_pwrsf;
  ra8_mipi_phy_dispatch();
  TEST_ASSERT_EQ(k_ra8_mipi_phy_event_pll_lost, s_phy_cb_last_event);

  /* PWRSF falling. */
  *ra8_mipi_phy_reg32(k_ra8_mipi_phy_off_sfr) = 0U;
  ra8_mipi_phy_dispatch();
  TEST_ASSERT_EQ(k_ra8_mipi_phy_event_ldo_lost, s_phy_cb_last_event);

  TEST_END("mipi_phy dispatch decodes ldo / pll edges");
}
/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */

static void test_power_transition(void)
{
  TEST_BEGIN("mipi_phy power transition");
  prep_fixture();

  const ra8_mipi_phy_timing_t tim = make_timing();
  const ra8_mipi_phy_config_t cfg = make_dsi_cfg(&tim);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_phy_init(&cfg));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_phy_enter_stop());
  TEST_ASSERT_EQ(0, *ra8_mipi_phy_reg32(k_ra8_mipi_phy_off_pwrcr));

  /* Restore the status flags before exit_stop reruns init(). */
  *ra8_mipi_phy_reg32(k_ra8_mipi_phy_off_sfr) =
    (uint32_t)k_ra8_mipi_phy_sfr_pwrsf | (uint32_t)k_ra8_mipi_phy_sfr_pllsf;

  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_phy_exit_stop(&cfg));
  TEST_ASSERT_EQ(k_ra8_mipi_phy_ocr_dphyen, *ra8_mipi_phy_reg32(k_ra8_mipi_phy_off_ocr));

  TEST_END("mipi_phy power transition");
}
/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */

static void test_set_lane_speed_happy(void)
{
  TEST_BEGIN("mipi_phy set_lane_speed happy");
  prep_fixture();

  const ra8_mipi_phy_timing_t tim = make_timing();
  const ra8_mipi_phy_config_t cfg = make_dsi_cfg(&tim);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_phy_init(&cfg));

  ra8_mipi_phy_pll_t pll = make_pll();
  pll.nmul_int           = 200U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_phy_set_lane_speed(&pll));

  /* DPHYPLFCR should hold the new NMUL in [24:16]. */
  const uint32_t plfcr = *ra8_mipi_phy_reg32(k_ra8_mipi_phy_off_plfcr);
  const uint32_t nmul_rb =
    (plfcr >> (uint32_t)k_ra8_mipi_phy_plfcr_shift_nmul) & (uint32_t)k_ra8_mipi_phy_plfcr_mask_nmul;
  TEST_ASSERT_EQ(200, nmul_rb);

  /* PLLSTP must be cleared after relock. */
  TEST_ASSERT_EQ(0, *ra8_mipi_phy_reg32(k_ra8_mipi_phy_off_plocr));

  TEST_END("mipi_phy set_lane_speed happy");
}
/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */

static void test_set_lane_speed_rejects(void)
{
  TEST_BEGIN("mipi_phy set_lane_speed rejects bad input");
  prep_fixture();

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_mipi_phy_set_lane_speed(nullptr));

  ra8_mipi_phy_pll_t pll = make_pll();
  pll.nmul_int           = (uint16_t)k_test_mipi_phy_nmul_bad_hi;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_mipi_phy_set_lane_speed(&pll));

  TEST_END("mipi_phy set_lane_speed rejects bad input");
}
/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */

static void test_switch_mode(void)
{
  TEST_BEGIN("mipi_phy switch_mode toggles MASTEREN + clears DPHYEN");
  prep_fixture();

  const ra8_mipi_phy_timing_t tim = make_timing();
  const ra8_mipi_phy_config_t cfg = make_dsi_cfg(&tim);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_phy_init(&cfg));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_phy_switch_mode(k_ra8_mipi_phy_mode_csi_device));
  TEST_ASSERT_EQ(0, *ra8_mipi_phy_reg32(k_ra8_mipi_phy_off_mdc));
  TEST_ASSERT_EQ(0, *ra8_mipi_phy_reg32(k_ra8_mipi_phy_off_ocr));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_phy_switch_mode(k_ra8_mipi_phy_mode_dsi_host));
  TEST_ASSERT_EQ(k_ra8_mipi_phy_mdc_hosten, *ra8_mipi_phy_reg32(k_ra8_mipi_phy_off_mdc));

  /* Bad arg. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_mipi_phy_switch_mode((ra8_mipi_phy_mode_t)9U));

  TEST_END("mipi_phy switch_mode toggles MASTEREN + clears DPHYEN");
}
/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */

static void test_lane_count_get_set(void)
{
  TEST_BEGIN("mipi_phy lane count get/set");
  prep_fixture();

  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_phy_set_lane_count(k_ra8_mipi_phy_lane_count_1));
  TEST_ASSERT_EQ(k_ra8_mipi_phy_lane_count_1, ra8_mipi_phy_get_lane_count());
  /* Lane 1 should now be auto-disabled. */
  TEST_ASSERT(!ra8_mipi_phy_is_lane_enabled(k_ra8_mipi_phy_lane_d1));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_phy_set_lane_count(k_ra8_mipi_phy_lane_count_2));
  TEST_ASSERT(ra8_mipi_phy_is_lane_enabled(k_ra8_mipi_phy_lane_d1));

  TEST_ASSERT_EQ(k_ra8_err_not_supported, ra8_mipi_phy_set_lane_count(k_ra8_mipi_phy_lane_count_3));
  TEST_ASSERT_EQ(k_ra8_err_not_supported, ra8_mipi_phy_set_lane_count(k_ra8_mipi_phy_lane_count_4));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_mipi_phy_set_lane_count((ra8_mipi_phy_lane_count_t)0U));

  TEST_END("mipi_phy lane count get/set");
}
/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */

static void test_lane_enable_disable(void)
{
  TEST_BEGIN("mipi_phy per-lane enable / disable");
  prep_fixture();

  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_phy_set_lane_count(k_ra8_mipi_phy_lane_count_2));

  TEST_ASSERT(ra8_mipi_phy_is_lane_enabled(k_ra8_mipi_phy_lane_clk));
  TEST_ASSERT(ra8_mipi_phy_is_lane_enabled(k_ra8_mipi_phy_lane_d0));
  TEST_ASSERT(ra8_mipi_phy_is_lane_enabled(k_ra8_mipi_phy_lane_d1));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_phy_set_lane_enable(k_ra8_mipi_phy_lane_d1, false));
  TEST_ASSERT(!ra8_mipi_phy_is_lane_enabled(k_ra8_mipi_phy_lane_d1));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_phy_set_lane_enable(k_ra8_mipi_phy_lane_d1, true));
  TEST_ASSERT(ra8_mipi_phy_is_lane_enabled(k_ra8_mipi_phy_lane_d1));

  /* Cannot enable lane 1 when count == 1. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_phy_set_lane_count(k_ra8_mipi_phy_lane_count_1));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_mipi_phy_set_lane_enable(k_ra8_mipi_phy_lane_d1, true));

  /* Lanes 2 / 3 do not exist. */
  TEST_ASSERT_EQ(k_ra8_err_not_supported,
                 ra8_mipi_phy_set_lane_enable(k_ra8_mipi_phy_lane_d2, true));
  TEST_ASSERT_EQ(k_ra8_err_not_supported,
                 ra8_mipi_phy_set_lane_enable(k_ra8_mipi_phy_lane_d3, false));

  TEST_END("mipi_phy per-lane enable / disable");
}
/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */

static void test_clock_mode(void)
{
  TEST_BEGIN("mipi_phy clock_mode setter / getter");
  prep_fixture();

  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_phy_set_clock_mode(k_ra8_mipi_phy_clk_continuous));
  TEST_ASSERT_EQ(k_ra8_mipi_phy_clk_continuous, ra8_mipi_phy_get_clock_mode());

  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_phy_set_clock_mode(k_ra8_mipi_phy_clk_noncontinuous));
  TEST_ASSERT_EQ(k_ra8_mipi_phy_clk_noncontinuous, ra8_mipi_phy_get_clock_mode());

  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_mipi_phy_set_clock_mode((ra8_mipi_phy_clk_mode_t)9U));

  TEST_END("mipi_phy clock_mode setter / getter");
}
/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */

static void test_eotp(void)
{
  TEST_BEGIN("mipi_phy eotp setter / getter");
  prep_fixture();

  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_phy_set_eotp(k_ra8_mipi_phy_eotp_enabled));
  TEST_ASSERT_EQ(k_ra8_mipi_phy_eotp_enabled, ra8_mipi_phy_get_eotp());

  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_phy_set_eotp(k_ra8_mipi_phy_eotp_disabled));
  TEST_ASSERT_EQ(k_ra8_mipi_phy_eotp_disabled, ra8_mipi_phy_get_eotp());

  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_mipi_phy_set_eotp((ra8_mipi_phy_eotp_t)9U));

  TEST_END("mipi_phy eotp setter / getter");
}
/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */

static void test_set_pclka_freq(void)
{
  TEST_BEGIN("mipi_phy set_pclka_freq");
  prep_fixture();

  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_phy_set_pclka_freq(80U));
  /* RFREQ encodes (MHz - 1); 80 MHz -> 0x4F. HUM Ch 64.2.1 p 3822. */
  TEST_ASSERT_EQ((80U - (uint32_t)k_ra8_mipi_phy_refcr_rfreq_bias),
                 *ra8_mipi_phy_reg32(k_ra8_mipi_phy_off_refcr));

  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_mipi_phy_set_pclka_freq(39U));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_mipi_phy_set_pclka_freq(126U));

  TEST_END("mipi_phy set_pclka_freq");
}
/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */

static void test_set_escape_divisor(void)
{
  TEST_BEGIN("mipi_phy set_escape_divisor");
  prep_fixture();

  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_phy_set_escape_divisor(7U));
  TEST_ASSERT_EQ(7, *ra8_mipi_phy_reg32(k_ra8_mipi_phy_off_esccr));
  TEST_ASSERT_EQ(0, *ra8_mipi_phy_reg32(k_ra8_mipi_phy_off_plocr));

  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_mipi_phy_set_escape_divisor(32U));

  TEST_END("mipi_phy set_escape_divisor");
}
/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */

static void test_pll_start_stop(void)
{
  TEST_BEGIN("mipi_phy pll start / stop / ldo enable / disable");
  prep_fixture();

  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_phy_ldo_enable());
  TEST_ASSERT_EQ(k_ra8_mipi_phy_pwrcr_pwrsen, *ra8_mipi_phy_reg32(k_ra8_mipi_phy_off_pwrcr));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_phy_pll_start());
  TEST_ASSERT_EQ(0, *ra8_mipi_phy_reg32(k_ra8_mipi_phy_off_plocr));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_phy_pll_stop());
  TEST_ASSERT_EQ(k_ra8_mipi_phy_plocr_pllstp, *ra8_mipi_phy_reg32(k_ra8_mipi_phy_off_plocr));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_phy_ldo_disable());
  TEST_ASSERT_EQ(0, *ra8_mipi_phy_reg32(k_ra8_mipi_phy_off_pwrcr));

  /* Timeout paths. */
  *ra8_mipi_phy_reg32(k_ra8_mipi_phy_off_sfr) = 0U;
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout, ra8_mipi_phy_ldo_enable());
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout, ra8_mipi_phy_pll_start());

  TEST_END("mipi_phy pll start / stop / ldo enable / disable");
}
/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */

static void test_select_timing_dsi_125_250(void)
{
  TEST_BEGIN("mipi_phy select_timing DSI 125 MHz 250 Mbps");
  prep_fixture();

  ra8_mipi_phy_timing_t out;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_mipi_phy_select_timing(k_ra8_mipi_phy_mode_dsi_host, 125U, 250U, &out));
  /* HUM Table 64.2 PCLKA 125 MHz / 150-250 column row, p 3832. */
  TEST_ASSERT_EQ(0x000124F9U, out.tinit);
  TEST_ASSERT_EQ(0x21U, out.tclkzero);
  TEST_ASSERT_EQ(0x08U, out.tlpx);

  TEST_END("mipi_phy select_timing DSI 125 MHz 250 Mbps");
}
/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */

static void test_select_timing_csi_100_300(void)
{
  TEST_BEGIN("mipi_phy select_timing CSI 100 MHz 300 Mbps");
  prep_fixture();

  ra8_mipi_phy_timing_t out;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_mipi_phy_select_timing(k_ra8_mipi_phy_mode_csi_device, 100U, 300U, &out));
  /* HUM Table 64.3 PCLKA 100 MHz / 200-300 column row, p 3835. */
  TEST_ASSERT_EQ(0x0000EA61U, out.tinit);
  TEST_ASSERT_EQ(0x0DU, out.tclkprep);

  TEST_END("mipi_phy select_timing CSI 100 MHz 300 Mbps");
}
/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */

static void test_select_timing_rejects(void)
{
  TEST_BEGIN("mipi_phy select_timing rejects bad input");
  prep_fixture();

  /* Out-of-range rate. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_mipi_phy_select_timing(k_ra8_mipi_phy_mode_dsi_host, 125U, 79U, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_mipi_phy_select_timing(k_ra8_mipi_phy_mode_dsi_host, 125U, 721U, nullptr));
  /* Bad mode. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_mipi_phy_select_timing((ra8_mipi_phy_mode_t)9U, 125U, 250U, nullptr));
  /* PCLKA not in the table. */
  TEST_ASSERT_EQ(k_ra8_err_not_supported,
                 ra8_mipi_phy_select_timing(k_ra8_mipi_phy_mode_dsi_host, 99U, 250U, nullptr));

  TEST_END("mipi_phy select_timing rejects bad input");
}
/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */

static void test_validate_pll_band(void)
{
  TEST_BEGIN("mipi_phy validate_pll_band");
  prep_fixture();

  /* HUM 64.2.2 example p 3823: fMAIN=24.576 (round to 24), IDIV=1/2,
   * NMUL=100, NFMUL=0.5, PMUL=1 -> ~1212 MHz, lands in P=1 band. */
  ra8_mipi_phy_pll_t pll = {
    .idiv     = k_ra8_mipi_phy_idiv_2,
    .pmul     = k_ra8_mipi_phy_pmul_1,
    .nfmul    = k_ra8_mipi_phy_nfmul_0_50,
    .nmul_int = 100U,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_phy_validate_pll_band(&pll, 24U));

  /* Force out-of-band: PMUL=1/8 with high N -> exceeds 375 MHz ceiling. */
  pll.pmul     = k_ra8_mipi_phy_pmul_8;
  pll.nmul_int = 300U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_mipi_phy_validate_pll_band(&pll, 24U));

  /* Bad MOSC. */
  pll.pmul     = k_ra8_mipi_phy_pmul_1;
  pll.nmul_int = 100U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_mipi_phy_validate_pll_band(&pll, 7U));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_mipi_phy_validate_pll_band(&pll, 49U));

  /* NULL pll. */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_mipi_phy_validate_pll_band(nullptr, 24U));

  TEST_END("mipi_phy validate_pll_band");
}
/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */

static void test_init_writes_all_timings(void)
{
  TEST_BEGIN("mipi_phy init writes every DPHYTIMx register");
  prep_fixture();

  const ra8_mipi_phy_timing_t tim = make_timing();
  const ra8_mipi_phy_config_t cfg = make_dsi_cfg(&tim);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_phy_init(&cfg));

  /* TIM2: TCLKPREP in byte 0. */
  TEST_ASSERT_EQ(tim.tclkprep, (*ra8_mipi_phy_reg32(k_ra8_mipi_phy_off_tim2) & 0xFFU));
  /* TIM3: THSPREP in byte 0, THSSETT in byte 1. */
  const uint32_t tim3 = *ra8_mipi_phy_reg32(k_ra8_mipi_phy_off_tim3);
  TEST_ASSERT_EQ(tim.thsprep, (tim3 & 0xFFU));
  TEST_ASSERT_EQ(tim.thssett, ((tim3 >> 8U) & 0xFFU));
  /* TIM4: TCLKZERO in byte 0, TCLKTRL in byte 3. */
  const uint32_t tim4 = *ra8_mipi_phy_reg32(k_ra8_mipi_phy_off_tim4);
  TEST_ASSERT_EQ(tim.tclkzero, (tim4 & 0xFFU));
  TEST_ASSERT_EQ(tim.tclktrl, ((tim4 >> 24U) & 0xFFU));
  /* TIM5: THSZERO in byte 0, THSEXIT in byte 2. */
  const uint32_t tim5 = *ra8_mipi_phy_reg32(k_ra8_mipi_phy_off_tim5);
  TEST_ASSERT_EQ(tim.thszero, (tim5 & 0xFFU));
  TEST_ASSERT_EQ(tim.thsexit, ((tim5 >> 16U) & 0xFFU));

  TEST_END("mipi_phy init writes every DPHYTIMx register");
}

/* MC/DC vector tests for ra8_mipi_phy.c compound decisions
 * (DO-178C Level B / IEC 61508 SIL 3 / ISO 26262 ASIL C). */

typedef enum : uint8_t {
  k_mcdc_phy_pclka_in    = 100U,  /**< Mcdc PHY pclka in.    */
  k_mcdc_phy_pclka_below = 39U,   /**< Mcdc PHY pclka below. */
  k_mcdc_phy_pclka_above = 126U,  /**< Mcdc PHY pclka above. */
  k_mcdc_phy_mosc_lo     = 7U,    /**< Mcdc PHY mosc lo.     */
  k_mcdc_phy_mosc_hi     = 49U,   /**< Mcdc PHY mosc hi.     */
  k_mcdc_phy_mosc_in     = 24U,   /**< Mcdc PHY mosc in.     */
  k_mcdc_phy_lane_bad_5  = 5U,    /**< Mcdc PHY lane bad 5.  */
  k_mcdc_phy_mode_bad    = 0xFFU, /**< Mcdc PHY mode bad.    */
  k_mcdc_phy_clk_bad     = 0xFEU, /**< Mcdc PHY clk bad.     */
  k_mcdc_phy_eotp_bad    = 0xFDU, /**< Mcdc PHY eotp bad.    */
} mcdc_phy_const_t;

typedef enum : uint16_t {
  k_mcdc_phy_nmul_below = 39U,   /**< Mcdc PHY nmul below. */
  k_mcdc_phy_nmul_above = 376U,  /**< Mcdc PHY nmul above. */
  k_mcdc_phy_rate_below = 79U,   /**< Mcdc PHY rate below. */
  k_mcdc_phy_rate_above = 1501U, /**< Mcdc PHY rate above. */
  k_mcdc_phy_rate_in    = 250U,  /**< Mcdc PHY rate in.    */
} mcdc_phy_u16_t;

/**
 * @test test_mcdc_validate_pll_nmul
 * @par MC/DC:
 * Decision (libs/ra8_hal/src/ra8_mipi_phy.c line 221, 2 conditions):
 * `(pll->nmul_int < nmul_min) || (pll->nmul_int > nmul_max)`
 * - V1 nmul=100 -> C1=F C2=F -> F (ok)
 * - V2 nmul=39  -> C1=T (-)  -> T (invalid_arg) [vary C1]
 * - V3 nmul=376 -> C1=F C2=T -> T (invalid_arg) [vary C2]
 * N+1=3 vectors.
 */
static void test_mcdc_validate_pll_nmul(void)
{
  TEST_BEGIN("mipi_phy MC/DC validate_pll: nmul<min || nmul>max");
  prep_fixture();
  ra8_mipi_phy_pll_t pll = make_pll();
  pll.nmul_int           = (uint16_t)k_test_mipi_phy_nmul_int;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_phy_validate_pll_band(&pll, (uint8_t)k_mcdc_phy_mosc_in));
  pll.nmul_int = (uint16_t)k_mcdc_phy_nmul_below;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_mipi_phy_validate_pll_band(&pll, (uint8_t)k_mcdc_phy_mosc_in));
  pll.nmul_int = (uint16_t)k_mcdc_phy_nmul_above;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_mipi_phy_validate_pll_band(&pll, (uint8_t)k_mcdc_phy_mosc_in));
  TEST_END("mipi_phy MC/DC validate_pll: nmul<min || nmul>max");
}

/**
 * @test test_mcdc_validate_init_cfg_null
 * @par MC/DC:
 * Decision (libs/ra8_hal/src/ra8_mipi_phy.c line 1133, 2 conditions):
 * `cfg == nullptr || cfg->p_timing == nullptr`. V1 both non-null -> F;
 * V2 cfg==NULL -> T via C1; V3 ptiming==NULL -> T via C2. N+1=3.
 */
static void test_mcdc_validate_init_cfg_null(void)
{
  TEST_BEGIN("mipi_phy MC/DC init: cfg==NULL || p_timing==NULL");
  prep_fixture();
  const ra8_mipi_phy_timing_t tim = make_timing();
  const ra8_mipi_phy_config_t ok  = make_dsi_cfg(&tim);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_phy_init(&ok));
  prep_fixture();
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_mipi_phy_init(nullptr));
  prep_fixture();
  ra8_mipi_phy_config_t bad = make_dsi_cfg(&tim);
  bad.p_timing              = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_mipi_phy_init(&bad));
  TEST_END("mipi_phy MC/DC init: cfg==NULL || p_timing==NULL");
}

/**
 * @test test_mcdc_validate_init_cfg_pclka
 * @par MC/DC:
 * Decision (libs/ra8_hal/src/ra8_mipi_phy.c line 1136, 2 conditions):
 * `(pclka_mhz < min) || (pclka_mhz > max)`. V1 100 (F ok), V2 39 (T),
 * V3 126 (T). N+1=3.
 */
static void test_mcdc_validate_init_cfg_pclka(void)
{
  TEST_BEGIN("mipi_phy MC/DC init: pclka<min || pclka>max");
  prep_fixture();
  const ra8_mipi_phy_timing_t tim = make_timing();
  ra8_mipi_phy_config_t       cfg = make_dsi_cfg(&tim);
  cfg.pclka_mhz                   = (uint8_t)k_mcdc_phy_pclka_in;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_phy_init(&cfg));
  prep_fixture();
  cfg           = make_dsi_cfg(&tim);
  cfg.pclka_mhz = (uint8_t)k_mcdc_phy_pclka_below;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_mipi_phy_init(&cfg));
  prep_fixture();
  cfg           = make_dsi_cfg(&tim);
  cfg.pclka_mhz = (uint8_t)k_mcdc_phy_pclka_above;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_mipi_phy_init(&cfg));
  TEST_END("mipi_phy MC/DC init: pclka<min || pclka>max");
}

/**
 * @test test_mcdc_validate_init_cfg_lane_count
 * @par MC/DC:
 * Two coupled decisions in libs/ra8_hal/src/ra8_mipi_phy.c:
 *   D1 line 1143: `(lc != 1) && (lc != 2)`
 *   D2 line 1145: `(lc == 3) || (lc == 4)`
 * Vectors lc=2 (D1 F ok), lc=1 (D1 F ok), lc=3 (D1 T D2 T not_supported),
 * lc=5 (D1 T D2 F invalid_arg), lc=4 (D1 T D2 T not_supported). N+1=5.
 */
static void test_mcdc_validate_init_cfg_lane_count(void)
{
  TEST_BEGIN("mipi_phy MC/DC init: lane_count gating + 3/4 disambiguation");
  const ra8_mipi_phy_timing_t tim = make_timing();
  prep_fixture();
  ra8_mipi_phy_config_t cfg = make_dsi_cfg(&tim);
  cfg.lane_count            = k_ra8_mipi_phy_lane_count_2;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_phy_init(&cfg));
  prep_fixture();
  cfg            = make_dsi_cfg(&tim);
  cfg.lane_count = k_ra8_mipi_phy_lane_count_1;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_phy_init(&cfg));
  prep_fixture();
  cfg            = make_dsi_cfg(&tim);
  cfg.lane_count = k_ra8_mipi_phy_lane_count_3;
  TEST_ASSERT_EQ(k_ra8_err_not_supported, ra8_mipi_phy_init(&cfg));
  prep_fixture();
  cfg            = make_dsi_cfg(&tim);
  cfg.lane_count = (ra8_mipi_phy_lane_count_t)k_mcdc_phy_lane_bad_5;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_mipi_phy_init(&cfg));
  prep_fixture();
  cfg            = make_dsi_cfg(&tim);
  cfg.lane_count = k_ra8_mipi_phy_lane_count_4;
  TEST_ASSERT_EQ(k_ra8_err_not_supported, ra8_mipi_phy_init(&cfg));
  TEST_END("mipi_phy MC/DC init: lane_count gating + 3/4 disambiguation");
}

/**
 * @test test_mcdc_dispatch_pwr_pll_edges
 * @par MC/DC:
 * Four cascaded 2-condition AND decisions (libs/ra8_hal/src/ra8_mipi_phy.c
 * lines 1368, 1370, 1372, 1374) for pwr/pll edges. Six dispatch invocations
 * traverse prev->curr SFR transitions to exercise every (T,T) row plus the
 * all-F status_chg baseline = N+1 across the four decisions.
 */
static void test_mcdc_dispatch_pwr_pll_edges(void)
{
  TEST_BEGIN("mipi_phy MC/DC dispatch: pwr/pll edge decisions");
  prep_fixture();
  const ra8_mipi_phy_timing_t tim = make_timing();
  const ra8_mipi_phy_config_t cfg = make_dsi_cfg(&tim);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_phy_init(&cfg));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_phy_attach_handler(stub_phy_cb, (void*)(uintptr_t)0xC0FFEEU));
  volatile uint32_t* sfr = ra8_mipi_phy_reg32(k_ra8_mipi_phy_off_sfr);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_phy_reset());
  *sfr           = (uint32_t)k_ra8_mipi_phy_sfr_pwrsf;
  s_phy_cb_count = 0U;
  ra8_mipi_phy_dispatch();
  TEST_ASSERT_EQ(k_ra8_mipi_phy_event_ldo_ready, s_phy_cb_last_event);
  *sfr           = 0U;
  s_phy_cb_count = 0U;
  ra8_mipi_phy_dispatch();
  TEST_ASSERT_EQ(k_ra8_mipi_phy_event_ldo_lost, s_phy_cb_last_event);
  *sfr           = (uint32_t)k_ra8_mipi_phy_sfr_pwrsf | (uint32_t)k_ra8_mipi_phy_sfr_pllsf;
  s_phy_cb_count = 0U;
  ra8_mipi_phy_dispatch();
  TEST_ASSERT_EQ(k_ra8_mipi_phy_event_ldo_ready, s_phy_cb_last_event);
  *sfr           = (uint32_t)k_ra8_mipi_phy_sfr_pwrsf | (uint32_t)k_ra8_mipi_phy_sfr_pllsf;
  s_phy_cb_count = 0U;
  ra8_mipi_phy_dispatch();
  TEST_ASSERT_EQ(k_ra8_mipi_phy_event_status_chg, s_phy_cb_last_event);
  *sfr           = (uint32_t)k_ra8_mipi_phy_sfr_pwrsf;
  s_phy_cb_count = 0U;
  ra8_mipi_phy_dispatch();
  TEST_ASSERT_EQ(k_ra8_mipi_phy_event_pll_lost, s_phy_cb_last_event);
  *sfr           = (uint32_t)k_ra8_mipi_phy_sfr_pwrsf | (uint32_t)k_ra8_mipi_phy_sfr_pllsf;
  s_phy_cb_count = 0U;
  ra8_mipi_phy_dispatch();
  TEST_ASSERT_EQ(k_ra8_mipi_phy_event_pll_locked, s_phy_cb_last_event);
  TEST_END("mipi_phy MC/DC dispatch: pwr/pll edge decisions");
}

/**
 * @test test_mcdc_switch_mode
 * @par MC/DC:
 * Decision (libs/ra8_hal/src/ra8_mipi_phy.c line 1448, 2 conditions):
 * `(mode != dsi_host) && (mode != csi_device)`. V1 dsi (C1=F ok),
 * V2 csi (C1=T C2=F ok), V3 0xFF (T,T invalid_arg). N+1=3.
 */
static void test_mcdc_switch_mode(void)
{
  TEST_BEGIN("mipi_phy MC/DC switch_mode: mode != dsi && mode != csi");
  prep_fixture();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_phy_switch_mode(k_ra8_mipi_phy_mode_dsi_host));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_phy_switch_mode(k_ra8_mipi_phy_mode_csi_device));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_mipi_phy_switch_mode((ra8_mipi_phy_mode_t)k_mcdc_phy_mode_bad));
  TEST_END("mipi_phy MC/DC switch_mode: mode != dsi && mode != csi");
}

/**
 * @test test_mcdc_set_lane_count
 * @par MC/DC:
 * Two cascaded decisions in libs/ra8_hal/src/ra8_mipi_phy.c:
 *   D1 line 1461: `(c == 3) || (c == 4)`
 *   D2 line 1465: `(c != 1) && (c != 2)`
 * V1=1, V2=2 (both F ok); V3=3, V4=4 (D1 T not_supported); V5=5
 * (D1 F D2 T invalid_arg). N+1=5 over both.
 */
static void test_mcdc_set_lane_count(void)
{
  TEST_BEGIN("mipi_phy MC/DC set_lane_count: 3/4 reject + 1/2 accept");
  prep_fixture();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_phy_set_lane_count(k_ra8_mipi_phy_lane_count_1));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_phy_set_lane_count(k_ra8_mipi_phy_lane_count_2));
  TEST_ASSERT_EQ(k_ra8_err_not_supported, ra8_mipi_phy_set_lane_count(k_ra8_mipi_phy_lane_count_3));
  TEST_ASSERT_EQ(k_ra8_err_not_supported, ra8_mipi_phy_set_lane_count(k_ra8_mipi_phy_lane_count_4));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_mipi_phy_set_lane_count((ra8_mipi_phy_lane_count_t)k_mcdc_phy_lane_bad_5));
  TEST_END("mipi_phy MC/DC set_lane_count: 3/4 reject + 1/2 accept");
}

/**
 * @test test_mcdc_set_lane_enable
 * @par MC/DC:
 * Two decisions in libs/ra8_hal/src/ra8_mipi_phy.c:
 *   D1 line 1486: `(lane == d2) || (lane == d3)`
 *   D2 line 1501: `enable && (s_lane_count == lane_count_1)`
 * D1: clk (F ok), d2 (C1=T not_supported), d3 (C1=F C2=T not_supported).
 * D2 (lane=d1): en=T lc=2 (C1=T C2=F ok), en=F lc=1 (C1=F ok), en=T lc=1
 * (C1=T C2=T invalid_arg). N+1=6 over both decisions.
 */
static void test_mcdc_set_lane_enable(void)
{
  TEST_BEGIN("mipi_phy MC/DC set_lane_enable: D2/D3 reject + D1 lane_count=1");
  prep_fixture();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_phy_set_lane_count(k_ra8_mipi_phy_lane_count_2));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_phy_set_lane_enable(k_ra8_mipi_phy_lane_clk, true));
  TEST_ASSERT_EQ(k_ra8_err_not_supported,
                 ra8_mipi_phy_set_lane_enable(k_ra8_mipi_phy_lane_d2, true));
  TEST_ASSERT_EQ(k_ra8_err_not_supported,
                 ra8_mipi_phy_set_lane_enable(k_ra8_mipi_phy_lane_d3, true));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_phy_set_lane_enable(k_ra8_mipi_phy_lane_d1, true));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_phy_set_lane_count(k_ra8_mipi_phy_lane_count_1));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_phy_set_lane_enable(k_ra8_mipi_phy_lane_d1, false));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_mipi_phy_set_lane_enable(k_ra8_mipi_phy_lane_d1, true));
  TEST_END("mipi_phy MC/DC set_lane_enable: D2/D3 reject + D1 lane_count=1");
}

/**
 * @test test_mcdc_set_clock_mode_eotp
 * @par MC/DC:
 * Two parallel 2-condition decisions in libs/ra8_hal/src/ra8_mipi_phy.c:
 *   D_clk  line 1537: `(mode != continuous) && (mode != noncontinuous)`
 *   D_eotp line 1551: `(eotp != enabled) && (eotp != disabled)`
 * Per decision: V1 valid-1 (C1=F ok), V2 valid-2 (T,F ok), V3 bad
 * (T,T invalid_arg). N+1=3 each.
 */
static void test_mcdc_set_clock_mode_eotp(void)
{
  TEST_BEGIN("mipi_phy MC/DC set_clock_mode + set_eotp guards");
  prep_fixture();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_phy_set_clock_mode(k_ra8_mipi_phy_clk_continuous));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_phy_set_clock_mode(k_ra8_mipi_phy_clk_noncontinuous));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_mipi_phy_set_clock_mode((ra8_mipi_phy_clk_mode_t)k_mcdc_phy_clk_bad));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_phy_set_eotp(k_ra8_mipi_phy_eotp_enabled));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_phy_set_eotp(k_ra8_mipi_phy_eotp_disabled));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_mipi_phy_set_eotp((ra8_mipi_phy_eotp_t)k_mcdc_phy_eotp_bad));
  TEST_END("mipi_phy MC/DC set_clock_mode + set_eotp guards");
}

/**
 * @test test_mcdc_set_pclka_freq
 * @par MC/DC:
 * Decision (libs/ra8_hal/src/ra8_mipi_phy.c line 1568, 2 conditions):
 * `(mhz < min) || (mhz > max)`. V1 100 (F ok), V2 39 (T), V3 126 (T). N+1=3.
 */
static void test_mcdc_set_pclka_freq(void)
{
  TEST_BEGIN("mipi_phy MC/DC set_pclka_freq: mhz<min || mhz>max");
  prep_fixture();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_phy_set_pclka_freq((uint8_t)k_mcdc_phy_pclka_in));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_mipi_phy_set_pclka_freq((uint8_t)k_mcdc_phy_pclka_below));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_mipi_phy_set_pclka_freq((uint8_t)k_mcdc_phy_pclka_above));
  TEST_END("mipi_phy MC/DC set_pclka_freq: mhz<min || mhz>max");
}

/**
 * @test test_mcdc_select_timing
 * @par MC/DC:
 * Two decisions in libs/ra8_hal/src/ra8_mipi_phy.c:
 *   D1 line 1598: `(mode != dsi_host) && (mode != csi_device)`
 *   D2 line 1602: `(rate < min) || (rate > max)`
 * dsi+rate_in (both F), dsi+rate=79 (D2 C1=T), dsi+rate=1501 (D2 C1=F C2=T),
 * bad_mode+rate_in (D1 T), csi+rate_in (D1 F via C2=F). N+1=5 vectors.
 */
static void test_mcdc_select_timing(void)
{
  TEST_BEGIN("mipi_phy MC/DC select_timing: mode + rate guards");
  prep_fixture();
  ra8_mipi_phy_timing_t out = {};
  ra8_err_t             e1  = ra8_mipi_phy_select_timing(k_ra8_mipi_phy_mode_dsi_host,
                                                         (uint8_t)k_test_mipi_phy_pclka_mhz,
                                                         (uint16_t)k_mcdc_phy_rate_in,
                                                         &out);
  TEST_ASSERT(e1 == k_ra8_ok || e1 == k_ra8_err_not_supported);
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_mipi_phy_select_timing(k_ra8_mipi_phy_mode_dsi_host,
                                            (uint8_t)k_test_mipi_phy_pclka_mhz,
                                            (uint16_t)k_mcdc_phy_rate_below,
                                            &out));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_mipi_phy_select_timing(k_ra8_mipi_phy_mode_dsi_host,
                                            (uint8_t)k_test_mipi_phy_pclka_mhz,
                                            (uint16_t)k_mcdc_phy_rate_above,
                                            &out));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_mipi_phy_select_timing((ra8_mipi_phy_mode_t)k_mcdc_phy_mode_bad,
                                            (uint8_t)k_test_mipi_phy_pclka_mhz,
                                            (uint16_t)k_mcdc_phy_rate_in,
                                            &out));
  ra8_err_t e5 = ra8_mipi_phy_select_timing(k_ra8_mipi_phy_mode_csi_device,
                                            (uint8_t)k_test_mipi_phy_pclka_mhz,
                                            (uint16_t)k_mcdc_phy_rate_in,
                                            &out);
  TEST_ASSERT(e5 == k_ra8_ok || e5 == k_ra8_err_not_supported);
  TEST_END("mipi_phy MC/DC select_timing: mode + rate guards");
}

/**
 * @test test_mcdc_validate_pll_band_mosc_and_freq
 * @par MC/DC:
 * Two decisions in libs/ra8_hal/src/ra8_mipi_phy.c:
 *   D_mosc line 1636: `(mosc < min) || (mosc > max)`
 *   D_freq line 1667: `(f_mhz < lo) || (f_mhz > hi)` per PMUL band
 * V1 mosc_in + in-band PLL (both F ok), V2 mosc=7 (D_mosc T), V3 mosc=49
 * (D_mosc T), V5 small-nmul -> f<80 (D_freq T), V6 large-nmul small-idiv
 * -> f>1500 (D_freq T). N+1=6.
 */
static void test_mcdc_validate_pll_band_mosc_and_freq(void)
{
  TEST_BEGIN("mipi_phy MC/DC validate_pll_band: mosc range + freq band");
  prep_fixture();
  ra8_mipi_phy_pll_t pll = make_pll();
  pll.idiv               = k_ra8_mipi_phy_idiv_2;
  pll.pmul               = k_ra8_mipi_phy_pmul_1;
  pll.nfmul              = k_ra8_mipi_phy_nfmul_0_00;
  pll.nmul_int           = 100U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_phy_validate_pll_band(&pll, (uint8_t)k_mcdc_phy_mosc_in));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_mipi_phy_validate_pll_band(&pll, (uint8_t)k_mcdc_phy_mosc_lo));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_mipi_phy_validate_pll_band(&pll, (uint8_t)k_mcdc_phy_mosc_hi));
  pll.nmul_int = 40U;
  pll.idiv     = k_ra8_mipi_phy_idiv_4;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_mipi_phy_validate_pll_band(&pll, (uint8_t)k_mcdc_phy_mosc_in));
  pll.nmul_int = 375U;
  pll.idiv     = k_ra8_mipi_phy_idiv_1;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_mipi_phy_validate_pll_band(&pll, (uint8_t)k_mcdc_phy_mosc_in));
  TEST_END("mipi_phy MC/DC validate_pll_band: mosc range + freq band");
}

int32_t main(void)
{
  test_init_happy_dsi_host();
  test_init_csi_device_skips_pll();
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
  test_mcdc_validate_pll_nmul();
  test_mcdc_validate_init_cfg_null();
  test_mcdc_validate_init_cfg_pclka();
  test_mcdc_validate_init_cfg_lane_count();
  test_mcdc_dispatch_pwr_pll_edges();
  test_mcdc_switch_mode();
  test_mcdc_set_lane_count();
  test_mcdc_set_lane_enable();
  test_mcdc_set_clock_mode_eotp();
  test_mcdc_set_pclka_freq();
  test_mcdc_select_timing();
  test_mcdc_validate_pll_band_mosc_and_freq();
  (void)fprintf(stderr, "[OK  ] test_ra8_mipi_phy.c\n");
  return 0;
}
