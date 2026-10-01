/**
 * @file fw_if_pwm_ra8.c
 * @brief The seven `fw_if_pwm` ops, over `ra8_gpt` pin A.
 * @ingroup grp_fw_pwm
 *
 * @par Tag
 * [Ring 3 / HAL] {World: S}
 *
 * @details
 * The facade has already refused a bad handle, an unknown output, a zero or
 * oversized period, an unenumerated polarity and a duty above full. What is
 * checked here is ownership of the channel, and what is kept here is each
 * open output's period, duty and whether it is counting, because the port
 * asks the backend to preserve the ratio across a period change.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include "fw_if_pwm_ra8.h"

#include <stdbool.h>
#include <stdint.h>

#include "fw_if_gpt_ra8_claim.h"
#include "fw_if_pwm.h"
#include "ra8_err.h"
#include "ra8_gpt.h"

/** @brief What an open output was last told. */
typedef struct {
  uint32_t      period;  /**< Current wrap point.     */
  fw_pwm_duty_t duty;    /**< Current Q16 ratio.      */
  bool          running; /**< Between start and stop. */
} internal_out_t;

/** @brief Per-output state, meaningful only while the channel is ours. */
static internal_out_t s_out[k_fw_pwm_ra8_channel_count];

/**
 * @brief Whether this adapter holds @p ch.
 *
 * @param[in] ch Output.
 * @return True when opened here and not yet closed.
 */
static bool internal_is_open(fw_pwm_ch_t ch)
{
  return fw_gpt_ra8_owned_by(ch.index, k_fw_gpt_ra8_owner_pwm);
}

/**
 * @brief Compare value for @p duty of a cycle of @p period + 1 counts.
 *
 * @param[in] period Wrap point, at most ::k_fw_pwm_ra8_period_max.
 * @param[in] duty   Q16 ratio, at most ::K_FW_PWM_DUTY_FULL.
 * @return Counts the output spends active; period + 1 at full duty.
 */
static uint32_t internal_compare(uint32_t period, fw_pwm_duty_t duty)
{
  const uint64_t counts = (uint64_t)period + 1U;
  return (uint32_t)((counts * duty) / K_FW_PWM_DUTY_FULL);
}

/**
 * @brief Push the stored duty of @p ch to the compare registers.
 *
 * @details Always through the GTCCRC buffer, so the next cycle-end reload
 *          cannot bring back a stale value; also straight into GTCCRA when
 *          stopped, so the first cycle after start is right.
 *
 * @param[in] ch Open output.
 * @return ::k_ra8_ok or what the driver reported.
 */
static ra8_err_t internal_apply_duty(fw_pwm_ch_t ch)
{
  const internal_out_t* out     = &s_out[ch.index];
  const uint32_t        compare = internal_compare(out->period, out->duty);
  const ra8_err_t       err     = ra8_gpt_duty_cycle_set(ch.index, k_ra8_gpt_pin_a, compare);
  if ((err != k_ra8_ok) || out->running) {
    return err;
  }
  return ra8_gpt_set_duty(ch.index, k_ra8_gpt_ccr_a, compare);
}

/**
 * @brief Report ten pin-A outputs on 32-bit counters, both polarities.
 *
 * @param[in]  ctx Unused.
 * @param[out] out Caps on success.
 * @return ::k_ra8_ok, or ::k_ra8_err_invalid_arg for a NULL @p out.
 */
