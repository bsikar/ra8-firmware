/**
 * @file ra_layer3_switch.c
 * @brief Layer-3 Ethernet switch driver -- placeholder implementation
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Host-testable stand-in for the FSP `r_layer3_switch` driver. The
 * RA8D2 silicon does not include a Layer-3 switch; this TU exists
 * so portable networking code keeps compiling.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_layer3_switch.h"

#include <stdint.h>

#include "ra_check.h"
#include "ra_err.h"
#include "ra_log.h"

static const char* s_tag = "L3SW";

typedef struct {
  bool opened;
  bool promiscuous;
} ra_layer3_switch_state_t;

static ra_layer3_switch_state_t s_state = {};

/* Implementation of ra_layer3_switch_open (see header for full contract) -- see header for the documented contract. */
ra_err_t ra_layer3_switch_open(const ra_layer3_switch_cfg_t* cfg)
{
  RA_CHECK_NULL_PTR(cfg, s_tag, "cfg must not be nullptr");
  if (cfg->port_count == 0U) {
    return k_ra_err_invalid_arg;
  }
  if (cfg->mtu_bytes == 0U) {
    return k_ra_err_invalid_arg;
  }
  if (s_state.opened) {
    return k_ra_err_exists;
  }
  s_state.opened      = true;
  s_state.promiscuous = (cfg->promiscuous != 0U);
  ra_log_info_val(s_tag, "l3sw open ports", (uint32_t)cfg->port_count);
  return k_ra_ok;
}

/* Implementation of ra_layer3_switch_route_add (see header for full contract) -- see header for the documented contract. */
ra_err_t ra_layer3_switch_route_add(const ra_layer3_switch_route_t* route)
{
  RA_CHECK_NULL_PTR(route, s_tag, "route must not be nullptr");
  if (!s_state.opened) {
    return k_ra_err_not_initialized;
  }
  return k_ra_err_not_supported;
}

/* Implementation of ra_layer3_switch_route_delete (see header for full contract) -- see header for the documented contract. */
ra_err_t ra_layer3_switch_route_delete(uint32_t dst_ip, uint32_t mask)
{
  (void)dst_ip;
  (void)mask;
  if (!s_state.opened) {
    return k_ra_err_not_initialized;
  }
  return k_ra_err_not_supported;
}

/* Implementation of ra_layer3_switch_status_get (see header for full contract) -- see header for the documented contract. */
ra_err_t ra_layer3_switch_status_get(uint8_t* out_open, uint8_t* out_promisc)
{
  RA_CHECK_NULL_PTR(out_open, s_tag, "out_open must not be nullptr");
  RA_CHECK_NULL_PTR(out_promisc, s_tag, "out_promisc must not be nullptr");
  *out_open    = (uint8_t)(s_state.opened ? 1U : 0U);
  *out_promisc = (uint8_t)(s_state.promiscuous ? 1U : 0U);
  return k_ra_ok;
}

/* Implementation of ra_layer3_switch_close (see header for full contract) -- see header for the documented contract. */
ra_err_t ra_layer3_switch_close(void)
{
  if (!s_state.opened) {
    return k_ra_err_invalid_state;
  }
  s_state.opened      = false;
  s_state.promiscuous = false;
  return k_ra_ok;
}
