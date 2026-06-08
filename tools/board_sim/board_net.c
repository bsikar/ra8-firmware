/**
 * @file board_net.c
 * @brief Virtual network peer (Ethernet/ARP/IPv4/ICMP/TCP) for board_sim
 *
 * @details
 * Implements the "other host on the wire" behind the ra_eth frame seam (see
 * board_net.h). The peer is 192.168.1.1 (MAC 02:00:5E:00:53:01); the firmware is
 * 192.168.1.42. Its state machine resolves the firmware over ARP, pings it
 * (ICMP), then opens a TCP connection to the echo server on port 7, sends a
 * payload, and verifies the echo -- proving the NetX networking example runs
 * end-to-end with no hardware. Frames are exchanged as plain byte buffers;
 * main.c marshals them to/from guest memory.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * @since 0.1.0
 */

#include "board_net.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/** @brief Addressing + protocol constants for the peer and the firmware. */
typedef enum : uint32_t {
  k_net_peer_ip = 0xC0A80101UL, /**< 192.168.1.1   (the peer).        */
  k_net_fw_ip   = 0xC0A8012AUL, /**< 192.168.1.42  (the firmware).    */
  k_net_echo_port = 7U,         /**< Firmware TCP echo server port.   */
  k_net_peer_port = 49152U,     /**< Peer ephemeral source port.      */
} net_addr_t;

/** @brief Frame offsets / sizes (Ethernet II + ARP + IPv4 + ICMP + TCP). */
typedef enum : uint32_t {
  k_eth_hdr   = 14U,     /**< dst[6] src[6] type[2].          */
  k_eth_arp   = 0x0806U, /**< ARP ethertype.                  */
  k_eth_ipv4  = 0x0800U, /**< IPv4 ethertype.                 */
  k_arp_len   = 28U,     /**< ARP payload length.             */
  k_ip_hdr    = 20U,     /**< IPv4 header (no options).       */
  k_icmp_hdr  = 8U,      /**< ICMP echo header.               */
  k_ip_proto_icmp = 1U,  /**< IPv4 protocol = ICMP.           */
  k_ip_proto_tcp  = 6U,  /**< IPv4 protocol = TCP.            */
  k_mac_len   = 6U,      /**< Ethernet address length.        */
  k_net_buf   = 1600U,   /**< Staging buffer size.            */
} net_frame_t;

/** @brief Peer state machine. */
typedef enum : uint8_t {
  k_net_init = 0U, /**< Nothing sent yet.                 */
  k_net_arp  = 1U, /**< ARP request out; awaiting reply.  */
  k_net_ping = 2U, /**< ICMP echo out; awaiting reply.    */
  k_net_done = 3U, /**< ICMP done (TCP handled here too). */
} net_state_t;

static const uint8_t s_peer_mac[k_mac_len] = {0x02U, 0x00U, 0x5EU, 0x00U, 0x53U, 0x01U};

static bool     s_trace;
static uint8_t  s_state;
static uint8_t  s_fw_mac[k_mac_len]; /**< Learned from ARP.            */
static bool     s_fw_mac_known;
static uint32_t s_arp_replies;       /**< ARP replies received.        */
static uint32_t s_pings;             /**< ICMP echo replies received.  */
static uint32_t s_wait;              /**< Ticks since the last send.   */
static uint16_t s_ping_seq;          /**< ICMP echo sequence.          */
static uint32_t s_tx_frames;         /**< Frames the firmware sent.    */
static uint32_t s_polls;             /**< ra_eth_read polls served.    */
static uint32_t s_delivered;         /**< Frames delivered to firmware.*/

/* One-deep TX queue: the next frame the peer wants the firmware to receive. */
static uint8_t  s_rxq[k_net_buf];
static uint32_t s_rxq_len;

/* =============================================================================
 * Byte / checksum helpers.
 * =============================================================================
 */

