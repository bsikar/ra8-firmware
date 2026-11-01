/**
 * @file port/netxduo/src/nx_ether_driver_netif.c
 * @brief NetX Duo link driver over the RA8 ``ra8_io_netif`` seam (RA8FW-318).
 *
 * [Ring 4 / PORT] {World: NS}
 *
 * The unified successor of ``nx_ether_driver_ra8_eth`` and
 * ``nx_ether_driver_c6``. Every frame crosses the ``ra8_io_netif`` seam
 * (``ra8_io_netif_send`` out, ``ra8_io_netif_poll`` in); what the seam does
 * not carry -- bring-up, teardown, link presence, MTU -- arrives at bind
 * time in ::nx_netif_backend_ops_t, so this core names no backend symbol
 * and links against either transport.
 *
 * It holds the same three pieces of state the transport-specific drivers
 * hold: whether the backend was opened, the ``NX_LINK_ENABLE`` mirror, and
 * the ``NX_IP`` the RX worker pushes frames into. NetX serialises driver
 * invocations on the IP thread, so no extra locking guards these statics.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "nx_ether_driver_netif.h"

#include <stdint.h>
#include <string.h>

#include "nx_api.h"
#include "nx_arp.h"
#include "nx_ip.h"
#include "ra8_attributes.h"
#include "ra8_err.h"
#include "tx_api.h"

#ifndef NX_DISABLE_IPV4
#include "nx_rarp.h"
#endif

/**
 * @enum nx_netif_constant_t
 * @brief Frame and address constants for the unified driver.
 *
 * @details The staging buffers cover the largest untagged 802.3 frame the
 *          driver forwards (1514 bytes payload, no FCS) regardless of the
 *          bound backend's MTU: the MTU caps what NetX asks for, the
 *          staging covers what a misbehaving peer could still deliver.
 *          The minimum frame length is the 802.3 minimum.
 */
typedef enum : uint16_t {
  k_nx_netif_max_frame      = 1514U, /**< Largest 802.3 frame forwarded. */
  k_nx_netif_min_frame      = 60U,   /**< Smallest 802.3 frame.          */
  k_nx_netif_mac_len        = 6U,    /**< Length of an Ethernet MAC.     */
  k_nx_netif_phys_addr_len  = 6U,    /**< Reported to NX_LINK_FACTORY_*. */
  k_nx_netif_hdr_bytes      = 14U,   /**< Ethernet header length.        */
  k_nx_netif_etype_off      = 12U,   /**< EtherType offset in a frame.   */
  k_nx_netif_msw_shift      = 8U,    /**< Bytes-per-byte shift for MAC.  */
  k_nx_netif_byte_mask      = 0xFFU, /**< Keep the least-significant octet. */
} nx_netif_constant_t;

/**
 * @enum nx_netif_ethertype_t
 * @brief Ethernet II EtherType constants in host byte order.
 */
typedef enum : uint16_t {
  k_nx_netif_ethertype_ipv4 = 0x0800U, /**< IPv4 frame. */
  k_nx_netif_ethertype_arp  = 0x0806U, /**< ARP frame.  */
  k_nx_netif_ethertype_rarp = 0x8035U, /**< RARP frame. */
} nx_netif_ethertype_t;

/**
 * @var s_netif
 * @brief The bound seam handle (caller-owned; see ::nx_ether_driver_netif_bind).
 */
RA8_INTERNAL static const ra8_io_netif_t* s_netif;

/**
 * @var s_ops
 * @brief The bound backend ops (caller-owned; see ::nx_ether_driver_netif_bind).
 */
RA8_INTERNAL static const nx_netif_backend_ops_t* s_ops;

/**
 * @var s_open
 * @brief Latched flag: has the backend been opened for this interface?
 *
 * @details Driven by ``NX_LINK_INITIALIZE`` and ``NX_LINK_UNINITIALIZE``.
 *          The TX / RX paths refuse to operate while it is clear.
 */
RA8_INTERNAL static UCHAR s_open;

/**
 * @var s_link_up
 * @brief Latched flag: has ``NX_LINK_ENABLE`` been the most recent transition?
 */
RA8_INTERNAL static UCHAR s_link_up;

