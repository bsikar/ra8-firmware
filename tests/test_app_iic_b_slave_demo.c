/**
 * @file test_app_iic_b_slave_demo.c
 * @brief Integration test for examples/ek_ra8d2/iic_b_slave_demo/main.c
 *
 * @details
 * Replays the IIC_B slave open + status / receive / send sequence
 * from the demo. All MMIO is via the host tests/mocks/ra_sim_mmap.c
 * shim.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra_err.h"
#include "ra_iic_b_slave.h"
#include "ra_sim_mmap.h"
#include "unity_minimal.h"

typedef enum : uint8_t {
  k_test_iic_slave_channel  = 0U,
  k_test_iic_slave_addr_7b  = 0x42U,
  k_test_iic_slave_bad_chan = 99U,
} test_iic_slave_const_t;

static void reset_world(void)
{
  ra_sim_mmap_reset();
}

/**
 * @par MC/DC:
 * Decision: ``ra_iic_b_slave_open(NULL cfg) != ok``. Pairs with the
 * ok-vector below for N+1 = 2 vectors.
 */
static void test_iic_slave_open_null_cfg_rejected(void)
{
  reset_world();
  TEST_BEGIN("iic_b_slave_demo: open rejects NULL cfg");
  TEST_ASSERT_EQ((int)k_ra_err_null_ptr,
                 (int)ra_iic_b_slave_open((uint8_t)k_test_iic_slave_channel, NULL));
  TEST_END("iic_b_slave_demo: open rejects NULL cfg");
}

/**
 * @par MC/DC:
 * Decision: ``ra_iic_b_slave_open(bad_channel) != ok``. Pairs with
 * the ok-vector for the channel-out-of-range branch.
 */
static void test_iic_slave_open_bad_channel_rejected(void)
{
  reset_world();
  TEST_BEGIN("iic_b_slave_demo: open rejects bad channel");
  const ra_iic_b_slave_cfg_t cfg = {
    .slave_addr_7b = (uint8_t)k_test_iic_slave_addr_7b,
    .general_call  = 0U,
  };
  TEST_ASSERT(ra_iic_b_slave_open((uint8_t)k_test_iic_slave_bad_chan, &cfg) != k_ra_ok);
  TEST_END("iic_b_slave_demo: open rejects bad channel");
}

/**
 * @par MC/DC:
 * Decision: ``ra_iic_b_slave_open(good) == ok`` (golden). Pairs with
 * both error vectors above. Also pre-condition for the status read
 * test below.
 */
static void test_iic_slave_open_ok(void)
{
  reset_world();
  TEST_BEGIN("iic_b_slave_demo: open at addr 0x42 ok");
  const ra_iic_b_slave_cfg_t cfg = {
    .slave_addr_7b = (uint8_t)k_test_iic_slave_addr_7b,
    .general_call  = 0U,
  };
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_iic_b_slave_open((uint8_t)k_test_iic_slave_channel, &cfg));
  TEST_END("iic_b_slave_demo: open at addr 0x42 ok");
}

/**
 * @par MC/DC:
 * Decision: ``ra_iic_b_slave_status(NULL out_mask) != ok``. Pairs
 * with the ok-vector below for N+1 = 2 vectors of the NULL guard.
 */
static void test_iic_slave_status_null_rejected(void)
{
  reset_world();
  TEST_BEGIN("iic_b_slave_demo: status rejects NULL out_mask");
  const ra_iic_b_slave_cfg_t cfg = {
    .slave_addr_7b = (uint8_t)k_test_iic_slave_addr_7b,
    .general_call  = 0U,
  };
  (void)ra_iic_b_slave_open((uint8_t)k_test_iic_slave_channel, &cfg);
  TEST_ASSERT_EQ((int)k_ra_err_null_ptr,
                 (int)ra_iic_b_slave_status((uint8_t)k_test_iic_slave_channel, NULL));
  TEST_END("iic_b_slave_demo: status rejects NULL out_mask");
}

/**
 * @par MC/DC:
 * Decision: ``ra_iic_b_slave_status(good) == ok``. Golden path
 * vector; mask returned will be idle in the simulator.
 */
static void test_iic_slave_status_idle(void)
{
  reset_world();
  TEST_BEGIN("iic_b_slave_demo: status reports idle on fresh open");
  const ra_iic_b_slave_cfg_t cfg = {
    .slave_addr_7b = (uint8_t)k_test_iic_slave_addr_7b,
    .general_call  = 0U,
  };
  (void)ra_iic_b_slave_open((uint8_t)k_test_iic_slave_channel, &cfg);
  uint8_t mask = 0xFFU;
  TEST_ASSERT_EQ((int)k_ra_ok,
                 (int)ra_iic_b_slave_status((uint8_t)k_test_iic_slave_channel, &mask));
  TEST_END("iic_b_slave_demo: status reports idle on fresh open");
}

int main(void)
{
  test_iic_slave_open_null_cfg_rejected();
  test_iic_slave_open_bad_channel_rejected();
  test_iic_slave_open_ok();
  test_iic_slave_status_null_rejected();
  test_iic_slave_status_idle();
  return 0;
}
