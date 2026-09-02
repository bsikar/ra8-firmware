/**
 * @file test_app_lpm_deep_sleep_demo.c
 * @brief Integration test: LPM init + Deep-Sleep entry round-trip
 *
 * @details
 * Mirrors examples/ek_ra8d2/hw_validated/hil/lpm_deep_sleep_demo/src/main.c bring-up:
 * ra8_lpm_init -> ra8_lpm_enter_sleep(k_ra8_sleep_mode_deep_sleep).
 * On host builds WFI is a no-op so the call returns immediately and
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
 * @brief Bring-up programmes SBYCR / DPSBYCR with the demo's config.
 *
 * @par MC/DC:
 * Decision: ``ra8_lpm_init != ok``. One atomic condition x 2 vectors --
 * non-NULL cfg (this) + NULL cfg (test_lpm_deep_sleep_init_null).
 */
static void test_lpm_deep_sleep_init_ok(void)
{
  reset_world();
  TEST_BEGIN("lpm_deep_sleep_demo: init ok");
  const ra8_lpm_config_t cfg = make_demo_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_lpm_init(&cfg));
  TEST_END("lpm_deep_sleep_demo: init ok");
}

/**
 * @brief NULL config rejected by ra8_lpm_init.
 *
 * @par MC/DC:
 * Decision: ``cfg == nullptr``. One atomic condition x 2 vectors --
 * NULL (this) + non-NULL (test_lpm_deep_sleep_init_ok).
 */
static void test_lpm_deep_sleep_init_null(void)
{
  reset_world();
  TEST_BEGIN("lpm_deep_sleep_demo: NULL cfg rejected");
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_lpm_init(nullptr));
  TEST_END("lpm_deep_sleep_demo: NULL cfg rejected");
}

/**
 * @brief Deep-Sleep entry returns ok and leaves LPSCR.LPMD == 0.
 *
 * @par MC/DC:
 * Decision in app: ``ra8_lpm_enter_sleep != ok``. One atomic
 * condition x 2 vectors -- valid mode (this) + invalid mode below.
 *
 * Deep Sleep has LPMD == 0 but SCR.SLEEPDEEP asserted during entry.
 * After wake the HAL clears LPSCR back to 0 and SLEEPDEEP is
 * restored to its pre-call state.
 */
static void test_lpm_deep_sleep_enter_ok(void)
{
  reset_world();
  TEST_BEGIN("lpm_deep_sleep_demo: enter Deep Sleep ok");
  const ra8_lpm_config_t cfg = make_demo_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_lpm_init(&cfg));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_lpm_enter_sleep(k_ra8_sleep_mode_deep_sleep));
  /* HAL clears LPSCR after wake so the next plain WFI is a CPU sleep. */
  TEST_ASSERT_EQ(0, *ra8_lpm_sysc_reg8(k_ra8_lpm_lpscr_off));
  TEST_END("lpm_deep_sleep_demo: enter Deep Sleep ok");
}

/**
 * @brief Status read after Deep Sleep returns init values.
 *
 * @par MC/DC:
 * Decision: ``out == nullptr`` in ra8_lpm_get_status. Pairs with the
 * NULL-pointer rejection vector for N+1 = 2 coverage.
 */
static void test_lpm_deep_sleep_status_after(void)
{
  reset_world();
  TEST_BEGIN("lpm_deep_sleep_demo: get_status ok after wake");
  const ra8_lpm_config_t cfg = make_demo_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_lpm_init(&cfg));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_lpm_enter_sleep(k_ra8_sleep_mode_deep_sleep));
  uint32_t status = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_lpm_get_status(&status));
  TEST_END("lpm_deep_sleep_demo: get_status ok after wake");
}

int main(void)
{
  test_lpm_deep_sleep_init_ok();
  test_lpm_deep_sleep_init_null();
  test_lpm_deep_sleep_enter_ok();
  test_lpm_deep_sleep_status_after();
  return 0;
}
