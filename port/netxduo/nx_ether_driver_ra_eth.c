/**
 * @file port/netxduo/nx_ether_driver_ra_eth.c
 * @brief NetX Duo network driver bridging onto the RA8D2 ``ra_eth`` frame API
 *
 * @par Tag
 * [Ring 4 / PORT] {World: NS}
 *
 * @details
 * Implements ``nx_ether_driver_ra_eth`` -- the single entry point
 * passed to ``nx_ip_create()`` -- by dispatching on
 * ``driver_req->nx_ip_driver_command`` into the matching
 * ``ra_eth_*`` polled frame primitive.
 *
 * The shim is intentionally thin: every NetX Duo I/O request maps
 * 1:1 onto one ``ra_eth_open`` / ``ra_eth_close`` / ``ra_eth_write``
 * / ``ra_eth_read`` / ``ra_eth_link_status`` call. The driver holds
 * three pieces of state of its own:
 *
 *   - ``s_ra_eth_open`` -- whether ``ra_eth_open`` has been called.
 *   - ``s_link_up``     -- whether ``NX_LINK_ENABLE`` was the most
 *                          recent enable / disable command.
 *   - ``s_attached_ip`` -- the NX_IP pointer captured on
 *                          ``NX_LINK_INTERFACE_ATTACH``, used by the
 *                          deferred-RX path so it can hand the
 *                          received packet to the right stack.
 *
 * NetX serialises driver invocations on the IP thread, so no
 * additional locking is required around these statics.
 *
 * Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "nx_ether_driver_ra_eth.h"

#include <stdint.h>
#include <string.h>

#include "nx_api.h"
#include "ra_err.h"
#include "ra_eth.h"

/**
 * @enum nx_ra_eth_constants_t
 * @brief Compile-time constants for the NetX Duo <-> ``ra_eth`` bridge.
 *
 * @details
 * The staging buffer covers the largest untagged 802.3 frame we ever
 * push to ``ra_eth_write`` (1514 bytes payload, no FCS). The minimum
 * frame length matches ``ra_eth``'s own ``k_ra_eth_min_frame``.
 */
typedef enum : uint16_t {
  k_nx_ra_eth_max_frame      = 1514U, /**< Largest 802.3 frame we forward.   */
  k_nx_ra_eth_min_frame      = 60U,   /**< Smallest 802.3 frame.             */
  k_nx_ra_eth_mac_len        = 6U,    /**< Length of an Ethernet MAC.        */
  k_nx_ra_eth_phys_addr_len  = 6U,    /**< Reported to NX_LINK_FACTORY_*.    */
  k_nx_ra_eth_mtu            = 1500U, /**< Default Ethernet payload MTU.     */
  k_nx_ra_eth_phys_msw_shift = 8U,    /**< Bytes-per-byte shift for MAC msw. */
} nx_ra_eth_constants_t;

/**
 * @enum nx_ra_eth_mac_byte_idx_t
 * @brief Indices into the local 6-byte MAC array.
 */
typedef enum : uint8_t {
  k_nx_ra_eth_mac_byte_0 = 0U,
  k_nx_ra_eth_mac_byte_1 = 1U,
  k_nx_ra_eth_mac_byte_2 = 2U,
  k_nx_ra_eth_mac_byte_3 = 3U,
  k_nx_ra_eth_mac_byte_4 = 4U,
  k_nx_ra_eth_mac_byte_5 = 5U,
} nx_ra_eth_mac_byte_idx_t;

/**
 * @enum nx_ra_eth_mac_word_shift_t
 * @brief Bit shifts that pack the 6 MAC bytes into NetX's msw/lsw words.
 *
 * @details
 * NetX reports the local MAC as two 32-bit words: the MSW holds
 * octets 0-1 (with octet 0 in bits 8-15), the LSW holds octets 2-5
 * (with octet 2 in bits 24-31). These named shifts replace the
 * magic numbers the casts would otherwise contain.
 */
typedef enum : uint8_t {
  k_nx_ra_eth_msw_shift_byte0 = 8U,
  k_nx_ra_eth_lsw_shift_byte2 = 24U,
  k_nx_ra_eth_lsw_shift_byte3 = 16U,
  k_nx_ra_eth_lsw_shift_byte4 = 8U,
  k_nx_ra_eth_lsw_shift_byte5 = 0U,
} nx_ra_eth_mac_word_shift_t;

/**
 * @var s_tx_staging
 * @brief Contiguous TX staging buffer.
 *
 * @details
 * NetX packets may be chained across multiple ``NX_PACKET`` blocks;
 * ``ra_eth_write`` wants a single linear buffer. We copy each
 * outgoing chain into ``s_tx_staging`` before handing it to the HAL.
 * The buffer is sized to the largest untagged frame we ever forward
 * (1514 bytes) and is statically allocated (NASA Power-of-10 Rule 3).
 *
 * @note Only safe to access from the NetX IP thread.
 *
 * @since 0.1.0
 */
