/**
 * @file test_ra8_board_ek_ra8d2_bringup.c
 * @brief Unit tests for the EK-RA8D2 substrate prologue facade
 *
 * @details
 * Exercises ra8_board_bringup() over the fake register map: both null
 * guards, the leds_mask validation, propagation of a clock-tree failure
 * with the caller's output left untouched, the full happy path with
 * console + every LED + interrupts, the console-skipped path, and a
 * pre-claimed LED pin so the LED loop's failure arm is propagated.
 *
 * Test ordering is significant: the console-skipped case must run
 * before any successful console init in this binary, because the
 * console's "is it up" flag is module state that never clears.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_board_ek_ra8d2.h"
#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_fake_mmio.h"
#include "ra8_pin_validator.h"
#include "ra8_port_constants.h"
#include "ra8_system_regs.h"
#include "ra8_time.h"
#include "ra8_time_constants.h"
#include "unity_minimal.h"

/**
 * @enum board_bringup_fixture_t
 * @brief Fixture constants: the all-ready oscillator byte and the poison written into an output before a call.
 */
typedef enum : uint32_t {
  k_bringup_oscsf_all_ready = 0xFFU,   /**< Every oscillator flag reported stable. */
  k_bringup_console_baud    = 115200U, /**< J-Link OB VCOM rate the examples use.  */
  k_bringup_poison_hz       = 0xDEADU, /**< Poison in an out-parameter rate field. */
} board_bringup_fixture_t;

/* -------------------------------------------------------------------------
 * Helpers
 * -------------------------------------------------------------------------
 */

/**
 * @brief Reset fake register state, armed MMIO faults, and pin ownership.
 * @return Nothing.
 * @pre None.
 * @post Register window cleared, fault table empty, pin bitmap zeroed.
 * @note Not thread-safe; single-threaded test context only.
 * @since 0.1.0
 */
static void reset_state(void)
{
  ra8_fake_mmap_reset();
  ra8_fake_mmio_reset();
  ra8_pin_validator_reset();
}

/**
 * @brief Make every oscillator report stable so the clock tree comes up.
 * @return Nothing.
 * @pre ra8_fake_mmap_reset() has been called (OSCSF is writable memory).
 * @post The CGC spin-loops resolve on their first iteration.
 * @note Not thread-safe; single-threaded test context only.
 * @since 0.1.0
 */
static void oscsf_preseed(void)
{
  *ra8_sys_oscsf() = (uint8_t)k_bringup_oscsf_all_ready;
}

/* -------------------------------------------------------------------------
 * 1. Argument guards
 * -------------------------------------------------------------------------
 */

/**
 * @test test_bringup_rejects_null_cfg
 * @brief A null config is refused before any hardware is touched.
 * @par MC/DC:
 * Decision: `(cfg == nullptr) || (out == nullptr)` (2 conditions).
 * - Vector A: cfg null, out nonnull -> true (this test).
 * - Vector B: cfg nonnull, out null -> true (next test).
 * - Vector C: both nonnull          -> false (the happy-path tests).
 * N+1 = 3 vectors; each condition independently flips the outcome.
 * @pre None.
 * @post No clock, module-stop, or pin state changes.
 * @note Single-threaded host test.
 * @since 0.1.0
 */
static void test_bringup_rejects_null_cfg(void)
{
  TEST_BEGIN("bringup: null cfg rejected");
  reset_state();
  ra8_board_bringup_out_t out = {};
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_board_bringup(nullptr, &out));
  TEST_END("bringup: null cfg rejected");
}

/**
 * @test test_bringup_rejects_null_out
 * @brief A null output pointer is refused before any hardware is touched.
 * @par MC/DC: Vector B of the guard documented on the previous test.
 * @pre None.
 * @post No clock, module-stop, or pin state changes.
 * @note Single-threaded host test.
 * @since 0.1.0
 */
static void test_bringup_rejects_null_out(void)
{
  TEST_BEGIN("bringup: null out rejected");
  reset_state();
  const ra8_board_bringup_cfg_t cfg = {};
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_board_bringup(&cfg, nullptr));
  TEST_END("bringup: null out rejected");
}

