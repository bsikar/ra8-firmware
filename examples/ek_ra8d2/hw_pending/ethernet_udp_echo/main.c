/**
 * @file examples/ek_ra8d2/ethernet_udp_echo/main.c
 * @brief Ethernet UDP echo responder for EK-RA8D2 (HIL companion to ethernet_tcp_echo)
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Brings the chip up via ``ra_cgc_init()``, routes the on-board PEF7071
 * PHY pins via ``ra_board_ethernet_init()`` (RGMII per EK-RA8D2 v1 UM
 * Table 26 p 33), opens the NIC at MAC ``02:00:00:00:00:01``
 * (locally-administered, unicast), then sits on the descriptor ring
 * popping frames and answering them by hand:
 *
 *   - RFC 826 ARP "who-has 192.168.1.43" -> ARP reply with our MAC.
 *   - RFC 791 IPv4 / RFC 792 ICMP echo request -> ICMP echo reply with
 *     payload mirrored back, type/code rewritten and checksum redone.
 *   - RFC 768 UDP, listening on port 7 (Echo Protocol per RFC 862).
 *     Any datagram with destination port 7 has its payload mirrored
 *     back to the sender's source IP / port with the UDP and IPv4
 *     checksums recomputed.
 *
 * Frame construction is byte-level by hand (no lwIP); header layouts
 * are named typed enums and every checksum is recomputed. The structure
 * follows ``ethernet_tcp_echo`` so the two apps look like siblings.
 *
 * The HIL host (Pi) verifies the echo by:
 *   1. Flashing the firmware.
 *   2. Waiting for the boot banner ``eth: ip=192.168.1.43`` on UART.
 *   3. Sending a random N-byte UDP datagram to 192.168.1.43:7.
 *   4. Receiving the datagram back and byte-asserting equality.
 *
 * @author Brighton Sikarskie
 * @date 2026-05-17
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra_board_ek_ra8d2.h"
#include "ra_cgc.h"
#include "ra_err.h"
#include "ra_eth.h"
#include "ra_gpio_constants.h"
#include "ra_isr.h"
#include "ra_port_constants.h"
#include "ra_port_utils.h"
#include "ra_sci.h"
#include "ra_time.h"

/**
 * @enum eth_udp_echo_config_t
 * @brief Top-level demo configuration constants.
 *
 * @details
 * Every numeric used by the bring-up path appears here so the magic-
 * number lint never sees a bare integer. The SCI8 channel matches the
 * on-board J-Link OB CDC bridge pins (PD_02 / PD_03).
 */
typedef enum : uint32_t {
  k_eth_udp_echo_baud         = 115200U, /**< J-Link OB CDC baud.     */
  k_eth_udp_echo_sci_channel  = 8U,      /**< SCI8 logging channel.   */
  k_eth_udp_echo_link_poll_ms = 100U,    /**< PHY BMSR poll period.   */
} eth_udp_echo_config_t;

/**
 * @enum eth_udp_echo_buf_size_t
 * @brief Frame buffer sizing constants.
 */
typedef enum : uint16_t {
  k_eth_udp_echo_frame_buf = 1536U, /**< Max raw Ethernet frame.   */
  k_eth_udp_echo_min_frame = 60U,   /**< 802.3 minimum (excl FCS). */
} eth_udp_echo_buf_size_t;

/**
 * @enum eth_hdr_layout_t
 * @brief Byte offsets inside the 14-byte Ethernet II header.
 *
 * @details RFC 894 / IEEE 802.3 -- destination MAC, source MAC,
 * EtherType big-endian.
 */
typedef enum : uint8_t {
  k_eth_off_dst_mac   = 0U,  /**< Destination MAC (6 bytes).  */
  k_eth_off_src_mac   = 6U,  /**< Source MAC (6 bytes).       */
  k_eth_off_ethertype = 12U, /**< EtherType (2 bytes BE).     */
  k_eth_hdr_len       = 14U, /**< Total Ethernet II header.   */
  k_eth_mac_len       = 6U,  /**< MAC address byte count.     */
} eth_hdr_layout_t;

/**
 * @enum eth_ethertype_t
 * @brief EtherType values we recognise (RFC 7042).
 */