static uint8_t s_tx_staging[k_nx_ra_eth_max_frame];

/**
 * @var s_rx_staging
 * @brief Contiguous RX staging buffer.
 *
 * @details
 * ``ra_eth_read`` returns frames into a flat buffer; we hold the
 * scratch space here and then copy into a freshly-allocated
 * ``NX_PACKET`` for the stack to consume.
 *
 * @since 0.1.0
 */
static uint8_t s_rx_staging[k_nx_ra_eth_max_frame];

/**
 * @var s_ra_eth_open
 * @brief Latched flag: has ``ra_eth_open`` been called for this interface?
 *
 * @details
 * Driven by ``NX_LINK_INITIALIZE`` (true) and ``NX_LINK_UNINITIALIZE``
 * (false). The TX / RX command paths refuse to operate when this flag
 * is clear -- mirrors how NetX's reference Ethernet driver gates IO.
 *
 * @since 0.1.0
 */
static UCHAR s_ra_eth_open;

/**
 * @var s_link_up
 * @brief Latched flag: has ``NX_LINK_ENABLE`` been the most recent transition?
 *
 * @details
 * NetX uses ``NX_LINK_ENABLE`` / ``NX_LINK_DISABLE`` as a software
 * gate independent of PHY link state; we honour it by refusing to
 * transmit (and by skipping the RX drain) when the flag is clear.
 *
 * @since 0.1.0
 */
static UCHAR s_link_up;

/**
 * @var s_local_mac
 * @brief Cached local MAC address captured at INITIALIZE time.
 *
 * @details
 * NetX exposes the MAC through the interface's
 * ``nx_interface_physical_address_msw`` / ``..._lsw`` fields. We pack
 * them back into a 6-byte array so the packed form can be handed to
 * ``ra_eth_open``.
 *
 * @since 0.1.0
 */
static uint8_t s_local_mac[k_nx_ra_eth_mac_len];

/* Pull the 6-byte MAC out of the NetX interface descriptor -- see implementation for details. */
static void priv_unpack_mac(const NX_INTERFACE* iface, uint8_t* mac)
{
  ULONG msw                   = iface->nx_interface_physical_address_msw;
  ULONG lsw                   = iface->nx_interface_physical_address_lsw;
  mac[k_nx_ra_eth_mac_byte_0] = (uint8_t)((msw >> (ULONG)k_nx_ra_eth_msw_shift_byte0) & 0xFFU);
  mac[k_nx_ra_eth_mac_byte_1] = (uint8_t)(msw & 0xFFU);
  mac[k_nx_ra_eth_mac_byte_2] = (uint8_t)((lsw >> (ULONG)k_nx_ra_eth_lsw_shift_byte2) & 0xFFU);
  mac[k_nx_ra_eth_mac_byte_3] = (uint8_t)((lsw >> (ULONG)k_nx_ra_eth_lsw_shift_byte3) & 0xFFU);
  mac[k_nx_ra_eth_mac_byte_4] = (uint8_t)((lsw >> (ULONG)k_nx_ra_eth_lsw_shift_byte4) & 0xFFU);
  mac[k_nx_ra_eth_mac_byte_5] = (uint8_t)(lsw & 0xFFU);
}

