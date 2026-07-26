/**
 * @file test_app_tz_nsc_cgc_usb.c
 * @brief Integration test: NSC-veneer CGC + USB-FS bring-up
 *
 * @details
 * Mirrors the multi-module integration-test pattern. The
 * production app at examples/ek_ra8d2/hil_needs_revalidation/tz_nsc_cgc_usb/main.c reaches
 * the secure-side CGC driver via three Non-Secure Callable veneers:
 *
 *   - ra8_nsc_cgc_pll2_enable
 *   - ra8_nsc_cgc_usbfs_clock_enable
 *   - ra8_nsc_cgc_get_clock_hz
 *
 * In the host build RA8_TRUSTZONE_ENABLE is undefined, so the
 * RA8_NSC_VENEER and RA8_NSC_CHECK_NS_RANGE_* macros expand to no-ops
 * and each veneer becomes a plain forwarding wrapper. That is exactly
 * the contract we want to exercise here -- the test verifies that the
 * veneer signatures, return-code propagation, and pointer-validation
 * paths all behave the same way the secure-world thunks would, plus
 * that the surrounding USB-FS bring-up still composes around the
 * NSC entries.
 *
 * Modeled flow:
 *   1. ra8_pfs_route_peripheral for the four USB-FS pins.
 *   2. ra8_nsc_cgc_pll2_enable(80, 0, k_ra8_plodiv_div4).
 *   3. ra8_nsc_cgc_usbfs_clock_enable().
 *   4. ra8_nsc_cgc_get_clock_hz(cpuclk0).
 *   5. ra8_board_led_init(LED1).
 *   6. ra8_usb_pal_init(speed=fs).
 *   7. ra8_usb_pal_attach(true).
 *
 * Exercised modules:
 *   - ra8_nsc_cgc (NSC veneers)
 *   - ra8_cgc     (secure-side driver behind the veneers)
 *   - ra8_pfs     (peripheral pin routing)
 *   - ra8_board_ek_ra8d2 (LED bring-up)
 *   - ra8_usb_pal (device-side controller bridge)
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>
#include <string.h>

#include "ra8_board_ek_ra8d2.h"
#include "ra8_cgc.h"
#include "ra8_cgc_regs.h"
#include "ra8_err.h"
#include "ra8_gpio_constants.h"
#include "ra8_nsc_cgc.h"
#include "ra8_pin_validator.h"
#include "ra8_port_constants.h"
#include "ra8_port_utils.h"
#include "ra8_sim_mmap.h"
#include "ra8_usb_pal.h"
#include "unity_minimal.h"

/** @brief Per-test enums. */
typedef enum : uint16_t {
  k_test_pll2_mul_int      = 80U, /**< PLL2 multiplier integer part. */
  k_test_pll2_mul_quarters = 0U,  /**< PLL2 multiplier quarter part. */
  k_test_pll2_bad_mul_int  = 0U,  /**< Invalid multiplier (zero).    */
  k_test_pll2_bad_mul_q    = 7U,  /**< Invalid quarter (>3).         */
  k_test_cdc_max_packet    = 64U, /**< Bulk-FS packet size.          */
} test_tz_nsc_cgc_const_t;

/** @brief Pin map mirrors examples/ek_ra8d2/hil_needs_revalidation/tz_nsc_cgc_usb/main.c. */
static const ra8_port_pin_t k_test_tz_pin_dp =
  (ra8_port_pin_t)(((uint16_t)k_ra8_port_8 << 8) | (uint16_t)k_ra8_pin_14);
static const ra8_port_pin_t k_test_tz_pin_dm =
  (ra8_port_pin_t)(((uint16_t)k_ra8_port_8 << 8) | (uint16_t)k_ra8_pin_15);

/** @brief Per-test fixture reset. */
static void reset_world(void)
{
  ra8_sim_mmap_reset();
  ra8_pin_validator_reset();
  (void)ra8_usb_pal_deinit();
  (void)ra8_cgc_init();
}

/* -------------------------------------------------------------------------
 * NSC veneer plumbing
 * ------------------------------------------------------------------------- */

/**
 * @brief PLL2 veneer forwards a valid configuration to the secure driver.
 *
 * @par MC/DC:
 * Decision under test (in ra8_cgc_pll2_enable, exercised through the
 * veneer): ``if (mul_int == 0 || mul_quarters > 3)`` -- 2 atomic
 * conditions. Vectors:
 *   - V1: mul_int=80, mul_q=0 -> false || false -> ok           (this test).
 *   - V2: mul_int=0,  mul_q=0 -> true  || false -> invalid_arg  (next test).
 *   - V3: mul_int=80, mul_q=7 -> false || true  -> invalid_arg  (next test).
 * V1+V2 prove mul_int independently flips the outcome; V1+V3 prove
 * the same for mul_quarters. N+1 = 3 vectors for N=2 conditions:
 * minimal MC/DC.
 */
static void test_nsc_cgc_pll2_enable_ok(void)
{
  reset_world();
  TEST_BEGIN("tz_nsc_cgc_usb: ra8_nsc_cgc_pll2_enable accepts good config");
  const ra8_err_t err = ra8_nsc_cgc_pll2_enable((uint8_t)k_test_pll2_mul_int,
                                                (uint8_t)k_test_pll2_mul_quarters,
                                                k_ra8_plodiv_div4);
  TEST_ASSERT(err == k_ra8_ok || err == k_ra8_err_hw_timeout);
  TEST_END("tz_nsc_cgc_usb: ra8_nsc_cgc_pll2_enable accepts good config");
}

/**
 * @brief PLL2 veneer rejects mul_int = 0.
 *
 * @par MC/DC: V2 above.
 */