/**
 * @var s_local_mac
 * @brief Local MAC resolved from the interface at ``NX_LINK_INITIALIZE``.
 */
RA8_INTERNAL static uint8_t s_local_mac[k_nx_netif_mac_len];

/**
 * @var s_netif_ip / s_netif_iface / worker state
 * @brief RX worker state (see below).
 */
/**
 * @enum nx_netif_worker_t
 * @brief Sizing constants for the RX-poll worker thread.
 *
 * @details Neither transport raises an RX IRQ into NetX, so a dedicated
 *          ThreadX worker polls the seam at a short cadence and pushes
 *          frames in via the deferred-receive paths. Same cadence both
 *          transport-specific drivers shipped.
 */
typedef enum : uint32_t {
  k_nx_netif_worker_stack_bytes  = 4096U, /**< RX worker stack, in octets.    */
  k_nx_netif_worker_priority     = 4U,    /**< RX worker ThreadX priority.    */
  k_nx_netif_worker_period_ticks = 1U,    /**< Sleep between polls, in ticks. */
} nx_netif_worker_t;

RA8_INTERNAL static TX_THREAD s_rx_thread;
RA8_INTERNAL static uint8_t  s_rx_thread_stack[k_nx_netif_worker_stack_bytes];
RA8_INTERNAL static NX_IP*        s_netif_ip   = NX_NULL;
RA8_INTERNAL static NX_INTERFACE* s_netif_iface = NX_NULL;
RA8_INTERNAL static uint8_t       s_rx_thread_made = 0U;

/**
 * @brief Unpack the 6-byte MAC from a NetX interface descriptor.
 *
 * @details NetX reports the local MAC as two 32-bit words: the MSW holds
 *          octets 0-1 (octet 0 in bits 8-15), the LSW octets 2-5 (octet 2
 *          in bits 24-31).
 */
RA8_INTERNAL static void internal_unpack_mac(const NX_INTERFACE* iface, uint8_t* mac)
{
  const ULONG msw = iface->nx_interface_physical_address_msw;
  const ULONG lsw = iface->nx_interface_physical_address_lsw;
  mac[0] = (uint8_t)((msw >> (ULONG)k_nx_netif_msw_shift) & (ULONG)k_nx_netif_byte_mask);
  mac[1] = (uint8_t)(msw & (ULONG)k_nx_netif_byte_mask);
  mac[2] = (uint8_t)((lsw >> 24UL) & (ULONG)k_nx_netif_byte_mask);
  mac[3] = (uint8_t)((lsw >> 16UL) & (ULONG)k_nx_netif_byte_mask);
  mac[4] = (uint8_t)((lsw >> 8UL) & (ULONG)k_nx_netif_byte_mask);
  mac[5] = (uint8_t)(lsw & (ULONG)k_nx_netif_byte_mask);
}

/**
 * @brief Pack a 6-byte MAC back into a NetX interface descriptor.
 *
 * @details Pushes the driver-resolved MAC into the interface struct so
 *          NetX subsystems that read it later (ARP, sender-MAC stamping)
 *          see the right value.
 */
RA8_INTERNAL static void internal_stamp_mac(NX_INTERFACE* iface, const uint8_t* mac)
{
  const ULONG msw = ((ULONG)mac[0] << (ULONG)k_nx_netif_msw_shift) | (ULONG)mac[1];
  const ULONG lsw = ((ULONG)mac[2] << 24UL) | ((ULONG)mac[3] << 16UL) |
                    ((ULONG)mac[4] << 8UL) | (ULONG)mac[5];
  iface->nx_interface_physical_address_msw = msw;
  iface->nx_interface_physical_address_lsw = lsw;
}

/**
 * @brief Linearise a (possibly-chained) NX_PACKET into a flat buffer.
 *
 * @return 1U on success with ``*out_len`` holding the byte count;
 *         0U when the chain does not fit ``cap``.
 */
