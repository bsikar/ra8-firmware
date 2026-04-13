/**
 * @file test_ra_pwr.c
 * @brief Unit tests for the LPM + clock-domain wrapper (libs/ra_hal/src/ra_pwr.c).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8d2_lpm_regs.h"
#include "ra8d2_mstp_regs.h"
#include "ra8d2_system_regs.h"
#include "ra_cgc.h"
#include "ra_err.h"
#include "ra_mstp.h"
#include "ra_pwr.h"
#include "ra_sim_mmap.h"
#include "unity_minimal.h"

static volatile uint32_t* wupen0_ptr(void)
{
  return (volatile uint32_t*)((uintptr_t)k_ra_system_base_addr + (uintptr_t)k_ra_lpm_wupen0_off);
}

static volatile uint32_t* wupen1_ptr(void)
{
  return (volatile uint32_t*)((uintptr_t)k_ra_system_base_addr + (uintptr_t)k_ra_lpm_wupen1_off);
}

static void test_init_clears_wupen_and_resets_mstp(void)
{
  TEST_BEGIN("ra_pwr_init: WUPEN0/1 cleared, MSTP table reset");
  ra_sim_mmap_reset();

  /* Pollute WUPEN0/1 first to prove init clears them. */
  *wupen0_ptr() = 0xDEADBEEFU;
  *wupen1_ptr() = 0xCAFEBABEU;

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_pwr_init());
  TEST_ASSERT_EQ((int64_t)0, (int64_t)*wupen0_ptr());
  TEST_ASSERT_EQ((int64_t)0, (int64_t)*wupen1_ptr());

  /* MSTP all stopped. */
  TEST_ASSERT_EQ((int64_t)0xFFFFFFFFU, (int64_t)ra_mstp()->MSTPCRA);
  TEST_END("ra_pwr_init: WUPEN0/1 cleared, MSTP table reset");
}

static void test_module_request_release_round_trip(void)
{
  TEST_BEGIN("ra_pwr_module_request / release: round trip");
  ra_sim_mmap_reset();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_pwr_init());

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_pwr_module_request(k_ra_mstp_sci0));

  uint8_t ref = 0U;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_mstp_get_refcount(k_ra_mstp_sci0, &ref));
  TEST_ASSERT_EQ((int64_t)1, (int64_t)ref);

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_pwr_module_release(k_ra_mstp_sci0));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_mstp_get_refcount(k_ra_mstp_sci0, &ref));
  TEST_ASSERT_EQ((int64_t)0, (int64_t)ref);

  TEST_END("ra_pwr_module_request / release: round trip");
}

static void test_set_clear_wake_source(void)
{
  TEST_BEGIN("ra_pwr_set_wake_source / clear_wake_source");
  ra_sim_mmap_reset();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_pwr_init());

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_pwr_set_wake_source(k_ra_pwr_wake_irq3));
  /* IRQ3 is bit 3 in WUPEN0. */
  TEST_ASSERT(((*wupen0_ptr()) & (1U << 3)) != 0U);

  bool enabled = false;
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_pwr_wake_source_is_enabled(k_ra_pwr_wake_irq3, &enabled));
  TEST_ASSERT(enabled);

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_pwr_clear_wake_source(k_ra_pwr_wake_irq3));
  TEST_ASSERT_EQ((int64_t)0, (int64_t)((*wupen0_ptr()) & (1U << 3)));

  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_pwr_wake_source_is_enabled(k_ra_pwr_wake_irq3, &enabled));
  TEST_ASSERT(!enabled);

  TEST_END("ra_pwr_set_wake_source / clear_wake_source");
}

static void test_wake_source_wupen1(void)
{
  TEST_BEGIN("ra_pwr_set_wake_source: WUPEN1 source (AGT0)");
  ra_sim_mmap_reset();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_pwr_init());

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_pwr_set_wake_source(k_ra_pwr_wake_agt0));
  /* AGT0 is bit 0 in WUPEN1. */
  TEST_ASSERT_EQ((int64_t)0x1U, (int64_t)*wupen1_ptr());

  TEST_END("ra_pwr_set_wake_source: WUPEN1 source (AGT0)");
}

static void test_wake_source_invalid_id(void)
{
  TEST_BEGIN("ra_pwr_set_wake_source: invalid id rejected");
  ra_sim_mmap_reset();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_pwr_init());

  /* reg=2 -> out of range (only WUPEN0/1 exist). */
  const ra_pwr_wake_t bogus = (ra_pwr_wake_t)((2U << 8) | 0U);
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_pwr_set_wake_source(bogus));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_pwr_clear_wake_source(bogus));

  bool enabled = false;
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_pwr_wake_source_is_enabled(bogus, &enabled));
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr,
                 (int32_t)ra_pwr_wake_source_is_enabled(k_ra_pwr_wake_irq0, nullptr));

  TEST_END("ra_pwr_set_wake_source: invalid id rejected");
}

static void test_enter_sleep_is_no_op_on_host(void)
{
  TEST_BEGIN("ra_pwr_enter_sleep: returns immediately on host");
  ra_sim_mmap_reset();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_pwr_init());

  /* On RA_SIMULATOR_MODE this is a no-op so the test simply
   * proves the function compiles and links cleanly. */
  ra_pwr_enter_sleep();

  TEST_END("ra_pwr_enter_sleep: returns immediately on host");
}

static void test_software_standby_requires_wake_source(void)
{
  TEST_BEGIN("ra_pwr_enter_software_standby: refuses without wake source");
  ra_sim_mmap_reset();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_pwr_init());

  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_state, (int32_t)ra_pwr_enter_software_standby());

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_pwr_set_wake_source(k_ra_pwr_wake_rtc_alarm));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_pwr_enter_software_standby());

  TEST_END("ra_pwr_enter_software_standby: refuses without wake source");
}

static void test_get_clock_hz_forwards_to_cgc(void)
{
  TEST_BEGIN("ra_pwr_get_clock_hz: forwards to ra_cgc_get_clock_hz");
  ra_sim_mmap_reset();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_pwr_init());

  uint32_t hz = 0U;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_pwr_get_clock_hz(k_ra_clock_id_cpuclk0, &hz));
  TEST_ASSERT(hz != 0U);

  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr,
                 (int32_t)ra_pwr_get_clock_hz(k_ra_clock_id_cpuclk0, nullptr));

  TEST_END("ra_pwr_get_clock_hz: forwards to ra_cgc_get_clock_hz");
}

int32_t main(void)
{
  test_init_clears_wupen_and_resets_mstp();
  test_module_request_release_round_trip();
  test_set_clear_wake_source();
  test_wake_source_wupen1();
  test_wake_source_invalid_id();
  test_enter_sleep_is_no_op_on_host();
  test_software_standby_requires_wake_source();
  test_get_clock_hz_forwards_to_cgc();
  (void)fprintf(stderr, "[OK  ] test_ra_pwr.c\n");
  return 0;
}
