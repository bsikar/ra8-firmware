/**
 * @file test_ra8_display_pal_backlight.c
 * @brief Host unit tests for display backlight control and idle dim policy.
 *
 * @details
 * Exercises ``display_backlight_percent_to_q16``, binary-mode fail-closed
 * validation (rejecting 1..99), PWM-mode ``fw_pwm_t`` Q16 duty scaling +
 * enable gate sequencing, and ``display_backlight_policy_*`` across wrap-around
 * millisecond timestamps and binary vs PWM hardware capabilities.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>

#include "fw_if_pwm.h"
#include "ra8_display_pal_backlight.h"
#include "ra8_err.h"
#include "unity_minimal.h"

/* Pin C ABI struct layout assumptions shared with ra8_display_pal_backlight_abi.zig. */
static_assert(sizeof(display_backlight_policy_cfg_t) == 12U,
              "display_backlight_policy_cfg_t size must match Zig PolicyCfg");
static_assert(sizeof(display_backlight_policy_t) == 20U,
              "display_backlight_policy_t size must match Zig Policy");
static_assert(sizeof(display_backlight_policy_decision_t) == 4U,
              "display_backlight_policy_decision_t size must match Zig PolicyDecision");
static_assert(offsetof(display_backlight_cfg_t, mode) == sizeof(void*) * 3U + 4U,
              "display_backlight_cfg_t mode offset mismatch");

typedef struct fake_backlight_hw_s {
  uint32_t  open_calls;
  uint32_t  close_calls;
  uint32_t  start_calls;
  uint32_t  stop_calls;
  uint32_t  duty_calls;
  uint32_t  gate_calls;
  uint32_t  last_period;
  uint32_t  last_duty;
  uint8_t   last_ch;
  uint8_t   last_pol;
  uint8_t   last_gate;
  uint8_t   has_active_low;
  ra8_err_t next_err;
} fake_backlight_hw_t;

static ra8_err_t fake_pwm_get_caps(void* ctx, fw_pwm_caps_t* out)
{
  const fake_backlight_hw_t* hw = (const fake_backlight_hw_t*)ctx;
  out->channel_count            = 2U;
  out->counter_bits             = 32U;
  out->period_max               = 100000U;
  out->has_active_low           = (hw->has_active_low != 0U);
  return k_ra8_ok;
}

static ra8_err_t fake_pwm_open(void* ctx, fw_pwm_ch_t ch, uint32_t period, fw_pwm_polarity_t pol)
{
  fake_backlight_hw_t* hw = (fake_backlight_hw_t*)ctx;
  if (hw->next_err != k_ra8_ok) {
    return hw->next_err;
  }
  hw->open_calls++;
  hw->last_ch     = ch.index;
  hw->last_period = period;
  hw->last_pol    = (uint8_t)pol;
  return k_ra8_ok;
}

static ra8_err_t fake_pwm_close(void* ctx, fw_pwm_ch_t ch)
{
  fake_backlight_hw_t* hw = (fake_backlight_hw_t*)ctx;
  hw->close_calls++;
  hw->last_ch = ch.index;
  return k_ra8_ok;
}

static ra8_err_t fake_pwm_start(void* ctx, fw_pwm_ch_t ch)
{
  fake_backlight_hw_t* hw = (fake_backlight_hw_t*)ctx;
  hw->start_calls++;
  hw->last_ch = ch.index;
  return k_ra8_ok;
}

static ra8_err_t fake_pwm_stop(void* ctx, fw_pwm_ch_t ch)
{
  fake_backlight_hw_t* hw = (fake_backlight_hw_t*)ctx;
  hw->stop_calls++;
  hw->last_ch = ch.index;
  return k_ra8_ok;
}

static ra8_err_t fake_pwm_set_period(void* ctx, fw_pwm_ch_t ch, uint32_t period)
{
  fake_backlight_hw_t* hw = (fake_backlight_hw_t*)ctx;
  hw->last_ch             = ch.index;
  hw->last_period         = period;
  return k_ra8_ok;
}

