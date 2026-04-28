/**
 * @file test_ra_lpm.c
 * @brief Unit tests for ra_lpm.c (Low Power Mode HAL driver)
 *
 * @details
 * Drives every public API in ``ra_lpm.h`` through the host sim mmap
 * and verifies the resulting register state. Covers lifecycle,
 * PRCR unlock/relock, raw + per-source WUPEN0/1 manipulation,
 * DPSIER/DPSIFR/DPSIEGR programming, snooze request/end source
 * helpers, RAM/LDO retention, clock-shutdown matrix, OPCCR read +
 * timeout-wait, all six sleep modes, and packed-status diagnostics.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8d2_lpm_regs.h"
#include "ra_err.h"
#include "ra_lpm.h"
#include "ra_sim_mmap.h"
#include "unity_minimal.h"

/**
 * @enum ra_lpm_test_const_t
 * @brief Magic values used by the LPM unit tests.
 */
typedef enum : uint32_t {
  k_ra_lpm_test_wupen0_pattern = 0x0123ABCDUL, /**< Arbitrary WUPEN0 pattern.   */
  k_ra_lpm_test_wupen1_pattern = 0xDEADBEEFUL, /**< Arbitrary WUPEN1 pattern.   */
  k_ra_lpm_test_bad_mode_value = 0x77U,        /**< Not a valid LPMD encoding.  */
  k_ra_lpm_test_poll_budget    = 16U,          /**< OPCMTSF poll budget.        */
} ra_lpm_test_const_t;

static ra_lpm_config_t make_default_cfg(void)
{
  const ra_lpm_config_t cfg = {
    .io_port_keep     = true,
    .opa_bus_keep     = true,
    .sscr_fast_return = true,
    .dcdc_softstart   = k_ra_lpm_dcssmode_256us,
    .sscr_low_power   = k_ra_lpm_ss2lp_low,
  };
  return cfg;
}

static void test_init_happy(void)
{
  TEST_BEGIN("lpm init happy");
  ra_sim_mmap_reset();

  const ra_lpm_config_t cfg = make_default_cfg();
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_lpm_init(&cfg));

  /* SBYCR.OPE bit set; bits 5:0 and 7 cleared. */
  TEST_ASSERT_EQ((int)k_ra_lpm_sbycr_ope_mask, (int)(*ra_lpm_sysc_reg8(k_ra_lpm_sbycr_off)));

  /* DPSBYCR: IOKEEP set, DCSSMODE = 0b10 << 2. */
  const uint8_t expected_dpsbycr =
    (uint8_t)((uint8_t)k_ra_lpm_dpsbycr_iokeep_mask |
              ((uint8_t)k_ra_lpm_dcssmode_256us << k_ra_lpm_dpsbycr_dcssmode_shift));
  TEST_ASSERT_EQ((int)expected_dpsbycr, (int)(*ra_lpm_sysc_reg8(k_ra_lpm_dpsbycr_off)));

  /* SSCR1: SS2FR=1, SS2LP=01b. */
  const uint8_t expected_sscr1 =
    (uint8_t)((uint8_t)k_ra_lpm_sscr1_ss2fr_mask |
              ((uint8_t)k_ra_lpm_ss2lp_low << k_ra_lpm_sscr1_ss2lp_shift));
  TEST_ASSERT_EQ((int)expected_sscr1, (int)(*ra_lpm_sysc_reg8(k_ra_lpm_sscr1_off)));

  /* LPSCR cleared on init -- next WFI is plain CPU sleep. */
  TEST_ASSERT_EQ((int)0, (int)(*ra_lpm_sysc_reg8(k_ra_lpm_lpscr_off)));

  TEST_END("lpm init happy");
}

static void test_init_null_cfg(void)
{
  TEST_BEGIN("lpm init null cfg");
  ra_sim_mmap_reset();

  TEST_ASSERT_EQ((int)k_ra_err_null_ptr, (int)ra_lpm_init(NULL));
  TEST_END("lpm init null cfg");
}

