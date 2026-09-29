/**
 * @file examples/ek_ra8d2/hw_validated/c6/c6_wifi_join/src/c6_join_net.c
 * @brief IP bring-up over the C6 link through the shared facade, plus the
 *        application's own ICMP reachability probe.
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Implements ::c6_join_net_up. Once the C6 station has associated, this brings
 * an IP interface up over ``nx_ether_driver_c6``, takes a DHCP lease, and pings
 * the leased gateway. All of it is the host side of the stack: the C6 is a pure
 * L2 bridge, so DHCP, ARP and ICMP are answered by the RA8, not the
 * co-processor.
 *
 * @par What this file used to be
 * The ninety lines of vendor API every application carrying this bring-up wrote
 * for itself: a packet pool, an IP instance, an ARP cache and a helper-thread
 * stack created by hand, ARP / UDP / ICMP enabled one call at a time, the
 * vendored DHCP client run to a bound lease, and four addresses read back out.
 * That body now lives once, in ``port/netxduo``, and what is left here is this
 * application's half of it: the buffers, the sizes, and the gateway probe.
 *
 * The ping stays. It is application behaviour, not bring-up: this app exists to
 * prove the bench network answers, and ::ra8_ipif_t publishes its ``NX_IP`` so
 * a consumer can reach the stack the facade brought up.
 *
 * The buffers are file-scope statics because this image has no heap (NASA Power
 * of 10 Rule 3). ::c6_join_net_up runs exactly once, on the application worker
 * thread.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>
#include <string.h>

#include "c6_join.h"
#include "nx_api.h"
#include "ra8_c6link.h"
#include "ra8_err.h"
#include "ra8_ipif.h"
#include "ra8_ipif_wifi.h"
#include "ra8_wifi.h"
#include "tx_api.h"

/**
 * @enum c6_join_net_size_t
 * @brief Static sizing for the NetX Duo objects this app owns.
 * @details Sized for full 1514-octet Ethernet frames plus DHCP and ICMP working
 * set, with a comfortable packet count so a DHCP retransmit never starves ARP.
 * These are the figures the hand-written bring-up used; the bring-up moved, the
 * budget did not.
 * @invariant ::k_c6_join_pkt_payload is at least a full Ethernet frame plus the
 *            two-octet alignment slide the driver applies, so it is at least
 *            ::k_ra8_ipif_pkt_payload_min.
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

static_assert((uint32_t)k_c6_join_pkt_payload >= (uint32_t)k_ra8_ipif_pkt_payload_min,
              "the application packet payload still clears the facade's floor");
static_assert((uint32_t)k_ra8_c6link_mac_bytes == (uint32_t)k_ra8_wifi_mac_bytes,
              "the link address and the facade address are the same width");

/** @brief Packet-pool backing store. @since 0.1.0 */
alignas(4) static uint8_t s_pool_mem[k_c6_join_pool_bytes];
/** @brief NetX IP helper-thread stack. @since 0.1.0 */
alignas(8) static uint8_t s_ip_stack[k_c6_join_ip_stack];
/** @brief ARP cache backing store. @since 0.1.0 */
alignas(4) static uint8_t s_arp_cache[k_c6_join_arp_bytes];
/** @brief Fixed ICMP echo payload. @since 0.1.0 */
static char s_ping_payload[k_c6_join_ping_len] = "ra8-c6-ping";
/** @brief The NetX control blocks and object names the bring-up fills. @since 0.1.0 */
static ra8_ipif_t s_ipif;
/** @brief Context the provider is handed as its ``ip_ctx``. @since 0.1.0 */
static ra8_ipif_wifi_t s_bind;

/** @brief Buffers, sizes and waits this application brings the interface up with.
 *  @details `driver` is left null, which is how ::ra8_ipif_wifi_bind is told to
 *           use the C6 link driver. @since 0.1.0 */
static const ra8_ipif_cfg_t k_ipif_cfg = {
  .name           = "c6_join",
  .driver         = nullptr,
  .pool_mem       = s_pool_mem,
  .pool_bytes     = (uint32_t)sizeof(s_pool_mem),
  .pkt_payload    = (uint32_t)k_c6_join_pkt_payload,
  .ip_stack       = s_ip_stack,
  .ip_stack_bytes = (uint32_t)sizeof(s_ip_stack),
  .ip_prio        = (uint32_t)k_c6_join_ip_prio,
  .arp_cache      = s_arp_cache,
  .arp_bytes      = (uint32_t)sizeof(s_arp_cache),
  .enable_tcp     = false,
  .dhcp_wait_ms   = (uint32_t)k_c6_join_dhcp_wait_ms,
};

/* Send bounded ICMP echoes to the gateway; true once one is answered. */
static bool priv_net_ping(uint32_t gateway)
{
  if (gateway == 0U) {
    return false;
  }
  for (uint32_t i = 0U; i < (uint32_t)k_c6_join_ping_tries; i++) {
    NX_PACKET* resp = NX_NULL;
    UINT       s    = nx_icmp_ping(&s_ipif.ip,
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

  ra8_wifi_mac_t station = {};
  (void)memcpy(station.octet, mac->octet, sizeof(station.octet));

  s_bind = (ra8_ipif_wifi_t){
    .ipif = &s_ipif,
    .cfg  = &k_ipif_cfg,
    .link = link,
  };
  /* The handle is file-scope, so a second association would otherwise meet an
     interface still up from the first. Down on a handle that was never up, or
     is already down, is a no-op. */
  (void)ra8_ipif_down(&s_ipif);

  ra8_wifi_lease_t lease  = {};
  const ra8_err_t  leased = ra8_ipif_wifi_bind(&s_bind, &station, &lease);
  if (leased != k_ra8_ok) {
    return leased;
  }
  out->ip          = lease.ip;
  out->mask        = lease.mask;
  out->gateway     = lease.gateway;
  out->dhcp_server = lease.dhcp_server;
  out->ping_ok     = priv_net_ping(out->gateway);
  return k_ra8_ok;
}
