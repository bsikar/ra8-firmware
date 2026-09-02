/**
 * @file test_app_i2c_loopback.c
 * @brief Integration test: RIIC (ra8_i2c) init + scan path mirrors i2c_loopback main
 *
 * @details
 * Mirrors the bus path of examples/ek_ra8d2/hw_validated/hil/i2c_loopback/src/main.c, which
 * brings RIIC channel 1 up (through the board U15 helper) and then loops
 * ra8_i2c_scan against 0x43 (the on-board PI4IOE5V6408 expander). Here we
 * drive ra8_i2c_init(ch1) + ra8_i2c_scan directly through the host
 * tests/mocks/src/ra8_fake_mmap.c shim; the board-side pin/pull-up bring-up is
 * covered by the board library's own tests.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_i2c.h"
#include "ra8_mstp.h"
#include "ra8_pin_validator.h"
#include "unity_minimal.h"

typedef enum : uint32_t {
  k_test_i2c_app_bus_hz   = 100000U,   /**< Test I2C app bus Hz.   */
  k_test_i2c_app_pclkb_hz = 62500000U, /**< Test I2C app pclkb Hz. */
} test_i2c_app_const_t;

typedef enum : uint8_t {
  k_test_i2c_app_channel     = 1U,    /**< RIIC ch1 -- where U15 lives. */
  k_test_i2c_app_probe_addr  = 0x43U, /**< Test I2C app probe address.  */
  k_test_i2c_app_bad_channel = 99U,   /**< Test I2C app bad channel.    */
} test_i2c_app_byte_t;

static void reset_world(void)
{
  ra8_fake_mmap_reset();
  ra8_pin_validator_reset();
}

/**
 * @brief Golden path: RIIC ch1 init succeeds, as the app's board bring-up does.
 *
 * @par MC/DC:
 * Decision: ``ra8_i2c_init() == k_ra8_ok``. One atomic condition x 2
 * vectors -- ok (this) + not-ok (bad-channel test below).
 */
static void test_i2c_app_bringup_ok(void)
{
  reset_world();
  TEST_BEGIN("i2c_loopback: ra8_i2c ch1 init ok");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mstp_init());
  const ra8_i2c_cfg_t cfg = {
    .bus_hz   = (uint32_t)k_test_i2c_app_bus_hz,
    .pclkb_hz = (uint32_t)k_test_i2c_app_pclkb_hz,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i2c_init((uint8_t)k_test_i2c_app_channel, &cfg));
  TEST_END("i2c_loopback: ra8_i2c ch1 init ok");
}

/**
 * @brief NULL config rejected by ra8_i2c_init.
 *
 * @par MC/DC:
 * Decision: ``cfg == nullptr``. One atomic condition x 2 vectors --
 * NULL (this) + non-NULL (test_i2c_app_bringup_ok).
 */
static void test_i2c_app_init_null_rejected(void)
{
  reset_world();
  TEST_BEGIN("i2c_loopback: NULL cfg rejected");
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_i2c_init((uint8_t)k_test_i2c_app_channel, nullptr));
  TEST_END("i2c_loopback: NULL cfg rejected");
}

/**
 * @brief Bad channel rejected by ra8_i2c_init.
 *
 * @par MC/DC:
 * Decision: ``channel out-of-range``. One atomic condition x 2
 * vectors -- in-range (test_i2c_app_bringup_ok) + out-of-range (this).
 */
static void test_i2c_app_init_bad_channel(void)
{
  reset_world();
  TEST_BEGIN("i2c_loopback: bad channel rejected");
  const ra8_i2c_cfg_t cfg = {
    .bus_hz   = (uint32_t)k_test_i2c_app_bus_hz,
    .pclkb_hz = (uint32_t)k_test_i2c_app_pclkb_hz,
  };
  TEST_ASSERT(ra8_i2c_init((uint8_t)k_test_i2c_app_bad_channel, &cfg) != k_ra8_ok);
  TEST_END("i2c_loopback: bad channel rejected");
}

/**
 * @brief ra8_i2c_scan with NULL out_acked is rejected.
 *
 * @par MC/DC:
 * Decision: ``out_acked == nullptr``. One atomic condition x 2
 * vectors -- non-NULL (app golden path) + NULL (this).
 */
static void test_i2c_app_scan_null_out_rejected(void)
{
  reset_world();
  TEST_BEGIN("i2c_loopback: scan rejects NULL out_acked");
  const ra8_i2c_cfg_t cfg = {
    .bus_hz   = (uint32_t)k_test_i2c_app_bus_hz,
    .pclkb_hz = (uint32_t)k_test_i2c_app_pclkb_hz,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i2c_init((uint8_t)k_test_i2c_app_channel, &cfg));
  TEST_ASSERT(ra8_i2c_scan((uint8_t)k_test_i2c_app_channel,
                           (uint8_t)k_test_i2c_app_probe_addr,
                           nullptr) != k_ra8_ok);
  TEST_END("i2c_loopback: scan rejects NULL out_acked");
}

int main(void)
{
  test_i2c_app_bringup_ok();
  test_i2c_app_init_null_rejected();
  test_i2c_app_init_bad_channel();
  test_i2c_app_scan_null_out_rejected();
  return 0;
}