static void test_init_no_keep_no_bus(void)
{
  TEST_BEGIN("lpm init no keep no bus");
  ra_sim_mmap_reset();

  ra_lpm_config_t cfg  = make_default_cfg();
  cfg.io_port_keep     = false;
  cfg.opa_bus_keep     = false;
  cfg.sscr_fast_return = false;
  cfg.dcdc_softstart   = k_ra_lpm_dcssmode_128us;
  cfg.sscr_low_power   = k_ra_lpm_ss2lp_default;
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_lpm_init(&cfg));

  /* SBYCR has OPE cleared. */
  TEST_ASSERT_EQ((int)0, (int)(*ra_lpm_sysc_reg8(k_ra_lpm_sbycr_off)));
  /* DPSBYCR: IOKEEP cleared, DCSSMODE = 0b01 << 2 = 0x04. */
  const uint8_t expected =
    (uint8_t)((uint8_t)k_ra_lpm_dcssmode_128us << k_ra_lpm_dpsbycr_dcssmode_shift);
  TEST_ASSERT_EQ((int)expected, (int)(*ra_lpm_sysc_reg8(k_ra_lpm_dpsbycr_off)));
  /* SSCR1: all bits clear. */
  TEST_ASSERT_EQ((int)0, (int)(*ra_lpm_sysc_reg8(k_ra_lpm_sscr1_off)));
  TEST_END("lpm init no keep no bus");
}

static void test_set_wakeup_sources(void)
{
  TEST_BEGIN("lpm set_wakeup_sources");
  ra_sim_mmap_reset();

  TEST_ASSERT_EQ((int)k_ra_ok,
                 (int)ra_lpm_set_wakeup_sources((uint32_t)k_ra_lpm_test_wupen0_pattern,
                                                (uint32_t)k_ra_lpm_test_wupen1_pattern));

  TEST_ASSERT_EQ((int64_t)k_ra_lpm_test_wupen0_pattern,
                 (int64_t)(*ra_lpm_icu_reg32(k_ra_lpm_wupen0_off)));
  TEST_ASSERT_EQ((int64_t)k_ra_lpm_test_wupen1_pattern,
                 (int64_t)(*ra_lpm_icu_reg32(k_ra_lpm_wupen1_off)));
  TEST_END("lpm set_wakeup_sources");
}

static void test_arm_clear_wupen0(void)
{
  TEST_BEGIN("lpm arm/clear wupen0");
  ra_sim_mmap_reset();

  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_lpm_arm_wupen0_bits((uint32_t)k_ra_lpm_wupen0_irq3));
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_lpm_arm_wupen0_bits((uint32_t)k_ra_lpm_wupen0_rtcalm));
  TEST_ASSERT_EQ((int64_t)((uint32_t)k_ra_lpm_wupen0_irq3 | (uint32_t)k_ra_lpm_wupen0_rtcalm),
                 (int64_t)(*ra_lpm_icu_reg32(k_ra_lpm_wupen0_off)));

  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_lpm_clear_wupen0_bits((uint32_t)k_ra_lpm_wupen0_irq3));
  TEST_ASSERT_EQ((int64_t)k_ra_lpm_wupen0_rtcalm,
                 (int64_t)(*ra_lpm_icu_reg32(k_ra_lpm_wupen0_off)));
  TEST_END("lpm arm/clear wupen0");
}

static void test_arm_clear_wupen1(void)
{
  TEST_BEGIN("lpm arm/clear wupen1");
  ra_sim_mmap_reset();

  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_lpm_arm_wupen1_bits((uint32_t)k_ra_lpm_wupen1_ulpt0u));
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_lpm_arm_wupen1_bits((uint32_t)k_ra_lpm_wupen1_i3c0));
  TEST_ASSERT_EQ((int64_t)((uint32_t)k_ra_lpm_wupen1_ulpt0u | (uint32_t)k_ra_lpm_wupen1_i3c0),
                 (int64_t)(*ra_lpm_icu_reg32(k_ra_lpm_wupen1_off)));

  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_lpm_clear_wupen1_bits((uint32_t)k_ra_lpm_wupen1_i3c0));
  TEST_ASSERT_EQ((int64_t)k_ra_lpm_wupen1_ulpt0u,
                 (int64_t)(*ra_lpm_icu_reg32(k_ra_lpm_wupen1_off)));
  TEST_END("lpm arm/clear wupen1");
}

