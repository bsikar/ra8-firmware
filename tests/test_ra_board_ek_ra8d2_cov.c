/**
 * @file test_ra_board_ek_ra8d2_cov.c
 * @brief Coverage-boosting unit tests for the EK-RA8D2 v1 board-support layer
 *
 * @details
 * Companion to test_ra_board_ek_ra8d2.c.  Exercises the error-return
 * branches and alternate switch arms that the primary test deliberately
 * omitted (it favoured API shape over branch depth).  Every test in
 * this file is deterministic: register-window state is reset via
 * ra_sim_mmap_reset() + ra_pin_validator_reset() before each claim
 * sequence, so no ordering dependency exists between this binary and
 * the primary test binary.
 *
 * Lines targeted (84.8% -> >=90%):
 *   155  -- ra_board_sw_pin null out_pin early return
 *   169  -- ra_board_sw_init invalid sw id propagation
 *   576-582 -- ra_board_glcdc_init rgb666 / rgb565 arms
 *   593  -- ra_board_glcdc_init conflict return
 *   629  -- ra_board_lcd_panel_power_on conflict return
 *   714  -- ra_board_xspi_pins_init gpio conflict return
 *   733  -- ra_board_xspi_pins_init pfs conflict return
 *   773-787 -- ra_board_sdhi_pins_init (all lines)
 *   798, 800, 802 -- ra_board_arduino_pin_init valid modes
 *
 * Lines marked GCOVR_EXCL_LINE in the source (HW-only paths):
 *   187, 213, 487, 558, 634, 721
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra_board_ek_ra8d2.h"
#include "ra_err.h"
#include "ra_pin_validator.h"
#include "ra_port_constants.h"
#include "ra_sim_mmap.h"
#include "unity_minimal.h"

/* -------------------------------------------------------------------------
 * Helpers
 * -------------------------------------------------------------------------
 */

/**
 * @brief Reset all simulated peripheral state and pin ownership.
 *
 * @details
 * Mirrors reset_board_hal_state() in test_ra_board_ek_ra8d2.c.  Called
 * at the start of every test that makes HAL pin claims to guarantee a
 * clean slate regardless of execution order within this binary.
 *
 * @pre None.
 * @post ra_sim_mmap register window cleared; pin validator bitmap zeroed.
 *
 * @note Not thread-safe; single-threaded test context only.
 * @since 0.1.0
 */
static void reset_state(void)
{
  ra_sim_mmap_reset();
  ra_pin_validator_reset();
}

/* -------------------------------------------------------------------------
 * 1. ra_board_sw_pin -- null out_pin (source line 155)
 * -------------------------------------------------------------------------
 */

/**
 * @brief Verify that ra_board_sw_pin rejects a null output pointer.
 *
 * @details
 * The existing test exercises the invalid-sw-id path and the happy path
 * but not the null-out_pin early return at source line 155.  This test
 * provides the missing vector.
 *
 * @par MC/DC:
 * (no compound decisions -- single-condition guard `out_pin == nullptr`)
 *
 * @pre ra_sim_mmap and pin validator in any state (no claims made here).
 * @post No side effects beyond the early return.
 *
 * @note Not thread-safe; single-threaded test context.
 * @since 0.1.0
 */
static void test_sw_pin_null_out_pin(void)
{
  TEST_BEGIN("sw_pin rejects null out_pin (line 155)");
  TEST_ASSERT_EQ(k_ra_err_invalid_arg, ra_board_sw_pin(k_ra_board_sw1, nullptr));
  TEST_ASSERT_EQ(k_ra_err_invalid_arg, ra_board_sw_pin(k_ra_board_sw2, nullptr));
  TEST_END("sw_pin rejects null out_pin (line 155)");
}

/* -------------------------------------------------------------------------
 * 2. ra_board_sw_init -- invalid sw id propagation (source line 169)
 * -------------------------------------------------------------------------
 */

