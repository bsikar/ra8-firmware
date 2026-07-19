/**
 * @file test_ra8_lpm.c
 * @brief Unit tests for ra8_lpm.c (Low Power Mode HAL driver)
 *
 * @details
 * Drives every public API in ``ra8_lpm.h`` through the host sim mmap
 * and verifies the resulting register state. Covers lifecycle,
 * PRCR unlock/relock, raw + per-source WUPEN0/1 manipulation,
 * DPSIER/DPSIFR/DPSIEGR programming, snooze request/end source
 * helpers, RAM/LDO retention, clock-shutdown matrix, OPCCR read +
 * timeout-wait, all six sleep modes (including the SCR.SLEEPDEEP
 * read-modify-write against the RAM-backed SCS window), and
 * packed-status diagnostics.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_err.h"
#include "ra8_lpm.h"
#include "ra8_lpm_regs.h"
#include "ra8_sim_mmap.h"
#include "unity_minimal.h"

/**
 * @enum lpm_uint8_const_t
 * @brief Named uint8_t constants used by this file.
 *
 * @details
 * Every literal this translation unit needs, named so the
 * value's role is visible at the point of use (CLAUDE.md
 * "No Magic Numbers").
 */
typedef enum : uint8_t {
  k_lpm_probe_dpsifr3 =
    0x55U, /**< Planted in DPSIFR3; the complement of the DPSIFR0 value, so a swap between the two is unmistakable. */
  k_lpm_probe_dpsifr0  = 0xAAU, /**< Planted in DPSIFR0.                                       */
  k_lpm_probe_lpscr    = 0x05U, /**< Planted in LPSCR, and reused for SSCR1.                   */
  k_lpm_probe_dpsier0  = 0x11U, /**< Planted in DPSIER0.                                       */
  k_lpm_probe_dpsier1  = 0x22U, /**< Planted in DPSIER1.                                       */
  k_lpm_probe_dpsiegr0 = 0x33U, /**< Planted in DPSIEGR0.                                      */
  k_lpm_probe_sbycr    = 0x40U, /**< Planted in SBYCR.                                         */
  k_lpm_probe_dpsbycr  = 0x54U, /**< Planted in DPSBYCR.                                       */
  k_lpm_probe_top_bit  = 0x80U, /**< A single high bit, planted where only one flag must move. */
  k_lpm_all_ones       = 0xFFU, /**< All flags set, so a clear that missed a bit leaves evidence. */
} lpm_uint8_const_t;

/**
 * @enum ra8_lpm_test_const_t
 * @brief Magic values used by the LPM unit tests.
 */
typedef enum : uint32_t {
  k_ra8_lpm_test_wupen0_pattern  = 0x0123ABCDUL, /**< Arbitrary WUPEN0 pattern.  */
  k_ra8_lpm_test_wupen1_pattern  = 0xDEADBEEFUL, /**< Arbitrary WUPEN1 pattern.  */
  k_ra8_lpm_test_bad_mode_value  = 0x77U,        /**< Not a valid LPMD encoding. */
  k_ra8_lpm_test_poll_budget     = 16U,          /**< OPCMTSF poll budget.       */
  k_ra8_lpm_test_scr_sleeponexit = 0x00000002UL, /**< SCR.SLEEPONEXIT @ bit 1 (sibling
                                                  *   bit the driver must preserve). */
} ra8_lpm_test_const_t;