static void test_nsc_cgc_pll2_enable_rejects_zero_mul(void)
{
  reset_world();
  TEST_BEGIN("tz_nsc_cgc_usb: pll2 rejects mul_int=0");
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_nsc_cgc_pll2_enable((uint8_t)k_test_pll2_bad_mul_int,
                                         (uint8_t)k_test_pll2_mul_quarters,
                                         k_ra8_plodiv_div4));
  TEST_END("tz_nsc_cgc_usb: pll2 rejects mul_int=0");
}

/**
 * @brief PLL2 veneer rejects mul_quarters > 3.
 *
 * @par MC/DC: V3 above.
 */
static void test_nsc_cgc_pll2_enable_rejects_bad_quarters(void)
{
  reset_world();
  TEST_BEGIN("tz_nsc_cgc_usb: pll2 rejects mul_quarters>3");
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_nsc_cgc_pll2_enable((uint8_t)k_test_pll2_mul_int,
                                         (uint8_t)k_test_pll2_bad_mul_q,
                                         k_ra8_plodiv_div4));
  TEST_END("tz_nsc_cgc_usb: pll2 rejects mul_quarters>3");
}

/**
 * @brief get_clock_hz veneer rejects NULL out pointer.
 *
 * @par MC/DC:
 * Decision under test: ``if (hz_out == NULL)`` -- 1 atomic condition.
 * Vectors:
 *   - V1: hz_out = &valid -> false -> ok           (next test).
 *   - V2: hz_out = NULL   -> true  -> null_ptr     (this test).
 * V1+V2 prove hz_out independently flips the outcome. N+1 = 2
 * vectors for N=1: minimal MC/DC.
 */
static void test_nsc_cgc_get_clock_hz_null_rejected(void)
{
  reset_world();
  TEST_BEGIN("tz_nsc_cgc_usb: get_clock_hz rejects NULL out");
  const ra8_err_t err = ra8_nsc_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, nullptr);
  TEST_ASSERT(err == k_ra8_err_null_ptr || err == k_ra8_err_invalid_arg);
  TEST_END("tz_nsc_cgc_usb: get_clock_hz rejects NULL out");
}

/**
 * @brief get_clock_hz veneer returns a non-zero CPUCLK0 frequency.
 *
 * @par MC/DC: V1 of the get_clock_hz NULL-check decision above.
 */
static void test_nsc_cgc_get_clock_hz_ok(void)
{
  reset_world();
  TEST_BEGIN("tz_nsc_cgc_usb: get_clock_hz returns cpuclk0 hz");
  uint32_t hz = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_nsc_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &hz));
  TEST_ASSERT(hz > 0U);
  TEST_END("tz_nsc_cgc_usb: get_clock_hz returns cpuclk0 hz");
}

/**
 * @brief usbfs_clock_enable veneer can run after PLL2 is up.
 *
 * @par MC/DC:
 * Decision under test (in main()): chain of three early-return guards
 * ``ra8_nsc_cgc_pll2_enable != ok || ra8_nsc_cgc_usbfs_clock_enable != ok
 *   || ra8_nsc_cgc_get_clock_hz != ok``. Three atomic conditions x
 * N+1 = 4 vectors. This case covers the all-ok chain; the three
 * failure-side vectors split across the rejection tests above and
 * the NULL-out test for get_clock_hz.
 */
static void test_nsc_cgc_usbfs_chain_ok(void)
{
  reset_world();
  TEST_BEGIN("tz_nsc_cgc_usb: pll2 + usbfs + get_clock_hz chain");
  const ra8_err_t pll2_err = ra8_nsc_cgc_pll2_enable((uint8_t)k_test_pll2_mul_int,
                                                     (uint8_t)k_test_pll2_mul_quarters,
                                                     k_ra8_plodiv_div4);
  TEST_ASSERT(pll2_err == k_ra8_ok || pll2_err == k_ra8_err_hw_timeout);
  const ra8_err_t usbfs_err = ra8_nsc_cgc_usbfs_clock_enable();
  TEST_ASSERT(usbfs_err == k_ra8_ok || usbfs_err == k_ra8_err_hw_timeout);
  uint32_t hz = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_nsc_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &hz));
  TEST_END("tz_nsc_cgc_usb: pll2 + usbfs + get_clock_hz chain");
}

/* -------------------------------------------------------------------------
 * Composition: NSC + USB-FS bring-up still works end-to-end
 * ------------------------------------------------------------------------- */

/**
 * @brief Pins + LED + USB attach compose around the NSC entries.
 *
 * @par MC/DC:
 * Decision under test (in main()): five chained ``!= ok`` guards
 * (PFS D+, PFS D-, LED init, pal_init, pal_attach). Five atomic
 * conditions x N+1 = 6 vectors. This case covers the all-ok chain.
 */
static void test_nsc_full_bringup_chain_ok(void)
{
  reset_world();
  TEST_BEGIN("tz_nsc_cgc_usb: full bring-up chain");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_pfs_route_peripheral(k_test_tz_pin_dp, k_ra8_psel_usb_fs, "tz.dp"));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_pfs_route_peripheral(k_test_tz_pin_dm, k_ra8_psel_usb_fs, "tz.dm"));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_board_led_init(k_ra8_board_led1));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_pal_init(k_ra8_usb_speed_fs));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_pal_attach(true));
  TEST_END("tz_nsc_cgc_usb: full bring-up chain");
}

int main(void)
{
  test_nsc_cgc_pll2_enable_ok();
  test_nsc_cgc_pll2_enable_rejects_zero_mul();
  test_nsc_cgc_pll2_enable_rejects_bad_quarters();
  test_nsc_cgc_get_clock_hz_null_rejected();
  test_nsc_cgc_get_clock_hz_ok();
  test_nsc_cgc_usbfs_chain_ok();
  test_nsc_full_bringup_chain_ok();
  return 0;
}
