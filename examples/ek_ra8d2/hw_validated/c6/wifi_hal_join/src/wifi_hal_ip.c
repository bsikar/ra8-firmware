/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie */
/**
 * @file examples/ek_ra8d2/hw_validated/c6/wifi_hal_join/src/wifi_hal_ip.c
 * @brief The NetX Duo DHCP provider the ra8_wifi facade calls for an address.
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Implements ::wifi_hal_ip_bind, the ::ra8_wifi_ip_bind_fn. Once ``ra8_wifi``
 * reports the station associated, the facade calls this to turn the L2 link into
 * an IP address: it binds ``nx_ether_driver_c6`` to the open C6 link, creates
 * the NetX Duo packet pool and IP instance, enables ARP / UDP / ICMP, and runs
 * the vendored DHCP client to a bound lease. The whole IP stack runs on the RA8;
 * the C6 is a pure L2 bridge.
 *
 * This is the one piece the facade cannot own without dragging NetX Duo into
 * every consumer, so it lives with the application by design. The heavy NetX
 * objects are file-scope statics because this image has no heap (NASA Rule 3).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 *
 * @since 0.1.0
 */

#include <stdint.h>

#include "nx_api.h"
#include "nx_ether_driver_c6.h"
#include "nxd_dhcp_client.h"
#include "ra8_c6link.h"
#include "ra8_err.h"
#include "ra8_wifi.h"
#include "tx_api.h"
#include "wifi_hal_join.h"

/**
 * @enum wifi_hal_net_size_t
 * @brief Static sizing for the NetX Duo objects this application owns.
 * @details Sized for full Ethernet frames plus the DHCP working set, with a
 *          comfortable packet count so a DHCP retransmit never starves ARP.
 * @invariant ::k_wifi_hal_pkt_payload is at least a full Ethernet frame plus the
 *            driver's two-octet alignment slide.
 * @invariant ::k_wifi_hal_pool_bytes holds several ::k_wifi_hal_pkt_payload
 *            packets.
 * @par Example:
 * @code
 * static uint8_t pool[k_wifi_hal_pool_bytes];
 * @endcode
 * @see wifi_hal_ip_bind
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_wifi_hal_pkt_payload = 1568U,  /**< Per-packet payload, in octets.          */
  k_wifi_hal_pool_bytes  = 40960U, /**< Packet-pool backing store, in octets.   */
  k_wifi_hal_ip_stack    = 2048U,  /**< NetX IP helper-thread stack, in octets. */
  k_wifi_hal_arp_bytes   = 1040U,  /**< ARP cache backing store, in octets.     */
  k_wifi_hal_ip_prio     = 3U,     /**< NetX IP helper-thread priority.         */
} wifi_hal_net_size_t;

/** @var s_pool @brief NetX packet pool control block. @since 0.1.0 */
static NX_PACKET_POOL s_pool;
/** @var s_pool_mem @brief Packet-pool backing store. @since 0.1.0 */
alignas(4) static uint8_t s_pool_mem[k_wifi_hal_pool_bytes];
/** @var s_ip @brief NetX IP instance driven over the C6 link. @since 0.1.0 */
static NX_IP s_ip;
/** @var s_ip_stack @brief NetX IP helper-thread stack. @since 0.1.0 */
alignas(8) static uint8_t s_ip_stack[k_wifi_hal_ip_stack];
/** @var s_arp_cache @brief ARP cache backing store. @since 0.1.0 */
alignas(4) static uint8_t s_arp_cache[k_wifi_hal_arp_bytes];
/** @var s_dhcp @brief NetX DHCP client control block. @since 0.1.0 */
static NX_DHCP s_dhcp;
/** @var s_pool_name @brief Mutable pool name (NetX takes CHAR*). @since 0.1.0 */
static CHAR s_pool_name[] = "wifi_hal_pool";
/** @var s_ip_name @brief Mutable IP-instance name. @since 0.1.0 */
static CHAR s_ip_name[] = "wifi_hal_ip";
/** @var s_dhcp_name @brief Mutable DHCP-client name. @since 0.1.0 */
static CHAR s_dhcp_name[] = "wifi_hal_dhcp";

/* Create the packet pool + IP instance and enable the protocols DHCP needs. */
static UINT priv_net_create_ip(void)
{
  UINT s = nx_packet_pool_create(&s_pool,
                                 s_pool_name,
                                 (ULONG)k_wifi_hal_pkt_payload,
                                 (VOID*)s_pool_mem,
                                 (ULONG)sizeof(s_pool_mem));
  if (s != NX_SUCCESS) {
    return s;
  }
  s = nx_ip_create(&s_ip,
                   s_ip_name,
                   IP_ADDRESS(0, 0, 0, 0),
                   IP_ADDRESS(0, 0, 0, 0),
                   &s_pool,
                   nx_ether_driver_c6,
                   (VOID*)s_ip_stack,
                   (ULONG)sizeof(s_ip_stack),
                   (UINT)k_wifi_hal_ip_prio);
  if (s != NX_SUCCESS) {
    return s;
  }
  s = nx_arp_enable(&s_ip, (VOID*)s_arp_cache, (ULONG)sizeof(s_arp_cache));
  if (s != NX_SUCCESS) {
    return s;
  }
  s = nx_udp_enable(&s_ip);
  if (s != NX_SUCCESS) {
    return s;
  }
  return nx_icmp_enable(&s_ip);
}

/* Run the DHCP client to a bound lease, then read the lease into out. */
static UINT priv_net_dhcp(ra8_wifi_lease_t* out)
{
  UINT s = nx_dhcp_create(&s_dhcp, &s_ip, s_dhcp_name);
  if (s != NX_SUCCESS) {
    return s;
  }
  s = nx_dhcp_start(&s_dhcp);
  if (s != NX_SUCCESS) {
    return s;
  }
  ULONG actual = 0U;
  s            = nx_ip_status_check(&s_ip,
                                    (ULONG)NX_IP_ADDRESS_RESOLVED,
                                    &actual,
                                    (ULONG)k_wifi_hal_dhcp_wait_ms);
  if (s != NX_SUCCESS) {
    return s;
  }
  ULONG ip     = 0U;
  ULONG mask   = 0U;
  ULONG gw     = 0U;
  ULONG server = 0U;
  (void)nx_ip_address_get(&s_ip, &ip, &mask);
  (void)nx_ip_gateway_address_get(&s_ip, &gw);
  (void)nx_dhcp_server_address_get(&s_dhcp, &server);
  out->ip          = (uint32_t)ip;
  out->mask        = (uint32_t)mask;
  out->gateway     = (uint32_t)gw;
  out->dhcp_server = (uint32_t)server;
  return NX_SUCCESS;
}

ra8_err_t wifi_hal_ip_bind(void* ip_ctx, const ra8_wifi_mac_t* mac, ra8_wifi_lease_t* out)
{
  if ((ip_ctx == nullptr) || (mac == nullptr) || (out == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  *out               = (ra8_wifi_lease_t){};
  ra8_c6link_t* link = (ra8_c6link_t*)ip_ctx;

  nx_ether_driver_c6_bind(link);
  nx_ether_driver_c6_set_mac(mac->octet);
  nx_system_initialize();

  if (priv_net_create_ip() != NX_SUCCESS) {
    return k_ra8_err_not_initialized;
  }
  if (priv_net_dhcp(out) != NX_SUCCESS) {
    *out = (ra8_wifi_lease_t){};
    return k_ra8_err_timeout;
  }
  out->bound = (out->ip != 0U);
  return k_ra8_ok;
}