static ra8_lpm_config_t make_default_cfg(void)
{
  const ra8_lpm_config_t cfg = {
    .io_port_keep     = true,
    .opa_bus_keep     = true,
    .sscr_fast_return = true,
    .dcdc_softstart   = k_ra8_lpm_dcssmode_256us,
    .sscr_low_power   = k_ra8_lpm_ss2lp_low,
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
  TEST_BEGIN("lpm init happy");
  ra8_sim_mmap_reset();

  const ra8_lpm_config_t cfg = make_default_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_lpm_init(&cfg));

  /* SBYCR.OPE bit set; bits 5:0 and 7 cleared. */
  TEST_ASSERT_EQ(k_ra8_lpm_sbycr_ope_mask, (*ra8_lpm_sysc_reg8(k_ra8_lpm_sbycr_off)));

  /* DPSBYCR: IOKEEP set, DCSSMODE = 0b10 << 2. */
  const uint8_t expected_dpsbycr =
    (uint8_t)((uint8_t)k_ra8_lpm_dpsbycr_iokeep_mask |
              ((uint8_t)k_ra8_lpm_dcssmode_256us << k_ra8_lpm_dpsbycr_dcssmode_shift));
  TEST_ASSERT_EQ(expected_dpsbycr, (*ra8_lpm_sysc_reg8(k_ra8_lpm_dpsbycr_off)));

  /* SSCR1: SS2FR=1, SS2LP=01b. */
  const uint8_t expected_sscr1 =
    (uint8_t)((uint8_t)k_ra8_lpm_sscr1_ss2fr_mask |
              ((uint8_t)k_ra8_lpm_ss2lp_low << k_ra8_lpm_sscr1_ss2lp_shift));
  TEST_ASSERT_EQ(expected_sscr1, (*ra8_lpm_sysc_reg8(k_ra8_lpm_sscr1_off)));

  /* LPSCR cleared on init -- next WFI is plain CPU sleep. */
  TEST_ASSERT_EQ(0, (*ra8_lpm_sysc_reg8(k_ra8_lpm_lpscr_off)));

  TEST_END("lpm init happy");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_null_cfg(void)
{
  TEST_BEGIN("lpm init null cfg");
  ra8_sim_mmap_reset();

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_lpm_init(nullptr));
  TEST_END("lpm init null cfg");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_no_keep_no_bus(void)
{
  TEST_BEGIN("lpm init no keep no bus");
  ra8_sim_mmap_reset();

  ra8_lpm_config_t cfg = make_default_cfg();
  cfg.io_port_keep     = false;
  cfg.opa_bus_keep     = false;
  cfg.sscr_fast_return = false;
  cfg.dcdc_softstart   = k_ra8_lpm_dcssmode_128us;
  cfg.sscr_low_power   = k_ra8_lpm_ss2lp_default;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_lpm_init(&cfg));

  /* SBYCR has OPE cleared. */
  TEST_ASSERT_EQ(0, (*ra8_lpm_sysc_reg8(k_ra8_lpm_sbycr_off)));
  /* DPSBYCR: IOKEEP cleared, DCSSMODE = 0b01 << 2 = 0x04. */
  const uint8_t expected =
    (uint8_t)((uint8_t)k_ra8_lpm_dcssmode_128us << k_ra8_lpm_dpsbycr_dcssmode_shift);
  TEST_ASSERT_EQ(expected, (*ra8_lpm_sysc_reg8(k_ra8_lpm_dpsbycr_off)));
  /* SSCR1: all bits clear. */
  TEST_ASSERT_EQ(0, (*ra8_lpm_sysc_reg8(k_ra8_lpm_sscr1_off)));
  TEST_END("lpm init no keep no bus");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_set_wakeup_sources(void)
{
  TEST_BEGIN("lpm set_wakeup_sources");
  ra8_sim_mmap_reset();

  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_lpm_set_wakeup_sources((uint32_t)k_ra8_lpm_test_wupen0_pattern,
                                            (uint32_t)k_ra8_lpm_test_wupen1_pattern));

  TEST_ASSERT_EQ(k_ra8_lpm_test_wupen0_pattern, (*ra8_lpm_icu_reg32(k_ra8_lpm_wupen0_off)));
  TEST_ASSERT_EQ(k_ra8_lpm_test_wupen1_pattern, (*ra8_lpm_icu_reg32(k_ra8_lpm_wupen1_off)));
  TEST_END("lpm set_wakeup_sources");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_arm_clear_wupen0(void)
{
  TEST_BEGIN("lpm arm/clear wupen0");
  ra8_sim_mmap_reset();

  TEST_ASSERT_EQ(k_ra8_ok, ra8_lpm_arm_wupen0_bits((uint32_t)k_ra8_lpm_wupen0_irq3));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_lpm_arm_wupen0_bits((uint32_t)k_ra8_lpm_wupen0_rtcalm));
  TEST_ASSERT_EQ(((uint32_t)k_ra8_lpm_wupen0_irq3 | (uint32_t)k_ra8_lpm_wupen0_rtcalm),
                 (*ra8_lpm_icu_reg32(k_ra8_lpm_wupen0_off)));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_lpm_clear_wupen0_bits((uint32_t)k_ra8_lpm_wupen0_irq3));
  TEST_ASSERT_EQ(k_ra8_lpm_wupen0_rtcalm, (*ra8_lpm_icu_reg32(k_ra8_lpm_wupen0_off)));
  TEST_END("lpm arm/clear wupen0");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_arm_clear_wupen1(void)
{
  TEST_BEGIN("lpm arm/clear wupen1");
  ra8_sim_mmap_reset();

  TEST_ASSERT_EQ(k_ra8_ok, ra8_lpm_arm_wupen1_bits((uint32_t)k_ra8_lpm_wupen1_ulpt0u));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_lpm_arm_wupen1_bits((uint32_t)k_ra8_lpm_wupen1_i3c0));
  TEST_ASSERT_EQ(((uint32_t)k_ra8_lpm_wupen1_ulpt0u | (uint32_t)k_ra8_lpm_wupen1_i3c0),
                 (*ra8_lpm_icu_reg32(k_ra8_lpm_wupen1_off)));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_lpm_clear_wupen1_bits((uint32_t)k_ra8_lpm_wupen1_i3c0));
  TEST_ASSERT_EQ(k_ra8_lpm_wupen1_ulpt0u, (*ra8_lpm_icu_reg32(k_ra8_lpm_wupen1_off)));
  TEST_END("lpm arm/clear wupen1");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_arm_dpsier(void)
{
  TEST_BEGIN("lpm arm_dpsier all four banks");
  ra8_sim_mmap_reset();

  /* DPSIER0 = IRQ0..IRQ7 mask. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_lpm_arm_dpsier(k_ra8_lpm_dpsier_idx_0, 0xFFU));
  TEST_ASSERT_EQ(0xFF, *ra8_lpm_sysc_reg8(k_ra8_lpm_dpsier0_off));
  /* DPSIFR0 should be cleared by the driver. */
  TEST_ASSERT_EQ(0, *ra8_lpm_sysc_reg8(k_ra8_lpm_dpsifr0_off));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_lpm_arm_dpsier(k_ra8_lpm_dpsier_idx_1, 0xAAU));
  TEST_ASSERT_EQ(0xAA, *ra8_lpm_sysc_reg8(k_ra8_lpm_dpsier1_off));

  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_lpm_arm_dpsier(k_ra8_lpm_dpsier_idx_2, (uint8_t)k_ra8_lpm_dpsier2_drtcaie_mask));
  TEST_ASSERT_EQ(k_ra8_lpm_dpsier2_drtcaie_mask, *ra8_lpm_sysc_reg8(k_ra8_lpm_dpsier2_off));

  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_lpm_arm_dpsier(k_ra8_lpm_dpsier_idx_3, (uint8_t)k_ra8_lpm_dpsier3_diwdtie_mask));
  TEST_ASSERT_EQ(k_ra8_lpm_dpsier3_diwdtie_mask, *ra8_lpm_sysc_reg8(k_ra8_lpm_dpsier3_off));

  /* Bad index. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_lpm_arm_dpsier((ra8_lpm_dpsier_idx_t)5U, 0x01U));
  TEST_END("lpm arm_dpsier all four banks");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_clear_dpsifr(void)
{
  TEST_BEGIN("lpm clear_dpsifr");
  ra8_sim_mmap_reset();

  /* Stamp some 'pending' flags then clear them. */
  *ra8_lpm_sysc_reg8(k_ra8_lpm_dpsifr0_off) = k_lpm_all_ones;
  *ra8_lpm_sysc_reg8(k_ra8_lpm_dpsifr3_off) = k_lpm_probe_top_bit;

  TEST_ASSERT_EQ(k_ra8_ok, ra8_lpm_clear_dpsifr(k_ra8_lpm_dpsier_idx_0));
  TEST_ASSERT_EQ(0, *ra8_lpm_sysc_reg8(k_ra8_lpm_dpsifr0_off));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_lpm_clear_dpsifr(k_ra8_lpm_dpsier_idx_3));
  TEST_ASSERT_EQ(0, *ra8_lpm_sysc_reg8(k_ra8_lpm_dpsifr3_off));

  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_lpm_clear_dpsifr((ra8_lpm_dpsier_idx_t)9U));
  TEST_END("lpm clear_dpsifr");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_set_dpsiegr(void)
{
  TEST_BEGIN("lpm set_dpsiegr");
  ra8_sim_mmap_reset();

  TEST_ASSERT_EQ(k_ra8_ok, ra8_lpm_set_dpsiegr(k_ra8_lpm_dpsier_idx_0, 0x55U));
  TEST_ASSERT_EQ(0x55, *ra8_lpm_sysc_reg8(k_ra8_lpm_dpsiegr0_off));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_lpm_set_dpsiegr(k_ra8_lpm_dpsier_idx_1, 0xAAU));
  TEST_ASSERT_EQ(0xAA, *ra8_lpm_sysc_reg8(k_ra8_lpm_dpsiegr1_off));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_lpm_set_dpsiegr(k_ra8_lpm_dpsier_idx_2, 0x10U));
  TEST_ASSERT_EQ(0x10, *ra8_lpm_sysc_reg8(k_ra8_lpm_dpsiegr2_off));

  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_lpm_set_dpsiegr((ra8_lpm_dpsier_idx_t)4U, 0xFFU));
  TEST_END("lpm set_dpsiegr");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_snooze_request_sources(void)
{
  TEST_BEGIN("lpm snooze_set_request_sources");
  ra8_sim_mmap_reset();

  TEST_ASSERT_EQ(k_ra8_ok, ra8_lpm_snooze_set_request_sources(true, true, true));

  const uint32_t want_w1 = (uint32_t)k_ra8_lpm_wupen1_ulpt0u | (uint32_t)k_ra8_lpm_wupen1_ulpt1u;
  TEST_ASSERT_EQ(want_w1, (*ra8_lpm_icu_reg32(k_ra8_lpm_wupen1_off)));
  TEST_ASSERT_EQ(k_ra8_lpm_wupen0_acmphs0, (*ra8_lpm_icu_reg32(k_ra8_lpm_wupen0_off)));
  TEST_END("lpm snooze_set_request_sources");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_snooze_end_sources(void)
{
  TEST_BEGIN("lpm snooze_set_end_sources");
  ra8_sim_mmap_reset();

  /* Pre-stamp DPSIFR3 to verify the dummy-read-then-clear sequence. */
  *ra8_lpm_sysc_reg8(k_ra8_lpm_dpsifr3_off) = k_lpm_all_ones;

  TEST_ASSERT_EQ(k_ra8_ok, ra8_lpm_snooze_set_end_sources(true, false, true, false));

  const uint8_t want =
    (uint8_t)((uint8_t)k_ra8_lpm_dpsier3_dulpt0ie_mask | (uint8_t)k_ra8_lpm_dpsier3_dusbfsie_mask);
  TEST_ASSERT_EQ(want, *ra8_lpm_sysc_reg8(k_ra8_lpm_dpsier3_off));
  TEST_ASSERT_EQ(0, *ra8_lpm_sysc_reg8(k_ra8_lpm_dpsifr3_off));
  TEST_END("lpm snooze_set_end_sources");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_ram_retention(void)
{
  TEST_BEGIN("lpm set_ram_retention");
  ra8_sim_mmap_reset();

  ra8_lpm_ram_retention_t cfg = {
    .pdramscr0_bits = (uint16_t)k_ra8_lpm_pdramscr0_all_keep,
    .cpu0_tcm_keep  = true,
    .cpu1_tcm_keep  = false,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_lpm_set_ram_retention(&cfg));
  TEST_ASSERT_EQ(k_ra8_lpm_pdramscr0_all_keep, *ra8_lpm_sysc_reg16(k_ra8_lpm_pdramscr0_off));
  TEST_ASSERT_EQ(k_ra8_lpm_pdramscr1_rkeep0_mask, *ra8_lpm_sysc_reg8(k_ra8_lpm_pdramscr1_off));

  cfg.cpu0_tcm_keep  = false;
  cfg.cpu1_tcm_keep  = true;
  cfg.pdramscr0_bits = (uint16_t)k_ra8_lpm_pdramscr0_sram1_keep;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_lpm_set_ram_retention(&cfg));
  TEST_ASSERT_EQ(k_ra8_lpm_pdramscr0_sram1_keep, *ra8_lpm_sysc_reg16(k_ra8_lpm_pdramscr0_off));
  TEST_ASSERT_EQ(k_ra8_lpm_pdramscr1_rkeep1_mask, *ra8_lpm_sysc_reg8(k_ra8_lpm_pdramscr1_off));

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_lpm_set_ram_retention(nullptr));
  TEST_END("lpm set_ram_retention");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_ldo_standby(void)
{
  TEST_BEGIN("lpm set_ldo_standby");
  ra8_sim_mmap_reset();

  /* OPCCR.OPCM = 0 (HSM) so writes are permitted. */
  *ra8_lpm_sysc_reg8(k_ra8_lpm_opccr_off) = 0U;

  ra8_lpm_ldo_cfg_t cfg = {
    .pll1 = k_ra8_lpm_ldo_retained,
    .pll2 = k_ra8_lpm_ldo_disabled,
    .hoco = k_ra8_lpm_ldo_retained,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_lpm_set_ldo_standby(&cfg));
  TEST_ASSERT_EQ(
    k_ra8_lpm_ldocr_skeep_mask,
    ((*ra8_lpm_sysc_reg8(k_ra8_lpm_pll1ldocr_off)) & (uint8_t)k_ra8_lpm_ldocr_skeep_mask));
  TEST_ASSERT_EQ(
    0,
    ((*ra8_lpm_sysc_reg8(k_ra8_lpm_pll2ldocr_off)) & (uint8_t)k_ra8_lpm_ldocr_skeep_mask));
  TEST_ASSERT_EQ(
    k_ra8_lpm_ldocr_skeep_mask,
    ((*ra8_lpm_sysc_reg8(k_ra8_lpm_hocoldocr_off)) & (uint8_t)k_ra8_lpm_ldocr_skeep_mask));

  /* Force OPCM != 0 -- write must be rejected with invalid_state. */
  *ra8_lpm_sysc_reg8(k_ra8_lpm_opccr_off) = 0x01U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_lpm_set_ldo_standby(&cfg));

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_lpm_set_ldo_standby(nullptr));
  TEST_END("lpm set_ldo_standby");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_clock_stop_each(void)
{
  TEST_BEGIN("lpm set_clock_stop / get_clock_stop");
  ra8_sim_mmap_reset();

  bool                  v        = false;
  const ra8_lpm_clock_t clocks[] = {
    k_ra8_lpm_clock_moco,
    k_ra8_lpm_clock_hoco,
    k_ra8_lpm_clock_loco,
    k_ra8_lpm_clock_main,
    k_ra8_lpm_clock_sub,
  };
  for (uint8_t i = 0U; i < (uint8_t)(sizeof(clocks) / sizeof(clocks[0])); ++i) {
    TEST_ASSERT_EQ(k_ra8_ok, ra8_lpm_set_clock_stop(clocks[i], true));
    TEST_ASSERT_EQ(k_ra8_ok, ra8_lpm_get_clock_stop(clocks[i], &v));
    TEST_ASSERT(v);
    TEST_ASSERT_EQ(k_ra8_ok, ra8_lpm_set_clock_stop(clocks[i], false));
    TEST_ASSERT_EQ(k_ra8_ok, ra8_lpm_get_clock_stop(clocks[i], &v));
    TEST_ASSERT(!v);
  }

  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_lpm_set_clock_stop(k_ra8_lpm_clock_count, true));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_lpm_get_clock_stop(k_ra8_lpm_clock_count, &v));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_lpm_get_clock_stop(k_ra8_lpm_clock_moco, nullptr));
  TEST_END("lpm set_clock_stop / get_clock_stop");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_opccr_read_and_wait(void)
{
  TEST_BEGIN("lpm get_opccr + wait_for_opccr");
  ra8_sim_mmap_reset();

  /* Hand-set OPCCR to "transition complete" (OPCMTSF=0). */
  *ra8_lpm_sysc_reg8(k_ra8_lpm_opccr_off) = 0x00U;
  uint8_t v                               = k_lpm_all_ones;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_lpm_get_opccr(&v));
  TEST_ASSERT_EQ(0x00, v);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_lpm_wait_for_opccr((uint32_t)k_ra8_lpm_test_poll_budget));

  /* Stick OPCMTSF=1 and observe the timeout. */
  *ra8_lpm_sysc_reg8(k_ra8_lpm_opccr_off) = (uint8_t)k_ra8_lpm_opccr_opcmtsf_msk;
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout,
                 ra8_lpm_wait_for_opccr((uint32_t)k_ra8_lpm_test_poll_budget));

  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_lpm_wait_for_opccr(0U));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_lpm_get_opccr(nullptr));
  TEST_END("lpm get_opccr + wait_for_opccr");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_prcr_unlock_relock(void)
{
  TEST_BEGIN("lpm prcr unlock/relock");
  ra8_sim_mmap_reset();

  TEST_ASSERT_EQ(k_ra8_ok, ra8_lpm_prcr_unlock());
  /* Upper byte must be the 0xA5 key; lower byte must include PRC1=1. */
  const uint16_t prcr_after_unlock = *ra8_lpm_sysc_reg16(k_ra8_lpm_prcr_off);
  TEST_ASSERT_EQ(((uint16_t)k_ra8_lpm_prcr_key), (prcr_after_unlock & (uint16_t)0xFF00U));
  TEST_ASSERT((prcr_after_unlock & (uint16_t)k_ra8_lpm_prcr_prc1_msk) != 0U);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_lpm_prcr_relock());
  const uint16_t prcr_after_relock = *ra8_lpm_sysc_reg16(k_ra8_lpm_prcr_off);
  TEST_ASSERT_EQ(((uint16_t)k_ra8_lpm_prcr_key), (prcr_after_relock & (uint16_t)0xFF00U));
  TEST_ASSERT((prcr_after_relock & (uint16_t)k_ra8_lpm_prcr_prc1_msk) == 0U);
  TEST_END("lpm prcr unlock/relock");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_enter_sleep_modes(void)
{
  TEST_BEGIN("lpm enter sleep modes");
  ra8_sim_mmap_reset();

  /* Plain sleep -- LPSCR.LPMD = 0. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_lpm_enter_sleep(k_ra8_sleep_mode_sleep));
  TEST_ASSERT_EQ(0, (*ra8_lpm_sysc_reg8(k_ra8_lpm_lpscr_off)));

  /* Deep Sleep -- LPSCR.LPMD = 0 (SCR.SLEEPDEEP toggled by driver). */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_lpm_enter_sleep(k_ra8_sleep_mode_deep_sleep));
  TEST_ASSERT_EQ(0, (*ra8_lpm_sysc_reg8(k_ra8_lpm_lpscr_off)));

  /* Software standby / deep standby: the driver clears LPSCR back to 0
   * after WFI returns (see ra8_lpm.c).  This guarantees the *next* WFI
   * is a plain CPU sleep, not a re-entry into standby with no wake
   * source -- the latter is what J-Link's RAMCode helper trips over
   * after a debugger SYSRESETREQ, since the SYSC LPM block is in a
   * separate reset domain and LPSCR survives SYSRESETREQ.  On host
   * the WFI is a no-op so the post-WFI clear is observable
   * immediately. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_lpm_enter_sleep(k_ra8_sleep_mode_software_std));
  TEST_ASSERT_EQ(0, (*ra8_lpm_sysc_reg8(k_ra8_lpm_lpscr_off)));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_lpm_enter_sleep(k_ra8_sleep_mode_deep_standby_1));
  TEST_ASSERT_EQ(0, (*ra8_lpm_sysc_reg8(k_ra8_lpm_lpscr_off)));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_lpm_enter_sleep(k_ra8_sleep_mode_deep_standby_2));
  TEST_ASSERT_EQ(0, (*ra8_lpm_sysc_reg8(k_ra8_lpm_lpscr_off)));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_lpm_enter_sleep(k_ra8_sleep_mode_deep_standby_3));
  TEST_ASSERT_EQ(0, (*ra8_lpm_sysc_reg8(k_ra8_lpm_lpscr_off)));
  TEST_END("lpm enter sleep modes");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- drives both single-condition
 * legs of the driver's SCR.SLEEPDEEP read-modify-write: the Deep
 * Sleep entry runs the set leg then the post-wake clear leg, and the
 * plain Sleep entry runs the clear leg twice. The staged SLEEPONEXIT
 * sibling bit surviving both entries proves each leg is a true RMW,
 * not a blind store)
 */
