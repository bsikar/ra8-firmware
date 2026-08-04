/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file test_app_i2c_i3c_combined.c
 * @brief Integration test: RIIC + I3C coexistence mirrors i2c_i3c_combined main
 *
 * @details
 * Mirrors examples/ek_ra8d2/i2c_i3c_combined/main.c, which brings up RIIC
 * channel 1 (ra8_i2c -> U15) and the I3C channel 0 controller (ra8_i3c in
 * I2C-compat mode) in one firmware. Here we drive both inits + their scan
 * reject paths through the host tests/mocks/ra8_fake_mmap.c shim.
 *
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_i2c.h"
#include "ra8_i3c.h"
#include "ra8_mstp.h"
#include "ra8_pin_validator.h"
#include "unity_minimal.h"

typedef enum : uint32_t {
  k_combo_test_bus_hz   = 100000U,    /**< Combo test bus Hz.   */
  k_combo_test_pclka_hz = 125000000U, /**< Combo test pclka Hz. */
  k_combo_test_pclkb_hz = 62500000U,  /**< Combo test pclkb Hz. */
} combo_test_const_t;

typedef enum : uint8_t {
  k_combo_test_i2c_ch = 1U,    /**< RIIC ch1 -- U15.  */
  k_combo_test_i3c_ch = 0U,    /**< I3C ch0.          */
  k_combo_test_probe  = 0x43U, /**< Combo test probe. */
} combo_test_byte_t;

static void reset_world(void)
{
  ra8_fake_mmap_reset();
  ra8_pin_validator_reset();
  (void)ra8_mstp_init();
}

/**
 * @brief Both controllers init in one firmware (RIIC ch1 + I3C ch0).
 *
 * @par MC/DC:
 * Decision: ``ra8_i2c_init()==ok && ra8_i3c_init()==ok``. Each is an atomic
 * init result; this case covers both-ok, the reject cases below cover the
 * single-fail vectors.
 */
static void test_combo_both_init_ok(void)
{
  reset_world();
  TEST_BEGIN("i2c_i3c_combined: RIIC ch1 + I3C ch0 init ok");
  const ra8_i2c_cfg_t i2c_cfg = {
    .bus_hz   = (uint32_t)k_combo_test_bus_hz,
    .pclkb_hz = (uint32_t)k_combo_test_pclkb_hz,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i2c_init((uint8_t)k_combo_test_i2c_ch, &i2c_cfg));
  const ra8_i3c_cfg_t i3c_cfg = {
    .mode     = k_ra8_i3c_mode_i2c,
    .bus_hz   = (uint32_t)k_combo_test_bus_hz,
    .pclka_hz = (uint32_t)k_combo_test_pclka_hz,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i3c_init((uint8_t)k_combo_test_i3c_ch, &i3c_cfg));
  TEST_END("i2c_i3c_combined: RIIC ch1 + I3C ch0 init ok");
}

/**
 * @brief Each scan rejects a NULL out_acked.
 *
 * @par MC/DC:
 * Decision: ``out_acked == nullptr`` in each scan. Atomic condition x 2
 * vectors per driver -- non-NULL (golden path) + NULL (here).
 */
static void test_combo_scan_null_rejected(void)
{
  reset_world();
  TEST_BEGIN("i2c_i3c_combined: both scans reject NULL out_acked");
  const ra8_i2c_cfg_t i2c_cfg = {
    .bus_hz   = (uint32_t)k_combo_test_bus_hz,
    .pclkb_hz = (uint32_t)k_combo_test_pclkb_hz,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i2c_init((uint8_t)k_combo_test_i2c_ch, &i2c_cfg));
  TEST_ASSERT(ra8_i2c_scan((uint8_t)k_combo_test_i2c_ch, (uint8_t)k_combo_test_probe, nullptr) !=
              k_ra8_ok);
  const ra8_i3c_cfg_t i3c_cfg = {
    .mode     = k_ra8_i3c_mode_i2c,
    .bus_hz   = (uint32_t)k_combo_test_bus_hz,
    .pclka_hz = (uint32_t)k_combo_test_pclka_hz,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i3c_init((uint8_t)k_combo_test_i3c_ch, &i3c_cfg));
  TEST_ASSERT(ra8_i3c_scan((uint8_t)k_combo_test_i3c_ch, (uint8_t)k_combo_test_probe, nullptr) !=
              k_ra8_ok);
  TEST_END("i2c_i3c_combined: both scans reject NULL out_acked");
}

int main(void)
{
  test_combo_both_init_ok();
  test_combo_scan_null_rejected();
  return 0;
}
