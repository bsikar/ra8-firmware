/**
 * @file fw_if_timer_ra8.c
 * @brief The nine `fw_if_timer` ops, over `ra8_gpt`.
 * @ingroup grp_fw_timer
 *
 * @par Tag
 * [Ring 3 / HAL] {World: S}
 *
 * @details
 * The port facade has already refused a NULL output, an unbound handle, a
 * channel at or past ::k_fw_timer_ra8_channel_count, a period above the
 * counter maximum and a mode the caps rule out. These functions check only
 * what the adapter itself owns: whether a channel is open here.
 *
 * Open configures and leaves the counter stopped; start and stop only flip
 * the channel's own start bit, so a stopped channel resumes where it was.
 * A period change goes through `ra8_gpt_period_set`, which buffers the new
 * value while counting and writes it live while stopped.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include "fw_if_timer_ra8.h"

#include <stdbool.h>
#include <stdint.h>

#include "fw_if_gpt_ra8_claim.h"
#include "fw_if_timer.h"
#include "ra8_err.h"
#include "ra8_gpt.h"

/**
 * @brief Whether @p ch is open here.
 *
 * @param[in] ch Channel.
 * @return True when opened and not yet closed.
 */
static bool internal_is_open(fw_timer_ch_t ch)
{
  return fw_gpt_ra8_owned_by(ch.index, k_fw_gpt_ra8_owner_timer);
}

/**
 * @brief Report the ten GPT32 channels.
 *
 * @param[in]  ctx Unused; the GPT block is a chip singleton.
 * @param[out] out Caps on success.
 * @return ::k_ra8_ok, or ::k_ra8_err_invalid_arg for a NULL @p out.
 */