static void test_enter_sleep_scr_sleepdeep_rmw(void)
{
  TEST_BEGIN("lpm enter_sleep drives SCR.SLEEPDEEP as RMW");
  ra8_sim_mmap_reset();

  /* Arm Cortex-M85 SCB->SCR, RAM-backed by the sim mmap core window.
   * Stage a sibling SCR bit the driver must never disturb. */
  volatile uint32_t* scr = ra8_lpm_scb_scr();
  *scr                   = (uint32_t)k_ra8_lpm_test_scr_sleeponexit;

  /* Deep Sleep: SLEEPDEEP is set before WFI and cleared after wake
   * (host WFI is a no-op, so the restored state is observable on
   * return). SLEEPONEXIT survives both writes. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_lpm_enter_sleep(k_ra8_sleep_mode_deep_sleep));
  TEST_ASSERT_EQ(k_ra8_lpm_test_scr_sleeponexit, *scr);

  /* Plain Sleep must scrub a stale SLEEPDEEP left by earlier code so
   * the WFI is a plain CPU sleep -- and still preserve the sibling. */
  *scr = (uint32_t)k_ra8_lpm_test_scr_sleeponexit | (uint32_t)k_ra8_lpm_scb_scr_sleepdeep;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_lpm_enter_sleep(k_ra8_sleep_mode_sleep));
  TEST_ASSERT_EQ(k_ra8_lpm_test_scr_sleeponexit, *scr);

  /* Software Standby also restores SLEEPDEEP = 0 after wake. */
  *scr = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_lpm_enter_sleep(k_ra8_sleep_mode_software_std));
  TEST_ASSERT_EQ(0, *scr);
  TEST_END("lpm enter_sleep drives SCR.SLEEPDEEP as RMW");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_enter_deep_standby_helper(void)
{
  TEST_BEGIN("lpm enter_deep_standby helper");
  ra8_sim_mmap_reset();

  TEST_ASSERT_EQ(k_ra8_ok, ra8_lpm_enter_deep_standby());
  /* LPSCR is cleared by the driver after WFI returns -- see
   * test_enter_sleep_modes for the rationale. */
  TEST_ASSERT_EQ(0, (*ra8_lpm_sysc_reg8(k_ra8_lpm_lpscr_off)));
  TEST_END("lpm enter_deep_standby helper");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_enter_sleep_bad_mode(void)
{
  TEST_BEGIN("lpm enter sleep bad mode");
  ra8_sim_mmap_reset();

  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_lpm_enter_sleep((ra8_sleep_mode_t)k_ra8_lpm_test_bad_mode_value));
  /* Bad call must NOT have written LPSCR. */
  TEST_ASSERT_EQ(0, (*ra8_lpm_sysc_reg8(k_ra8_lpm_lpscr_off)));
  TEST_END("lpm enter sleep bad mode");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_get_status_packs_four_regs(void)
{
  TEST_BEGIN("lpm get_status packs SBYCR/DPSBYCR/LPSCR/SSCR1");
  ra8_sim_mmap_reset();

  /* Hand-set the four single-byte LPM control regs. */
  *ra8_lpm_sysc_reg8(k_ra8_lpm_sbycr_off)   = k_lpm_probe_sbycr;
  *ra8_lpm_sysc_reg8(k_ra8_lpm_dpsbycr_off) = k_lpm_probe_dpsbycr;
  *ra8_lpm_sysc_reg8(k_ra8_lpm_lpscr_off)   = k_lpm_probe_lpscr;
  *ra8_lpm_sysc_reg8(k_ra8_lpm_sscr1_off)   = k_lpm_probe_lpscr;

  uint32_t status = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_lpm_get_status(&status));
  TEST_ASSERT_EQ(0x05055440UL, status);

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_lpm_get_status(nullptr));
  TEST_END("lpm get_status packs SBYCR/DPSBYCR/LPSCR/SSCR1");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_get_exit_cause_packs_wupen(void)
{
  TEST_BEGIN("lpm get_exit_cause packs WUPEN0/WUPEN1");
  ra8_sim_mmap_reset();

  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_lpm_set_wakeup_sources((uint32_t)k_ra8_lpm_test_wupen0_pattern,
                                            (uint32_t)k_ra8_lpm_test_wupen1_pattern));
  uint64_t cause = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_lpm_get_exit_cause(&cause));

  const uint64_t expected =
    ((uint64_t)k_ra8_lpm_test_wupen0_pattern) | (((uint64_t)k_ra8_lpm_test_wupen1_pattern) << 32);
  TEST_ASSERT((cause == expected));

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_lpm_get_exit_cause(nullptr));
  TEST_END("lpm get_exit_cause packs WUPEN0/WUPEN1");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_get_dpsi_state(void)
{
  TEST_BEGIN("lpm get_dpsi_state snapshots all banks");
  ra8_sim_mmap_reset();

  *ra8_lpm_sysc_reg8(k_ra8_lpm_dpsier0_off)  = k_lpm_probe_dpsier0;
  *ra8_lpm_sysc_reg8(k_ra8_lpm_dpsier1_off)  = k_lpm_probe_dpsier1;
  *ra8_lpm_sysc_reg8(k_ra8_lpm_dpsier2_off)  = 0x03U;
  *ra8_lpm_sysc_reg8(k_ra8_lpm_dpsier3_off)  = k_lpm_probe_top_bit;
  *ra8_lpm_sysc_reg8(k_ra8_lpm_dpsifr0_off)  = k_lpm_probe_dpsifr0;
  *ra8_lpm_sysc_reg8(k_ra8_lpm_dpsifr3_off)  = k_lpm_probe_dpsifr3;
  *ra8_lpm_sysc_reg8(k_ra8_lpm_dpsiegr0_off) = k_lpm_probe_dpsiegr0;
  *ra8_lpm_sysc_reg8(k_ra8_lpm_dpsiegr2_off) = 0x10U;

  uint8_t enables[4] = {0U};
  uint8_t flags[4]   = {0U};
  uint8_t edges[3]   = {0U};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_lpm_get_dpsi_state(enables, flags, edges));
  TEST_ASSERT_EQ(0x11, enables[0]);
  TEST_ASSERT_EQ(0x22, enables[1]);
  TEST_ASSERT_EQ(0x03, enables[2]);
  TEST_ASSERT_EQ(0x80, enables[3]);
  TEST_ASSERT_EQ(0xAA, flags[0]);
  TEST_ASSERT_EQ(0x55, flags[3]);
  TEST_ASSERT_EQ(0x33, edges[0]);
  TEST_ASSERT_EQ(0x10, edges[2]);

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_lpm_get_dpsi_state(nullptr, flags, edges));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_lpm_get_dpsi_state(enables, nullptr, edges));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_lpm_get_dpsi_state(enables, flags, nullptr));
  TEST_END("lpm get_dpsi_state snapshots all banks");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_deinit_resets_registers(void)
{
  TEST_BEGIN("lpm deinit resets registers");
  ra8_sim_mmap_reset();

  /* Dirty state. */
  const ra8_lpm_config_t cfg = make_default_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_lpm_init(&cfg));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_lpm_set_wakeup_sources(0xFFFFFFFFUL, 0xFFFFFFFFUL));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_lpm_arm_dpsier(k_ra8_lpm_dpsier_idx_0, 0xFFU));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_lpm_enter_sleep(k_ra8_sleep_mode_software_std));

  /* Clean it. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_lpm_deinit());
  TEST_ASSERT_EQ(0, (*ra8_lpm_icu_reg32(k_ra8_lpm_wupen0_off)));
  TEST_ASSERT_EQ(0, (*ra8_lpm_icu_reg32(k_ra8_lpm_wupen1_off)));
  TEST_ASSERT_EQ(0, (*ra8_lpm_sysc_reg8(k_ra8_lpm_lpscr_off)));
  /* SBYCR must read back its reset value (OPE = 1). */
  TEST_ASSERT_EQ(k_ra8_lpm_sbycr_reset_val, (*ra8_lpm_sysc_reg8(k_ra8_lpm_sbycr_off)));
  /* DPSBYCR must read back its reset value (DCSSMODE=01b). */
  TEST_ASSERT_EQ(k_ra8_lpm_dpsbycr_reset_val, (*ra8_lpm_sysc_reg8(k_ra8_lpm_dpsbycr_off)));
  /* SSCR1 == 0; DPSIER0 == 0. */
  TEST_ASSERT_EQ(0, (*ra8_lpm_sysc_reg8(k_ra8_lpm_sscr1_off)));
  TEST_ASSERT_EQ(0, (*ra8_lpm_sysc_reg8(k_ra8_lpm_dpsier0_off)));
  TEST_END("lpm deinit resets registers");
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
  test_init_null_cfg,
  test_init_no_keep_no_bus,
  test_set_wakeup_sources,
  test_arm_clear_wupen0,
  test_arm_clear_wupen1,
  test_arm_dpsier,
  test_clear_dpsifr,
  test_set_dpsiegr,
  test_snooze_request_sources,
  test_snooze_end_sources,
  test_ram_retention,
  test_ldo_standby,
  test_clock_stop_each,
  test_opccr_read_and_wait,
  test_prcr_unlock_relock,
  test_enter_sleep_modes,
  test_enter_sleep_scr_sleepdeep_rmw,
  test_enter_deep_standby_helper,
  test_enter_sleep_bad_mode,
  test_get_status_packs_four_regs,
  test_get_exit_cause_packs_wupen,
  test_get_dpsi_state,
  test_deinit_resets_registers,
};

int32_t main(void)
{
  for (size_t i = 0U; i < (sizeof s_test_roster / sizeof s_test_roster[0]); ++i) {
    s_test_roster[i]();
  }
  (void)fprintf(stderr, "[OK  ] test_ra8_lpm.c\n");
  return 0;
}
