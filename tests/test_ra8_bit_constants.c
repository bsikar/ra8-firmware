/**
 * @file test_ra8_bit_constants.c
 * @brief Unit tests for ra8_bit_constants.h named bit positions
 * @details Verifies the public named-bit constants retain their exact monotonic positions and packed-mask semantics.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_bit_constants.h"
#include "unity_minimal.h"

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_bit_positions_monotonic(void)
{
  TEST_BEGIN("bit positions are monotonic");
  TEST_ASSERT_EQ(0, k_ra8_bit_0);
  TEST_ASSERT_EQ(7, k_ra8_bit_7);
  TEST_ASSERT_EQ(15, k_ra8_bit_15);
  TEST_ASSERT_EQ(31, k_ra8_bit_31);
  TEST_END("bit positions are monotonic");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_byte_masks(void)
{
  TEST_BEGIN("byte masks hold");
  TEST_ASSERT_EQ(0x0F, k_ra8_mask_nibble);
  TEST_ASSERT_EQ(0xFF, k_ra8_mask_byte);
  TEST_ASSERT_EQ(0xFFFF, k_ra8_mask_word);
  TEST_ASSERT_EQ(0x03, k_ra8_mask_lsn2);
  TEST_ASSERT_EQ(0x1F, k_ra8_mask_lsn5);
  TEST_END("byte masks hold");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_bits_per_type(void)
{
  TEST_BEGIN("bits per integer type");
  TEST_ASSERT_EQ(8, k_ra8_bits_per_byte);
  TEST_ASSERT_EQ(16, k_ra8_bits_per_u16);
  TEST_ASSERT_EQ(32, k_ra8_bits_per_u32);
  TEST_ASSERT_EQ(64, k_ra8_bits_per_u64);
  TEST_END("bits per integer type");
}

int main(void)
{
  test_bit_positions_monotonic();
  test_byte_masks();
  test_bits_per_type();
  return 0;
}
