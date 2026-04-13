/**
 * @file test_ra_infrastructure.c
 * @brief Unit tests for ra_infrastructure.c bring-up helpers
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra_err.h"
#include "ra_infrastructure.h"
#include "ra_sim_mmap.h"
#include "unity_minimal.h"

/* ra_stack_canary_check() has no public header declaration; forward it
 * here so the test can invoke it without pulling in a new dependency. */
ra_err_t ra_stack_canary_check(void);

static void test_infrastructure_init_runs(void)
{
  TEST_BEGIN("ra_infrastructure_init runs without crashing");
  ra_sim_mmap_reset();
  ra_infrastructure_init();
  TEST_END("ra_infrastructure_init runs without crashing");
}

static void test_stack_canary_check_host(void)
{
  TEST_BEGIN("ra_stack_canary_check returns ok on host");
  ra_sim_mmap_reset();
  /* On the host the canary range is compiled out via
   * RA_SIMULATOR_MODE and the function always reports success. */
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_stack_canary_check());
  TEST_END("ra_stack_canary_check returns ok on host");
}

static void test_double_init_is_safe(void)
{
  TEST_BEGIN("ra_infrastructure_init is safe to call twice");
  ra_sim_mmap_reset();
  ra_infrastructure_init();
  ra_infrastructure_init();
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_stack_canary_check());
  TEST_END("ra_infrastructure_init is safe to call twice");
}

int32_t main(void)
{
  test_infrastructure_init_runs();
  test_stack_canary_check_host();
  test_double_init_is_safe();
  (void)fprintf(stderr, "[OK  ] test_ra_infrastructure.c\n");
  return 0;
}