typedef enum : uint16_t {
  k_ethertype_ipv4 = 0x0800U, /**< Internet Protocol v4. */
  k_ethertype_arp  = 0x0806U, /**< Address Resolution.   */
} eth_ethertype_t;

/**
 * @enum arp_layout_t
 * @brief Byte offsets inside an ARP-over-Ethernet packet (RFC 826).
 */
typedef enum : uint8_t {
  k_arp_off_htype   = 0U,
  k_arp_off_ptype   = 2U,
  k_arp_off_hlen    = 4U,
  k_arp_off_plen    = 5U,
  k_arp_off_oper    = 6U,
  k_arp_off_sha     = 8U,
  k_arp_off_spa     = 14U,
  k_arp_off_tha     = 18U,
  k_arp_off_tpa     = 24U,
  k_arp_payload_len = 28U,
} arp_layout_t;

/**
 * @enum arp_const_t
 * @brief ARP fixed-field values.
 */
typedef enum : uint16_t {
  k_arp_htype_ether = 1U,
  k_arp_oper_req    = 1U,
  k_arp_oper_reply  = 2U,
  k_arp_hlen_eth    = 6U,
  k_arp_plen_ipv4   = 4U,
} arp_const_t;

/**
 * @enum ipv4_layout_t
 * @brief Byte offsets inside an IPv4 header (RFC 791, no options).
 */
typedef enum : uint8_t {
  k_ipv4_off_ver_ihl  = 0U,
  k_ipv4_off_tos      = 1U,
  k_ipv4_off_total    = 2U,
  k_ipv4_off_id       = 4U,
  k_ipv4_off_frag     = 6U,
  k_ipv4_off_ttl      = 8U,
  k_ipv4_off_proto    = 9U,
  k_ipv4_off_checksum = 10U,
  k_ipv4_off_src_ip   = 12U,
  k_ipv4_off_dst_ip   = 16U,
  k_ipv4_hdr_len      = 20U,
} ipv4_layout_t;

/**
 * @enum ipv4_const_t
 * @brief IPv4 fixed-field values used by the responder.
 */
typedef enum : uint8_t {
  k_ipv4_ver_ihl     = 0x45U,
  k_ipv4_tos_default = 0x00U,
  k_ipv4_ttl_default = 64U,
  k_ipv4_proto_icmp  = 1U,
  k_ipv4_proto_udp   = 17U,
  k_ipv4_addr_len    = 4U,
} ipv4_const_t;

/**
 * @enum icmp_layout_t
 * @brief Byte offsets inside the ICMP header (RFC 792).
 */
typedef enum : uint8_t {
  k_icmp_off_type = 0U,
  k_icmp_off_code = 1U,
  k_icmp_off_sum  = 2U,
  k_icmp_hdr_len  = 8U,
} icmp_layout_t;

/**
 * @enum icmp_type_t
 * @brief ICMP message types we generate / recognise.
 */
typedef enum : uint8_t {
  k_icmp_type_echo_reply   = 0U,
  k_icmp_type_echo_request = 8U,
} icmp_type_t;

/**
 * @enum udp_layout_t
 * @brief Byte offsets inside the UDP header (RFC 768).
 *
 * @details
 * 8 bytes total: src_port (2 BE), dst_port (2 BE), length (2 BE),
 * checksum (2 BE). The IPv4 pseudo-header used for checksum is shared
 * with the TCP pseudo-header layout in RFC 793 sec 3.1.
 */
typedef enum : uint8_t {
  k_udp_off_src_port = 0U, /**< Source port BE.   */
  k_udp_off_dst_port = 2U, /**< Destination port BE. */
  k_udp_off_length   = 4U, /**< UDP length (hdr+data) BE. */
  k_udp_off_checksum = 6U, /**< UDP checksum BE.  */
  k_udp_hdr_len      = 8U, /**< UDP header length. */
} udp_layout_t;

/**
 * @enum udp_const_t
 * @brief UDP demo constants.
 */
typedef enum : uint16_t {
  k_udp_listen_port = 7U, /**< Echo Protocol (RFC 862). */
} udp_const_t;

/**
 * @enum proto_misc_t
 * @brief Misc protocol constants (bit shifts, masks, byte indices).
 */
