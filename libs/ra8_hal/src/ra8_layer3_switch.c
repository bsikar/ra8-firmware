/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_layer3_switch.c
 * @brief Layer-3 Ethernet switch driver -- placeholder implementation
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Host-testable stand-in for the FSP `r_layer3_switch` driver. The
 * RA8D2 silicon does not include a Layer-3 switch; this TU exists
 * so portable networking code keeps compiling.
 */

#include "ra8_layer3_switch.h"

#include <stdint.h>

#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_log.h"

static const char* s_tag = "L3SW";

typedef struct {
  bool opened;      /**< Opened.      */
  bool promiscuous; /**< Promiscuous. */
} ra8_layer3_switch_state_t;

static ra8_layer3_switch_state_t s_state = {};

ra8_err_t ra8_layer3_switch_open(const ra8_layer3_switch_cfg_t* cfg)
{
  RA8_CHECK_NULL_PTR(cfg, s_tag, "cfg must not be nullptr");
  if (cfg->port_count == 0U) {
    return k_ra8_err_invalid_arg;
  }
  if (cfg->mtu_bytes == 0U) {
    return k_ra8_err_invalid_arg;
  }
  if (s_state.opened) {
    return k_ra8_err_exists;
  }
  s_state.opened      = true;
  s_state.promiscuous = (cfg->promiscuous != 0U);
  ra8_log_info_val(s_tag, "l3sw open ports", (uint32_t)cfg->port_count);
  return k_ra8_ok;
}

ra8_err_t ra8_layer3_switch_route_add(const ra8_layer3_switch_route_t* route)
{
  RA8_CHECK_NULL_PTR(route, s_tag, "route must not be nullptr");
  if (!s_state.opened) {
    return k_ra8_err_not_initialized;
  }
  return k_ra8_err_not_supported;
}

ra8_err_t ra8_layer3_switch_route_delete(uint32_t dst_ip, uint32_t mask)
{
  (void)dst_ip;
  (void)mask;
  if (!s_state.opened) {
    return k_ra8_err_not_initialized;
  }
  return k_ra8_err_not_supported;
}

ra8_err_t ra8_layer3_switch_status_get(uint8_t* out_open, uint8_t* out_promisc)
{
  RA8_CHECK_NULL_PTR(out_open, s_tag, "out_open must not be nullptr");
  RA8_CHECK_NULL_PTR(out_promisc, s_tag, "out_promisc must not be nullptr");
  *out_open    = (uint8_t)(s_state.opened ? 1U : 0U);
  *out_promisc = (uint8_t)(s_state.promiscuous ? 1U : 0U);
  return k_ra8_ok;
}

ra8_err_t ra8_layer3_switch_close(void)
{
  if (!s_state.opened) {
    return k_ra8_err_invalid_state;
  }
  s_state.opened      = false;
  s_state.promiscuous = false;
  return k_ra8_ok;
}