/**
 * @brief Verify that ra_board_sw_init propagates the ra_board_sw_pin error.
 *
 * @details
 * Source line 169 is the `return err;` inside `if (err != k_ra_ok)` in
 * ra_board_sw_init.  It is only reached when the pin lookup fails
 * (invalid sw id).
 *
 * @par MC/DC:
 * (no compound decisions -- single-condition guard `err != k_ra_ok`)
 *
 * @pre None (no HAL state required).
 * @post No HAL state modified.
 *
 * @note Not thread-safe; single-threaded test context.
 * @since 0.1.0
 */
static void test_sw_init_invalid_id(void)
{
  TEST_BEGIN("sw_init propagates error for invalid sw id (line 169)");
  reset_state();
  TEST_ASSERT_EQ(k_ra_err_invalid_arg, ra_board_sw_init((ra_board_sw_id_t)99));
  TEST_END("sw_init propagates error for invalid sw id (line 169)");
}

/* -------------------------------------------------------------------------
 * 3. ra_board_glcdc_init -- rgb666 arm (source lines 576-578)
 * -------------------------------------------------------------------------
 */

/**
 * @brief Verify that ra_board_glcdc_init accepts the rgb666 format.
 *
 * @details
 * The existing test only calls glcdc_init with an out-of-range format
 * (which hits the default: case).  This test reaches the
 * k_ra_board_glcdc_fmt_rgb666 arm (source lines 576-578).
 *
 * @par MC/DC:
 * (no compound decisions -- switch dispatch only)
 *
 * @pre Clean pin-validator state (all pins free).
 * @post All rgb666 GLCDC output pins are routed to the GLCDC peripheral.
 *
 * @note Not thread-safe; single-threaded test context.
 * @since 0.1.0
 */
static void test_glcdc_init_rgb666(void)
{
  TEST_BEGIN("glcdc_init rgb666 routes pins (lines 576-578)");
  reset_state();
  TEST_ASSERT_EQ(k_ra_ok, ra_board_glcdc_init(k_ra_board_glcdc_fmt_rgb666));
  TEST_END("glcdc_init rgb666 routes pins (lines 576-578)");
}

/* -------------------------------------------------------------------------
 * 4. ra_board_glcdc_init -- rgb565 arm (source lines 580-582)
 * -------------------------------------------------------------------------
 */

/**
 * @brief Verify that ra_board_glcdc_init accepts the rgb565 format.
 *
 * @details
 * Mirrors test_glcdc_init_rgb666 but targets the rgb565 switch arm
 * at source lines 580-582.
 *
 * @par MC/DC:
 * (no compound decisions -- switch dispatch only)
 *
 * @pre Clean pin-validator state (all pins free).
 * @post All rgb565 GLCDC output pins are routed to the GLCDC peripheral.
 *
 * @note Not thread-safe; single-threaded test context.
 * @since 0.1.0
 */
static void test_glcdc_init_rgb565(void)
{
  TEST_BEGIN("glcdc_init rgb565 routes pins (lines 580-582)");
  reset_state();
  TEST_ASSERT_EQ(k_ra_ok, ra_board_glcdc_init(k_ra_board_glcdc_fmt_rgb565));
  TEST_END("glcdc_init rgb565 routes pins (lines 580-582)");
}

/* -------------------------------------------------------------------------
 * 5. ra_board_glcdc_init -- pfs conflict return (source line 593)
 * -------------------------------------------------------------------------
 */

/**
 * @brief Verify that ra_board_glcdc_init propagates a pin-conflict error.
 *
 * @details
 * Source line 593 is `return err;` inside the GLCDC output-pin routing
 * loop when ra_pfs_route_peripheral reports a conflict.  Calling
 * ra_board_glcdc_init a second time (without a pin-validator reset)
 * forces the first GLCDC output pin (TCON0) to conflict, exercising
 * that return path.
 *
 * @par MC/DC:
 * (no compound decisions -- single-condition guard `err != k_ra_ok`)
 *
 * @pre Clean pin-validator state.
 * @post GLCDC output pins remain claimed from the first call.
 *
 * @note Not thread-safe; single-threaded test context.
 * @since 0.1.0
 */
