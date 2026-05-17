/**
 * @file examples/ek_ra8d2/ethernet_http_responder/main.c
 * @brief Minimal HTTP/1.1 GET responder for EK-RA8D2 (HIL companion app)
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
 *   - RFC 826 ARP "who-has 192.168.1.44" -> ARP reply with our MAC.
 *   - RFC 791 IPv4 / RFC 792 ICMP echo request -> ICMP echo reply.
 *   - RFC 793 TCP on port 80, single connection at a time. After the
 *     SYN handshake, the firmware does not parse the request -- it
 *     just sends a canned HTTP/1.1 200 OK with a tiny ASCII body on
 *     the first inbound segment, then closes the connection.
 *
 * The fixed response is:
 *
 * ``HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n
 *   Content-Length: 21\r\n\r\nHello from RA8D2!\r\n``
 *
 * The host probes this app with ``curl http://192.168.1.44/`` (or
 * ``hil_eth_tcp.sh`` in --mode http) and asserts the response contains
 * the marker string ``Hello from RA8D2``.
 *
 * Frame construction is byte-level by hand. Header layouts are named
 * typed enums and every checksum is recomputed.
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
 * @enum eth_http_config_t
 * @brief Top-level demo configuration constants.
 */
typedef enum : uint32_t {
  k_eth_http_baud         = 115200U,
  k_eth_http_sci_channel  = 8U,
  k_eth_http_link_poll_ms = 100U,
} eth_http_config_t;

/**
 * @enum eth_http_buf_size_t
 * @brief Frame buffer sizing constants.
 */
typedef enum : uint16_t {
  k_eth_http_frame_buf = 1536U, /**< Max raw Ethernet frame.   */
  k_eth_http_min_frame = 60U,   /**< 802.3 minimum (excl FCS). */
} eth_http_buf_size_t;

/**
 * @enum eth_hdr_layout_t
 * @brief Byte offsets inside the 14-byte Ethernet II header (RFC 894).
 */
typedef enum : uint8_t {
  k_eth_off_dst_mac   = 0U,
  k_eth_off_src_mac   = 6U,
  k_eth_off_ethertype = 12U,
  k_eth_hdr_len       = 14U,
  k_eth_mac_len       = 6U,
} eth_hdr_layout_t;

/**
 * @enum eth_ethertype_t
 * @brief EtherType values we recognise (RFC 7042).
 */
typedef enum : uint16_t {
  k_ethertype_ipv4 = 0x0800U,
  k_ethertype_arp  = 0x0806U,
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
  k_ipv4_proto_tcp   = 6U,
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
 * @enum tcp_layout_t
 * @brief Byte offsets inside the TCP header (RFC 793, no options).
 */
typedef enum : uint8_t {
  k_tcp_off_src_port = 0U,
  k_tcp_off_dst_port = 2U,
  k_tcp_off_seq      = 4U,
  k_tcp_off_ack      = 8U,
  k_tcp_off_data_off = 12U,
  k_tcp_off_flags    = 13U,
  k_tcp_off_window   = 14U,
  k_tcp_off_checksum = 16U,
  k_tcp_off_urgent   = 18U,
  k_tcp_hdr_len      = 20U,
  k_tcp_data_off_5   = 0x50U,
} tcp_layout_t;

/**
 * @enum tcp_flag_t
 * @brief TCP control-flag bit positions (RFC 793 sec 3.1).
 */
typedef enum : uint8_t {
  k_tcp_flag_fin = 0x01U,
  k_tcp_flag_syn = 0x02U,
  k_tcp_flag_rst = 0x04U,
  k_tcp_flag_psh = 0x08U,
  k_tcp_flag_ack = 0x10U,
} tcp_flag_t;

/**
 * @enum tcp_const_t
 * @brief TCP demo constants.
 */
typedef enum : uint16_t {
  k_tcp_listen_port = 80U,     /**< HTTP (RFC 9110).            */
  k_tcp_rx_window   = 1024U,   /**< Advertised window.          */
  k_tcp_initial_seq = 0x1000U, /**< Deterministic ISN.          */
} tcp_const_t;

/**
 * @enum tcp_state_t
 * @brief Minimal single-connection TCP state (subset of RFC 793).
 */
typedef enum : uint8_t {
  k_tcp_state_listen       = 0U,
  k_tcp_state_syn_received = 1U,
  k_tcp_state_established  = 2U,
  k_tcp_state_response_sent = 3U,
  k_tcp_state_last_ack     = 4U,
} tcp_state_t;

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
  k_idx_b2           = 2U,
  k_idx_b3           = 3U,
  k_idx_pseudo_zero  = 8U,
  k_idx_pseudo_proto = 9U,
  k_idx_pseudo_len   = 10U,
  k_pseudo_hdr_len   = 12U,
  k_ipv4_ver_field   = 0x40U,
  k_step_word_bytes  = 2U,
  k_step_data_off    = 4U,
  k_mask_nibble_lo   = 0x0FU,
} proto_misc_t;

