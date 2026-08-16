/**
 * @file ra8_c6link_transfer_validation_test.c
 * @brief Isolated transfer-configuration validation vectors
 * @details Uses a begin callback that stops the valid control before transport,
 * then independently removes every required value, callback, context, or bound.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8_c6link_transfer_validation_test_internal.h"
#include "ra8_err.h"
#include "unity_minimal.h"

/** @brief Count storage begins reached after successful validation. */
static uint32_t s_validation_begins;

/**
 * @brief Stop the all-valid control immediately after validation.
 * @details Separates successful validation from transport behavior by
 * returning a deterministic status at the first injected operation.
 * @param[in,out] ctx Counter incremented for each reached begin.
 * @param[in] destination Non-null destination accepted by validation.
 * @return Deliberate terminal status for the control vector.
 * @retval k_ra8_err_busy Validation succeeded and begin was reached.
 * @pre @p ctx addresses writable counter storage.
 * @pre @p destination is a valid NUL-terminated string.
 * @post The counter is incremented exactly once.
 * @post No transport, hash, or publication callback runs.
 * @note Test-only fault seam.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_validation_begin(void* ctx, const char* destination)
{
  (void)destination;
  uint32_t* begins = (uint32_t*)ctx;
  *begins += 1U;
  return k_ra8_err_busy;
}

/**
 * @brief Exercise the five required top-level transfer values.
 * @details Uses one complete control and five otherwise-identical null variants
 * while tracking whether validation reached the begin callback.
 * @param[in,out] link Valid link for the all-present control.
 * @param[in] base Complete configuration with the test begin seam installed.
 * @pre @p link and @p base are non-null.
 * @pre The result and string literals remain valid throughout the calls.
 * @post The complete control reaches begin exactly once.
 * @post Each single-null variant returns `k_ra8_err_null_ptr` before begin.
 * @note Does not inspect or mutate transport state.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_required_values(ra8_c6link_t*                    link,
                                                       const ra8_mdl_transfer_config_t* base)
{
  ra8_mdl_transfer_result_t result = {};
  s_validation_begins              = 0U;
  TEST_ASSERT_EQ(
    k_ra8_err_busy,
    ra8_c6link_mdl_transfer(link, "https://example.test/book", "/book", base, &result));
  TEST_ASSERT_EQ((int64_t)1, (int64_t)s_validation_begins);

  s_validation_begins = 0U;
  TEST_ASSERT_EQ(
    k_ra8_err_null_ptr,
    ra8_c6link_mdl_transfer(nullptr, "https://example.test/book", "/book", base, &result));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_c6link_mdl_transfer(link, nullptr, "/book", base, &result));
  TEST_ASSERT_EQ(
    k_ra8_err_null_ptr,
    ra8_c6link_mdl_transfer(link, "https://example.test/book", nullptr, base, &result));
  TEST_ASSERT_EQ(
    k_ra8_err_null_ptr,
    ra8_c6link_mdl_transfer(link, "https://example.test/book", "/book", nullptr, &result));
  TEST_ASSERT_EQ(
    k_ra8_err_null_ptr,
    ra8_c6link_mdl_transfer(link, "https://example.test/book", "/book", base, nullptr));
  TEST_ASSERT_EQ((int64_t)0, (int64_t)s_validation_begins);
}

/**
 * @brief Exercise every required injected callback and context.
 * @details Restores the complete base before clearing each required mechanism,
 * so every rejection differs from the control by one operand.
 * @param[in,out] link Valid link that no rejected vector reaches.
 * @param[in] base Complete configuration with the test begin seam installed.
 * @pre @p link and @p base are non-null.
 * @pre Every callback and context in @p base is initially non-null.
 * @post Each isolated null returns `k_ra8_err_null_ptr`.
 * @post No rejected vector reaches storage begin.
 * @note Optional storage validation and cancellation remain deliberately null.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_required_callbacks(ra8_c6link_t*                    link,
                                                          const ra8_mdl_transfer_config_t* base)
{
  ra8_mdl_transfer_result_t result = {};
  ra8_mdl_transfer_config_t config = *base;
  /** @def EXPECT_NULL @brief Clear and reject one required config member. */
#define EXPECT_NULL(member)                                                                        \
  do {                                                                                             \
    config        = *base;                                                                         \
    config.member = nullptr;                                                                       \
    TEST_ASSERT_EQ(                                                                                \
      k_ra8_err_null_ptr,                                                                          \
      ra8_c6link_mdl_transfer(link, "https://example.test/book", "/book", &config, &result));      \
  } while (0)
  s_validation_begins = 0U;
  EXPECT_NULL(storage.begin);
  EXPECT_NULL(storage.write);
  EXPECT_NULL(storage.commit);
  EXPECT_NULL(storage.abort);
  EXPECT_NULL(storage.ctx);
  EXPECT_NULL(sha256.init);
  EXPECT_NULL(sha256.update);
  EXPECT_NULL(sha256.final);
  EXPECT_NULL(sha256.ctx);
#undef EXPECT_NULL
  TEST_ASSERT_EQ((int64_t)0, (int64_t)s_validation_begins);
}

/**
 * @brief Exercise every transfer-bound operand independently.
 * @details Restores the complete base before selecting zero chunk size,
 * oversize chunk size, zero pull budget, and an excessive total byte budget
 * in turn.
 * @param[in,out] link Valid link that no rejected vector reaches.
 * @param[in] base Complete configuration with the test begin seam installed.
 * @pre @p link and @p base are non-null.
 * @pre @p base has a legal chunk size and non-zero chunk limit.
 * @post Zero and oversize chunks return `k_ra8_err_invalid_size`.
 * @post Zero and excessive chunk-count budgets return `k_ra8_err_invalid_size` before begin.
 * @note No transport or storage state is created.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_transfer_bounds(ra8_c6link_t*                    link,
                                                       const ra8_mdl_transfer_config_t* base)
{
  ra8_mdl_transfer_result_t result = {};
  ra8_mdl_transfer_config_t config = *base;
  config.chunk_bytes               = 0U;
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_size,
    ra8_c6link_mdl_transfer(link, "https://example.test/book", "/book", &config, &result));
  config             = *base;
  config.chunk_bytes = k_ra8_mdl_chunk_data_max + 1U;
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_size,
    ra8_c6link_mdl_transfer(link, "https://example.test/book", "/book", &config, &result));
  config            = *base;
  config.max_chunks = 0U;
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_size,
    ra8_c6link_mdl_transfer(link, "https://example.test/book", "/book", &config, &result));
  config             = *base;
  config.chunk_bytes = k_ra8_mdl_chunk_data_max;
  config.max_chunks  = (uint32_t)(k_ra8_mdl_transfer_bytes_max / k_ra8_mdl_chunk_data_max) + 1U;
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_size,
    ra8_c6link_mdl_transfer(link, "https://example.test/book", "/book", &config, &result));
  TEST_ASSERT_EQ((int64_t)0, (int64_t)s_validation_begins);
}

RA8_PRIV void priv_test_c6link_transfer_validation_run(ra8_c6link_t*                    link,
                                                       const ra8_mdl_transfer_config_t* base)
{
  ra8_mdl_transfer_config_t validation = *base;
  validation.storage.begin             = internal_validation_begin;
  validation.storage.ctx               = &s_validation_begins;
  internal_test_required_values(link, &validation);
  internal_test_required_callbacks(link, &validation);
  internal_test_transfer_bounds(link, &validation);
}