static void test_arm_dpsier(void)
{
  TEST_BEGIN("lpm arm_dpsier all four banks");
  ra_sim_mmap_reset();

  /* DPSIER0 = IRQ0..IRQ7 mask. */
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_lpm_arm_dpsier(k_ra_lpm_dpsier_idx_0, 0xFFU));
  TEST_ASSERT_EQ((int)0xFF, (int)*ra_lpm_sysc_reg8(k_ra_lpm_dpsier0_off));
  /* DPSIFR0 should be cleared by the driver. */
  TEST_ASSERT_EQ((int)0, (int)*ra_lpm_sysc_reg8(k_ra_lpm_dpsifr0_off));

  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_lpm_arm_dpsier(k_ra_lpm_dpsier_idx_1, 0xAAU));
  TEST_ASSERT_EQ((int)0xAA, (int)*ra_lpm_sysc_reg8(k_ra_lpm_dpsier1_off));

  TEST_ASSERT_EQ(
    (int)k_ra_ok,
    (int)ra_lpm_arm_dpsier(k_ra_lpm_dpsier_idx_2, (uint8_t)k_ra_lpm_dpsier2_drtcaie_mask));
  TEST_ASSERT_EQ((int)k_ra_lpm_dpsier2_drtcaie_mask, (int)*ra_lpm_sysc_reg8(k_ra_lpm_dpsier2_off));

  TEST_ASSERT_EQ(
    (int)k_ra_ok,
    (int)ra_lpm_arm_dpsier(k_ra_lpm_dpsier_idx_3, (uint8_t)k_ra_lpm_dpsier3_diwdtie_mask));
  TEST_ASSERT_EQ((int)k_ra_lpm_dpsier3_diwdtie_mask, (int)*ra_lpm_sysc_reg8(k_ra_lpm_dpsier3_off));

  /* Bad index. */
  TEST_ASSERT_EQ((int)k_ra_err_invalid_arg, (int)ra_lpm_arm_dpsier((ra_lpm_dpsier_idx_t)5U, 0x01U));
  TEST_END("lpm arm_dpsier all four banks");
}

static void test_clear_dpsifr(void)
{
  TEST_BEGIN("lpm clear_dpsifr");
  ra_sim_mmap_reset();

  /* Stamp some 'pending' flags then clear them. */
  *ra_lpm_sysc_reg8(k_ra_lpm_dpsifr0_off) = 0xFFU;
  *ra_lpm_sysc_reg8(k_ra_lpm_dpsifr3_off) = 0x80U;

  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_lpm_clear_dpsifr(k_ra_lpm_dpsier_idx_0));
  TEST_ASSERT_EQ((int)0, (int)*ra_lpm_sysc_reg8(k_ra_lpm_dpsifr0_off));

  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_lpm_clear_dpsifr(k_ra_lpm_dpsier_idx_3));
  TEST_ASSERT_EQ((int)0, (int)*ra_lpm_sysc_reg8(k_ra_lpm_dpsifr3_off));

  TEST_ASSERT_EQ((int)k_ra_err_invalid_arg, (int)ra_lpm_clear_dpsifr((ra_lpm_dpsier_idx_t)9U));
  TEST_END("lpm clear_dpsifr");
}

static void test_set_dpsiegr(void)
{
  TEST_BEGIN("lpm set_dpsiegr");
  ra_sim_mmap_reset();

  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_lpm_set_dpsiegr(k_ra_lpm_dpsier_idx_0, 0x55U));
  TEST_ASSERT_EQ((int)0x55, (int)*ra_lpm_sysc_reg8(k_ra_lpm_dpsiegr0_off));

  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_lpm_set_dpsiegr(k_ra_lpm_dpsier_idx_1, 0xAAU));
  TEST_ASSERT_EQ((int)0xAA, (int)*ra_lpm_sysc_reg8(k_ra_lpm_dpsiegr1_off));

  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_lpm_set_dpsiegr(k_ra_lpm_dpsier_idx_2, 0x10U));
  TEST_ASSERT_EQ((int)0x10, (int)*ra_lpm_sysc_reg8(k_ra_lpm_dpsiegr2_off));

  TEST_ASSERT_EQ((int)k_ra_err_invalid_arg,
                 (int)ra_lpm_set_dpsiegr((ra_lpm_dpsier_idx_t)4U, 0xFFU));
  TEST_END("lpm set_dpsiegr");
}