/** @brief Static demo identity. */
static const uint8_t k_my_mac[k_eth_mac_len]    = {0x02U, 0x00U, 0x00U, 0x00U, 0x00U, 0x01U};
static const uint8_t k_my_ip[k_ipv4_addr_len]   = {192U, 168U, 1U, 44U};
static const uint8_t k_broadcast[k_eth_mac_len] = {0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU};

/** @brief J-Link OB CDC pinout (SCI8 / PD02 + PD03). */
static const ra_port_pin_t k_pin_log_txd =
  (ra_port_pin_t)(((uint16_t)k_ra_port_13 << 8) | (uint16_t)k_ra_pin_2);
static const ra_port_pin_t k_pin_log_rxd =
  (ra_port_pin_t)(((uint16_t)k_ra_port_13 << 8) | (uint16_t)k_ra_pin_3);

/**
 * @brief Canned HTTP/1.1 200 OK body sent on every accepted connection.
 *
 * @details
 * Hand-built so total length is known at compile time. The
 * Content-Length value must equal the length of the body that follows
 * the blank line ("Hello from RA8D2!\r\n" = 19 bytes).
 */
static const char k_http_response[] =
  "HTTP/1.1 200 OK\r\n"
  "Content-Type: text/plain\r\n"
  "Content-Length: 19\r\n"
  "Connection: close\r\n"
  "\r\n"
  "Hello from RA8D2!\r\n";

/**
 * @struct tcp_conn_t
 * @brief Single-connection TCP control block.
 */
typedef struct {
  tcp_state_t state;                    /**< Current FSM state.             */
  uint8_t     peer_mac[k_eth_mac_len];  /**< Peer's MAC for replies.        */
  uint8_t     peer_ip[k_ipv4_addr_len]; /**< Peer's IPv4 address.           */
  uint16_t    peer_port;                /**< Peer's TCP port.               */
  uint32_t    snd_nxt;                  /**< Our next SEQ number.           */
  uint32_t    rcv_nxt;                  /**< Next expected peer SEQ.        */
} tcp_conn_t;

/** @brief The single accepted TCP connection (or LISTEN). */
static tcp_conn_t s_conn = {.state = k_tcp_state_listen};

static void eth_http_panic_halt(void)
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
 * @post Bytes have been polled out of TXD8 (or discarded on backpressure).
 *
 * @since 0.1.0
 */
static void eth_http_log(const char* s)
{
  if (s == nullptr) {
    return;
  }
  uint32_t len = 0U;
  while (s[len] != '\0') {
    len++;
  }
  (void)ra_sci_write_polling((uint8_t)k_eth_http_sci_channel, (const uint8_t*)s, len);
}

/**
 * @brief Read a 16-bit big-endian word.
 *
 * @param[in] p Pointer to high byte.
 * @return Native-order value.
 *
 * @pre p covers >= 2 readable bytes.
 * @post No state changes.
 *
 * @since 0.1.0
 */
