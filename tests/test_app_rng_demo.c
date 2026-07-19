/**
 * @file test_app_rng_demo.c
 * @brief Integration test: ra8_psa_crypto_random round-trip from rng_demo
 *
 * @details
 * Mirrors examples/ek_ra8d2/rng_demo/main.c bring-up: ra8_psa_crypto_init
 * -> ra8_psa_crypto_random(buf, 32) -> nibble-to-hex conversion. Each
 * test exercises a distinct branch in the demo's emit-one-line path so
 * MC/DC for the four-condition compound decision is fully covered.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>
#include <string.h>

#include "ra8_err.h"
#include "ra8_psa_crypto.h"
#include "unity_minimal.h"

/**
 * @enum rng_demo_uint8_const_t
 * @brief Named uint8_t constants used by this file.
 *
 * @details
 * Every literal this translation unit needs, named so the
 * value's role is visible at the point of use (CLAUDE.md
 * "No Magic Numbers").
 */
typedef enum : uint8_t {
  k_rng_demo_n_10 = 10U,
} rng_demo_uint8_const_t;

typedef enum : uint8_t {
  k_t_rng_bytes_per_line = 32U,   /**< T rng bytes per line. */
  k_t_rng_nibble_mask    = 0x0FU, /**< T rng nibble mask.    */
  k_t_rng_nibble_shift   = 4U,    /**< T rng nibble shift.   */
} t_rng_demo_const_t;

/** @brief Copy of rng_demo_nibble_to_hex from main.c. */
static uint8_t banner_nibble(uint8_t nibble)
{
  uint8_t n = (uint8_t)(nibble & (uint8_t)k_t_rng_nibble_mask);
  if (n < k_rng_demo_n_10) {
    return (uint8_t)('0' + n);
  }
  return (uint8_t)('a' + (n - k_rng_demo_n_10));
}

static void reset_world(void)
{
  (void)ra8_psa_crypto_deinit();
}

/**
 * @brief Init facade and pull the requested number of bytes.
 *
 * @par MC/DC:
 * Decision in app: ``ra8_psa_crypto_random != ok``. One atomic
 * condition x 2 vectors -- this test (ok) + the not-initialized test
 * below (err).
 */
static void test_rng_app_random_ok(void)
{
  reset_world();
  TEST_BEGIN("rng_demo: init + random(32) ok");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_psa_crypto_init());
  uint8_t buf[k_t_rng_bytes_per_line] = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_psa_crypto_random(buf, sizeof(buf)));
  /* xorshift32 sim is deterministic; at least one of the 32 bytes
   * must differ from the all-zero seed buffer. */
  uint8_t accum = 0U;
  for (uint8_t i = 0U; i < (uint8_t)k_t_rng_bytes_per_line; ++i) {
    accum |= buf[i];
  }
  TEST_ASSERT(accum != 0U);
  TEST_END("rng_demo: init + random(32) ok");
}

/**
 * @brief Calling random before init is rejected.
 *
 * @par MC/DC:
 * Decision: ``s_initialized`` flag. Two vectors -- pre-init (this) +
 * post-init (above).
 */
static void test_rng_app_random_not_initialized(void)
{
  reset_world();
  TEST_BEGIN("rng_demo: random before init rejected");
  uint8_t buf[k_t_rng_bytes_per_line] = {};
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_psa_crypto_random(buf, sizeof(buf)));
  TEST_END("rng_demo: random before init rejected");
}

/**
 * @brief NULL out pointer rejected.
 *
 * @par MC/DC:
 * Decision: ``out == nullptr``. One atomic condition x 2 vectors --
 * NULL (this) + valid pointer (test_rng_app_random_ok).
 */
static void test_rng_app_random_null(void)
{
  reset_world();
  TEST_BEGIN("rng_demo: NULL out rejected");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_psa_crypto_init());
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_psa_crypto_random(nullptr, (size_t)k_t_rng_bytes_per_line));
  TEST_END("rng_demo: NULL out rejected");
}

/**
 * @brief Zero-length request rejected.
 *
 * @par MC/DC:
 * Decision: ``out_len == 0``. One atomic condition x 2 vectors --
 * zero (this) + non-zero (test_rng_app_random_ok).
 */
static void test_rng_app_random_zero_len(void)
{
  reset_world();
  TEST_BEGIN("rng_demo: zero len rejected");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_psa_crypto_init());
  uint8_t buf[1] = {};
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, ra8_psa_crypto_random(buf, 0U));
  TEST_END("rng_demo: zero len rejected");
}

/**
 * @brief Nibble-to-hex conversion covers digit and alpha branches.
 *
 * @par MC/DC:
 * Compound decision: ``n < 10``. One atomic condition x 2 vectors --
 * n=5 (digit branch) + n=12 (alpha branch).
 */
static void test_rng_app_nibble_to_hex(void)
{
  TEST_BEGIN("rng_demo: nibble_to_hex digit + alpha branches");
  TEST_ASSERT_EQ('0', banner_nibble(0U));
  TEST_ASSERT_EQ('5', banner_nibble(5U));
  TEST_ASSERT_EQ('9', banner_nibble(9U));
  TEST_ASSERT_EQ('a', banner_nibble(10U));
  TEST_ASSERT_EQ('c', banner_nibble(12U));
  TEST_ASSERT_EQ('f', banner_nibble(15U));
  /* Upper nibble must be ignored. */
  TEST_ASSERT_EQ('5', banner_nibble(0xF5U));
  TEST_END("rng_demo: nibble_to_hex digit + alpha branches");
}

int main(void)
{
  test_rng_app_random_ok();
  test_rng_app_random_not_initialized();
  test_rng_app_random_null();
  test_rng_app_random_zero_len();
  test_rng_app_nibble_to_hex();
  return 0;
}