typedef enum : uint32_t {
  k_shift_byte       = 8U,
  k_shift_word       = 16U,
  k_shift_dword      = 24U,
  k_shift_nibble_hi  = 4U,
  k_mask_byte        = 0xFFU,
  k_mask_word        = 0xFFFFU,
  k_mask_nibble_hi   = 0xF0U,
  k_idx_b0           = 0U,
  k_idx_b1           = 1U,
  k_idx_pseudo_zero  = 8U,
  k_idx_pseudo_proto = 9U,
  k_idx_pseudo_len   = 10U,
  k_pseudo_hdr_len   = 12U,
  k_ipv4_ver_field   = 0x40U,
  k_step_word_bytes  = 2U,
} proto_misc_t;

/** @brief Static demo identity: MAC, IPv4. */
static const uint8_t k_my_mac[k_eth_mac_len]    = {0x02U, 0x00U, 0x00U, 0x00U, 0x00U, 0x01U};
static const uint8_t k_my_ip[k_ipv4_addr_len]   = {192U, 168U, 1U, 43U};
static const uint8_t k_broadcast[k_eth_mac_len] = {0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU};

/** @brief Pinout for the on-board J-Link OB CDC channel (SCI8 / PD02 + PD03). */
static const ra_port_pin_t k_pin_log_txd =
  (ra_port_pin_t)(((uint16_t)k_ra_port_13 << 8) | (uint16_t)k_ra_pin_2);
static const ra_port_pin_t k_pin_log_rxd =
  (ra_port_pin_t)(((uint16_t)k_ra_port_13 << 8) | (uint16_t)k_ra_pin_3);

/**
 * @brief Park the CPU forever in WFI on fatal init failure.
 *
 * @pre Called only after a fatal error in boot.
 * @pre IRQs may or may not be enabled.
 * @post CPU is parked; only a debugger or external reset wakes it.
 *
 * @since 0.1.0
 */
static void eth_udp_echo_panic_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

/**
 * @brief Send a NUL-terminated ASCII line over SCI8 (best-effort).
 *
 * @param[in] s ASCII string (NUL-terminated).
 *
 * @pre s != nullptr.
 * @pre ra_sci_init() succeeded for the SCI8 channel.
 * @post Bytes have been polled out of TXD8 (or discarded silently on
 *       backpressure -- this is logging only).
 *
 * @since 0.1.0
 */
static void eth_udp_echo_log(const char* s)
{
  if (s == nullptr) {
    return;
  }
  uint32_t len = 0U;
  while (s[len] != '\0') {
    len++;
  }
  (void)ra_sci_write_polling((uint8_t)k_eth_udp_echo_sci_channel, (const uint8_t*)s, len);
}

/**
 * @brief Read a 16-bit big-endian word from a packet buffer.
 *
 * @param[in] p Pointer to the high byte.
 * @return The native-order 16-bit value.
 *
 * @pre p points to at least 2 readable bytes.
 * @post No state changes.
 * @since 0.1.0
 */
static uint16_t rd_be16(const uint8_t* p)
{
  return (uint16_t)(((uint16_t)p[k_idx_b0] << k_shift_byte) | (uint16_t)p[k_idx_b1]);
}

/**
 * @brief Write a 16-bit big-endian word to a packet buffer.
 *
 * @param[out] p Pointer to the high byte.
 * @param[in]  v Native-order 16-bit value.
 *
 * @pre p points to at least 2 writable bytes.
 * @post Two bytes at p hold v in network byte order.
 * @since 0.1.0
 */
static void wr_be16(uint8_t* p, uint16_t v)
{
  p[k_idx_b0] = (uint8_t)((v >> k_shift_byte) & k_mask_byte);
  p[k_idx_b1] = (uint8_t)(v & k_mask_byte);
}

/**
 * @brief Copy ``n`` bytes from src to dst (no overlap allowed).
 *
 * @param[out] dst Destination buffer.
 * @param[in]  src Source buffer.
 * @param[in]  n   Byte count.
 *
 * @pre dst and src do not alias.
 * @pre Both buffers cover at least n bytes.
 * @post n bytes have been copied.
 * @since 0.1.0
 */
static void mem_copy(uint8_t* dst, const uint8_t* src, uint16_t n)
{
  for (uint16_t i = 0U; i < n; i++) {
    dst[i] = src[i];
  }
}