RA8_INTERNAL static UCHAR internal_packet_to_buffer(const NX_PACKET* packet, uint8_t* dst,
                                                    uint32_t cap, uint32_t* out_len)
{
  uint32_t         total = 0U;
  const NX_PACKET* cur   = packet;
  while (cur != NX_NULL) {
    const uint8_t* seg     = (const uint8_t*)cur->nx_packet_prepend_ptr;
    const uint32_t seg_len = (uint32_t)(cur->nx_packet_append_ptr - cur->nx_packet_prepend_ptr);
    if ((total + seg_len) > cap) {
      return 0U;
    }
    (void)memcpy((void*)(dst + total), (const void*)seg, (size_t)seg_len);
    total += seg_len;
#ifndef NX_DISABLE_PACKET_CHAIN
    cur = cur->nx_packet_next;
#else
    cur = NX_NULL;
#endif
  }
  *out_len = total;
  return 1U;
}

/**
 * @brief Pick the ethernet ethertype for a NetX driver SEND command.
 *
 * @details NetX hands the driver the L3 payload only; the command kind
 *          says which ethertype goes on the wire.
 */
RA8_INTERNAL static uint16_t internal_ethertype_for_cmd(UINT cmd)
{
  if ((cmd == NX_LINK_ARP_SEND) || (cmd == NX_LINK_ARP_RESPONSE_SEND)) {
    return (uint16_t)k_nx_netif_ethertype_arp;
  }
  if (cmd == NX_LINK_RARP_SEND) {
    return (uint16_t)k_nx_netif_ethertype_rarp;
  }
  return (uint16_t)k_nx_netif_ethertype_ipv4;
}

/**
 * @brief Dispatch a received frame into NetX by ethertype.
 *
 * @details Strips the 14-byte ethernet header and hands the payload to
 *          the matching deferred-receive path. Calling
 *          ``_nx_ip_packet_deferred_receive`` for everything would drop
 *          ARP frames silently, so the ethertype is read first. Unknown
 *          ethertypes release the packet rather than leaking it.
 */
RA8_INTERNAL static void internal_dispatch_to_netx(NX_PACKET* pkt)
{
  if (pkt->nx_packet_length < (ULONG)k_nx_netif_hdr_bytes) {
    (void)nx_packet_release(pkt);
    return;
  }
  const UCHAR*   p  = (const UCHAR*)pkt->nx_packet_prepend_ptr;
  const uint16_t et = (uint16_t)(((uint16_t)p[k_nx_netif_etype_off] << 8) |
                                 (uint16_t)p[k_nx_netif_etype_off + 1U]);
  pkt->nx_packet_prepend_ptr = pkt->nx_packet_prepend_ptr + (ULONG)k_nx_netif_hdr_bytes;
  pkt->nx_packet_length      = pkt->nx_packet_length - (ULONG)k_nx_netif_hdr_bytes;
  if (et == (uint16_t)k_nx_netif_ethertype_ipv4) {
    _nx_ip_packet_deferred_receive(s_netif_ip, pkt);
    return;
  }
#ifndef NX_DISABLE_IPV4
  if (et == (uint16_t)k_nx_netif_ethertype_arp) {
    _nx_arp_packet_deferred_receive(s_netif_ip, pkt);
    return;
  }
  if (et == (uint16_t)k_nx_netif_ethertype_rarp) {
    _nx_rarp_packet_deferred_receive(s_netif_ip, pkt);
    return;
  }
#endif
  (void)nx_packet_release(pkt);
}



/**
 * @brief Allocate a receive packet, copy one frame in, tag the interface.
 *
 * @details Slides prepend by 2 bytes so the IP header lands 4-byte aligned
 *          after the driver strips the 14-byte ethernet header (NetX's IP
 *          receive path word-accesses the IP header on Cortex-M parts).
 *
 * @return The packet with the frame appended, or ``NX_NULL`` on pool
 *         exhaustion / append failure (the caller drops the frame).
 */
RA8_INTERNAL static NX_PACKET* internal_rx_packet(NX_PACKET_POOL* pool, const uint8_t* frame, uint32_t len)
{
  NX_PACKET* pkt = NX_NULL;
  if (nx_packet_allocate(pool, &pkt, NX_RECEIVE_PACKET, NX_NO_WAIT) != NX_SUCCESS) {
    return NX_NULL;
  }
  if (pkt == NX_NULL) {
    return NX_NULL;
  }
  pkt->nx_packet_prepend_ptr = pkt->nx_packet_prepend_ptr + 2U;
  pkt->nx_packet_append_ptr  = pkt->nx_packet_prepend_ptr;
  if (nx_packet_data_append(pkt, (void*)frame, (ULONG)len, pool, NX_NO_WAIT) != NX_SUCCESS) {
    (void)nx_packet_release(pkt);
    return NX_NULL;
  }
  pkt->nx_packet_ip_interface = s_netif_iface;
  return pkt;
}

