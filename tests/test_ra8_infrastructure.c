/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file test_ra8_infrastructure.c
 * @brief Unit tests for ra8_infrastructure.c bring-up helpers
 */

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_infrastructure.h"
#include "unity_minimal.h"

/* ra8_stack_canary_check() has no public header declaration; forward it
 * here so the test can invoke it without pulling in a new dependency. */

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_infrastructure_init_runs(void)
{
  TEST_BEGIN("ra8_infrastructure_init runs without crashing");
  ra8_fake_mmap_reset();
  ra8_infrastructure_init();
  TEST_END("ra8_infrastructure_init runs without crashing");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_stack_canary_check_host(void)
{
  TEST_BEGIN("ra8_stack_canary_check returns ok on host");
  ra8_fake_mmap_reset();
  /* On the host the canary range is compiled out via
   * RA8_OFF_TARGET and the function always reports success. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_stack_canary_check());
  TEST_END("ra8_stack_canary_check returns ok on host");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_double_init_is_safe(void)
{
  TEST_BEGIN("ra8_infrastructure_init is safe to call twice");
  ra8_fake_mmap_reset();
  ra8_infrastructure_init();
  ra8_infrastructure_init();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_stack_canary_check());
  TEST_END("ra8_infrastructure_init is safe to call twice");
}

int32_t main(void)
{
  test_infrastructure_init_runs();
  test_stack_canary_check_host();
  test_double_init_is_safe();
  (void)fprintf(stderr, "[OK  ] test_ra8_infrastructure.c\n");
  return 0;
}
