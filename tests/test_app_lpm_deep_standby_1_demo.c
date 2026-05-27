/**
 * @file test_app_lpm_deep_standby_1_demo.c
 * @brief Integration test: LPM init + DPSIER2.DRTCAIE + WUPEN0.RTCALM + DSBY1
 *
 * @details
 * Mirrors examples/ek_ra8d2/lpm_deep_standby_1_demo/main.c bring-up:
 * ra_lpm_init -> ra_lpm_arm_dpsier(2, DRTCAIE) ->
 * ra_lpm_arm_wupen0_bits(RTCALM) ->
 * ra_lpm_enter_sleep(k_ra_sleep_mode_deep_standby_1). The host sim
 * mmap records each register write so the test can verify the
 * DPSIER2 / WUPEN0 / LPSCR state.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8d2_lpm_regs.h"
#include "ra_err.h"
#include "ra_lpm.h"
#include "ra_sim_mmap.h"
#include "unity_minimal.h"

static void reset_world(void)
{
  ra_sim_mmap_reset();
}

static ra_lpm_config_t make_demo_cfg(void)
{
  const ra_lpm_config_t cfg = {
    .io_port_keep     = false,
    .opa_bus_keep     = true,
    .sscr_fast_return = false,
    .dcdc_softstart   = k_ra_lpm_dcssmode_128us,
    .sscr_low_power   = k_ra_lpm_ss2lp_default,
  };
  return cfg;
}

/**
 * @par MC/DC:
 * Decision: ``ra_lpm_init != ok``. One atomic condition x 2 vectors.
 */
static void test_lpm_dpsby1_init_ok(void)
{
  reset_world();
  TEST_BEGIN("lpm_deep_standby_1_demo: init ok");
  const ra_lpm_config_t cfg = make_demo_cfg();
  TEST_ASSERT_EQ(k_ra_ok, ra_lpm_init(&cfg));
  TEST_END("lpm_deep_standby_1_demo: init ok");
}

/**
 * @par MC/DC:
 * Decision: ``cfg == nullptr``. One atomic condition x 2 vectors.
 */
static void test_lpm_dpsby1_init_null(void)
{
  reset_world();
  TEST_BEGIN("lpm_deep_standby_1_demo: NULL cfg rejected");
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_lpm_init(nullptr));
  TEST_END("lpm_deep_standby_1_demo: NULL cfg rejected");
}

/**
 * @brief Demo programmes DPSIER2.DRTCAIE before deep-standby entry.
 *
 * @par MC/DC:
 * Decision: ``ra_lpm_arm_dpsier != ok``. One atomic condition x 2
 * vectors -- ok (this) + invalid idx covered by test_ra_lpm.c.
 */
static void test_lpm_dpsby1_arm_dpsier2_drtca(void)
{
  reset_world();
  TEST_BEGIN("lpm_deep_standby_1_demo: arm DPSIER2.DRTCAIE");
  const ra_lpm_config_t cfg = make_demo_cfg();
  TEST_ASSERT_EQ(k_ra_ok, ra_lpm_init(&cfg));
  TEST_ASSERT_EQ(k_ra_ok,
                 ra_lpm_arm_dpsier(k_ra_lpm_dpsier_idx_2, (uint8_t)k_ra_lpm_dpsier2_drtcaie_mask));
  TEST_ASSERT_EQ(k_ra_lpm_dpsier2_drtcaie_mask, *ra_lpm_sysc_reg8(k_ra_lpm_dpsier2_off));
  TEST_END("lpm_deep_standby_1_demo: arm DPSIER2.DRTCAIE");
}

/**
 * @brief Deep Standby 1 entry writes LPMD=0x8 then HAL clears LPSCR.
 *
 * @par MC/DC:
 * Decision: ``ra_lpm_enter_sleep != ok``. One atomic condition x 2
 * vectors -- valid mode (this) + invalid mode covered elsewhere.
 */
static void test_lpm_dpsby1_enter_ok(void)
{
  reset_world();
  TEST_BEGIN("lpm_deep_standby_1_demo: enter Deep Standby 1 ok");
  const ra_lpm_config_t cfg = make_demo_cfg();
  TEST_ASSERT_EQ(k_ra_ok, ra_lpm_init(&cfg));
  TEST_ASSERT_EQ(k_ra_ok,
                 ra_lpm_arm_dpsier(k_ra_lpm_dpsier_idx_2, (uint8_t)k_ra_lpm_dpsier2_drtcaie_mask));
  TEST_ASSERT_EQ(k_ra_ok, ra_lpm_arm_wupen0_bits((uint32_t)k_ra_lpm_wupen0_rtcalm));
  TEST_ASSERT_EQ(k_ra_ok, ra_lpm_enter_sleep(k_ra_sleep_mode_deep_standby_1));
  /* HAL clears LPSCR after WFI returns. */
  TEST_ASSERT_EQ(0, *ra_lpm_sysc_reg8(k_ra_lpm_lpscr_off));
  TEST_END("lpm_deep_standby_1_demo: enter Deep Standby 1 ok");
}

int main(void)
{
  test_lpm_dpsby1_init_ok();
  test_lpm_dpsby1_init_null();
  test_lpm_dpsby1_arm_dpsier2_drtca();
  test_lpm_dpsby1_enter_ok();
  return 0;
}
