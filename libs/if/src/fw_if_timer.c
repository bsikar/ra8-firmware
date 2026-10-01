/**
 * @file fw_if_timer.c
 * @brief Facade half of the neutral timer port: guards, range checks, pass-through.
 * @ingroup grp_fw_timer
 *
 * @par Tag
 * [Ring 2 / Interface] {World: Any}
 *
 * @details
 * Implements `fw_if_timer.h`. Every function here is a guard and a forward:
 * the port owns no state beyond the caller's handle and reaches no register.
 *
 * The checks are the value. A timer backend handed a period wider than its
 * counter does not fail, it truncates, and the symptom is an interval that is
 * wrong by a factor nobody notices until something downstream is out of spec.
 * Catching that at the seam turns a silent wrong answer into
 * ::k_ra8_err_out_of_range with the caller's own line number on it.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include "fw_if_timer.h"

#include <stdbool.h>
#include <stdint.h>

#include "ra8_err.h"

/**
 * @brief Whether every op in a candidate binding is set.
 *
 * @details
 * All nine, checked individually, so adding a tenth op cannot be forgotten
 * here: a NULL op is a malformed binding, and a backend declining a capability
 * says so from the op itself plus its caps.
 */
static bool internal_iface_complete(const fw_timer_iface_t *iface)
{
  return (iface->get_caps != nullptr) && (iface->open != nullptr) && (iface->close != nullptr) &&
         (iface->start != nullptr) && (iface->stop != nullptr) && (iface->read != nullptr) &&
         (iface->set_period != nullptr) && (iface->capture_read != nullptr) &&
         (iface->take_wrap != nullptr);
}