static void test_snooze_request_sources(void)
{
  TEST_BEGIN("lpm snooze_set_request_sources");
  ra_sim_mmap_reset();

  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_lpm_snooze_set_request_sources(true, true, true));

  const uint32_t want_w1 = (uint32_t)k_ra_lpm_wupen1_ulpt0u | (uint32_t)k_ra_lpm_wupen1_ulpt1u;
  TEST_ASSERT_EQ((int64_t)want_w1, (int64_t)(*ra_lpm_icu_reg32(k_ra_lpm_wupen1_off)));
  TEST_ASSERT_EQ((int64_t)k_ra_lpm_wupen0_acmphs0,
                 (int64_t)(*ra_lpm_icu_reg32(k_ra_lpm_wupen0_off)));
  TEST_END("lpm snooze_set_request_sources");
}

static void test_snooze_end_sources(void)
{
  TEST_BEGIN("lpm snooze_set_end_sources");
  ra_sim_mmap_reset();

  /* Pre-stamp DPSIFR3 to verify the dummy-read-then-clear sequence. */
  *ra_lpm_sysc_reg8(k_ra_lpm_dpsifr3_off) = 0xFFU;

  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_lpm_snooze_set_end_sources(true, false, true, false));

  const uint8_t want =
    (uint8_t)((uint8_t)k_ra_lpm_dpsier3_dulpt0ie_mask | (uint8_t)k_ra_lpm_dpsier3_dusbfsie_mask);
  TEST_ASSERT_EQ((int)want, (int)*ra_lpm_sysc_reg8(k_ra_lpm_dpsier3_off));
  TEST_ASSERT_EQ((int)0, (int)*ra_lpm_sysc_reg8(k_ra_lpm_dpsifr3_off));
  TEST_END("lpm snooze_set_end_sources");
}

static void test_ram_retention(void)
{
  TEST_BEGIN("lpm set_ram_retention");
  ra_sim_mmap_reset();

  ra_lpm_ram_retention_t cfg = {
    .pdramscr0_bits = (uint16_t)k_ra_lpm_pdramscr0_all_keep,
    .cpu0_tcm_keep  = true,
    .cpu1_tcm_keep  = false,
  };
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_lpm_set_ram_retention(&cfg));
  TEST_ASSERT_EQ((int64_t)k_ra_lpm_pdramscr0_all_keep,
                 (int64_t)*ra_lpm_sysc_reg16(k_ra_lpm_pdramscr0_off));
  TEST_ASSERT_EQ((int)k_ra_lpm_pdramscr1_rkeep0_mask,
                 (int)*ra_lpm_sysc_reg8(k_ra_lpm_pdramscr1_off));

  cfg.cpu0_tcm_keep  = false;
  cfg.cpu1_tcm_keep  = true;
  cfg.pdramscr0_bits = (uint16_t)k_ra_lpm_pdramscr0_sram1_keep;
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_lpm_set_ram_retention(&cfg));
  TEST_ASSERT_EQ((int64_t)k_ra_lpm_pdramscr0_sram1_keep,
                 (int64_t)*ra_lpm_sysc_reg16(k_ra_lpm_pdramscr0_off));
  TEST_ASSERT_EQ((int)k_ra_lpm_pdramscr1_rkeep1_mask,
                 (int)*ra_lpm_sysc_reg8(k_ra_lpm_pdramscr1_off));

  TEST_ASSERT_EQ((int)k_ra_err_null_ptr, (int)ra_lpm_set_ram_retention(NULL));
  TEST_END("lpm set_ram_retention");
}

