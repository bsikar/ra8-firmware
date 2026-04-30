/**
 * @file ra_tml.c
 * @brief TML driver -- placeholder implementation
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_tml.h"

#include <stdint.h>

#include "ra_check.h"
#include "ra_err.h"
#include "ra_log.h"

static const char* s_tag = "TML";

typedef struct {
  bool     opened;
  bool     running;
  uint32_t period;
  bool     one_shot;
} ra_tml_state_t;

static ra_tml_state_t s_state = {};

ra_err_t ra_tml_open(const ra_tml_cfg_t* cfg)
{
  RA_CHECK_NULL_PTR(cfg, s_tag, "cfg must not be nullptr");
  if (cfg->period == 0U) {
    return k_ra_err_invalid_arg;
  }
  if (s_state.opened) {
    return k_ra_err_exists;
  }
  s_state.opened   = true;
  s_state.running  = false;
  s_state.period   = cfg->period;
  s_state.one_shot = (cfg->one_shot != 0U);
  ra_log_info_val(s_tag, "tml open ch", (uint32_t)cfg->channel);
  return k_ra_ok;
}

ra_err_t ra_tml_start(void)
{
  if (!s_state.opened) {
    return k_ra_err_not_initialized;
  }
  s_state.running = true;
  return k_ra_ok;
}

ra_err_t ra_tml_stop(void)
{
  if (!s_state.opened) {
    return k_ra_err_not_initialized;
  }
  s_state.running = false;
  return k_ra_ok;
}

ra_err_t ra_tml_count_get(uint32_t* out)
{
  RA_CHECK_NULL_PTR(out, s_tag, "out must not be nullptr");
  if (!s_state.opened) {
    return k_ra_err_not_initialized;
  }
  *out = 0U;
  return k_ra_ok;
}

ra_err_t ra_tml_status_get(uint8_t* out_open, uint8_t* out_running)
{
  RA_CHECK_NULL_PTR(out_open, s_tag, "out_open must not be nullptr");
  RA_CHECK_NULL_PTR(out_running, s_tag, "out_running must not be nullptr");
  *out_open    = (uint8_t)(s_state.opened ? 1U : 0U);
  *out_running = (uint8_t)(s_state.running ? 1U : 0U);
  return k_ra_ok;
}

ra_err_t ra_tml_close(void)
{
  if (!s_state.opened) {
    return k_ra_err_invalid_state;
  }
  s_state.opened   = false;
  s_state.running  = false;
  s_state.period   = 0U;
  s_state.one_shot = false;
  return k_ra_ok;
}