static void test_glcdc_init_conflict(void)
{
  TEST_BEGIN("glcdc_init second call returns conflict (line 593)");
  reset_state();
  /* First call: all rgb666 GLCDC output pins claimed. */
  TEST_ASSERT_EQ(k_ra_ok, ra_board_glcdc_init(k_ra_board_glcdc_fmt_rgb666));
  /* Second call: first GLCDC output pin already owned -> conflict. */
  const ra_err_t err = ra_board_glcdc_init(k_ra_board_glcdc_fmt_rgb666);
  TEST_ASSERT(err != k_ra_ok);
  TEST_END("glcdc_init second call returns conflict (line 593)");
}

/* -------------------------------------------------------------------------
 * 6. ra_board_lcd_panel_power_on -- gpio conflict return (source line 629)
 * -------------------------------------------------------------------------
 */

/**
 * @brief Verify that ra_board_lcd_panel_power_on returns conflict on retry.
 *
 * @details
 * Source line 629 is `return err;` when the first ra_gpio_output_init
 * call inside ra_board_lcd_panel_power_on fails.  After a successful
 * first call the lcd_reset_l pin (P606) is claimed; a second call
 * therefore fails at that first init attempt, covering line 629.
 *
 * @par MC/DC:
 * (no compound decisions -- single-condition guard `err != k_ra_ok`)
 *
 * @pre Clean pin-validator state.
 * @post lcd_reset_l and lcd_blen pins remain claimed from the first call.
 *
 * @note Not thread-safe; single-threaded test context.
 * @since 0.1.0
 */
static void test_lcd_panel_power_on_conflict(void)
{
  TEST_BEGIN("lcd_panel_power_on second call returns conflict (line 629)");
  reset_state();
  /* First call: claims lcd_reset_l and lcd_blen. */
  TEST_ASSERT_EQ(k_ra_ok, ra_board_lcd_panel_power_on());
  /* Second call: gpio_output_init(lcd_reset_l) fails -> line 629. */
  const ra_err_t err = ra_board_lcd_panel_power_on();
  TEST_ASSERT(err != k_ra_ok);
  TEST_END("lcd_panel_power_on second call returns conflict (line 629)");
}

/**
 * @brief Verify ra_board_backlight_set drives BLEN both on and off.
 *
 * @details
 * ra_board_backlight_set writes the BLEN (P514) output level via ra_gpio_write,
 * mapping @c true -> high (lit) and @c false -> low (blanked). BLEN is claimed
 * and configured as an output by ra_board_lcd_panel_power_on, so drive it there
 * first, then exercise both the on and off arms of the level ternary so both
 * paths of the accessor are covered.
 *
 * @par MC/DC:
 * (no compound decisions -- `on ? high : low` is a single-condition ternary)
 *
 * @pre Clean pin-validator state.
 * @post BLEN (P514) is left driven low (backlight blanked).
 *
 * @note Not thread-safe; single-threaded test context.
 * @since 0.1.0
 */
static void test_backlight_set_on_off(void)
{
  TEST_BEGIN("backlight_set drives BLEN high then low");
  reset_state();
  /* panel_power_on claims + configures BLEN (P514) as a GPIO output. */
  TEST_ASSERT_EQ(k_ra_ok, ra_board_lcd_panel_power_on());
  TEST_ASSERT_EQ(k_ra_ok, ra_board_backlight_set(true));  /* light */
  TEST_ASSERT_EQ(k_ra_ok, ra_board_backlight_set(false)); /* blank */
  TEST_END("backlight_set drives BLEN high then low");
}

/* -------------------------------------------------------------------------
 * 7. ra_board_xspi_pins_init -- gpio conflict return (source line 714)
 * -------------------------------------------------------------------------
 */

