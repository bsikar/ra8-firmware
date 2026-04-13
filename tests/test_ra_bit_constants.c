/**
 * @file test_ra_bit_constants.c
 * @brief Unit tests for ra_bit_constants.h named bit positions
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_bit_constants.h"
#include "unity_minimal.h"

static void test_bit_positions_monotonic(void)
{
  TEST_BEGIN("bit positions are monotonic");
  TEST_ASSERT_EQ(0, (int)k_ra_bit_0);
  TEST_ASSERT_EQ(7, (int)k_ra_bit_7);
  TEST_ASSERT_EQ(15, (int)k_ra_bit_15);
  TEST_ASSERT_EQ(31, (int)k_ra_bit_31);
  TEST_END("bit positions are monotonic");
}

static void test_byte_masks(void)
{
  TEST_BEGIN("byte masks hold");
  TEST_ASSERT_EQ(0x0F, (int)k_ra_mask_nibble);
  TEST_ASSERT_EQ(0xFF, (int)k_ra_mask_byte);
  TEST_ASSERT_EQ(0xFFFF, (int)k_ra_mask_word);
  TEST_ASSERT_EQ(0x03, (int)k_ra_mask_lsn2);
  TEST_ASSERT_EQ(0x1F, (int)k_ra_mask_lsn5);
  TEST_END("byte masks hold");
}

static void test_bits_per_type(void)
{
  TEST_BEGIN("bits per integer type");
  TEST_ASSERT_EQ(8, (int)k_ra_bits_per_byte);
  TEST_ASSERT_EQ(16, (int)k_ra_bits_per_u16);
  TEST_ASSERT_EQ(32, (int)k_ra_bits_per_u32);
  TEST_ASSERT_EQ(64, (int)k_ra_bits_per_u64);
  TEST_END("bits per integer type");
}

int32_t main(void)
{
  test_bit_positions_monotonic();
  test_byte_masks();
  test_bits_per_type();
  (void)fprintf(stderr, "[OK  ] test_ra_bit_constants.c\n");
  return 0;
}