/**
 * @brief Compute an RFC 1071 internet checksum.
 *
 * @param[in] buf Bytes to checksum.
 * @param[in] len Length in bytes.
 * @return 16-bit network-byte-order ready checksum.
 *
 * @pre buf points to at least len bytes.
 * @post No state changes.
 * @since 0.1.0
 */
static uint16_t inet_csum(const uint8_t* buf, uint16_t len)
{
  uint32_t sum = 0U;
  uint16_t i   = 0U;
  while ((uint16_t)(i + 1U) < len) {
    sum += (uint32_t)rd_be16(&buf[i]);
    i = (uint16_t)(i + k_step_word_bytes);
  }
  if (i < len) {
    sum += ((uint32_t)buf[i]) << k_shift_byte;
  }
  while ((sum >> k_shift_word) != 0U) {
    sum = (sum & k_mask_word) + (sum >> k_shift_word);
  }
  return (uint16_t)(~sum & k_mask_word);
}

/**
 * @brief Build an Ethernet II header at the head of ``out``.
 *
 * @param[out] out         Destination frame buffer (>=14 bytes).
 * @param[in]  dst_mac     Destination MAC.
 * @param[in]  ethertype   EtherType.
 *
 * @pre out has space for k_eth_hdr_len bytes.
 * @pre dst_mac points to 6 readable bytes.
 * @post First 14 bytes of out are an Ethernet II header.
 *
 * @since 0.1.0
 */
static void build_eth_hdr(uint8_t* out, const uint8_t* dst_mac, uint16_t ethertype)
{
  mem_copy(&out[k_eth_off_dst_mac], dst_mac, k_eth_mac_len);
  mem_copy(&out[k_eth_off_src_mac], k_my_mac, k_eth_mac_len);
  wr_be16(&out[k_eth_off_ethertype], ethertype);
}

/**
 * @brief Build an IPv4 header at offset k_eth_hdr_len in ``out``.
 *
 * @param[out] out      Frame buffer.
 * @param[in]  dst_ip   Destination IPv4 address.
 * @param[in]  proto    IPv4 Protocol field.
 * @param[in]  payload_len Bytes of payload (after IPv4 header).
 *
 * @pre out has space for at least 14 + 20 + payload_len bytes.
 * @pre dst_ip points to 4 readable bytes.
 * @post Bytes 14..33 of out are a valid IPv4 header with checksum.
 *
 * @since 0.1.0
 */
static void build_ipv4_hdr(uint8_t* out, const uint8_t* dst_ip, uint8_t proto, uint16_t payload_len)
{
  uint8_t* ip            = &out[k_eth_hdr_len];
  ip[k_ipv4_off_ver_ihl] = k_ipv4_ver_ihl;
  ip[k_ipv4_off_tos]     = k_ipv4_tos_default;
  wr_be16(&ip[k_ipv4_off_total], (uint16_t)(k_ipv4_hdr_len + payload_len));
  wr_be16(&ip[k_ipv4_off_id], 0U);
  wr_be16(&ip[k_ipv4_off_frag], 0U);
  ip[k_ipv4_off_ttl]   = k_ipv4_ttl_default;
  ip[k_ipv4_off_proto] = proto;
  wr_be16(&ip[k_ipv4_off_checksum], 0U);
  mem_copy(&ip[k_ipv4_off_src_ip], k_my_ip, k_ipv4_addr_len);
  mem_copy(&ip[k_ipv4_off_dst_ip], dst_ip, k_ipv4_addr_len);
  wr_be16(&ip[k_ipv4_off_checksum], inet_csum(ip, k_ipv4_hdr_len));
}

/**
 * @brief Compute the UDP checksum (pseudo-header + UDP segment) per RFC 768.
 *
 * @details
 * RFC 768 says the UDP checksum is computed over the 96-bit IPv4
 * pseudo-header (src IP, dst IP, zero, proto, UDP-length) followed by
 * the UDP header + payload. A zero result is transmitted as 0xFFFF; we
 * use that convention here to match standard sockets implementations.
 *
 * @param[in] src_ip 4-byte source IPv4.
 * @param[in] dst_ip 4-byte destination IPv4.
 * @param[in] seg    UDP header + payload (checksum field zeroed).
 * @param[in] seg_len Total segment length in bytes.
 *
 * @return 16-bit checksum to drop in the UDP header.
 *
 * @pre All pointers point to readable buffers.
 * @post No state changes.
 * @since 0.1.0
 */
