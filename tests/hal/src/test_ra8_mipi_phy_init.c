/**
 * @file test_ra8_mipi_phy_init.c
 * @brief Unit tests for the MIPI D-PHY bring-up, status and dispatch surface
 *
 * @details
 * Split sibling of the original test_ra8_mipi_phy.c suite covering
 * init / deinit / reset and the status surface of ra8_mipi_phy.c.
 * The host test fixture pre-seeds DPHYSFR with PWRSF | PLLSF before
 * calling ``ra8_mipi_phy_init`` so the LDO and PLL stabilisation
 * polls complete on the first iteration. Real silicon needs ~us
 * for the LDO and ~tens of us for the PLL, both validated against
 * HUM Ch 64.3.1 p 3837.
 *
 * Covers: DSI-host and CSI-device init incl. every rejection leg and
 * the LDO / PLL timeout paths, deinit / reset / recover, status get +
 * clear, the is_* helpers, callback dispatch edge decoding, power
 * transition, PLL band validation and the timing-register writeback.
 *
 * Sibling suite: test_ra8_mipi_phy_lanes.c (lane / clock / PLL /
 * timing setters + the MC/DC vector tests).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_mipi_phy.h"
#include "ra8_mipi_phy_regs.h"
#include "unity_minimal.h"

/**
 * @enum t_phy_init_t
 * @brief PLL multipliers and the callback out-parameter seed.
 */
typedef enum : uint16_t {
  k_t_nmul_valid = 100U,  /**< A multiplier inside the validator's range. */
  k_t_nmul_over  = 300U,  /**< One past its maximum.                      */
  k_t_event_none = 0xFFU, /**< "No event delivered yet"; a callback that fires
                                without an event id leaves this value.         */
} t_phy_init_t;

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
  ra8_fake_mmap_reset();
  s_phy_cb_count      = 0U;
  s_phy_cb_last_sfr   = 0U;
  s_phy_cb_last_ctx   = nullptr;
  s_phy_cb_last_event = k_t_event_none;
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
  ra8_fake_mmap_reset(); /* zeros DPHYSFR -- LDO never asserts. */

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
  ra8_fake_mmap_reset();
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

static void test_status_get(void)
{
  TEST_BEGIN("mipi_phy status get");
  prep_fixture();

  uint32_t mask = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_phy_get_status(&mask));
  TEST_ASSERT((mask & (uint32_t)k_ra8_mipi_phy_sfr_pwrsf) != 0U);
  TEST_ASSERT((mask & (uint32_t)k_ra8_mipi_phy_sfr_pllsf) != 0U);

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_mipi_phy_get_status(nullptr));

  TEST_END("mipi_phy status get");
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
    .nmul_int = k_t_nmul_valid,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_phy_validate_pll_band(&pll, 24U));

  /* Force out-of-band: PMUL=1/8 with high N -> exceeds 375 MHz ceiling. */
  pll.pmul     = k_ra8_mipi_phy_pmul_8;
  pll.nmul_int = k_t_nmul_over;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_mipi_phy_validate_pll_band(&pll, 24U));

  /* Bad MOSC. */
  pll.pmul     = k_ra8_mipi_phy_pmul_1;
  pll.nmul_int = k_t_nmul_valid;
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
  test_init_happy_dsi_host,
  test_init_csi_device_skips_pll,
  test_init_null_cfg_rejected,
  test_init_null_timing_rejected,
  test_init_pclka_out_of_range,
  test_init_escdiv_out_of_range,
  test_init_nmul_out_of_range,
  test_init_lane_count_3_4_rejected,
  test_init_lane_count_1_accepted,
  test_init_ldo_timeout,
  test_init_pll_timeout,
  test_deinit_clears_enables,
  test_reset_clears_all_regs,
  test_recover_from_error,
  test_status_get,
  test_is_helpers,
  test_attach_and_dispatch_status_chg,
  test_dispatch_decodes_edges,
  test_power_transition,
  test_validate_pll_band,
  test_init_writes_all_timings,
};

int main(void)
{
  for (size_t i = 0U; i < (sizeof s_test_roster / sizeof s_test_roster[0]); ++i) {
    s_test_roster[i]();
  }
  return 0;
}