/**
 * @brief Verify that ra_board_xspi_pins_init returns conflict on retry.
 *
 * @details
 * Source line 714 is `return err;` when ra_gpio_output_init for the
 * RESET_L pin fails.  After a successful first call P106 is claimed;
 * a second call fails immediately at that gpio step.
 *
 * @par MC/DC:
 * (no compound decisions -- single-condition guard `err != k_ra_ok`)
 *
 * @pre Clean pin-validator state.
 * @post RESET_L and all 11 OCTA bus pins remain claimed from first call.
 *
 * @note Not thread-safe; single-threaded test context.
 * @since 0.1.0
 */
static void test_xspi_pins_init_gpio_conflict(void)
{
  TEST_BEGIN("xspi_pins_init second call fails at gpio step (line 714)");
  reset_state();
  /* First call: claims reset pin + all 11 OCTA bus pins. */
  TEST_ASSERT_EQ(k_ra_ok, ra_board_xspi_pins_init());
  /* Second call: gpio_output_init(reset_pin) fails -> line 714. */
  const ra_err_t err = ra_board_xspi_pins_init();
  TEST_ASSERT(err != k_ra_ok);
  TEST_END("xspi_pins_init second call fails at gpio step (line 714)");
}

/* -------------------------------------------------------------------------
 * 8. ra_board_xspi_pins_init -- pfs conflict return (source line 733)
 * -------------------------------------------------------------------------
 */

/**
 * @brief Verify that ra_board_xspi_pins_init returns conflict in the bus-pin loop.
 *
 * @details
 * Source line 733 is `return err;` inside the OCTA bus-pin routing
 * loop.  To reach it the gpio steps must succeed, which requires the
 * RESET_L pin to be free.  After a full successful init the reset pin
 * is released via ra_pin_validator_release, and then a second call is
 * made: the gpio steps now succeed but the first OCTA bus pin (CS) is
 * still claimed from the first call, causing ra_pfs_route_peripheral to
 * return a conflict and triggering line 733.
 *
 * @par MC/DC:
 * (no compound decisions -- single-condition guard `err != k_ra_ok`)
 *
 * @pre Clean pin-validator state.
 * @post RESET_L is re-claimed; OCTA bus pins remain from first call.
 *
 * @note Not thread-safe; single-threaded test context.
 * @since 0.1.0
 */
static void test_xspi_pins_init_pfs_conflict(void)
{
  TEST_BEGIN("xspi_pins_init fails in pfs loop (line 733)");
  reset_state();
  /* First call: claims reset pin + all 11 OCTA bus pins. */
  TEST_ASSERT_EQ(k_ra_ok, ra_board_xspi_pins_init());
  /* Free only the reset pin so the gpio steps succeed on the next call.
   * The 11 OCTA bus pins remain claimed from the first call. */
  /* NOLINTBEGIN(clang-analyzer-optin.core.EnumCastOutOfRange) */
  const ra_port_pin_t reset_pin = (ra_port_pin_t)k_ra_board_xspi_reset;
  /* NOLINTEND(clang-analyzer-optin.core.EnumCastOutOfRange) */
  TEST_ASSERT_EQ(k_ra_ok, ra_pin_validator_release(reset_pin));
  /* Second call: gpio steps succeed; first OCTA bus pin conflicts -> 733. */
  const ra_err_t err = ra_board_xspi_pins_init();
  TEST_ASSERT(err != k_ra_ok);
  TEST_END("xspi_pins_init fails in pfs loop (line 733)");
}

/* -------------------------------------------------------------------------
 * 9. ra_board_sdhi_pins_init -- full coverage (source lines 773-787)
 * -------------------------------------------------------------------------
 */

/**
 * @brief Verify that ra_board_sdhi_pins_init routes all eight SDHI bus pins.
 *
 * @details
 * No existing test calls ra_board_sdhi_pins_init, leaving the entire
 * function uncovered.  This test covers the happy path (lines 773-782,
 * 785-787) plus the error return (line 783) by calling the function
 * twice: the second call conflicts on the first SDHI bus pin.
 *
 * @par MC/DC:
 * Decision: `if (err != k_ra_ok)` inside the SDHI pin loop (1 condition).
 * - Vector A: err=k_ra_ok (first call, all pins free)    -> body not taken
 * - Vector B: err!=k_ra_ok (second call, first pin owned) -> body taken
 * Vectors A+B prove the single condition independently affects outcome.
 *
 * @pre Clean pin-validator state.
 * @post All eight SDHI bus pins remain claimed after the second call
 *       fails on the first pin.
 *
 * @note Not thread-safe; single-threaded test context.
 * @since 0.1.0
 */
