/**
 * @file test_ra8_board_ek_ra8d2_touch.c
 * @brief Unit tests for the EK-RA8D2 board-level GT911 touch bring-up
 *
 * @details
 * Drives every decision in
 * ``libs/ra8_board_ek_ra8d2/src/ra8_board_ek_ra8d2_touch.c`` both ways:
 *
 *   - the null-descriptor guard;
 *   - the `max_points` range guard;
 *   - propagation of an I3C bring-up failure, forced by arming the host MMIO
 *     fault seam on MSTPCRB so the module-stop read-back never settles;
 *   - both arms of the "0 means the board default" contact-count selection,
 *     each of which reaches the driver open at the end of the unit.
 *
 * The remaining error returns (the CGC read, the bus bind, the ops
 * projection) are marked GCOVR_EXCL in the unit itself: each rejects only a
 * null pointer or an out-of-range channel, and every argument at those call
 * sites is a compile-time constant of this module.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_board_ek_ra8d2.h"
#include "ra8_board_ek_ra8d2_touch.h"
#include "ra8_cgc.h"
#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_fake_mmio.h"
#include "ra8_mstp_regs.h"
#include "ra8_pin_validator.h"
#include "ra8_system_regs.h"
#include "ra8_touch.h"
#include "unity_minimal.h"

/**
 * @enum board_touch_fixture_t
 * @brief Fixed inputs the touch tests need, named so no numeric literal
 *        appears in a test body.
 */
typedef enum : uint8_t {
  k_bt_oscsf_all_ready = 0xFFU, /**< Every oscillator-stabilisation flag set, so
                                 *   ra8_cgc_init settles on its first iteration. */
  k_bt_points_over_cap =
    (uint8_t)k_ra8_touch_max_points + 1U, /**< One past the GT911's capacity. */
  k_bt_points_single = 1U,                /**< Single-contact tap detection.  */
} board_touch_fixture_t;

/**
 * @brief Clear every fake peripheral window, pin claim and touch-driver state.
 *
 * @details
 * The touch driver refuses a second open, and the module-stop layer
 * reference-counts, so each test starts from a zeroed register window with the
 * driver closed. ``ra8_touch_close`` is called unconditionally and its result
 * discarded: it legitimately reports "not open" for a test whose open failed.
 *
 * @pre None.
 * @post The fake register windows are zeroed and the touch driver is closed.
 * @note Not thread-safe; single-threaded test context only.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_reset_state(void)
{
  (void)ra8_touch_close();
  ra8_fake_mmap_reset();
  ra8_fake_mmio_reset();
  ra8_pin_validator_reset();
}

/**
 * @brief Publish a post-PLL clock tree on the fake CGC registers.
 *
 * @details
 * Seeds OSCSF so every oscillator spin-loop settles at once, then runs
 * ``ra8_cgc_init`` so the bit-rate solve in the unit under test reads the real
 * PCLKA rather than the MOCO reset default.
 *
 * @pre ::internal_reset_state has been called.
 * @post ``ra8_cgc_get_clock_hz(k_ra8_clock_id_pclka, ...)`` reports the PLL1
 *       rate.
 * @note Not thread-safe; single-threaded test context only.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_publish_clocks(void)
{
  /* HUM Ch 11.2.16 "OSCSF : Oscillation Stabilization Flag Register" -- the
   * host CGC spin-loops read this window; all-ones satisfies each at once. */
  *ra8_sys_oscsf() = (uint8_t)k_bt_oscsf_all_ready;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_cgc_init());
}

/**
 * @test board_touch_open_rejects_null_cfg
 *
 * @brief Verify a null descriptor is refused before any bus is touched.
 *
 * @return Nothing.
 * @pre The board touch unit is linked into the test binary.
 * @post No peripheral or driver state is modified.
 * @note Single-threaded host test.
 * @since 0.1.0
 *
 * @par MC/DC:
 * Decision: `if ((cfg) == nullptr)` inside `RA8_CHECK_NULL_PTR` (1 condition).
 * - Vector 1: cfg=NULL  -> true  (this test)
 * - Vector 2: cfg=&cfg  -> false (every other test in this file)
 * N+1 = 2 vectors for N=1: minimal MC/DC; the pair proves the descriptor
 * pointer alone controls the return.
 */
RA8_INTERNAL static void internal_test_rejects_null_cfg(void)
{
  TEST_BEGIN("board_touch_open: null descriptor rejected");
  internal_reset_state();
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_board_touch_open(nullptr));
  TEST_END("board_touch_open: null descriptor rejected");
}

/**
 * @test board_touch_open_rejects_too_many_points
 *
 * @brief Verify a contact cap above the GT911's capacity is refused.
 *
 * @details
 * The driver would clamp silently. The board call rejects instead, because a
 * caller asking for six contacts on a five-contact controller has a bug the
 * clamp would hide.
 *
 * @return Nothing.
 * @pre The board touch unit is linked into the test binary.
 * @post No peripheral or driver state is modified.
 * @note Single-threaded host test.
 * @since 0.1.0
 *
 * @par MC/DC:
 * Decision: `if (cfg->max_points > k_ra8_touch_max_points)` (1 condition).
 * - Vector 1: max_points=6 -> true  (this test)
 * - Vector 2: max_points=1 -> false (the default/explicit tests below)
 * N+1 = 2 vectors for N=1: minimal MC/DC.
 */
