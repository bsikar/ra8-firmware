/**
 * @file test_ra8_board_ek_ra8d2_ethernet_cov.c
 * @brief Coverage-boosting unit tests for the EK-RA8D2 Ethernet BSP layer
 *
 * @details
 * Companion to test_ra8_board_ek_ra8d2.c.  Exercises the two error-return
 * branches in ra8_board_ek_ra8d2_ethernet.c that are reachable from a
 * host unit-test context by pre-claiming a board pin before the call
 * so the underlying GPIO / PFS validator returns a conflict:
 *
 *   - internal_eth_phy_hw_reset:
 *       Pre-claim the PHY RSTN pin (P7.8) so ra8_gpio_output_init inside
 *       internal_eth_phy_hw_reset returns k_ra8_err_gpio_conflict, which
 *       ra8_board_ethernet_init then propagates from its Step 0 guard.
 *
 *   - internal_eth_route_alt_pins:
 *       Pre-claim the MDC pin (P4.15) so ra8_pfs_route_peripheral inside
 *       internal_eth_route_alt_pins returns k_ra8_err_gpio_conflict on the
 *       second non-RSTN pin, which ra8_board_ethernet_init then propagates
 *       from its Step 1 guard.
 *
 * All other uncovered branches in the module are error-return paths where
 * the called HAL function cannot fail under RA8_OFF_TARGET given the
 * fixed, valid arguments that the ethernet module always passes; those
 * lines carry GCOVR_EXCL_LINE in the source with a justification comment.
 * The chip-generic COMA / RGMII register work moved to the ETH HAL in
 * issue #581, so the CABPIRM.BPR timeout leg is now covered in
 * test_ra8_eth_coma.c and the RGMII media-select in test_ra8_eth.c; the
 * board happy path (which drives both HAL primitives) stays covered by
 * test_board_ethernet_init in test_ra8_board_ek_ra8d2.c.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_board_ek_ra8d2.h"
#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_pin_validator.h"
#include "ra8_port_constants.h"
#include "unity_minimal.h"

/* -------------------------------------------------------------------------
 * Helpers
 * -------------------------------------------------------------------------
 */

/**
 * @brief Reset all fake peripheral state and pin ownership.
 *
 * @details
 * Mirrors reset_board_hal_state() in test_ra8_board_ek_ra8d2.c.  Called
 * at the start of every test that makes HAL pin claims to guarantee a
 * clean slate regardless of execution order within this binary.
 *
 * @pre None.
 * @post ra8_fake_mmap register window cleared; pin validator bitmap zeroed.
 *
 * @note Not thread-safe; single-threaded test context only.
 * @since 0.1.0
 */
static void reset_state(void)
{
  ra8_fake_mmap_reset();
  ra8_pin_validator_reset();
}

/* -------------------------------------------------------------------------
 * Test 1 -- internal_eth_phy_hw_reset gpio conflict
 * -------------------------------------------------------------------------
 */

/**
 * @brief Verify that ra8_board_ethernet_init propagates a gpio conflict from
 *        internal_eth_phy_hw_reset.
 *
 * @details
 * Pre-claims the PHY RSTN pin (P7.8 = k_ra8_board_eth_pin_rstn) via
 * ra8_pin_validator_claim before calling ra8_board_ethernet_init.  The call
 * sequence inside ra8_board_ethernet_init is:
 *
 *   1. internal_eth_phy_hw_reset():
 *      ra8_gpio_output_init(P7.8, LOW) -> ra8_pin_validator_claim(P7.8, ...)
 *      fails with k_ra8_err_gpio_conflict (P7.8 already claimed).
 *      -> return err from internal_eth_phy_hw_reset.
 *   2. ra8_board_ethernet_init:
 *      err != k_ra8_ok -> propagate err from the Step 0 guard.
 *
 * The test verifies that the returned error is non-zero.  The exact
 * value (k_ra8_err_gpio_conflict) is asserted to confirm the conflict
 * path, not a different failure mode.
 *
 * @par MC/DC:
 * Decision: `if (err != k_ra8_ok)` in internal_eth_phy_hw_reset
 *   1 condition.
 * - Vector A: err=k_ra8_ok (happy path, covered by test_board_ethernet_init
 *   in test_ra8_board_ek_ra8d2.c)
 * - Vector B: err=k_ra8_err_gpio_conflict (this test)
 * Vectors A+B prove the condition independently affects the outcome;
 * N+1=2 vectors for N=1 condition: minimal MC/DC.
 *
 * Decision: `if (err != k_ra8_ok)` in ra8_board_ethernet_init (Step 0 guard)
 *   1 condition.
 * - Vector A: err=k_ra8_ok (happy path, existing test)
 * - Vector B: err=k_ra8_err_gpio_conflict (this test)
 * Same minimal MC/DC argument as above.
 *
 * @pre Clean pin-validator state (all pins free).
 * @post P7.8 remains claimed by the test's pre-claim; the function
 *       returned before ra8_gpio_output_init had a chance to claim it.
 *
 * @note Not thread-safe; single-threaded test context.
 * @since 0.1.0
 */
