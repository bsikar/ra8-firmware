/**
 * @file examples/ek_ra8d2/ethernet_tcp_echo/main.c
 * @brief Ethernet TCP echo responder for EK-RA8D2 (sweep-2 ra_eth ring exercise)
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
 *   - RFC 826 ARP "who-has 192.168.1.42" -> ARP reply with our MAC.
 *   - RFC 791 IPv4 / RFC 792 ICMP echo request -> ICMP echo reply with
 *     payload mirrored back, type/code rewritten and checksum redone.
 *   - RFC 793 TCP, single connection at a time, listening on port 7
 *     (Echo Protocol). The state machine answers SYN with SYN-ACK,
 *     mirrors any data segment payload back as ACK + PSH, and closes
 *     gracefully on FIN with FIN-ACK -> ACK.
 *
 * No lwIP -- frame construction is byte-level by hand. Header layouts
 * are described in named typed enums (no magic numbers) and every
 * checksum is recomputed (IPv4 header, ICMP, TCP pseudo-header).
 *
 * ## Ethernet pinout
 *
 * The EK-RA8D2 v1 routes the on-chip RMAC through the on-board PEF7071
 * PHY in **RGMII** mode (UM Table 26 p 33). The full sixteen-pin RGMII
 * map (ETH_TXC, ETH_TX_CTL, ETH_TXD0..3, ETH_RXC, ETH_RX_CTL,
 * ETH_RXD0..3, ETH_MDC, ETH_MDIO, plus PHY reset / interrupt / power
 * lines) is owned by ``ra_board_ethernet_init()`` so this app does not
 * touch IOPORT directly. See ``libs/ra_board_ek_ra8d2`` for the
 * authoritative pin list.
 *
 * @author Brighton Sikarskie
 * @date 2026-04-29
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
 * @enum eth_tcp_echo_config_t
 * @brief Top-level demo configuration constants.
 *
 * @details
 * Every numeric used by the bring-up path appears here so the magic-
 * number lint never sees a bare integer. The SCI8 channel matches the
 * on-board J-Link OB CDC bridge pins (PD_02 / PD_03).
 */
typedef enum : uint32_t {
  k_eth_tcp_echo_baud         = 115200U, /**< J-Link OB CDC baud.     */
  k_eth_tcp_echo_sci_channel  = 8U,      /**< SCI8 logging channel.   */
  k_eth_tcp_echo_link_poll_ms = 100U,    /**< PHY BMSR poll period.   */
  k_eth_tcp_echo_idle_us      = 200U,    /**< Idle wait between RX.   */
} eth_tcp_echo_config_t;

/**
 * @enum eth_tcp_echo_buf_size_t
 * @brief Frame buffer sizing constants.
 */
typedef enum : uint16_t {
  k_eth_tcp_echo_frame_buf = 1536U, /**< Max raw Ethernet frame.   */
  k_eth_tcp_echo_min_frame = 60U,   /**< 802.3 minimum (excl FCS). */
} eth_tcp_echo_buf_size_t;

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
 *
 * @details Field order: HTYPE / PTYPE / HLEN / PLEN / OPER / SHA / SPA
 * / THA / TPA. All multi-byte fields are big-endian.
 */
typedef enum : uint8_t {
  k_arp_off_htype   = 0U,  /**< Hardware type (Ethernet=1). */
  k_arp_off_ptype   = 2U,  /**< Protocol type (IPv4=0x0800).*/
  k_arp_off_hlen    = 4U,  /**< Hardware addr length (6).   */
  k_arp_off_plen    = 5U,  /**< Protocol addr length (4).   */
  k_arp_off_oper    = 6U,  /**< Operation (1=req, 2=reply). */
  k_arp_off_sha     = 8U,  /**< Sender hardware addr.       */
  k_arp_off_spa     = 14U, /**< Sender protocol addr.       */
  k_arp_off_tha     = 18U, /**< Target hardware addr.       */
  k_arp_off_tpa     = 24U, /**< Target protocol addr.       */
  k_arp_payload_len = 28U, /**< Total ARP-over-Eth payload. */
} arp_layout_t;