/**
 * @brief One RX drain pass: pull frames off the seam until it runs dry.
 */
RA8_INTERNAL static void internal_rx_drain(void)
{
  NX_PACKET_POOL* pool = (s_netif_ip != NX_NULL) ? s_netif_ip->nx_ip_default_packet_pool : NX_NULL;
  if (pool == NX_NULL) {
    return;
  }
  static uint8_t frame[k_nx_netif_max_frame];
  while (1) {
    uint16_t  len = (uint16_t)k_nx_netif_max_frame;
    ra8_err_t err = ra8_io_netif_poll(s_netif, frame, &len);
    if ((err != k_ra8_ok) || (len == 0U)) {
      break;
    }
    NX_PACKET* pkt = internal_rx_packet(pool, frame, (uint32_t)len);
    if (pkt == NX_NULL) {
      continue;
    }
    internal_dispatch_to_netx(pkt);
  }
}

/**
 * @brief RX-poll worker entry: drain the seam every tick, forever.
 */
RA8_INTERNAL static void internal_rx_worker_entry(ULONG arg)
{
  (void)arg;
  while (1) {
    if ((s_netif_ip != NX_NULL) && (s_open != 0U)) {
      internal_rx_drain();
    }
    tx_thread_sleep((ULONG)k_nx_netif_worker_period_ticks);
  }
}

/**
 * @brief Spawn the RX-poll worker thread once, idempotent.
 */
RA8_INTERNAL static void internal_spawn_rx_worker(NX_IP* ip, NX_INTERFACE* iface)
{
  static CHAR s_thread_name[] = {'r', 'a', '8', '_', 'n', 'e', 't', 'i', 'f', '_', 'r', 'x', '\0'};
  if (s_rx_thread_made != 0U) {
    return;
  }
  s_netif_ip    = ip;
  s_netif_iface = iface;
  const UINT t  = tx_thread_create(&s_rx_thread,
                                   s_thread_name,
                                   internal_rx_worker_entry,
                                   0,
                                   (void*)s_rx_thread_stack,
                                   (ULONG)k_nx_netif_worker_stack_bytes,
                                   (UINT)k_nx_netif_worker_priority,
                                   (UINT)k_nx_netif_worker_priority,
                                   (ULONG)TX_NO_TIME_SLICE,
                                   (UINT)TX_AUTO_START);
  if (t == TX_SUCCESS) {
    s_rx_thread_made = 1U;
  }
}

/**
 * @brief Handle ``NX_LINK_INITIALIZE``: resolve the MAC, open the backend.
 */
RA8_INTERNAL static void internal_handle_init(NX_IP_DRIVER* req)
{
  NX_INTERFACE* iface = req->nx_ip_driver_interface;
  if ((iface == NX_NULL) || (s_netif == NX_NULL) || (s_ops == NX_NULL)) {
    req->nx_ip_driver_status = NX_NOT_SUCCESSFUL;
    return;
  }
  internal_unpack_mac(iface, s_local_mac);
  internal_stamp_mac(iface, s_local_mac);
  if (s_ops->open(s_local_mac) != k_ra8_ok) {
    req->nx_ip_driver_status = NX_NOT_SUCCESSFUL;
    return;
  }
  s_open                                   = 1U;
  iface->nx_interface_ip_mtu_size          = (ULONG)s_ops->mtu;
  iface->nx_interface_address_mapping_needed = NX_TRUE;
  internal_spawn_rx_worker(req->nx_ip_driver_ptr, iface);
  req->nx_ip_driver_status = NX_SUCCESS;
}

/**
 * @brief Handle ``NX_LINK_UNINITIALIZE``: tear the backend down.
 */
