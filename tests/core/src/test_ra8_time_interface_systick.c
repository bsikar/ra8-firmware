/**
 * @file test_ra8_time_interface_systick.c
 * @brief Unit tests for libs/ra8_core/src/ra8_time_interface_systick.c
 *
 * @details
 * The unit under test is the production `ra8_time_interface_t` instance
 * `g_ra8_time_interface_systick`. Asserting the calls merely return proves
 * nothing here: the whole point of the object is that a consumer reading
 * time through the vtable sees the SAME tick source a direct ::ra8_time_ms
 * caller sees. So every forwarding test drives the underlying counter with
 * ::ra8_time_on_tick (the SysTick IRQ body, callable off-target because no
 * SysTick fires here) and compares what the vtable reports against what the
 * direct call reports, rather than against a constant.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8_time.h"
#include "ra8_time_interface.h"
#include "unity_minimal.h"

typedef enum : uint8_t {
  k_time_if_ticks_short = 3U,  /**< Tick burst for the forwarding test.    */
  k_time_if_ticks_long  = 17U, /**< Second burst, different from the first.*/
  k_time_if_delay_zero  = 0U,  /**< Zero delay: must return immediately.   */
} time_if_test_const_t;

/** @brief Sentinel the forwarders must ignore rather than dereference. */
static uint32_t s_ctx_sentinel = 0xA5A5A5A5U;

/**
 * @brief Advance the SysTick tick counter by `n` milliseconds.
 *
 * @param[in] n Number of ticks to inject.
 *
 * @pre None.
 * @post The counter behind ::ra8_time_ms has advanced by exactly `n`.
 *
 * @since 0.1.0
 */
static void internal_advance_ticks(uint32_t n)
{
  for (uint32_t i = 0U; i < n; i++) {
    ra8_time_on_tick();
  }
}

/**
 * @brief Verify the vtable object exists and is fully populated.
 *
 * @details
 * This is the regression guard for the defect that motivated the unit: the
 * header published `g_ra8_time_interface_systick` while no translation unit
 * defined it, so a consumer following the documented DI pattern failed to
 * link. Taking the address here and checking both slots keeps the object
 * linked and complete.
 *
 * @pre None.
 * @post No state modified.
 *
 * @note `ctx` is asserted NULL on purpose: the SysTick source is a
 *       process-wide singleton and must not smuggle per-instance state.
 *
 * @since 0.1.0
 */
static void test_time_if_systick_object_is_bound(void)
{
  TEST_BEGIN("g_ra8_time_interface_systick is defined and populated");
  TEST_ASSERT_NOT_NULL((const void*)&g_ra8_time_interface_systick);
  TEST_ASSERT_NOT_NULL((const void*)g_ra8_time_interface_systick.now_ms);
  TEST_ASSERT_NOT_NULL((const void*)g_ra8_time_interface_systick.delay_ms);
  TEST_ASSERT_NULL(g_ra8_time_interface_systick.ctx);
  TEST_END("g_ra8_time_interface_systick is defined and populated");
}

/**
 * @brief Verify `now_ms` reports the live SysTick counter, not a snapshot.
 *
 * @details
 * Reads through the vtable, injects a tick burst, reads again, and asserts
 * the delta equals the number of ticks injected. A forwarder that returned
 * a cached value, a constant, or a different clock fails this.
 *
 * @pre None.
 * @post The tick counter has advanced by ::k_time_if_ticks_short.
 *
 * @par MC/DC:
 * (no compound decisions in the code under test)
 *
 * @since 0.1.0
 */
static void test_time_if_systick_now_ms_tracks_ticks(void)
{
  TEST_BEGIN("now_ms tracks the SysTick counter");
  const uint32_t before = g_ra8_time_interface_systick.now_ms(g_ra8_time_interface_systick.ctx);
  internal_advance_ticks((uint32_t)k_time_if_ticks_short);
  const uint32_t after = g_ra8_time_interface_systick.now_ms(g_ra8_time_interface_systick.ctx);
  TEST_ASSERT_EQ((uint32_t)k_time_if_ticks_short, (uint32_t)(after - before));
  TEST_END("now_ms tracks the SysTick counter");
}

/**
 * @brief Verify `now_ms` and ::ra8_time_ms observe one shared counter.
 *
 * @details
 * The vtable read and the direct read are taken either side of a second,
 * differently sized tick burst and must agree exactly. This is what pins
 * the forwarder to the production time source rather than to any stub that
 * merely happens to increase.
 *
 * @pre None.
 * @post The tick counter has advanced by ::k_time_if_ticks_long.
 *
 * @since 0.1.0
 */
static void test_time_if_systick_now_ms_matches_direct_call(void)
{
  TEST_BEGIN("now_ms agrees with ra8_time_ms");
  TEST_ASSERT_EQ(ra8_time_ms(), g_ra8_time_interface_systick.now_ms(nullptr));
  internal_advance_ticks((uint32_t)k_time_if_ticks_long);
  TEST_ASSERT_EQ(ra8_time_ms(), g_ra8_time_interface_systick.now_ms(nullptr));
  TEST_END("now_ms agrees with ra8_time_ms");
}

/**
 * @brief Verify the context argument is ignored, not dereferenced.
 *
 * @details
 * The object advertises `ctx == nullptr`, but a caller is free to hand the
 * forwarder anything. Passing a sentinel pointer must produce the same
 * reading as passing NULL.
 *
 * @pre None.
 * @post No state modified.
 *
 * @since 0.1.0
 */
static void test_time_if_systick_ignores_ctx(void)
{
  TEST_BEGIN("forwarders ignore the ctx argument");
  const uint32_t with_null = g_ra8_time_interface_systick.now_ms(nullptr);
  const uint32_t with_ptr  = g_ra8_time_interface_systick.now_ms(&s_ctx_sentinel);
  TEST_ASSERT_EQ(with_null, with_ptr);
  TEST_ASSERT_EQ(0xA5A5A5A5U, s_ctx_sentinel);
  TEST_END("forwarders ignore the ctx argument");
}

/**
 * @brief Verify a zero-millisecond delay returns and perturbs nothing.
 *
 * @details
 * ::ra8_delay_ms documents that zero returns immediately. Driven through
 * the vtable, the call must return and must leave the tick counter alone,
 * which also proves `delay_ms` does not secretly advance time itself.
 *
 * @pre None.
 * @post The tick counter is unchanged.
 *
 * @since 0.1.0
 */
static void test_time_if_systick_delay_zero_is_a_noop(void)
{
  TEST_BEGIN("delay_ms(0) returns without moving the counter");
  const uint32_t before = ra8_time_ms();
  g_ra8_time_interface_systick.delay_ms(g_ra8_time_interface_systick.ctx,
                                        (uint32_t)k_time_if_delay_zero);
  TEST_ASSERT_EQ(before, ra8_time_ms());
  TEST_END("delay_ms(0) returns without moving the counter");
}

int main(void)
{
  test_time_if_systick_object_is_bound();
  test_time_if_systick_now_ms_tracks_ticks();
  test_time_if_systick_now_ms_matches_direct_call();
  test_time_if_systick_ignores_ctx();
  test_time_if_systick_delay_zero_is_a_noop();
  return 0;
}
