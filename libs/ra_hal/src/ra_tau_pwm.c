/**
 * @file ra_tau_pwm.c
 * @brief TAU PWM-mode driver -- placeholder implementation
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

/* NOLINTBEGIN(readability-magic-numbers,readability-function-size,readability-function-cognitive-complexity) */

#include "ra_tau_pwm.h"

#include <stdint.h>

#include "ra_check.h"
#include "ra_err.h"
#include "ra_log.h"

static const char* s_tag = "TAU_PWM";

typedef struct {
  bool     opened;
  bool     running;
  uint16_t period;
  uint16_t duty;
} ra_tau_pwm_state_t;

static ra_tau_pwm_state_t s_state = {};

ra_err_t ra_tau_pwm_open(const ra_tau_pwm_cfg_t* cfg)
{
  RA_CHECK_NULL_PTR(cfg, s_tag, "cfg must not be nullptr");
  if (cfg->period == 0U) {
    return k_ra_err_invalid_arg;
  }
  if (cfg->duty >= cfg->period) {
    return k_ra_err_invalid_arg;
  }
  if (s_state.opened) {
    return k_ra_err_exists;
  }
  s_state.opened  = true;
  s_state.running = false;
  s_state.period  = cfg->period;
  s_state.duty    = cfg->duty;
  ra_log_info_val(s_tag, "tau_pwm open ch", (uint32_t)cfg->channel);
  return k_ra_ok;
}

ra_err_t ra_tau_pwm_start(void)
{
  if (!s_state.opened) {
    return k_ra_err_not_initialized;
  }
  s_state.running = true;
  return k_ra_ok;
}

ra_err_t ra_tau_pwm_stop(void)
{
  if (!s_state.opened) {
    return k_ra_err_not_initialized;
  }
  s_state.running = false;
  return k_ra_ok;
}

ra_err_t ra_tau_pwm_duty_set(uint16_t duty)
{
  if (!s_state.opened) {
    return k_ra_err_not_initialized;
  }
  if (duty >= s_state.period) {
    return k_ra_err_invalid_arg;
  }
  s_state.duty = duty;
  return k_ra_ok;
}

ra_err_t ra_tau_pwm_period_set(uint16_t period)
{
  if (!s_state.opened) {
    return k_ra_err_not_initialized;
  }
  if (period == 0U) {
    return k_ra_err_invalid_arg;
  }
  if (period <= s_state.duty) {
    return k_ra_err_invalid_arg;
  }
  s_state.period = period;
  return k_ra_ok;
}

ra_err_t ra_tau_pwm_status_get(uint8_t* out_open, uint8_t* out_running)
{
  RA_CHECK_NULL_PTR(out_open, s_tag, "out_open must not be nullptr");
  RA_CHECK_NULL_PTR(out_running, s_tag, "out_running must not be nullptr");
  *out_open    = (uint8_t)(s_state.opened ? 1U : 0U);
  *out_running = (uint8_t)(s_state.running ? 1U : 0U);
  return k_ra_ok;
}

ra_err_t ra_tau_pwm_close(void)
{
  if (!s_state.opened) {
    return k_ra_err_invalid_state;
  }
  s_state.opened  = false;
  s_state.running = false;
  s_state.period  = 0U;
  s_state.duty    = 0U;
  return k_ra_ok;
}

/* NOLINTEND(readability-magic-numbers,readability-function-size,readability-function-cognitive-complexity) */