static uint16_t
udp_csum(const uint8_t* src_ip, const uint8_t* dst_ip, const uint8_t* seg, uint16_t seg_len)
{
  uint8_t pseudo[k_pseudo_hdr_len] = {};
  mem_copy(&pseudo[0], src_ip, k_ipv4_addr_len);
  mem_copy(&pseudo[k_ipv4_addr_len], dst_ip, k_ipv4_addr_len);
  pseudo[k_idx_pseudo_zero]  = 0U;
  pseudo[k_idx_pseudo_proto] = k_ipv4_proto_udp;
  wr_be16(&pseudo[k_idx_pseudo_len], seg_len);

  uint32_t sum = 0U;
  for (uint8_t i = 0U; (uint8_t)(i + 1U) < k_pseudo_hdr_len; i = (uint8_t)(i + k_step_word_bytes)) {
    sum += (uint32_t)rd_be16(&pseudo[i]);
  }
  uint16_t i2 = 0U;
  while ((uint16_t)(i2 + 1U) < seg_len) {
    sum += (uint32_t)rd_be16(&seg[i2]);
    i2 = (uint16_t)(i2 + k_step_word_bytes);
  }
  if (i2 < seg_len) {
    sum += ((uint32_t)seg[i2]) << k_shift_byte;
  }
  while ((sum >> k_shift_word) != 0U) {
    sum = (sum & k_mask_word) + (sum >> k_shift_word);
  }
  uint16_t cs = (uint16_t)(~sum & k_mask_word);
  /* RFC 768: a transmitted zero checksum is replaced by all-ones. */
  if (cs == 0U) {
    cs = (uint16_t)k_mask_word;
  }
  return cs;
}

/**
 * @brief Send a frame and toggle LED2 (TX activity indicator).
 *
 * @param[in] frame Bytes to transmit (Ethernet header + payload).
 * @param[in] len   Length in bytes (60..1514).
 *
 * @return Error code from ::ra_eth_write.
 *
 * @pre ra_eth_open succeeded.
 * @pre frame != nullptr and len >= k_eth_udp_echo_min_frame.
 * @post On success the frame has been queued on the TX ring and LED2
 *       has been toggled exactly once.
 *
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t eth_udp_echo_tx(const uint8_t* frame, uint16_t len)
{
  uint16_t pad_len = len;
  if (pad_len < k_eth_udp_echo_min_frame) {
    pad_len = k_eth_udp_echo_min_frame;
  }
  const ra_err_t err = ra_eth_write(frame, (uint32_t)pad_len);
  if (err == k_ra_ok) {
    (void)ra_board_led_toggle(k_ra_board_led2);
  }
  return err;
}

/**
 * @brief Handle an incoming ARP packet -- reply if it asks for our IP.
 *
 * @param[in] frame Inbound frame (Ethernet header + ARP payload).
 * @param[in] len   Total frame length.
 *
 * @pre frame != nullptr.
 * @pre len >= k_eth_hdr_len + k_arp_payload_len.
 * @post On a request for k_my_ip, an ARP reply has been queued on TX.
 *
 * @since 0.1.0
 */
static void handle_arp(const uint8_t* frame, uint16_t len)
{
  if (len < (uint16_t)(k_eth_hdr_len + k_arp_payload_len)) {
    return;
  }
  const uint8_t* arp = &frame[k_eth_hdr_len];
  if (rd_be16(&arp[k_arp_off_oper]) != k_arp_oper_req) {
    return;
  }
  for (uint8_t i = 0U; i < k_ipv4_addr_len; i++) {
    if (arp[k_arp_off_tpa + i] != k_my_ip[i]) {
      return;
    }
  }

  eth_udp_echo_log("ra8d2: ARP request from peer\r\n");

  uint8_t reply[k_eth_hdr_len + k_arp_payload_len] = {};
  build_eth_hdr(reply, &arp[k_arp_off_sha], k_ethertype_arp);
  uint8_t* a = &reply[k_eth_hdr_len];
  wr_be16(&a[k_arp_off_htype], k_arp_htype_ether);
  wr_be16(&a[k_arp_off_ptype], k_ethertype_ipv4);
  a[k_arp_off_hlen] = k_arp_hlen_eth;
  a[k_arp_off_plen] = k_arp_plen_ipv4;
  wr_be16(&a[k_arp_off_oper], k_arp_oper_reply);
  mem_copy(&a[k_arp_off_sha], k_my_mac, k_eth_mac_len);
  mem_copy(&a[k_arp_off_spa], k_my_ip, k_ipv4_addr_len);
  mem_copy(&a[k_arp_off_tha], &arp[k_arp_off_sha], k_eth_mac_len);
  mem_copy(&a[k_arp_off_tpa], &arp[k_arp_off_spa], k_ipv4_addr_len);

  (void)eth_udp_echo_tx(reply, (uint16_t)sizeof(reply));
}

