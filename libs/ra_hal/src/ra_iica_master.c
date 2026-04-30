/**
 * @file ra_iica_master.c
 * @brief IICA controller (master) driver -- placeholder implementation
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Host-testable stand-in for the FSP `r_iica_master` driver. The
 * RA8D2 silicon does not include an IICA block; this TU exists so
 * portable code keeps compiling and so unit tests can exercise the
 * NULL-arg / lifecycle behaviour of the FSP-shaped surface.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_iica_master.h"

#include <stdint.h>

#include "ra_check.h"
#include "ra_err.h"
#include "ra_log.h"

static const char* s_tag = "IICA_M";

typedef enum : uint16_t {
  k_ra_iica_master_addr_max_10bit = 0x03FFU,
} ra_iica_master_const_t;

typedef struct {
  bool     opened;
  bool     busy;
  uint16_t slave_addr;
} ra_iica_master_state_t;

static ra_iica_master_state_t s_state = {};

ra_err_t ra_iica_master_open(const ra_iica_master_cfg_t* cfg)
{
  RA_CHECK_NULL_PTR(cfg, s_tag, "cfg must not be nullptr");
  if (cfg->addr_mode >= k_ra_iica_master_addr_max) {
    return k_ra_err_invalid_arg;
  }
  if (cfg->rate_hz == 0U) {
    return k_ra_err_invalid_arg;
  }
  if (s_state.opened) {
    return k_ra_err_exists;
  }
  s_state.opened     = true;
  s_state.busy       = false;
  s_state.slave_addr = cfg->slave_addr;
  ra_log_info_val(s_tag, "iica_master open ch", (uint32_t)cfg->channel);
  return k_ra_ok;
}

ra_err_t ra_iica_master_read(uint8_t* dst, uint32_t len, uint8_t restart)
{
  (void)restart;
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

ra_err_t ra_iica_master_write(const uint8_t* src, uint32_t len, uint8_t restart)
{
  (void)restart;
  RA_CHECK_NULL_PTR(src, s_tag, "src must not be nullptr");
  if (len == 0U) {
    return k_ra_err_invalid_arg;
  }
  if (!s_state.opened) {
    return k_ra_err_not_initialized;
  }
  return k_ra_err_not_supported;
}

ra_err_t ra_iica_master_abort(void)
{
  if (!s_state.opened) {
    return k_ra_err_not_initialized;
  }
  s_state.busy = false;
  return k_ra_ok;
}

ra_err_t ra_iica_master_slave_address_set(uint16_t new_addr)
{
  if (new_addr > k_ra_iica_master_addr_max_10bit) {
    return k_ra_err_invalid_arg;
  }
  if (!s_state.opened) {
    return k_ra_err_not_initialized;
  }
  s_state.slave_addr = new_addr;
  return k_ra_ok;
}

ra_err_t ra_iica_master_status_get(uint8_t* out_open, uint8_t* out_busy)
{
  RA_CHECK_NULL_PTR(out_open, s_tag, "out_open must not be nullptr");
  RA_CHECK_NULL_PTR(out_busy, s_tag, "out_busy must not be nullptr");
  *out_open = (uint8_t)(s_state.opened ? 1U : 0U);
  *out_busy = (uint8_t)(s_state.busy ? 1U : 0U);
  return k_ra_ok;
}

ra_err_t ra_iica_master_close(void)
{
  if (!s_state.opened) {
    return k_ra_err_invalid_state;
  }
  s_state.opened     = false;
  s_state.busy       = false;
  s_state.slave_addr = 0U;
  return k_ra_ok;
}