RA8_INTERNAL static void internal_handle_uninit(NX_IP_DRIVER* req)
{
  if ((s_open != 0U) && (s_ops != NX_NULL) && (s_ops->close != NX_NULL)) {
    s_ops->close();
  }
  s_open                   = 0U;
  s_link_up                = 0U;
  req->nx_ip_driver_status = NX_SUCCESS;
}

/**
 * @brief Handle the SEND family: build the header, stage, hand to the seam.
 *
 * @details NetX hands the link driver the L3 payload only; the 14-byte
 *          ethernet header is the driver's job (destination MAC from the
 *          request, source from the resolved local MAC, ethertype from
 *          the command). Runt frames are padded to the 802.3 minimum.
 *          The packet is released on every path.
 */
RA8_INTERNAL static void internal_handle_send(NX_IP_DRIVER* req)
{
  NX_PACKET* pkt = req->nx_ip_driver_packet;
  if ((pkt == NX_NULL) || (s_open == 0U) || (s_link_up == 0U)) {
    if (pkt != NX_NULL) {
      (void)nx_packet_transmit_release(pkt);
    }
    req->nx_ip_driver_status = NX_NOT_SUCCESSFUL;
    return;
  }
  static uint8_t tx_staging[k_nx_netif_max_frame];
  const ULONG    msw = req->nx_ip_driver_physical_address_msw;
  const ULONG    lsw = req->nx_ip_driver_physical_address_lsw;
  const uint16_t et  = internal_ethertype_for_cmd(req->nx_ip_driver_command);
  tx_staging[0] = (uint8_t)((msw >> (ULONG)k_nx_netif_msw_shift) & (ULONG)k_nx_netif_byte_mask);
  tx_staging[1] = (uint8_t)(msw & (ULONG)k_nx_netif_byte_mask);
  tx_staging[2] = (uint8_t)((lsw >> 24UL) & (ULONG)k_nx_netif_byte_mask);
  tx_staging[3] = (uint8_t)((lsw >> 16UL) & (ULONG)k_nx_netif_byte_mask);
  tx_staging[4] = (uint8_t)((lsw >> 8UL) & (ULONG)k_nx_netif_byte_mask);
  tx_staging[5] = (uint8_t)(lsw & (ULONG)k_nx_netif_byte_mask);
  tx_staging[6] = s_local_mac[0];
  tx_staging[7] = s_local_mac[1];
  tx_staging[8] = s_local_mac[2];
  tx_staging[9] = s_local_mac[3];
  tx_staging[10] = s_local_mac[4];
  tx_staging[11] = s_local_mac[5];
  tx_staging[12] = (uint8_t)((et >> 8) & (uint16_t)k_nx_netif_byte_mask);
  tx_staging[13] = (uint8_t)(et & (uint16_t)k_nx_netif_byte_mask);
  uint32_t body_len = 0U;
  if (internal_packet_to_buffer(pkt,
                                tx_staging + k_nx_netif_hdr_bytes,
                                (uint32_t)sizeof(tx_staging) - (uint32_t)k_nx_netif_hdr_bytes,
                                &body_len) == 0U) {
    (void)nx_packet_transmit_release(pkt);
    req->nx_ip_driver_status = NX_NOT_SUCCESSFUL;
    return;
  }
  uint32_t len = body_len + (uint32_t)k_nx_netif_hdr_bytes;
  if (len < (uint32_t)k_nx_netif_min_frame) {
    (void)memset((void*)(tx_staging + len), 0, (size_t)((uint32_t)k_nx_netif_min_frame - len));
    len = (uint32_t)k_nx_netif_min_frame;
  }
  const ra8_err_t err = ra8_io_netif_send(s_netif, tx_staging, (uint16_t)len);
  (void)nx_packet_transmit_release(pkt);
  req->nx_ip_driver_status = (err == k_ra8_ok) ? (UINT)NX_SUCCESS : (UINT)NX_NOT_SUCCESSFUL;
}

/**
 * @brief Handle ``NX_LINK_DEFERRED_PROCESSING``: drain the seam into NetX.
 */
RA8_INTERNAL static void internal_handle_deferred_rx(NX_IP_DRIVER* req)
{
  if ((req->nx_ip_driver_ptr == NX_NULL) || (s_open == 0U)) {
    req->nx_ip_driver_status = NX_NOT_SUCCESSFUL;
    return;
  }
  s_netif_iface = req->nx_ip_driver_interface;
  internal_rx_drain();
  req->nx_ip_driver_status = NX_SUCCESS;
}

