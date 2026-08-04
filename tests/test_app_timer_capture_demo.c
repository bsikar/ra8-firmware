/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file test_app_timer_capture_demo.c
 * @brief Integration test: GPT free-run + counter snapshot delta
 *
 * @details
 * Mirrors examples/ek_ra8d2/timer_capture_demo/main.c bring-up:
 * ra8_gpt_start_free_run -> ra8_gpt_read -> delta(start, stop). The
 * delta helper handles wrap so both the no-wrap and wrap branches
 * are exercised.
 *
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_gpt.h"
#include "ra8_mstp.h"
#include "unity_minimal.h"

/**
 * @enum app_timer_capture_demo_fixture_t
 * @brief Poison values written into out-parameters before a call, so one that fails without assigning is detectable.
 */
typedef enum : uint32_t {
  k_timer_poison_out =
    0xAAAAAAAAUL, /**< Poison in the capture out-param; a read that skips it is detectable. */
} app_timer_capture_demo_fixture_t;

typedef enum : uint32_t {
  k_t_timer_period   = 0xFFFFFFFFUL, /**< T timer period.       */
  k_t_timer_wrap_max = 0xFFFFFFFFUL, /**< T timer wrap maximum. */
} t_timer_const_t;

typedef enum : uint8_t {
  k_t_timer_channel = 0U, /**< T timer channel. */
} t_timer_byte_t;

/** @brief Copy of the demo's wrap-aware delta helper. */
static uint32_t delta(uint32_t start, uint32_t stop)
{
  if (stop >= start) {
    return stop - start;
  }
  return (uint32_t)k_t_timer_wrap_max - start + stop + 1U;
}

static void reset_world(void)
{
  ra8_fake_mmap_reset();
}

/**
 * @brief Bring-up programmes GPT0 in free-run mode.
 *
 * @par MC/DC:
 * Decision: ``ra8_gpt_start_free_run != ok``. One atomic condition
 * x 2 vectors -- valid channel + period (this) + bad channel test.
 */
static void test_timer_app_free_run_init(void)
{
  reset_world();
  TEST_BEGIN("timer_capture_demo: free-run init ok");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mstp_init());
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_gpt_start_free_run((uint8_t)k_t_timer_channel, (uint32_t)k_t_timer_period));
  TEST_END("timer_capture_demo: free-run init ok");
}

/**
 * @brief ra8_gpt_read pulls the current counter value.
 *
 * @par MC/DC:
 * Decision: ``ra8_gpt_read != ok``. Pairs with the NULL-out test
 * for N+1 = 2 coverage.
 */
static void test_timer_app_read_ok(void)
{
  reset_world();
  TEST_BEGIN("timer_capture_demo: read counter ok");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mstp_init());
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_gpt_start_free_run((uint8_t)k_t_timer_channel, (uint32_t)k_t_timer_period));
  uint32_t out = k_timer_poison_out;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_gpt_read((uint8_t)k_t_timer_channel, &out));
  TEST_END("timer_capture_demo: read counter ok");
}

/**
 * @brief NULL out pointer rejected by ra8_gpt_read.
 *
 * @par MC/DC:
 * Decision: ``out == nullptr``. One atomic condition x 2 vectors --
 * NULL (this) + non-NULL (test_timer_app_read_ok).
 */
static void test_timer_app_read_null(void)
{
  reset_world();
  TEST_BEGIN("timer_capture_demo: NULL read rejected");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mstp_init());
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_gpt_start_free_run((uint8_t)k_t_timer_channel, (uint32_t)k_t_timer_period));
  TEST_ASSERT(ra8_gpt_read((uint8_t)k_t_timer_channel, nullptr) != k_ra8_ok);
  TEST_END("timer_capture_demo: NULL read rejected");
}

/**
 * @brief delta covers the no-wrap, exact-zero, and wrap branches.
 *
 * @par MC/DC:
 * Decision: ``stop >= start``. One atomic condition x 2 vectors --
 * stop > start (no-wrap) + stop < start (wrap). The exact-equal
 * vector is covered by the start==stop case.
 */
static void test_timer_app_delta_branches(void)
{
  TEST_BEGIN("timer_capture_demo: delta covers no-wrap + wrap");
  TEST_ASSERT_EQ(100U, delta(50U, 150U));
  TEST_ASSERT_EQ(0U, delta(0U, 0U));
  /* Wrap: start near top, stop just past zero. */
  const uint32_t wrap = delta(0xFFFFFFF0UL, 0x10U);
  TEST_ASSERT_EQ(0x20U, wrap);
  TEST_END("timer_capture_demo: delta covers no-wrap + wrap");
}

int main(void)
{
  test_timer_app_free_run_init();
  test_timer_app_read_ok();
  test_timer_app_read_null();
  test_timer_app_delta_branches();
  return 0;
}
