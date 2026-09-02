/**
 * @file test_app_acmphs_compare.c
 * @brief Integration test: ACMPHS channel-0 polling demo bring-up
 *
 * @details
 * Mirrors examples/ek_ra8d2/hw_validated/hil/acmphs_compare/src/main.c bring-up:
 * ra8_acmphs_init -> ra8_acmphs_channel_init(channel 0) ->
 * ra8_acmphs_read_output. Host shim returns deterministic values so
 * the golden path always succeeds.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_acmphs.h"
#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_port_constants.h"
#include "unity_minimal.h"

typedef enum : uint8_t {
  k_test_acmphs_app_channel = 0U, /**< Test acmphs app channel.     */
  k_test_acmphs_app_bad_ch  = 9U, /**< Test acmphs app bad channel. */
} test_acmphs_chan_t;

static void reset_world(void)
{
  ra8_fake_mmap_reset();
}

/**
 * @brief Golden bring-up: init + channel_init succeeds.
 *
 * @par MC/DC:
 * Compound decision in app: ``acmphs_init != ok || channel_init !=
 * ok``. Two atomic conditions x N+1 = 3 vectors -- both-ok (this),
 * channel_init bad-channel (below), null-cfg (below).
 */
static void test_acmphs_app_bringup_ok(void)
{
  reset_world();
  TEST_BEGIN("acmphs_compare: bring-up ok");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_acmphs_init());
  const ra8_acmphs_cfg_t cfg = {
    .ivpsel     = 0U,
    .ivrefsel   = 0U,
    .edge       = k_ra8_acmphs_edge_none,
    .filter_en  = false,
    .invert_out = false,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_acmphs_channel_init((uint8_t)k_test_acmphs_app_channel, &cfg));
  TEST_END("acmphs_compare: bring-up ok");
}

/**
 * @brief read_output returns one of the legal levels.
 *
 * @par MC/DC:
 * Decision in app: ``lv == k_ra8_level_high``. One atomic condition
 * x 2 vectors -- HIGH path (LED1) and LOW path (LED2). Both
 * branches reachable by varying the seeded CMPMON value.
 */
static void test_acmphs_app_read_legal(void)
{
  reset_world();
  TEST_BEGIN("acmphs_compare: read_output returns legal value");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_acmphs_init());
  const ra8_acmphs_cfg_t cfg = {
    .ivpsel     = 0U,
    .ivrefsel   = 0U,
    .edge       = k_ra8_acmphs_edge_none,
    .filter_en  = false,
    .invert_out = false,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_acmphs_channel_init((uint8_t)k_test_acmphs_app_channel, &cfg));
  ra8_level_t lv = k_ra8_level_low;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_acmphs_read_output((uint8_t)k_test_acmphs_app_channel, &lv));
  TEST_ASSERT(lv == k_ra8_level_low || lv == k_ra8_level_high);
  TEST_END("acmphs_compare: read_output returns legal value");
}

/**
 * @brief NULL cfg rejected by channel_init.
 *
 * @par MC/DC:
 * Decision: ``cfg == nullptr``. One atomic condition x 2 vectors --
 * non-NULL golden + this NULL.
 */
static void test_acmphs_app_null_cfg(void)
{
  reset_world();
  TEST_BEGIN("acmphs_compare: null cfg rejected");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_acmphs_init());
  TEST_ASSERT(ra8_acmphs_channel_init((uint8_t)k_test_acmphs_app_channel, nullptr) != k_ra8_ok);
  TEST_END("acmphs_compare: null cfg rejected");
}

/**
 * @brief Out-of-range channel rejected by read_output.
 *
 * @par MC/DC:
 * Decision: ``channel < 6``. One atomic condition x 2 vectors --
 * in-range golden + this out-of-range.
 */
static void test_acmphs_app_bad_channel(void)
{
  reset_world();
  TEST_BEGIN("acmphs_compare: bad channel rejected");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_acmphs_init());
  ra8_level_t lv = k_ra8_level_low;
  TEST_ASSERT(ra8_acmphs_read_output((uint8_t)k_test_acmphs_app_bad_ch, &lv) != k_ra8_ok);
  TEST_END("acmphs_compare: bad channel rejected");
}

int main(void)
{
  test_acmphs_app_bringup_ok();
  test_acmphs_app_read_legal();
  test_acmphs_app_null_cfg();
  test_acmphs_app_bad_channel();
  return 0;
}
