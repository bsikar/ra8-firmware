/**
 * @file board_net.c
 * @brief Virtual network peer (Ethernet/ARP/IPv4/ICMP/TCP) for ra8_emulator
 *
 * @details
 * Implements the "other host on the wire" behind the ra8_eth frame seam (see
 * board_net.h). The peer is 192.168.1.1 (MAC 02:00:5E:00:53:01); the firmware is
 * 192.168.1.42. Its state machine resolves the firmware over ARP, pings it
 * (ICMP), then opens a TCP connection to the echo server on port 7, sends a
 * payload, and verifies the echo -- proving the NetX networking example runs
 * end-to-end with no hardware. Frames are exchanged as plain byte buffers;
 * main.c marshals them to/from guest memory.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include "board_net.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "board_console.h"
#include "emu_host_io_internal.h"

/** @brief Offsets into the frames this stub synthesises (RFC 826 / RFC 793). */
typedef enum : uint8_t {
  k_arp_plen_off    = 5U,  /**< ARP protocol-address-length field offset. */
  k_arp_spa_off     = 14U, /**< ARP sender-protocol-address offset.       */
  k_arp_tha_off     = 18U, /**< ARP target-hardware-address offset.       */
  k_tcp_off_payload = 20U, /**< TCP payload offset (no options here).     */
} board_net_frame_off_t;

/** @brief Console-tap line buffer capacity for a network packet summary. */
typedef enum : uint32_t {
  k_net_console_line_cap = 48U, /**< Max chars in a "NET tx eth=.." line. */
} net_console_t;

/** @brief Addressing + protocol constants for the peer and the firmware. */
typedef enum : uint32_t {
  k_net_peer_ip   = 0xC0A80101UL, /**< 192.168.1.1   (the peer).      */
  k_net_fw_ip     = 0xC0A8012AUL, /**< 192.168.1.42  (the firmware).  */
  k_net_echo_port = 7U,           /**< Firmware TCP echo server port. */
  k_net_peer_port = 49152U,       /**< Peer ephemeral source port.    */
} net_addr_t;

/** @brief Frame offsets / sizes (Ethernet II + ARP + IPv4 + ICMP + TCP). */
typedef enum : uint32_t {
  k_eth_hdr       = 14U,     /**< dst[6] src[6] type[2].    */
  k_eth_arp       = 0x0806U, /**< ARP ethertype.            */
  k_eth_ipv4      = 0x0800U, /**< IPv4 ethertype.           */
  k_arp_len       = 28U,     /**< ARP payload length.       */
  k_ip_hdr        = 20U,     /**< IPv4 header (no options). */
  k_icmp_hdr      = 8U,      /**< ICMP echo header.         */
  k_ip_proto_icmp = 1U,      /**< IPv4 protocol = ICMP.     */
  k_ip_proto_tcp  = 6U,      /**< IPv4 protocol = TCP.      */
  k_mac_len       = 6U,      /**< Ethernet address length.  */
  k_net_buf       = 1600U,   /**< Staging buffer size.      */
} net_frame_t;

/**
 * @enum net_proto_t
 * @brief Protocol field offsets, masks, and well-known values for the frames
 *        board_net builds/parses (Ethernet / ARP / IPv4 / ICMP / TCP).
 */
typedef enum : uint32_t {
  k_byte_mask         = 0xFFU,   /**< One octet.                          */
  k_u16_mask          = 0xFFFFU, /**< 16-bit field.                       */
  k_shift24           = 24U,     /**< Byte-3 position in a 32-bit word.   */
  k_peer_mac_b2       = 0x5EU,   /**< Peer MAC octet 2 (locally admin).   */
  k_peer_mac_b4       = 0x53U,   /**< Peer MAC octet 4.                   */
  k_eth_ethertype_off = 12U,     /**< EtherType offset in the eth header. */
  k_arp_plen          = 4U,      /**< ARP protocol-address length (IPv4). */
  k_arp_tpa_off       = 24U,     /**< ARP target-protocol-address offset. */
  k_ipv4_ver_ihl      = 0x45U,   /**< IPv4 version 4, IHL 5.              */
  k_ip_ident          = 0x1234U, /**< IPv4 identification (fixed).        */
  k_ip_flag_df        = 0x4000U, /**< IPv4 don't-fragment flag.           */
  k_ip_ttl            = 64U,     /**< IPv4 default TTL.                   */
  k_ip_proto_off      = 9U,      /**< IPv4 protocol-field offset.         */
  k_ip_csum_off       = 10U,     /**< IPv4 header-checksum offset.        */
  k_ip_src_off        = 12U,     /**< IPv4 source-address offset.         */
  k_ip_dst_off        = 16U,     /**< IPv4 destination-address offset.    */
  k_ip_ihl_mask       = 0x0FU,   /**< IHL / data-offset nibble mask.      */
  k_ihl_word          = 4U,      /**< IHL/data-offset word size (bytes).  */
  k_icmp_ident        = 0xBEEFU, /**< ICMP echo identifier (fixed).       */
  k_icmp_pat_base     = 0x40U,   /**< ICMP payload byte-pattern base.     */
  k_tcp_hdr           = 20U,     /**< TCP header bytes (no options).      */
  k_tcp_payload_max   = 64U,     /**< Max TCP payload board_net sends.    */
  k_tcp_off_dataoff   = 12U,     /**< TCP data-offset byte position.      */
  k_tcp_off_flags     = 13U,     /**< TCP flags byte position.            */
  k_tcp_off_window    = 14U,     /**< TCP window field position.          */
  k_tcp_data_off      = 0x50U,   /**< TCP data offset = 5 words.          */
  k_tcp_window        = 2048U,   /**< TCP advertised window.              */
  k_tcp_isn           = 1000U,   /**< Deterministic initial seq number.   */
  k_tcp_doff_shift    = 4U,      /**< TCP data-offset high-nibble shift.  */
} net_proto_t;

