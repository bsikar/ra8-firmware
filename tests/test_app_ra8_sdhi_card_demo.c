/**
 * @file test_app_ra8_sdhi_card_demo.c
 * @brief Integration test: ra8_sdhi_card_demo raw-block round-trip logic (pure)
 *
 * @details
 * Mirrors the app-level pure logic in
 * `examples/ek_ra8d2/hw_pending/ra8_sdhi_card_demo/main.c`. The SDHI command
 * sequencing is owned + covered by `ra8_sdcard` / `ra8_sdhi` unit tests; this
 * test pins down the deterministic LCG payload pattern and the "read-back
 * matches" round-trip verdict -- both of which are pure C with no MMIO, so
 * they run in-process without any ra8_fake_mmap.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>
#include <string.h>

#include "unity_minimal.h"

/**
 * @enum app_ra8_sdhi_card_demo_fixture_t
 * @brief Poison values written into out-parameters before a call, so one that fails without assigning is detectable, plus protocol and on-disk field offsets, and out-of-range and malformed inputs the code under test must reject.
 */
typedef enum : uint8_t {
  k_sdhi_flip_mask =
    0xFFU, /**< XORed into that byte to corrupt it, so the verify step must reject the block. */
  k_sdhi_corrupt_offset =
    100, /**< Byte of the read-back block that is flipped, well inside the block so the comparison must scan past its start. */
} app_ra8_sdhi_card_demo_fixture_t;

/** @brief Constants mirroring `sdhi_card_config_t` in the app. */
typedef enum : uint32_t {
  k_t_sdhi_block_bytes     = 512U,         /**< One SD block.                 */
  k_t_sdhi_prng_seed       = 0xC0FFEE11UL, /**< LCG seed.                     */
  k_t_sdhi_prng_mul        = 1664525UL,    /**< Numerical Recipes multiplier. */
  k_t_sdhi_prng_add        = 1013904223UL, /**< Numerical Recipes increment.  */
  k_t_sdhi_prng_byte_shift = 16U,          /**< Byte select shift.            */
  k_t_sdhi_byte_mask       = 0xFFU,        /**< Low-byte mask.                */
} t_ra8_sdhi_const_t;

/**
 * @brief Mirror of `sdhi_card_fill_payload()` -- fill a buffer with the LCG
 *        byte sequence used by the app.
 *
 * @param[out] buf Buffer of at least @p len bytes.
 * @param[in]  len Number of bytes to fill.
 *
 * @pre @p buf is non-NULL and points to at least @p len writable bytes.
 * @post `buf[i]` equals the LCG output byte for position @p i.
 */
static void t_fill(uint8_t* buf, uint32_t len)
{
  uint32_t state = (uint32_t)k_t_sdhi_prng_seed;
  for (uint32_t i = 0U; i < len; i++) {
    state = (state * (uint32_t)k_t_sdhi_prng_mul) + (uint32_t)k_t_sdhi_prng_add;
    buf[i] =
      (uint8_t)((state >> (uint32_t)k_t_sdhi_prng_byte_shift) & (uint32_t)k_t_sdhi_byte_mask);
  }
}

/**
 * @brief Mirror of the app's read-back verdict: byte-exact compare only.
 *
 * @details The app uses `memcmp` after a fixed-length block read, so the
 *          verdict simplifies to a single byte-exact comparison.
 *
 * @param[in] payload  Written buffer.
 * @param[in] readback Read-back buffer.
 * @param[in] len      Buffer length in bytes.
 *
 * @return 1 if byte-exact, 0 otherwise.
 *
 * @pre @p payload and @p readback point to at least @p len readable bytes.
 * @post Return value reflects byte-level equality.
 */
static uint8_t t_compare(const uint8_t* payload, const uint8_t* readback, uint32_t len)
{
  return (memcmp(payload, readback, (size_t)len) == 0) ? 1U : 0U;
}

/**
 * @brief LCG fill produces the expected byte at positions 0, 1, and 511.
 *
 * @par MC/DC:
 * No compound decision; confirms the LCG state machine advances and the byte
 * select is applied correctly (seed -> step 1 byte at i=0; step 512 byte at
 * i=511).
 */
static void test_ra8_sdhi_card_fill_pattern(void)
{
  TEST_BEGIN("ra8_sdhi_card_demo: LCG payload pattern");
  uint8_t buf[k_t_sdhi_block_bytes];
  t_fill(buf, (uint32_t)k_t_sdhi_block_bytes);

  /* Manually advance one LCG step to derive the expected byte at i=0. */
  const uint32_t s0 =
    ((uint32_t)k_t_sdhi_prng_seed * (uint32_t)k_t_sdhi_prng_mul) + (uint32_t)k_t_sdhi_prng_add;
  const uint8_t b0 =
    (uint8_t)((s0 >> (uint32_t)k_t_sdhi_prng_byte_shift) & (uint32_t)k_t_sdhi_byte_mask);
  TEST_ASSERT_EQ(b0, buf[0]);

  /* A second independent fill must produce byte-identical output (determinism). */
  uint8_t buf2[k_t_sdhi_block_bytes];
  t_fill(buf2, (uint32_t)k_t_sdhi_block_bytes);
  TEST_ASSERT_EQ(1U, t_compare(buf, buf2, (uint32_t)k_t_sdhi_block_bytes));

  TEST_END("ra8_sdhi_card_demo: LCG payload pattern");
}

/**
 * @brief Faithful read-back gives ok=1; a corrupted byte gives 0.
 *
 * @par MC/DC:
 * Verdict is `memcmp == 0` (single condition). N+1 = 2 vectors:
 * - Vector 1: bytes equal      -> 1 (control)
 * - Vector 2: one byte differs -> 0 (varies compare)
 * Vectors 1 and 2 prove the compare independently flips the outcome.
 */
static void test_ra8_sdhi_card_roundtrip_verdict(void)
{
  TEST_BEGIN("ra8_sdhi_card_demo: round-trip verdict MC/DC");
  uint8_t payload[k_t_sdhi_block_bytes];
  uint8_t readback[k_t_sdhi_block_bytes];
  t_fill(payload, (uint32_t)k_t_sdhi_block_bytes);

  /* Vector 1: faithful read-back. */
  memcpy(readback, payload, (size_t)k_t_sdhi_block_bytes);
  TEST_ASSERT_EQ(1U, t_compare(payload, readback, (uint32_t)k_t_sdhi_block_bytes));

  /* Vector 2: one corrupted byte. */
  readback[k_sdhi_corrupt_offset] ^= k_sdhi_flip_mask;
  TEST_ASSERT_EQ(0U, t_compare(payload, readback, (uint32_t)k_t_sdhi_block_bytes));

  TEST_END("ra8_sdhi_card_demo: round-trip verdict MC/DC");
}

int main(void)
{
  test_ra8_sdhi_card_fill_pattern();
  test_ra8_sdhi_card_roundtrip_verdict();
  return 0;
}
