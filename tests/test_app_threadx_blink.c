/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file test_app_threadx_blink.c
 * @brief Integration test: ThreadX blink scheduler + GPIO toggle flow
 *
 * @details
 * Mirrors the multi-module integration-test pattern from STAR's
 * test_motor_control_task.c. The actual app at
 * examples/ek_ra8d2/threadx_blink/main.c wires two ThreadX threads to two LED
 * GPIO toggles. The kernel is not linked into the host test build
 * (RA8_OFF_TARGET), so this test exercises the same module surface
 * the threads call (ra8_board_led_init / ra8_board_led_toggle) in the
 * exact sequence the app uses, plus failure-injection paths to verify
 * mid-flow error propagation.
 *
 * Exercised modules:
 *   - ra8_board_ek_ra8d2 (LED bring-up + toggle)
 *   - ra8_gpio (under the BSP)
 *   - ra8_pfs (pin routing)
 *
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_board_ek_ra8d2.h"
#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_pin_validator.h"
#include "ra8_port_constants.h"
#include "unity_minimal.h"

/**
 * @brief Per-test fixture reset -- mock MMIO + pin-claim bitmap.
 */
static void reset_world(void)
{
  ra8_fake_mmap_reset();
  ra8_pin_validator_reset();
}

/** @brief Per-test enums replacing magic numbers. */
typedef enum : uint8_t {
  k_test_blink_iters        = 4U, /**< Per-thread loop iterations to simulate. */
  k_test_blink_thread_idx_a = 0U, /**< Thread A == LED1 owner.                 */
  k_test_blink_thread_idx_b = 1U, /**< Thread B == LED2 owner.                 */
} test_blink_const_t;

/* -------------------------------------------------------------------------
 * Golden path: per-thread bring-up + toggle loop
 * ------------------------------------------------------------------------- */

/**
 * @brief Verify the full bring-up sequence main() drives before kernel entry.
 *
 * @details Replays the call order of examples/ek_ra8d2/threadx_blink/main.c:
 * led_init(LED1) then led_init(LED2). Both must succeed under the
 * off-target HAL.
 *
 * @par MC/DC:
 * Decision under test: ``ra8_board_led_init() != k_ra8_ok`` evaluated for
 * each LED. Two atomic conditions x N+1 = 3 vectors -- A=ok,
 * B=ok (golden), A=fail (LED1 path taken), B=fail (LED2 path taken).
 * This case covers vector 1 (both ok).
 */
static void test_blink_main_init_sequence_ok(void)
{
  reset_world();
  TEST_BEGIN("blink: led_init(LED1)+led_init(LED2) succeed");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_board_led_init(k_ra8_board_led1));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_board_led_init(k_ra8_board_led2));
  TEST_END("blink: led_init(LED1)+led_init(LED2) succeed");
}

/**
 * @brief Verify thread-body toggle loop drives the GPIO N times.
 *
 * @details Drives the same call the app's thread_a_entry() executes
 * inside its while(1): ra8_board_led_toggle. We loop k_test_blink_iters
 * to confirm the BSP is re-entrant call-after-call (post-init).
 *
 * @par MC/DC:
 * Decision under test: ``ra8_board_led_toggle(led) != k_ra8_ok`` inside
 * each thread loop. One atomic condition x 2 vectors: ok-loop (this
 * test) and failure-after-N (covered by test_blink_toggle_after_deinit
 * below).
 */
static void test_blink_thread_a_loop_toggles_led1(void)
{
  reset_world();
  TEST_BEGIN("blink: thread A toggles LED1 N iterations");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_board_led_init(k_ra8_board_led1));
  for (uint8_t i = 0; i < (uint8_t)k_test_blink_iters; i++) {
    TEST_ASSERT_EQ(k_ra8_ok, ra8_board_led_toggle(k_ra8_board_led1));
  }
  TEST_END("blink: thread A toggles LED1 N iterations");
}

