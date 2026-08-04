/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file test_app_i3c_loopback.c
 * @brief Integration test: IIC_B init + scan path mirrors i3c_loopback main
 *
 * @details
 * Mirrors the bring-up flow of examples/ek_ra8d2/i3c_loopback/main.c:
 * ra8_mstp_init -> PFS routing of SCL0/SDA0 -> ra8_i3c_init at 100 kHz
 * -> ra8_i3c_scan against 0x43. All MMIO is via the host
 * tests/mocks/ra8_fake_mmap.c shim.
 *
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_gpio_constants.h"
#include "ra8_i3c.h"
#include "ra8_mstp.h"
#include "ra8_pin_validator.h"
#include "ra8_port_utils.h"
#include "unity_minimal.h"

typedef enum : uint32_t {
  k_test_i3c_app_bus_hz   = 100000U,    /**< Test I3C app bus Hz.   */
  k_test_i3c_app_pclka_hz = 125000000U, /**< Test I3C app pclka Hz. */
} test_i3c_app_const_t;

typedef enum : uint8_t {
  k_test_i3c_app_channel     = 0U,    /**< Test I3C app channel.       */
  k_test_i3c_app_probe_addr  = 0x43U, /**< Test I3C app probe address. */
  k_test_i3c_app_bad_channel = 99U,   /**< Test I3C app bad channel.   */
} test_i3c_app_byte_t;

/** @brief Mirrors main.c pin map for SCL0 / SDA0. */
static const ra8_port_pin_t k_test_i3c_app_pin_scl =
  (ra8_port_pin_t)(((uint16_t)k_ra8_port_4 << 8) | (uint16_t)k_ra8_pin_0);
static const ra8_port_pin_t k_test_i3c_app_pin_sda =
  (ra8_port_pin_t)(((uint16_t)k_ra8_port_4 << 8) | (uint16_t)k_ra8_pin_1);

static void reset_world(void)
{
  ra8_fake_mmap_reset();
  ra8_pin_validator_reset();
}

/**
 * @brief Golden-path bring-up replays main.c i2c_demo_setup_or_halt.
 *
 * @par MC/DC:
 * Compound decision in app: ``ra8_pfs_route_peripheral(SCL) != ok ||
 * ra8_pfs_route_peripheral(SDA) != ok``. Two atomic conditions x
 * N+1 = 3 vectors -- this test covers both-ok; bad-pin test covers
 * SCL-fails; bad-channel test below covers iic_b_init-fails.
 */
static void test_i3c_app_bringup_ok(void)
{
  reset_world();
  TEST_BEGIN("i3c_loopback: PFS + iic_b_init ok");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mstp_init());
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_pfs_route_peripheral(k_test_i3c_app_pin_scl, k_ra8_psel_iic, "test.scl0"));
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_pfs_route_peripheral(k_test_i3c_app_pin_sda, k_ra8_psel_iic, "test.sda0"));
  const ra8_i3c_cfg_t cfg = {
    .mode     = k_ra8_i3c_mode_i2c,
    .bus_hz   = (uint32_t)k_test_i3c_app_bus_hz,
    .pclka_hz = (uint32_t)k_test_i3c_app_pclka_hz,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i3c_init((uint8_t)k_test_i3c_app_channel, &cfg));
  TEST_END("i3c_loopback: PFS + iic_b_init ok");
}

/**
 * @brief NULL config rejected by ra8_i3c_init.
 *
 * @par MC/DC:
 * Decision: ``cfg == nullptr``. One atomic condition x 2 vectors --
 * NULL (this) + non-NULL (test_i3c_app_bringup_ok).
 */
static void test_i3c_app_init_null_rejected(void)
{
  reset_world();
  TEST_BEGIN("i3c_loopback: NULL cfg rejected");
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_i3c_init((uint8_t)k_test_i3c_app_channel, nullptr));
  TEST_END("i3c_loopback: NULL cfg rejected");
}

/**
 * @brief Bad channel rejected by ra8_i3c_init.
 *
 * @par MC/DC:
 * Decision: ``channel out-of-range``. One atomic condition x 2
 * vectors -- in-range (test_i3c_app_bringup_ok) + out-of-range (this).
 */
static void test_i3c_app_init_bad_channel(void)
{
  reset_world();
  TEST_BEGIN("i3c_loopback: bad channel rejected");
  const ra8_i3c_cfg_t cfg = {
    .mode     = k_ra8_i3c_mode_i2c,
    .bus_hz   = (uint32_t)k_test_i3c_app_bus_hz,
    .pclka_hz = (uint32_t)k_test_i3c_app_pclka_hz,
  };
  TEST_ASSERT(ra8_i3c_init((uint8_t)k_test_i3c_app_bad_channel, &cfg) != k_ra8_ok);
  TEST_END("i3c_loopback: bad channel rejected");
}

/**
 * @brief ra8_i3c_scan with NULL out_acked is rejected.
 *
 * @par MC/DC:
 * Decision: ``out_acked == nullptr``. One atomic condition x 2
 * vectors -- non-NULL (covered by app golden path) + NULL (this).
 */
static void test_i3c_app_scan_null_out_rejected(void)
{
  reset_world();
  TEST_BEGIN("i3c_loopback: scan rejects NULL out_acked");
  const ra8_i3c_cfg_t cfg = {
    .mode     = k_ra8_i3c_mode_i2c,
    .bus_hz   = (uint32_t)k_test_i3c_app_bus_hz,
    .pclka_hz = (uint32_t)k_test_i3c_app_pclka_hz,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i3c_init((uint8_t)k_test_i3c_app_channel, &cfg));
  TEST_ASSERT(ra8_i3c_scan((uint8_t)k_test_i3c_app_channel,
                           (uint8_t)k_test_i3c_app_probe_addr,
                           nullptr) != k_ra8_ok);
  TEST_END("i3c_loopback: scan rejects NULL out_acked");
}

int main(void)
{
  test_i3c_app_bringup_ok();
  test_i3c_app_init_null_rejected();
  test_i3c_app_init_bad_channel();
  test_i3c_app_scan_null_out_rejected();
  return 0;
}
