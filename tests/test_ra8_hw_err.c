/**
 * @file test_ra8_hw_err.c
 * @brief Unit tests for the bounded wait-flag primitives in ra8_hw_err.h.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_hw_err.h"
#include "ra8_sim_mmio.h"
#include "unity_minimal.h"

/**
 * @enum hw_err_uint8_const_t
 * @brief Named uint8_t constants used by this file.
 *
 * @details
 * Every literal this translation unit needs, named so the
 * value's role is visible at the point of use (CLAUDE.md
 * "No Magic Numbers").
 */
typedef enum : uint8_t {
  k_hw_err_r_80 = 0x80U,
  k_hw_err_r_aa = 0xAAU,
  k_hw_err_r_fe = 0xFEU,
} hw_err_uint8_const_t;

/**
 * @enum hw_err_uint32_const_t
 * @brief Named uint32_t constants used by this file.
 *
 * @details
 * Every literal this translation unit needs, named so the
 * value's role is visible at the point of use (CLAUDE.md
 * "No Magic Numbers").
 */
typedef enum : uint32_t {
  k_hw_err_r_80000000 = 0x80000000U,
  k_hw_err_r_ffffffff = 0xFFFFFFFFU,
} hw_err_uint32_const_t;

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_set8_already_set_returns_immediately(void)
{
  TEST_BEGIN("ra8_hw_wait_flag_set8: pre-set returns ok");
  ra8_sim_mmio_reset();
  uint8_t r = k_hw_err_r_80;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_hw_wait_flag_set8(&r, 0x80U, (uint32_t)k_ra8_hw_budget_short));
  TEST_END("ra8_hw_wait_flag_set8: pre-set returns ok");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_set8_clear_times_out(void)
{
  TEST_BEGIN("ra8_hw_wait_flag_set8: cleared register times out");
  ra8_sim_mmio_reset();
  uint8_t r = 0x00U;
  /* Arm the seam so the un-set flag never satisfies -- the waiter must run to
   * budget and return the timeout, not the unarmed "flag ready" success. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sim_mmio_fail_wait((const volatile void*)&r));
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout,
                 ra8_hw_wait_flag_set8(&r, 0x80U, (uint32_t)k_ra8_hw_budget_short));
  TEST_END("ra8_hw_wait_flag_set8: cleared register times out");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_clear8_pre_clear_returns_immediately(void)
{
  TEST_BEGIN("ra8_hw_wait_flag_clear8: pre-clear returns ok");
  ra8_sim_mmio_reset();
  uint8_t r = 0x00U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_hw_wait_flag_clear8(&r, 0xFFU, (uint32_t)k_ra8_hw_budget_short));
  TEST_END("ra8_hw_wait_flag_clear8: pre-clear returns ok");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_clear8_set_times_out(void)
{
  TEST_BEGIN("ra8_hw_wait_flag_clear8: stuck-set bit times out");
  ra8_sim_mmio_reset();
  uint8_t r = k_hw_err_r_aa;
  /* Arm the seam so the stuck-set bit never clears -- force the timeout leg. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sim_mmio_fail_wait((const volatile void*)&r));
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout,
                 ra8_hw_wait_flag_clear8(&r, 0x02U, (uint32_t)k_ra8_hw_budget_short));
  TEST_END("ra8_hw_wait_flag_clear8: stuck-set bit times out");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_set32_already_set_returns_immediately(void)
{
  TEST_BEGIN("ra8_hw_wait_flag_set32: pre-set returns ok");
  ra8_sim_mmio_reset();
  uint32_t r = k_hw_err_r_80000000;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_hw_wait_flag_set32(&r, 0x80000000U, (uint32_t)k_ra8_hw_budget_short));
  TEST_END("ra8_hw_wait_flag_set32: pre-set returns ok");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_set32_times_out(void)
{
  TEST_BEGIN("ra8_hw_wait_flag_set32: cleared register times out");
  ra8_sim_mmio_reset();
  uint32_t r = 0U;
  /* Arm the seam so the un-set flag never satisfies -- force the timeout leg. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sim_mmio_fail_wait((const volatile void*)&r));
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout,
                 ra8_hw_wait_flag_set32(&r, 0x10U, (uint32_t)k_ra8_hw_budget_short));
  TEST_END("ra8_hw_wait_flag_set32: cleared register times out");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_clear32_pre_clear_returns_immediately(void)
{
  TEST_BEGIN("ra8_hw_wait_flag_clear32: pre-clear returns ok");
  ra8_sim_mmio_reset();
  uint32_t r = k_hw_err_r_fe;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_hw_wait_flag_clear32(&r, 0x01U, (uint32_t)k_ra8_hw_budget_short));
  TEST_END("ra8_hw_wait_flag_clear32: pre-clear returns ok");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_clear32_times_out(void)
{
  TEST_BEGIN("ra8_hw_wait_flag_clear32: stuck-set bit times out");
  ra8_sim_mmio_reset();
  uint32_t r = k_hw_err_r_ffffffff;
  /* Arm the seam so the stuck-set bits never clear -- force the timeout leg. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sim_mmio_fail_wait((const volatile void*)&r));
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout,
                 ra8_hw_wait_flag_clear32(&r, 0xFFU, (uint32_t)k_ra8_hw_budget_short));
  TEST_END("ra8_hw_wait_flag_clear32: stuck-set bit times out");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_null_register_rejected(void)
{
  TEST_BEGIN("ra8_hw_wait_flag_*: NULL register rejected");
  ra8_sim_mmio_reset();
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_hw_wait_flag_set8(nullptr, 0x80U, (uint32_t)k_ra8_hw_budget_short));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_hw_wait_flag_clear8(nullptr, 0x80U, (uint32_t)k_ra8_hw_budget_short));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_hw_wait_flag_set32(nullptr, 0x80000000U, (uint32_t)k_ra8_hw_budget_short));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_hw_wait_flag_clear32(nullptr, 0x80000000U, (uint32_t)k_ra8_hw_budget_short));
  TEST_END("ra8_hw_wait_flag_*: NULL register rejected");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_zero_budget_immediate_timeout(void)
{
  TEST_BEGIN("ra8_hw_wait_flag_*: zero budget times out without polling");
  ra8_sim_mmio_reset();
  uint8_t r = k_hw_err_r_80;
  /* Even though the bit is already set, a budget of 0 means the
   * loop body never runs and the helper returns timeout. */
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout, ra8_hw_wait_flag_set8(&r, 0x80U, 0U));
  TEST_END("ra8_hw_wait_flag_*: zero budget times out without polling");
}

int32_t main(void)
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
  (void)fprintf(stderr, "[OK  ] test_ra8_hw_err.c\n");
  return 0;
}