/**
 * @enum arp_const_t
 * @brief ARP fixed-field values.
 */
typedef enum : uint16_t {
  k_arp_htype_ether = 1U, /**< Hardware = Ethernet.     */
  k_arp_oper_req    = 1U, /**< OPER = Request.          */
  k_arp_oper_reply  = 2U, /**< OPER = Reply.            */
  k_arp_hlen_eth    = 6U, /**< MAC addr length.         */
  k_arp_plen_ipv4   = 4U, /**< IPv4 addr length.        */
} arp_const_t;

/**
 * @enum ipv4_layout_t
 * @brief Byte offsets inside an IPv4 header (RFC 791, no options).
 */
typedef enum : uint8_t {
  k_ipv4_off_ver_ihl  = 0U,  /**< Version (4 hi) | IHL (4 lo). */
  k_ipv4_off_tos      = 1U,  /**< Type of service / DSCP.      */
  k_ipv4_off_total    = 2U,  /**< Total length BE.             */
  k_ipv4_off_id       = 4U,  /**< Identification BE.           */
  k_ipv4_off_frag     = 6U,  /**< Flags + fragment offset.     */
  k_ipv4_off_ttl      = 8U,  /**< Time to live.                */
  k_ipv4_off_proto    = 9U,  /**< Protocol (1=ICMP, 6=TCP).    */
  k_ipv4_off_checksum = 10U, /**< Header checksum BE.          */
  k_ipv4_off_src_ip   = 12U, /**< Source IPv4.                 */
  k_ipv4_off_dst_ip   = 16U, /**< Destination IPv4.            */
  k_ipv4_hdr_len      = 20U, /**< Min IPv4 header (no opts).   */
} ipv4_layout_t;

/**
 * @enum ipv4_const_t
 * @brief IPv4 fixed-field values used by the responder.
 */
typedef enum : uint8_t {
  k_ipv4_ver_ihl     = 0x45U, /**< IPv4, IHL=5 (no options).   */
  k_ipv4_tos_default = 0x00U, /**< Routine traffic.            */
  k_ipv4_ttl_default = 64U,   /**< Default TTL per RFC 1700.   */
  k_ipv4_proto_icmp  = 1U,    /**< ICMP (RFC 792).             */
  k_ipv4_proto_tcp   = 6U,    /**< TCP (RFC 793).              */
  k_ipv4_addr_len    = 4U,    /**< Octets in an IPv4 address.  */
} ipv4_const_t;

/**
 * @enum icmp_layout_t
 * @brief Byte offsets inside the ICMP header (RFC 792).
 */
typedef enum : uint8_t {
  k_icmp_off_type = 0U, /**< Type field.                   */
  k_icmp_off_code = 1U, /**< Code field.                   */
  k_icmp_off_sum  = 2U, /**< Checksum BE over header+data. */
  k_icmp_off_id   = 4U, /**< Identifier BE (echo).         */
  k_icmp_off_seq  = 6U, /**< Sequence BE (echo).           */
  k_icmp_hdr_len  = 8U, /**< Header length (incl id/seq).  */
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
  k_tcp_off_src_port = 0U,    /**< Source port BE.        */
  k_tcp_off_dst_port = 2U,    /**< Destination port BE.   */
  k_tcp_off_seq      = 4U,    /**< Sequence number BE.    */
  k_tcp_off_ack      = 8U,    /**< Acknowledgement BE.    */
  k_tcp_off_data_off = 12U,   /**< Data offset (4 hi).    */
  k_tcp_off_flags    = 13U,   /**< Control flags.         */
  k_tcp_off_window   = 14U,   /**< Window size BE.        */
  k_tcp_off_checksum = 16U,   /**< Checksum BE.           */
  k_tcp_off_urgent   = 18U,   /**< Urgent pointer BE.     */
  k_tcp_hdr_len      = 20U,   /**< Min TCP header.        */
  k_tcp_data_off_5   = 0x50U, /**< 5x32-bit words = 20. */
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
  k_tcp_listen_port = 7U,      /**< Echo Protocol (RFC 862).    */
  k_tcp_rx_window   = 1024U,   /**< Advertised window.          */
  k_tcp_initial_seq = 0x1000U, /**< Our deterministic ISN.      */
} tcp_const_t;