static void test_ldo_standby(void)
{
  TEST_BEGIN("lpm set_ldo_standby");
  ra_sim_mmap_reset();

  /* OPCCR.OPCM = 0 (HSM) so writes are permitted. */
  *ra_lpm_sysc_reg8(k_ra_lpm_opccr_off) = 0U;

  ra_lpm_ldo_cfg_t cfg = {
    .pll1 = k_ra_lpm_ldo_retained,
    .pll2 = k_ra_lpm_ldo_disabled,
    .hoco = k_ra_lpm_ldo_retained,
  };
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_lpm_set_ldo_standby(&cfg));
  TEST_ASSERT_EQ(
    (int)k_ra_lpm_ldocr_skeep_mask,
    (int)((*ra_lpm_sysc_reg8(k_ra_lpm_pll1ldocr_off)) & (uint8_t)k_ra_lpm_ldocr_skeep_mask));
  TEST_ASSERT_EQ(
    (int)0,
    (int)((*ra_lpm_sysc_reg8(k_ra_lpm_pll2ldocr_off)) & (uint8_t)k_ra_lpm_ldocr_skeep_mask));
  TEST_ASSERT_EQ(
    (int)k_ra_lpm_ldocr_skeep_mask,
    (int)((*ra_lpm_sysc_reg8(k_ra_lpm_hocoldocr_off)) & (uint8_t)k_ra_lpm_ldocr_skeep_mask));

  /* Force OPCM != 0 -- write must be rejected with invalid_state. */
  *ra_lpm_sysc_reg8(k_ra_lpm_opccr_off) = 0x01U;
  TEST_ASSERT_EQ((int)k_ra_err_invalid_state, (int)ra_lpm_set_ldo_standby(&cfg));

  TEST_ASSERT_EQ((int)k_ra_err_null_ptr, (int)ra_lpm_set_ldo_standby(NULL));
  TEST_END("lpm set_ldo_standby");
}

static void test_clock_stop_each(void)
{
  TEST_BEGIN("lpm set_clock_stop / get_clock_stop");
  ra_sim_mmap_reset();

  bool                 v        = false;
  const ra_lpm_clock_t clocks[] = {
    k_ra_lpm_clock_moco,
    k_ra_lpm_clock_hoco,
    k_ra_lpm_clock_loco,
    k_ra_lpm_clock_main,
    k_ra_lpm_clock_sub,
  };
  for (uint8_t i = 0U; i < (uint8_t)(sizeof(clocks) / sizeof(clocks[0])); ++i) {
    TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_lpm_set_clock_stop(clocks[i], true));
    TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_lpm_get_clock_stop(clocks[i], &v));
    TEST_ASSERT(v);
    TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_lpm_set_clock_stop(clocks[i], false));
    TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_lpm_get_clock_stop(clocks[i], &v));
    TEST_ASSERT(!v);
  }

  TEST_ASSERT_EQ((int)k_ra_err_invalid_arg, (int)ra_lpm_set_clock_stop(k_ra_lpm_clock_count, true));
  TEST_ASSERT_EQ((int)k_ra_err_invalid_arg, (int)ra_lpm_get_clock_stop(k_ra_lpm_clock_count, &v));
  TEST_ASSERT_EQ((int)k_ra_err_null_ptr, (int)ra_lpm_get_clock_stop(k_ra_lpm_clock_moco, NULL));
  TEST_END("lpm set_clock_stop / get_clock_stop");
}

static void test_opccr_read_and_wait(void)
{
  TEST_BEGIN("lpm get_opccr + wait_for_opccr");
  ra_sim_mmap_reset();

  /* Hand-set OPCCR to "transition complete" (OPCMTSF=0). */
  *ra_lpm_sysc_reg8(k_ra_lpm_opccr_off) = 0x00U;
  uint8_t v                             = 0xFFU;
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_lpm_get_opccr(&v));
  TEST_ASSERT_EQ((int)0x00, (int)v);
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_lpm_wait_for_opccr((uint32_t)k_ra_lpm_test_poll_budget));

  /* Stick OPCMTSF=1 and observe the timeout. */
  *ra_lpm_sysc_reg8(k_ra_lpm_opccr_off) = (uint8_t)k_ra_lpm_opccr_opcmtsf_msk;
  TEST_ASSERT_EQ((int)k_ra_err_hw_timeout,
                 (int)ra_lpm_wait_for_opccr((uint32_t)k_ra_lpm_test_poll_budget));

  TEST_ASSERT_EQ((int)k_ra_err_invalid_arg, (int)ra_lpm_wait_for_opccr(0U));
  TEST_ASSERT_EQ((int)k_ra_err_null_ptr, (int)ra_lpm_get_opccr(NULL));
  TEST_END("lpm get_opccr + wait_for_opccr");
}

