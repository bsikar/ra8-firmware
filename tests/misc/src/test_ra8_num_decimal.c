/**
 * @file test_ra8_num_decimal.c
 * @brief Unit tests for the exact decimal to binary64 conversion.
 * @details Pins correct rounding, ties-to-even, subnormal and refusal behaviour against known bit patterns.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <string.h>

#include "ra8_num.h"
#include "unity_minimal.h"

/**
 * @brief Read the binary64 bit pattern of a value without type punning.
 * @details @param[in] value Converted value. @return Its IEEE-754 bit pattern.
 * @pre None. @post The value is unchanged.
 * @note Transfer uses memcpy. @since 0.1.0
 */
static uint64_t bits_of(double value)
{
  uint64_t bits = 0U;
  memcpy(&bits, &value, sizeof(bits));
  return bits;
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path against known bit patterns)
 */
static void test_exact_small_integers(void)
{
  TEST_BEGIN("small integers convert exactly");
  double v = 1.0;
  TEST_ASSERT(ra8_num_decimal_to_binary64(0U, 0, false, &v));
  TEST_ASSERT_EQ(UINT64_C(0x0000000000000000), bits_of(v));
  TEST_ASSERT(ra8_num_decimal_to_binary64(0U, 0, true, &v));
  TEST_ASSERT_EQ(UINT64_C(0x8000000000000000), bits_of(v));
  TEST_ASSERT(ra8_num_decimal_to_binary64(1U, 0, false, &v));
  TEST_ASSERT_EQ(UINT64_C(0x3FF0000000000000), bits_of(v));
  TEST_ASSERT(ra8_num_decimal_to_binary64(1U, 0, true, &v));
  TEST_ASSERT_EQ(UINT64_C(0xBFF0000000000000), bits_of(v));
  TEST_ASSERT(ra8_num_decimal_to_binary64(2U, 0, false, &v));
  TEST_ASSERT_EQ(UINT64_C(0x4000000000000000), bits_of(v));
  TEST_ASSERT(ra8_num_decimal_to_binary64(100U, -2, false, &v));
  TEST_ASSERT_EQ(UINT64_C(0x3FF0000000000000), bits_of(v));
  TEST_END("small integers convert exactly");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path against known bit patterns)
 */
static void test_inexact_decimals_round_to_nearest(void)
{
  TEST_BEGIN("inexact decimals round to nearest");
  double v = 0.0;
  /* 0.1 -> the canonical 3FB999999999999A, not 3FB9999999999999. */
  TEST_ASSERT(ra8_num_decimal_to_binary64(1U, -1, false, &v));
  TEST_ASSERT_EQ(UINT64_C(0x3FB999999999999A), bits_of(v));
  /* 0.2, 0.3 and 1/3 pin the same rounding one and two bits along. */
  TEST_ASSERT(ra8_num_decimal_to_binary64(2U, -1, false, &v));
  TEST_ASSERT_EQ(UINT64_C(0x3FC999999999999A), bits_of(v));
  TEST_ASSERT(ra8_num_decimal_to_binary64(3U, -1, false, &v));
  TEST_ASSERT_EQ(UINT64_C(0x3FD3333333333333), bits_of(v));
  TEST_ASSERT(ra8_num_decimal_to_binary64(UINT64_C(3333333333333333), -16, false, &v));
  TEST_ASSERT_EQ(UINT64_C(0x3FD5555555555555), bits_of(v));
  /* A 17-digit significand, the widest the contract accepts. */
  TEST_ASSERT(ra8_num_decimal_to_binary64(UINT64_C(31415926535897932), -16, false, &v));
  TEST_ASSERT_EQ(UINT64_C(0x400921FB54442D18), bits_of(v));
  TEST_END("inexact decimals round to nearest");
}

/**
 * @par MC/DC:
 * Decision: `(comparison > 0) || ((comparison == 0) && ((quotient & 1U) != 0U))`
 * in internal_ra8_num_divide, the nearest-even tie break.
 * - T,-  : remainder above half (2^-1075 * 3) rounds up.
 * - F,T,T: exact half with an odd quotient rounds up.
 * - F,T,F: exact half with an even quotient stays put.
 * - F,F,-: remainder below half truncates.
 * Each operand independently flips the outcome.
 */
static void test_ties_round_to_even(void)
{
  TEST_BEGIN("exact ties round to even");
  double v = 0.0;
  /* 2^53 + 1 is the first integer binary64 cannot hold; it is an exact tie
     between 2^53 and 2^53 + 2, and even wins. */
  TEST_ASSERT(ra8_num_decimal_to_binary64(UINT64_C(9007199254740993), 0, false, &v));
  TEST_ASSERT_EQ(UINT64_C(0x4340000000000000), bits_of(v));
  /* 2^53 + 3 is an exact tie the other way: 2^53 + 4 is the even neighbour. */
  TEST_ASSERT(ra8_num_decimal_to_binary64(UINT64_C(9007199254740995), 0, false, &v));
  TEST_ASSERT_EQ(UINT64_C(0x4340000000000002), bits_of(v));
  /* Below half truncates, above half rounds up, at the same magnitude. */
  TEST_ASSERT(ra8_num_decimal_to_binary64(UINT64_C(9007199254740994), 0, false, &v));
  TEST_ASSERT_EQ(UINT64_C(0x4340000000000001), bits_of(v));
  TEST_ASSERT(ra8_num_decimal_to_binary64(UINT64_C(9007199254740996), 0, false, &v));
  TEST_ASSERT_EQ(UINT64_C(0x4340000000000002), bits_of(v));
  TEST_END("exact ties round to even");
}