/** @brief Store a 16-bit value big-endian (network order) at @p p. */
static void put16(uint8_t* p, uint16_t v)
{
  p[0] = (uint8_t)(v >> 8);
  p[1] = (uint8_t)(v & 0xFFU);
}

/** @brief Store a 32-bit value big-endian at @p p. */
static void put32(uint8_t* p, uint32_t v)
{
  p[0] = (uint8_t)(v >> 24);
  p[1] = (uint8_t)((v >> 16) & 0xFFU);
  p[2] = (uint8_t)((v >> 8) & 0xFFU);
  p[3] = (uint8_t)(v & 0xFFU);
}

/** @brief Read a big-endian 16-bit value from @p p. */
static uint16_t get16(const uint8_t* p)
{
  return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

/** @brief Read a big-endian 32-bit value from @p p. */
static uint32_t get32(const uint8_t* p)
{
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

/** @brief 16-bit one's-complement checksum over @p len bytes at @p d. */
static uint16_t net_checksum(const uint8_t* d, uint32_t len, uint32_t seed)
{
  uint32_t sum = seed;
  for (uint32_t i = 0U; (i + 1U) < len; i += 2U) {
    sum += ((uint32_t)d[i] << 8) | (uint32_t)d[i + 1U];
  }
  if ((len & 1U) != 0U) {
    sum += (uint32_t)d[len - 1U] << 8;
  }
  while ((sum >> 16) != 0U) {
    sum = (sum & 0xFFFFU) + (sum >> 16);
  }
  return (uint16_t)(~sum & 0xFFFFU);
}

/** @brief Queue a built frame for the firmware to receive (drops if one waits). */
static void net_queue(const uint8_t* frame, uint32_t len)
{
  if ((s_rxq_len != 0U) || (len > (uint32_t)k_net_buf)) {
    return;
  }
  (void)memcpy(s_rxq, frame, len);
  s_rxq_len = len;
}

/** @brief Fill the 14-byte Ethernet header into @p f. */
static void net_eth_hdr(uint8_t* f, const uint8_t* dst, uint16_t ethertype)
{
  (void)memcpy(&f[0], dst, k_mac_len);
  (void)memcpy(&f[6], s_peer_mac, k_mac_len);
  put16(&f[12], ethertype);
}

/* =============================================================================
 * ARP -- resolve the firmware's MAC.
 * =============================================================================
 */

/** @brief Build + queue an ARP request asking who-has the firmware's IP. */
static void net_send_arp_request(void)
{
  static const uint8_t bcast[k_mac_len] = {0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU};
  uint8_t              f[k_eth_hdr + k_arp_len];
  (void)memset(f, 0, sizeof(f));
  net_eth_hdr(f, bcast, (uint16_t)k_eth_arp);
  uint8_t* a = &f[k_eth_hdr];
  put16(&a[0], 1U);                  /* htype = Ethernet.   */
  put16(&a[2], (uint16_t)k_eth_ipv4); /* ptype = IPv4.      */
  a[4] = (uint8_t)k_mac_len;
  a[5] = 4U;
  put16(&a[6], 1U); /* op = request. */
  (void)memcpy(&a[8], s_peer_mac, k_mac_len);
  put32(&a[14], (uint32_t)k_net_peer_ip);
  put32(&a[24], (uint32_t)k_net_fw_ip);
  net_queue(f, sizeof(f));
}

/** @brief Build + queue an ARP reply giving the peer's MAC to the firmware. */
static void net_send_arp_reply(const uint8_t* to_mac, uint32_t to_ip)
{
  uint8_t f[k_eth_hdr + k_arp_len];
  (void)memset(f, 0, sizeof(f));
  net_eth_hdr(f, to_mac, (uint16_t)k_eth_arp);
  uint8_t* a = &f[k_eth_hdr];
  put16(&a[0], 1U);
  put16(&a[2], (uint16_t)k_eth_ipv4);
  a[4] = (uint8_t)k_mac_len;
  a[5] = 4U;
  put16(&a[6], 2U); /* op = reply. */
  (void)memcpy(&a[8], s_peer_mac, k_mac_len);
  put32(&a[14], (uint32_t)k_net_peer_ip);
  (void)memcpy(&a[18], to_mac, k_mac_len);
  put32(&a[24], to_ip);
  net_queue(f, sizeof(f));
}

/* =============================================================================
 * ICMP -- ping the firmware.
 * =============================================================================
 */

/** @brief Fill a 20-byte IPv4 header (no options) + compute its checksum. */
static void net_ip_hdr(uint8_t* ip, uint8_t proto, uint16_t payload_len)
{
  (void)memset(ip, 0, k_ip_hdr);
  ip[0] = 0x45U; /* version 4, IHL 5. */
  put16(&ip[2], (uint16_t)(k_ip_hdr + payload_len));
  put16(&ip[4], 0x1234U); /* identification. */
  put16(&ip[6], 0x4000U); /* don't fragment. */
  ip[8]  = 64U;           /* TTL. */
  ip[9]  = proto;
  put32(&ip[12], (uint32_t)k_net_peer_ip);
  put32(&ip[16], (uint32_t)k_net_fw_ip);
  put16(&ip[10], net_checksum(ip, k_ip_hdr, 0U));
}

/** @brief Build + queue an ICMP echo request to the firmware. */
static void net_send_ping(void)
{
  if (!s_fw_mac_known) {
    return;
  }
  uint8_t f[k_eth_hdr + k_ip_hdr + k_icmp_hdr + 16U];
  (void)memset(f, 0, sizeof(f));
  net_eth_hdr(f, s_fw_mac, (uint16_t)k_eth_ipv4);
  const uint16_t icmp_len = (uint16_t)(k_icmp_hdr + 16U);
  net_ip_hdr(&f[k_eth_hdr], (uint8_t)k_ip_proto_icmp, icmp_len);
  uint8_t* ic = &f[k_eth_hdr + k_ip_hdr];
  ic[0]       = 8U; /* echo request. */
  s_ping_seq++;
  put16(&ic[4], 0xBEEFU); /* identifier. */
  put16(&ic[6], s_ping_seq);
  for (uint32_t i = 0U; i < 16U; i++) {
    ic[k_icmp_hdr + i] = (uint8_t)(0x40U + i); /* payload pattern. */
  }
  put16(&ic[2], net_checksum(ic, icmp_len, 0U));
  net_queue(f, sizeof(f));
}

/* =============================================================================
 * RX parsing + the peer state machine.
 * =============================================================================
 */

/** @brief Handle an inbound ARP frame (learn the firmware MAC / answer who-has). */
static void net_rx_arp(const uint8_t* a, uint32_t len)
{
  if (len < (uint32_t)k_arp_len) {
    return;
  }
  const uint16_t op     = get16(&a[6]);
  const uint32_t spa    = get32(&a[14]);
  const uint32_t tpa    = get32(&a[24]);
  if (spa == (uint32_t)k_net_fw_ip) {
    (void)memcpy(s_fw_mac, &a[8], k_mac_len); /* sender HW = firmware MAC. */
    s_fw_mac_known = true;
    if (op == 2U) {
      s_arp_replies++;
    }
  }
  if ((op == 1U) && (tpa == (uint32_t)k_net_peer_ip)) {
    net_send_arp_reply(&a[8], spa); /* firmware asked who-has us. */
  }
  if (s_fw_mac_known && (s_state == (uint8_t)k_net_arp)) {
    net_send_ping();
    s_state = (uint8_t)k_net_ping;
    s_wait  = 0U;
  }
}

/** @brief Handle an inbound ICMP echo reply (count a successful ping). */
static void net_rx_icmp(const uint8_t* ic, uint32_t len)
{
  if ((len >= (uint32_t)k_icmp_hdr) && (ic[0] == 0U)) { /* echo reply. */
    s_pings++;
    if (s_state == (uint8_t)k_net_ping) {
      s_state = (uint8_t)k_net_done;
      if (s_trace) {
        (void)fprintf(stderr, "  [net] ICMP echo reply from 192.168.1.42 -- ping ok\n");
      }
    }
  }
}

/** @brief Handle an inbound IPv4 frame, dispatching by protocol. */
static void net_rx_ipv4(const uint8_t* ip, uint32_t len)
{
  if (len < (uint32_t)k_ip_hdr) {
    return;
  }
  const uint32_t ihl = (uint32_t)(ip[0] & 0x0FU) * 4U;
  if ((ihl < (uint32_t)k_ip_hdr) || (ihl > len)) {
    return;
  }
  const uint8_t proto = ip[9];
  if (proto == (uint8_t)k_ip_proto_icmp) {
    net_rx_icmp(&ip[ihl], len - ihl);
  }
}

void board_net_on_tx(const uint8_t* frame, uint32_t len)
{
  s_tx_frames++;
  if (len < (uint32_t)k_eth_hdr) {
    return;
  }
  if (s_trace) {
    (void)fprintf(stderr,
                  "  [net] firmware TX %u bytes ethertype 0x%04X\n",
                  len,
                  (unsigned)get16(&frame[12]));
  }
  const uint16_t ethertype = get16(&frame[12]);
  if (ethertype == (uint16_t)k_eth_arp) {
    net_rx_arp(&frame[k_eth_hdr], len - (uint32_t)k_eth_hdr);
  } else if (ethertype == (uint16_t)k_eth_ipv4) {
    net_rx_ipv4(&frame[k_eth_hdr], len - (uint32_t)k_eth_hdr);
  }
}

uint32_t board_net_poll_rx(uint8_t* buf, uint32_t max)
{
  s_polls++;
  if ((s_rxq_len == 0U) || (s_rxq_len > max)) {
    return 0U;
  }
  const uint32_t n = s_rxq_len;
  (void)memcpy(buf, s_rxq, n);
  s_rxq_len = 0U;
  s_delivered++;
  return n;
}

void board_net_tick(void)
{
  s_wait++;
  if (s_state == (uint8_t)k_net_init) {
    net_send_arp_request();
    s_state = (uint8_t)k_net_arp;
    s_wait  = 0U;
    return;
  }
  /* Retransmit the pending step if the firmware has not answered for a while
   * (the stack may still be bringing the interface up on the first attempts). */
  enum : uint32_t { k_net_retry = 2000U };
  if (s_wait > k_net_retry) {
    s_wait = 0U;
    if (s_state == (uint8_t)k_net_arp) {
      net_send_arp_request();
    } else if (s_state == (uint8_t)k_net_ping) {
      net_send_ping();
    }
  }
}

void board_net_init(bool trace)
{
  s_trace        = trace;
  s_state        = (uint8_t)k_net_init;
  s_fw_mac_known = false;
  s_arp_replies  = 0U;
  s_pings        = 0U;
  s_wait         = 0U;
  s_ping_seq     = 0U;
  s_rxq_len      = 0U;
  s_tx_frames    = 0U;
  s_polls        = 0U;
  s_delivered    = 0U;
  (void)memset(s_fw_mac, 0, sizeof(s_fw_mac));
}

void board_net_report(void)
{
  if (s_state == (uint8_t)k_net_init) {
    return; /* networking never came up in this run. */
  }
  (void)fprintf(stderr,
                "  NET peer      : 192.168.1.1 <-> 192.168.1.42  ARP %s  ping %s (%u)\n",
                s_fw_mac_known ? "resolved" : "--",
                (s_pings > 0U) ? "ok" : "--",
                s_pings);
  (void)fprintf(stderr,
                "  NET activity  : fw TX %u frame(s), RX polls %u, delivered %u\n",
                s_tx_frames,
                s_polls,
                s_delivered);
}