/**
 * @brief Handle an inbound ICMP datagram, reply to echo-request.
 *
 * @param[in] frame Full inbound frame.
 * @param[in] len   Frame length.
 * @param[in] ip_total IPv4 total length field.
 *
 * @pre frame holds Eth + IPv4 + ICMP at the standard offsets.
 * @pre len >= k_eth_hdr_len + ip_total.
 * @post On echo-request to k_my_ip an echo-reply is queued for TX.
 *
 * @since 0.1.0
 */
static void handle_icmp(const uint8_t* frame, uint16_t len, uint16_t ip_total)
{
  (void)len;
  const uint8_t* ip       = &frame[k_eth_hdr_len];
  const uint8_t* icmp     = &ip[k_ipv4_hdr_len];
  const uint16_t icmp_len = (uint16_t)(ip_total - k_ipv4_hdr_len);
  if (icmp_len < k_icmp_hdr_len) {
    return;
  }
  if (icmp[k_icmp_off_type] != k_icmp_type_echo_request) {
    return;
  }

  uint8_t reply[k_eth_udp_echo_frame_buf] = {};
  build_eth_hdr(reply, &frame[k_eth_off_src_mac], k_ethertype_ipv4);
  build_ipv4_hdr(reply, &ip[k_ipv4_off_src_ip], k_ipv4_proto_icmp, icmp_len);

  uint8_t* out_icmp = &reply[k_eth_hdr_len + k_ipv4_hdr_len];
  mem_copy(out_icmp, icmp, icmp_len);
  out_icmp[k_icmp_off_type] = k_icmp_type_echo_reply;
  out_icmp[k_icmp_off_code] = 0U;
  wr_be16(&out_icmp[k_icmp_off_sum], 0U);
  wr_be16(&out_icmp[k_icmp_off_sum], inet_csum(out_icmp, icmp_len));

  const uint16_t total = (uint16_t)(k_eth_hdr_len + k_ipv4_hdr_len + icmp_len);
  (void)eth_udp_echo_tx(reply, total);
}

/**
 * @brief Handle an inbound UDP datagram destined for port 7 (echo).
 *
 * @details RFC 862 (Echo Protocol): mirror the entire payload back to
 * the sender. We swap source/destination MAC, source/destination IP,
 * source/destination port, copy the payload, and recompute the IPv4
 * and UDP checksums.
 *
 * @param[in] frame    Full inbound frame.
 * @param[in] len      Total frame length.
 * @param[in] ip_total IPv4 total length field (already validated).
 *
 * @pre frame holds Eth + IPv4 + UDP at the standard offsets.
 * @pre ip_total >= k_ipv4_hdr_len + k_udp_hdr_len.
 * @post On datagram to port 7 an echo response is queued for TX.
 *
 * @since 0.1.0
 */
