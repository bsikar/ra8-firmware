/**
 * @file test_ra_nsc_io.c
 * @brief Unit tests for libs/ra_nsc/src/ra_nsc_io.c
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra_acmphs.h"
#include "ra_crc.h"
#include "ra_err.h"
#include "ra_glcdc.h"
#include "ra_gpt.h"
#include "ra_mstp.h"
#include "ra_nsc_io.h"
#include "ra_sim_mmap.h"
#include "unity_minimal.h"

static void prep(void)
{
  ra_sim_mmap_reset();
  (void)ra_mstp_init();
}

static void test_gpt_init_forwards(void)
{
  TEST_BEGIN("ra_nsc_gpt_init forwards + null rejected");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_nsc_gpt_init(0U, nullptr));
  ra_gpt_cfg_t cfg = {};
  cfg.mode         = k_ra_gpt_mode_saw_pwm;
  cfg.prescaler    = k_ra_gpt_ps_div_4;
  cfg.period       = 1000U;
  cfg.duty_a       = 500U;
  cfg.duty_b       = 0U;
  cfg.auto_start   = true;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_nsc_gpt_init(0U, &cfg));
  uint32_t cnt = 0U;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_nsc_gpt_read(0U, &cnt));
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_nsc_gpt_read(0U, nullptr));
  TEST_END("ra_nsc_gpt_init forwards + null rejected");
}

static void test_adc_dac_acmphs_init_forwards(void)
{
  TEST_BEGIN("ra_nsc_{adc,dac_b,acmphs}_init forwards");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_nsc_adc_init());
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_nsc_dac_b_init());
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_nsc_acmphs_init());
  uint16_t raw = 0U;
  /* The host ADC sim does not set conversion-done flags, so the
   * driver returns hw_timeout. The veneer test only cares that the
   * forwarding path runs and the null-ptr guard fires. */
  (void)ra_nsc_adc_read_channel(0U, &raw);
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_nsc_adc_read_channel(0U, nullptr));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_nsc_dac_b_write(0U, 0x800U));
  ra_level_t lvl = k_ra_level_low;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_nsc_acmphs_read_output(0U, &lvl));
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_nsc_acmphs_read_output(0U, nullptr));
  TEST_END("ra_nsc_{adc,dac_b,acmphs}_init forwards");
}

static void test_crc_init_compute(void)
{
  TEST_BEGIN("ra_nsc_crc_init + compute");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_nsc_crc_init(k_ra_crc_poly_16_ccitt));
  uint32_t      out  = 0U;
  const uint8_t data = 0xAAU;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_nsc_crc_compute(&data, 1U, &out));
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_nsc_crc_compute(nullptr, 1U, &out));
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_nsc_crc_compute(&data, 1U, nullptr));
  TEST_END("ra_nsc_crc_init + compute");
}

static void test_glcdc_pdm_eth_init(void)
{
  TEST_BEGIN("ra_nsc_{glcdc,pdm,eth}_init forwards");
  prep();
  ra_glcdc_config_t glcfg = {};
  glcfg.width_px          = 800U;
  glcfg.height_px         = 480U;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_nsc_glcdc_init(&glcfg));
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_nsc_glcdc_init(nullptr));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_nsc_pdm_init());
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_nsc_eth_init());
  TEST_END("ra_nsc_{glcdc,pdm,eth}_init forwards");
}

int32_t main(void)
{
  test_gpt_init_forwards();
  test_adc_dac_acmphs_init_forwards();
  test_crc_init_compute();
  test_glcdc_pdm_eth_init();
  (void)fprintf(stderr, "[OK ] test_ra_nsc_io.c\n");
  return 0;
}
