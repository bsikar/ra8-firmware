/**
 * @file ra_sau_i2c.c
 * @brief SAU I2C driver -- placeholder implementation
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_sau_i2c.h"

#include <stdint.h>

#include "ra_check.h"
#include "ra_err.h"
#include "ra_log.h"

static const char* s_tag = "SAU_I";

typedef struct {
  bool opened;
  bool busy;
} ra_sau_i2c_state_t;

static ra_sau_i2c_state_t s_state = {};

ra_err_t ra_sau_i2c_open(const ra_sau_i2c_cfg_t* cfg)
{
  RA_CHECK_NULL_PTR(cfg, s_tag, "cfg must not be nullptr");
  if (cfg->rate_hz == 0U) {
    return k_ra_err_invalid_arg;
  }
  if (s_state.opened) {
    return k_ra_err_exists;
  }
  s_state.opened = true;
  s_state.busy   = false;
  ra_log_info_val(s_tag, "sau_i2c open ch", (uint32_t)cfg->channel);
  return k_ra_ok;
}

ra_err_t ra_sau_i2c_write(const uint8_t* src, uint32_t len)
{
  RA_CHECK_NULL_PTR(src, s_tag, "src must not be nullptr");
  if (len == 0U) {
    return k_ra_err_invalid_arg;
  }
  if (!s_state.opened) {
    return k_ra_err_not_initialized;
  }
  return k_ra_err_not_supported;
}

ra_err_t ra_sau_i2c_read(uint8_t* dst, uint32_t len)
{
  RA_CHECK_NULL_PTR(dst, s_tag, "dst must not be nullptr");
  if (len == 0U) {
    return k_ra_err_invalid_arg;
  }
  if (!s_state.opened) {
    return k_ra_err_not_initialized;
  }
  dst[0] = 0U;
  return k_ra_err_not_supported;
}

ra_err_t ra_sau_i2c_abort(void)
{
  if (!s_state.opened) {
    return k_ra_err_not_initialized;
  }
  s_state.busy = false;
  return k_ra_ok;
}

ra_err_t ra_sau_i2c_status_get(uint8_t* out_open, uint8_t* out_busy)
{
  RA_CHECK_NULL_PTR(out_open, s_tag, "out_open must not be nullptr");
  RA_CHECK_NULL_PTR(out_busy, s_tag, "out_busy must not be nullptr");
  *out_open = (uint8_t)(s_state.opened ? 1U : 0U);
  *out_busy = (uint8_t)(s_state.busy ? 1U : 0U);
  return k_ra_ok;
}

ra_err_t ra_sau_i2c_close(void)
{
  if (!s_state.opened) {
    return k_ra_err_invalid_state;
  }
  s_state.opened = false;
  s_state.busy   = false;
  return k_ra_ok;
}
