/**
 * @file ra_tau.c
 * @brief TAU driver -- placeholder implementation
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Host-testable stand-in for FSP `r_tau`. Per-channel lifecycle
 * lives in a static array of `ra_tau_chan_state_t`. The 16-bit
 * period values and prescaler digits are exercised by the unit
 * tests through the placeholder accessors only -- there is no
 * real silicon to drive on RA8D2.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

/* NOLINTBEGIN(readability-magic-numbers,readability-function-size,readability-function-cognitive-complexity) */

#include "ra_tau.h"

#include <stdint.h>

#include "ra_check.h"
#include "ra_err.h"
#include "ra_log.h"

static const char* s_tag = "TAU";

typedef struct {
  bool     opened;
  bool     running;
  uint16_t period;
} ra_tau_chan_state_t;

static ra_tau_chan_state_t s_chans[k_ra_tau_channel_count] = {};

ra_err_t ra_tau_open(const ra_tau_cfg_t* cfg)
{
  RA_CHECK_NULL_PTR(cfg, s_tag, "cfg must not be nullptr");
  if (cfg->channel >= k_ra_tau_channel_count) {
    return k_ra_err_invalid_arg;
  }
  if (cfg->period == 0U) {
    return k_ra_err_invalid_arg;
  }
  if (s_chans[cfg->channel].opened) {
    return k_ra_err_exists;
  }
  s_chans[cfg->channel].opened  = true;
  s_chans[cfg->channel].running = false;
  s_chans[cfg->channel].period  = cfg->period;
  ra_log_info_val(s_tag, "tau open ch", (uint32_t)cfg->channel);
  return k_ra_ok;
}

ra_err_t ra_tau_start(ra_tau_channel_t channel)
{
  if (channel >= k_ra_tau_channel_count) {
    return k_ra_err_invalid_arg;
  }
  if (!s_chans[channel].opened) {
    return k_ra_err_not_initialized;
  }
  s_chans[channel].running = true;
  return k_ra_ok;
}

ra_err_t ra_tau_stop(ra_tau_channel_t channel)
{
  if (channel >= k_ra_tau_channel_count) {
    return k_ra_err_invalid_arg;
  }
  if (!s_chans[channel].opened) {
    return k_ra_err_not_initialized;
  }
  s_chans[channel].running = false;
  return k_ra_ok;
}

ra_err_t ra_tau_count_get(ra_tau_channel_t channel, uint16_t* out)
{
  RA_CHECK_NULL_PTR(out, s_tag, "out must not be nullptr");
  if (channel >= k_ra_tau_channel_count) {
    return k_ra_err_invalid_arg;
  }
  if (!s_chans[channel].opened) {
    return k_ra_err_not_initialized;
  }
  *out = 0U;
  return k_ra_ok;
}

ra_err_t ra_tau_status_get(ra_tau_channel_t channel, uint8_t* out_open, uint8_t* out_running)
{
  RA_CHECK_NULL_PTR(out_open, s_tag, "out_open must not be nullptr");
  RA_CHECK_NULL_PTR(out_running, s_tag, "out_running must not be nullptr");
  if (channel >= k_ra_tau_channel_count) {
    return k_ra_err_invalid_arg;
  }
  *out_open    = (uint8_t)(s_chans[channel].opened ? 1U : 0U);
  *out_running = (uint8_t)(s_chans[channel].running ? 1U : 0U);
  return k_ra_ok;
}

ra_err_t ra_tau_close(ra_tau_channel_t channel)
{
  if (channel >= k_ra_tau_channel_count) {
    return k_ra_err_invalid_arg;
  }
  if (!s_chans[channel].opened) {
    return k_ra_err_invalid_state;
  }
  s_chans[channel].opened  = false;
  s_chans[channel].running = false;
  s_chans[channel].period  = 0U;
  return k_ra_ok;
}

/* NOLINTEND(readability-magic-numbers,readability-function-size,readability-function-cognitive-complexity) */