static void handle_udp(const uint8_t* frame, uint16_t len, uint16_t ip_total)
{
  (void)len;
  const uint8_t* ip      = &frame[k_eth_hdr_len];
  const uint8_t* udp     = &ip[k_ipv4_hdr_len];
  const uint16_t udp_len = (uint16_t)(ip_total - k_ipv4_hdr_len);
  if (udp_len < k_udp_hdr_len) {
    return;
  }
  /* The UDP length field is independent of IPv4 total length but must
   * agree -- drop mismatches per RFC 768. */
  const uint16_t udp_hdr_length = rd_be16(&udp[k_udp_off_length]);
  if (udp_hdr_length != udp_len) {
    return;
  }
  if (rd_be16(&udp[k_udp_off_dst_port]) != k_udp_listen_port) {
    return;
  }

  const uint16_t payload_len = (uint16_t)(udp_len - k_udp_hdr_len);
  const uint16_t src_port    = rd_be16(&udp[k_udp_off_src_port]);

  eth_udp_echo_log("ra8d2: UDP echo\r\n");

  uint8_t reply[k_eth_udp_echo_frame_buf] = {};
  build_eth_hdr(reply, &frame[k_eth_off_src_mac], k_ethertype_ipv4);
  build_ipv4_hdr(reply, &ip[k_ipv4_off_src_ip], k_ipv4_proto_udp, udp_len);

  uint8_t* out_udp = &reply[k_eth_hdr_len + k_ipv4_hdr_len];
  wr_be16(&out_udp[k_udp_off_src_port], k_udp_listen_port);
  wr_be16(&out_udp[k_udp_off_dst_port], src_port);
  wr_be16(&out_udp[k_udp_off_length], udp_len);
  wr_be16(&out_udp[k_udp_off_checksum], 0U);
  mem_copy(&out_udp[k_udp_hdr_len], &udp[k_udp_hdr_len], payload_len);
  const uint8_t* peer_ip = &ip[k_ipv4_off_src_ip];
  wr_be16(&out_udp[k_udp_off_checksum], udp_csum(k_my_ip, peer_ip, out_udp, udp_len));

  const uint16_t total = (uint16_t)(k_eth_hdr_len + k_ipv4_hdr_len + udp_len);
  (void)eth_udp_echo_tx(reply, total);
}

/**
 * @brief Handle one received frame -- dispatch to ARP / ICMP / UDP.
 *
 * @param[in] frame Received frame.
 * @param[in] len   Frame length.
 *
 * @pre frame != nullptr and len >= k_eth_hdr_len.
 * @post LED1 toggled, suitable response queued (or dropped silently).
 *
 * @since 0.1.0
 */
static void handle_frame(const uint8_t* frame, uint16_t len)
{
  (void)ra_board_led_toggle(k_ra_board_led1);
  if (len < k_eth_hdr_len) {
    return;
  }
  const uint16_t etype = rd_be16(&frame[k_eth_off_ethertype]);
  if (etype == k_ethertype_arp) {
    handle_arp(frame, len);
    return;
  }
  if (etype != k_ethertype_ipv4) {
    return;
  }
  if (len < (uint16_t)(k_eth_hdr_len + k_ipv4_hdr_len)) {
    return;
  }
  const uint8_t* ip = &frame[k_eth_hdr_len];
  if ((ip[k_ipv4_off_ver_ihl] & (uint8_t)k_mask_nibble_hi) != (uint8_t)k_ipv4_ver_field) {
    return;
  }
  for (uint8_t i = 0U; i < k_ipv4_addr_len; i++) {
    if (ip[k_ipv4_off_dst_ip + i] != k_my_ip[i]) {
      return;
    }
  }
  const uint16_t ip_total = rd_be16(&ip[k_ipv4_off_total]);
  if (ip_total < k_ipv4_hdr_len || (uint16_t)(k_eth_hdr_len + ip_total) > len) {
    return;
  }
  const uint8_t proto = ip[k_ipv4_off_proto];
  if (proto == k_ipv4_proto_icmp) {
    handle_icmp(frame, len, ip_total);
  } else if (proto == k_ipv4_proto_udp) {
    handle_udp(frame, len, ip_total);
  }
  /* Silently drop TCP and everything else. */
}

/**
 * @brief Bring the chip up: CGC, time, GPIO, SCI8, RGMII pins, NIC.
 *
 * @details Panic-halts on any failure -- there is no graceful retry.
 *
 * @pre Reset_Handler has copied .data and zeroed .bss.
 * @pre SystemInit set VTOR / FPU / priority grouping.
 * @post All HAL modules required by the echo loop are running.
 *
 * @since 0.1.0
 */
