/**
 * @file test_ra_hw_err.c
 * @brief Unit tests for the bounded wait-flag primitives in ra_hw_err.h.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra_err.h"
#include "ra_hw_err.h"
#include "unity_minimal.h"

static void test_set8_already_set_returns_immediately(void)
{
  TEST_BEGIN("ra_hw_wait_flag_set8: pre-set returns ok");
  uint8_t r = 0x80U;
  TEST_ASSERT_EQ((int)k_ra_ok,
                 (int)ra_hw_wait_flag_set8(&r, 0x80U, (uint32_t)k_ra_hw_budget_short));
  TEST_END("ra_hw_wait_flag_set8: pre-set returns ok");
}

static void test_set8_clear_times_out(void)
{
  TEST_BEGIN("ra_hw_wait_flag_set8: cleared register times out");
  uint8_t r = 0x00U;
  TEST_ASSERT_EQ((int)k_ra_err_hw_timeout,
                 (int)ra_hw_wait_flag_set8(&r, 0x80U, (uint32_t)k_ra_hw_budget_short));
  TEST_END("ra_hw_wait_flag_set8: cleared register times out");
}

static void test_clear8_pre_clear_returns_immediately(void)
{
  TEST_BEGIN("ra_hw_wait_flag_clear8: pre-clear returns ok");
  uint8_t r = 0x00U;
  TEST_ASSERT_EQ((int)k_ra_ok,
                 (int)ra_hw_wait_flag_clear8(&r, 0xFFU, (uint32_t)k_ra_hw_budget_short));
  TEST_END("ra_hw_wait_flag_clear8: pre-clear returns ok");
}

static void test_clear8_set_times_out(void)
{
  TEST_BEGIN("ra_hw_wait_flag_clear8: stuck-set bit times out");
  uint8_t r = 0xAAU;
  TEST_ASSERT_EQ((int)k_ra_err_hw_timeout,
                 (int)ra_hw_wait_flag_clear8(&r, 0x02U, (uint32_t)k_ra_hw_budget_short));
  TEST_END("ra_hw_wait_flag_clear8: stuck-set bit times out");
}

static void test_set32_already_set_returns_immediately(void)
{
  TEST_BEGIN("ra_hw_wait_flag_set32: pre-set returns ok");
  uint32_t r = 0x80000000U;
  TEST_ASSERT_EQ((int)k_ra_ok,
                 (int)ra_hw_wait_flag_set32(&r, 0x80000000U, (uint32_t)k_ra_hw_budget_short));
  TEST_END("ra_hw_wait_flag_set32: pre-set returns ok");
}

static void test_set32_times_out(void)
{
  TEST_BEGIN("ra_hw_wait_flag_set32: cleared register times out");
  uint32_t r = 0U;
  TEST_ASSERT_EQ((int)k_ra_err_hw_timeout,
                 (int)ra_hw_wait_flag_set32(&r, 0x10U, (uint32_t)k_ra_hw_budget_short));
  TEST_END("ra_hw_wait_flag_set32: cleared register times out");
}

static void test_clear32_pre_clear_returns_immediately(void)
{
  TEST_BEGIN("ra_hw_wait_flag_clear32: pre-clear returns ok");
  uint32_t r = 0xFEU;
  TEST_ASSERT_EQ((int)k_ra_ok,
                 (int)ra_hw_wait_flag_clear32(&r, 0x01U, (uint32_t)k_ra_hw_budget_short));
  TEST_END("ra_hw_wait_flag_clear32: pre-clear returns ok");
}

static void test_clear32_times_out(void)
{
  TEST_BEGIN("ra_hw_wait_flag_clear32: stuck-set bit times out");
  uint32_t r = 0xFFFFFFFFU;
  TEST_ASSERT_EQ((int)k_ra_err_hw_timeout,
                 (int)ra_hw_wait_flag_clear32(&r, 0xFFU, (uint32_t)k_ra_hw_budget_short));
  TEST_END("ra_hw_wait_flag_clear32: stuck-set bit times out");
}

static void test_null_register_rejected(void)
{
  TEST_BEGIN("ra_hw_wait_flag_*: NULL register rejected");
  TEST_ASSERT_EQ((int)k_ra_err_null_ptr,
                 (int)ra_hw_wait_flag_set8(nullptr, 0x80U, (uint32_t)k_ra_hw_budget_short));
  TEST_ASSERT_EQ((int)k_ra_err_null_ptr,
                 (int)ra_hw_wait_flag_clear8(nullptr, 0x80U, (uint32_t)k_ra_hw_budget_short));
  TEST_ASSERT_EQ((int)k_ra_err_null_ptr,
                 (int)ra_hw_wait_flag_set32(nullptr, 0x80000000U, (uint32_t)k_ra_hw_budget_short));
  TEST_ASSERT_EQ(
    (int)k_ra_err_null_ptr,
    (int)ra_hw_wait_flag_clear32(nullptr, 0x80000000U, (uint32_t)k_ra_hw_budget_short));
  TEST_END("ra_hw_wait_flag_*: NULL register rejected");
}

static void test_zero_budget_immediate_timeout(void)
{
  TEST_BEGIN("ra_hw_wait_flag_*: zero budget times out without polling");
  uint8_t r = 0x80U;
  /* Even though the bit is already set, a budget of 0 means the
   * loop body never runs and the helper returns timeout. */
  TEST_ASSERT_EQ((int)k_ra_err_hw_timeout, (int)ra_hw_wait_flag_set8(&r, 0x80U, 0U));
  TEST_END("ra_hw_wait_flag_*: zero budget times out without polling");
}

int main(void)
{
  test_set8_already_set_returns_immediately();
  test_set8_clear_times_out();
  test_clear8_pre_clear_returns_immediately();
  test_clear8_set_times_out();
  test_set32_already_set_returns_immediately();
  test_set32_times_out();
  test_clear32_pre_clear_returns_immediately();
  test_clear32_times_out();
  test_null_register_rejected();
  test_zero_budget_immediate_timeout();
  (void)fprintf(stderr, "[OK  ] test_ra_hw_err.c\n");
  return 0;
}