/**
 * @test test_bringup_rejects_unknown_led_bit
 * @brief A leds_mask bit outside the three user LEDs is refused.
 * @details Catches the ordinal-for-mask mistake: passing
 *          k_ra8_board_led3 (2) would ask for LED2 rather than LED3, and
 *          any bit above bit 2 names no LED at all.
 * @par MC/DC:
 * Decision: `(leds_mask & ~leds_all) != 0U` (1 condition).
 * - Vector A: a bit outside the mask -> true (this test).
 * - Vector B: only known bits        -> false (the happy-path tests).
 * @pre None.
 * @post No clock, module-stop, or pin state changes.
 * @note Single-threaded host test.
 * @since 0.1.0
 */
static void test_bringup_rejects_unknown_led_bit(void)
{
  TEST_BEGIN("bringup: unknown leds_mask bit rejected");
  reset_state();
  const ra8_board_bringup_cfg_t cfg = {
    .leds_mask = (uint32_t)k_ra8_board_bringup_leds_all + 1U,
  };
  ra8_board_bringup_out_t out = {};
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_board_bringup(&cfg, &out));
  TEST_END("bringup: unknown leds_mask bit rejected");
}

/* -------------------------------------------------------------------------
 * 2. Clock-tree failure
 * -------------------------------------------------------------------------
 */

/**
 * @test test_bringup_propagates_clock_failure
 * @brief An oscillator timeout is returned and the output left untouched.
 * @par MC/DC:
 * Decision: `if (err != k_ra8_ok)` after step 1 (1 condition).
 * - Vector A: the oscillator wait fails -> true (this test).
 * - Vector B: the tree comes up         -> false (the happy-path tests).
 * @pre The fake oscillator-stable wait is armed to fail.
 * @post The caller's rates are exactly as passed in.
 * @note Single-threaded host test.
 * @since 0.1.0
 */
static void test_bringup_propagates_clock_failure(void)
{
  TEST_BEGIN("bringup: clock failure propagated, out untouched");
  reset_state();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fake_mmio_fail_wait(ra8_sys_oscsf()));
  const ra8_board_bringup_cfg_t cfg = {};
  ra8_board_bringup_out_t       out = {
          .cpuclk0_hz = (uint32_t)k_bringup_poison_hz,
          .pclka_hz   = (uint32_t)k_bringup_poison_hz,
  };
  TEST_ASSERT(ra8_board_bringup(&cfg, &out) != k_ra8_ok);
  TEST_ASSERT_EQ(k_bringup_poison_hz, out.cpuclk0_hz);
  TEST_ASSERT_EQ(k_bringup_poison_hz, out.pclka_hz);
  TEST_END("bringup: clock failure propagated, out untouched");
}

/* -------------------------------------------------------------------------
 * 3. Console-skipped path (must precede any successful console init)
 * -------------------------------------------------------------------------
 */

/**
 * @test test_bringup_zero_baud_skips_console
 * @brief console_baud == 0 brings the clocks and timebase up, no console.
 * @par MC/DC:
 * Decision: `if (cfg->console_baud != 0U)` (1 condition).
 * - Vector A: baud == 0 -> false, console skipped (this test).
 * - Vector B: baud != 0 -> true, console brought up (the next test).
 * @pre No successful console init has run in this binary yet.
 * @post The console reports not-initialized; the tick source is live.
 * @note Single-threaded host test.
 * @since 0.1.0
 */
static void test_bringup_zero_baud_skips_console(void)
{
  TEST_BEGIN("bringup: zero baud skips the console");
  reset_state();
  oscsf_preseed();
  const ra8_board_bringup_cfg_t cfg = {};
  ra8_board_bringup_out_t       out = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_board_bringup(&cfg, &out));
  TEST_ASSERT_EQ(k_ra8_cpuclk0_hz, out.cpuclk0_hz);
  TEST_ASSERT_EQ(k_ra8_pclka_hz, out.pclka_hz);
  /* No console was requested, so writing to it is still refused. */
  const uint8_t byte = 0U;
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_board_uart_console_write(&byte, 1U));
  TEST_END("bringup: zero baud skips the console");
}

/* -------------------------------------------------------------------------
 * 4. LED failure arm
 * -------------------------------------------------------------------------
 */