RA8_INTERNAL static void internal_test_rejects_too_many_points(void)
{
  TEST_BEGIN("board_touch_open: contact cap above the controller capacity rejected");
  internal_reset_state();
  const ra8_board_touch_cfg_t cfg = {.max_points = (uint8_t)k_bt_points_over_cap,
                                     .irq_pin    = (uint8_t)k_ra8_touch_irq_pin_unset};
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_board_touch_open(&cfg));
  TEST_END("board_touch_open: contact cap above the controller capacity rejected");
}

/**
 * @test board_touch_open_propagates_bus_bringup_failure
 *
 * @brief Verify an I3C bring-up failure is propagated, not swallowed.
 *
 * @details
 * Arms the host MMIO fault seam on the module-stop control register so the
 * I3C block's ungate read-back never settles. ``ra8_i3c_init`` therefore
 * reports a hardware timeout, which the board call must return unchanged
 * rather than continuing to open a driver over a dead bus.
 *
 * @return Nothing.
 * @pre ::internal_reset_state has been called; the clock tree is published.
 * @post The touch driver is left closed.
 * @note Single-threaded host test.
 * @since 0.1.0
 *
 * @par MC/DC:
 * Decision: `if (bus_err != k_ra8_ok)` after the I3C bring-up (1 condition).
 * - Vector 1: bus_err=k_ra8_err_hw_timeout -> true  (this test)
 * - Vector 2: bus_err=k_ra8_ok             -> false (the two tests below)
 * N+1 = 2 vectors for N=1: minimal MC/DC.
 */
RA8_INTERNAL static void internal_test_propagates_bus_bringup_failure(void)
{
  TEST_BEGIN("board_touch_open: I3C bring-up failure propagated");
  internal_reset_state();
  internal_publish_clocks();
  /* HUM Ch 11.2.7 "MSTPCRB : Module Stop Control Register B" p 444 -- the
   * ungate path polls this register for its read-back; arm the fault seam on
   * it so the read-back never settles. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fake_mmio_fail_wait(&ra8_mstp()->MSTPCRB));
  const ra8_board_touch_cfg_t cfg = {.max_points = (uint8_t)k_bt_points_single,
                                     .irq_pin    = (uint8_t)k_ra8_touch_irq_pin_unset};
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout, ra8_board_touch_open(&cfg));
  TEST_END("board_touch_open: I3C bring-up failure propagated");
}

/**
 * @test board_touch_open_reaches_the_driver
 *
 * @brief Verify both contact-cap arms carry through to the driver open.
 *
 * @details
 * Runs the call twice with a live bus: once with `max_points = 0`, which
 * selects the board default, and once with an explicit single contact. Neither
 * can succeed on the host -- no GT911 answers the modelled bus, so the driver's
 * product-id probe reports a bring-up failure -- but that verdict is exactly
 * the propagation being checked: the board call ran the whole sequence and
 * handed back the driver's own code. The two runs together drive both arms of
 * the default-selection expression.
 *
 * @return Nothing.
 * @pre ::internal_reset_state has been called; the clock tree is published.
 * @post The touch driver is left closed.
 * @note Single-threaded host test.
 * @since 0.1.0
 *
 * @par MC/DC:
 * Decision: `(cfg->max_points == 0U) ? default : cfg->max_points` (1 condition).
 * - Vector 1: max_points=0 -> true  (first half of this test)
 * - Vector 2: max_points=1 -> false (second half of this test)
 * N+1 = 2 vectors for N=1: minimal MC/DC. Both vectors reach the driver open,
 * so the same pair also supplies the false vector for the bring-up guard above.
 */
RA8_INTERNAL static void internal_test_reaches_the_driver(void)
{
  TEST_BEGIN("board_touch_open: both contact-cap arms reach the driver");
  internal_reset_state();
  internal_publish_clocks();
  const ra8_board_touch_cfg_t board_default = {.max_points = 0U,
                                               .irq_pin    = (uint8_t)k_ra8_touch_irq_pin_unset};
  TEST_ASSERT_EQ(k_ra8_err_hw_init_failed, ra8_board_touch_open(&board_default));

  internal_reset_state();
  internal_publish_clocks();
  const ra8_board_touch_cfg_t explicit_cap = {.max_points = (uint8_t)k_bt_points_single,
                                              .irq_pin    = (uint8_t)k_ra8_touch_irq_pin_unset};
  TEST_ASSERT_EQ(k_ra8_err_hw_init_failed, ra8_board_touch_open(&explicit_cap));
  TEST_END("board_touch_open: both contact-cap arms reach the driver");
}

/**
 * @brief Test binary entry point.
 *
 * @details
 * Each test resets the fake register windows and closes the touch driver
 * first, so ordering carries no hidden dependency.
 *
 * @return 0 on success; a failing assertion exits before this returns.
 *
 * @pre The fake register windows are mapped by the test framework.
 * @post Every non-excluded decision in the board touch unit has been driven
 *       both ways.
 * @note Not thread-safe; single-threaded test runner.
 * @since 0.1.0
 */
int main(void)
{
  internal_test_rejects_null_cfg();
  internal_test_rejects_too_many_points();
  internal_test_propagates_bus_bringup_failure();
  internal_test_reaches_the_driver();
  return 0;
}
