/**
 * @file port/netxduo/src/ra8_ipif.c
 * @brief The shared NetX Duo IP bring-up three applications each hand-wrote.
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details
 * Implements ::ra8_ipif_up, ::ra8_ipif_dhcp and ::ra8_ipif_down. The control
 * flow is the one already present in
 * ``examples/ek_ra8d2/hw_validated/c6/wifi_hal_join/src/wifi_hal_ip.c``,
 * ``examples/ek_ra8d2/hw_validated/c6/c6_wifi_join/src/c6_join_net.c`` and
 * ``examples/ek_ra8d2/common/c6_camera_server/src/c6_cam_net.c``, with the
 * per-application sizing enums and the file-scope NetX statics replaced by
 * caller-supplied storage.
 *
 * Two things this adds to those copies rather than merely relocating: a failed
 * ::ra8_ipif_up unwinds what it created instead of leaving a half-built IP
 * instance behind, and ::ra8_ipif_down exists at all.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include "ra8_ipif.h"

#include <stdint.h>
#include <string.h>

/**
 * @enum ra8_ipif_create_t
 * @brief The address an IP instance is created with before DHCP runs.
 * @details All three hand-written copies pass ``IP_ADDRESS(0, 0, 0, 0)`` for both
 *          the address and the mask, because the lease is what fills them in.
 *          Naming it keeps the magic-number gate honest about why it is zero.
 * @invariant ::k_ra8_ipif_unbound_ip is zero; DHCP overwrites it.
 * @par Example:
 * @code
 * (void)nx_ip_create(&ip, name, (ULONG)k_ra8_ipif_unbound_ip, ...);
 * @endcode
 * @see ra8_ipif_up
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_ra8_ipif_unbound_ip = 0U, /**< Address and mask an IP instance starts with. */
} ra8_ipif_create_t;

/** @brief Base name used when the caller supplies none. @since 0.1.0 */
static const char k_ra8_ipif_default_name[] = "ra8_ipif";
/** @brief Suffix for the packet-pool name. @since 0.1.0 */
static const char k_ra8_ipif_pool_suffix[] = "_pool";
/** @brief Suffix for the IP-instance name. @since 0.1.0 */
static const char k_ra8_ipif_ip_suffix[] = "_ip";
/** @brief Suffix for the DHCP-client name. @since 0.1.0 */
static const char k_ra8_ipif_dhcp_suffix[] = "_dhcp";

/* Compose base + suffix into dst, truncating the base so the suffix survives. */
static void internal_compose_name(CHAR* dst, const char* base, const char* suffix)
{
  const size_t suffix_len = strlen(suffix);
  const size_t room       = (size_t)k_ra8_ipif_name_max - suffix_len - 1U;
  size_t       base_len   = strlen(base);

  if (base_len > room) {
    base_len = room;
  }
  (void)memcpy(dst, base, base_len);
  (void)memcpy(&dst[base_len], suffix, suffix_len);
  dst[base_len + suffix_len] = '\0';
}