/** @brief Peer state machine: ARP -> ping -> TCP connect / echo / close. */
typedef enum : uint8_t {
  k_net_init  = 0U, /**< Nothing sent yet.                   */
  k_net_arp   = 1U, /**< ARP request out; awaiting reply.    */
  k_net_ping  = 2U, /**< ICMP echo out; awaiting reply.      */
  k_net_syn   = 3U, /**< TCP SYN out; awaiting SYN-ACK.      */
  k_net_estab = 4U, /**< Connected; data out, awaiting echo. */
  k_net_fin   = 5U, /**< FIN out; awaiting close.            */
  k_net_done  = 6U, /**< Connection closed.                  */
} net_state_t;

/** @brief TCP control-bit flags. */
typedef enum : uint8_t {
  k_tcp_fin = 0x01U, /**< TCP fin. */
  k_tcp_syn = 0x02U, /**< TCP syn. */
  k_tcp_rst = 0x04U, /**< TCP rst. */
  k_tcp_psh = 0x08U, /**< TCP psh. */
  k_tcp_ack = 0x10U, /**< TCP ack. */
} net_tcp_flag_t;

static const uint8_t s_peer_mac[k_mac_len] =
  {0x02U, 0x00U, (uint8_t)k_peer_mac_b2, 0x00U, (uint8_t)k_peer_mac_b4, 0x01U};

static bool     s_trace;
static uint8_t  s_state;
static uint8_t  s_fw_mac[k_mac_len]; /**< Learned from ARP. */
static bool     s_fw_mac_known;
static uint32_t s_arp_replies;    /**< ARP replies received.                   */
static uint32_t s_pings;          /**< ICMP echo replies received.             */
static uint32_t s_wait;           /**< Ticks since the last send.              */
static uint16_t s_ping_seq;       /**< ICMP echo sequence.                     */
static uint32_t s_tx_frames;      /**< Frames the firmware sent.               */
static uint32_t s_polls;          /**< ra8_eth_read polls served.              */
static uint32_t s_delivered;      /**< Frames delivered to firmware.           */
static uint32_t s_tcp_our_seq;    /**< Our next TCP send sequence.             */
static uint32_t s_tcp_their_seq;  /**< Their next seq (our ack).               */
static uint32_t s_tcp_echoed;     /**< Echo payload bytes received.            */
static bool     s_tcp_match;      /**< Echo matched what we sent.              */
static bool     s_tcp_need_data;  /**< Payload queued to send post-handshake.  */
static uint32_t s_tcp_estab_wait; /**< Ticks since the connection established. */

/**
 * @brief Payload the peer sends to the firmware's TCP echo server.
 *
 * @details
 * Its LENGTH is part of the EIL contract, not just an implementation detail:
 * `threadx_netx_tcp_echo` echoes it and logs "[netx] echoed N bytes", and that
 * exact line is the `HIL_EXPECT` its `hil.conf` asserts. So this string must
 * not carry anything that gets renamed. It used to read "hello from
 * board_sim\n" (21 bytes); the board_sim -> ra8_emulator rename silently made
 * it 24 and broke that assertion, which nothing noticed because the app was
 * parked outside the EIL run set at the time (#499). Hence a name-free literal
 * and the static_assert below -- change the text freely, but a change in
 * length has to be made deliberately and mirrored in the hil.conf.
 */
static const uint8_t s_tcp_payload[] = "tcp echo probe payload\n";

/** @brief Payload length the `threadx_netx_tcp_echo` hil.conf asserts. */
enum : uint32_t {
  k_net_tcp_payload_len = 23U, /**< sizeof(s_tcp_payload) - 1, pinned. */
};
static_assert((sizeof(s_tcp_payload) - 1U) == (size_t)k_net_tcp_payload_len,
              "EIL contract: threadx_netx_tcp_echo's hil.conf HIL_EXPECT names "
              "this byte count -- update examples/ek_ra8d2/hw_validated/hil/"
              "threadx_netx_tcp_echo/hil.conf in the same change");

/* Ring of frames queued for the firmware to receive. The firmware's RX worker
 * drains all available frames per poll, so a handshake burst (ACK + data) can
 * queue several at once. */
