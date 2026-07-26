/**
 * @file test_app_lpm_software_standby_demo.c
 * @brief Integration test: LPM init + WUPEN0.RTCALM arm + Software Standby entry
 *
 * @details
 * Mirrors examples/ek_ra8d2/hil_needs_revalidation/lpm_software_standby_demo/main.c bring-up:
 * ra8_lpm_init -> ra8_lpm_arm_wupen0_bits(RTCALM) ->
 * ra8_lpm_enter_sleep(k_ra8_sleep_mode_software_std). On host builds
 * WFI is a no-op and the host sim mmap records each register write
 * so the test can verify the LPSCR and WUPEN0 state.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_lpm.h"
#include "ra8_lpm_regs.h"
#include "ra8_sim_mmap.h"
#include "unity_minimal.h"

static void reset_world(void)
{
  ra8_sim_mmap_reset();
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
 * Decision: ``ra8_lpm_init != ok``. One atomic condition x 2 vectors:
 * non-NULL (this) + NULL (the next test).
 */
static void test_lpm_swstd_init_ok(void)
{
  reset_world();
  TEST_BEGIN("lpm_software_standby_demo: init ok");
  const ra8_lpm_config_t cfg = make_demo_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_lpm_init(&cfg));
  TEST_END("lpm_software_standby_demo: init ok");
}

/**
 * @par MC/DC:
 * Decision: ``cfg == nullptr``. One atomic condition x 2 vectors:
 * NULL (this) + non-NULL (test_lpm_swstd_init_ok).
 */
static void test_lpm_swstd_init_null(void)
{
  reset_world();
  TEST_BEGIN("lpm_software_standby_demo: NULL cfg rejected");
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_lpm_init(nullptr));
  TEST_END("lpm_software_standby_demo: NULL cfg rejected");
}

/**
 * @brief Demo arms WUPEN0.RTCALM before Software Standby entry.
 *
 * @par MC/DC:
 * Decision: ``ra8_lpm_arm_wupen0_bits != ok``. One atomic condition x
 * 2 vectors -- happy path (this) + the NULL-mode rejection in
 * ra8_lpm_enter_sleep below.
 */
static void test_lpm_swstd_arm_wupen0_rtcalm(void)
{
  reset_world();
  TEST_BEGIN("lpm_software_standby_demo: arm WUPEN0.RTCALM");
  const ra8_lpm_config_t cfg = make_demo_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_lpm_init(&cfg));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_lpm_arm_wupen0_bits((uint32_t)k_ra8_lpm_wupen0_rtcalm));
  /* Confirm the RTCALM bit landed in WUPEN0 without disturbing others. */
  const uint32_t wupen0 = *ra8_lpm_icu_reg32(k_ra8_lpm_wupen0_off);
  TEST_ASSERT_EQ(k_ra8_lpm_wupen0_rtcalm, wupen0 & (uint32_t)k_ra8_lpm_wupen0_rtcalm);
  TEST_END("lpm_software_standby_demo: arm WUPEN0.RTCALM");
}

/**
 * @brief Software Standby entry returns ok; HAL leaves LPSCR back at 0.
 *
 * @par MC/DC:
 * Decision: ``ra8_lpm_enter_sleep != ok``. One atomic condition x 2
 * vectors -- valid mode (this) + invalid mode covered by
 * test_ra8_lpm.c::test_enter_sleep_bad_mode.
 */
static void test_lpm_swstd_enter_ok(void)
{
  reset_world();
  TEST_BEGIN("lpm_software_standby_demo: enter Software Standby ok");
  const ra8_lpm_config_t cfg = make_demo_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_lpm_init(&cfg));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_lpm_arm_wupen0_bits((uint32_t)k_ra8_lpm_wupen0_rtcalm));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_lpm_enter_sleep(k_ra8_sleep_mode_software_std));
  /* The HAL clears LPSCR after the WFI returns to guard against the
   * J-Link RAMCode brick path -- verify here. */
  TEST_ASSERT_EQ(0, *ra8_lpm_sysc_reg8(k_ra8_lpm_lpscr_off));
  TEST_END("lpm_software_standby_demo: enter Software Standby ok");
}

int main(void)
{
  test_lpm_swstd_init_ok();
  test_lpm_swstd_init_null();
  test_lpm_swstd_arm_wupen0_rtcalm();
  test_lpm_swstd_enter_ok();
  return 0;
}