static uint16_t rd_be16(const uint8_t* p)
{
  return (uint16_t)(((uint16_t)p[k_idx_b0] << k_shift_byte) | (uint16_t)p[k_idx_b1]);
}

/**
 * @brief Read a 32-bit big-endian word.
 *
 * @param[in] p Pointer to high byte.
 * @return Native-order value.
 *
 * @pre p covers >= 4 readable bytes.
 * @post No state changes.
 *
 * @since 0.1.0
 */
static uint32_t rd_be32(const uint8_t* p)
{
  return ((uint32_t)p[k_idx_b0] << k_shift_dword) | ((uint32_t)p[k_idx_b1] << k_shift_word) |
         ((uint32_t)p[k_idx_b2] << k_shift_byte) | (uint32_t)p[k_idx_b3];
}

/**
 * @brief Write a 16-bit big-endian word.
 *
 * @param[out] p Pointer to high byte.
 * @param[in]  v Native-order value.
 *
 * @pre p covers >= 2 writable bytes.
 * @post Two bytes at p hold v in network byte order.
 *
 * @since 0.1.0
 */
static void wr_be16(uint8_t* p, uint16_t v)
{
  p[k_idx_b0] = (uint8_t)((v >> k_shift_byte) & k_mask_byte);
  p[k_idx_b1] = (uint8_t)(v & k_mask_byte);
}

/**
 * @brief Write a 32-bit big-endian word.
 *
 * @param[out] p Pointer to high byte.
 * @param[in]  v Native-order value.
 *
 * @pre p covers >= 4 writable bytes.
 * @post Four bytes at p hold v in network byte order.
 *
 * @since 0.1.0
 */