/* Linearise a (possibly-chained) NX_PACKET into the staging buffer -- see implementation for details. */
static UCHAR
priv_packet_to_buffer(const NX_PACKET* packet, uint8_t* dst, uint32_t cap, uint32_t* out_len)
{
  uint32_t         total = 0U;
  const NX_PACKET* cur   = packet;
  while (cur != NX_NULL) {
    const uint8_t* seg     = (const uint8_t*)cur->nx_packet_prepend_ptr;
    uint32_t       seg_len = (uint32_t)(cur->nx_packet_append_ptr - cur->nx_packet_prepend_ptr);
    if ((total + seg_len) > cap) {
      return 0U;
    }
    /* Copy payload bytes into the linear staging buffer. */
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

/* Handle ``NX_LINK_INITIALIZE``: capture MAC + open the NIC -- see implementation for details. */
static void priv_handle_init(NX_IP_DRIVER* req)
{
  NX_INTERFACE* iface = req->nx_ip_driver_interface;
  if (iface == NX_NULL) {
    req->nx_ip_driver_status = NX_NOT_SUCCESSFUL;
    return;
  }
  priv_unpack_mac(iface, s_local_mac);

  ra_eth_cfg_t cfg = {
    .mac_address        = {0U, 0U, 0U, 0U, 0U, 0U},
    .channel            = 0U,
    .num_tx_descriptors = 0U,
    .num_rx_descriptors = 0U,
    .buffer_size        = 0U,
  };
  (void)memcpy((void*)cfg.mac_address, (const void*)s_local_mac, (size_t)k_nx_ra_eth_mac_len);

  ra_err_t err = ra_eth_open(&cfg);
  if (err != k_ra_ok) {
    req->nx_ip_driver_status = NX_NOT_SUCCESSFUL;
    return;
  }
  s_ra_eth_open = 1U;
  /* Advertise NetX's framing capabilities for this interface. */
  iface->nx_interface_ip_mtu_size            = (ULONG)k_nx_ra_eth_mtu;
  iface->nx_interface_address_mapping_needed = NX_TRUE;
  req->nx_ip_driver_status                   = NX_SUCCESS;
}

/* Handle ``NX_LINK_UNINITIALIZE``: close the NIC -- see implementation for details. */
static void priv_handle_uninit(NX_IP_DRIVER* req)
{
  if (s_ra_eth_open != 0U) {
    (void)ra_eth_close();
    s_ra_eth_open = 0U;
  }
  s_link_up                = 0U;
  req->nx_ip_driver_status = NX_SUCCESS;
}

/**
 * @brief Handle ``NX_LINK_PACKET_SEND`` / ``..._BROADCAST`` / ``ARP_*``.
 *
 * @details
 * Linearises the NX_PACKET chain, calls ``ra_eth_write``, then
 * unconditionally releases the packet via
 * ``nx_packet_transmit_release`` (NetX expects the driver to own
 * the buffer once it accepts the request).
 *
 * @param[in,out] req NetX driver request.
 *
 * @since 0.1.0
 *
 * @pre Module has been initialised.
 * @pre Caller has validated arguments.
 * @post Side effects bounded to documented state.
 * @post State reflects operation result.
 * @note Not thread-safe unless documented otherwise.
 */
static void priv_handle_send(NX_IP_DRIVER* req)
{
  NX_PACKET* pkt = req->nx_ip_driver_packet;
  if (pkt == NX_NULL || s_ra_eth_open == 0U || s_link_up == 0U) {
    if (pkt != NX_NULL) {
      (void)nx_packet_transmit_release(pkt);
    }
    req->nx_ip_driver_status = NX_NOT_SUCCESSFUL;
    return;
  }

  uint32_t len = 0U;
  if (priv_packet_to_buffer(pkt, s_tx_staging, (uint32_t)sizeof(s_tx_staging), &len) == 0U) {
    (void)nx_packet_transmit_release(pkt);
    req->nx_ip_driver_status = NX_NOT_SUCCESSFUL;
    return;
  }

  /* Pad runt frames up to the 802.3 minimum so the EDMAC engine
   * does not reject them. The pad bytes are zero per IEEE 802.3-2018
   * "padding" requirement. */
  if (len < (uint32_t)k_nx_ra_eth_min_frame) {
    (void)memset((void*)(s_tx_staging + len), 0, (size_t)((uint32_t)k_nx_ra_eth_min_frame - len));
    len = (uint32_t)k_nx_ra_eth_min_frame;
  }

  ra_err_t err = ra_eth_write(s_tx_staging, len);
  (void)nx_packet_transmit_release(pkt);
  req->nx_ip_driver_status = (err == k_ra_ok) ? (UINT)NX_SUCCESS : (UINT)NX_NOT_SUCCESSFUL;
}

/**
 * @brief Handle ``NX_LINK_DEFERRED_PROCESSING``: drain RX into NetX.
 *
 * @details
 * Loops calling ``ra_eth_read`` until the descriptor ring is empty.
 * For each frame we allocate a new NX_PACKET, copy the bytes in,
 * and hand the packet to ``_nx_ip_packet_receive`` (NetX consumes
 * and eventually releases it). Allocation failures are tolerated --
 * the frame is dropped and the rx_err counter (in ``ra_eth_stats``)
 * captures the loss.
 *
 * @param[in,out] req NetX driver request.
 *
 * @since 0.1.0
 *
 * @pre Module has been initialised.
 * @pre Caller has validated arguments.
 * @post Side effects bounded to documented state.
 * @post State reflects operation result.
 * @note Not thread-safe unless documented otherwise.
 */
static void priv_handle_deferred_rx(NX_IP_DRIVER* req)
{
  NX_IP* ip_ptr = req->nx_ip_driver_ptr;
  if (ip_ptr == NX_NULL || s_ra_eth_open == 0U) {
    req->nx_ip_driver_status = NX_NOT_SUCCESSFUL;
    return;
  }

  /* The default packet pool lives off the IP control block; NetX
   * expects RX-side allocation to come from it. */
  NX_PACKET_POOL* pool = ip_ptr->nx_ip_default_packet_pool;
  if (pool == NX_NULL) {
    req->nx_ip_driver_status = NX_NOT_SUCCESSFUL;
    return;
  }

  /* Drain the descriptor ring. ra_eth_read returns k_ra_err_no_data
   * when the ring is empty -- that is the loop exit. */
  while (1) {
    uint32_t got = 0U;
    ra_err_t err = ra_eth_read(s_rx_staging, (uint32_t)sizeof(s_rx_staging), &got);
    if (err != k_ra_ok || got == 0U) {
      break;
    }
    NX_PACKET* pkt = NX_NULL;
    UINT       a   = nx_packet_allocate(pool, &pkt, NX_RECEIVE_PACKET, NX_NO_WAIT);
    if (a != NX_SUCCESS || pkt == NX_NULL) {
      /* Packet pool exhausted; drop the frame and keep draining. */
      continue;
    }
    /* Append the bytes (this also bumps nx_packet_length). */
    UINT app = nx_packet_data_append(pkt, (VOID*)s_rx_staging, (ULONG)got, pool, (ULONG)NX_NO_WAIT);
    if (app != NX_SUCCESS) {
      (void)nx_packet_release(pkt);
      continue;
    }
    pkt->nx_packet_ip_interface = req->nx_ip_driver_interface;

    /* Hand to the NetX receive engine (it owns the packet from here). */
    _nx_ip_packet_deferred_receive(ip_ptr, pkt);
  }
  req->nx_ip_driver_status = NX_SUCCESS;
}

/* Handle ``NX_LINK_GET_STATUS``: report the PHY link bit -- see implementation for details. */
static void priv_handle_get_status(NX_IP_DRIVER* req)
{
  if (req->nx_ip_driver_return_ptr == NX_NULL) {
    req->nx_ip_driver_status = NX_NOT_SUCCESSFUL;
    return;
  }
  ra_eth_link_t link = {.link_up = 0U, .speed_mbps = 0U, .full_duplex = 0U, .bmsr = 0U};
  if (s_ra_eth_open == 0U) {
    *(req->nx_ip_driver_return_ptr) = (ULONG)NX_FALSE;
    req->nx_ip_driver_status        = NX_SUCCESS;
    return;
  }
  ra_err_t err = ra_eth_link_status(&link);
  if (err != k_ra_ok) {
    *(req->nx_ip_driver_return_ptr) = (ULONG)NX_FALSE;
    req->nx_ip_driver_status        = NX_NOT_SUCCESSFUL;
    return;
  }
  *(req->nx_ip_driver_return_ptr) = (link.link_up != 0U) ? (ULONG)NX_TRUE : (ULONG)NX_FALSE;
  req->nx_ip_driver_status        = NX_SUCCESS;
}

/* Nx ether driver ra eth -- see implementation for details. */
void nx_ether_driver_ra_eth(NX_IP_DRIVER* driver_req)
{
  /* NetX never invokes the driver with NULL but mirror the FileX
   * pattern and the upstream nx_link reference driver. */
  if (driver_req == NX_NULL) {
    return;
  }

  switch (driver_req->nx_ip_driver_command) {
    case NX_LINK_INITIALIZE:
      priv_handle_init(driver_req);
      break;

    case NX_LINK_ENABLE:
      s_link_up                       = 1U;
      driver_req->nx_ip_driver_status = NX_SUCCESS;
      if (driver_req->nx_ip_driver_interface != NX_NULL) {
        driver_req->nx_ip_driver_interface->nx_interface_link_up = NX_TRUE;
      }
      break;

    case NX_LINK_DISABLE:
      s_link_up                       = 0U;
      driver_req->nx_ip_driver_status = NX_SUCCESS;
      if (driver_req->nx_ip_driver_interface != NX_NULL) {
        driver_req->nx_ip_driver_interface->nx_interface_link_up = NX_FALSE;
      }
      break;

    case NX_LINK_PACKET_SEND:
    case NX_LINK_PACKET_BROADCAST:
    case NX_LINK_ARP_SEND:
    case NX_LINK_ARP_RESPONSE_SEND:
    case NX_LINK_RARP_SEND:
      priv_handle_send(driver_req);
      break;

    case NX_LINK_DEFERRED_PROCESSING:
      priv_handle_deferred_rx(driver_req);
      break;

    case NX_LINK_GET_STATUS:
      priv_handle_get_status(driver_req);
      break;

    case NX_LINK_UNINITIALIZE:
      priv_handle_uninit(driver_req);
      break;

    case NX_LINK_MULTICAST_JOIN:
    case NX_LINK_MULTICAST_LEAVE:
      /* No multicast filter support on the ESWM block at this layer;
       * accept the request so NetX does not abort, and let the
       * software-side IP stack do its own multicast filtering. */
      driver_req->nx_ip_driver_status = NX_SUCCESS;
      break;

    default:
      driver_req->nx_ip_driver_status = NX_NOT_SUCCESSFUL;
      break;
  }
}