/**
 * @brief Handle ``NX_LINK_GET_STATUS``: report link presence.
 *
 * @details A backend with a ``link_up`` op gets asked; a backend without
 *          one (the c6link contract) is answered from the driver's own
 *          ``NX_LINK_ENABLE`` mirror.
 */
RA8_INTERNAL static void internal_handle_get_status(NX_IP_DRIVER* req)
{
  if (req->nx_ip_driver_return_ptr == NX_NULL) {
    req->nx_ip_driver_status = NX_NOT_SUCCESSFUL;
    return;
  }
  if (s_open == 0U) {
    *(req->nx_ip_driver_return_ptr) = (ULONG)NX_FALSE;
    req->nx_ip_driver_status        = NX_SUCCESS;
    return;
  }
  if (s_ops->link_up != NX_NULL) {
    *(req->nx_ip_driver_return_ptr) = s_ops->link_up() ? (ULONG)NX_TRUE : (ULONG)NX_FALSE;
  } else {
    *(req->nx_ip_driver_return_ptr) = (s_link_up != 0U) ? (ULONG)NX_TRUE : (ULONG)NX_FALSE;
  }
  req->nx_ip_driver_status = NX_SUCCESS;
}

/**
 * @brief Apply an ENABLE/DISABLE state change to the link mirror and iface.
 */
RA8_INTERNAL static void internal_set_link_state(NX_IP_DRIVER* driver_req, UCHAR link_up)
{
  s_link_up                       = link_up;
  driver_req->nx_ip_driver_status = NX_SUCCESS;
  if (driver_req->nx_ip_driver_interface != NX_NULL) {
    driver_req->nx_ip_driver_interface->nx_interface_link_up = (link_up != 0U) ? NX_TRUE : NX_FALSE;
  }
}

ra8_err_t nx_ether_driver_netif_bind(const ra8_io_netif_t* netif, const nx_netif_backend_ops_t* ops)
{
  if ((netif == NX_NULL) || (ops == NX_NULL)) {
    return k_ra8_err_null_ptr;
  }
  if (s_open != 0U) {
    return k_ra8_err_busy;
  }
  s_netif = netif;
  s_ops   = ops;
  return k_ra8_ok;
}

/* nx_ether_driver_netif -- see header for full description. */
void nx_ether_driver_netif(NX_IP_DRIVER* driver_req)
{
  /* NetX never invokes the driver with NULL but mirror the upstream
   * nx_link reference driver and guard anyway. */
  if (driver_req == NX_NULL) {
    return;
  }
  switch (driver_req->nx_ip_driver_command) {
    case NX_LINK_INITIALIZE:
      internal_handle_init(driver_req);
      break;
    case NX_LINK_ENABLE:
      internal_set_link_state(driver_req, 1U);
      break;
    case NX_LINK_DISABLE:
      internal_set_link_state(driver_req, 0U);
      break;
    case NX_LINK_PACKET_SEND:
    case NX_LINK_PACKET_BROADCAST:
    case NX_LINK_ARP_SEND:
    case NX_LINK_ARP_RESPONSE_SEND:
    case NX_LINK_RARP_SEND:
      internal_handle_send(driver_req);
      break;
    case NX_LINK_DEFERRED_PROCESSING:
      internal_handle_deferred_rx(driver_req);
      break;
    case NX_LINK_GET_STATUS:
      internal_handle_get_status(driver_req);
      break;
    case NX_LINK_UNINITIALIZE:
      internal_handle_uninit(driver_req);
      break;
    case NX_LINK_MULTICAST_JOIN:
    case NX_LINK_MULTICAST_LEAVE:
    case NX_LINK_INTERFACE_ATTACH:
    case NX_LINK_INTERFACE_DETACH:
    case NX_LINK_SET_PHYSICAL_ADDRESS:
      driver_req->nx_ip_driver_status = NX_SUCCESS;
      break;
    default:
      driver_req->nx_ip_driver_status = NX_NOT_SUCCESSFUL;
      break;
  }
}
