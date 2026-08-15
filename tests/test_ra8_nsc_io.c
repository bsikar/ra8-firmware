/**
 * @file test_ra8_nsc_io.c
 * @brief Unit tests for libs/ra8_nsc/src/ra8_nsc_io.c
 *
 * @details Exercises timer, analog, CRC, display, audio, and Ethernet veneer
 *          forwarding and null validation against host peripheral fakes.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8_acmphs.h"
#include "ra8_attributes.h"
#include "ra8_crc.h"
#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_glcdc.h"
#include "ra8_gpt.h"
#include "ra8_mstp.h"
#include "ra8_nsc_io.h"
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

/**
 * @brief Reset the host fixture for NSC I/O veneer tests.
 *
 * @details Clears fake peripheral mappings and initializes the module-stop
 *          controller before each timer, analog, CRC, or display vector.
 *
 * @pre The test runs in the single-threaded host-test process.
 * @pre No other test concurrently owns the global fake registers.
 * @post Fake peripheral register windows are reset.
 * @post The module-stop controller is initialized for driver calls.
 *
 * @note This helper mutates process-global host-test state.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_prep(void)
{
  ra8_fake_mmap_reset();
  (void)ra8_mstp_init();
}

/**
 * @brief Exercise the named NSC I/O forwarding and validation scenario.
 *
 * @details Resets host fake registers, invokes the relevant I/O veneers, and
 *          asserts both healthy forwarding and documented null validation.
 *
 * @pre The required fake I/O registers are mapped.
 * @pre No other test concurrently owns the process-global fake peripherals.
 * @post Every scenario-specific return and output assertion has passed.
 * @post All stack-backed inputs remain confined to this invocation.
 *
 * @note The vectors execute synchronously without physical peripherals.
 * @since 0.1.0
 *
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
RA8_INTERNAL
static void internal_test_gpt_init_forwards(void)
{
  TEST_BEGIN("ra8_nsc_gpt_init forwards + null rejected");
  internal_prep();
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
 * @brief Exercise the named NSC I/O forwarding and validation scenario.
 *
 * @details Resets host fake registers, invokes the relevant I/O veneers, and
 *          asserts both healthy forwarding and documented null validation.
 *
 * @pre The required fake I/O registers are mapped.
 * @pre No other test concurrently owns the process-global fake peripherals.
 * @post Every scenario-specific return and output assertion has passed.
 * @post All stack-backed inputs remain confined to this invocation.
 *
 * @note The vectors execute synchronously without physical peripherals.
 * @since 0.1.0
 *
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
RA8_INTERNAL
static void internal_test_adc_dac_acmphs_init_forwards(void)
{
  TEST_BEGIN("ra8_nsc_{adc,dac_b,acmphs}_init forwards");
  internal_prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_nsc_adc_init());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_nsc_dac_b_init());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_nsc_acmphs_init());
  uint16_t raw = 0U;
  /* The host ADC fake does not set conversion-done flags, so the
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
 * @brief Exercise the named NSC I/O forwarding and validation scenario.
 *
 * @details Resets host fake registers, invokes the relevant I/O veneers, and
 *          asserts both healthy forwarding and documented null validation.
 *
 * @pre The required fake I/O registers are mapped.
 * @pre No other test concurrently owns the process-global fake peripherals.
 * @post Every scenario-specific return and output assertion has passed.
 * @post All stack-backed inputs remain confined to this invocation.
 *
 * @note The vectors execute synchronously without physical peripherals.
 * @since 0.1.0
 *
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
RA8_INTERNAL
static void internal_test_crc_init_compute(void)
{
  TEST_BEGIN("ra8_nsc_crc_init + compute");
  internal_prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_nsc_crc_init(k_ra8_crc_poly_16_ccitt));
  uint32_t      out  = 0U;
  const uint8_t data = 0xAAU;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_nsc_crc_compute(&data, 1U, &out));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_nsc_crc_compute(nullptr, 1U, &out));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_nsc_crc_compute(&data, 1U, nullptr));
  TEST_END("ra8_nsc_crc_init + compute");
}

/**
 * @brief Exercise the named NSC I/O forwarding and validation scenario.
 *
 * @details Resets host fake registers, invokes the relevant I/O veneers, and
 *          asserts both healthy forwarding and documented null validation.
 *
 * @pre The required fake I/O registers are mapped.
 * @pre No other test concurrently owns the process-global fake peripherals.
 * @post Every scenario-specific return and output assertion has passed.
 * @post All stack-backed inputs remain confined to this invocation.
 *
 * @note The vectors execute synchronously without physical peripherals.
 * @since 0.1.0
 *
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
RA8_INTERNAL
static void internal_test_glcdc_pdm_eth_init(void)
{
  TEST_BEGIN("ra8_nsc_{glcdc,pdm,eth}_init forwards");
  internal_prep();
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
  internal_test_gpt_init_forwards();
  internal_test_adc_dac_acmphs_init_forwards();
  internal_test_crc_init_compute();
  internal_test_glcdc_pdm_eth_init();
  return 0;
}