static void eth_udp_echo_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;
  uint32_t pclka_hz   = 0U;
  if (ra_cgc_init() != k_ra_ok) {
    eth_udp_echo_panic_halt();
  }
  if (ra_cgc_get_clock_hz(k_ra_clock_id_cpuclk0, &cpuclk0_hz) != k_ra_ok) {
    eth_udp_echo_panic_halt();
  }
  if (ra_cgc_get_clock_hz(k_ra_clock_id_pclka, &pclka_hz) != k_ra_ok) {
    eth_udp_echo_panic_halt();
  }
  if (ra_time_init(cpuclk0_hz) != k_ra_ok) {
    eth_udp_echo_panic_halt();
  }
  if (ra_board_led_init(k_ra_board_led1) != k_ra_ok) {
    eth_udp_echo_panic_halt();
  }
  if (ra_board_led_init(k_ra_board_led2) != k_ra_ok) {
    eth_udp_echo_panic_halt();
  }

  if (ra_pfs_route_peripheral(k_pin_log_txd, k_ra_psel_sci_async, "eth_udp_echo.log_tx") !=
      k_ra_ok) {
    eth_udp_echo_panic_halt();
  }
  if (ra_pfs_route_peripheral(k_pin_log_rxd, k_ra_psel_sci_async, "eth_udp_echo.log_rx") !=
      k_ra_ok) {
    eth_udp_echo_panic_halt();
  }
  const ra_sci_cfg_t sci_cfg = {
    .baud      = k_eth_udp_echo_baud,
    .data_bits = k_ra_sci_data_8,
    .parity    = k_ra_sci_parity_none,
    .stop_bits = k_ra_sci_stop_1,
    .pclk_hz   = pclka_hz,
  };
  if (ra_sci_init((uint8_t)k_eth_udp_echo_sci_channel, &sci_cfg) != k_ra_ok) {
    eth_udp_echo_panic_halt();
  }

  if (ra_board_ethernet_init() != k_ra_ok) {
    eth_udp_echo_panic_halt();
  }

  const ra_eth_cfg_t eth_cfg = {
    .mac_address = {k_my_mac[0], k_my_mac[1], k_my_mac[2], k_my_mac[3], k_my_mac[4], k_my_mac[5]},
    .channel     = 0U,
    .num_tx_descriptors = 0U,
    .num_rx_descriptors = 0U,
    .buffer_size        = 0U,
  };
  if (ra_eth_open(&eth_cfg) != k_ra_ok) {
    eth_udp_echo_panic_halt();
  }
}

/**
 * @brief Block until the PHY reports link-up via BMSR.
 *
 * @pre ra_eth_open succeeded.
 * @post On return, the PHY has reported link-up at least once.
 *
 * @since 0.1.0
 */
static void eth_udp_echo_wait_link(void)
{
  while (1) {
    ra_eth_link_t  st  = {};
    const ra_err_t err = ra_eth_link_status(&st);
    if (err == k_ra_ok && st.link_up != 0U) {
      eth_udp_echo_log("ra8d2: link up\r\n");
      return;
    }
    ra_delay_ms(k_eth_udp_echo_link_poll_ms);
  }
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
/**
 * @brief Application entry. Brings hardware up and runs the responder.
 *
 * @return Never returns.
 *
 * @pre Reset_Handler has copied .data and zeroed .bss.
 * @pre SystemInit has set VTOR, FPU, and priority grouping.
 * @post On clean entry the CPU stays in the RX/TX loop forever.
 * @post On any HAL init failure the function halts in WFI.
 *
 * @since 0.1.0
 */
int32_t main(void)
{
  /* Suppress "address never null" by leaning on the broadcast constant. */
  (void)k_broadcast;

  eth_udp_echo_setup_or_halt();
  ra_isr_globals_enable();
  eth_udp_echo_wait_link();

  /* HIL probe banner -- parsed by scripts/hil_eth_tcp.sh (UDP variant)
   * after flashing to discover the static IPv4 address the firmware
   * responds at, plus an explicit "ready" mark so the host knows to
   * start probing. */
  eth_udp_echo_log("eth: ip=192.168.1.43 port=7 proto=udp\r\n");
  eth_udp_echo_log("eth: ready\r\n");

  uint8_t rx_frame[k_eth_udp_echo_frame_buf] = {};
  while (1) {
    uint32_t       rx_len = 0U;
    const ra_err_t err    = ra_eth_read(rx_frame, (uint32_t)k_eth_udp_echo_frame_buf, &rx_len);
    if (err == k_ra_ok && rx_len > 0U) {
      handle_frame(rx_frame, (uint16_t)rx_len);
    } else {
      ra_delay_ms(0U); /* Cooperative yield placeholder. */
    }
  }

  eth_udp_echo_panic_halt();
  return 0;
}
#pragma GCC diagnostic pop
