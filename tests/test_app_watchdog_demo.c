/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file test_app_watchdog_demo.c
 * @brief Integration test: IWDT refresh + reset-cause introspection
 *
 * @details
 * Mirrors examples/ek_ra8d2/watchdog_demo/main.c bring-up:
 * ra8_reset_init -> ra8_reset_get_cause -> banner selection ->
 * ra8_iwdt_init + ra8_iwdt_refresh_deferred loop.
 *
 * The reset-cause banner picker is replicated here so we can MC/DC
 * the three-way decision (power_on, iwdt, default).
 *
 * @since 0.1.0
 */

#include <stdint.h>
#include <string.h>

#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_iwdt.h"
#include "ra8_reset.h"
#include "unity_minimal.h"

static const uint8_t k_t_msg_pwr[]   = "wdt: boot reason=power_on\r\n";
static const uint8_t k_t_msg_wdt[]   = "wdt: boot reason=iwdt\r\n";
static const uint8_t k_t_msg_other[] = "wdt: boot reason=other\r\n";

/**
 * @brief Banner picker -- copy of wdt_demo_banner_for in main.c.
 */
static const uint8_t* banner_for(ra8_reset_cause_t cause, uint32_t* out_len)
{
  if (cause == k_ra8_reset_cause_power_on) {
    *out_len = (uint32_t)(sizeof(k_t_msg_pwr) - 1U);
    return k_t_msg_pwr;
  }
  if (cause == k_ra8_reset_cause_iwdt) {
    *out_len = (uint32_t)(sizeof(k_t_msg_wdt) - 1U);
    return k_t_msg_wdt;
  }
  *out_len = (uint32_t)(sizeof(k_t_msg_other) - 1U);
  return k_t_msg_other;
}

static void reset_world(void)
{
  ra8_fake_mmap_reset();
  ra8_reset_test_only_reset_state();
}

/**
 * @brief Banner selection covers all three branches.
 *
 * @par MC/DC:
 * Compound decision: ``cause == power_on || cause == iwdt`` (with a
 * default fall-through). Two atomic conditions x N+1 = 3 vectors --
 * power_on (vector A), iwdt (vector B), other cause (vector C).
 */
static void test_wdt_app_banner_branches(void)
{
  TEST_BEGIN("watchdog_demo: banner picks correct branch");
  uint32_t       n = 0U;
  const uint8_t* a = banner_for(k_ra8_reset_cause_power_on, &n);
  TEST_ASSERT_EQ(0, memcmp(a, k_t_msg_pwr, n));
  const uint8_t* b = banner_for(k_ra8_reset_cause_iwdt, &n);
  TEST_ASSERT_EQ(0, memcmp(b, k_t_msg_wdt, n));
  const uint8_t* c = banner_for(k_ra8_reset_cause_software, &n);
  TEST_ASSERT_EQ(0, memcmp(c, k_t_msg_other, n));
  TEST_END("watchdog_demo: banner picks correct branch");
}

/**
 * @brief reset-init + cause read returns ok with non-NULL out.
 *
 * @par MC/DC:
 * Decision: ``out == nullptr``. One atomic condition x 2 vectors --
 * non-NULL (this test) + NULL (test_wdt_app_cause_null).
 */
static void test_wdt_app_reset_cause_ok(void)
{
  reset_world();
  TEST_BEGIN("watchdog_demo: reset_init + get_cause");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_reset_init());
  ra8_reset_cause_t cause = k_ra8_reset_cause_unknown;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_reset_get_cause(&cause));
  TEST_END("watchdog_demo: reset_init + get_cause");
}

/**
 * @brief NULL out pointer is rejected by ra8_reset_get_cause.
 *
 * @par MC/DC:
 * Decision: ``out == nullptr`` -- NULL vector pairs with golden test.
 */
static void test_wdt_app_cause_null(void)
{
  reset_world();
  TEST_BEGIN("watchdog_demo: NULL get_cause rejected");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_reset_init());
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_reset_get_cause(nullptr));
  TEST_END("watchdog_demo: NULL get_cause rejected");
}

/**
 * @brief IWDT init + refresh loop runs with no error.
 *
 * @par MC/DC:
 * Decision in app: ``ra8_iwdt_init != ok``. Two vectors -- ok (this
 * test) + the host mock cannot fail today, but the call site is
 * structured so a future fault-injection seam covers the err path.
 */
static void test_wdt_app_iwdt_loop(void)
{
  reset_world();
  TEST_BEGIN("watchdog_demo: iwdt init + refresh loop");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_iwdt_init());
  for (uint8_t i = 0U; i < 8U; ++i) {
    ra8_iwdt_refresh_deferred();
  }
  uint16_t status = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_iwdt_get_status(&status));
  TEST_END("watchdog_demo: iwdt init + refresh loop");
}

int main(void)
{
  test_wdt_app_banner_branches();
  test_wdt_app_reset_cause_ok();
  test_wdt_app_cause_null();
  test_wdt_app_iwdt_loop();
  return 0;
}
