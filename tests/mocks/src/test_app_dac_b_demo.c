/**
 * @file test_app_dac_b_demo.c
 * @brief Integration test: DAC_B 12-bit DC sweep demo
 *
 * @details
 * Mirrors examples/ek_ra8d2/hw_validated/hil/dac_b_demo/src/main.c bring-up:
 * ``ra8_dac_b_init_configured`` -> ``ra8_dac_b_set_output_enable`` ->
 * ``ra8_dac_b_write``. Backed by the host MMIO shim.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_dac_b.h"
#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "unity_minimal.h"

typedef enum : uint16_t {
  k_test_dac_b_full_scale = 4095U,   /**< Test DAC b full scale. */
  k_test_dac_b_mid        = 2048U,   /**< Test DAC b mid.        */
  k_test_dac_b_over       = 0xFFFFU, /**< Test DAC b over.       */
} test_dac_b_const_t;

typedef enum : uint8_t {
  k_test_dac_b_channel     = 0U,  /**< Test DAC b channel.     */
  k_test_dac_b_channel_bad = 99U, /**< Test DAC b channel bad. */
} test_dac_b_chan_t;

static void reset_world(void)
{
  ra8_fake_mmap_reset();
}

static ra8_dac_b_cfg_t make_cfg(void)
{
  const ra8_dac_b_cfg_t cfg = {
    .vref            = k_ra8_dac_b_vref_normal,
    .data_format     = k_ra8_dac_b_format_right,
    .enable_channel0 = true,
    .enable_channel1 = false,
  };
  return cfg;
}

/**
 * @brief Golden bring-up: configure + drive a mid-scale code.
 *
 * @par MC/DC:
 * Decision in app: ``ra8_dac_b_init_configured != ok``. One atomic
 * condition x 2 vectors -- golden (this) + null cfg below.
 */
static void test_dac_b_arm_ok(void)
{
  reset_world();
  TEST_BEGIN("dac_b_demo: configure + write mid-scale");
  const ra8_dac_b_cfg_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_dac_b_init_configured(&cfg));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_dac_b_set_output_enable((uint8_t)k_test_dac_b_channel, true));
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_dac_b_write((uint8_t)k_test_dac_b_channel, (uint16_t)k_test_dac_b_mid));
  TEST_END("dac_b_demo: configure + write mid-scale");
}

/**
 * @brief NULL cfg rejected by ``ra8_dac_b_init_configured``.
 *
 * @par MC/DC:
 * Decision: ``cfg == NULL``. One atomic condition x 2 vectors --
 * non-NULL above + NULL here.
 */
static void test_dac_b_null_cfg(void)
{
  reset_world();
  TEST_BEGIN("dac_b_demo: NULL cfg rejected");
  TEST_ASSERT(ra8_dac_b_init_configured(nullptr) != k_ra8_ok);
  TEST_END("dac_b_demo: NULL cfg rejected");
}

/**
 * @brief Out-of-12-bit-range value clamped by ``ra8_dac_b_write``.
 *
 * @par MC/DC:
 * Decision: ``value > 0x0FFF`` -> clamp to 0x0FFF. One atomic
 * condition x 2 vectors -- in-range golden (write_mid above leaves
 * value untouched) + out-of-range here (write returns ok and clamps
 * silently per HUM Ch 38 DAC_B "Data Format" spec).
 */
static void test_dac_b_clamp_value(void)
{
  reset_world();
  TEST_BEGIN("dac_b_demo: out-of-range value clamps");
  const ra8_dac_b_cfg_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_dac_b_init_configured(&cfg));
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_dac_b_write((uint8_t)k_test_dac_b_channel, (uint16_t)k_test_dac_b_over));
  TEST_END("dac_b_demo: out-of-range value clamps");
}

/**
 * @brief Bad channel rejected by ``ra8_dac_b_write``.
 *
 * @par MC/DC:
 * Decision: ``channel < k_ra8_dac_b_max_channel``. One atomic condition
 * x 2 vectors -- in-range golden + out-of-range here.
 */
static void test_dac_b_bad_channel(void)
{
  reset_world();
  TEST_BEGIN("dac_b_demo: bad channel rejected");
  const ra8_dac_b_cfg_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_dac_b_init_configured(&cfg));
  TEST_ASSERT(ra8_dac_b_write((uint8_t)k_test_dac_b_channel_bad, (uint16_t)k_test_dac_b_mid) !=
              k_ra8_ok);
  TEST_END("dac_b_demo: bad channel rejected");
}

int main(void)
{
  test_dac_b_arm_ok();
  test_dac_b_null_cfg();
  test_dac_b_clamp_value();
  test_dac_b_bad_channel();
  return 0;
}