/**
 * @par MC/DC:
 * Decision: `exponent >= k_ra8_num_binary64_exponent_min`, the normal/subnormal
 * selector in ra8_num_decimal_to_binary64.
 * - T: the smallest normal keeps its implicit leading bit.
 * - F: a subnormal and the smallest subnormal encode without one.
 */
static void test_subnormal_boundary(void)
{
  TEST_BEGIN("the subnormal boundary encodes both ways");
  double v = 0.0;
  /* DBL_MIN, the smallest normal. */
  TEST_ASSERT(ra8_num_decimal_to_binary64(UINT64_C(22250738585072014), -324, false, &v));
  TEST_ASSERT_EQ(UINT64_C(0x0010000000000000), bits_of(v));
  /* The largest subnormal, one ulp below it. */
  TEST_ASSERT(ra8_num_decimal_to_binary64(UINT64_C(22250738585072009), -324, false, &v));
  TEST_ASSERT_EQ(UINT64_C(0x000FFFFFFFFFFFFF), bits_of(v));
  /* The smallest subnormal, 5e-324, and its negative. */
  TEST_ASSERT(ra8_num_decimal_to_binary64(5U, -324, false, &v));
  TEST_ASSERT_EQ(UINT64_C(0x0000000000000001), bits_of(v));
  TEST_ASSERT(ra8_num_decimal_to_binary64(5U, -324, true, &v));
  TEST_ASSERT_EQ(UINT64_C(0x8000000000000001), bits_of(v));
  TEST_END("the subnormal boundary encodes both ways");
}

/**
 * @par MC/DC:
 * Decision: `(out == nullptr) || (decimal_scale < -k_ra8_num_decimal_scale_max) ||
 * (decimal_scale > k_ra8_num_decimal_scale_max)`, the entry guard.
 * - T,-,-: a null destination is refused.
 * - F,T,-: a scale below the negative bound is refused.
 * - F,F,T: a scale above the positive bound is refused.
 * - F,F,F: a scale exactly on each bound is accepted.
 */
static void test_refusals(void)
{
  TEST_BEGIN("out-of-contract inputs are refused");
  double v = 12345.0;
  TEST_ASSERT(!ra8_num_decimal_to_binary64(1U, 0, false, nullptr));
  TEST_ASSERT(!ra8_num_decimal_to_binary64(1U, -(k_ra8_num_decimal_scale_max + 1), false, &v));
  TEST_ASSERT(!ra8_num_decimal_to_binary64(1U, k_ra8_num_decimal_scale_max + 1, false, &v));
  /* A refusal writes nothing. */
  TEST_ASSERT_EQ(bits_of(12345.0), bits_of(v));
  /* Both bounds themselves are inside the contract; they overflow or
     underflow binary64 rather than being rejected on scale. */
  TEST_ASSERT(!ra8_num_decimal_to_binary64(1U, k_ra8_num_decimal_scale_max, false, &v));
  TEST_ASSERT(!ra8_num_decimal_to_binary64(1U, -k_ra8_num_decimal_scale_max, false, &v));
  /* A zero mantissa is a signed zero at any accepted scale. */
  TEST_ASSERT(ra8_num_decimal_to_binary64(0U, k_ra8_num_decimal_scale_max, true, &v));
  TEST_ASSERT_EQ(UINT64_C(0x8000000000000000), bits_of(v));
  TEST_END("out-of-contract inputs are refused");
}

/**
 * @par MC/DC:
 * Decision: `exponent > k_ra8_num_binary64_exponent_max`, the overflow refusal.
 * - T: past DBL_MAX is refused rather than becoming an infinity.
 * - F: DBL_MAX itself converts.
 */
static void test_finite_range_ends(void)
{
  TEST_BEGIN("the finite range ends in a refusal, never an infinity");
  double v = 0.0;
  /* DBL_MAX. */
  TEST_ASSERT(ra8_num_decimal_to_binary64(UINT64_C(17976931348623157), 292, false, &v));
  TEST_ASSERT_EQ(UINT64_C(0x7FEFFFFFFFFFFFFF), bits_of(v));
  /* One decimal step past it is refused, not rounded to infinity. */
  TEST_ASSERT(!ra8_num_decimal_to_binary64(UINT64_C(17976931348623159), 292, false, &v));
  TEST_ASSERT(!ra8_num_decimal_to_binary64(2U, 308, false, &v));
  /* Below the smallest subnormal is refused rather than flushed to zero. */
  TEST_ASSERT(!ra8_num_decimal_to_binary64(1U, -400, false, &v));
  TEST_END("the finite range ends in a refusal, never an infinity");
}

int main(void)
{
  test_exact_small_integers();
  test_inexact_decimals_round_to_nearest();
  test_ties_round_to_even();
  test_subnormal_boundary();
  test_refusals();
  test_finite_range_ends();
  return 0;
}
