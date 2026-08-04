/**
 * @file test_app_blink_hal.c
 * @brief Integration test: HAL-driven LED bring-up + toggle loop for blink_hal
 *
 * @details
 * Mirrors examples/ek_ra8d2/blink_hal/main.c which calls ra8_time_init,
 * iterates ra8_board_led_init across all three EK-RA8D2 LEDs, then runs
 * ra8_board_led_toggle on each in a steady-state loop. Replays the
 * exact init order against the host-mocked HAL.
 *
 * Exercised modules:
 *   - ra8_board_ek_ra8d2 (LED bring-up + toggle)
 *   - ra8_time           (SysTick init at the reset-default MOCO clock)
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
#include "ra8_time.h"
#include "unity_minimal.h"

/** @brief Per-test enums replacing magic numbers. */
typedef enum : uint32_t {
  k_test_blink_hal_cpu_hz_reset = 8400000U, /**< MOCO ~8.4 MHz at reset.      */
  k_test_blink_hal_iters        = 3U,       /**< Toggle-loop iteration count. */
} test_blink_hal_const_t;

/**
 * @brief Per-test fixture reset.
 */
static void reset_world(void)
{
  ra8_fake_mmap_reset();
  ra8_pin_validator_reset();
}

/**
 * @brief Bring up SysTick at the reset-default MOCO clock.
 *
 * @par MC/DC: not applicable -- this test exercises a single sequential
 * ra8_time_init call with no compound boolean decisions in the path.
 */
static void test_blink_hal_time_init_at_moco(void)
{
  reset_world();
  TEST_BEGIN("blink_hal: ra8_time_init at MOCO");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_time_init((uint32_t)k_test_blink_hal_cpu_hz_reset));
  TEST_END("blink_hal: ra8_time_init at MOCO");
}

/**
 * @brief Init every user LED -- the loop body main() runs.
 *
 * @par MC/DC:
 * Compound decision under test (in app): ``ra8_board_led_init(i) != ok``
 * inside the per-LED loop. One atomic condition x N+1 = 2 vectors;
 * this case covers the all-ok vector (every LED accepted).
 */
static void test_blink_hal_init_all_three_leds(void)
{
  reset_world();
  TEST_BEGIN("blink_hal: init LED1 + LED2 + LED3");
  for (uint8_t i = 0U; i < (uint8_t)k_ra8_board_led_count; ++i) {
    TEST_ASSERT_EQ(k_ra8_ok, ra8_board_led_init((ra8_board_led_id_t)i));
  }
  TEST_END("blink_hal: init LED1 + LED2 + LED3");
}

/**
 * @brief Toggle every LED in a steady-state loop.
 *
 * @par MC/DC:
 * Compound decision under test: ``ra8_board_led_toggle(i) != ok`` inside
 * the per-LED loop. One atomic condition x N+1 = 2 vectors; this case
 * covers the all-ok vector. Failure vector is in test_blink_hal_toggle_invalid.
 */
static void test_blink_hal_toggle_all_three_leds_loop(void)
{
  reset_world();
  TEST_BEGIN("blink_hal: toggle LED1+LED2+LED3 loop");
  for (uint8_t i = 0U; i < (uint8_t)k_ra8_board_led_count; ++i) {
    TEST_ASSERT_EQ(k_ra8_ok, ra8_board_led_init((ra8_board_led_id_t)i));
  }
  for (uint8_t k = 0U; k < (uint8_t)k_test_blink_hal_iters; ++k) {
    for (uint8_t i = 0U; i < (uint8_t)k_ra8_board_led_count; ++i) {
      TEST_ASSERT_EQ(k_ra8_ok, ra8_board_led_toggle((ra8_board_led_id_t)i));
    }
  }
  TEST_END("blink_hal: toggle LED1+LED2+LED3 loop");
}

/**
 * @brief Out-of-range LED id during init must be rejected (panic-halt path).
 *
 * @par MC/DC:
 * Decision vector under test: ``led_id >= k_ra8_board_led_count`` guard
 * inside ra8_board_led_init. Failure vector pairing with the all-ok init
 * test for full coverage of the precondition guard.
 */
static void test_blink_hal_init_invalid_led_rejected(void)
{
  reset_world();
  TEST_BEGIN("blink_hal: init rejects out-of-range LED id");
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_board_led_init((ra8_board_led_id_t)k_ra8_board_led_count));
  TEST_END("blink_hal: init rejects out-of-range LED id");
}

/**
 * @brief Out-of-range LED id during toggle is also rejected.
 *
 * @par MC/DC:
 * Decision vector under test: ``led_id >= k_ra8_board_led_count`` guard
 * inside ra8_board_led_toggle. Failure vector pairing with the all-ok
 * toggle loop.
 */
static void test_blink_hal_toggle_invalid(void)
{
  reset_world();
  TEST_BEGIN("blink_hal: toggle rejects out-of-range LED id");
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_board_led_toggle((ra8_board_led_id_t)k_ra8_board_led_count));
  TEST_END("blink_hal: toggle rejects out-of-range LED id");
}

/**
 * @brief Full bring-up sequence end-to-end (the exact main() flow).
 *
 * @par MC/DC: not applicable -- this test exercises sequential
 * init/teardown (time -> pins -> toggle); no compound boolean decisions
 * in the path beyond per-call return-checks already covered above.
 */
static void test_blink_hal_full_main_sequence(void)
{
  reset_world();
  TEST_BEGIN("blink_hal: full main() sequence");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_time_init((uint32_t)k_test_blink_hal_cpu_hz_reset));
  for (uint8_t i = 0U; i < (uint8_t)k_ra8_board_led_count; ++i) {
    TEST_ASSERT_EQ(k_ra8_ok, ra8_board_led_init((ra8_board_led_id_t)i));
  }
  for (uint8_t i = 0U; i < (uint8_t)k_ra8_board_led_count; ++i) {
    TEST_ASSERT_EQ(k_ra8_ok, ra8_board_led_toggle((ra8_board_led_id_t)i));
  }
  TEST_END("blink_hal: full main() sequence");
}

int main(void)
{
  test_blink_hal_time_init_at_moco();
  test_blink_hal_init_all_three_leds();
  test_blink_hal_toggle_all_three_leds_loop();
  test_blink_hal_init_invalid_led_rejected();
  test_blink_hal_toggle_invalid();
  test_blink_hal_full_main_sequence();
  return 0;
}
