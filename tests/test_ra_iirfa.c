/**
 * @file test_ra_iirfa.c
 * @brief Unit tests for ra_iirfa.c (IIR Filter Accelerator placeholder)
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_err.h"
#include "ra_iirfa.h"
#include "ra_sim_mmap.h"
#include "unity_minimal.h"

typedef enum : int32_t {
  k_test_b0    = 0x1000,
  k_test_shift = 12,
} iirfa_test_t;

static void test_open_close(void)
{
  TEST_BEGIN("iirfa open/close");
  ra_sim_mmap_reset();

  const ra_iirfa_cfg_t cfg = {
    .stage_count = 1U,
    .shift       = (uint8_t)k_test_shift,
    .stages = {{(int32_t)k_test_b0, 0, 0, 0, 0}, {0, 0, 0, 0, 0}, {0, 0, 0, 0, 0}, {0, 0, 0, 0, 0}},
  };
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_iirfa_open(&cfg));

  uint8_t status = 0U;
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_iirfa_get_status(&status));
  TEST_ASSERT((status & 0x1U) != 0U);

  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_iirfa_close());
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_iirfa_get_status(&status));
  TEST_ASSERT((status & 0x1U) == 0U);
  TEST_END("iirfa open/close");
}

static void test_open_null_bad(void)
{
  TEST_BEGIN("iirfa open null/bad");
  TEST_ASSERT_EQ((int)k_ra_err_null_ptr, (int)ra_iirfa_open(nullptr));
  ra_iirfa_cfg_t cfg = {.stage_count = 0U};
  TEST_ASSERT_EQ((int)k_ra_err_invalid_arg, (int)ra_iirfa_open(&cfg));
  cfg.stage_count = 99U;
  TEST_ASSERT_EQ((int)k_ra_err_invalid_arg, (int)ra_iirfa_open(&cfg));
  TEST_END("iirfa open null/bad");
}

static void test_step_passthrough(void)
{
  TEST_BEGIN("iirfa step passthrough");
  ra_sim_mmap_reset();

  /* Single-stage with b0 = 1<<12 and shift = 12 -> y = x. */
  ra_iirfa_cfg_t cfg = {.stage_count = 1U, .shift = (uint8_t)k_test_shift};
  cfg.stages[0]      = (ra_iirfa_biquad_t){.b0 = (int32_t)k_test_b0};
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_iirfa_open(&cfg));

  int32_t out = 0;
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_iirfa_step(42, &out));
  TEST_ASSERT_EQ((int)42, (int)out);

  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_iirfa_step(-7, &out));
  TEST_ASSERT_EQ((int)-7, (int)out);
  TEST_END("iirfa step passthrough");
}

static void test_reset_history(void)
{
  TEST_BEGIN("iirfa reset history");
  ra_sim_mmap_reset();

  ra_iirfa_cfg_t cfg = {.stage_count = 1U, .shift = (uint8_t)k_test_shift};
  cfg.stages[0]      = (ra_iirfa_biquad_t){.b0 = (int32_t)k_test_b0, .b1 = (int32_t)k_test_b0};
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_iirfa_open(&cfg));

  int32_t out = 0;
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_iirfa_step(10, &out));
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_iirfa_step(20, &out));
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_iirfa_reset());
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_iirfa_step(0, &out));
  TEST_ASSERT_EQ((int)0, (int)out);
  TEST_END("iirfa reset history");
}

static void test_step_unopened(void)
{
  TEST_BEGIN("iirfa step unopened");
  ra_sim_mmap_reset();
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_iirfa_close());
  int32_t out = 0;
  TEST_ASSERT_EQ((int)k_ra_err_invalid_state, (int)ra_iirfa_step(1, &out));
  TEST_END("iirfa step unopened");
}

static void test_null_outs(void)
{
  TEST_BEGIN("iirfa null outs");
  TEST_ASSERT_EQ((int)k_ra_err_null_ptr, (int)ra_iirfa_step(0, nullptr));
  TEST_ASSERT_EQ((int)k_ra_err_null_ptr, (int)ra_iirfa_get_status(nullptr));
  TEST_END("iirfa null outs");
}

int32_t main(void)
{
  test_open_close();
  test_open_null_bad();
  test_step_passthrough();
  test_reset_history();
  test_step_unopened();
  test_null_outs();
  (void)fprintf(stderr, "[OK  ] test_ra_iirfa.c\n");
  return 0;
}
