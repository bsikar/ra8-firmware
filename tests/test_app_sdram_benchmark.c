/**
 * @file test_app_sdram_benchmark.c
 * @brief Integration test: SDRAM init + mbps helper bench math
 *
 * @details
 * Mirrors examples/ek_ra8d2/sdram_benchmark/main.c bring-up:
 * ra8_sdramc_init -> get_status. The mbps helper math is exercised
 * directly so MC/DC vectors land on the divide-by-zero clamp.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_pin_validator.h"
#include "ra8_sdramc.h"
#include "unity_minimal.h"

/**
 * @enum app_sdram_benchmark_fixture_t
 * @brief Loop bounds and counts, sized so the case under test is actually reached.
 */
typedef enum : uint16_t {
  k_ms_per_second =
    1000U, /**< Milliseconds per second, converting a byte count and a duration into a rate. */
} app_sdram_benchmark_fixture_t;

typedef enum : uint32_t {
  k_test_sdram_app_block_bytes = 65536U, /**< Test SDRAM app block bytes. */
} test_sdram_app_const_t;

static void reset_world(void)
{
  ra8_fake_mmap_reset();
  ra8_pin_validator_reset();
}

/** @brief Copy of the app helper to exercise the divide-by-zero clamp. */
static uint32_t app_mbps(uint32_t bytes, uint32_t ms)
{
  const uint32_t ms_clamped = (ms == 0U) ? 1U : ms;
  return bytes / (ms_clamped * k_ms_per_second);
}

/**
 * @brief ra8_sdramc_init succeeds in off-target mode.
 *
 * @par MC/DC:
 * Decision: ``ra8_sdramc_init != ok``. One atomic condition x 2
 * vectors -- ok (this) + status NULL-out below covers the get_status
 * NULL guard.
 */
static void test_sdram_app_init_ok(void)
{
  reset_world();
  TEST_BEGIN("sdram_benchmark: init ok");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sdramc_init());
  TEST_END("sdram_benchmark: init ok");
}

/**
 * @brief ra8_sdramc_get_status with NULL is rejected.
 *
 * @par MC/DC:
 * Decision: ``out_enabled == nullptr``. One atomic condition x 2
 * vectors -- NULL (this) + non-NULL (status_ok).
 */
static void test_sdram_app_status_null_rejected(void)
{
  reset_world();
  TEST_BEGIN("sdram_benchmark: get_status NULL rejected");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sdramc_init());
  TEST_ASSERT(ra8_sdramc_get_status(nullptr) != k_ra8_ok);
  TEST_END("sdram_benchmark: get_status NULL rejected");
}

/**
 * @brief Status read after init returns ok with a valid mask byte.
 *
 * @par MC/DC:
 * Pairs with the NULL-out test above for full N+1 = 2 coverage of
 * the get_status null check.
 */
static void test_sdram_app_status_ok(void)
{
  reset_world();
  TEST_BEGIN("sdram_benchmark: get_status ok");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sdramc_init());
  uint8_t enabled = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sdramc_get_status(&enabled));
  TEST_END("sdram_benchmark: get_status ok");
}

/**
 * @brief mbps helper covers the ms==0 clamp + non-zero math branches.
 *
 * @par MC/DC:
 * Decision: ``ms == 0``. One atomic condition x 2 vectors --
 * ms==0 (this, clamps to 1) + ms!=0 (this, normal divide).
 */
static void test_sdram_app_mbps_branches(void)
{
  TEST_BEGIN("sdram_benchmark: mbps clamps ms==0");
  /* ms==0 path: 64 KB / (1 * 1000) = 65 (truncated). */
  TEST_ASSERT_EQ(65, app_mbps((uint32_t)k_test_sdram_app_block_bytes, 0U));
  /* ms!=0 path: 64 KB / (10 * 1000) = 6 (truncated). */
  TEST_ASSERT_EQ(6, app_mbps((uint32_t)k_test_sdram_app_block_bytes, 10U));
  TEST_END("sdram_benchmark: mbps clamps ms==0");
}

int main(void)
{
  test_sdram_app_init_ok();
  test_sdram_app_status_null_rejected();
  test_sdram_app_status_ok();
  test_sdram_app_mbps_branches();
  return 0;
}