/* Reject a configuration that NetX would only reject later, or not at all. */
static ra8_err_t internal_check_cfg(const ra8_ipif_cfg_t* cfg)
{
  if ((cfg->driver == nullptr) || (cfg->pool_mem == nullptr) || (cfg->ip_stack == nullptr) ||
      (cfg->arp_cache == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  if ((cfg->pool_bytes == 0U) || (cfg->ip_stack_bytes == 0U) || (cfg->arp_bytes == 0U)) {
    return k_ra8_err_invalid_size;
  }
  if (cfg->pkt_payload < (uint32_t)k_ra8_ipif_pkt_payload_min) {
    return k_ra8_err_invalid_size;
  }
  if ((cfg->ip_address != (uint32_t)k_ra8_ipif_unbound_ip) &&
      (cfg->ip_netmask == (uint32_t)k_ra8_ipif_unbound_ip)) {
    return k_ra8_err_invalid_arg;
  }
  return k_ra8_ok;
}

/* Enable the protocols DHCP and the applications need, in the copies' order. */
static UINT internal_enable_protocols(ra8_ipif_t* ipif, const ra8_ipif_cfg_t* cfg)
{
  UINT status = nx_arp_enable(&ipif->ip, cfg->arp_cache, (ULONG)cfg->arp_bytes);
  if (status != NX_SUCCESS) {
    return status;
  }
  if (!cfg->disable_udp) {
    status = nx_udp_enable(&ipif->ip);
    if (status != NX_SUCCESS) {
      return status;
    }
  }
  if (cfg->enable_tcp) {
    status = nx_tcp_enable(&ipif->ip);
    if (status != NX_SUCCESS) {
      return status;
    }
  }
  return nx_icmp_enable(&ipif->ip);
}

ra8_err_t ra8_ipif_up(ra8_ipif_t* ipif, const ra8_ipif_cfg_t* cfg)
{
  if ((ipif == nullptr) || (cfg == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  if (ipif->up) {
    return k_ra8_err_invalid_state;
  }

  const ra8_err_t checked = internal_check_cfg(cfg);
  if (checked != k_ra8_ok) {
    return checked;
  }

  const char* base = (cfg->name != nullptr) ? cfg->name : k_ra8_ipif_default_name;
  internal_compose_name(ipif->pool_name, base, k_ra8_ipif_pool_suffix);
  internal_compose_name(ipif->ip_name, base, k_ra8_ipif_ip_suffix);
  internal_compose_name(ipif->dhcp_name, base, k_ra8_ipif_dhcp_suffix);
  ipif->dhcp_wait_ms = cfg->dhcp_wait_ms;

  nx_system_initialize();

  UINT status = nx_packet_pool_create(&ipif->pool,
                                      ipif->pool_name,
                                      (ULONG)cfg->pkt_payload,
                                      cfg->pool_mem,
                                      (ULONG)cfg->pool_bytes);
  if (status != NX_SUCCESS) {
    return k_ra8_err_not_initialized;
  }

  status = nx_ip_create(&ipif->ip,
                        ipif->ip_name,
                        (ULONG)cfg->ip_address,
                        (ULONG)cfg->ip_netmask,
                        &ipif->pool,
                        cfg->driver,
                        cfg->ip_stack,
                        (ULONG)cfg->ip_stack_bytes,
                        (UINT)cfg->ip_prio);
  if (status != NX_SUCCESS) {
    (void)nx_packet_pool_delete(&ipif->pool);
    return k_ra8_err_not_initialized;
  }

  if (internal_enable_protocols(ipif, cfg) != NX_SUCCESS) {
    (void)nx_ip_delete(&ipif->ip);
    (void)nx_packet_pool_delete(&ipif->pool);
    return k_ra8_err_not_initialized;
  }

  ipif->up = true;
  return k_ra8_ok;
}

ra8_err_t ra8_ipif_dhcp(ra8_ipif_t* ipif, ra8_wifi_lease_t* out)
{
  if ((ipif == nullptr) || (out == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  *out = (ra8_wifi_lease_t){};
  if (!ipif->up) {
    return k_ra8_err_not_initialized;
  }
  if (ipif->dhcp_started) {
    return k_ra8_err_invalid_state;
  }

  if (nx_dhcp_create(&ipif->dhcp, &ipif->ip, ipif->dhcp_name) != NX_SUCCESS) {
    return k_ra8_err_timeout;
  }
  ipif->dhcp_started = true;

  ULONG actual = 0U;
  if ((nx_dhcp_start(&ipif->dhcp) != NX_SUCCESS) ||
      (nx_ip_status_check(&ipif->ip,
                          (ULONG)NX_IP_ADDRESS_RESOLVED,
                          &actual,
                          (ULONG)ipif->dhcp_wait_ms) != NX_SUCCESS)) {
    (void)nx_dhcp_delete(&ipif->dhcp);
    ipif->dhcp_started = false;
    return k_ra8_err_timeout;
  }

  ULONG ip      = 0U;
  ULONG mask    = 0U;
  ULONG gateway = 0U;
  ULONG server  = 0U;
  (void)nx_ip_address_get(&ipif->ip, &ip, &mask);
  (void)nx_ip_gateway_address_get(&ipif->ip, &gateway);
  (void)nx_dhcp_server_address_get(&ipif->dhcp, &server);

  out->ip          = (uint32_t)ip;
  out->mask        = (uint32_t)mask;
  out->gateway     = (uint32_t)gateway;
  out->dhcp_server = (uint32_t)server;
  out->bound       = (out->ip != 0U);
  if (!out->bound) {
    *out = (ra8_wifi_lease_t){};
    (void)nx_dhcp_delete(&ipif->dhcp);
    ipif->dhcp_started = false;
    return k_ra8_err_timeout;
  }
  return k_ra8_ok;
}

ra8_err_t ra8_ipif_down(ra8_ipif_t* ipif)
{
  if (ipif == nullptr) {
    return k_ra8_err_null_ptr;
  }

  bool refused = false;
  if (ipif->dhcp_started) {
    if (nx_dhcp_delete(&ipif->dhcp) != NX_SUCCESS) {
      refused = true;
    }
    ipif->dhcp_started = false;
  }
  if (ipif->up) {
    if (nx_ip_delete(&ipif->ip) != NX_SUCCESS) {
      refused = true;
    }
    if (nx_packet_pool_delete(&ipif->pool) != NX_SUCCESS) {
      refused = true;
    }
    ipif->up = false;
  }
  return refused ? k_ra8_err_busy : k_ra8_ok;
}
