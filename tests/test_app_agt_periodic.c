/**
 * @file test_app_agt_periodic.c
 * @brief Integration test: AGT 1 Hz periodic-tick demo
 *
 * @details
 * Mirrors examples/ek_ra8d2/agt_periodic/main.c bring-up flow:
 * ra8_agt_start_free_run -> ra8_agt_get_status -> ra8_agt_stop ->
 * re-arm. All MMIO is via the host tests/mocks/ra8_fake_mmap.c shim.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_agt.h"
#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "unity_minimal.h"

typedef enum : uint16_t {
  k_test_agt_app_reload   = 0x7FFFU, /**< Test AGT app reload.   */
  k_test_agt_app_bad_chan = 16U,     /**< Test AGT app bad chan. */
} test_agt_app_const_t;

typedef enum : uint8_t {
  k_test_agt_app_channel = 0U, /**< Test AGT app channel. */
} test_agt_app_chan_t;

static void reset_world(void)
{
  ra8_fake_mmap_reset();
}

/**
 * @brief Golden bring-up: arm + read status + stop.
 *
 * @par MC/DC:
 * Compound decision in app: ``ra8_agt_start_free_run != ok``.
 * One atomic condition x 2 vectors -- golden (this) + bad-channel
 * reject (test_agt_app_bad_channel).
 */
static void test_agt_app_arm_ok(void)
{
  reset_world();
  TEST_BEGIN("agt_periodic: arm + status + stop");
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_agt_start_free_run((uint8_t)k_test_agt_app_channel, (uint16_t)k_test_agt_app_reload));
  uint8_t status = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_agt_get_status((uint8_t)k_test_agt_app_channel, &status));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_agt_stop((uint8_t)k_test_agt_app_channel));
  TEST_END("agt_periodic: arm + status + stop");
}

/**
 * @brief Bad channel rejected.
 *
 * @par MC/DC:
 * Decision: ``channel < k_ra8_agt_max_channel``. One atomic condition
 * x 2 vectors -- in-range golden + this out-of-range.
 */
static void test_agt_app_bad_channel(void)
{
  reset_world();
  TEST_BEGIN("agt_periodic: bad channel rejected");
  TEST_ASSERT(ra8_agt_start_free_run((uint8_t)k_test_agt_app_bad_chan, 0U) != k_ra8_ok);
  TEST_END("agt_periodic: bad channel rejected");
}

/**
 * @brief NULL status out rejected by ra8_agt_get_status.
 *
 * @par MC/DC:
 * Decision: ``out_mask == nullptr``. One atomic condition x 2
 * vectors -- golden (non-NULL) + this NULL.
 */
static void test_agt_app_status_null(void)
{
  reset_world();
  TEST_BEGIN("agt_periodic: status NULL rejected");
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_agt_start_free_run((uint8_t)k_test_agt_app_channel, (uint16_t)k_test_agt_app_reload));
  TEST_ASSERT(ra8_agt_get_status((uint8_t)k_test_agt_app_channel, nullptr) != k_ra8_ok);
  TEST_END("agt_periodic: status NULL rejected");
}

int main(void)
{
  test_agt_app_arm_ok();
  test_agt_app_bad_channel();
  test_agt_app_status_null();
  return 0;
}