static void test_sdhi_pins_init(void)
{
  TEST_BEGIN("sdhi_pins_init happy path then conflict (lines 773-787)");
  reset_state();
  /* First call: all eight SDHI0 bus pins routed. */
  TEST_ASSERT_EQ(k_ra_ok, ra_board_sdhi_pins_init());
  /* Second call: first bus pin already owned -> conflict (line 783). */
  const ra_err_t err = ra_board_sdhi_pins_init();
  TEST_ASSERT(err != k_ra_ok);
  TEST_END("sdhi_pins_init happy path then conflict (lines 773-787)");
}

/* -------------------------------------------------------------------------
 * 10. ra_board_arduino_pin_init -- valid mode arms (lines 798, 800, 802)
 * -------------------------------------------------------------------------
 */

/**
 * @brief Verify that ra_board_arduino_pin_init succeeds for each valid mode.
 *
 * @details
 * The existing test only exercises the default (invalid mode) arm.  This
 * test hits all three valid modes:
 *   - k_ra_board_arduino_mode_input       (line 798)
 *   - k_ra_board_arduino_mode_input_pullup (line 800)
 *   - k_ra_board_arduino_mode_output       (line 802)
 *
 * Distinct Arduino pins are used for each mode so pin-validator claims
 * do not conflict within the same test.
 *
 * @par MC/DC:
 * (no compound decisions -- switch dispatch only)
 *
 * @pre Clean pin-validator state.
 * @post D2, D3, D4 pins are claimed as input / input-pullup / output.
 *
 * @note Not thread-safe; single-threaded test context.
 * @since 0.1.0
 */
static void test_arduino_pin_init_valid_modes(void)
{
  TEST_BEGIN("arduino_pin_init valid modes (lines 798, 800, 802)");
  reset_state();
  /* input mode -> ra_gpio_input_init(pin, k_ra_pull_none) -- line 798. */
  TEST_ASSERT_EQ(k_ra_ok,
                 ra_board_arduino_pin_init(k_ra_board_arduino_d2, k_ra_board_arduino_mode_input));
  /* input_pullup mode -> ra_gpio_input_init(pin, k_ra_pull_up) -- line 800. */
  TEST_ASSERT_EQ(
    k_ra_ok,
    ra_board_arduino_pin_init(k_ra_board_arduino_d3, k_ra_board_arduino_mode_input_pullup));
  /* output mode -> ra_gpio_output_init(pin, k_ra_level_low) -- line 802. */
  TEST_ASSERT_EQ(k_ra_ok,
                 ra_board_arduino_pin_init(k_ra_board_arduino_d4, k_ra_board_arduino_mode_output));
  TEST_END("arduino_pin_init valid modes (lines 798, 800, 802)");
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
 * @pre ra_sim_mmap register window allocated by the test framework.
 * @post All targeted source lines are instrumented with gcov data.
 *
 * @note Not thread-safe; single-threaded test runner.
 * @since 0.1.0
 */
int32_t main(void)
{
  test_sw_pin_null_out_pin();
  test_sw_init_invalid_id();
  test_glcdc_init_rgb666();
  test_glcdc_init_rgb565();
  test_glcdc_init_conflict();
  test_lcd_panel_power_on_conflict();
  test_backlight_set_on_off();
  test_xspi_pins_init_gpio_conflict();
  test_xspi_pins_init_pfs_conflict();
  test_sdhi_pins_init();
  test_arduino_pin_init_valid_modes();
  (void)fprintf(stderr, "[OK ] test_ra_board_ek_ra8d2_cov.c\n");
  return 0;
}