enum : uint32_t {
  k_net_qdepth = 8U, /**< Net qdepth. */
};
static uint8_t  s_rxq[k_net_qdepth][k_net_buf];
static uint16_t s_rxq_len[k_net_qdepth];
static uint32_t s_rxq_head;
static uint32_t s_rxq_tail;

/* =============================================================================
 * Byte / checksum helpers.
 * =============================================================================
 */

/**
 * @brief Store a 16-bit value big-endian (network order) at @p p.
 * @details Store a 16-bit value big-endian (network order) at @p p; this step is contained within the board net model and uses bounded caller or module-owned storage.
 * @param[in,out] p Module-owned state object processed by the operation.
 * @param[in] v Register or payload value processed by the operation.
 * @pre Arguments satisfy the ranges documented for put16. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the board net model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_put16(uint8_t* p, uint16_t v)
{
  p[0] = (uint8_t)(v >> 8);
  p[1] = (uint8_t)(v & (uint32_t)k_byte_mask);
}

/**
 * @brief Store a 32-bit value big-endian at @p p.
 * @details Store a 32-bit value big-endian at @p p; this step is contained within the board net model and uses bounded caller or module-owned storage.
 * @param[in,out] p Module-owned state object processed by the operation.
 * @param[in] v Register or payload value processed by the operation.
 * @pre Arguments satisfy the ranges documented for put32. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the board net model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_put32(uint8_t* p, uint32_t v)
{
  p[0] = (uint8_t)(v >> (uint32_t)k_shift24);
  p[1] = (uint8_t)((v >> 16) & (uint32_t)k_byte_mask);
  p[2] = (uint8_t)((v >> 8) & (uint32_t)k_byte_mask);
  p[3] = (uint8_t)(v & (uint32_t)k_byte_mask);
}

/**
 * @brief Read a big-endian 16-bit value from @p p.
 * @details Read a big-endian 16-bit value from @p p; this step is contained within the board net model and uses bounded caller or module-owned storage.
 * @param[in] p Module-owned state object processed by the operation.
 * @return The get16 result produced by the board net model.
 * @retval value The operation-specific get16 value.
 * @pre Arguments satisfy the ranges documented for get16. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the board net model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static uint16_t internal_get16(const uint8_t* p)
{
  return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

/**
 * @brief Read a big-endian 32-bit value from @p p.
 * @details Read a big-endian 32-bit value from @p p; this step is contained within the board net model and uses bounded caller or module-owned storage.
 * @param[in] p Module-owned state object processed by the operation.
 * @return The get32 result produced by the board net model.
 * @retval value The operation-specific get32 value.
 * @pre Arguments satisfy the ranges documented for get32. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the board net model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static uint32_t internal_get32(const uint8_t* p)
{
  return ((uint32_t)p[0] << (uint32_t)k_shift24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) |
         (uint32_t)p[3];
}

/**
 * @brief 16-bit one's-complement checksum over @p len bytes at @p d.
 * @details 16-bit one's-complement checksum over @p len bytes at @p d; this step is contained within the board net model and uses bounded caller or module-owned storage.
 * @param[in] d D input used by the operation.
 * @param[in] len Number of payload bytes to process.
 * @param[in] seed Seed input used by the operation.
 * @return The net checksum result produced by the board net model.
 * @retval value The operation-specific net checksum value.
 * @pre Arguments satisfy the ranges documented for net checksum. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the board net model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static uint16_t internal_net_checksum(const uint8_t* d, uint32_t len, uint32_t seed)
{
  uint32_t sum = seed;
  for (uint32_t i = 0U; (i + 1U) < len; i += 2U) {
    sum += ((uint32_t)d[i] << 8) | (uint32_t)d[i + 1U];
  }
  if ((len & 1U) != 0U) {
    sum += (uint32_t)d[len - 1U] << 8;
  }
  while ((sum >> 16) != 0U) {
    sum = (sum & (uint32_t)k_u16_mask) + (sum >> 16);
  }
  return (uint16_t)(~sum & (uint32_t)k_u16_mask);
}

/**
 * @brief Queue a built frame for the firmware to receive (drops if ring full).
 * @details Queue a built frame for the firmware to receive (drops if ring full); this step is contained within the board net model and uses bounded caller or module-owned storage.
 * @param[in] frame Frame input used by the operation.
 * @param[in] len Number of payload bytes to process.
 * @pre Arguments satisfy the ranges documented for net queue. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the board net model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_net_queue(const uint8_t* frame, uint32_t len)
{
  const uint32_t next = (s_rxq_tail + 1U) % (uint32_t)k_net_qdepth;
  if ((next == s_rxq_head) || (len > (uint32_t)k_net_buf)) {
    return;
  }
  (void)memcpy(s_rxq[s_rxq_tail], frame, len);
  s_rxq_len[s_rxq_tail] = (uint16_t)len;
  s_rxq_tail            = next;
}

/**
 * @brief Fill the 14-byte Ethernet header into @p f.
 * @details Fill the 14-byte ethernet header into @p f; this step is contained within the board net model and uses bounded caller or module-owned storage.
 * @param[in,out] f F state or storage updated in place by the operation.
 * @param[in] dst Destination storage receiving the result.
 * @param[in] ethertype Ethertype input used by the operation.
 * @pre Arguments satisfy the ranges documented for net Ethernet hdr. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the board net model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_net_eth_hdr(uint8_t* f, const uint8_t* dst, uint16_t ethertype)
{
  (void)memcpy(&f[0], dst, k_mac_len);
  (void)memcpy(&f[6], s_peer_mac, k_mac_len);
  internal_put16(&f[k_eth_ethertype_off], ethertype);
}

/* =============================================================================
 * ARP -- resolve the firmware's MAC.
 * =============================================================================
 */