/**
 * @enum tcp_state_t
 * @brief Minimal single-connection TCP state (subset of RFC 793).
 */
typedef enum : uint8_t {
  k_tcp_state_listen       = 0U,
  k_tcp_state_syn_received = 1U,
  k_tcp_state_established  = 2U,
  k_tcp_state_close_wait   = 3U,
  k_tcp_state_last_ack     = 4U,
} tcp_state_t;

/**
 * @enum proto_misc_t
 * @brief Misc protocol constants (bit shifts, masks, byte indices).
 */
typedef enum : uint32_t {
  k_shift_byte       = 8U,      /**< 8-bit shift.                       */
  k_shift_word       = 16U,     /**< 16-bit shift.                      */
  k_shift_dword      = 24U,     /**< 24-bit shift.                      */
  k_shift_nibble_hi  = 4U,      /**< High-nibble shift (data offset).   */
  k_mask_byte        = 0xFFU,   /**< Low-byte mask.                     */
  k_mask_word        = 0xFFFFU, /**< Low-word mask.                     */
  k_mask_nibble_hi   = 0xF0U,   /**< High-nibble mask.                  */
  k_idx_b0           = 0U,      /**< Byte index 0.                      */
  k_idx_b1           = 1U,      /**< Byte index 1.                      */
  k_idx_b2           = 2U,      /**< Byte index 2.                      */
  k_idx_b3           = 3U,      /**< Byte index 3.                      */
  k_idx_pseudo_zero  = 8U,      /**< TCP pseudo-header zero byte index. */
  k_idx_pseudo_proto = 9U,      /**< TCP pseudo-header proto-byte index.*/
  k_idx_pseudo_len   = 10U,     /**< TCP pseudo-header seg-len offset.  */
  k_pseudo_hdr_len   = 12U,     /**< IPv4 TCP pseudo-header bytes.      */
  k_ipv4_ver_field   = 0x40U,   /**< Version-4 nibble (top nibble).     */
  k_step_word_bytes  = 2U,      /**< Bytes per 16-bit checksum word.    */
  k_step_data_off    = 4U,      /**< Bytes per TCP data-offset word.    */
  k_mask_nibble_lo   = 0x0FU,   /**< Low-nibble mask.                   */
} proto_misc_t;

/**
 * @brief Static demo identity: MAC, IPv4, listen port (config block).
 *
 * @details Locally-administered unicast MAC (bit 1 of first octet=1,
 * bit 0=0). IPv4 192.168.1.42 / 24, gateway 192.168.1.1 -- the gateway
 * is recorded for completeness but the responder never originates a
 * routed packet, so no ARP-for-gateway is performed.
 */
static const uint8_t k_my_mac[k_eth_mac_len]    = {0x02U, 0x00U, 0x00U, 0x00U, 0x00U, 0x01U};
static const uint8_t k_my_ip[k_ipv4_addr_len]   = {192U, 168U, 1U, 42U};
static const uint8_t k_broadcast[k_eth_mac_len] = {0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU};

/** @brief Pinout for the on-board J-Link OB CDC channel (SCI8 / PD02 + PD03). */
static const ra_port_pin_t k_pin_log_txd =
  (ra_port_pin_t)(((uint16_t)k_ra_port_13 << 8) | (uint16_t)k_ra_pin_2);
static const ra_port_pin_t k_pin_log_rxd =
  (ra_port_pin_t)(((uint16_t)k_ra_port_13 << 8) | (uint16_t)k_ra_pin_3);

/**
 * @struct tcp_conn_t
 * @brief Single-connection TCP control block.
 *
 * @details The responder accepts exactly one TCP client at a time
 * (RFC 793 "passive open" + a tiny FSM). Sequence numbers track the
 * usual SND/RCV variables: ``snd_nxt`` is what we will put in the
 * next outgoing SEQ field, ``rcv_nxt`` is what we expect from the peer.
 */