static ra8_err_t fake_pwm_set_duty(void* ctx, fw_pwm_ch_t ch, fw_pwm_duty_t duty)
{
  fake_backlight_hw_t* hw = (fake_backlight_hw_t*)ctx;
  if (hw->next_err != k_ra8_ok) {
    return hw->next_err;
  }
  hw->duty_calls++;
  hw->last_ch   = ch.index;
  hw->last_duty = duty;
  return k_ra8_ok;
}

static ra8_err_t fake_gate_set(void* ctx, uint8_t on)
{
  fake_backlight_hw_t* hw = (fake_backlight_hw_t*)ctx;
  if (hw->next_err != k_ra8_ok) {
    return hw->next_err;
  }
  hw->gate_calls++;
  hw->last_gate = on;
  return k_ra8_ok;
}

static const fw_pwm_iface_t s_fake_pwm_iface = {
  .get_caps   = fake_pwm_get_caps,
  .open       = fake_pwm_open,
  .close      = fake_pwm_close,
  .start      = fake_pwm_start,
  .stop       = fake_pwm_stop,
  .set_period = fake_pwm_set_period,
  .set_duty   = fake_pwm_set_duty,
};

/**
 * @test test_percent_to_q16
 * @brief Verify Q16 duty scaling across endpoints and invalid (>100) inputs.
 */
static void test_percent_to_q16(void)
{
  TEST_BEGIN("backlight percent_to_q16 conversion and bounds");
  uint32_t q16 = 0U;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, display_backlight_percent_to_q16(50U, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, display_backlight_percent_to_q16(101U, &q16));

  TEST_ASSERT_EQ(k_ra8_ok, display_backlight_percent_to_q16(0U, &q16));
  TEST_ASSERT_EQ(0U, q16);
  TEST_ASSERT_EQ(k_ra8_ok, display_backlight_percent_to_q16(25U, &q16));
  TEST_ASSERT_EQ(16384U, q16);
  TEST_ASSERT_EQ(k_ra8_ok, display_backlight_percent_to_q16(50U, &q16));
  TEST_ASSERT_EQ(32768U, q16);
  TEST_ASSERT_EQ(k_ra8_ok, display_backlight_percent_to_q16(100U, &q16));
  TEST_ASSERT_EQ(K_FW_PWM_DUTY_FULL, q16);
  TEST_END("backlight percent_to_q16 conversion and bounds");
}

/**
 * @test test_binary_mode_controller
 * @brief Verify binary backlight only permits 0/100 and fails closed on 1..99.
 */
static void test_binary_mode_controller(void)
{
  TEST_BEGIN("backlight binary mode: 0/100 valid, 1..99 fails closed");
  fake_backlight_hw_t     hw  = {.last_gate = 0xFFU};
  display_backlight_t     dev = {};
  display_backlight_cfg_t cfg = {
    .mode       = k_display_backlight_mode_binary,
    .set_enable = fake_gate_set,
    .ctx        = &hw,
  };

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, display_backlight_init(nullptr, &cfg));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, display_backlight_init(&dev, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, display_backlight_set_brightness(&dev, 100U));

  TEST_ASSERT_EQ(k_ra8_ok, display_backlight_init(&dev, &cfg));
  TEST_ASSERT_EQ(1U, hw.gate_calls);
  TEST_ASSERT_EQ(0U, hw.last_gate);

  TEST_ASSERT_EQ(k_ra8_ok, display_backlight_set_brightness(&dev, 100U));
  TEST_ASSERT_EQ(1U, hw.last_gate);
  TEST_ASSERT_EQ(100U, dev.brightness_pct);

  /* Intermediate brightness must refuse without toggling hardware. */
  TEST_ASSERT_EQ(k_ra8_err_not_supported, display_backlight_set_brightness(&dev, 40U));
  TEST_ASSERT_EQ(2U, hw.gate_calls);
  TEST_ASSERT_EQ(100U, dev.brightness_pct);

  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, display_backlight_set_brightness(&dev, 101U));
  TEST_ASSERT_EQ(k_ra8_ok, display_backlight_deinit(&dev));
  TEST_ASSERT_EQ(0U, dev.initialized);
  TEST_ASSERT_EQ(0U, hw.last_gate);
  TEST_END("backlight binary mode: 0/100 valid, 1..99 fails closed");
}