static ra8_err_t internal_get_caps(void* ctx, fw_pwm_caps_t* out)
{
  (void)ctx;
  if (out == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  *out = (fw_pwm_caps_t){
    .channel_count  = (uint8_t)k_fw_pwm_ra8_channel_count,
    .counter_bits   = (uint8_t)k_fw_pwm_ra8_counter_bits,
    .period_max     = (uint32_t)k_fw_pwm_ra8_period_max,
    .has_active_low = true,
  };
  return k_ra8_ok;
}

/**
 * @brief Program the GTIOR fields for pin A: pattern, inactive stop level, on.
 *
 * @param[in] ch  Output.
 * @param[in] pol Requested polarity.
 * @return ::k_ra8_ok or what the driver reported.
 */
static ra8_err_t internal_configure_pin(fw_pwm_ch_t ch, fw_pwm_polarity_t pol)
{
  const bool                  low = (pol == k_fw_pwm_pol_active_low);
  const ra8_gpt_pwm_pin_cfg_t pin = {
    .output_enable    = true,
    .polarity         = low ? k_ra8_gpt_pol_active_low : k_ra8_gpt_pol_active_high,
    .stop_level       = low ? k_ra8_gpt_stop_high : k_ra8_gpt_stop_low,
    .disable_on_fault = k_ra8_gpt_disable_none,
  };
  return ra8_gpt_pwm_pin_configure(ch.index, k_ra8_gpt_pin_a, &pin);
}

/**
 * @brief Claim, configure saw-wave PWM at @p period, duty zero, stopped.
 *
 * @param[in] ctx    Unused.
 * @param[in] ch     Output.
 * @param[in] period Wrap point.
 * @param[in] pol    Polarity, already validated by the facade.
 * @return ::k_ra8_ok, ::k_ra8_err_busy when either adapter holds the
 *         channel, or what the driver reported (the claim is undone).
 */
static ra8_err_t internal_open(void* ctx, fw_pwm_ch_t ch, uint32_t period, fw_pwm_polarity_t pol)
{
  (void)ctx;
  const ra8_err_t claim = fw_gpt_ra8_claim(ch.index, k_fw_gpt_ra8_owner_pwm);
  if (claim != k_ra8_ok) {
    return claim;
  }
  const ra8_gpt_cfg_t cfg = {
    .mode       = k_ra8_gpt_mode_saw_pwm,
    .prescaler  = k_ra8_gpt_ps_div_1,
    .period     = period,
    .duty_a     = 0U,
    .duty_b     = 0U,
    .auto_start = false,
  };
  s_out[ch.index] = (internal_out_t){.period = period, .duty = 0U, .running = false};
  ra8_err_t err   = ra8_gpt_init(ch.index, &cfg);
  if (err == k_ra8_ok) {
    err = internal_configure_pin(ch, pol);
  }
  if (err == k_ra8_ok) {
    err = internal_apply_duty(ch);
  }
  if (err != k_ra8_ok) {
    (void)ra8_gpt_deinit(ch.index);
    fw_gpt_ra8_release(ch.index, k_fw_gpt_ra8_owner_pwm);
  }
  return err;
}

/**
 * @brief Stop, release the module-stop reference and the claim.
 *
 * @param[in] ctx Unused.
 * @param[in] ch  Output.
 * @return ::k_ra8_ok, ::k_ra8_err_invalid_state when not open here, or
 *         what `ra8_gpt_deinit` reported.
 */
static ra8_err_t internal_close(void* ctx, fw_pwm_ch_t ch)
{
  (void)ctx;
  if (!internal_is_open(ch)) {
    return k_ra8_err_invalid_state;
  }
  fw_gpt_ra8_release(ch.index, k_fw_gpt_ra8_owner_pwm);
  s_out[ch.index].running = false;
  return ra8_gpt_deinit(ch.index);
}

/**
 * @brief Begin driving.
 *
 * @param[in] ctx Unused.
 * @param[in] ch  Output.
 * @return ::k_ra8_ok, ::k_ra8_err_invalid_state when not open here, or
 *         what `ra8_gpt_start` reported.
 */
static ra8_err_t internal_start(void* ctx, fw_pwm_ch_t ch)
{
  (void)ctx;
  if (!internal_is_open(ch)) {
    return k_ra8_err_invalid_state;
  }
  const ra8_err_t err = ra8_gpt_start(ch.index);
  if (err == k_ra8_ok) {
    s_out[ch.index].running = true;
  }
  return err;
}

/**
 * @brief Stop counting; the pin falls to its inactive stop level.
 *
 * @param[in] ctx Unused.
 * @param[in] ch  Output.
 * @return ::k_ra8_ok, ::k_ra8_err_invalid_state when not open here, or
 *         what `ra8_gpt_stop` reported.
 */
static ra8_err_t internal_stop(void* ctx, fw_pwm_ch_t ch)
{
  (void)ctx;
  if (!internal_is_open(ch)) {
    return k_ra8_err_invalid_state;
  }
  const ra8_err_t err = ra8_gpt_stop(ch.index);
  if (err == k_ra8_ok) {
    s_out[ch.index].running = false;
  }
  return err;
}

/**
 * @brief New period, same duty ratio.
 *
 * @param[in] ctx    Unused.
 * @param[in] ch     Output.
 * @param[in] period New wrap point.
 * @return ::k_ra8_ok, ::k_ra8_err_invalid_state when not open here, or
 *         what the driver reported.
 */
static ra8_err_t internal_set_period(void* ctx, fw_pwm_ch_t ch, uint32_t period)
{
  (void)ctx;
  if (!internal_is_open(ch)) {
    return k_ra8_err_invalid_state;
  }
  const ra8_err_t err = ra8_gpt_period_set(ch.index, period);
  if (err != k_ra8_ok) {
    return err;
  }
  s_out[ch.index].period = period;
  return internal_apply_duty(ch);
}

/**
 * @brief New duty ratio, same period.
 *
 * @param[in] ctx  Unused.
 * @param[in] ch   Output.
 * @param[in] duty Q16 ratio, already range-checked.
 * @return ::k_ra8_ok, ::k_ra8_err_invalid_state when not open here, or
 *         what the driver reported.
 */
static ra8_err_t internal_set_duty(void* ctx, fw_pwm_ch_t ch, fw_pwm_duty_t duty)
{
  (void)ctx;
  if (!internal_is_open(ch)) {
    return k_ra8_err_invalid_state;
  }
  s_out[ch.index].duty = duty;
  return internal_apply_duty(ch);
}

static const fw_pwm_iface_t k_internal_iface = {
  .get_caps   = internal_get_caps,
  .open       = internal_open,
  .close      = internal_close,
  .start      = internal_start,
  .stop       = internal_stop,
  .set_period = internal_set_period,
  .set_duty   = internal_set_duty,
};

const fw_pwm_iface_t* fw_pwm_ra8_iface(void)
{
  return &k_internal_iface;
}

ra8_err_t fw_pwm_ra8_bind(fw_pwm_t* pwm)
{
  return fw_pwm_bind(pwm, &k_internal_iface, nullptr);
}