/**
 * @test test_bringup_propagates_led_conflict
 * @brief A pre-claimed LED pin fails the LED step and is propagated.
 * @par MC/DC:
 * Decision: `if (err != k_ra8_ok)` inside the LED loop (1 condition).
 * - Vector A: the pin is owned -> true (this test).
 * - Vector B: the pin is free  -> false (the happy-path test).
 * @pre LED1's pin is claimed by another owner.
 * @post The conflict code is returned unchanged.
 * @note Single-threaded host test.
 * @since 0.1.0
 */
static void test_bringup_propagates_led_conflict(void)
{
  TEST_BEGIN("bringup: LED pin conflict propagated");
  reset_state();
  oscsf_preseed();
  ra8_port_pin_t led1_pin = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_board_led_pin(k_ra8_board_led1, &led1_pin));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_pin_validator_claim(led1_pin, "test.led1.conflict"));
  const ra8_board_bringup_cfg_t cfg = {
    .leds_mask = (uint32_t)k_ra8_board_bringup_led1,
  };
  ra8_board_bringup_out_t out = {};
  TEST_ASSERT_EQ(k_ra8_err_gpio_conflict, ra8_board_bringup(&cfg, &out));
  TEST_END("bringup: LED pin conflict propagated");
}

/* -------------------------------------------------------------------------
 * 5. Full prologue
 * -------------------------------------------------------------------------
 */

/**
 * @test test_bringup_full_prologue
 * @brief Console, all three LEDs and interrupts come up and rates publish.
 * @details This is the block the examples hand-write: the same seven
 *          steps, in the order the substrate requires, behind one call.
 * @par MC/DC:
 * Supplies the false vector for every guard in the function and the true
 * vector for `if (cfg->enable_interrupts)`; the preceding tests supply
 * the complements.
 * @pre Every oscillator reports stable and no LED pin is owned.
 * @post The console accepts bytes and each LED pin is owned and low.
 * @note Single-threaded host test.
 * @since 0.1.0
 */
static void test_bringup_full_prologue(void)
{
  TEST_BEGIN("bringup: full prologue, console + LEDs + interrupts");
  reset_state();
  oscsf_preseed();
  const ra8_board_bringup_cfg_t cfg = {
    .console_baud      = (uint32_t)k_bringup_console_baud,
    .leds_mask         = (uint32_t)k_ra8_board_bringup_leds_all,
    .enable_interrupts = true,
  };
  ra8_board_bringup_out_t out = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_board_bringup(&cfg, &out));
  TEST_ASSERT_EQ(k_ra8_cpuclk0_hz, out.cpuclk0_hz);
  TEST_ASSERT_EQ(k_ra8_pclka_hz, out.pclka_hz);
  /* The console is up: a write is accepted rather than refused. */
  const uint8_t byte = 0x41U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_board_uart_console_write(&byte, 1U));
  /* Every LED pin is owned, so a fresh claim on each is refused. */
  for (uint8_t led = 0U; led < (uint8_t)k_ra8_board_led_count; ++led) {
    ra8_port_pin_t pin = 0U;
    TEST_ASSERT_EQ(k_ra8_ok, ra8_board_led_pin((ra8_board_led_id_t)led, &pin));
    TEST_ASSERT(ra8_pin_validator_claim(pin, "test.led.after") != k_ra8_ok);
  }
  TEST_END("bringup: full prologue, console + LEDs + interrupts");
}

/* -------------------------------------------------------------------------
 * Entry point
 * -------------------------------------------------------------------------
 */

/**
 * @brief Test binary entry point.
 * @return 0 on success; a failing assertion exits non-zero first.
 * @pre The fake register window is allocated by the test framework.
 * @post Every case above has run in order.
 * @note Not thread-safe; single-threaded test runner.
 * @since 0.1.0
 */
int main(void)
{
  test_bringup_rejects_null_cfg();
  test_bringup_rejects_null_out();
  test_bringup_rejects_unknown_led_bit();
  test_bringup_propagates_clock_failure();
  test_bringup_zero_baud_skips_console();
  test_bringup_propagates_led_conflict();
  test_bringup_full_prologue();
  return 0;
}