/**
 * @test test_pwm_mode_controller
 * @brief Verify PWM mode binds fw_pwm_t, scales Q16 duty, and gates start/stop.
 */
static void test_pwm_mode_controller(void)
{
  TEST_BEGIN("backlight PWM mode: fw_pwm_t duty scaling + gate");
  fake_backlight_hw_t hw  = {.last_gate = 0xFFU};
  fw_pwm_t            pwm = {};
  TEST_ASSERT_EQ(k_ra8_ok, fw_pwm_bind(&pwm, &s_fake_pwm_iface, &hw));

  display_backlight_t     dev = {};
  display_backlight_cfg_t cfg = {
    .pwm              = &pwm,
    .set_enable       = fake_gate_set,
    .ctx              = &hw,
    .pwm_period_ticks = 20000U,
    .mode             = k_display_backlight_mode_pwm,
    .pwm_ch           = {.index = 1U},
    .pwm_polarity     = k_fw_pwm_pol_active_high,
  };
  TEST_ASSERT_EQ(k_ra8_ok, display_backlight_init(&dev, &cfg));
  TEST_ASSERT_EQ(1U, hw.open_calls);
  TEST_ASSERT_EQ(1U, hw.last_ch);
  TEST_ASSERT_EQ(20000U, hw.last_period);
  TEST_ASSERT_EQ(0U, hw.start_calls);
  TEST_ASSERT_EQ(0U, hw.last_gate);

  TEST_ASSERT_EQ(k_ra8_ok, display_backlight_set_brightness(&dev, 50U));
  TEST_ASSERT_EQ(1U, hw.start_calls);
  TEST_ASSERT_EQ(1U, hw.last_gate);
  TEST_ASSERT_EQ(32768U, hw.last_duty);

  TEST_ASSERT_EQ(k_ra8_ok, display_backlight_set_brightness(&dev, 100U));
  TEST_ASSERT_EQ(1U, hw.start_calls); /* Already running; no duplicate start. */
  TEST_ASSERT_EQ(K_FW_PWM_DUTY_FULL, hw.last_duty);

  TEST_ASSERT_EQ(k_ra8_ok, display_backlight_set_brightness(&dev, 0U));
  TEST_ASSERT_EQ(1U, hw.stop_calls);
  TEST_ASSERT_EQ(0U, hw.last_duty);
  TEST_ASSERT_EQ(0U, hw.last_gate);

  TEST_ASSERT_EQ(k_ra8_ok, display_backlight_deinit(&dev));
  TEST_ASSERT_EQ(1U, hw.close_calls);
  TEST_END("backlight PWM mode: fw_pwm_t duty scaling + gate");
}

/**
 * @test test_policy_pwm_and_binary_lifecycle
 * @brief Verify ACTIVE -> DIMMED -> OFF -> ACTIVE policy with hardware actuation.
 */
