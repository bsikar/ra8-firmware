/**
 * @file ra_dac8.c
 * @brief 8-bit D/A Converter (DAC8) driver implementation
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Software-only placeholder for the FSP ``r_dac8`` peripheral
 * driver. RA8D2 carries only the 12-bit DAC_B; this file exposes the
 * FSP-shaped 8-bit surface so example projects can build cleanly.
 *
 * @warning RA8D2 silicon may not include this peripheral; verify
 *          against the BSP feature header before flashing.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_dac8.h"

#include <stdint.h>

#include "ra_check.h"
#include "ra_err.h"
#include "ra_log.h"

static const char* s_tag = "DAC8";

/**
 * @enum ra_dac8_limits_t
 * @brief Channel-count and status-bit constants.
 */
typedef enum : uint8_t {
  k_ra_dac8_channel_count = 2U,
  k_ra_dac8_status_open   = 0x1U,
  k_ra_dac8_status_run    = 0x2U,
  k_ra_dac8_status_pump   = 0x4U,
  k_ra_dac8_status_amp    = 0x8U,
} ra_dac8_limits_t;

/**
 * @struct ra_dac8_state_t
 * @brief Per-channel placeholder state.
 */
typedef struct {
  bool                open;
  bool                running;
  uint8_t             value;
  ra_dac8_oper_mode_t mode;
  bool                charge_pump;
  bool                output_amp;
} ra_dac8_state_t;

static ra_dac8_state_t s_channels[k_ra_dac8_channel_count];

ra_err_t ra_dac8_open(const ra_dac8_cfg_t* cfg)
{
  RA_CHECK_NULL_PTR((void*)cfg, s_tag, "cfg must not be nullptr");
  if ((uint16_t)cfg->channel >= k_ra_dac8_channel_count) {
    return k_ra_err_invalid_arg;
  }
  ra_dac8_state_t* st = &s_channels[cfg->channel];
  st->open            = true;
  st->running         = false;
  st->value           = 0U;
  st->mode            = cfg->mode;
  st->charge_pump     = cfg->charge_pump;
  st->output_amp      = cfg->output_amp;
  ra_log_info_val(s_tag, "open ch", (uint32_t)cfg->channel);
  return k_ra_ok;
}

ra_err_t ra_dac8_close(uint8_t channel)
{
  if ((uint16_t)channel >= k_ra_dac8_channel_count) {
    return k_ra_err_invalid_arg;
  }
  s_channels[channel].open    = false;
  s_channels[channel].running = false;
  s_channels[channel].value   = 0U;
  return k_ra_ok;
}

ra_err_t ra_dac8_write(uint8_t channel, uint8_t value)
{
  if ((uint16_t)channel >= k_ra_dac8_channel_count) {
    return k_ra_err_invalid_arg;
  }
  s_channels[channel].value = value;
  return k_ra_ok;
}

ra_err_t ra_dac8_start(uint8_t channel)
{
  if ((uint16_t)channel >= k_ra_dac8_channel_count) {
    return k_ra_err_invalid_arg;
  }
  if (!s_channels[channel].open) {
    return k_ra_err_invalid_state;
  }
  s_channels[channel].running = true;
  return k_ra_ok;
}

ra_err_t ra_dac8_stop(uint8_t channel)
{
  if ((uint16_t)channel >= k_ra_dac8_channel_count) {
    return k_ra_err_invalid_arg;
  }
  s_channels[channel].running = false;
  return k_ra_ok;
}

ra_err_t ra_dac8_get_status(uint8_t channel, uint8_t* out_mask)
{
  RA_CHECK_NULL_PTR(out_mask, s_tag, "out_mask must not be nullptr");
  if ((uint16_t)channel >= k_ra_dac8_channel_count) {
    return k_ra_err_invalid_arg;
  }
  const ra_dac8_state_t* st   = &s_channels[channel];
  uint8_t                mask = 0U;
  if (st->open) {
    mask |= k_ra_dac8_status_open;
  }
  if (st->running) {
    mask |= k_ra_dac8_status_run;
  }
  if (st->charge_pump) {
    mask |= k_ra_dac8_status_pump;
  }
  if (st->output_amp) {
    mask |= k_ra_dac8_status_amp;
  }
  *out_mask = mask;
  return k_ra_ok;
}