typedef struct {
  tcp_state_t state;                    /**< Current FSM state.             */
  uint8_t     peer_mac[k_eth_mac_len];  /**< Peer's MAC for replies.        */
  uint8_t     peer_ip[k_ipv4_addr_len]; /**< Peer's IPv4 address.           */
  uint16_t    peer_port;                /**< Peer's TCP port.               */
  uint32_t    snd_nxt;                  /**< Our next SEQ number.           */
  uint32_t    rcv_nxt;                  /**< Next expected peer SEQ.        */
} tcp_conn_t;

/**
 * @var s_conn
 * @brief The single accepted TCP connection (or LISTEN).
 *
 * @details Initialized to LISTEN; reset back to LISTEN whenever the
 * peer sends FIN / RST or a new SYN preempts the existing flow.
 */
static tcp_conn_t s_conn = {.state = k_tcp_state_listen};

/**
 * @brief Park the CPU forever in WFI on fatal init failure.
 *
 * @pre Called only after a fatal error in boot.
 * @pre IRQs may or may not be enabled (we do not require any state).
 *
 * @post CPU is parked; only a debugger or external reset wakes it.
 *
 * @since 0.1.0
 */
static void eth_tcp_echo_panic_halt(void)
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
 *
 * @post Bytes have been polled out of TXD8 (or the call silently
 *       discarded them on backpressure -- this is logging only).
 *
 * @since 0.1.0
 */
