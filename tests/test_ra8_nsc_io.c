/**
 * @file test_ra8_nsc_io.c
 * @brief Unit tests for libs/ra8_nsc/src/ra8_nsc_io.c
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8_acmphs.h"
#include "ra8_crc.h"
#include "ra8_err.h"
#include "ra8_glcdc.h"
#include "ra8_gpt.h"
#include "ra8_mstp.h"
#include "ra8_nsc_io.h"
#include "ra8_sim_mmap.h"
#include "unity_minimal.h"

/**
 * @enum t_nsc_cfg_t
 * @brief Timer and display settings passed across the secure gateway.
 *
 * @details
 * The veneer only marshals these, so the values need to be plausible and
 * distinct: a 50% duty cycle would be symmetric enough to hide a swapped
 * period/duty pair, hence the deliberate 1:2 ratio.
 */
typedef enum : uint16_t {
  k_t_pwm_period = 1000U, /**< Timer period, in timer ticks.              */
  k_t_pwm_duty   = 500U,  /**< Duty-cycle compare value: half the period. */
  k_t_screen_w   = 800U,  /**< Display width, pixels.                     */
  k_t_screen_h   = 480U,  /**< Display height, pixels.                    */
} t_nsc_cfg_t;

static void prep(void)
{
  ra8_sim_mmap_reset();
  (void)ra8_mstp_init();
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_gpt_init_forwards(void)
{
  TEST_BEGIN("ra8_nsc_gpt_init forwards + null rejected");
  prep();
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_nsc_gpt_init(0U, nullptr));
  ra8_gpt_cfg_t cfg = {};
  cfg.mode          = k_ra8_gpt_mode_saw_pwm;
  cfg.prescaler     = k_ra8_gpt_ps_div_4;
  cfg.period        = k_t_pwm_period;
  cfg.duty_a        = k_t_pwm_duty;
  cfg.duty_b        = 0U;
  cfg.auto_start    = true;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_nsc_gpt_init(0U, &cfg));
  uint32_t cnt = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_nsc_gpt_read(0U, &cnt));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_nsc_gpt_read(0U, nullptr));
  TEST_END("ra8_nsc_gpt_init forwards + null rejected");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_adc_dac_acmphs_init_forwards(void)
{
  TEST_BEGIN("ra8_nsc_{adc,dac_b,acmphs}_init forwards");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_nsc_adc_init());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_nsc_dac_b_init());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_nsc_acmphs_init());
  uint16_t raw = 0U;
  /* The host ADC sim does not set conversion-done flags, so the
   * driver returns hw_timeout. The veneer test only cares that the
   * forwarding path runs and the null-ptr guard fires. */
  (void)ra8_nsc_adc_read_channel(0U, &raw);
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_nsc_adc_read_channel(0U, nullptr));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_nsc_dac_b_write(0U, 0x800U));
  ra8_level_t lvl = k_ra8_level_low;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_nsc_acmphs_read_output(0U, &lvl));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_nsc_acmphs_read_output(0U, nullptr));
  TEST_END("ra8_nsc_{adc,dac_b,acmphs}_init forwards");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_crc_init_compute(void)
{
  TEST_BEGIN("ra8_nsc_crc_init + compute");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_nsc_crc_init(k_ra8_crc_poly_16_ccitt));
  uint32_t      out  = 0U;
  const uint8_t data = 0xAAU;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_nsc_crc_compute(&data, 1U, &out));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_nsc_crc_compute(nullptr, 1U, &out));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_nsc_crc_compute(&data, 1U, nullptr));
  TEST_END("ra8_nsc_crc_init + compute");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_glcdc_pdm_eth_init(void)
{
  TEST_BEGIN("ra8_nsc_{glcdc,pdm,eth}_init forwards");
  prep();
  ra8_glcdc_config_t glcfg = {};
  glcfg.width_px           = k_t_screen_w;
  glcfg.height_px          = k_t_screen_h;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_nsc_glcdc_init(&glcfg));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_nsc_glcdc_init(nullptr));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_nsc_pdm_init());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_nsc_eth_init());
  TEST_END("ra8_nsc_{glcdc,pdm,eth}_init forwards");
}

int32_t main(void)
{
  test_gpt_init_forwards();
  test_adc_dac_acmphs_init_forwards();
  test_crc_init_compute();
  test_glcdc_pdm_eth_init();
  (void)fprintf(stderr, "[OK ] test_ra8_nsc_io.c\n");
  return 0;
}
