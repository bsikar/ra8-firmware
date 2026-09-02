/**
 * @file test_app_dac_waveform.c
 * @brief Integration test: DAC_B init + triangle-wave write loop
 *
 * @details
 * Mirrors examples/ek_ra8d2/hw_validated/hil/dac_waveform/src/main.c bring-up flow:
 * ra8_dac_b_init_configured -> ra8_dac_b_write loop. Verifies that
 * the 12-bit clamp inside ra8_dac_b_write covers the boundary
 * (0, 4095, 4096-and-above), and that NULL cfg + invalid channel
 * are rejected by the API.
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
  k_test_dac_app_steps    = 64U,   /**< Test DAC app steps.        */
  k_test_dac_app_max_code = 4095U, /**< Test DAC app maximum code. */
  k_test_dac_app_oversize = 5000U, /**< Test DAC app oversize.     */
} test_dac_app_const_t;

typedef enum : uint8_t {
  k_test_dac_app_channel  = 0U, /**< Test DAC app channel.  */
  k_test_dac_app_bad_chan = 4U, /**< Test DAC app bad chan. */
} test_dac_app_chan_t;

static void reset_world(void)
{
  ra8_fake_mmap_reset();
}

/**
 * @brief Golden bring-up: init_configured + steady-state write loop.
 *
 * @par MC/DC:
 * Compound decision: ``write != ok`` checked twice (up + down).
 * Two atomic conditions x N+1 = 3 vectors -- both ok (this test) +
 * up-write fails (test_dac_app_bad_channel) + clamp boundary
 * (test_dac_app_clamp).
 */
static void test_dac_app_bringup_and_loop(void)
{
  reset_world();
  TEST_BEGIN("dac_waveform: init + triangle write loop");
  const ra8_dac_b_cfg_t cfg = {
    .vref                    = k_ra8_dac_b_vref_normal,
    .data_format             = k_ra8_dac_b_format_right,
    .internal_output_enabled = false,
    .enable_channel0         = true,
    .enable_channel1         = false,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_dac_b_init_configured(&cfg));
  const uint16_t step =
    (uint16_t)((uint32_t)k_test_dac_app_max_code / (uint32_t)k_test_dac_app_steps);
  for (uint16_t i = 0U; i < (uint16_t)k_test_dac_app_steps; ++i) {
    const uint16_t code = (uint16_t)(i * step);
    TEST_ASSERT_EQ(k_ra8_ok, ra8_dac_b_write((uint8_t)k_test_dac_app_channel, code));
  }
  TEST_END("dac_waveform: init + triangle write loop");
}

/**
 * @brief NULL cfg pointer is rejected by ra8_dac_b_init_configured.
 *
 * @par MC/DC:
 * Decision: ``cfg == nullptr``. One atomic condition x 2 vectors --
 * NULL (this test) + non-NULL (golden test).
 */
static void test_dac_app_null_cfg(void)
{
  reset_world();
  TEST_BEGIN("dac_waveform: NULL cfg rejected");
  TEST_ASSERT(ra8_dac_b_init_configured(nullptr) != k_ra8_ok);
  TEST_END("dac_waveform: NULL cfg rejected");
}

/**
 * @brief Invalid channel index is rejected by ra8_dac_b_write.
 *
 * @par MC/DC:
 * Decision: ``channel >= 2``. One atomic condition x 2 vectors --
 * channel=4 (this test) + channel=0 (golden test).
 */
static void test_dac_app_bad_channel(void)
{
  reset_world();
  TEST_BEGIN("dac_waveform: bad channel rejected");
  TEST_ASSERT(ra8_dac_b_write((uint8_t)k_test_dac_app_bad_chan, 0U) != k_ra8_ok);
  TEST_END("dac_waveform: bad channel rejected");
}

/**
 * @brief Oversize value is silently clamped to 12-bit max.
 *
 * @par MC/DC:
 * Decision in driver: ``value > k_ra8_dac_b_max_value``. Two vectors --
 * over the cap (5000, this test) + at the boundary (golden test loop).
 */
static void test_dac_app_clamp(void)
{
  reset_world();
  TEST_BEGIN("dac_waveform: oversize value clamps OK");
  const ra8_dac_b_cfg_t cfg = {
    .vref                    = k_ra8_dac_b_vref_normal,
    .data_format             = k_ra8_dac_b_format_right,
    .internal_output_enabled = false,
    .enable_channel0         = true,
    .enable_channel1         = false,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_dac_b_init_configured(&cfg));
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_dac_b_write((uint8_t)k_test_dac_app_channel, (uint16_t)k_test_dac_app_oversize));
  TEST_END("dac_waveform: oversize value clamps OK");
}

int main(void)
{
  test_dac_app_bringup_and_loop();
  test_dac_app_null_cfg();
  test_dac_app_bad_channel();
  test_dac_app_clamp();
  return 0;
}
