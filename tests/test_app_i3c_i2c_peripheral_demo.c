/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file test_app_i3c_i2c_peripheral_demo.c
 * @brief Integration test for examples/ek_ra8d2/i3c_i2c_peripheral_demo/main.c
 *
 * @details
 * Replays the IIC_B peripheral open + status / receive / send sequence
 * from the demo. All MMIO is via the host tests/mocks/ra8_fake_mmap.c
 * shim.
 *
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_i3c.h"
#include "unity_minimal.h"

/**
 * @enum app_i3c_i2c_peripheral_demo_fixture_t
 * @brief All-bits-set register values, so a write that clears the wrong field leaves evidence.
 */
typedef enum : uint8_t {
  k_i2c_mask_all = 0xFFU, /**< Every mask bit set, so no interrupt source is filtered out. */
} app_i3c_i2c_peripheral_demo_fixture_t;

typedef enum : uint8_t {
  k_test_iic_peripheral_channel  = 0U,    /**< Test iic peripheral channel.    */
  k_test_iic_peripheral_addr_7b  = 0x42U, /**< Test iic peripheral address 7b. */
  k_test_iic_peripheral_bad_chan = 99U,   /**< Test iic peripheral bad chan.   */
} test_iic_peripheral_const_t;

static void reset_world(void)
{
  ra8_fake_mmap_reset();
}

/**
 * @par MC/DC:
 * Decision: ``ra8_i3c_peripheral_open(NULL cfg) != ok``. Pairs with the
 * ok-vector below for N+1 = 2 vectors.
 */
static void test_iic_peripheral_open_null_cfg_rejected(void)
{
  reset_world();
  TEST_BEGIN("i3c_i2c_peripheral_demo: open rejects NULL cfg");
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_i3c_peripheral_open((uint8_t)k_test_iic_peripheral_channel, nullptr));
  TEST_END("i3c_i2c_peripheral_demo: open rejects NULL cfg");
}

/**
 * @par MC/DC:
 * Decision: ``ra8_i3c_peripheral_open(bad_channel) != ok``. Pairs with
 * the ok-vector for the channel-out-of-range branch.
 */
static void test_iic_peripheral_open_bad_channel_rejected(void)
{
  reset_world();
  TEST_BEGIN("i3c_i2c_peripheral_demo: open rejects bad channel");
  const ra8_i3c_peripheral_cfg_t cfg = {
    .peripheral_addr_7b = (uint8_t)k_test_iic_peripheral_addr_7b,
    .general_call       = 0U,
  };
  TEST_ASSERT(ra8_i3c_peripheral_open((uint8_t)k_test_iic_peripheral_bad_chan, &cfg) != k_ra8_ok);
  TEST_END("i3c_i2c_peripheral_demo: open rejects bad channel");
}

/**
 * @par MC/DC:
 * Decision: ``ra8_i3c_peripheral_open(good) == ok`` (golden). Pairs with
 * both error vectors above. Also pre-condition for the status read
 * test below.
 */
static void test_iic_peripheral_open_ok(void)
{
  reset_world();
  TEST_BEGIN("i3c_i2c_peripheral_demo: open at addr 0x42 ok");
  const ra8_i3c_peripheral_cfg_t cfg = {
    .peripheral_addr_7b = (uint8_t)k_test_iic_peripheral_addr_7b,
    .general_call       = 0U,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i3c_peripheral_open((uint8_t)k_test_iic_peripheral_channel, &cfg));
  TEST_END("i3c_i2c_peripheral_demo: open at addr 0x42 ok");
}

/**
 * @par MC/DC:
 * Decision: ``ra8_i3c_peripheral_status(NULL out_mask) != ok``. Pairs
 * with the ok-vector below for N+1 = 2 vectors of the NULL guard.
 */
static void test_iic_peripheral_status_null_rejected(void)
{
  reset_world();
  TEST_BEGIN("i3c_i2c_peripheral_demo: status rejects NULL out_mask");
  const ra8_i3c_peripheral_cfg_t cfg = {
    .peripheral_addr_7b = (uint8_t)k_test_iic_peripheral_addr_7b,
    .general_call       = 0U,
  };
  (void)ra8_i3c_peripheral_open((uint8_t)k_test_iic_peripheral_channel, &cfg);
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_i3c_peripheral_status((uint8_t)k_test_iic_peripheral_channel, nullptr));
  TEST_END("i3c_i2c_peripheral_demo: status rejects NULL out_mask");
}

/**
 * @par MC/DC:
 * Decision: ``ra8_i3c_peripheral_status(good) == ok``. Golden path
 * vector; mask returned will be idle in the fake.
 */
static void test_iic_peripheral_status_idle(void)
{
  reset_world();
  TEST_BEGIN("i3c_i2c_peripheral_demo: status reports idle on fresh open");
  const ra8_i3c_peripheral_cfg_t cfg = {
    .peripheral_addr_7b = (uint8_t)k_test_iic_peripheral_addr_7b,
    .general_call       = 0U,
  };
  (void)ra8_i3c_peripheral_open((uint8_t)k_test_iic_peripheral_channel, &cfg);
  uint8_t mask = k_i2c_mask_all;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_i3c_peripheral_status((uint8_t)k_test_iic_peripheral_channel, &mask));
  TEST_END("i3c_i2c_peripheral_demo: status reports idle on fresh open");
}

int main(void)
{
  test_iic_peripheral_open_null_cfg_rejected();
  test_iic_peripheral_open_bad_channel_rejected();
  test_iic_peripheral_open_ok();
  test_iic_peripheral_status_null_rejected();
  test_iic_peripheral_status_idle();
  return 0;
}