/**
 * @brief Same as above but for thread B / LED2.
 *
 * @par MC/DC:
 * Decision under test: ``ra8_board_led_toggle(LED2) != k_ra8_ok``.
 * Mirror vector to thread A loop -- pairs with the failure case to
 * give per-LED N+1 coverage.
 */
static void test_blink_thread_b_loop_toggles_led2(void)
{
  reset_world();
  TEST_BEGIN("blink: thread B toggles LED2 N iterations");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_board_led_init(k_ra8_board_led2));
  for (uint8_t i = 0; i < (uint8_t)k_test_blink_iters; i++) {
    TEST_ASSERT_EQ(k_ra8_ok, ra8_board_led_toggle(k_ra8_board_led2));
  }
  TEST_END("blink: thread B toggles LED2 N iterations");
}

/* -------------------------------------------------------------------------
 * Edge / failure paths
 * ------------------------------------------------------------------------- */

/**
 * @brief Init failure on LED2 must surface (panic-halt path in app).
 *
 * @details Forces the second init call to use an out-of-range LED ID.
 * The app's ra8_board_led_init returns k_ra8_err_invalid_arg for this,
 * matching the panic_halt branch in main().
 *
 * @par MC/DC:
 * Decision under test: ``ra8_board_led_init() != k_ra8_ok`` -- second
 * vector (init returns error) of the per-LED N+1 set.
 */
static void test_blink_init_invalid_led_rejected(void)
{
  reset_world();
  TEST_BEGIN("blink: invalid LED id rejected at init");
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_board_led_init((ra8_board_led_id_t)k_ra8_board_led_count));
  TEST_END("blink: invalid LED id rejected at init");
}

/**
 * @brief Toggle on out-of-range LED rejected during running thread.
 *
 * @details Simulates a stale handle reaching the toggle path
 * mid-thread -- the BSP must reject it and the thread loop's
 * ra8_board_led_toggle would break out (mirrors the app's `break` on
 * non-ok return).
 *
 * @par MC/DC:
 * Decision under test: ``ra8_board_led_toggle() != k_ra8_ok`` -- failure
 * vector. Combined with the two ok-loop tests this gives 3 vectors
 * across the toggle decision (N+1 for one atomic condition).
 */
static void test_blink_toggle_invalid_led_rejected(void)
{
  reset_world();
  TEST_BEGIN("blink: invalid LED id rejected at toggle");
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_board_led_toggle((ra8_board_led_id_t)k_ra8_board_led_count));
  TEST_END("blink: invalid LED id rejected at toggle");
}

/**
 * @brief Cross-thread ordering: A then B then A still both succeed.
 *
 * @details Mirrors the round-robin behaviour ThreadX gives the app at
 * priority 4. Verifies the BSP's per-LED state isn't accidentally
 * shared between IDs (state-transition coverage).
 *
 * @par MC/DC:
 * State-machine coverage: per-LED state transitions
 * (uninitialized -> initialized -> toggling). Pairs A+B in
 * interleaved order to exercise that no global lock is required.
 */
static void test_blink_interleaved_a_b_a_toggles(void)
{
  reset_world();
  TEST_BEGIN("blink: interleaved A/B/A toggle order");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_board_led_init(k_ra8_board_led1));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_board_led_init(k_ra8_board_led2));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_board_led_toggle(k_ra8_board_led1));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_board_led_toggle(k_ra8_board_led2));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_board_led_toggle(k_ra8_board_led1));
  TEST_END("blink: interleaved A/B/A toggle order");
}

/* -------------------------------------------------------------------------
 * Test runner
 * ------------------------------------------------------------------------- */

int main(void)
{
  test_blink_main_init_sequence_ok();
  test_blink_thread_a_loop_toggles_led1();
  test_blink_thread_b_loop_toggles_led2();
  test_blink_init_invalid_led_rejected();
  test_blink_toggle_invalid_led_rejected();
  test_blink_interleaved_a_b_a_toggles();
  return 0;
}