/**
 * @brief Build + queue an ARP request asking who-has the firmware's IP.
 * @details Build + queue an arp request asking who-has the firmware's ip; this step is contained within the board net model and uses bounded caller or module-owned storage.
 * @pre Arguments satisfy the ranges documented for net send arp request. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the board net model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_net_send_arp_request(void)
{
  static const uint8_t bcast[k_mac_len] = {(uint8_t)k_byte_mask,
                                           (uint8_t)k_byte_mask,
                                           (uint8_t)k_byte_mask,
                                           (uint8_t)k_byte_mask,
                                           (uint8_t)k_byte_mask,
                                           (uint8_t)k_byte_mask};
  uint8_t              f[k_eth_hdr + k_arp_len];
  (void)memset(f, 0, sizeof(f));
  internal_net_eth_hdr(f, bcast, (uint16_t)k_eth_arp);
  uint8_t* a = &f[k_eth_hdr];
  internal_put16(&a[0], 1U);                   /* htype = Ethernet. */
  internal_put16(&a[2], (uint16_t)k_eth_ipv4); /* ptype = IPv4.     */
  a[4]              = (uint8_t)k_mac_len;
  a[k_arp_plen_off] = (uint8_t)k_arp_plen;
  internal_put16(&a[6], 1U); /* op = request. */
  (void)memcpy(&a[8], s_peer_mac, k_mac_len);
  internal_put32(&a[k_arp_spa_off], (uint32_t)k_net_peer_ip);
  internal_put32(&a[k_arp_tpa_off], (uint32_t)k_net_fw_ip);
  internal_net_queue(f, sizeof(f));
}

/**
 * @brief Build + queue an ARP reply giving the peer's MAC to the firmware.
 * @details Build + queue an arp reply giving the peer's mac to the firmware; this step is contained within the board net model and uses bounded caller or module-owned storage.
 * @param[in] to_mac To mac input used by the operation.
 * @param[in] to_ip To ip input used by the operation.
 * @pre Arguments satisfy the ranges documented for net send arp reply. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the board net model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_net_send_arp_reply(const uint8_t* to_mac, uint32_t to_ip)
{
  uint8_t f[k_eth_hdr + k_arp_len];
  (void)memset(f, 0, sizeof(f));
  internal_net_eth_hdr(f, to_mac, (uint16_t)k_eth_arp);
  uint8_t* a = &f[k_eth_hdr];
  internal_put16(&a[0], 1U);
  internal_put16(&a[2], (uint16_t)k_eth_ipv4);
  a[4]              = (uint8_t)k_mac_len;
  a[k_arp_plen_off] = (uint8_t)k_arp_plen;
  internal_put16(&a[6], 2U); /* op = reply. */
  (void)memcpy(&a[8], s_peer_mac, k_mac_len);
  internal_put32(&a[k_arp_spa_off], (uint32_t)k_net_peer_ip);
  (void)memcpy(&a[k_arp_tha_off], to_mac, k_mac_len);
  internal_put32(&a[k_arp_tpa_off], to_ip);
  internal_net_queue(f, sizeof(f));
}

/* =============================================================================
 * ICMP -- ping the firmware.
 * =============================================================================
 */

/**
 * @brief Fill a 20-byte IPv4 header (no options) + compute its checksum.
 * @details Fill a 20-byte ipv4 header (no options) + compute its checksum; this step is contained within the board net model and uses bounded caller or module-owned storage.
 * @param[in,out] ip Ip state or storage updated in place by the operation.
 * @param[in] proto Proto input used by the operation.
 * @param[in] payload_len Bound for the payload data.
 * @pre Arguments satisfy the ranges documented for net ip hdr. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the board net model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_net_ip_hdr(uint8_t* ip, uint8_t proto, uint16_t payload_len)
{
  (void)memset(ip, 0, k_ip_hdr);
  ip[0] = (uint8_t)k_ipv4_ver_ihl; /* version 4, IHL 5. */
  internal_put16(&ip[2], (uint16_t)(k_ip_hdr + payload_len));
  internal_put16(&ip[4], (uint16_t)k_ip_ident);   /* identification. */
  internal_put16(&ip[6], (uint16_t)k_ip_flag_df); /* don't fragment. */
  ip[8]              = (uint8_t)k_ip_ttl;         /* TTL.            */
  ip[k_ip_proto_off] = proto;
  internal_put32(&ip[k_ip_src_off], (uint32_t)k_net_peer_ip);
  internal_put32(&ip[k_ip_dst_off], (uint32_t)k_net_fw_ip);
  internal_put16(&ip[k_ip_csum_off], internal_net_checksum(ip, k_ip_hdr, 0U));
}