static void wr_be32(uint8_t* p, uint32_t v)
{
  p[k_idx_b0] = (uint8_t)((v >> k_shift_dword) & k_mask_byte);
  p[k_idx_b1] = (uint8_t)((v >> k_shift_word) & k_mask_byte);
  p[k_idx_b2] = (uint8_t)((v >> k_shift_byte) & k_mask_byte);
  p[k_idx_b3] = (uint8_t)(v & k_mask_byte);
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
 *
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
 * @pre buf covers at least len bytes.
 * @post No state changes.
 *
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
 * @param[out] out         Destination buffer (>= 14 bytes).
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
 * @brief Compute the TCP pseudo-header checksum per RFC 793 sec 3.1.
 *
 * @param[in] src_ip 4-byte source IPv4.
 * @param[in] dst_ip 4-byte destination IPv4.
 * @param[in] seg    TCP header + payload (checksum field zeroed).
 * @param[in] seg_len Total segment length in bytes.
 * @return 16-bit checksum to drop in the TCP header.
 *
 * @pre All pointers are readable.
 * @post No state changes.
 *
 * @since 0.1.0
 */
static uint16_t
tcp_csum(const uint8_t* src_ip, const uint8_t* dst_ip, const uint8_t* seg, uint16_t seg_len)
{
  uint8_t pseudo[k_pseudo_hdr_len] = {};
  mem_copy(&pseudo[0], src_ip, k_ipv4_addr_len);
  mem_copy(&pseudo[k_ipv4_addr_len], dst_ip, k_ipv4_addr_len);
  pseudo[k_idx_pseudo_zero]  = 0U;
  pseudo[k_idx_pseudo_proto] = k_ipv4_proto_tcp;
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
  return (uint16_t)(~sum & k_mask_word);
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
 * @pre frame != nullptr and len >= k_eth_http_min_frame.
 * @post On success the frame has been queued on the TX ring and LED2
 *       has been toggled exactly once.
 *
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t eth_http_tx(const uint8_t* frame, uint16_t len)
{
  uint16_t pad_len = len;
  if (pad_len < k_eth_http_min_frame) {
    pad_len = k_eth_http_min_frame;
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
 * @param[in] frame Inbound frame.
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

  eth_http_log("ra8d2: ARP request from peer\r\n");

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

  (void)eth_http_tx(reply, (uint16_t)sizeof(reply));
}

/**
 * @brief Handle an inbound ICMP datagram, reply to echo-request.
 *
 * @param[in] frame Full inbound frame.
 * @param[in] len   Frame length.
 * @param[in] ip_total IPv4 total length.
 *
 * @pre frame holds Eth + IPv4 + ICMP at standard offsets.
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

  uint8_t reply[k_eth_http_frame_buf] = {};
  build_eth_hdr(reply, &frame[k_eth_off_src_mac], k_ethertype_ipv4);
  build_ipv4_hdr(reply, &ip[k_ipv4_off_src_ip], k_ipv4_proto_icmp, icmp_len);

  uint8_t* out_icmp = &reply[k_eth_hdr_len + k_ipv4_hdr_len];
  mem_copy(out_icmp, icmp, icmp_len);
  out_icmp[k_icmp_off_type] = k_icmp_type_echo_reply;
  out_icmp[k_icmp_off_code] = 0U;
  wr_be16(&out_icmp[k_icmp_off_sum], 0U);
  wr_be16(&out_icmp[k_icmp_off_sum], inet_csum(out_icmp, icmp_len));

  const uint16_t total = (uint16_t)(k_eth_hdr_len + k_ipv4_hdr_len + icmp_len);
  (void)eth_http_tx(reply, total);
}

/**
 * @brief Compute the length of a NUL-terminated string at compile time.
 *
 * @details
 * Local helper to avoid pulling in ``<string.h>``. NASA Rule 2 bound is
 * a fixed upper limit -- the only NUL-terminated string in this TU is
 * the canned HTTP response which is < 256 bytes.
 *
 * @param[in] s NUL-terminated string.
 * @return Length in bytes, not counting the NUL.
 *
 * @pre s != nullptr.
 * @pre strlen(s) < 256.
 * @post No state changes.
 *
 * @since 0.1.0
 */
static uint16_t s_strlen(const char* s)
{
  uint16_t n = 0U;
  while (n < (uint16_t)0xFFU && s[n] != '\0') {
    n++;
  }
  return n;
}

/**
 * @brief Build and send a TCP segment using s_conn for addressing.
 *
 * @param[in] flags       TCP flags byte.
 * @param[in] payload     Payload bytes (may be nullptr if payload_len=0).
 * @param[in] payload_len Payload length in bytes.
 *
 * @return Error code from ::ra_eth_write.
 *
 * @pre s_conn has valid peer addressing and snd_nxt/rcv_nxt.
 * @pre payload covers payload_len bytes when payload_len > 0.
 * @post Segment queued on the TX descriptor ring.
 *
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t tcp_send(uint8_t flags, const uint8_t* payload, uint16_t payload_len)
{
  uint8_t  frame[k_eth_http_frame_buf] = {};
  uint16_t seg_len                     = (uint16_t)(k_tcp_hdr_len + payload_len);

  build_eth_hdr(frame, s_conn.peer_mac, k_ethertype_ipv4);
  build_ipv4_hdr(frame, s_conn.peer_ip, k_ipv4_proto_tcp, seg_len);

  uint8_t* tcp = &frame[k_eth_hdr_len + k_ipv4_hdr_len];
  wr_be16(&tcp[k_tcp_off_src_port], k_tcp_listen_port);
  wr_be16(&tcp[k_tcp_off_dst_port], s_conn.peer_port);
  wr_be32(&tcp[k_tcp_off_seq], s_conn.snd_nxt);
  wr_be32(&tcp[k_tcp_off_ack], s_conn.rcv_nxt);
  tcp[k_tcp_off_data_off] = k_tcp_data_off_5;
  tcp[k_tcp_off_flags]    = flags;
  wr_be16(&tcp[k_tcp_off_window], k_tcp_rx_window);
  wr_be16(&tcp[k_tcp_off_checksum], 0U);
  wr_be16(&tcp[k_tcp_off_urgent], 0U);
  if (payload_len > 0U) {
    mem_copy(&tcp[k_tcp_hdr_len], payload, payload_len);
  }
  wr_be16(&tcp[k_tcp_off_checksum], tcp_csum(k_my_ip, s_conn.peer_ip, tcp, seg_len));

  return eth_http_tx(frame, (uint16_t)(k_eth_hdr_len + k_ipv4_hdr_len + seg_len));
}

/**
 * @struct tcp_seg_t
 * @brief Parsed TCP segment fields.
 */
typedef struct {
  const uint8_t* frame;
  const uint8_t* ip;
  const uint8_t* payload;
  uint16_t       payload_len;
  uint16_t       src_port;
  uint32_t       seq;
  uint32_t       ack;
  uint8_t        flags;
} tcp_seg_t;

/**
 * @brief Handle a SYN landing in LISTEN -- transition to SYN-RECEIVED.
 *
 * @param[in] s Parsed segment.
 *
 * @pre s_conn.state == k_tcp_state_listen.
 * @pre s != nullptr.
 * @post s_conn populated; SYN-ACK queued; snd_nxt advanced past the SYN.
 *
 * @since 0.1.0
 */
static void tcp_on_syn(const tcp_seg_t* s)
{
  eth_http_log("ra8d2: HTTP connection from peer\r\n");
  mem_copy(s_conn.peer_mac, &s->frame[k_eth_off_src_mac], k_eth_mac_len);
  mem_copy(s_conn.peer_ip, &s->ip[k_ipv4_off_src_ip], k_ipv4_addr_len);
  s_conn.peer_port = s->src_port;
  s_conn.rcv_nxt   = s->seq + 1U;
  s_conn.snd_nxt   = (uint32_t)k_tcp_initial_seq;
  s_conn.state     = k_tcp_state_syn_received;
  (void)tcp_send((uint8_t)(k_tcp_flag_syn | k_tcp_flag_ack), nullptr, 0U);
  s_conn.snd_nxt += 1U;
}

/**
 * @brief Send the canned HTTP response then queue our FIN.
 *
 * @param[in] s Parsed segment that delivered the request bytes.
 *
 * @pre s_conn.state == k_tcp_state_established.
 * @pre s->payload covers s->payload_len readable bytes.
 * @post Response sent; FIN queued; state advanced to RESPONSE_SENT.
 *
 * @since 0.1.0
 */
static void tcp_on_data(const tcp_seg_t* s)
{
  s_conn.rcv_nxt = s->seq + (uint32_t)s->payload_len;

  const uint16_t resp_len = s_strlen(k_http_response);
  (void)tcp_send((uint8_t)(k_tcp_flag_ack | k_tcp_flag_psh),
                 (const uint8_t*)k_http_response,
                 resp_len);
  s_conn.snd_nxt += (uint32_t)resp_len;

  /* Immediately queue our FIN -- we are server-side close. */
  (void)tcp_send((uint8_t)(k_tcp_flag_ack | k_tcp_flag_fin), nullptr, 0U);
  s_conn.snd_nxt += 1U;
  s_conn.state = k_tcp_state_response_sent;
  eth_http_log("ra8d2: HTTP 200 sent\r\n");
}

/**
 * @brief Handle an inbound FIN -- queue FIN-ACK and enter LAST-ACK.
 *
 * @param[in] s Parsed segment.
 *
 * @pre s_conn.state != k_tcp_state_listen.
 * @post s_conn.state == k_tcp_state_last_ack on a fresh FIN; otherwise
 *       reset to LISTEN.
 *
 * @since 0.1.0
 */
static void tcp_on_fin(const tcp_seg_t* s)
{
  s_conn.rcv_nxt = s->seq + (uint32_t)s->payload_len + 1U;
  /* If we have not yet sent our FIN, queue one now; otherwise this is
   * the peer ACKing both FINs and we can return to LISTEN. */
  if (s_conn.state == k_tcp_state_response_sent) {
    s_conn.state = k_tcp_state_last_ack;
  } else {
    (void)tcp_send((uint8_t)(k_tcp_flag_ack | k_tcp_flag_fin), nullptr, 0U);
    s_conn.snd_nxt += 1U;
    s_conn.state = k_tcp_state_last_ack;
  }
}

/**
 * @brief Run the FSM transitions for a parsed segment.
 *
 * @param[in] s Parsed segment.
 *
 * @pre s != nullptr.
 * @post s_conn.state advances per the RFC 793 subset:
 *         LISTEN -> SYN-RECEIVED -> ESTABLISHED -> RESPONSE_SENT ->
 *         LAST-ACK -> LISTEN.
 *
 * @since 0.1.0
 */
static void tcp_step_fsm(const tcp_seg_t* s)
{
  if ((s->flags & k_tcp_flag_rst) != 0U) {
    s_conn.state = k_tcp_state_listen;
    return;
  }
  if ((s->flags & k_tcp_flag_syn) != 0U && s_conn.state == k_tcp_state_listen) {
    tcp_on_syn(s);
    return;
  }
  if ((s->flags & k_tcp_flag_ack) != 0U && s_conn.state == k_tcp_state_syn_received) {
    s_conn.state = k_tcp_state_established;
  }
  if (s_conn.state == k_tcp_state_established && s->payload_len > 0U) {
    tcp_on_data(s);
  }
  if ((s->flags & k_tcp_flag_fin) != 0U && s_conn.state != k_tcp_state_listen &&
      s_conn.state != k_tcp_state_last_ack) {
    tcp_on_fin(s);
    return;
  }
  if (s_conn.state == k_tcp_state_last_ack && (s->flags & k_tcp_flag_ack) != 0U &&
      s->ack == s_conn.snd_nxt) {
    s_conn.state = k_tcp_state_listen;
  }
}

/**
 * @brief Handle an inbound TCP segment (single-connection FSM).
 *
 * @param[in] frame    Inbound frame.
 * @param[in] len      Frame length.
 * @param[in] ip_total IPv4 total length field.
 *
 * @pre frame holds Eth + IPv4 + TCP at standard offsets.
 * @pre ip_total >= k_ipv4_hdr_len + k_tcp_hdr_len.
 * @post s_conn.state advances per the RFC 793 subset.
 *
 * @since 0.1.0
 */
static void handle_tcp(const uint8_t* frame, uint16_t len, uint16_t ip_total)
{
  (void)len;
  const uint8_t* ip        = &frame[k_eth_hdr_len];
  const uint8_t* tcp       = &ip[k_ipv4_hdr_len];
  const uint16_t tcp_total = (uint16_t)(ip_total - k_ipv4_hdr_len);
  if (tcp_total < k_tcp_hdr_len) {
    return;
  }
  if (rd_be16(&tcp[k_tcp_off_dst_port]) != k_tcp_listen_port) {
    return;
  }

  const uint8_t data_off_words =
    (uint8_t)((tcp[k_tcp_off_data_off] >> k_shift_nibble_hi) & (uint8_t)k_mask_nibble_lo);
  const uint16_t hdr_bytes = (uint16_t)((uint16_t)data_off_words * (uint16_t)k_step_data_off);
  if (hdr_bytes < k_tcp_hdr_len || hdr_bytes > tcp_total) {
    return;
  }

  const tcp_seg_t s = {
    .frame       = frame,
    .ip          = ip,
    .payload     = &tcp[hdr_bytes],
    .payload_len = (uint16_t)(tcp_total - hdr_bytes),
    .src_port    = rd_be16(&tcp[k_tcp_off_src_port]),
    .seq         = rd_be32(&tcp[k_tcp_off_seq]),
    .ack         = rd_be32(&tcp[k_tcp_off_ack]),
    .flags       = tcp[k_tcp_off_flags],
  };
  tcp_step_fsm(&s);
}

/**
 * @brief Handle one received frame -- dispatch to ARP / ICMP / TCP.
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
  } else if (proto == k_ipv4_proto_tcp) {
    handle_tcp(frame, len, ip_total);
  }
  /* Silently drop UDP and everything else. */
}

/**
 * @brief Bring the chip up: CGC, time, GPIO, SCI8, RGMII pins, NIC.
 *
 * @pre Reset_Handler has copied .data and zeroed .bss.
 * @pre SystemInit set VTOR / FPU / priority grouping.
 * @post All HAL modules required by the responder are running.
 *
 * @since 0.1.0
 */
static void eth_http_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;
  uint32_t pclka_hz   = 0U;
  if (ra_cgc_init() != k_ra_ok) {
    eth_http_panic_halt();
  }
  if (ra_cgc_get_clock_hz(k_ra_clock_id_cpuclk0, &cpuclk0_hz) != k_ra_ok) {
    eth_http_panic_halt();
  }
  if (ra_cgc_get_clock_hz(k_ra_clock_id_pclka, &pclka_hz) != k_ra_ok) {
    eth_http_panic_halt();
  }
  if (ra_time_init(cpuclk0_hz) != k_ra_ok) {
    eth_http_panic_halt();
  }
  if (ra_board_led_init(k_ra_board_led1) != k_ra_ok) {
    eth_http_panic_halt();
  }
  if (ra_board_led_init(k_ra_board_led2) != k_ra_ok) {
    eth_http_panic_halt();
  }

  if (ra_pfs_route_peripheral(k_pin_log_txd, k_ra_psel_sci_async, "eth_http.log_tx") != k_ra_ok) {
    eth_http_panic_halt();
  }
  if (ra_pfs_route_peripheral(k_pin_log_rxd, k_ra_psel_sci_async, "eth_http.log_rx") != k_ra_ok) {
    eth_http_panic_halt();
  }
  const ra_sci_cfg_t sci_cfg = {
    .baud      = k_eth_http_baud,
    .data_bits = k_ra_sci_data_8,
    .parity    = k_ra_sci_parity_none,
    .stop_bits = k_ra_sci_stop_1,
    .pclk_hz   = pclka_hz,
  };
  if (ra_sci_init((uint8_t)k_eth_http_sci_channel, &sci_cfg) != k_ra_ok) {
    eth_http_panic_halt();
  }

  if (ra_board_ethernet_init() != k_ra_ok) {
    eth_http_panic_halt();
  }

  const ra_eth_cfg_t eth_cfg = {
    .mac_address = {k_my_mac[0], k_my_mac[1], k_my_mac[2], k_my_mac[3], k_my_mac[4], k_my_mac[5]},
    .channel     = 0U,
    .num_tx_descriptors = 0U,
    .num_rx_descriptors = 0U,
    .buffer_size        = 0U,
  };
  if (ra_eth_open(&eth_cfg) != k_ra_ok) {
    eth_http_panic_halt();
  }
}

/**
 * @brief Block until the PHY reports link-up via BMSR.
 *
 * @pre ra_eth_open succeeded.
 * @post On return, PHY reports link-up at least once.
 *
 * @since 0.1.0
 */
static void eth_http_wait_link(void)
{
  while (1) {
    ra_eth_link_t  st  = {};
    const ra_err_t err = ra_eth_link_status(&st);
    if (err == k_ra_ok && st.link_up != 0U) {
      eth_http_log("ra8d2: link up\r\n");
      return;
    }
    ra_delay_ms(k_eth_http_link_poll_ms);
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
  (void)k_broadcast;

  eth_http_setup_or_halt();
  ra_isr_globals_enable();
  eth_http_wait_link();

  /* HIL probe banner. */
  eth_http_log("eth: ip=192.168.1.44 port=80 proto=http\r\n");
  eth_http_log("eth: ready\r\n");

  uint8_t rx_frame[k_eth_http_frame_buf] = {};
  while (1) {
    uint32_t       rx_len = 0U;
    const ra_err_t err    = ra_eth_read(rx_frame, (uint32_t)k_eth_http_frame_buf, &rx_len);
    if (err == k_ra_ok && rx_len > 0U) {
      handle_frame(rx_frame, (uint16_t)rx_len);
    } else {
      ra_delay_ms(0U);
    }
  }

  eth_http_panic_halt();
  return 0;
}
#pragma GCC diagnostic pop