static void test_prcr_unlock_relock(void)
{
  TEST_BEGIN("lpm prcr unlock/relock");
  ra_sim_mmap_reset();

  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_lpm_prcr_unlock());
  /* Upper byte must be the 0xA5 key; lower byte must include PRC1=1. */
  const uint16_t prcr_after_unlock = *ra_lpm_sysc_reg16(k_ra_lpm_prcr_off);
  TEST_ASSERT_EQ((int)((uint16_t)k_ra_lpm_prcr_key), (int)(prcr_after_unlock & (uint16_t)0xFF00U));
  TEST_ASSERT((prcr_after_unlock & (uint16_t)k_ra_lpm_prcr_prc1_msk) != 0U);

  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_lpm_prcr_relock());
  const uint16_t prcr_after_relock = *ra_lpm_sysc_reg16(k_ra_lpm_prcr_off);
  TEST_ASSERT_EQ((int)((uint16_t)k_ra_lpm_prcr_key), (int)(prcr_after_relock & (uint16_t)0xFF00U));
  TEST_ASSERT((prcr_after_relock & (uint16_t)k_ra_lpm_prcr_prc1_msk) == 0U);
  TEST_END("lpm prcr unlock/relock");
}

static void test_enter_sleep_modes(void)
{
  TEST_BEGIN("lpm enter sleep modes");
  ra_sim_mmap_reset();

  /* Plain sleep -- LPSCR.LPMD = 0. */
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_lpm_enter_sleep(k_ra_sleep_mode_sleep));
  TEST_ASSERT_EQ((int)0, (int)(*ra_lpm_sysc_reg8(k_ra_lpm_lpscr_off)));

  /* Deep Sleep -- LPSCR.LPMD = 0 (SCR.SLEEPDEEP toggled by driver). */
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_lpm_enter_sleep(k_ra_sleep_mode_deep_sleep));
  TEST_ASSERT_EQ((int)0, (int)(*ra_lpm_sysc_reg8(k_ra_lpm_lpscr_off)));

  /* Software standby -- LPSCR.LPMD = 0x5. */
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_lpm_enter_sleep(k_ra_sleep_mode_software_std));
  TEST_ASSERT_EQ((int)k_ra_sleep_mode_software_std, (int)(*ra_lpm_sysc_reg8(k_ra_lpm_lpscr_off)));

  /* Deep standby 1 / 2 / 3. */
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_lpm_enter_sleep(k_ra_sleep_mode_deep_standby_1));
  TEST_ASSERT_EQ((int)k_ra_sleep_mode_deep_standby_1, (int)(*ra_lpm_sysc_reg8(k_ra_lpm_lpscr_off)));
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_lpm_enter_sleep(k_ra_sleep_mode_deep_standby_2));
  TEST_ASSERT_EQ((int)k_ra_sleep_mode_deep_standby_2, (int)(*ra_lpm_sysc_reg8(k_ra_lpm_lpscr_off)));
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_lpm_enter_sleep(k_ra_sleep_mode_deep_standby_3));
  TEST_ASSERT_EQ((int)k_ra_sleep_mode_deep_standby_3, (int)(*ra_lpm_sysc_reg8(k_ra_lpm_lpscr_off)));
  TEST_END("lpm enter sleep modes");
}

static void test_enter_deep_standby_helper(void)
{
  TEST_BEGIN("lpm enter_deep_standby helper");
  ra_sim_mmap_reset();

  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_lpm_enter_deep_standby());
  TEST_ASSERT_EQ((int)k_ra_sleep_mode_deep_standby_1, (int)(*ra_lpm_sysc_reg8(k_ra_lpm_lpscr_off)));
  TEST_END("lpm enter_deep_standby helper");
}