/**
 * @brief Build + queue an ICMP echo request to the firmware.
 * @details Build + queue an icmp echo request to the firmware; this step is contained within the board net model and uses bounded caller or module-owned storage.
 * @pre Arguments satisfy the ranges documented for net send ping. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the board net model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_net_send_ping(void)
{
  if (!s_fw_mac_known) {
    return;
  }
  uint8_t f[k_eth_hdr + k_ip_hdr + k_icmp_hdr + 16U];
  (void)memset(f, 0, sizeof(f));
  internal_net_eth_hdr(f, s_fw_mac, (uint16_t)k_eth_ipv4);
  const uint16_t icmp_len = (uint16_t)(k_icmp_hdr + 16U);
  internal_net_ip_hdr(&f[k_eth_hdr], (uint8_t)k_ip_proto_icmp, icmp_len);
  uint8_t* ic = &f[k_eth_hdr + k_ip_hdr];
  ic[0]       = 8U; /* echo request. */
  s_ping_seq++;
  internal_put16(&ic[4], (uint16_t)k_icmp_ident); /* identifier. */
  internal_put16(&ic[6], s_ping_seq);
  for (uint32_t i = 0U; i < 16U; i++) {
    ic[k_icmp_hdr + i] = (uint8_t)((uint32_t)k_icmp_pat_base + i); /* payload pattern. */
  }
  internal_put16(&ic[2], internal_net_checksum(ic, icmp_len, 0U));
  internal_net_queue(f, sizeof(f));
}

/* =============================================================================
 * TCP -- connect to the firmware's echo server, send a payload, verify the echo.
 * =============================================================================
 */

/**
 * @brief Build + queue a TCP segment to the firmware echo port (flags+payload).
 * @details Build + queue a tcp segment to the firmware echo port (flags+payload); this step is contained within the board net model and uses bounded caller or module-owned storage.
 * @param[in] flags Flags input used by the operation.
 * @param[in] payload Payload input used by the operation.
 * @param[in] payload_len Bound for the payload data.
 * @pre Arguments satisfy the ranges documented for net send tcp. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the board net model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static void
internal_net_send_tcp(uint8_t flags, const uint8_t* payload, uint16_t payload_len)
{
  if (!s_fw_mac_known) {
    return;
  }
  uint8_t        f[k_eth_hdr + k_ip_hdr + k_tcp_hdr + k_tcp_payload_max];
  const uint16_t tcp_len = (uint16_t)((uint16_t)k_tcp_hdr + payload_len);
  if (((uint32_t)k_eth_hdr + (uint32_t)k_ip_hdr + (uint32_t)tcp_len) > sizeof(f)) {
    return;
  }
  (void)memset(f, 0, sizeof(f));
  internal_net_eth_hdr(f, s_fw_mac, (uint16_t)k_eth_ipv4);
  internal_net_ip_hdr(&f[k_eth_hdr], (uint8_t)k_ip_proto_tcp, tcp_len);
  uint8_t* t = &f[k_eth_hdr + k_ip_hdr];
  internal_put16(&t[0], (uint16_t)k_net_peer_port);
  internal_put16(&t[2], (uint16_t)k_net_echo_port);
  internal_put32(&t[4], s_tcp_our_seq);
  internal_put32(&t[8], s_tcp_their_seq);
  t[k_tcp_off_dataoff] = (uint8_t)k_tcp_data_off; /* data offset = 5 32-bit words. */
  t[k_tcp_off_flags]   = flags;
  internal_put16(&t[k_tcp_off_window], (uint16_t)k_tcp_window); /* window. */
  if (payload_len > 0U) {
    (void)memcpy(&t[k_tcp_off_payload], payload, payload_len);
  }
  /* TCP checksum covers the IPv4 pseudo-header + the segment. */
  const uint32_t pseudo =
    ((uint32_t)k_net_peer_ip >> 16) + ((uint32_t)k_net_peer_ip & (uint32_t)k_u16_mask) +
    ((uint32_t)k_net_fw_ip >> 16) + ((uint32_t)k_net_fw_ip & (uint32_t)k_u16_mask) +
    (uint32_t)k_ip_proto_tcp + (uint32_t)tcp_len;
  internal_put16(&t[16], internal_net_checksum(t, tcp_len, pseudo));
  internal_net_queue(f, (uint32_t)k_eth_hdr + (uint32_t)k_ip_hdr + (uint32_t)tcp_len);
}

