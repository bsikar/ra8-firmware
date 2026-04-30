/**
 * @file ra_dac.c
 * @brief Legacy 12-bit D/A Converter (DAC12) driver implementation
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Software-only placeholder for the FSP ``r_dac`` peripheral driver.
 * RA8D2 carries DAC_B (see ``ra_dac_b.c``); this file exposes the
 * FSP-shaped legacy surface so example projects can build unchanged.
 *
 * @warning RA8D2 silicon may not include this peripheral; verify
 *          against the BSP feature header before flashing.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_dac.h"

#include <stdint.h>

#include "ra_check.h"
#include "ra_err.h"
#include "ra_log.h"

static const char* s_tag = "DAC";

/**
 * @enum ra_dac_limits_t
 * @brief Channel-count, max-code, and status-bit constants.
 */
typedef enum : uint16_t {
  k_ra_dac_channel_count = 2U,
  k_ra_dac_max_code      = 4095U,
  k_ra_dac_status_open   = 0x1U,
  k_ra_dac_status_run    = 0x2U,
  k_ra_dac_status_amp    = 0x4U,
} ra_dac_limits_t;

/**
 * @struct ra_dac_state_t
 * @brief Per-channel placeholder state.
 */
typedef struct {
  bool                 open;
  bool                 running;
  uint16_t             value;
  ra_dac_data_format_t data_format;
  bool                 ad_da_sync;
  bool                 output_amp;
} ra_dac_state_t;

static ra_dac_state_t s_channels[k_ra_dac_channel_count];

ra_err_t ra_dac_open(const ra_dac_cfg_t* cfg)
{
  RA_CHECK_NULL_PTR((void*)cfg, s_tag, "cfg must not be nullptr");
  if ((uint16_t)cfg->channel >= k_ra_dac_channel_count) {
    return k_ra_err_invalid_arg;
  }
  ra_dac_state_t* st = &s_channels[cfg->channel];
  st->open           = true;
  st->running        = false;
  st->value          = 0U;
  st->data_format    = cfg->data_format;
  st->ad_da_sync     = cfg->ad_da_sync;
  st->output_amp     = cfg->output_amp;
  ra_log_info_val(s_tag, "open ch", (uint32_t)cfg->channel);
  return k_ra_ok;
}

ra_err_t ra_dac_close(uint8_t channel)
{
  if ((uint16_t)channel >= k_ra_dac_channel_count) {
    return k_ra_err_invalid_arg;
  }
  s_channels[channel].open    = false;
  s_channels[channel].running = false;
  s_channels[channel].value   = 0U;
  return k_ra_ok;
}

ra_err_t ra_dac_write(uint8_t channel, uint16_t value)
{
  if ((uint16_t)channel >= k_ra_dac_channel_count) {
    return k_ra_err_invalid_arg;
  }
  uint16_t clamped = value;
  if (clamped > k_ra_dac_max_code) {
    clamped = k_ra_dac_max_code;
  }
  s_channels[channel].value = clamped;
  return k_ra_ok;
}

ra_err_t ra_dac_start(uint8_t channel)
{
  if ((uint16_t)channel >= k_ra_dac_channel_count) {
    return k_ra_err_invalid_arg;
  }
  if (!s_channels[channel].open) {
    return k_ra_err_invalid_state;
  }
  s_channels[channel].running = true;
  return k_ra_ok;
}

ra_err_t ra_dac_stop(uint8_t channel)
{
  if ((uint16_t)channel >= k_ra_dac_channel_count) {
    return k_ra_err_invalid_arg;
  }
  s_channels[channel].running = false;
  return k_ra_ok;
}

ra_err_t ra_dac_get_status(uint8_t channel, uint8_t* out_mask)
{
  RA_CHECK_NULL_PTR(out_mask, s_tag, "out_mask must not be nullptr");
  if ((uint16_t)channel >= k_ra_dac_channel_count) {
    return k_ra_err_invalid_arg;
  }
  const ra_dac_state_t* st   = &s_channels[channel];
  uint8_t               mask = 0U;
  if (st->open) {
    mask |= (uint8_t)k_ra_dac_status_open;
  }
  if (st->running) {
    mask |= (uint8_t)k_ra_dac_status_run;
  }
  if (st->output_amp) {
    mask |= (uint8_t)k_ra_dac_status_amp;
  }
  *out_mask = mask;
  return k_ra_ok;
}