/** @brief Common entry guard: non-NULL handle, bound. */
static ra8_err_t internal_check(const fw_timer_t *tmr)
{
  if (tmr == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  if (!tmr->bound) {
    return k_ra8_err_not_initialized;
  }
  return k_ra8_ok;
}

/**
 * @brief Whether the board carries @p ch, judged from the bound caps snapshot.
 *
 * @details A board with no timers at all reports a zero count, and then every
 *          index is absent rather than index zero being special.
 */
static ra8_err_t internal_channel_present(const fw_timer_t *tmr, fw_timer_ch_t ch)
{
  if (ch.index >= tmr->caps.channel_count) {
    return k_ra8_err_not_found;
  }
  return k_ra8_ok;
}

/** @brief Whether a period is usable on this backend's counter. */
static ra8_err_t internal_period_ok(const fw_timer_t *tmr, uint32_t period)
{
  if (period == 0U) {
    return k_ra8_err_invalid_arg;
  }
  if (period > tmr->caps.counter_max) {
    return k_ra8_err_out_of_range;
  }
  return k_ra8_ok;
}

/** @brief Whether this backend offers @p mode at all. */
static ra8_err_t internal_mode_ok(const fw_timer_t *tmr, fw_timer_mode_t mode)
{
  if ((mode == k_fw_timer_mode_none) ||
      ((uint8_t)mode >= (uint8_t)K_FW_TIMER_MODE_COUNT)) {
    return k_ra8_err_invalid_arg;
  }
  if ((mode == k_fw_timer_mode_capture) && !tmr->caps.has_capture) {
    return k_ra8_err_not_supported;
  }
  if ((mode == k_fw_timer_mode_one_shot) && !tmr->caps.has_one_shot) {
    return k_ra8_err_not_supported;
  }
  return k_ra8_ok;
}

/** @brief Guard shared by the plain per-channel operations. */
static ra8_err_t internal_check_channel(const fw_timer_t *tmr, fw_timer_ch_t ch)
{
  const ra8_err_t guard = internal_check(tmr);
  if (guard != k_ra8_ok) {
    return guard;
  }
  return internal_channel_present(tmr, ch);
}

ra8_err_t fw_timer_bind(fw_timer_t *tmr, const fw_timer_iface_t *iface, void *ctx)
{
  if ((tmr == nullptr) || (iface == nullptr)) {
    return k_ra8_err_invalid_arg;
  }
  if (!internal_iface_complete(iface)) {
    return k_ra8_err_invalid_arg;
  }

  fw_timer_caps_t caps = {};
  const ra8_err_t err  = iface->get_caps(ctx, &caps);
  if (err != k_ra8_ok) {
    return err;
  }
  /* A zero counter width or limit makes every later period check vacuous.
   * Refusing here names the buggy binding instead of leaving a nonsense
   * limit for whichever driver binds next. */
  if ((caps.counter_bits == 0U) || (caps.counter_max == 0U)) {
    return k_ra8_err_invalid_state;
  }

  tmr->iface = iface;
  tmr->ctx   = ctx;
  tmr->caps  = caps;
  tmr->bound = true;
  return k_ra8_ok;
}

ra8_err_t fw_timer_get_caps(const fw_timer_t *tmr, fw_timer_caps_t *out)
{
  if (out == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  const fw_timer_caps_t zeroed = {};
  *out                         = zeroed;

  const ra8_err_t guard = internal_check(tmr);
  if (guard != k_ra8_ok) {
    return guard;
  }
  *out = tmr->caps;
  return k_ra8_ok;
}

ra8_err_t fw_timer_open(const fw_timer_t *tmr, fw_timer_ch_t ch, fw_timer_mode_t mode,
                        uint32_t period)
{
  const ra8_err_t guard = internal_check_channel(tmr, ch);
  if (guard != k_ra8_ok) {
    return guard;
  }
  const ra8_err_t mode_err = internal_mode_ok(tmr, mode);
  if (mode_err != k_ra8_ok) {
    return mode_err;
  }
  const ra8_err_t period_err = internal_period_ok(tmr, period);
  if (period_err != k_ra8_ok) {
    return period_err;
  }
  return tmr->iface->open(tmr->ctx, ch, mode, period);
}

ra8_err_t fw_timer_close(const fw_timer_t *tmr, fw_timer_ch_t ch)
{
  const ra8_err_t guard = internal_check_channel(tmr, ch);
  if (guard != k_ra8_ok) {
    return guard;
  }
  return tmr->iface->close(tmr->ctx, ch);
}

ra8_err_t fw_timer_start(const fw_timer_t *tmr, fw_timer_ch_t ch)
{
  const ra8_err_t guard = internal_check_channel(tmr, ch);
  if (guard != k_ra8_ok) {
    return guard;
  }
  return tmr->iface->start(tmr->ctx, ch);
}

ra8_err_t fw_timer_stop(const fw_timer_t *tmr, fw_timer_ch_t ch)
{
  const ra8_err_t guard = internal_check_channel(tmr, ch);
  if (guard != k_ra8_ok) {
    return guard;
  }
  return tmr->iface->stop(tmr->ctx, ch);
}

ra8_err_t fw_timer_read(const fw_timer_t *tmr, fw_timer_ch_t ch, uint32_t *out_counts)
{
  if (out_counts == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  *out_counts = 0U;

  const ra8_err_t guard = internal_check_channel(tmr, ch);
  if (guard != k_ra8_ok) {
    return guard;
  }

  uint32_t        counts = 0U;
  const ra8_err_t err    = tmr->iface->read(tmr->ctx, ch, &counts);
  if (err != k_ra8_ok) {
    return err;
  }
  *out_counts = counts;
  return k_ra8_ok;
}

ra8_err_t fw_timer_set_period(const fw_timer_t *tmr, fw_timer_ch_t ch, uint32_t period)
{
  const ra8_err_t guard = internal_check_channel(tmr, ch);
  if (guard != k_ra8_ok) {
    return guard;
  }
  const ra8_err_t period_err = internal_period_ok(tmr, period);
  if (period_err != k_ra8_ok) {
    return period_err;
  }
  return tmr->iface->set_period(tmr->ctx, ch, period);
}

ra8_err_t fw_timer_capture_read(const fw_timer_t *tmr, fw_timer_ch_t ch, uint32_t *out_counts)
{
  if (out_counts == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  *out_counts = 0U;

  const ra8_err_t guard = internal_check_channel(tmr, ch);
  if (guard != k_ra8_ok) {
    return guard;
  }
  /* Declared absent is answered here, so a caller never has to tell "this
   * backend cannot" apart from "no edge has arrived yet". */
  if (!tmr->caps.has_capture) {
    return k_ra8_err_not_supported;
  }

  uint32_t        counts = 0U;
  const ra8_err_t err    = tmr->iface->capture_read(tmr->ctx, ch, &counts);
  if (err != k_ra8_ok) {
    return err;
  }
  *out_counts = counts;
  return k_ra8_ok;
}

ra8_err_t fw_timer_take_wrap(const fw_timer_t* tmr, fw_timer_ch_t ch, bool* out_wrapped)
{
  if (out_wrapped == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  *out_wrapped = false;

  const ra8_err_t guard = internal_check_channel(tmr, ch);
  if (guard != k_ra8_ok) {
    return guard;
  }

  bool            wrapped = false;
  const ra8_err_t err     = tmr->iface->take_wrap(tmr->ctx, ch, &wrapped);
  if (err != k_ra8_ok) {
    return err;
  }
  *out_wrapped = wrapped;
  return k_ra8_ok;
}
