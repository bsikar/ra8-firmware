/**
 * @file test_mbedtls_psa_rng.c
 * @brief Host coverage for the HTTPS example's PSA external-RNG adapter.
 * @par Tag
 * [Ring 6 / APP] {World: S}
 * @details Compiles the production adapter without RA8_OFF_TARGET and replaces
 * only the RSIP TRNG call. Each return path is exercised without a crypto
 * backend or hardware register model.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "psa/crypto.h"
#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_rsip.h"
#include "unity_minimal.h"

/** @brief Fixed fixture sizes and sentinel bytes. */
typedef enum : uint32_t {
  k_rng_test_bytes           = 8U,    /**< Random bytes requested per success. */
  k_rng_test_pattern         = 0xA5U, /**< Byte emitted by the RSIP test seam. */
  k_rng_test_length_sentinel = 99U,   /**< Detects an unwritten output length. */
} rng_test_constant_t;

/** @brief Result returned by the deterministic RSIP seam. */
static ra8_err_t s_rng_result;
/** @brief Number of calls reaching the deterministic RSIP seam. */
static uint32_t s_rng_calls;
/** @brief Most recent requested byte count. */
static uint32_t s_rng_requested;

/**
 * @brief Test seam for the adapter's only hardware dependency.
 * @param[out] buf Destination passed by the adapter.
 * @param[in] len Requested byte count.
 * @return Configured fixture result.
 * @pre @p buf is writable for @p len bytes when success is configured.
 * @post Records the call and fills successful output with a fixed pattern.
 * @note Not thread-safe; one single-threaded test executable owns the state.
 * @since 0.1.0
 */
ra8_err_t ra8_rsip_trng_read(uint8_t* buf, uint32_t len)
{
  ++s_rng_calls;
  s_rng_requested = len;
  if (s_rng_result == k_ra8_ok) {
    (void)memset(buf, (int)k_rng_test_pattern, (size_t)len);
  }
  return s_rng_result;
}

/**
 * @brief Reset the deterministic RSIP seam.
 * @param[in] result Result returned by the next adapter call.
 * @pre No adapter call is active.
 * @post Counters are zero and @p result is installed.
 * @note Not thread-safe; used only by this single-threaded test.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_reset_rng(ra8_err_t result)
{
  s_rng_result    = result;
  s_rng_calls     = 0U;
  s_rng_requested = 0U;
}

/**
 * @brief A null output buffer is rejected before RSIP.
 * @par MC/DC:
 * No compound decisions. The output-null decision is true; later decisions
 * are not reached because the first guard returns.
 * @details Verifies the first fail-closed precondition and untouched length.
 * @pre Fixture state is writable.
 * @post The hardware seam remains uncalled.
 * @note Thread-safe because the executable runs cases serially.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_null_output(void)
{
  TEST_BEGIN("psa rng: null output");
  size_t output_length = (size_t)k_rng_test_length_sentinel;
  internal_reset_rng(k_ra8_ok);

  TEST_ASSERT_EQ(
    PSA_ERROR_INVALID_ARGUMENT,
    mbedtls_psa_external_get_random(nullptr, nullptr, (size_t)k_rng_test_bytes, &output_length));
  TEST_ASSERT_EQ(0U, s_rng_calls);
  TEST_ASSERT_EQ(k_rng_test_length_sentinel, output_length);
  TEST_END("psa rng: null output");
}

/**
 * @brief A null output-length pointer is rejected before RSIP.
 * @par MC/DC:
 * No compound decisions. The output-null decision is false and the
 * output-length-null decision is true.
 * @details Verifies the second fail-closed precondition independently.
 * @pre Fixture state is writable.
 * @post The hardware seam remains uncalled.
 * @note Thread-safe because the executable runs cases serially.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_null_length(void)
{
  TEST_BEGIN("psa rng: null output length");
  uint8_t output[k_rng_test_bytes] = {};
  internal_reset_rng(k_ra8_ok);

  TEST_ASSERT_EQ(
    PSA_ERROR_INVALID_ARGUMENT,
    mbedtls_psa_external_get_random(nullptr, output, (size_t)k_rng_test_bytes, nullptr));
  TEST_ASSERT_EQ(0U, s_rng_calls);
  TEST_ASSERT_EQ(0U, output[0]);
  TEST_END("psa rng: null output length");
}

/**
 * @brief An RSIP error maps to PSA hardware failure.
 * @par MC/DC:
 * No compound decisions. Both pointer-null decisions are false and the
 * hardware-error decision is true.
 * @details Verifies the request reaches RSIP but publishes no output length.
 * @pre Fixture state is writable.
 * @post Exactly one bounded RSIP request is recorded.
 * @note Thread-safe because the executable runs cases serially.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_hardware_failure(void)
{
  TEST_BEGIN("psa rng: hardware failure");
  uint8_t output[k_rng_test_bytes] = {};
  size_t  output_length            = (size_t)k_rng_test_length_sentinel;
  internal_reset_rng(k_ra8_err_hw_timeout);

  TEST_ASSERT_EQ(PSA_ERROR_HARDWARE_FAILURE,
                 mbedtls_psa_external_get_random(nullptr, output, sizeof(output), &output_length));
  TEST_ASSERT_EQ(1U, s_rng_calls);
  TEST_ASSERT_EQ(sizeof(output), s_rng_requested);
  TEST_ASSERT_EQ(k_rng_test_length_sentinel, output_length);
  TEST_END("psa rng: hardware failure");
}

/**
 * @brief A successful RSIP read publishes bytes and their exact length.
 * @par MC/DC:
 * No compound decisions. Both pointer-null decisions and the hardware-error
 * decision are false, completing both outcomes for all three decisions.
 * @details Verifies the adapter forwards the bounded request and output bytes.
 * @pre Fixture state is writable.
 * @post Output contains the configured pattern and its full length is reported.
 * @note Thread-safe because the executable runs cases serially.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_success(void)
{
  TEST_BEGIN("psa rng: success");
  const uint8_t expected[k_rng_test_bytes] = {
    k_rng_test_pattern,
    k_rng_test_pattern,
    k_rng_test_pattern,
    k_rng_test_pattern,
    k_rng_test_pattern,
    k_rng_test_pattern,
    k_rng_test_pattern,
    k_rng_test_pattern,
  };
  uint8_t output[k_rng_test_bytes] = {};
  size_t  output_length            = 0U;
  internal_reset_rng(k_ra8_ok);

  TEST_ASSERT_EQ(PSA_SUCCESS,
                 mbedtls_psa_external_get_random(nullptr, output, sizeof(output), &output_length));
  TEST_ASSERT_EQ(1U, s_rng_calls);
  TEST_ASSERT_EQ(sizeof(output), s_rng_requested);
  TEST_ASSERT_EQ(sizeof(output), output_length);
  TEST_ASSERT_EQ(0, memcmp(expected, output, sizeof(output)));
  TEST_END("psa rng: success");
}

int main(void)
{
  internal_test_null_output();
  internal_test_null_length();
  internal_test_hardware_failure();
  internal_test_success();
  return 0;
}