static void test_enter_sleep_bad_mode(void)
{
  TEST_BEGIN("lpm enter sleep bad mode");
  ra_sim_mmap_reset();

  TEST_ASSERT_EQ((int)k_ra_err_invalid_arg,
                 (int)ra_lpm_enter_sleep((ra_sleep_mode_t)k_ra_lpm_test_bad_mode_value));
  /* Bad call must NOT have written LPSCR. */
  TEST_ASSERT_EQ((int)0, (int)(*ra_lpm_sysc_reg8(k_ra_lpm_lpscr_off)));
  TEST_END("lpm enter sleep bad mode");
}

static void test_get_status_packs_four_regs(void)
{
  TEST_BEGIN("lpm get_status packs SBYCR/DPSBYCR/LPSCR/SSCR1");
  ra_sim_mmap_reset();

  /* Hand-set the four single-byte LPM control regs. */
  *ra_lpm_sysc_reg8(k_ra_lpm_sbycr_off)   = 0x40U;
  *ra_lpm_sysc_reg8(k_ra_lpm_dpsbycr_off) = 0x54U;
  *ra_lpm_sysc_reg8(k_ra_lpm_lpscr_off)   = 0x05U;
  *ra_lpm_sysc_reg8(k_ra_lpm_sscr1_off)   = 0x05U;

  uint32_t status = 0U;
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_lpm_get_status(&status));
  TEST_ASSERT_EQ((int64_t)0x05055440UL, (int64_t)status);

  TEST_ASSERT_EQ((int)k_ra_err_null_ptr, (int)ra_lpm_get_status(NULL));
  TEST_END("lpm get_status packs SBYCR/DPSBYCR/LPSCR/SSCR1");
}

static void test_get_exit_cause_packs_wupen(void)
{
  TEST_BEGIN("lpm get_exit_cause packs WUPEN0/WUPEN1");
  ra_sim_mmap_reset();

  TEST_ASSERT_EQ((int)k_ra_ok,
                 (int)ra_lpm_set_wakeup_sources((uint32_t)k_ra_lpm_test_wupen0_pattern,
                                                (uint32_t)k_ra_lpm_test_wupen1_pattern));
  uint64_t cause = 0U;
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_lpm_get_exit_cause(&cause));

  const uint64_t expected =
    ((uint64_t)k_ra_lpm_test_wupen0_pattern) | (((uint64_t)k_ra_lpm_test_wupen1_pattern) << 32);
  TEST_ASSERT((cause == expected));

  TEST_ASSERT_EQ((int)k_ra_err_null_ptr, (int)ra_lpm_get_exit_cause(NULL));
  TEST_END("lpm get_exit_cause packs WUPEN0/WUPEN1");
}