static void test_eth_phy_hw_reset_gpio_conflict(void)
{
  TEST_BEGIN("eth_phy_hw_reset: gpio conflict on RSTN pin");
  reset_state();

  /* Pre-claim the RSTN pin so ra8_gpio_output_init sees a conflict. */
  /* NOLINTBEGIN(clang-analyzer-optin.core.EnumCastOutOfRange) -- RA8_PIN()-packed board pin is valid ra8_port_pin_t data outside the enumerator list. */
  const ra8_port_pin_t rstn_pin = (ra8_port_pin_t)k_ra8_board_eth_pin_rstn;
  /* NOLINTEND(clang-analyzer-optin.core.EnumCastOutOfRange) */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_pin_validator_claim(rstn_pin, "test"));

  const ra8_err_t err = ra8_board_ethernet_init();
  TEST_ASSERT_EQ(k_ra8_err_gpio_conflict, err);

  TEST_END("eth_phy_hw_reset: gpio conflict on RSTN pin");
}

/* -------------------------------------------------------------------------
 * Test 2 -- internal_eth_route_alt_pins pfs conflict
 * -------------------------------------------------------------------------
 */

/**
 * @brief Verify that ra8_board_ethernet_init propagates a pin-conflict error
 *        from internal_eth_route_alt_pins.
 *
 * @details
 * Pre-claims the MDC pin (P4.15 = k_ra8_board_eth_pin_mdc) and leaves the
 * RSTN pin (P7.8) free.  The call sequence inside ra8_board_ethernet_init
 * is then:
 *
 *   1. internal_eth_phy_hw_reset(): RSTN (P7.8) is free, gpio_output_init
 *      succeeds, gpio_write succeeds -> returns k_ra8_ok.
 *
 *   2. internal_eth_route_alt_pins():
 *      s_eth_routes index 0  = MDINT (P1.7): free -> pfs_route_peripheral ok.
 *      s_eth_routes index 1  = MDC   (P4.15): pre-claimed -> conflict.
 *      -> return err from internal_eth_route_alt_pins.
 *
 *   3. ra8_board_ethernet_init:
 *      err != k_ra8_ok -> propagate err from the Step 1 guard.
 *
 * @par MC/DC:
 * Decision: `if (err != k_ra8_ok)` in internal_eth_route_alt_pins
 *   1 condition.
 * - Vector A: err=k_ra8_ok (MDINT at index 0 routed successfully; also the
 *   full happy-path in test_ra8_board_ek_ra8d2.c)
 * - Vector B: err=k_ra8_err_gpio_conflict (MDC at index 1, this test)
 * Vectors A+B: minimal MC/DC for a single-condition decision.
 *
 * Decision: `if (err != k_ra8_ok)` in ra8_board_ethernet_init (Step 1 guard)
 *   1 condition.
 * - Vector A: err=k_ra8_ok (happy path, existing test)
 * - Vector B: err=k_ra8_err_gpio_conflict (this test)
 * Vectors A+B: minimal MC/DC.
 *
 * @pre Clean pin-validator state (all pins free).
 * @post RSTN (P7.8) and MDINT (P1.7) are claimed by the partial ethernet
 *       init; MDC (P4.15) remains claimed by the test's pre-claim.
 *
 * @note Not thread-safe; single-threaded test context.
 * @since 0.1.0
 */
static void test_eth_route_alt_pins_pfs_conflict(void)
{
  TEST_BEGIN("eth_route_alt_pins: pfs conflict on MDC pin");
  reset_state();

  /* Pre-claim MDC (P4.15, index 1 in s_eth_routes) while leaving RSTN
   * (P7.8, index 15) free so internal_eth_phy_hw_reset succeeds. */
  /* NOLINTBEGIN(clang-analyzer-optin.core.EnumCastOutOfRange) -- RA8_PIN()-packed board pin is valid ra8_port_pin_t data outside the enumerator list. */
  const ra8_port_pin_t mdc_pin = (ra8_port_pin_t)k_ra8_board_eth_pin_mdc;
  /* NOLINTEND(clang-analyzer-optin.core.EnumCastOutOfRange) */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_pin_validator_claim(mdc_pin, "test"));

  const ra8_err_t err = ra8_board_ethernet_init();
  TEST_ASSERT_EQ(k_ra8_err_gpio_conflict, err);

  TEST_END("eth_route_alt_pins: pfs conflict on MDC pin");
}

/* -------------------------------------------------------------------------
 * Entry point
 * -------------------------------------------------------------------------
 */

/**
 * @brief Test binary entry point.
 *
 * @details
 * Runs every coverage-boosting test in sequence.  Returns 0 on success;
 * any TEST_ASSERT failure calls exit(1) before this function returns.
 *
 * @return 0 on success.
 *
 * @pre ra8_fake_mmap register window allocated by the test framework constructor.
 * @post All targeted source lines are instrumented with gcov data.
 *
 * @note Not thread-safe; single-threaded test runner.
 * @since 0.1.0
 */
int32_t main(void)
{
  test_eth_phy_hw_reset_gpio_conflict();
  test_eth_route_alt_pins_pfs_conflict();
  (void)fprintf(stderr, "[OK ] test_ra8_board_ek_ra8d2_ethernet_cov.c\n");
  return 0;
}
