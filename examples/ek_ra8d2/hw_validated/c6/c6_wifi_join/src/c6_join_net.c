/**
 * @file examples/ek_ra8d2/hw_validated/c6/c6_wifi_join/src/c6_join_net.c
 * @brief NetX Duo IP bring-up over the C6 link: DHCP lease + ICMP reachability.
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Implements ::c6_join_net_up. Once the C6 station has associated, this creates
 * the NetX Duo packet pool and IP instance on top of ``nx_ether_driver_c6``,
 * enables ARP / UDP / ICMP, runs the vendored DHCP client to a bound lease, and
 * pings the leased gateway. All of it is the host side of the stack: the C6 is
 * a pure L2 bridge, so DHCP, ARP and ICMP are answered by the RA8, not the
 * co-processor.
 *
 * The heavy NetX objects are file-scope statics because this image has no heap
 * (NASA Power of 10 Rule 3). ::c6_join_net_up runs exactly once, on the
 * application worker thread.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>
#include <string.h>

#include "c6_join.h"
#include "nx_api.h"
#include "nx_ether_driver_c6.h"
#include "nxd_dhcp_client.h"
#include "ra8_c6link.h"
#include "ra8_err.h"
#include "tx_api.h"

/**
 * @enum c6_join_net_size_t
 * @brief Static sizing for the NetX Duo objects this app owns.
 * @details Sized for full 1514-octet Ethernet frames plus DHCP and ICMP working
 * set, with a comfortable packet count so a DHCP retransmit never starves ARP.
 * @invariant ::k_c6_join_pkt_payload is at least a full Ethernet frame plus the
 *            two-octet alignment slide the driver applies.
 * @invariant ::k_c6_join_pool_bytes holds several ::k_c6_join_pkt_payload packets.
 * @par Example:
 * @code
 * static uint8_t pool[k_c6_join_pool_bytes];
 * @endcode
 * @see c6_join_net_up
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_c6_join_pkt_payload = 1568U,  /**< Per-packet payload, in octets.          */
  k_c6_join_pool_bytes  = 40960U, /**< Packet-pool backing store, in octets.   */
  k_c6_join_ip_stack    = 2048U,  /**< NetX IP helper-thread stack, in octets. */
  k_c6_join_arp_bytes   = 1040U,  /**< ARP cache backing store, in octets.     */
  k_c6_join_ip_prio     = 3U,     /**< NetX IP helper-thread priority.         */
  k_c6_join_ping_len    = 12U,    /**< ICMP echo payload length, in octets.    */
} c6_join_net_size_t;

/** @var s_pool @brief NetX packet pool control block. @since 0.1.0 */
static NX_PACKET_POOL s_pool;
/** @var s_pool_mem @brief Packet-pool backing store. @since 0.1.0 */
alignas(4) static uint8_t s_pool_mem[k_c6_join_pool_bytes];
/** @var s_ip @brief NetX IP instance driven over the C6 link. @since 0.1.0 */
static NX_IP s_ip;
/** @var s_ip_stack @brief NetX IP helper-thread stack. @since 0.1.0 */
alignas(8) static uint8_t s_ip_stack[k_c6_join_ip_stack];
/** @var s_arp_cache @brief ARP cache backing store. @since 0.1.0 */
alignas(4) static uint8_t s_arp_cache[k_c6_join_arp_bytes];
/** @var s_dhcp @brief NetX DHCP client control block. @since 0.1.0 */
static NX_DHCP s_dhcp;
/** @var s_ping_payload @brief Fixed ICMP echo payload. @since 0.1.0 */
static char s_ping_payload[k_c6_join_ping_len] = "ra8-c6-ping";
/** @var s_pool_name @brief Mutable pool name (NetX takes CHAR*). @since 0.1.0 */
static CHAR s_pool_name[] = "c6_join_pool";
/** @var s_ip_name @brief Mutable IP-instance name. @since 0.1.0 */
static CHAR s_ip_name[] = "c6_join_ip";
/** @var s_dhcp_name @brief Mutable DHCP-client name. @since 0.1.0 */
static CHAR s_dhcp_name[] = "c6_join_dhcp";

/* Create the packet pool + IP instance and enable the protocols DHCP needs. */
static UINT priv_net_create_ip(void)
{
  UINT s = nx_packet_pool_create(&s_pool,
                                 s_pool_name,
                                 (ULONG)k_c6_join_pkt_payload,
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
                   (UINT)k_c6_join_ip_prio);
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
static UINT priv_net_dhcp(c6_join_lease_t* out)
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
                                    (ULONG)k_c6_join_dhcp_wait_ms);
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

/* Send bounded ICMP echoes to the gateway; true once one is answered. */
static bool priv_net_ping(uint32_t gateway)
{
  if (gateway == 0U) {
    return false;
  }
  for (uint32_t i = 0U; i < (uint32_t)k_c6_join_ping_tries; i++) {
    NX_PACKET* resp = NX_NULL;
    UINT       s    = nx_icmp_ping(&s_ip,
                                   (ULONG)gateway,
                                   s_ping_payload,
                                   (ULONG)k_c6_join_ping_len,
                                   &resp,
                                   (ULONG)k_c6_join_ping_wait_ms);
    if (s == NX_SUCCESS) {
      if (resp != NX_NULL) {
        (void)nx_packet_release(resp);
      }
      return true;
    }
    tx_thread_sleep((ULONG)k_c6_join_ping_gap_ms);
  }
  return false;
}

ra8_err_t c6_join_net_up(ra8_c6link_t* link, const ra8_c6link_mac_t* mac, c6_join_lease_t* out)
{
  if ((link == nullptr) || (mac == nullptr) || (out == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  *out = (c6_join_lease_t){};

  nx_ether_driver_c6_bind(link);
  nx_ether_driver_c6_set_mac(mac->octet);
  nx_system_initialize();

  if (priv_net_create_ip() != NX_SUCCESS) {
    return k_ra8_err_not_initialized;
  }
  if (priv_net_dhcp(out) != NX_SUCCESS) {
    *out = (c6_join_lease_t){};
    return k_ra8_err_timeout;
  }
  out->ping_ok = priv_net_ping(out->gateway);
  return k_ra8_ok;
}
