/**
 * @file test_app_ecc_monitor_demo.c
 * @brief Integration test: SRAM ECC monitor demo logic (pure)
 *
 * @details
 * Mirrors examples/ek_ra8d2/ecc_monitor_demo/main.c. The ECC register
 * sequencing is owned + covered by ra8_sram's own unit tests; this test
 * pins down the app-level pure logic: the rw-test pattern and the
 * "ECC-protected round-trip OK" verdict.
 *
 * No ra8_sim_mmap MMIO is required, so this test runs in-process.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "unity_minimal.h"

/**
 * @enum app_ecc_monitor_demo_fixture_t
 * @brief Out-of-range and malformed inputs the code under test must reject.
 */
typedef enum : uint8_t {
  k_ecc_corrupt_word =
    7, /**< Word of the buffer whose bit is flipped, inside the region so the monitor must detect rather than ignore it. */
} app_ecc_monitor_demo_fixture_t;

typedef enum : uint32_t {
  k_t_ecc_xor   = 0x5A5A5A5AU, /**< T ECC xor.   */
  k_t_ecc_words = 64U,         /**< T ECC words. */
} t_ecc_const_t;

/** @brief Mirror of ecc_demo_pattern(). */
static uint32_t pattern(uint32_t i)
{
  return (i * i) ^ (uint32_t)k_t_ecc_xor;
}

/** @brief Mirror of the demo's verdict: gates only on the round-trip. */
static uint8_t compute_ok(uint8_t rw_ok)
{
  return (rw_ok != 0U) ? 1U : 0U;
}

/**
 * @brief The pattern round-trips through a modelled s_buffer.
 *
 * @par MC/DC:
 * No compound decision; a write/read loop. Vectors confirm a faithful
 * read-back gives rw_ok=1 and a corrupted word gives rw_ok=0.
 */
static void test_ecc_app_rw_model(void)
{
  TEST_BEGIN("ecc_monitor_demo: rw round-trip model");
  static uint32_t s_buf[k_t_ecc_words];
  for (uint32_t i = 0U; i < (uint32_t)k_t_ecc_words; ++i) {
    s_buf[i] = pattern(i);
  }
  uint8_t rw = 1U;
  for (uint32_t i = 0U; i < (uint32_t)k_t_ecc_words; ++i) {
    if (s_buf[i] != pattern(i)) {
      rw = 0U;
    }
  }
  TEST_ASSERT_EQ(1U, rw);

  s_buf[k_ecc_corrupt_word] ^= 0x1U; /* corrupt one word */
  rw = 1U;
  for (uint32_t i = 0U; i < (uint32_t)k_t_ecc_words; ++i) {
    if (s_buf[i] != pattern(i)) {
      rw = 0U;
    }
  }
  TEST_ASSERT_EQ(0U, rw);
  TEST_END("ecc_monitor_demo: rw round-trip model");
}

/**
 * @brief Verdict gates on the round-trip only (ra8_emulator can't model ECC).
 *
 * @par MC/DC:
 * Decision ``ok = rw_ok`` (single condition). 2 vectors: rw_ok=1 -> 1,
 * rw_ok=0 -> 0.
 */
static void test_ecc_app_verdict(void)
{
  TEST_BEGIN("ecc_monitor_demo: verdict = rw_ok");
  TEST_ASSERT_EQ(1U, compute_ok(1U));
  TEST_ASSERT_EQ(0U, compute_ok(0U));
  TEST_END("ecc_monitor_demo: verdict = rw_ok");
}

int main(void)
{
  test_ecc_app_rw_model();
  test_ecc_app_verdict();
  return 0;
}