/**
 * @brief Open the TCP connection: send SYN with our initial sequence number.
 * @details Open the tcp connection: send syn with our initial sequence number; this step is contained within the board net model and uses bounded caller or module-owned storage.
 * @pre Arguments satisfy the ranges documented for net send syn. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the board net model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_net_send_syn(void)
{
  s_tcp_our_seq = (uint32_t)k_tcp_isn; /* deterministic ISN. */
  internal_net_send_tcp((uint8_t)k_tcp_syn, nullptr, 0U);
}

/**
 * @brief Send the test payload to the established echo connection.
 * @details Send the test payload to the established echo connection; this step is contained within the board net model and uses bounded caller or module-owned storage.
 * @pre Arguments satisfy the ranges documented for net send data. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the board net model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_net_send_data(void)
{
  const uint16_t n = (uint16_t)(sizeof(s_tcp_payload) - 1U);
  internal_net_send_tcp((uint8_t)(k_tcp_psh | k_tcp_ack), s_tcp_payload, n);
  s_tcp_our_seq += (uint32_t)n;
}

/**
 * @brief ESTAB state: echo-match the payload, then start our active (FIN) close.
 * @details Estab state: echo-match the payload, then start our active (fin) close; this step is contained within the board net model and uses bounded caller or module-owned storage.
 * @param[in] seq Seq input used by the operation.
 * @param[in] pdata Pdata input used by the operation.
 * @param[in] plen Plen input used by the operation.
 * @pre Arguments satisfy the ranges documented for net tcp on estab. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the board net model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static void
internal_net_tcp_on_estab(uint32_t seq, const uint8_t* pdata, uint32_t plen)
{
  s_tcp_echoed = plen;
  s_tcp_match =
    (plen == (uint32_t)(sizeof(s_tcp_payload) - 1U)) && (memcmp(pdata, s_tcp_payload, plen) == 0);
  s_tcp_their_seq = seq + plen;
  internal_net_send_tcp((uint8_t)k_tcp_ack, nullptr, 0U);
  internal_net_send_tcp((uint8_t)(k_tcp_fin | k_tcp_ack), nullptr, 0U);
  s_tcp_our_seq += 1U; /* FIN consumes one. */
  s_state = (uint8_t)k_net_fin;
  if (s_trace) {
    (void)priv_emu_io_errf("  [net] TCP echo %u byte(s) match=%s\n", plen, s_tcp_match ? "Y" : "N");
  }
}

/**
 * @brief Handle an inbound TCP segment: drive the connect / echo / close FSM.
 * @details Handle an inbound tcp segment: drive the connect / echo / close fsm; this step is contained within the board net model and uses bounded caller or module-owned storage.
 * @param[in] t T input used by the operation.
 * @param[in] len Number of payload bytes to process.
 * @pre Arguments satisfy the ranges documented for net rx tcp. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the board net model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_net_rx_tcp(const uint8_t* t, uint32_t len)
{
  if (len < (uint32_t)k_tcp_hdr) {
    return;
  }
  if ((internal_get16(&t[0]) != (uint16_t)k_net_echo_port) ||
      (internal_get16(&t[2]) != (uint16_t)k_net_peer_port)) {
    return;
  }
  const uint32_t seq   = internal_get32(&t[4]);
  const uint8_t  flags = t[13];
  const uint32_t doff =
    (uint32_t)((t[k_tcp_off_dataoff] >> (uint32_t)k_tcp_doff_shift) & (uint32_t)k_ip_ihl_mask) *
    (uint32_t)k_ihl_word;
  if ((doff < (uint32_t)k_tcp_hdr) || (doff > len)) {
    return;
  }
  const uint32_t plen  = len - doff;
  const uint8_t* pdata = &t[doff];
  if (s_trace) {
    (void)priv_emu_io_errf("  [net] RX TCP flags=0x%02X seq=%u ack=%u plen=%u (state %u)\n",
                           (unsigned)flags,
                           seq,
                           internal_get32(&t[8]),
                           plen,
                           (unsigned)s_state);
  }

  if ((s_state == (uint8_t)k_net_syn) && ((flags & (uint8_t)k_tcp_syn) != 0U) &&
      ((flags & (uint8_t)k_tcp_ack) != 0U)) {
    s_tcp_their_seq = seq + 1U; /* their SYN consumes one sequence number. */
    s_tcp_our_seq += 1U;        /* our SYN consumed one.                   */
    internal_net_send_tcp((uint8_t)k_tcp_ack, nullptr, 0U);
    /* Defer the payload a few ticks so the firmware's accept() binds the socket
     * and the echo thread is waiting in receive() before the data arrives. */
    s_tcp_need_data  = true;
    s_tcp_estab_wait = 0U;
    s_state          = (uint8_t)k_net_estab;
    return;
  }
  if ((s_state == (uint8_t)k_net_estab) && (plen > 0U)) {
    internal_net_tcp_on_estab(seq, pdata, plen);
    return;
  }
  if ((s_state == (uint8_t)k_net_fin) && ((flags & (uint8_t)k_tcp_fin) != 0U)) {
    s_tcp_their_seq = seq + plen + 1U; /* their FIN consumes one. */
    internal_net_send_tcp((uint8_t)k_tcp_ack, nullptr, 0U);
    s_state = (uint8_t)k_net_done;
    return;
  }
}

