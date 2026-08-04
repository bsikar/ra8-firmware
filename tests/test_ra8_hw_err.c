/**
 * @file test_ra8_hw_err.c
 * @brief Unit tests for the bounded wait-flag primitives in ra8_hw_err.h.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_fake_mmio.h"
#include "ra8_hw_err.h"
#include "unity_minimal.h"

/**
 * @enum hw_err_fixture_t
 * @brief Values planted in registers to prove a read or write reaches them.
 */
typedef enum : uint8_t {
  k_hw_probe_byte_top_bit =
    0x80U, /**< An 8-bit value with only the top bit set; a signed conversion would corrupt it. */
  k_hw_probe_byte =
    0xAAU, /**< An 8-bit value with alternating bits, so truncation or sign-extension shows up. */
  k_hw_probe_all_but_lsb =
    0xFEU, /**< All bits set but the LSB, so a real read is distinct from an all-ones default. */
} hw_err_fixture_t;

/**
 * @enum hw_err_fixture2_t
 * @brief Values planted in registers to prove a read or write reaches them.
 */
typedef enum : uint32_t {
  k_hw_probe_word_top_bit = 0x80000000U, /**< A 32-bit value with only bit 31 set. */
  k_hw_probe_word_all_ones =
    0xFFFFFFFFU, /**< Full-width all-ones; also a dead-bus return, paired so the two differ. */
} hw_err_fixture2_t;

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_set8_already_set_returns_immediately(void)
{
  TEST_BEGIN("ra8_hw_wait_flag_set8: pre-set returns ok");
  ra8_fake_mmio_reset();
  uint8_t r = k_hw_probe_byte_top_bit;
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
  ra8_fake_mmio_reset();
  uint8_t r = 0x00U;
  /* Arm the seam so the un-set flag never satisfies -- the waiter must run to
   * budget and return the timeout, not the unarmed "flag ready" success. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fake_mmio_fail_wait((const volatile void*)&r));
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
  ra8_fake_mmio_reset();
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
  ra8_fake_mmio_reset();
  uint8_t r = k_hw_probe_byte;
  /* Arm the seam so the stuck-set bit never clears -- force the timeout leg. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fake_mmio_fail_wait((const volatile void*)&r));
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
  ra8_fake_mmio_reset();
  uint32_t r = k_hw_probe_word_top_bit;
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
  ra8_fake_mmio_reset();
  uint32_t r = 0U;
  /* Arm the seam so the un-set flag never satisfies -- force the timeout leg. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fake_mmio_fail_wait((const volatile void*)&r));
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
  ra8_fake_mmio_reset();
  uint32_t r = k_hw_probe_all_but_lsb;
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
  ra8_fake_mmio_reset();
  uint32_t r = k_hw_probe_word_all_ones;
  /* Arm the seam so the stuck-set bits never clear -- force the timeout leg. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fake_mmio_fail_wait((const volatile void*)&r));
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
  ra8_fake_mmio_reset();
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
  ra8_fake_mmio_reset();
  uint8_t r = k_hw_probe_byte_top_bit;
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
