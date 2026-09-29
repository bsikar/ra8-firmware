/**
 * @file port/netxduo/src/ra8_ipif_wifi.c
 * @brief The NetX Duo over ESP32-C6 provider for ``ra8_wifi``'s IP hook.
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details
 * Implements ::ra8_ipif_wifi_bind. The body is the head and tail of the three
 * hand-written copies this issue removes: the driver bind and MAC stamp they all
 * run before their private bring-up, and the lease they all read out of it,
 * with ::ra8_ipif_up and ::ra8_ipif_dhcp in the middle where ninety lines of
 * vendor API used to be.
 *
 * It is a separate translation unit from ``ra8_ipif.c`` on purpose: this file
 * names ``nx_ether_driver_c6`` and ``ra8_c6link_t``, and a consumer that wants
 * the bring-up over the on-chip MAC should not link the C6 link with it.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include "ra8_ipif_wifi.h"

/* Refuse a context that is missing any of the three things the hook needs. */
static ra8_err_t internal_check_ctx(const ra8_ipif_wifi_t* ctx)
{
  if ((ctx->ipif == nullptr) || (ctx->cfg == nullptr) || (ctx->link == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  if ((ctx->cfg->driver != nullptr) && (ctx->cfg->driver != nx_ether_driver_c6)) {
    return k_ra8_err_invalid_arg;
  }
  return k_ra8_ok;
}

ra8_err_t ra8_ipif_wifi_bind(void* ip_ctx, const ra8_wifi_mac_t* mac, ra8_wifi_lease_t* out)
{
  if ((ip_ctx == nullptr) || (mac == nullptr) || (out == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  *out = (ra8_wifi_lease_t){};

  const ra8_ipif_wifi_t* ctx     = (const ra8_ipif_wifi_t*)ip_ctx;
  const ra8_err_t        checked = internal_check_ctx(ctx);
  if (checked != k_ra8_ok) {
    return checked;
  }

  ra8_ipif_cfg_t cfg = *ctx->cfg;
  cfg.driver         = nx_ether_driver_c6;

  nx_ether_driver_c6_bind(ctx->link);
  nx_ether_driver_c6_set_mac(mac->octet);

  const ra8_err_t up = ra8_ipif_up(ctx->ipif, &cfg);
  if (up != k_ra8_ok) {
    return up;
  }

  const ra8_err_t leased = ra8_ipif_dhcp(ctx->ipif, out);
  if (leased != k_ra8_ok) {
    (void)ra8_ipif_down(ctx->ipif);
    *out = (ra8_wifi_lease_t){};
    return leased;
  }
  return k_ra8_ok;
}