/* =============================================================================
 * RX parsing + the peer state machine.
 * =============================================================================
 */

/**
 * @brief Handle an inbound ARP frame (learn the firmware MAC / answer who-has).
 * @details Handle an inbound arp frame (learn the firmware mac / answer who-has); this step is contained within the board net model and uses bounded caller or module-owned storage.
 * @param[in] a A input used by the operation.
 * @param[in] len Number of payload bytes to process.
 * @pre Arguments satisfy the ranges documented for net rx arp. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the board net model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_net_rx_arp(const uint8_t* a, uint32_t len)
{
  if (len < (uint32_t)k_arp_len) {
    return;
  }
  const uint16_t op  = internal_get16(&a[6]);
  const uint32_t spa = internal_get32(&a[14]);
  const uint32_t tpa = internal_get32(&a[24]);
  if (spa == (uint32_t)k_net_fw_ip) {
    (void)memcpy(s_fw_mac, &a[8], k_mac_len); /* sender HW = firmware MAC. */
    s_fw_mac_known = true;
    if (op == 2U) {
      s_arp_replies++;
    }
  }
  if ((op == 1U) && (tpa == (uint32_t)k_net_peer_ip)) {
    internal_net_send_arp_reply(&a[8], spa); /* firmware asked who-has us. */
  }
  if (s_fw_mac_known && (s_state == (uint8_t)k_net_arp)) {
    internal_net_send_ping();
    s_state = (uint8_t)k_net_ping;
    s_wait  = 0U;
  }
}

/**
 * @brief Handle an inbound ICMP echo reply (count a successful ping).
 * @details Handle an inbound icmp echo reply (count a successful ping); this step is contained within the board net model and uses bounded caller or module-owned storage.
 * @param[in] ic Ic input used by the operation.
 * @param[in] len Number of payload bytes to process.
 * @pre Arguments satisfy the ranges documented for net rx icmp. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the board net model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_net_rx_icmp(const uint8_t* ic, uint32_t len)
{
  if ((len >= (uint32_t)k_icmp_hdr) && (ic[0] == 0U)) { /* echo reply. */
    s_pings++;
    if (s_state == (uint8_t)k_net_ping) {
      if (s_trace) {
        (void)priv_emu_io_errf(
          "  [net] ICMP echo reply from 192.168.1.42 -- ping ok; opening TCP\n");
      }
      internal_net_send_syn(); /* ping proven; connect to the echo server. */
      s_state = (uint8_t)k_net_syn;
      s_wait  = 0U;
    }
  }
}

/**
 * @brief Handle an inbound IPv4 frame, dispatching by protocol.
 * @details Handle an inbound ipv4 frame, dispatching by protocol; this step is contained within the board net model and uses bounded caller or module-owned storage.
 * @param[in] ip Ip input used by the operation.
 * @param[in] len Number of payload bytes to process.
 * @pre Arguments satisfy the ranges documented for net rx ipv4. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the board net model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_net_rx_ipv4(const uint8_t* ip, uint32_t len)
{
  if (len < (uint32_t)k_ip_hdr) {
    return;
  }
  const uint32_t ihl = (uint32_t)(ip[0] & (uint32_t)k_ip_ihl_mask) * (uint32_t)k_ihl_word;
  if ((ihl < (uint32_t)k_ip_hdr) || (ihl > len)) {
    return;
  }
  /* Use the IP total-length field, not the frame length: a short frame is padded
   * to the 60-byte Ethernet minimum, and that padding must not be mistaken for
   * upper-layer payload (e.g. a bare TCP ACK would otherwise look like 6 data
   * bytes). */
  uint32_t       actual   = len;
  const uint16_t ip_total = internal_get16(&ip[2]);
  if (((uint32_t)ip_total >= ihl) && ((uint32_t)ip_total <= len)) {
    actual = (uint32_t)ip_total;
  }
  const uint8_t proto = ip[9];
  if (proto == (uint8_t)k_ip_proto_icmp) {
    internal_net_rx_icmp(&ip[ihl], actual - ihl);
  } else if (proto == (uint8_t)k_ip_proto_tcp) {
    internal_net_rx_tcp(&ip[ihl], actual - ihl);
  }
}

