/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file test_app_gpt_capture_input.c
 * @brief Integration test for examples/ek_ra8d2/gpt_capture_input/main.c
 *
 * @details
 * Replays the falling-edge detector and the GPT init / read pair
 * the demo uses to approximate input capture.
 *
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_board_ek_ra8d2.h"
#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_gpt.h"
#include "unity_minimal.h"

typedef enum : uint32_t {
  k_test_gpt_capture_period = 0xFFFFFFFFUL, /**< Test GPT capture period. */
} test_gpt_capture_const_t;

typedef enum : uint8_t {
  k_test_gpt_capture_channel  = 0U,  /**< Test GPT capture channel.  */
  k_test_gpt_capture_bad_chan = 99U, /**< Test GPT capture bad chan. */
} test_gpt_capture_chan_t;

static void reset_world(void)
{
  ra8_fake_mmap_reset();
}

/* Mirror of the app's edge detector. */
static bool edge_detect(ra8_board_sw_state_t* prev, ra8_board_sw_state_t now)
{
  bool fell = (*prev == k_ra8_board_sw_released) && (now == k_ra8_board_sw_pressed);
  *prev     = now;
  return fell;
}

/**
 * @par MC/DC:
 * Compound decision: ``prev == released && now == pressed``. Two
 * atomic conditions x N+1 = 3 vectors:
 *  - V1: prev=released, now=pressed -> true (this test).
 *  - V2: prev=pressed, now=pressed  -> false (varies prev).
 *  - V3: prev=released, now=released -> false (varies now).
 * V1 + V2 prove prev independently affects outcome; V1 + V3 prove
 * now does. Minimal MC/DC for N=2.
 */
static void test_gpt_capture_edge_true(void)
{
  reset_world();
  TEST_BEGIN("gpt_capture: falling edge V1 (released->pressed)");
  ra8_board_sw_state_t prev = k_ra8_board_sw_released;
  TEST_ASSERT(edge_detect(&prev, k_ra8_board_sw_pressed));
  TEST_END("gpt_capture: falling edge V1");
}

/**
 * @par MC/DC:
 * Same compound decision as test_gpt_capture_edge_true. This test
 * supplies vector V2 (prev=pressed, now=pressed -> false), pairing
 * with V1 to prove ``prev`` independently affects the outcome.
 */
static void test_gpt_capture_edge_already_pressed(void)
{
  reset_world();
  TEST_BEGIN("gpt_capture: falling edge V2 (pressed->pressed = false)");
  ra8_board_sw_state_t prev = k_ra8_board_sw_pressed;
  TEST_ASSERT(!edge_detect(&prev, k_ra8_board_sw_pressed));
  TEST_END("gpt_capture: falling edge V2");
}

/**
 * @par MC/DC:
 * Same compound decision as test_gpt_capture_edge_true. This test
 * supplies vector V3 (prev=released, now=released -> false), pairing
 * with V1 to prove ``now`` independently affects the outcome.
 */
static void test_gpt_capture_edge_release(void)
{
  reset_world();
  TEST_BEGIN("gpt_capture: falling edge V3 (released->released = false)");
  ra8_board_sw_state_t prev = k_ra8_board_sw_released;
  TEST_ASSERT(!edge_detect(&prev, k_ra8_board_sw_released));
  TEST_END("gpt_capture: falling edge V3");
}

/**
 * @par MC/DC:
 * Decision: ``ra8_gpt_init(...) != ok``. Pairs the ok vector here
 * with the bad-channel vector below for N+1 = 2 vectors.
 */
static void test_gpt_capture_init_ok(void)
{
  reset_world();
  TEST_BEGIN("gpt_capture: ra8_gpt_init free-run config ok");
  const ra8_gpt_cfg_t cfg = {
    .mode       = k_ra8_gpt_mode_saw_pwm,
    .prescaler  = k_ra8_gpt_ps_div_64,
    .period     = (uint32_t)k_test_gpt_capture_period,
    .duty_a     = 0U,
    .duty_b     = 0U,
    .auto_start = true,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_gpt_init((uint8_t)k_test_gpt_capture_channel, &cfg));
  TEST_END("gpt_capture: ra8_gpt_init free-run config ok");
}

/**
 * @par MC/DC:
 * Decision: ``ra8_gpt_init(bad_channel) != ok`` failure vector. Pairs
 * with the ok-vector test above for N+1 = 2 vectors of the
 * channel-out-of-range guard.
 */
static void test_gpt_capture_init_bad_channel(void)
{
  reset_world();
  TEST_BEGIN("gpt_capture: ra8_gpt_init rejects bad channel");
  const ra8_gpt_cfg_t cfg = {
    .mode       = k_ra8_gpt_mode_saw_pwm,
    .prescaler  = k_ra8_gpt_ps_div_64,
    .period     = (uint32_t)k_test_gpt_capture_period,
    .duty_a     = 0U,
    .duty_b     = 0U,
    .auto_start = true,
  };
  TEST_ASSERT(ra8_gpt_init((uint8_t)k_test_gpt_capture_bad_chan, &cfg) != k_ra8_ok);
  TEST_END("gpt_capture: ra8_gpt_init rejects bad channel");
}

int main(void)
{
  test_gpt_capture_edge_true();
  test_gpt_capture_edge_already_pressed();
  test_gpt_capture_edge_release();
  test_gpt_capture_init_ok();
  test_gpt_capture_init_bad_channel();
  return 0;
}
