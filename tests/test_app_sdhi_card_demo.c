/**
 * @file test_app_sdhi_card_demo.c
 * @brief Integration test: native SDHI card demo round-trip logic (pure)
 *
 * @details
 * Mirrors examples/ek_ra8d2/sdhi_card_demo/main.c. The SDHI command sequencing
 * + ra_fs FAT logic are owned + covered by ra_sdcard / ra_sdhi / ra_fs unit
 * tests; this test pins down the app-level pure logic: the fixed-seed payload
 * pattern and the "read-back matches" round-trip verdict.
 *
 * No ra_sim_mmap MMIO is required, so this test runs in-process.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "unity_minimal.h"

typedef enum : uint32_t {
  k_t_sdhi_xor     = 0xA5A5A5A5U,
  k_t_sdhi_payload = 512U,
} t_sdhi_const_t;

/** @brief Mirror of sdhi_demo_pattern(). */
static uint32_t pattern(uint32_t i)
{
  return (i * i) ^ (uint32_t)k_t_sdhi_xor;
}

/** @brief Mirror of sdhi_demo_fill(). */
static void fill(uint8_t* buf, uint32_t len)
{
  for (uint32_t i = 0U; i < len; ++i) {
    buf[i] = (uint8_t)(pattern(i) & 0xFFU);
  }
}

/** @brief Mirror of the demo's verdict: length match AND byte-exact compare. */
static uint8_t compute_ok(const uint8_t* a, const uint8_t* b, uint32_t got, uint32_t len)
{
  uint8_t match = (got == len) ? 1U : 0U;
  for (uint32_t i = 0U; i < len; ++i) {
    if (a[i] != b[i]) {
      match = 0U;
    }
  }
  return match;
}

/**
 * @brief A faithful read-back gives ok=1; a corrupted byte or short read gives 0.
 *
 * @par MC/DC:
 * Verdict ``ok = (got == len) && byte_exact`` (2 conditions). N+1 = 3 vectors:
 * - Vector 1: got==len, bytes equal      -> 1 (control: both true)
 * - Vector 2: got!=len, bytes equal      -> 0 (varies length only)
 * - Vector 3: got==len, one byte differs -> 0 (varies compare only)
 * Vectors 1+2 prove length independently flips the outcome; 1+3 the compare.
 */
static void test_sdhi_app_roundtrip_verdict(void)
{
  TEST_BEGIN("sdhi_card_demo: round-trip verdict MC/DC");
  uint8_t payload[k_t_sdhi_payload];
  uint8_t readback[k_t_sdhi_payload];
  fill(payload, (uint32_t)k_t_sdhi_payload);

  /* Vector 1: faithful read-back. */
  for (uint32_t i = 0U; i < (uint32_t)k_t_sdhi_payload; ++i) {
    readback[i] = payload[i];
  }
  TEST_ASSERT_EQ(
    1U,
    compute_ok(payload, readback, (uint32_t)k_t_sdhi_payload, (uint32_t)k_t_sdhi_payload));

  /* Vector 2: short read (length mismatch), bytes otherwise equal. */
  TEST_ASSERT_EQ(
    0U,
    compute_ok(payload, readback, (uint32_t)k_t_sdhi_payload - 1U, (uint32_t)k_t_sdhi_payload));

  /* Vector 3: full length, one corrupted byte. */
  readback[100] ^= 0xFFU;
  TEST_ASSERT_EQ(
    0U,
    compute_ok(payload, readback, (uint32_t)k_t_sdhi_payload, (uint32_t)k_t_sdhi_payload));
  TEST_END("sdhi_card_demo: round-trip verdict MC/DC");
}

/**
 * @brief The payload pattern is deterministic + byte-truncated from the word.
 *
 * @par MC/DC:
 * No compound decision; confirms fill() lays down (i*i ^ XOR) & 0xFF per byte.
 */
static void test_sdhi_app_pattern(void)
{
  TEST_BEGIN("sdhi_card_demo: payload pattern");
  uint8_t buf[k_t_sdhi_payload];
  fill(buf, (uint32_t)k_t_sdhi_payload);
  TEST_ASSERT_EQ((uint8_t)(pattern(0U) & 0xFFU), buf[0]);
  TEST_ASSERT_EQ((uint8_t)(pattern(7U) & 0xFFU), buf[7]);
  TEST_ASSERT_EQ((uint8_t)(pattern(511U) & 0xFFU), buf[511]);
  TEST_END("sdhi_card_demo: payload pattern");
}

int main(void)
{
  test_sdhi_app_roundtrip_verdict();
  test_sdhi_app_pattern();
  return 0;
}
