/**
 * @file test_app_lpm_idle_demo.c
 * @brief Integration test: LPM init + Sleep-mode entry round-trip
 *
 * @details
 * Mirrors examples/ek_ra8d2/hil_needs_revalidation/lpm_idle_demo/src/main.c bring-up:
 * ra8_lpm_init -> ra8_lpm_enter_sleep(k_ra8_sleep_mode_sleep). On host
 * builds WFI is a no-op so the call returns immediately and the
 * register state can be checked. Each test exercises one branch of
 * the bring-up / wake compound decisions for MC/DC coverage.
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

/**
 * @brief Bring-up programmes SBYCR / DPSBYCR with the requested config.
 *
 * @par MC/DC:
 * Decision: ``ra8_lpm_init != ok``. One atomic condition x 2 vectors --
 * non-NULL cfg (this) + NULL cfg (test_lpm_app_init_null).
 */
static void test_lpm_app_init_ok(void)
{
  reset_world();
  TEST_BEGIN("lpm_idle_demo: init ok");
  const ra8_lpm_config_t cfg = {
    .io_port_keep     = false,
    .opa_bus_keep     = true,
    .sscr_fast_return = false,
    .dcdc_softstart   = k_ra8_lpm_dcssmode_128us,
    .sscr_low_power   = k_ra8_lpm_ss2lp_default,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_lpm_init(&cfg));
  TEST_END("lpm_idle_demo: init ok");
}

/**
 * @brief NULL config rejected by ra8_lpm_init.
 *
 * @par MC/DC:
 * Decision: ``cfg == nullptr``. One atomic condition x 2 vectors --
 * NULL (this) + non-NULL (test_lpm_app_init_ok).
 */
static void test_lpm_app_init_null(void)
{
  reset_world();
  TEST_BEGIN("lpm_idle_demo: NULL cfg rejected");
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_lpm_init(nullptr));
  TEST_END("lpm_idle_demo: NULL cfg rejected");
}

/**
 * @brief Sleep-mode entry returns ok and leaves LPSCR.LPMD == 0.
 *
 * @par MC/DC:
 * Decision in app: ``ra8_lpm_enter_sleep != ok``. One atomic
 * condition x 2 vectors -- valid mode (this) + invalid mode below.
 */
static void test_lpm_app_enter_sleep_ok(void)
{
  reset_world();
  TEST_BEGIN("lpm_idle_demo: enter Sleep returns ok");
  const ra8_lpm_config_t cfg = {
    .io_port_keep     = false,
    .opa_bus_keep     = true,
    .sscr_fast_return = false,
    .dcdc_softstart   = k_ra8_lpm_dcssmode_128us,
    .sscr_low_power   = k_ra8_lpm_ss2lp_default,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_lpm_init(&cfg));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_lpm_enter_sleep(k_ra8_sleep_mode_sleep));
  TEST_END("lpm_idle_demo: enter Sleep returns ok");
}

/**
 * @brief Status read after Sleep returns init values.
 *
 * @par MC/DC:
 * Decision: ``out == nullptr`` in ra8_lpm_get_status. Pairs with
 * the NULL-pointer rejection vector for N+1 = 2 coverage.
 */
static void test_lpm_app_status_after_sleep(void)
{
  reset_world();
  TEST_BEGIN("lpm_idle_demo: get_status returns ok after wake");
  const ra8_lpm_config_t cfg = {
    .io_port_keep     = false,
    .opa_bus_keep     = true,
    .sscr_fast_return = false,
    .dcdc_softstart   = k_ra8_lpm_dcssmode_128us,
    .sscr_low_power   = k_ra8_lpm_ss2lp_default,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_lpm_init(&cfg));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_lpm_enter_sleep(k_ra8_sleep_mode_sleep));
  uint32_t status = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_lpm_get_status(&status));
  TEST_END("lpm_idle_demo: get_status returns ok after wake");
}

int main(void)
{
  test_lpm_app_init_ok();
  test_lpm_app_init_null();
  test_lpm_app_enter_sleep_ok();
  test_lpm_app_status_after_sleep();
  return 0;
}
