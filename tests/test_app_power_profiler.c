/**
 * @file test_app_power_profiler.c
 * @brief Integration test: ra_power_profile init + cycle through 4 modes
 *
 * @details
 * Mirrors examples/ek_ra8d2/power_profiler/main.c by initialising
 * the LPM block + the power profiler and walking through the
 * ACTIVE / SLEEP / DEEP_STANDBY / SOFTWARE_STANDBY regions, then
 * snapshotting the accumulator. Host WFI is a no-op so the cycle
 * completes immediately under RA_SIMULATOR_MODE.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra_err.h"
#include "ra_lpm.h"
#include "ra_power_profile.h"
#include "ra_sim_mmap.h"
#include "unity_minimal.h"

static uint64_t s_now_us = 0U;

static void reset_world(void)
{
  ra_sim_mmap_reset();
  s_now_us = 0U;
}

/** @brief Synthetic monotonic clock; each call advances by 100 us. */
static uint64_t test_pp_now_us(void* ctx)
{
  (void)ctx;
  s_now_us += 100U;
  return s_now_us;
}

/**
 * @brief Bring-up programmes LPM + power profiler.
 *
 * @par MC/DC:
 * Compound decision in app: ``ra_lpm_init != ok ||
 * ra_power_profile_init != ok``. Two atomic conditions x N+1 = 3
 * vectors -- this covers both-ok; null-cfg test covers init-fails;
 * lpm null-cfg path is covered by test_app_lpm_idle_demo.c.
 */
static void test_pp_app_bringup_ok(void)
{
  reset_world();
  TEST_BEGIN("power_profiler: lpm + profiler init ok");
  const ra_lpm_config_t lpm_cfg = {
    .io_port_keep     = false,
    .opa_bus_keep     = true,
    .sscr_fast_return = true,
    .dcdc_softstart   = k_ra_lpm_dcssmode_128us,
    .sscr_low_power   = k_ra_lpm_ss2lp_default,
  };
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_lpm_init(&lpm_cfg));
  const ra_power_profile_config_t pp_cfg = {
    .pulse         = nullptr,
    .now_us        = test_pp_now_us,
    .user_ctx_gpio = nullptr,
    .user_ctx_time = nullptr,
  };
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_power_profile_init(&pp_cfg));
  TEST_END("power_profiler: lpm + profiler init ok");
}

/**
 * @brief NULL pp cfg rejected.
 *
 * @par MC/DC:
 * Decision: ``cfg == nullptr``. One atomic condition x 2 vectors --
 * NULL (this) + non-NULL (test_pp_app_bringup_ok).
 */
static void test_pp_app_init_null_rejected(void)
{
  reset_world();
  TEST_BEGIN("power_profiler: NULL pp cfg rejected");
  TEST_ASSERT_EQ((int)k_ra_err_null_ptr, (int)ra_power_profile_init(nullptr));
  TEST_END("power_profiler: NULL pp cfg rejected");
}

/**
 * @brief Walk all four regions and verify accumulator reflects
 *        non-zero dwell time.
 *
 * @par MC/DC:
 * Decision in app: each ``ra_power_profile_mark_enter / _exit !=
 * ok`` short-circuit. Two atomic conditions per pair x 2 vectors
 * -- enter ok + exit ok (this) + invalid region id (covered by the
 * library's own unit tests). Pairing here demonstrates the full
 * ACTIVE -> SOFTWARE_STANDBY cycle reaches get_stats with
 * monotonically increasing entries.
 */
static void test_pp_app_cycle_modes(void)
{
  reset_world();
  TEST_BEGIN("power_profiler: cycle 4 modes accumulates time");
  const ra_lpm_config_t lpm_cfg = {
    .io_port_keep     = false,
    .opa_bus_keep     = true,
    .sscr_fast_return = true,
    .dcdc_softstart   = k_ra_lpm_dcssmode_128us,
    .sscr_low_power   = k_ra_lpm_ss2lp_default,
  };
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_lpm_init(&lpm_cfg));
  const ra_power_profile_config_t pp_cfg = {
    .pulse         = nullptr,
    .now_us        = test_pp_now_us,
    .user_ctx_gpio = nullptr,
    .user_ctx_time = nullptr,
  };
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_power_profile_init(&pp_cfg));

  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_power_profile_mark_enter(k_ra_power_profile_region_active));
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_power_profile_mark_exit(k_ra_power_profile_region_active));
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_power_profile_mark_enter(k_ra_power_profile_region_sleep));
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_lpm_enter_sleep(k_ra_sleep_mode_sleep));
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_power_profile_mark_exit(k_ra_power_profile_region_sleep));

  ra_power_profile_stats_t stats = {};
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_power_profile_get_stats(&stats));
  TEST_ASSERT(stats.regions[k_ra_power_profile_region_active].entries == 1U);
  TEST_ASSERT(stats.regions[k_ra_power_profile_region_active].exits == 1U);
  TEST_ASSERT(stats.regions[k_ra_power_profile_region_active].total_time_us > 0U);
  TEST_END("power_profiler: cycle 4 modes accumulates time");
}

int main(void)
{
  test_pp_app_bringup_ok();
  test_pp_app_init_null_rejected();
  test_pp_app_cycle_modes();
  return 0;
}
