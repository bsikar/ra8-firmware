/**
 * @file test_app_ulpt_demo.c
 * @brief Integration test: ULPT 1 Hz wake demo bring-up
 *
 * @details
 * Mirrors examples/ek_ra8d2/ulpt_demo/main.c bring-up flow:
 * ra_ulpt_init -> ra_ulpt_start -> ra_ulpt_get_status -> ra_ulpt_stop
 * -> re-arm. All MMIO is via the host tests/mocks/ra_sim_mmap.c shim.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra_err.h"
#include "ra_sim_mmap.h"
#include "ra_ulpt.h"
#include "unity_minimal.h"

typedef enum : uint32_t {
  k_test_ulpt_app_period   = 0x8000U,
  k_test_ulpt_app_period_2 = 0x4000U,
} test_ulpt_app_const_t;

typedef enum : uint8_t {
  k_test_ulpt_app_channel = 0U,
  k_test_ulpt_app_bad     = 8U,
} test_ulpt_app_chan_t;

static void reset_world(void)
{
  ra_sim_mmap_reset();
  (void)ra_ulpt_init();
}

/**
 * @brief Golden bring-up: init + start + read status + stop + re-arm.
 *
 * @par MC/DC:
 * Compound decision in app: ``ra_ulpt_start != ok``. One atomic
 * condition x 2 vectors -- golden (this) + bad-channel reject
 * (test_ulpt_app_bad_channel).
 */
static void test_ulpt_app_arm_ok(void)
{
  reset_world();
  TEST_BEGIN("ulpt_demo: arm + status + stop + re-arm");
  TEST_ASSERT_EQ(k_ra_ok,
                 ra_ulpt_start((uint8_t)k_test_ulpt_app_channel, (uint32_t)k_test_ulpt_app_period));
  uint8_t status = 0U;
  TEST_ASSERT_EQ(k_ra_ok, ra_ulpt_get_status((uint8_t)k_test_ulpt_app_channel, &status));
  TEST_ASSERT_EQ(k_ra_ok, ra_ulpt_stop((uint8_t)k_test_ulpt_app_channel));
  TEST_ASSERT_EQ(
    k_ra_ok,
    ra_ulpt_start((uint8_t)k_test_ulpt_app_channel, (uint32_t)k_test_ulpt_app_period_2));
  TEST_END("ulpt_demo: arm + status + stop + re-arm");
}

/**
 * @brief Bad channel rejected by ra_ulpt_start.
 *
 * @par MC/DC:
 * Decision: ``channel < k_ra_ulpt_max_channel``. One atomic
 * condition x 2 vectors -- in-range golden + this out-of-range.
 */
static void test_ulpt_app_bad_channel(void)
{
  reset_world();
  TEST_BEGIN("ulpt_demo: bad channel rejected");
  TEST_ASSERT(ra_ulpt_start((uint8_t)k_test_ulpt_app_bad, 0U) != k_ra_ok);
  TEST_END("ulpt_demo: bad channel rejected");
}

/**
 * @brief NULL status pointer rejected by ra_ulpt_get_status.
 *
 * @par MC/DC:
 * Decision: ``out_mask == nullptr``. One atomic condition x 2
 * vectors -- golden (non-NULL) + this NULL.
 */
static void test_ulpt_app_status_null(void)
{
  reset_world();
  TEST_BEGIN("ulpt_demo: status NULL rejected");
  TEST_ASSERT_EQ(k_ra_ok,
                 ra_ulpt_start((uint8_t)k_test_ulpt_app_channel, (uint32_t)k_test_ulpt_app_period));
  TEST_ASSERT(ra_ulpt_get_status((uint8_t)k_test_ulpt_app_channel, nullptr) != k_ra_ok);
  TEST_END("ulpt_demo: status NULL rejected");
}

int main(void)
{
  test_ulpt_app_arm_ok();
  test_ulpt_app_bad_channel();
  test_ulpt_app_status_null();
  return 0;
}
