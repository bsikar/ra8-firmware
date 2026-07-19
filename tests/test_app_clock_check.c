/**
 * @file test_app_clock_check.c
 * @brief Integration test: CGC bring-up + clock readback + LED blink for clock_check
 *
 * @details
 * Mirrors examples/ek_ra8d2/clock_check/main.c, which sequences
 * ra8_cgc_init -> ra8_cgc_get_clock_hz(CPUCLK0) -> ra8_time_init(hz) ->
 * ra8_board_led_init x 3 -> ra8_board_led_toggle x 3 inside a loop.
 * Replays each transition on the host-mocked HAL.
 *
 * Exercised modules:
 *   - ra8_cgc            (CGC bring-up + per-clock readback)
 *   - ra8_time           (SysTick init at the post-PLL clock)
 *   - ra8_board_ek_ra8d2 (LED bring-up + toggle)
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_board_ek_ra8d2.h"
#include "ra8_cgc.h"
#include "ra8_err.h"
#include "ra8_pin_validator.h"
#include "ra8_sim_mmap.h"
#include "ra8_system_regs.h"
#include "ra8_time.h"
#include "unity_minimal.h"

/**
 * @enum clock_check_uint8_const_t
 * @brief Named uint8_t constants used by this file.
 *
 * @details
 * Every literal this translation unit needs, named so the
 * value's role is visible at the point of use (CLAUDE.md
 * "No Magic Numbers").
 */
typedef enum : uint8_t {
  k_sys_oscsf_all_ready =
    0xFFU, /**< Every oscillator-stabilisation flag set, so clock bring-up sees all sources ready. */
} clock_check_uint8_const_t;

/** @brief Per-test enums. */
typedef enum : uint32_t {
  k_test_cc_clock_id_bad = 200U, /**< Out-of-range clock id. */
} test_cc_const_t;

/**
 * @brief Per-test fixture reset.
 */
static void reset_world(void)
{
  ra8_sim_mmap_reset();
  ra8_pin_validator_reset();
  /* Pre-seed OSCSF stabilisation bits so ra8_cgc_init() spin loops
   * complete on the first iteration in RA8_SIMULATOR_MODE. */
  *ra8_sys_oscsf() = (uint8_t)k_sys_oscsf_all_ready;
}

/**
 * @brief CGC init -- the first step main() runs.
 *
 * @par MC/DC: not applicable -- single sequential ra8_cgc_init call,
 * no compound boolean decision in the path under test.
 */
static void test_cc_cgc_init_ok(void)
{
  reset_world();
  TEST_BEGIN("clock_check: ra8_cgc_init succeeds");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_cgc_init());
  TEST_END("clock_check: ra8_cgc_init succeeds");
}

/**
 * @brief Read CPUCLK0 back -- the value the app feeds to ra8_time_init.
 *
 * @par MC/DC:
 * Compound decision under test (in app): ``ra8_cgc_get_clock_hz(...) != ok``
 * combined with the implicit ``hz != 0`` invariant the caller relies on.
 * Two atomic conditions x N+1 = 3 vectors; this case covers both-ok.
 * Failure vectors covered in test_cc_get_clock_hz_invalid_id and
 * test_cc_get_clock_hz_null_out_rejected.
 */
static void test_cc_readback_cpuclk0(void)
{
  reset_world();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_cgc_init());
  TEST_BEGIN("clock_check: get_clock_hz(CPUCLK0) returns non-zero");
  uint32_t hz = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &hz));
  TEST_ASSERT(hz > 0U);
  TEST_END("clock_check: get_clock_hz(CPUCLK0) returns non-zero");
}

/**
 * @brief Programme SysTick at the readback clock -- third step.
 *
 * @par MC/DC: not applicable -- sequential cgc_init -> readback ->
 * time_init wiring with no compound decisions beyond the per-call
 * return-checks already covered.
 */
static void test_cc_systick_init_at_cpuclk0(void)
{
  reset_world();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_cgc_init());
  uint32_t hz = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &hz));
  TEST_BEGIN("clock_check: ra8_time_init at post-PLL CPUCLK0");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_time_init(hz));
  TEST_END("clock_check: ra8_time_init at post-PLL CPUCLK0");
}

/**
 * @brief Pins-init + toggle-all loop body.
 *
 * @par MC/DC:
 * Compound decision under test (app): ``init(LED1) || init(LED2) ||
 * init(LED3) != ok``. Three atomic conditions x N+1 = 4 vectors; this
 * case covers all-ok. Failure vectors are exercised by
 * test_blink_hal_init_invalid_led_rejected (sibling test).
 */
static void test_cc_pins_init_and_toggle_all(void)
{
  reset_world();
  TEST_BEGIN("clock_check: init + toggle all three LEDs");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_board_led_init(k_ra8_board_led1));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_board_led_init(k_ra8_board_led2));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_board_led_init(k_ra8_board_led3));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_board_led_toggle(k_ra8_board_led1));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_board_led_toggle(k_ra8_board_led2));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_board_led_toggle(k_ra8_board_led3));
  TEST_END("clock_check: init + toggle all three LEDs");
}

/**
 * @brief Out-of-range clock id is rejected by ra8_cgc_get_clock_hz.
 *
 * @par MC/DC:
 * Decision vector under test: ``id >= k_ra8_clock_id_count`` guard
 * inside ra8_cgc_get_clock_hz. Failure vector pairing with the ok
 * readback test for full N+1 coverage.
 */
static void test_cc_get_clock_hz_invalid_id(void)
{
  reset_world();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_cgc_init());
  TEST_BEGIN("clock_check: get_clock_hz rejects bad id");
  uint32_t hz = 0U;
  TEST_ASSERT(ra8_cgc_get_clock_hz((ra8_clock_id_t)k_test_cc_clock_id_bad, &hz) != k_ra8_ok);
  TEST_END("clock_check: get_clock_hz rejects bad id");
}

/**
 * @brief NULL out-pointer is rejected by ra8_cgc_get_clock_hz.
 *
 * @par MC/DC:
 * Decision vector under test: ``out_hz == nullptr`` guard inside
 * ra8_cgc_get_clock_hz. Failure vector pairing with the ok readback.
 */
static void test_cc_get_clock_hz_null_out_rejected(void)
{
  reset_world();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_cgc_init());
  TEST_BEGIN("clock_check: get_clock_hz rejects NULL out");
  TEST_ASSERT(ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, nullptr) != k_ra8_ok);
  TEST_END("clock_check: get_clock_hz rejects NULL out");
}

int main(void)
{
  test_cc_cgc_init_ok();
  test_cc_readback_cpuclk0();
  test_cc_systick_init_at_cpuclk0();
  test_cc_pins_init_and_toggle_all();
  test_cc_get_clock_hz_invalid_id();
  test_cc_get_clock_hz_null_out_rejected();
  return 0;
}
