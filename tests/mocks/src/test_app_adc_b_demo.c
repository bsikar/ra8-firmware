/**
 * @file test_app_adc_b_demo.c
 * @brief Integration test: ADC_B 12-bit software-trigger sample demo
 *
 * @details
 * Mirrors examples/ek_ra8d2/hw_validated/hil/adc_b_demo/src/main.c bring-up:
 * ``ra8_adc_init_configured`` (12-bit, software trigger) ->
 * ``ra8_adc_read_channel``. The host MMIO shim backs the ADC16H
 * ADCHCRn / ADDOPCRCn / ADDRn window so the writes succeed and the test
 * validates both the acceptance path and documented rejection paths.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_adc.h"
#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "unity_minimal.h"

typedef enum : uint8_t {
  k_test_adc_b_channel     = 0U,   /**< Test ADC b channel.                     */
  k_test_adc_b_channel_oor = 200U, /**< Way past the implemented channel count. */
} test_adc_b_chan_t;

static void reset_world(void)
{
  ra8_fake_mmap_reset();
}

static ra8_adc_cfg_t make_cfg(void)
{
  const ra8_adc_cfg_t cfg = {
    .resolution    = k_ra8_adc_res_12bit,
    .trigger       = k_ra8_adc_trig_software,
    .right_aligned = true,
    .scan_mode     = false,
  };
  return cfg;
}

/**
 * @brief Golden bring-up: configure + read a channel.
 *
 * @par MC/DC:
 * Decision in app: ``ra8_adc_init_configured != ok``. One atomic
 * condition x 2 vectors -- golden (this) + null cfg below.
 */
static void test_adc_b_arm_ok(void)
{
  reset_world();
  TEST_BEGIN("adc_b_demo: configure + read channel");
  const ra8_adc_cfg_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_adc_init_configured(&cfg));
  uint16_t raw = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_adc_read_channel((uint8_t)k_test_adc_b_channel, &raw));
  TEST_END("adc_b_demo: configure + read channel");
}

/**
 * @brief NULL cfg rejected by ``ra8_adc_init_configured``.
 *
 * @par MC/DC:
 * Decision: ``cfg == NULL``. One atomic condition x 2 vectors --
 * non-NULL above + NULL here.
 */
static void test_adc_b_null_cfg(void)
{
  reset_world();
  TEST_BEGIN("adc_b_demo: NULL cfg rejected");
  TEST_ASSERT(ra8_adc_init_configured(nullptr) != k_ra8_ok);
  TEST_END("adc_b_demo: NULL cfg rejected");
}

/**
 * @brief NULL ``out_raw`` rejected by ``ra8_adc_read_channel``.
 *
 * @par MC/DC:
 * Decision: ``out_raw == NULL``. One atomic condition x 2 vectors --
 * non-NULL above + NULL here.
 */
static void test_adc_b_read_null_out(void)
{
  reset_world();
  TEST_BEGIN("adc_b_demo: NULL out_raw rejected");
  const ra8_adc_cfg_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_adc_init_configured(&cfg));
  TEST_ASSERT(ra8_adc_read_channel((uint8_t)k_test_adc_b_channel, nullptr) != k_ra8_ok);
  TEST_END("adc_b_demo: NULL out_raw rejected");
}

/**
 * @brief Out-of-range channel rejected by ``ra8_adc_read_channel``.
 *
 * @par MC/DC:
 * Decision: ``channel < k_ra8_adc_max_channel``. One atomic condition
 * x 2 vectors -- in-range golden + out-of-range here.
 */
static void test_adc_b_bad_channel(void)
{
  reset_world();
  TEST_BEGIN("adc_b_demo: bad channel rejected");
  const ra8_adc_cfg_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_adc_init_configured(&cfg));
  uint16_t raw = 0U;
  TEST_ASSERT(ra8_adc_read_channel((uint8_t)k_test_adc_b_channel_oor, &raw) != k_ra8_ok);
  TEST_END("adc_b_demo: bad channel rejected");
}

int main(void)
{
  test_adc_b_arm_ok();
  test_adc_b_null_cfg();
  test_adc_b_read_null_out();
  test_adc_b_bad_channel();
  return 0;
}
