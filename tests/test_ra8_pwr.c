/**
 * @file test_ra8_pwr.c
 * @brief Unit tests for the LPM + clock-domain wrapper (libs/ra8_hal/src/ra8_pwr.c).
 * @details Exercises power-domain acquisition, module-stop transitions, low-power entry guards, and wake configuration through fake registers.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8_cgc.h"
#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_lpm_regs.h"
#include "ra8_mstp.h"
#include "ra8_mstp_regs.h"
#include "ra8_pwr.h"
#include "ra8_system_regs.h"
#include "unity_minimal.h"

/**
 * @enum t_wupen_t
 * @brief Wake-up enable patterns staged into the two WUPEN registers.
 *
 * @details
 * Two different patterns, so a driver that reads or writes the wrong register
 * produces the other one's value rather than a plausible result.
 */
typedef enum : uint32_t {
  k_t_wupen0_pattern = 0xDEADBEEFU, /**< Pattern staged in WUPEN0. */
  k_t_wupen1_pattern = 0xCAFEBABEU, /**< Pattern staged in WUPEN1. */
} t_wupen_t;

static volatile uint32_t* wupen0_ptr(void)
{
  return (volatile uint32_t*)((uintptr_t)k_ra8_system_base_addr + (uintptr_t)k_ra8_lpm_wupen0_off);
}

static volatile uint32_t* wupen1_ptr(void)
{
  return (volatile uint32_t*)((uintptr_t)k_ra8_system_base_addr + (uintptr_t)k_ra8_lpm_wupen1_off);
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_clears_wupen_and_resets_mstp(void)
{
  TEST_BEGIN("ra8_pwr_init: WUPEN0/1 cleared, MSTP table reset");
  ra8_fake_mmap_reset();

  /* Pollute WUPEN0/1 first to prove init clears them. */
  *wupen0_ptr() = k_t_wupen0_pattern;
  *wupen1_ptr() = k_t_wupen1_pattern;

  TEST_ASSERT_EQ(k_ra8_ok, ra8_pwr_init());
  TEST_ASSERT_EQ(0, *wupen0_ptr());
  TEST_ASSERT_EQ(0, *wupen1_ptr());

  /* MSTP all stopped; MSTPCRA bits 0-3 (SRAM0-3) kept 0. */
  TEST_ASSERT_EQ(0xFFFFFFF0U, ra8_mstp()->MSTPCRA);
  TEST_END("ra8_pwr_init: WUPEN0/1 cleared, MSTP table reset");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_module_request_release_round_trip(void)
{
  TEST_BEGIN("ra8_pwr_module_request / release: round trip");
  ra8_fake_mmap_reset();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_pwr_init());

  TEST_ASSERT_EQ(k_ra8_ok, ra8_pwr_module_request(k_ra8_mstp_sci0));

  uint8_t ref = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mstp_get_refcount(k_ra8_mstp_sci0, &ref));
  TEST_ASSERT_EQ(1, ref);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_pwr_module_release(k_ra8_mstp_sci0));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mstp_get_refcount(k_ra8_mstp_sci0, &ref));
  TEST_ASSERT_EQ(0, ref);

  TEST_END("ra8_pwr_module_request / release: round trip");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_set_clear_wake_source(void)
{
  TEST_BEGIN("ra8_pwr_set_wake_source / clear_wake_source");
  ra8_fake_mmap_reset();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_pwr_init());

  TEST_ASSERT_EQ(k_ra8_ok, ra8_pwr_set_wake_source(k_ra8_pwr_wake_irq3));
  /* IRQ3 is bit 3 in WUPEN0. */
  TEST_ASSERT(((*wupen0_ptr()) & (1U << 3)) != 0U);

  bool enabled = false;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_pwr_wake_source_is_enabled(k_ra8_pwr_wake_irq3, &enabled));
  TEST_ASSERT(enabled);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_pwr_clear_wake_source(k_ra8_pwr_wake_irq3));
  TEST_ASSERT_EQ(0, ((*wupen0_ptr()) & (1U << 3)));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_pwr_wake_source_is_enabled(k_ra8_pwr_wake_irq3, &enabled));
  TEST_ASSERT(!enabled);

  TEST_END("ra8_pwr_set_wake_source / clear_wake_source");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_wake_source_wupen1(void)
{
  TEST_BEGIN("ra8_pwr_set_wake_source: WUPEN1 source (AGT0)");
  ra8_fake_mmap_reset();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_pwr_init());

  TEST_ASSERT_EQ(k_ra8_ok, ra8_pwr_set_wake_source(k_ra8_pwr_wake_agt0));
  /* AGT0 is bit 0 in WUPEN1. */
  TEST_ASSERT_EQ(0x1U, *wupen1_ptr());

  TEST_END("ra8_pwr_set_wake_source: WUPEN1 source (AGT0)");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_wake_source_invalid_id(void)
{
  TEST_BEGIN("ra8_pwr_set_wake_source: invalid id rejected");
  ra8_fake_mmap_reset();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_pwr_init());

  /* reg=2 -> out of range (only WUPEN0/1 exist). */
  const ra8_pwr_wake_t bogus = (ra8_pwr_wake_t)((2U << 8) | 0U);
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_pwr_set_wake_source(bogus));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_pwr_clear_wake_source(bogus));

  bool enabled = false;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_pwr_wake_source_is_enabled(bogus, &enabled));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_pwr_wake_source_is_enabled(k_ra8_pwr_wake_irq0, nullptr));

  TEST_END("ra8_pwr_set_wake_source: invalid id rejected");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_enter_sleep_is_no_op_on_host(void)
{
  TEST_BEGIN("ra8_pwr_enter_sleep: returns immediately on host");
  ra8_fake_mmap_reset();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_pwr_init());

  /* On RA8_OFF_TARGET this is a no-op so the test simply
   * proves the function compiles and links cleanly. */
  ra8_pwr_enter_sleep();

  TEST_END("ra8_pwr_enter_sleep: returns immediately on host");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_software_standby_requires_wake_source(void)
{
  TEST_BEGIN("ra8_pwr_enter_software_standby: refuses without wake source");
  ra8_fake_mmap_reset();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_pwr_init());

  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_pwr_enter_software_standby());

  TEST_ASSERT_EQ(k_ra8_ok, ra8_pwr_set_wake_source(k_ra8_pwr_wake_rtc_alarm));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_pwr_enter_software_standby());

  TEST_END("ra8_pwr_enter_software_standby: refuses without wake source");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_get_clock_hz_forwards_to_cgc(void)
{
  TEST_BEGIN("ra8_pwr_get_clock_hz: forwards to ra8_cgc_get_clock_hz");
  ra8_fake_mmap_reset();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_pwr_init());

  uint32_t hz = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_pwr_get_clock_hz(k_ra8_clock_id_cpuclk0, &hz));
  TEST_ASSERT(hz != 0U);

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_pwr_get_clock_hz(k_ra8_clock_id_cpuclk0, nullptr));

  TEST_END("ra8_pwr_get_clock_hz: forwards to ra8_cgc_get_clock_hz");
}

