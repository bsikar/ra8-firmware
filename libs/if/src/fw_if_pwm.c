/**
 * @file fw_if_pwm.c
 * @brief Facade half of the neutral PWM port: guards, range checks, pass-through.
 * @ingroup grp_fw_pwm
 *
 * @par Tag
 * [Ring 2 / Interface] {World: Any}
 *
 * @details
 * Implements `fw_if_pwm.h`. Guards and forwards only; no state beyond the
 * caller's handle and no register. The checks that matter are the period
 * against the counter width and the duty against ::K_FW_PWM_DUTY_FULL: both
 * fail silently as a wrong waveform if they reach a backend unchecked.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include "fw_if_pwm.h"

#include <stdbool.h>
#include <stdint.h>

#include "ra8_err.h"

/** @brief Whether every op in a candidate binding is set. */
static bool internal_iface_complete(const fw_pwm_iface_t* iface)
{
  return (iface->get_caps != nullptr) && (iface->open != nullptr) && (iface->close != nullptr) &&
         (iface->start != nullptr) && (iface->stop != nullptr) && (iface->set_period != nullptr) &&
         (iface->set_duty != nullptr);
}

/** @brief Common entry guard: non-NULL handle, bound, output present. */
static ra8_err_t internal_check_channel(const fw_pwm_t* pwm, fw_pwm_ch_t ch)
{
  if (pwm == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  if (!pwm->bound) {
    return k_ra8_err_not_initialized;
  }
  if (ch.index >= pwm->caps.channel_count) {
    return k_ra8_err_not_found;
  }
  return k_ra8_ok;
}

/** @brief Whether a period is usable on this backend's counter. */
static ra8_err_t internal_period_ok(const fw_pwm_t* pwm, uint32_t period)
{
  if (period == 0U) {
    return k_ra8_err_invalid_arg;
  }
  if (period > pwm->caps.period_max) {
    return k_ra8_err_out_of_range;
  }
  return k_ra8_ok;
}

/** @brief Whether @p pol is enumerated and offered by this backend. */
static ra8_err_t internal_polarity_ok(const fw_pwm_t* pwm, fw_pwm_polarity_t pol)
{
  if ((pol == k_fw_pwm_pol_none) || ((uint8_t)pol >= (uint8_t)K_FW_PWM_POL_COUNT)) {
    return k_ra8_err_invalid_arg;
  }
  if ((pol == k_fw_pwm_pol_active_low) && !pwm->caps.has_active_low) {
    return k_ra8_err_not_supported;
  }
  return k_ra8_ok;
}

ra8_err_t fw_pwm_bind(fw_pwm_t* pwm, const fw_pwm_iface_t* iface, void* ctx)
{
  if ((pwm == nullptr) || (iface == nullptr)) {
    return k_ra8_err_invalid_arg;
  }
  if (!internal_iface_complete(iface)) {
    return k_ra8_err_invalid_arg;
  }

  fw_pwm_caps_t   caps = {};
  const ra8_err_t err  = iface->get_caps(ctx, &caps);
  if (err != k_ra8_ok) {
    return err;
  }
  /* A zero width or period_max makes every later period check vacuous. */
  if ((caps.counter_bits == 0U) || (caps.period_max == 0U)) {
    return k_ra8_err_invalid_state;
  }

  pwm->iface = iface;
  pwm->ctx   = ctx;
  pwm->caps  = caps;
  pwm->bound = true;
  return k_ra8_ok;
}

ra8_err_t fw_pwm_get_caps(const fw_pwm_t* pwm, fw_pwm_caps_t* out)
{
  if (out == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  const fw_pwm_caps_t zeroed = {};
  *out                       = zeroed;

  if (pwm == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  if (!pwm->bound) {
    return k_ra8_err_not_initialized;
  }
  *out = pwm->caps;
  return k_ra8_ok;
}

ra8_err_t fw_pwm_open(const fw_pwm_t* pwm, fw_pwm_ch_t ch, uint32_t period, fw_pwm_polarity_t pol)
{
  const ra8_err_t guard = internal_check_channel(pwm, ch);
  if (guard != k_ra8_ok) {
    return guard;
  }
  const ra8_err_t pol_err = internal_polarity_ok(pwm, pol);
  if (pol_err != k_ra8_ok) {
    return pol_err;
  }
  const ra8_err_t period_err = internal_period_ok(pwm, period);
  if (period_err != k_ra8_ok) {
    return period_err;
  }
  return pwm->iface->open(pwm->ctx, ch, period, pol);
}

ra8_err_t fw_pwm_close(const fw_pwm_t* pwm, fw_pwm_ch_t ch)
{
  const ra8_err_t guard = internal_check_channel(pwm, ch);
  if (guard != k_ra8_ok) {
    return guard;
  }
  return pwm->iface->close(pwm->ctx, ch);
}

ra8_err_t fw_pwm_start(const fw_pwm_t* pwm, fw_pwm_ch_t ch)
{
  const ra8_err_t guard = internal_check_channel(pwm, ch);
  if (guard != k_ra8_ok) {
    return guard;
  }
  return pwm->iface->start(pwm->ctx, ch);
}

ra8_err_t fw_pwm_stop(const fw_pwm_t* pwm, fw_pwm_ch_t ch)
{
  const ra8_err_t guard = internal_check_channel(pwm, ch);
  if (guard != k_ra8_ok) {
    return guard;
  }
  return pwm->iface->stop(pwm->ctx, ch);
}

ra8_err_t fw_pwm_set_period(const fw_pwm_t* pwm, fw_pwm_ch_t ch, uint32_t period)
{
  const ra8_err_t guard = internal_check_channel(pwm, ch);
  if (guard != k_ra8_ok) {
    return guard;
  }
  const ra8_err_t period_err = internal_period_ok(pwm, period);
  if (period_err != k_ra8_ok) {
    return period_err;
  }
  return pwm->iface->set_period(pwm->ctx, ch, period);
}

ra8_err_t fw_pwm_set_duty(const fw_pwm_t* pwm, fw_pwm_ch_t ch, fw_pwm_duty_t duty)
{
  const ra8_err_t guard = internal_check_channel(pwm, ch);
  if (guard != k_ra8_ok) {
    return guard;
  }
  /* Above full scale has no waveform; a backend scaling it would wrap. */
  if (duty > (fw_pwm_duty_t)K_FW_PWM_DUTY_FULL) {
    return k_ra8_err_out_of_range;
  }
  return pwm->iface->set_duty(pwm->ctx, ch, duty);
}