static ra8_err_t internal_get_caps(void* ctx, fw_timer_caps_t* out)
{
  (void)ctx;
  if (out == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  *out = (fw_timer_caps_t){
    .channel_count = (uint8_t)k_fw_timer_ra8_channel_count,
    .counter_bits  = (uint8_t)k_fw_timer_ra8_counter_bits,
    .counter_max   = (uint32_t)k_fw_timer_ra8_counter_max,
    .has_capture   = false,
    .has_one_shot  = true,
  };
  return k_ra8_ok;
}

/**
 * @brief Configure a channel for @p mode with wrap point @p period, stopped.
 *
 * @param[in] ctx    Unused.
 * @param[in] ch     Chip channel.
 * @param[in] mode   Free-run or one-shot; capture is refused.
 * @param[in] period Written to GTPR.
 * @return ::k_ra8_ok, ::k_ra8_err_busy when either adapter holds it,
 *         ::k_ra8_err_not_supported for capture or an unknown mode, or
 *         whatever `ra8_gpt_init` reported.
 */
static ra8_err_t internal_open(void* ctx, fw_timer_ch_t ch, fw_timer_mode_t mode, uint32_t period)
{
  (void)ctx;
  ra8_gpt_cfg_t cfg = {
    .mode       = k_ra8_gpt_mode_saw_pwm,
    .prescaler  = k_ra8_gpt_ps_div_1,
    .period     = period,
    .duty_a     = 0U,
    .duty_b     = 0U,
    .auto_start = false,
  };
  if (mode == k_fw_timer_mode_one_shot) {
    cfg.mode = k_ra8_gpt_mode_saw_one_shot;
  } else if (mode != k_fw_timer_mode_free_run) {
    return k_ra8_err_not_supported;
  }

  const ra8_err_t claim = fw_gpt_ra8_claim(ch.index, k_fw_gpt_ra8_owner_timer);
  if (claim != k_ra8_ok) {
    return claim;
  }
  const ra8_err_t err = ra8_gpt_init(ch.index, &cfg);
  if (err != k_ra8_ok) {
    fw_gpt_ra8_release(ch.index, k_fw_gpt_ra8_owner_timer);
  }
  return err;
}

/**
 * @brief Stop a channel, release its module-stop reference, forget it.
 *
 * @param[in] ctx Unused.
 * @param[in] ch  Chip channel.
 * @return ::k_ra8_ok, ::k_ra8_err_invalid_state when not open, or whatever
 *         `ra8_gpt_deinit` reported.
 */
static ra8_err_t internal_close(void* ctx, fw_timer_ch_t ch)
{
  (void)ctx;
  if (!internal_is_open(ch)) {
    return k_ra8_err_invalid_state;
  }
  fw_gpt_ra8_release(ch.index, k_fw_gpt_ra8_owner_timer);
  return ra8_gpt_deinit(ch.index);
}

/**
 * @brief Begin counting on an open channel.
 *
 * @param[in] ctx Unused.
 * @param[in] ch  Chip channel.
 * @return ::k_ra8_ok, ::k_ra8_err_invalid_state when not open, or whatever
 *         `ra8_gpt_start` reported.
 */
static ra8_err_t internal_start(void* ctx, fw_timer_ch_t ch)
{
  (void)ctx;
  if (!internal_is_open(ch)) {
    return k_ra8_err_invalid_state;
  }
  return ra8_gpt_start(ch.index);
}

/**
 * @brief Halt counting, keeping the channel open.
 *
 * @param[in] ctx Unused.
 * @param[in] ch  Chip channel.
 * @return ::k_ra8_ok, ::k_ra8_err_invalid_state when not open, or whatever
 *         `ra8_gpt_stop` reported.
 */
static ra8_err_t internal_stop(void* ctx, fw_timer_ch_t ch)
{
  (void)ctx;
  if (!internal_is_open(ch)) {
    return k_ra8_err_invalid_state;
  }
  return ra8_gpt_stop(ch.index);
}

/**
 * @brief Read the live count.
 *
 * @param[in]  ctx        Unused.
 * @param[in]  ch         Chip channel.
 * @param[out] out_counts GTCNT on success.
 * @return ::k_ra8_ok, ::k_ra8_err_invalid_state when not open, or whatever
 *         `ra8_gpt_read` reported.
 */
static ra8_err_t internal_read(void* ctx, fw_timer_ch_t ch, uint32_t* out_counts)
{
  (void)ctx;
  if (!internal_is_open(ch)) {
    return k_ra8_err_invalid_state;
  }
  return ra8_gpt_read(ch.index, out_counts);
}

/**
 * @brief Change the wrap point of an open channel.
 *
 * @param[in] ctx    Unused.
 * @param[in] ch     Chip channel.
 * @param[in] period New GTPR value; buffered while counting.
 * @return ::k_ra8_ok, ::k_ra8_err_invalid_state when not open, or whatever
 *         `ra8_gpt_period_set` reported.
 */
static ra8_err_t internal_set_period(void* ctx, fw_timer_ch_t ch, uint32_t period)
{
  (void)ctx;
  if (!internal_is_open(ch)) {
    return k_ra8_err_invalid_state;
  }
  return ra8_gpt_period_set(ch.index, period);
}

/**
 * @brief Capture is not offered; see the header.
 *
 * @param[in]  ctx        Unused.
 * @param[in]  ch         Unused.
 * @param[out] out_counts Unused.
 * @return ::k_ra8_err_not_supported always.
 */
static ra8_err_t internal_capture_read(void* ctx, fw_timer_ch_t ch, uint32_t* out_counts)
{
  (void)ctx;
  (void)ch;
  (void)out_counts;
  return k_ra8_err_not_supported;
}

/**
 * @brief Report and clear a pending wrap, read from GTST.TCFPO.
 *
 * @details The overflow flag sets when the count reaches GTPR, in saw-wave
 *          free-run and at the end of a one-shot alike, and stays set until
 *          written clear, which is the sticky report the port promises.
 *
 * @param[in]  ctx         Unused.
 * @param[in]  ch          Chip channel.
 * @param[out] out_wrapped True when TCFPO was set.
 * @return ::k_ra8_ok, ::k_ra8_err_invalid_state when not open, or whatever
 *         `ra8_gpt_get_status` / `ra8_gpt_clear_status` reported.
 */
static ra8_err_t internal_take_wrap(void* ctx, fw_timer_ch_t ch, bool* out_wrapped)
{
  (void)ctx;
  if (!internal_is_open(ch)) {
    return k_ra8_err_invalid_state;
  }
  uint32_t        status = 0U;
  const ra8_err_t err    = ra8_gpt_get_status(ch.index, &status);
  if (err != k_ra8_ok) {
    return err;
  }
  *out_wrapped = (status & (uint32_t)k_ra8_gpt_status_overflow) != 0U;
  if (!*out_wrapped) {
    return k_ra8_ok;
  }
  return ra8_gpt_clear_status(ch.index, (uint32_t)k_ra8_gpt_status_overflow);
}

static const fw_timer_iface_t k_internal_iface = {
  .get_caps     = internal_get_caps,
  .open         = internal_open,
  .close        = internal_close,
  .start        = internal_start,
  .stop         = internal_stop,
  .read         = internal_read,
  .set_period   = internal_set_period,
  .capture_read = internal_capture_read,
  .take_wrap    = internal_take_wrap,
};

const fw_timer_iface_t* fw_timer_ra8_iface(void)
{
  return &k_internal_iface;
}

ra8_err_t fw_timer_ra8_bind(fw_timer_t* tmr)
{
  return fw_timer_bind(tmr, &k_internal_iface, nullptr);
}