static void test_get_dpsi_state(void)
{
  TEST_BEGIN("lpm get_dpsi_state snapshots all banks");
  ra_sim_mmap_reset();

  *ra_lpm_sysc_reg8(k_ra_lpm_dpsier0_off)  = 0x11U;
  *ra_lpm_sysc_reg8(k_ra_lpm_dpsier1_off)  = 0x22U;
  *ra_lpm_sysc_reg8(k_ra_lpm_dpsier2_off)  = 0x03U;
  *ra_lpm_sysc_reg8(k_ra_lpm_dpsier3_off)  = 0x80U;
  *ra_lpm_sysc_reg8(k_ra_lpm_dpsifr0_off)  = 0xAAU;
  *ra_lpm_sysc_reg8(k_ra_lpm_dpsifr3_off)  = 0x55U;
  *ra_lpm_sysc_reg8(k_ra_lpm_dpsiegr0_off) = 0x33U;
  *ra_lpm_sysc_reg8(k_ra_lpm_dpsiegr2_off) = 0x10U;

  uint8_t enables[4] = {0U};
  uint8_t flags[4]   = {0U};
  uint8_t edges[3]   = {0U};
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_lpm_get_dpsi_state(enables, flags, edges));
  TEST_ASSERT_EQ((int)0x11, (int)enables[0]);
  TEST_ASSERT_EQ((int)0x22, (int)enables[1]);
  TEST_ASSERT_EQ((int)0x03, (int)enables[2]);
  TEST_ASSERT_EQ((int)0x80, (int)enables[3]);
  TEST_ASSERT_EQ((int)0xAA, (int)flags[0]);
  TEST_ASSERT_EQ((int)0x55, (int)flags[3]);
  TEST_ASSERT_EQ((int)0x33, (int)edges[0]);
  TEST_ASSERT_EQ((int)0x10, (int)edges[2]);

  TEST_ASSERT_EQ((int)k_ra_err_null_ptr, (int)ra_lpm_get_dpsi_state(NULL, flags, edges));
  TEST_ASSERT_EQ((int)k_ra_err_null_ptr, (int)ra_lpm_get_dpsi_state(enables, NULL, edges));
  TEST_ASSERT_EQ((int)k_ra_err_null_ptr, (int)ra_lpm_get_dpsi_state(enables, flags, NULL));
  TEST_END("lpm get_dpsi_state snapshots all banks");
}

static void test_deinit_resets_registers(void)
{
  TEST_BEGIN("lpm deinit resets registers");
  ra_sim_mmap_reset();

  /* Dirty state. */
  const ra_lpm_config_t cfg = make_default_cfg();
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_lpm_init(&cfg));
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_lpm_set_wakeup_sources(0xFFFFFFFFUL, 0xFFFFFFFFUL));
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_lpm_arm_dpsier(k_ra_lpm_dpsier_idx_0, 0xFFU));
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_lpm_enter_sleep(k_ra_sleep_mode_software_std));

  /* Clean it. */
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_lpm_deinit());
  TEST_ASSERT_EQ((int)0, (int)(*ra_lpm_icu_reg32(k_ra_lpm_wupen0_off)));
  TEST_ASSERT_EQ((int)0, (int)(*ra_lpm_icu_reg32(k_ra_lpm_wupen1_off)));
  TEST_ASSERT_EQ((int)0, (int)(*ra_lpm_sysc_reg8(k_ra_lpm_lpscr_off)));
  /* SBYCR must read back its reset value (OPE = 1). */
  TEST_ASSERT_EQ((int)k_ra_lpm_sbycr_reset_val, (int)(*ra_lpm_sysc_reg8(k_ra_lpm_sbycr_off)));
  /* DPSBYCR must read back its reset value (DCSSMODE=01b). */
  TEST_ASSERT_EQ((int)k_ra_lpm_dpsbycr_reset_val, (int)(*ra_lpm_sysc_reg8(k_ra_lpm_dpsbycr_off)));
  /* SSCR1 == 0; DPSIER0 == 0. */
  TEST_ASSERT_EQ((int)0, (int)(*ra_lpm_sysc_reg8(k_ra_lpm_sscr1_off)));
  TEST_ASSERT_EQ((int)0, (int)(*ra_lpm_sysc_reg8(k_ra_lpm_dpsier0_off)));
  TEST_END("lpm deinit resets registers");
}

int32_t main(void)
{
  test_init_happy();
  test_init_null_cfg();
  test_init_no_keep_no_bus();
  test_set_wakeup_sources();
  test_arm_clear_wupen0();
  test_arm_clear_wupen1();
  test_arm_dpsier();
  test_clear_dpsifr();
  test_set_dpsiegr();
  test_snooze_request_sources();
  test_snooze_end_sources();
  test_ram_retention();
  test_ldo_standby();
  test_clock_stop_each();
  test_opccr_read_and_wait();
  test_prcr_unlock_relock();
  test_enter_sleep_modes();
  test_enter_deep_standby_helper();
  test_enter_sleep_bad_mode();
  test_get_status_packs_four_regs();
  test_get_exit_cause_packs_wupen();
  test_get_dpsi_state();
  test_deinit_resets_registers();
  (void)fprintf(stderr, "[OK  ] test_ra_lpm.c\n");
  return 0;
}