static void test_policy_pwm_and_binary_lifecycle(void)
{
  TEST_BEGIN("backlight policy: ACTIVE -> DIMMED -> OFF -> ACTIVE + binary skip");
  fake_backlight_hw_t hw  = {.last_gate = 0xFFU};
  fw_pwm_t            pwm = {};
  TEST_ASSERT_EQ(k_ra8_ok, fw_pwm_bind(&pwm, &s_fake_pwm_iface, &hw));

  display_backlight_t     dev     = {};
  display_backlight_cfg_t dev_cfg = {
    .pwm              = &pwm,
    .set_enable       = nullptr,
    .ctx              = nullptr,
    .pwm_period_ticks = 10000U,
    .mode             = k_display_backlight_mode_pwm,
    .pwm_ch           = {.index = 0U},
    .pwm_polarity     = k_fw_pwm_pol_active_high,
  };
  TEST_ASSERT_EQ(k_ra8_ok, display_backlight_init(&dev, &dev_cfg));
  TEST_ASSERT_EQ(k_ra8_ok, display_backlight_set_brightness(&dev, 100U));

  display_backlight_policy_t           pol     = {};
  display_backlight_policy_decision_t  dec     = {};
  const display_backlight_policy_cfg_t pol_cfg = {
    .dim_after_ms = 10000U,
    .off_after_ms = 30000U,
    .active_pct   = 100U,
    .dimmed_pct   = 25U,
    .supports_dim = 1U,
  };

  /* Start near uint32_t wrap boundary to verify wrap-safe elapsed arithmetic. */
  const uint32_t t0 = 0xFFFFE000U;
  TEST_ASSERT_EQ(k_ra8_ok, display_backlight_policy_init(&pol, &pol_cfg, t0));
  TEST_ASSERT_EQ(k_ra8_ok, display_backlight_policy_step(&pol, t0 + 9999U, &dec));
  TEST_ASSERT_EQ(k_display_backlight_state_active, dec.state);
  TEST_ASSERT_EQ(0U, dec.changed);

  /* At +10s: ACTIVE -> DIMMED (25% -> duty 16384). */
  TEST_ASSERT_EQ(k_ra8_ok, display_backlight_policy_step(&pol, t0 + 10000U, &dec));
  TEST_ASSERT_EQ(k_display_backlight_state_dimmed, dec.state);
  TEST_ASSERT_EQ(25U, dec.target_pct);
  TEST_ASSERT_EQ(1U, dec.changed);
  TEST_ASSERT_EQ(1U, dec.on);
  TEST_ASSERT_EQ(k_ra8_ok, display_backlight_apply_decision(&dev, &dec));
  TEST_ASSERT_EQ(16384U, hw.last_duty);

  /* At +30s (wrapped past 0): DIMMED -> OFF (0%). */
  TEST_ASSERT_EQ(k_ra8_ok, display_backlight_policy_step(&pol, t0 + 30000U, &dec));
  TEST_ASSERT_EQ(k_display_backlight_state_off, dec.state);
  TEST_ASSERT_EQ(0U, dec.target_pct);
  TEST_ASSERT_EQ(1U, dec.changed);
  TEST_ASSERT_EQ(0U, dec.on);
  TEST_ASSERT_EQ(k_ra8_ok, display_backlight_apply_decision(&dev, &dec));
  TEST_ASSERT_EQ(1U, hw.stop_calls);

  /* Activity wakes cleanly back to ACTIVE (100%). */
  TEST_ASSERT_EQ(k_ra8_ok, display_backlight_policy_activity(&pol, t0 + 35000U, &dec));
  TEST_ASSERT_EQ(k_display_backlight_state_active, dec.state);
  TEST_ASSERT_EQ(100U, dec.target_pct);
  TEST_ASSERT_EQ(1U, dec.changed);
  TEST_ASSERT_EQ(k_ra8_ok, display_backlight_apply_decision(&dev, &dec));
  TEST_ASSERT_EQ(K_FW_PWM_DUTY_FULL, hw.last_duty);
  TEST_ASSERT_EQ(2U, hw.start_calls);

  /* Binary policy skips DIMMED cleanly and transitions ACTIVE -> OFF at 30s. */
  display_backlight_policy_t           bin_pol = {};
  const display_backlight_policy_cfg_t bin_cfg = {
    .dim_after_ms = 15000U,
    .off_after_ms = 30000U,
    .active_pct   = 100U,
    .dimmed_pct   = 25U,
    .supports_dim = 0U,
  };
  TEST_ASSERT_EQ(k_ra8_ok, display_backlight_policy_init(&bin_pol, &bin_cfg, 10U));
  TEST_ASSERT_EQ(k_ra8_ok, display_backlight_policy_step(&bin_pol, 20010U, &dec));
  TEST_ASSERT_EQ(k_display_backlight_state_active, dec.state);
  TEST_ASSERT_EQ(100U, dec.target_pct);
  TEST_ASSERT_EQ(k_ra8_ok, display_backlight_policy_step(&bin_pol, 30010U, &dec));
  TEST_ASSERT_EQ(k_display_backlight_state_off, dec.state);
  TEST_ASSERT_EQ(0U, dec.target_pct);

  TEST_ASSERT_EQ(k_ra8_ok, display_backlight_deinit(&dev));
  TEST_END("backlight policy: ACTIVE -> DIMMED -> OFF -> ACTIVE + binary skip");
}

int main(void)
{
  test_percent_to_q16();
  test_binary_mode_controller();
  test_pwm_mode_controller();
  test_policy_pwm_and_binary_lifecycle();
  return 0;
}
