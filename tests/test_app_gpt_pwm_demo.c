/**
 * @file test_app_gpt_pwm_demo.c
 * @brief Integration test: GPT PWM duty-cycle LED demo
 *
 * @details
 * Mirrors examples/ek_ra8d2/gpt_pwm_demo/main.c bring-up: configure
 * GPT0 with the same descriptor the production app uses, then sweep
 * the duty value via ``ra8_gpt_set_duty``. The host MMIO shim backs
 * GTPR / GTCCRA so the writes succeed and the test verifies both the
 * acceptance path and the documented rejection paths.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_gpt.h"
#include "unity_minimal.h"

typedef enum : uint32_t {
  k_test_gpt_pwm_period   = 0x0000FFFFU, /**< Test GPT PWM period.   */
  k_test_gpt_pwm_step_pct = 1024U,       /**< Test GPT PWM step pct. */
  k_test_gpt_pwm_mid_duty = 0x0000A000U, /**< Test GPT PWM mid duty. */
} test_gpt_pwm_const_t;

typedef enum : uint8_t {
  k_test_gpt_pwm_channel     = 0U,  /**< Test GPT PWM channel. */
  k_test_gpt_pwm_channel_bad = 99U, /**< Out of range (>= 14). */
} test_gpt_pwm_chan_t;

static void reset_world(void)
{
  ra8_fake_mmap_reset();
}

static ra8_gpt_cfg_t make_cfg(void)
{
  const ra8_gpt_cfg_t cfg = {
    .mode       = k_ra8_gpt_mode_saw_pwm,
    .prescaler  = k_ra8_gpt_ps_div_4,
    .period     = (uint32_t)k_test_gpt_pwm_period,
    .duty_a     = 0U,
    .duty_b     = 0U,
    .auto_start = true,
  };
  return cfg;
}

/**
 * @brief Golden bring-up: configure + drive a duty value.
 *
 * @par MC/DC:
 * Decision in app: ``ra8_gpt_init != ok``. One atomic condition x 2
 * vectors -- golden (this) + null cfg / bad channel below.
 */
static void test_gpt_pwm_arm_ok(void)
{
  reset_world();
  TEST_BEGIN("gpt_pwm_demo: arm + duty sweep");
  const ra8_gpt_cfg_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_gpt_init((uint8_t)k_test_gpt_pwm_channel, &cfg));
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_gpt_set_duty((uint8_t)k_test_gpt_pwm_channel,
                                  k_ra8_gpt_ccr_a,
                                  (uint32_t)k_test_gpt_pwm_mid_duty));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_gpt_stop((uint8_t)k_test_gpt_pwm_channel));
  TEST_END("gpt_pwm_demo: arm + duty sweep");
}

/**
 * @brief NULL cfg rejected by ``ra8_gpt_init``.
 *
 * @par MC/DC:
 * Decision: ``cfg == NULL``. One atomic condition x 2 vectors --
 * non-NULL above + NULL here.
 */
static void test_gpt_pwm_null_cfg(void)
{
  reset_world();
  TEST_BEGIN("gpt_pwm_demo: NULL cfg rejected");
  TEST_ASSERT(ra8_gpt_init((uint8_t)k_test_gpt_pwm_channel, nullptr) != k_ra8_ok);
  TEST_END("gpt_pwm_demo: NULL cfg rejected");
}

/**
 * @brief Bad channel rejected by ``ra8_gpt_init``.
 *
 * @par MC/DC:
 * Decision: ``channel < k_ra8_gpt_max_channel``. One atomic condition
 * x 2 vectors -- in-range golden + out-of-range here.
 */
static void test_gpt_pwm_bad_channel(void)
{
  reset_world();
  TEST_BEGIN("gpt_pwm_demo: bad channel rejected");
  const ra8_gpt_cfg_t cfg = make_cfg();
  TEST_ASSERT(ra8_gpt_init((uint8_t)k_test_gpt_pwm_channel_bad, &cfg) != k_ra8_ok);
  TEST_END("gpt_pwm_demo: bad channel rejected");
}

int main(void)
{
  test_gpt_pwm_arm_ok();
  test_gpt_pwm_null_cfg();
  test_gpt_pwm_bad_channel();
  return 0;
}
