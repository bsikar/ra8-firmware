/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file test_app_iwdt_demo.c
 * @brief Integration test: IWDT window-mode refresh demo
 *
 * @details
 * Mirrors examples/ek_ra8d2/iwdt_demo/main.c bring-up:
 * ``ra8_iwdt_init`` -> ``ra8_iwdt_get_counter`` -> window check ->
 * ``ra8_iwdt_refresh_deferred`` -> ``ra8_iwdt_clear_status``. Backed by
 * the host MMIO shim. The window-membership decision is local to the
 * demo, so this test re-implements the same predicate and exhausts
 * its MC/DC vectors.
 *
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_iwdt.h"
#include "unity_minimal.h"

typedef enum : uint16_t {
  k_test_iwdt_window_low  = 0x0400U, /**< Test iwdt window low.  */
  k_test_iwdt_window_high = 0x0C00U, /**< Test iwdt window high. */
  k_test_iwdt_below       = 0x0100U, /**< Below window_low.      */
  k_test_iwdt_inside      = 0x0800U, /**< Inside window.         */
  k_test_iwdt_above       = 0x1000U, /**< Above window_high.     */
} test_iwdt_const_t;

static void reset_world(void)
{
  ra8_fake_mmap_reset();
}

/**
 * @brief Mirror of ``iwdt_demo_in_window`` from main.c.
 *
 * Re-implemented locally so this test owns the MC/DC vectors for the
 * compound decision without having to link against the demo TU.
 */
static bool in_window(uint16_t counter)
{
  return (counter >= (uint16_t)k_test_iwdt_window_low) &&
         (counter <= (uint16_t)k_test_iwdt_window_high);
}

/**
 * @brief Golden bring-up: init + counter read + refresh + status clear.
 *
 * @par MC/DC:
 * Decision in app: ``ra8_iwdt_init != ok``. One atomic condition x 2
 * vectors -- golden (this) + null counter pointer below.
 */
static void test_iwdt_arm_ok(void)
{
  reset_world();
  TEST_BEGIN("iwdt_demo: init + counter + refresh + clear");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_iwdt_init());
  uint16_t counter = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_iwdt_get_counter(&counter));
  ra8_iwdt_refresh_deferred();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_iwdt_clear_status());
  TEST_END("iwdt_demo: init + counter + refresh + clear");
}

/**
 * @brief NULL counter pointer rejected by ``ra8_iwdt_get_counter``.
 *
 * @par MC/DC:
 * Decision: ``out_counter == NULL``. One atomic condition x 2 vectors
 * -- non-NULL above + NULL here.
 */
static void test_iwdt_null_counter(void)
{
  reset_world();
  TEST_BEGIN("iwdt_demo: NULL counter pointer rejected");
  (void)ra8_iwdt_init();
  TEST_ASSERT(ra8_iwdt_get_counter(nullptr) != k_ra8_ok);
  TEST_END("iwdt_demo: NULL counter pointer rejected");
}

/**
 * @brief Window-membership predicate -- MC/DC for the compound decision.
 *
 * @par MC/DC:
 * Decision: ``counter >= window_low && counter <= window_high``.
 * Two atomic conditions, N+1 = 3 vectors:
 * - Vector 1: counter=0x0800 (inside)        -> true   (control: both true)
 * - Vector 2: counter=0x0100 (below low)     -> false  (varies low only)
 * - Vector 3: counter=0x1000 (above high)    -> false  (varies high only)
 * Vectors 1+2 prove the lower bound independently affects the
 * outcome; vectors 1+3 prove the same for the upper bound.
 */
static void test_iwdt_window_predicate(void)
{
  reset_world();
  TEST_BEGIN("iwdt_demo: window predicate MC/DC");
  TEST_ASSERT(in_window((uint16_t)k_test_iwdt_inside));
  TEST_ASSERT(!in_window((uint16_t)k_test_iwdt_below));
  TEST_ASSERT(!in_window((uint16_t)k_test_iwdt_above));
  TEST_END("iwdt_demo: window predicate MC/DC");
}

int main(void)
{
  test_iwdt_arm_ok();
  test_iwdt_null_counter();
  test_iwdt_window_predicate();
  return 0;
}
