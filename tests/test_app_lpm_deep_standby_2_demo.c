/**
 * @file test_app_lpm_deep_standby_2_demo.c
 * @brief Integration test: LPM init + DPSIER2.DRTCAIE + WUPEN0.RTCALM + DSBY1
 *
 * @details
 * Mirrors examples/ek_ra8d2/hil_needs_revalidation/lpm_deep_standby_2_demo/main.c bring-up:
 * ra8_lpm_init -> ra8_lpm_arm_dpsier(2, DRTCAIE) ->
 * ra8_lpm_arm_wupen0_bits(RTCALM) ->
 * ra8_lpm_enter_sleep(k_ra8_sleep_mode_deep_standby_2). The host fake
 * mmap records each register write so the test can verify the
 * DPSIER2 / WUPEN0 / LPSCR state.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_lpm.h"
#include "ra8_lpm_regs.h"
#include "unity_minimal.h"

static void reset_world(void)
{
  ra8_fake_mmap_reset();
}

static ra8_lpm_config_t make_demo_cfg(void)
{
  const ra8_lpm_config_t cfg = {
    .io_port_keep     = false,
    .opa_bus_keep     = true,
    .sscr_fast_return = false,
    .dcdc_softstart   = k_ra8_lpm_dcssmode_128us,
    .sscr_low_power   = k_ra8_lpm_ss2lp_default,
  };
  return cfg;
}

/**
 * @par MC/DC:
 * Decision: ``ra8_lpm_init != ok``. One atomic condition x 2 vectors.
 */
static void test_lpm_dpsby2_init_ok(void)
{
  reset_world();
  TEST_BEGIN("lpm_deep_standby_2_demo: init ok");
  const ra8_lpm_config_t cfg = make_demo_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_lpm_init(&cfg));
  TEST_END("lpm_deep_standby_2_demo: init ok");
}

/**
 * @par MC/DC:
 * Decision: ``cfg == nullptr``. One atomic condition x 2 vectors.
 */
static void test_lpm_dpsby2_init_null(void)
{
  reset_world();
  TEST_BEGIN("lpm_deep_standby_2_demo: NULL cfg rejected");
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_lpm_init(nullptr));
  TEST_END("lpm_deep_standby_2_demo: NULL cfg rejected");
}

/**
 * @brief Demo programmes DPSIER2.DRTCAIE before deep-standby entry.
 *
 * @par MC/DC:
 * Decision: ``ra8_lpm_arm_dpsier != ok``. One atomic condition x 2
 * vectors -- ok (this) + invalid idx covered by test_ra8_lpm.c.
 */
static void test_lpm_dpsby2_arm_dpsier2_drtca(void)
{
  reset_world();
  TEST_BEGIN("lpm_deep_standby_2_demo: arm DPSIER2.DRTCAIE");
  const ra8_lpm_config_t cfg = make_demo_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_lpm_init(&cfg));
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_lpm_arm_dpsier(k_ra8_lpm_dpsier_idx_2, (uint8_t)k_ra8_lpm_dpsier2_drtcaie_mask));
  TEST_ASSERT_EQ(k_ra8_lpm_dpsier2_drtcaie_mask, *ra8_lpm_sysc_reg8(k_ra8_lpm_dpsier2_off));
  TEST_END("lpm_deep_standby_2_demo: arm DPSIER2.DRTCAIE");
}

/**
 * @brief Deep Standby 2 entry writes LPMD=0x9 then HAL clears LPSCR.
 *
 * @par MC/DC:
 * Decision: ``ra8_lpm_enter_sleep != ok``. One atomic condition x 2
 * vectors -- valid mode (this) + invalid mode covered elsewhere.
 */
static void test_lpm_dpsby2_enter_ok(void)
{
  reset_world();
  TEST_BEGIN("lpm_deep_standby_2_demo: enter Deep Standby 2 ok");
  const ra8_lpm_config_t cfg = make_demo_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_lpm_init(&cfg));
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_lpm_arm_dpsier(k_ra8_lpm_dpsier_idx_2, (uint8_t)k_ra8_lpm_dpsier2_drtcaie_mask));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_lpm_arm_wupen0_bits((uint32_t)k_ra8_lpm_wupen0_rtcalm));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_lpm_enter_sleep(k_ra8_sleep_mode_deep_standby_2));
  /* HAL clears LPSCR after WFI returns. */
  TEST_ASSERT_EQ(0, *ra8_lpm_sysc_reg8(k_ra8_lpm_lpscr_off));
  TEST_END("lpm_deep_standby_2_demo: enter Deep Standby 2 ok");
}

int main(void)
{
  test_lpm_dpsby2_init_ok();
  test_lpm_dpsby2_init_null();
  test_lpm_dpsby2_arm_dpsier2_drtca();
  test_lpm_dpsby2_enter_ok();
  return 0;
}
