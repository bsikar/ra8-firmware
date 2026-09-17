/**
 * @file test_ra8_rsip_sha256_mcdc.c
 * @brief MC/DC vectors for the streaming SHA-256 update contract.
 * @details Drives each compound decision in the public update path with
 * independent-condition vectors while keeping the production state machine
 * and compression implementation under test.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_rsip.h"
#include "unity_minimal.h"

/**
 * @brief Cover the nullable-data compound decision.
 * @details Supplies valid data, a permitted empty NULL span, and a rejected
 * nonempty NULL span so each condition independently changes the outcome.
 * @par MC/DC:
 * `libs/ra8_hal/src/ra8_rsip.c@ra8_rsip_sha256_update`
 * - V1: data non-NULL, len 1 -- C1 false, decision false.
 * - V2: data NULL, len 0 -- C1 true, C2 false, decision false.
 * - V3: data NULL, len 1 -- C1 true, C2 true, decision true.
 * @pre The production SHA-256 implementation is linked into this test.
 * @pre The local context is uninitialized on entry.
 * @post All three documented decisions return their canonical status.
 * @post No storage outside the local context is modified.
 * @note File-local test helper.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_data_span_mcdc(void)
{
  const uint8_t         data = 0U;
  ra8_rsip_sha256_ctx_t ctx  = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rsip_sha256_init(&ctx));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rsip_sha256_update(&ctx, &data, 1U));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rsip_sha256_update(&ctx, nullptr, 0U));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_rsip_sha256_update(&ctx, nullptr, 1U));
}

/**
 * @brief Cover the accumulated-length overflow compound decision.
 * @details Seeds valid initialized contexts immediately below, at, and above
 * the maximum representable SHA-256 byte count before appending one byte.
 * @par MC/DC:
 * `libs/ra8_hal/src/ra8_rsip.c@ra8_rsip_sha256_update`
 * - V1: total max-1, len 1 -- C1 false, C2 false, decision false.
 * - V2: total max, len 1 -- C1 false, C2 true, decision true.
 * - V3: total max+1, len 1 -- C1 true, decision true.
 * @pre The production SHA-256 implementation is linked into this test.
 * @pre Each context is initialized before its accounting field is seeded.
 * @post Only the representable append succeeds.
 * @post Rejected contexts do not consume the supplied byte.
 * @note File-local test helper.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_length_overflow_mcdc(void)
{
  const uint64_t        max_bytes = UINT64_MAX / UINT64_C(8);
  const uint8_t         data      = 0U;
  ra8_rsip_sha256_ctx_t below     = {};
  ra8_rsip_sha256_ctx_t at        = {};
  ra8_rsip_sha256_ctx_t above     = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rsip_sha256_init(&below));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rsip_sha256_init(&at));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rsip_sha256_init(&above));
  below.total_bytes = max_bytes - 1U;
  at.total_bytes    = max_bytes;
  above.total_bytes = max_bytes + 1U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rsip_sha256_update(&below, &data, 1U));
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, ra8_rsip_sha256_update(&at, &data, 1U));
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, ra8_rsip_sha256_update(&above, &data, 1U));
}

/**
 * @brief Cover the partial-block copy loop compound condition.
 * @details Contrasts an empty partial block with a one-byte partial block and
 * lets the latter loop terminate because the one-byte input is consumed.
 * @par MC/DC:
 * `libs/ra8_hal/src/ra8_rsip.c@ra8_rsip_sha256_update`
 * - V1: used 0, consumed below len -- C1 false, decision false.
 * - V2a: used 1, consumed below len -- C1 true, C2 true, decision true.
 * - V2b: used 2, consumed equals len -- C1 true, C2 false, decision false.
 * @pre The production SHA-256 implementation is linked into this test.
 * @pre Both local contexts are initialized before their update.
 * @post Both one-byte updates succeed.
 * @post The seeded partial block consumes exactly one new byte.
 * @note File-local test helper.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_partial_block_loop_mcdc(void)
{
  const uint8_t         data    = 0U;
  ra8_rsip_sha256_ctx_t empty   = {};
  ra8_rsip_sha256_ctx_t partial = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rsip_sha256_init(&empty));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rsip_sha256_init(&partial));
  partial.used = 1U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rsip_sha256_update(&empty, &data, 1U));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rsip_sha256_update(&partial, &data, 1U));
  TEST_ASSERT_EQ(2, partial.used);
}

/**
 * @brief Run streaming SHA-256 MC/DC vectors.
 * @details Executes the three independent vector groups in deterministic order.
 * @return Process status.
 * @retval 0 Every assertion passed.
 * @pre The executable links the production RSIP software SHA-256 path.
 * @pre No concurrent caller mutates test state.
 * @post Every compound decision in the update path has an MC/DC citation.
 * @post No ownership escapes the process.
 * @note Hosted test entry point.
 * @since 0.1.0
 */
int main(void)
{
  internal_test_data_span_mcdc();
  internal_test_length_overflow_mcdc();
  internal_test_partial_block_loop_mcdc();
  return 0;
}