static void eth_tcp_echo_log(const char* s)
{
  if (s == nullptr) {
    return;
  }
  uint32_t len = 0U;
  while (s[len] != '\0') {
    len++;
  }
  (void)ra_sci_write_polling((uint8_t)k_eth_tcp_echo_sci_channel, (const uint8_t*)s, len);
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
 * @brief Read a 32-bit big-endian word from a packet buffer.
 *
 * @param[in] p Pointer to the high byte.
 * @return The native-order 32-bit value.
 *
 * @pre p points to at least 4 readable bytes.
 * @post No state changes.
 * @since 0.1.0
 */
static uint32_t rd_be32(const uint8_t* p)
{
  return ((uint32_t)p[k_idx_b0] << k_shift_dword) | ((uint32_t)p[k_idx_b1] << k_shift_word) |
         ((uint32_t)p[k_idx_b2] << k_shift_byte) | (uint32_t)p[k_idx_b3];
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
 * @brief Write a 32-bit big-endian word to a packet buffer.
 *
 * @param[out] p Pointer to the high byte.
 * @param[in]  v Native-order 32-bit value.
 *
 * @pre p points to at least 4 writable bytes.
 * @post Four bytes at p hold v in network byte order.
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
 * @details Folds the 1's-complement sum of all 16-bit words in ``buf``
 * into a 16-bit value, then returns its 1's complement. Trailing odd
 * byte (if any) is treated as the high half of a final 16-bit word.
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
 * @param[out] out      Frame buffer (>= 14 + 20 bytes).
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
 * @brief Compute the TCP checksum over the pseudo-header + segment.
 *
 * @details RFC 793 sec 3.1: 96-bit pseudo-header (src IP, dst IP, zero,
 * proto, TCP-length) prepended logically to the segment.
 *
 * @param[in] src_ip 4-byte source IPv4.
 * @param[in] dst_ip 4-byte destination IPv4.
 * @param[in] seg    TCP header + payload bytes (in network order, with
 *                   the checksum field already zeroed).
 * @param[in] seg_len Total segment length in bytes.
 *
 * @return 16-bit checksum to drop in the TCP header.
 *
 * @pre All pointers point to readable buffers.
 * @post No state changes.
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

  /* Sum pseudo-header + segment. We can't concatenate without a
   * scratch buffer, so we sum the two regions and fold once. */
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
 * @pre frame != nullptr and len >= k_eth_tcp_echo_min_frame.
 * @post On success the frame has been queued on the TX ring and LED2
 *       has been toggled exactly once.
 *
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t eth_tcp_echo_tx(const uint8_t* frame, uint16_t len)
{
  uint16_t pad_len = len;
  if (pad_len < k_eth_tcp_echo_min_frame) {
    pad_len = k_eth_tcp_echo_min_frame;
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
  /* Target Protocol Address must equal our IP. */
  for (uint8_t i = 0U; i < k_ipv4_addr_len; i++) {
    if (arp[k_arp_off_tpa + i] != k_my_ip[i]) {
      return;
    }
  }

  eth_tcp_echo_log("ra8d2: ARP request from peer\r\n");

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

  (void)eth_tcp_echo_tx(reply, (uint16_t)sizeof(reply));
}

/**
 * @brief Handle an inbound ICMP datagram, reply to echo-request.
 *
 * @param[in] frame Full inbound frame.
 * @param[in] len   Frame length.
 * @param[in] ip_total IPv4 total length field (already validated).
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

  uint8_t reply[k_eth_tcp_echo_frame_buf] = {};
  build_eth_hdr(reply, &frame[k_eth_off_src_mac], k_ethertype_ipv4);
  build_ipv4_hdr(reply, &ip[k_ipv4_off_src_ip], k_ipv4_proto_icmp, icmp_len);

  uint8_t* out_icmp = &reply[k_eth_hdr_len + k_ipv4_hdr_len];
  mem_copy(out_icmp, icmp, icmp_len);
  out_icmp[k_icmp_off_type] = k_icmp_type_echo_reply;
  out_icmp[k_icmp_off_code] = 0U;
  wr_be16(&out_icmp[k_icmp_off_sum], 0U);
  wr_be16(&out_icmp[k_icmp_off_sum], inet_csum(out_icmp, icmp_len));

  const uint16_t total = (uint16_t)(k_eth_hdr_len + k_ipv4_hdr_len + icmp_len);
  (void)eth_tcp_echo_tx(reply, total);
}

/**
 * @brief Build and send a TCP segment using s_conn for addressing.
 *
 * @param[in] flags   TCP flags byte.
 * @param[in] payload Payload bytes (may be nullptr if payload_len=0).
 * @param[in] payload_len Payload length in bytes.
 *
 * @return Error code from ::ra_eth_write.
 *
 * @pre s_conn has valid peer_mac/peer_ip/peer_port and snd_nxt/rcv_nxt.
 * @pre payload covers at least payload_len bytes if payload_len > 0.
 * @post The segment is queued on the TX descriptor ring.
 *
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t tcp_send(uint8_t flags, const uint8_t* payload, uint16_t payload_len)
{
  uint8_t  frame[k_eth_tcp_echo_frame_buf] = {};
  uint16_t seg_len                         = (uint16_t)(k_tcp_hdr_len + payload_len);

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

  return eth_tcp_echo_tx(frame, (uint16_t)(k_eth_hdr_len + k_ipv4_hdr_len + seg_len));
}

/**
 * @struct tcp_seg_t
 * @brief Parsed TCP segment fields used by the dispatch helpers.
 *
 * @details Lifts hdr/payload pointers and decoded SEQ/ACK/flags out of
 * ::handle_tcp so each branch fits inside the NASA-rule-4 line budget.
 */
typedef struct {
  const uint8_t* frame;       /**< Original frame pointer.       */
  const uint8_t* ip;          /**< Pointer to IPv4 header.       */
  const uint8_t* payload;     /**< Pointer to TCP payload bytes. */
  uint16_t       payload_len; /**< Payload length in bytes.      */
  uint16_t       src_port;    /**< Peer's TCP source port.       */
  uint32_t       seq;         /**< Peer's SEQ.                   */
  uint32_t       ack;         /**< Peer's ACK.                   */
  uint8_t        flags;       /**< Control-flag byte.            */
} tcp_seg_t;

/**
 * @brief Handle a SYN landing in LISTEN -- transition to SYN-RECEIVED.
 *
 * @param[in] s Parsed segment.
 *
 * @pre s_conn.state == k_tcp_state_listen.
 * @pre s != nullptr.
 * @post s_conn populated with peer addressing, SYN-ACK queued.
 * @post s_conn.snd_nxt advanced past the synthetic SYN byte.
 *
 * @since 0.1.0
 */
static void tcp_on_syn(const tcp_seg_t* s)
{
  eth_tcp_echo_log("ra8d2: TCP connection from peer\r\n");
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
 * @brief Echo data on an ESTABLISHED connection.
 *
 * @param[in] s Parsed segment with payload_len > 0.
 *
 * @pre s_conn.state == k_tcp_state_established.
 * @pre s->payload covers s->payload_len readable bytes.
 * @post Payload echoed, snd_nxt and rcv_nxt advanced.
 *
 * @since 0.1.0
 */
static void tcp_on_data(const tcp_seg_t* s)
{
  s_conn.rcv_nxt = s->seq + (uint32_t)s->payload_len;
  (void)tcp_send((uint8_t)(k_tcp_flag_ack | k_tcp_flag_psh), s->payload, s->payload_len);
  s_conn.snd_nxt += (uint32_t)s->payload_len;
  eth_tcp_echo_log("ra8d2: echoed bytes\r\n");
}

/**
 * @brief Handle an inbound FIN -- queue FIN-ACK and enter LAST-ACK.
 *
 * @param[in] s Parsed segment.
 *
 * @pre s_conn.state in {ESTABLISHED, SYN-RECEIVED}.
 * @post s_conn.state == k_tcp_state_last_ack.
 * @post FIN-ACK queued, snd_nxt advanced past the synthetic FIN byte.
 *
 * @since 0.1.0
 */
static void tcp_on_fin(const tcp_seg_t* s)
{
  s_conn.rcv_nxt = s->seq + (uint32_t)s->payload_len + 1U;
  s_conn.state   = k_tcp_state_last_ack;
  (void)tcp_send((uint8_t)(k_tcp_flag_ack | k_tcp_flag_fin), nullptr, 0U);
  s_conn.snd_nxt += 1U;
}

/**
 * @brief Run the FSM transitions for a parsed segment.
 *
 * @param[in] s Parsed segment.
 *
 * @pre s != nullptr and s->frame/ip/payload reference valid bytes.
 * @post s_conn.state advances per the RFC 793 subset.
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
  if ((s->flags & k_tcp_flag_fin) != 0U &&
      (s_conn.state == k_tcp_state_established || s_conn.state == k_tcp_state_syn_received)) {
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
 * @pre frame holds Eth + IPv4 + TCP at the standard offsets.
 * @pre ip_total >= k_ipv4_hdr_len + k_tcp_hdr_len.
 * @post s_conn.state advances per the RFC 793 subset:
 *         LISTEN -> SYN-RECEIVED -> ESTABLISHED -> CLOSE-WAIT ->
 *         LAST-ACK -> LISTEN.
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
  /* Drop frames not addressed to us (tolerate broadcast for ARP only). */
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
 * @brief Bring the chip up: CGC, time, GPIO, SCI8, RMII pins, NIC.
 *
 * @details Panic-halts on any failure -- there is no graceful retry.
 *
 * @pre Reset_Handler has copied .data and zeroed .bss.
 * @pre SystemInit set VTOR / FPU / priority grouping.
 * @post All HAL modules required by the echo loop are running.
 *
 * @since 0.1.0
 */
static void eth_tcp_echo_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;
  uint32_t pclka_hz   = 0U;
  if (ra_cgc_init() != k_ra_ok) {
    eth_tcp_echo_panic_halt();
  }
  if (ra_cgc_get_clock_hz(k_ra_clock_id_cpuclk0, &cpuclk0_hz) != k_ra_ok) {
    eth_tcp_echo_panic_halt();
  }
  if (ra_cgc_get_clock_hz(k_ra_clock_id_pclka, &pclka_hz) != k_ra_ok) {
    eth_tcp_echo_panic_halt();
  }
  if (ra_time_init(cpuclk0_hz) != k_ra_ok) {
    eth_tcp_echo_panic_halt();
  }
  if (ra_board_led_init(k_ra_board_led1) != k_ra_ok) {
    eth_tcp_echo_panic_halt();
  }
  if (ra_board_led_init(k_ra_board_led2) != k_ra_ok) {
    eth_tcp_echo_panic_halt();
  }

  /* SCI8 logging on PD_02 / PD_03 (J-Link OB CDC bridge). Pins are
   * file-scope above so the symbolic analyser sees them as immutable
   * compile-time constants and skips the enum-range warning. */
  if (ra_pfs_route_peripheral(k_pin_log_txd, k_ra_psel_sci_async, "eth_tcp_echo.log_tx") !=
      k_ra_ok) {
    eth_tcp_echo_panic_halt();
  }
  if (ra_pfs_route_peripheral(k_pin_log_rxd, k_ra_psel_sci_async, "eth_tcp_echo.log_rx") !=
      k_ra_ok) {
    eth_tcp_echo_panic_halt();
  }
  const ra_sci_cfg_t sci_cfg = {
    .baud      = k_eth_tcp_echo_baud,
    .data_bits = k_ra_sci_data_8,
    .parity    = k_ra_sci_parity_none,
    .stop_bits = k_ra_sci_stop_1,
    .pclk_hz   = pclka_hz,
  };
  if (ra_sci_init((uint8_t)k_eth_tcp_echo_sci_channel, &sci_cfg) != k_ra_ok) {
    eth_tcp_echo_panic_halt();
  }

  /* RGMII pinmux per EK-RA8D2 v1 UM Table 26 p 33. */
  if (ra_board_ethernet_init() != k_ra_ok) {
    eth_tcp_echo_panic_halt();
  }

  const ra_eth_cfg_t eth_cfg = {
    .mac_address = {k_my_mac[0], k_my_mac[1], k_my_mac[2], k_my_mac[3], k_my_mac[4], k_my_mac[5]},
    .channel     = 1U, /* EK-RA8D2: PEF7071 PHY is on RMAC1 (FSP eswm_rgmii1). */
    .num_tx_descriptors = 0U,
    .num_rx_descriptors = 0U,
    .buffer_size        = 0U,
  };
  if (ra_eth_open(&eth_cfg) != k_ra_ok) {
    eth_tcp_echo_panic_halt();
  }
}

/**
 * @brief Block until the PHY reports link-up via BMSR.
 *
 * @details Polls ::ra_eth_link_status every k_eth_tcp_echo_link_poll_ms
 * milliseconds until ``link_up == 1``. Logs once on first success.
 *
 * @pre ra_eth_open succeeded.
 * @post On return, the PHY has reported link-up at least once.
 *
 * @since 0.1.0
 */
static void eth_tcp_echo_wait_link(void)
{
  while (1) {
    ra_eth_link_t  st  = {};
    const ra_err_t err = ra_eth_link_status(&st);
    if (err == k_ra_ok && st.link_up != 0U) {
      eth_tcp_echo_log("ra8d2: link up\r\n");
      return;
    }
    ra_delay_ms(k_eth_tcp_echo_link_poll_ms);
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

  eth_tcp_echo_setup_or_halt();
  ra_isr_globals_enable();
  eth_tcp_echo_wait_link();

  /* HIL probe banner -- parsed by scripts/hil_eth_tcp.sh after flashing
   * to discover the static IPv4 address the firmware will respond at,
   * plus an explicit "ready" mark so the host knows to start probing. */
  eth_tcp_echo_log("eth: ip=192.168.1.42 port=7 proto=tcp\r\n");
  eth_tcp_echo_log("eth: ready\r\n");

  uint8_t rx_frame[k_eth_tcp_echo_frame_buf] = {};
  while (1) {
    uint32_t       rx_len = 0U;
    const ra_err_t err    = ra_eth_read(rx_frame, (uint32_t)k_eth_tcp_echo_frame_buf, &rx_len);
    if (err == k_ra_ok && rx_len > 0U) {
      handle_frame(rx_frame, (uint16_t)rx_len);
    } else {
      ra_delay_ms(0U); /* Cooperative yield placeholder. */
    }
  }

  eth_tcp_echo_panic_halt();
  return 0;
}
#pragma GCC diagnostic pop
