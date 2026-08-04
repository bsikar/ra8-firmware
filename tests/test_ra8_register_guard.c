/**
 * @file test_ra8_register_guard.c
 * @brief Unit tests for ra8_register_guard.h nesting semantics
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_register_guard.h"
#include "unity_minimal.h"

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_enter_exit_compiles(void)
{
  TEST_BEGIN("enter/exit pair");
  ra8_register_guard_t guard;
  ra8_register_guard_enter(&guard);
  /* Critical section body (no-op on host). */
  ra8_register_guard_exit(&guard);
  TEST_END("enter/exit pair");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_nested_guards(void)
{
  TEST_BEGIN("nested guards");
  ra8_register_guard_t outer;
  ra8_register_guard_t inner;
  ra8_register_guard_enter(&outer);
  ra8_register_guard_enter(&inner);
  ra8_register_guard_exit(&inner);
  ra8_register_guard_exit(&outer);
  TEST_END("nested guards");
}

int32_t main(void)
{
  test_enter_exit_compiles();
  test_nested_guards();
  (void)fprintf(stderr, "[OK  ] test_ra8_register_guard.c\n");
  return 0;
}