/**
 * @test test_mcdc_software_standby_wupen
 *
 * @par MC/DC:
 * Decision: `if ((wupen0 == 0U) && (wupen1 == 0U))`
 * (2 conditions, libs/ra8_hal/src/ra8_pwr.c line 203)
 * - Vector 1: wupen0=0, wupen1=0 -> T,T  decision T  -> invalid_state
 * - Vector 2: wupen0!=0, wupen1=0 -> F,_ decision F  -> ok (C1 short-circuits)
 * - Vector 3: wupen0=0, wupen1!=0 -> T,F decision F  -> ok
 * Vectors 1+2 vary C1 with decision flipping; vectors 1+3 vary C2 with
 * decision flipping. N+1 = 3 vectors for N=2 conditions: minimal MC/DC.
 */
static void test_mcdc_software_standby_wupen(void)
{
  TEST_BEGIN("ra8_pwr_enter_software_standby MC/DC: wupen0==0 && wupen1==0");
  ra8_fake_mmap_reset();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_pwr_init());

  /* Vector 1: both zero -> T,T -> decision T -> invalid_state. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_pwr_enter_software_standby());

  /* Vector 2: wupen0 != 0, wupen1 == 0 -> F,_ -> decision F -> ok.
   * IRQ3 lands in WUPEN0 bit 3. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_pwr_set_wake_source(k_ra8_pwr_wake_irq3));
  TEST_ASSERT(*wupen0_ptr() != 0U);
  TEST_ASSERT_EQ(0, *wupen1_ptr());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_pwr_enter_software_standby());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_pwr_clear_wake_source(k_ra8_pwr_wake_irq3));

  /* Vector 3: wupen0 == 0, wupen1 != 0 -> T,F -> decision F -> ok.
   * AGT0 lands in WUPEN1 bit 0. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_pwr_set_wake_source(k_ra8_pwr_wake_agt0));
  TEST_ASSERT_EQ(0, *wupen0_ptr());
  TEST_ASSERT(*wupen1_ptr() != 0U);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_pwr_enter_software_standby());

  TEST_END("ra8_pwr_enter_software_standby MC/DC: wupen0==0 && wupen1==0");
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
  test_mcdc_software_standby_wupen();
  return 0;
}