void board_net_on_tx(const uint8_t* frame, uint32_t len)
{
  s_tx_frames++;
  if (len < (uint32_t)k_eth_hdr) {
    return;
  }
  if (s_trace) {
    (void)priv_emu_io_errf("  [net] firmware TX %u bytes ethertype 0x%04X\n",
                           len,
                           (unsigned)internal_get16(&frame[k_eth_ethertype_off]));
  }
  const uint16_t ethertype = internal_get16(&frame[k_eth_ethertype_off]);
  /* Console NET tab: one line per frame the firmware transmits. */
  char ln[k_net_console_line_cap];
  (void)snprintf(ln, sizeof(ln), "NET tx eth=0x%04X %uB", (unsigned)ethertype, (unsigned)len);
  board_console_push(k_board_console_ch_net, ln);
  if (ethertype == (uint16_t)k_eth_arp) {
    internal_net_rx_arp(&frame[k_eth_hdr], len - (uint32_t)k_eth_hdr);
  } else if (ethertype == (uint16_t)k_eth_ipv4) {
    internal_net_rx_ipv4(&frame[k_eth_hdr], len - (uint32_t)k_eth_hdr);
  }
}

uint32_t board_net_poll_rx(uint8_t* buf, uint32_t max)
{
  s_polls++;
  if (s_rxq_head == s_rxq_tail) {
    return 0U; /* ring empty. */
  }
  const uint32_t n = s_rxq_len[s_rxq_head];
  if (n > max) {
    return 0U;
  }
  (void)memcpy(buf, s_rxq[s_rxq_head], n);
  s_rxq_head = (s_rxq_head + 1U) % (uint32_t)k_net_qdepth;
  s_delivered++;
  /* Console NET tab: one line per frame the peer delivers to the firmware. */
  char ln[k_net_console_line_cap];
  (void)snprintf(ln, sizeof(ln), "NET rx %uB (#%u)", (unsigned)n, (unsigned)s_delivered);
  board_console_push(k_board_console_ch_net, ln);
  return n;
}

void board_net_tick(void)
{
  s_wait++;
  if (s_state == (uint8_t)k_net_init) {
    internal_net_send_arp_request();
    s_state = (uint8_t)k_net_arp;
    s_wait  = 0U;
    return;
  }
  if ((s_state == (uint8_t)k_net_estab) && s_tcp_need_data) {
    enum : uint32_t { k_net_data_delay = 800U /**< Net data delay. */ };
    s_tcp_estab_wait++;
    if (s_tcp_estab_wait > k_net_data_delay) {
      internal_net_send_data(); /* connection settled; send the echo payload. */
      s_tcp_need_data = false;
    }
    return;
  }
  /* Retransmit the pending step if the firmware has not answered for a while
   * (the stack may still be bringing the interface up on the first attempts). */
  enum : uint32_t { k_net_retry = 2000U /**< Net retry. */ };
  if (s_wait > k_net_retry) {
    s_wait = 0U;
    if (s_state == (uint8_t)k_net_arp) {
      internal_net_send_arp_request();
    } else if (s_state == (uint8_t)k_net_ping) {
      internal_net_send_ping();
    } else if (s_state == (uint8_t)k_net_syn) {
      internal_net_send_syn();
    }
  }
}

void board_net_init(bool trace)
{
  s_trace          = trace;
  s_state          = (uint8_t)k_net_init;
  s_fw_mac_known   = false;
  s_arp_replies    = 0U;
  s_pings          = 0U;
  s_wait           = 0U;
  s_ping_seq       = 0U;
  s_rxq_head       = 0U;
  s_rxq_tail       = 0U;
  s_tx_frames      = 0U;
  s_polls          = 0U;
  s_delivered      = 0U;
  s_tcp_our_seq    = 0U;
  s_tcp_their_seq  = 0U;
  s_tcp_echoed     = 0U;
  s_tcp_match      = false;
  s_tcp_need_data  = false;
  s_tcp_estab_wait = 0U;
  (void)memset(s_fw_mac, 0, sizeof(s_fw_mac));
}

/**
 * @brief One-word verdict for the TCP echo leg of the run report.
 *
 * @return "MATCH" when the echo came back byte-identical, "MISMATCH" when
 *         bytes came back but differed, "pending" when none came back.
 *
 * @pre The TCP counters reflect the finished run.
 * @post No state is modified.
 */
RA8_INTERNAL static const char* internal_net_echo_state(void)
{
  if (s_tcp_match) {
    return "MATCH";
  }
  if (s_tcp_echoed > 0U) {
    return "MISMATCH";
  }
  return "pending";
}

void board_net_report(void)
{
  if (s_state == (uint8_t)k_net_init) {
    return; /* networking never came up in this run. */
  }
  (void)priv_emu_io_errf("  NET peer      : 192.168.1.1 <-> 192.168.1.42  ARP %s  ping %s (%u)\n",
                         s_fw_mac_known ? "resolved" : "--",
                         (s_pings > 0U) ? "ok" : "--",
                         s_pings);
  (void)priv_emu_io_errf("  NET activity  : fw TX %u frame(s), RX polls %u, delivered %u\n",
                         s_tx_frames,
                         s_polls,
                         s_delivered);
  if (s_state >= (uint8_t)k_net_syn) {
    (void)priv_emu_io_errf("  NET TCP       : port 7 %s; echo %s (%u byte(s))\n",
                           (s_state >= (uint8_t)k_net_estab) ? "established + data sent"
                                                             : "SYN sent",
                           internal_net_echo_state(),
                           s_tcp_echoed);
  }
}
