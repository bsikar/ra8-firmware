/**
 * @file ra_net_internal.h
 * @brief Shared internal definitions for the ra_net stack.
 *
 * @details
 * Not part of the public API. The five ra_net_*.c translation units
 * pull in this header to share the socket / connection / ARP-cache
 * tables and the protocol header layouts. All numeric constants are
 * defined as C23 typed enums so no magic numbers leak into the wire
 * code.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra_err.h"
#include "ra_net.h"

/* NOLINTBEGIN(readability-magic-numbers,readability-redundant-casting) */

/* =============================================================================
 * Wire-protocol constants
 * =============================================================================
 */

/** @brief EtherType field values (RFC 894 / IEEE 802.3). */
typedef enum : uint16_t {
  k_eth_type_ipv4 = 0x0800U, /* RFC 791 */
  k_eth_type_arp  = 0x0806U, /* RFC 826 */
} ra_net_eth_type_t;

/** @brief IPv4 protocol numbers (IANA, RFC 790). */
typedef enum : uint8_t {
  k_ip_proto_icmp = 1U,  /* RFC 792 */
  k_ip_proto_tcp  = 6U,  /* RFC 793 */
  k_ip_proto_udp  = 17U, /* RFC 768 */
} ra_net_ip_proto_t;

/** @brief ARP opcodes (RFC 826). */
typedef enum : uint16_t {
  k_arp_op_request = 1U,
  k_arp_op_reply   = 2U,
} ra_net_arp_op_t;

/** @brief ICMP type codes (RFC 792). */
typedef enum : uint8_t {
  k_icmp_type_echo_reply   = 0U,
  k_icmp_type_echo_request = 8U,
} ra_net_icmp_type_t;

/** @brief TCP control-flag bit positions (RFC 793). */
typedef enum : uint8_t {
  k_tcp_flag_fin = 0x01U,
  k_tcp_flag_syn = 0x02U,
  k_tcp_flag_rst = 0x04U,
  k_tcp_flag_psh = 0x08U,
  k_tcp_flag_ack = 0x10U,
} ra_net_tcp_flag_t;

/** @brief Selected sizes (avoid magic numbers). */
typedef enum : uint16_t {
  k_eth_hdr_len      = 14U,
  k_arp_pkt_len      = 28U,
  k_ipv4_hdr_len     = 20U,
  k_icmp_hdr_len     = 8U,
  k_udp_hdr_len      = 8U,
  k_tcp_hdr_len      = 20U,
  k_ipv4_version_ihl = 0x45U, /**< Version 4, IHL 5 (= 20 bytes). */
  k_ipv4_default_ttl = 64U,
  k_dns_port         = 53U,
} ra_net_wire_sizes_t;

/** @brief DNS record types (RFC 1035 sec 3.2.2). */
typedef enum : uint16_t {
  k_dns_rr_a        = 1U,
  k_dns_rr_class_in = 1U,
} ra_net_dns_rr_t;

/** @brief DNS header flag bits (RFC 1035 sec 4.1.1). */
typedef enum : uint16_t {
  k_dns_flag_qr_response = 0x8000U,
  k_dns_flag_rcode_mask  = 0x000FU,
  k_dns_rcode_nxdomain   = 0x0003U,
  k_dns_flag_rd          = 0x0100U,
} ra_net_dns_flags_t;

/* =============================================================================
 * Internal data tables
 * =============================================================================
 */

/** @brief Socket kind discriminator. */
typedef enum : uint8_t {
  k_ra_net_sock_unused = 0U,
  k_ra_net_sock_udp    = 1U,
  k_ra_net_sock_tcp    = 2U,
} ra_net_sock_kind_t;

/** @brief Simplified TCP FSM (RFC 793, subset). */
typedef enum : uint8_t {
  k_tcp_state_closed       = 0U,
  k_tcp_state_listen       = 1U,
  k_tcp_state_syn_sent     = 2U,
  k_tcp_state_syn_received = 3U,
  k_tcp_state_established  = 4U,
  k_tcp_state_fin_wait     = 5U,
  k_tcp_state_close_wait   = 6U,
  k_tcp_state_last_ack     = 7U,
} ra_net_tcp_state_t;

/** @brief One ARP cache entry. */
typedef struct {
  ra_net_ipv4_t ip;
  ra_net_mac_t  mac;
  uint32_t      timestamp_ms; /**< 0 == empty. */
} ra_net_arp_entry_t;

/** @brief Tiny in-memory queue of UDP datagrams. */
typedef enum : uint8_t {
  k_ra_net_udp_max_queued = 4U,
} ra_net_udp_q_t;

typedef struct {
  ra_net_ipv4_t src_ip;
  uint16_t      src_port;
  uint16_t      len;
  uint8_t       data[512]; /**< Per-datagram cap (512B; see k_udp_msg_data_max). */
} ra_net_udp_msg_t;

/** @brief Per-socket UDP state. */
typedef struct {
  uint16_t         local_port;
  uint8_t          rd_idx;
  uint8_t          wr_idx;
  uint8_t          count;
  ra_net_udp_msg_t q[k_ra_net_udp_max_queued];
} ra_net_udp_sock_t;

/** @brief Per-socket TCP state (listen socket OR a single connection). */
typedef struct {
  ra_net_tcp_state_t state;
  uint16_t           local_port;
  ra_net_ipv4_t      remote_ip;
  uint16_t           remote_port;
  uint32_t           snd_nxt;
  uint32_t           snd_una;
  uint32_t           rcv_nxt;
  uint16_t           rx_len;
  uint16_t           tx_len;
  uint8_t            rx_buf[k_ra_net_tcp_rx_buf_size];
  uint8_t            tx_buf[k_ra_net_tcp_tx_buf_size];
} ra_net_tcp_sock_t;

/** @brief Socket table entry (tagged-union over kinds). */
typedef struct {
  ra_net_sock_kind_t kind;
  ra_net_udp_sock_t  udp;
  ra_net_tcp_sock_t  tcp;
} ra_net_socket_t;

/** @brief Stack-wide singleton (defined in ra_net_ipv4.c). */
typedef struct {
  uint8_t            opened; /**< Boolean. */
  ra_net_config_t    cfg;
  ra_net_socket_t    socks[k_ra_net_max_sockets];
  ra_net_arp_entry_t arp[k_ra_net_arp_cache_size];
  uint16_t           dns_txid;
  uint8_t            dns_pending;
  ra_net_ipv4_t      dns_answer;
  uint8_t            dns_rcode;
} ra_net_state_t;

/* Singleton accessor (defined in ra_net_ipv4.c). */
/**
 * @brief Return the singleton ra_net_state_t pointer.
 *
 * @details Used by every ra_net TU to share the socket / ARP / DNS
 *          tables. Defined in ra_net_ipv4.c.
 *
 * @return ra_net_state_t* Pointer to the singleton state. Never NULL.
 * @retval !NULL Always returns a valid pointer.
 *
 * @pre Linker has placed s_state in writable storage.
 * @pre Caller is the network thread.
 * @post No state mutation.
 * @post Returned pointer is valid for the program's lifetime.
 *
 * @note Not thread-safe; the underlying state is not lock-guarded.
 *
 * @since 0.1.0
 */
ra_net_state_t* ra_net_internal_state(void);

/** @brief Generic numeric magic-numbers (kept as named enums to satisfy
 *         readability-magic-numbers). All values are wire-protocol or
 *         layout-derived constants. */
typedef enum : uint16_t {
  k_offset_etype      = 12U,
  k_ipv4_offset_proto = 9U,
  k_ipv4_offset_csum  = 10U,
  k_ipv4_offset_src   = 12U,
  k_ipv4_offset_dst   = 16U,
  k_tcp_offset_urgent = 18U,
  k_arp_offset_op     = 6U,
  k_byte_mask_low     = 0xFFU,
  k_word_mask_low     = 0xFFFFU,
  k_ipv4_version_mask = 0xF0U,
  k_ipv4_version_v4   = 0x40U,
  k_ipv4_ihl_mask     = 0x0FU,
  k_tcp_dataoff_v5    = 0x50U,
  k_eth_addr_len      = 6U,
  k_ipv4_addr_len     = 4U,
  k_arp_hlen_eth_v    = 6U,
  k_arp_plen_ipv4_v   = 4U,
  k_arp_op_reply_id   = 2U,
  k_udp_msg_data_max  = 512U,
  k_tcp_emp_port_base = 1024U,
  k_invalid_handle_v  = 0xFFU,
} ra_net_layout_t;

typedef enum : uint32_t {
  k_shift_byte_3 = 24U,
  k_shift_byte_2 = 16U,
  k_shift_byte_1 = 8U,
  k_shift_byte_0 = 0U,
} ra_net_shifts_t;

/* Bounded-buffer helpers (project style: avoid memcpy/memset linting). */
/**
 * @brief Ra net copy bytes.
 *
 * @details See implementation for details.
 *
 * @param[in,out] dst See function signature for type and usage.
 * @param[in,out] src See function signature for type and usage.
 * @param[in,out] n See function signature for type and usage.
 *
 * @pre Caller has validated arguments.
 * @pre Module has been initialised.
 * @post Side effects bounded to documented state.
 * @post Returned value reflects current state.
 *
 * @note Not thread-safe unless documented otherwise.
 *
 * @since 0.1.0
 */
static inline void ra_net_copy_bytes(uint8_t* dst, const uint8_t* src, uint16_t n)
{
  for (uint16_t i = 0U; i < n; i++) {
    dst[i] = src[i];
  }
}

/**
 * @brief Ra net zero bytes.
 *
 * @details See implementation for details.
 *
 * @param[in,out] dst See function signature for type and usage.
 * @param[in,out] n See function signature for type and usage.
 *
 * @pre Caller has validated arguments.
 * @pre Module has been initialised.
 * @post Side effects bounded to documented state.
 * @post Returned value reflects current state.
 *
 * @note Not thread-safe unless documented otherwise.
 *
 * @since 0.1.0
 */
static inline void ra_net_zero_bytes(uint8_t* dst, uint16_t n)
{
  for (uint16_t i = 0U; i < n; i++) {
    dst[i] = 0U;
  }
}

/**
 * @brief Ra net compare bytes.
 *
 * @details See implementation for details.
 *
 * @param[in,out] a See function signature for type and usage.
 * @param[in,out] b See function signature for type and usage.
 * @param[in,out] n See function signature for type and usage.
 *
 * @return Result code or value; see implementation.
 * @retval 0 Success or default value.
 *
 * @pre Caller has validated arguments.
 * @pre Module has been initialised.
 * @post Side effects bounded to documented state.
 * @post Returned value reflects current state.
 *
 * @note Not thread-safe unless documented otherwise.
 *
 * @since 0.1.0
 */
static inline int ra_net_compare_bytes(const uint8_t* a, const uint8_t* b, uint16_t n)
{
  for (uint16_t i = 0U; i < n; i++) {
    if (a[i] != b[i]) {
      return (int)a[i] - (int)b[i];
    }
  }
  return 0;
}

/* Helpers shared across TUs ------------------------------------------------ */

/**
 * @brief Ra net be16.
 *
 * @details See implementation for details.
 *
 * @param[in,out] p See function signature for type and usage.
 *
 * @return Result code or value; see implementation.
 * @retval 0 Success or default value.
 *
 * @pre Caller has validated arguments.
 * @pre Module has been initialised.
 * @post Side effects bounded to documented state.
 * @post Returned value reflects current state.
 *
 * @note Not thread-safe unless documented otherwise.
 *
 * @since 0.1.0
 */
static inline uint16_t ra_net_be16(const uint8_t* p)
{
  return (uint16_t)((((uint16_t)p[0]) << k_shift_byte_1) | (uint16_t)p[1]);
}

/**
 * @brief Ra net put be16.
 *
 * @details See implementation for details.
 *
 * @param[in,out] p See function signature for type and usage.
 * @param[in,out] v See function signature for type and usage.
 *
 * @pre Caller has validated arguments.
 * @pre Module has been initialised.
 * @post Side effects bounded to documented state.
 * @post Returned value reflects current state.
 *
 * @note Not thread-safe unless documented otherwise.
 *
 * @since 0.1.0
 */
static inline void ra_net_put_be16(uint8_t* p, uint16_t v)
{
  p[0] = (uint8_t)(v >> k_shift_byte_1);
  p[1] = (uint8_t)(v & (uint16_t)k_byte_mask_low);
}

/**
 * @brief Ra net be32.
 *
 * @details See implementation for details.
 *
 * @param[in,out] p See function signature for type and usage.
 *
 * @return Result code or value; see implementation.
 * @retval 0 Success or default value.
 *
 * @pre Caller has validated arguments.
 * @pre Module has been initialised.
 * @post Side effects bounded to documented state.
 * @post Returned value reflects current state.
 *
 * @note Not thread-safe unless documented otherwise.
 *
 * @since 0.1.0
 */
static inline uint32_t ra_net_be32(const uint8_t* p)
{
  return (((uint32_t)p[0]) << k_shift_byte_3) | (((uint32_t)p[1]) << k_shift_byte_2) |
         (((uint32_t)p[2]) << k_shift_byte_1) | ((uint32_t)p[3]);
}

/**
 * @brief Ra net put be32.
 *
 * @details See implementation for details.
 *
 * @param[in,out] p See function signature for type and usage.
 * @param[in,out] v See function signature for type and usage.
 *
 * @pre Caller has validated arguments.
 * @pre Module has been initialised.
 * @post Side effects bounded to documented state.
 * @post Returned value reflects current state.
 *
 * @note Not thread-safe unless documented otherwise.
 *
 * @since 0.1.0
 */
static inline void ra_net_put_be32(uint8_t* p, uint32_t v)
{
  p[0] = (uint8_t)(v >> k_shift_byte_3);
  p[1] = (uint8_t)(v >> k_shift_byte_2);
  p[2] = (uint8_t)(v >> k_shift_byte_1);
  p[3] = (uint8_t)(v & (uint32_t)k_byte_mask_low);
}

/**
 * @brief Compute the RFC 1071 one's-complement Internet checksum.
 *
 * @details Folds 16-bit words and the trailing odd byte into the
 *          16-bit running total seeded by sum0 (a pseudo-header sum
 *          when computing TCP/UDP checksums). Defined in ra_net_ipv4.c.
 *
 * @param[in] data Buffer to sum. Must not be NULL when len > 0.
 * @param[in] len  Byte count.
 * @param[in] sum0 Initial running sum (typically 0 or pseudo-header).
 *
 * @return uint16_t The one's-complement checksum.
 * @retval 0xFFFF When the folded sum is zero (RFC 1071 convention).
 * @retval other  Folded one's-complement of the running sum.
 *
 * @pre data is non-NULL when len > 0.
 * @pre Caller is the network thread.
 * @post No state mutation.
 * @post Return value is independent of host byte order.
 *
 * @note Not thread-safe; pure function.
 *
 * @since 0.1.0
 */
uint16_t ra_net_checksum_ones(const uint8_t* data, uint16_t len, uint32_t sum0);

/**
 * @brief Resolve an IPv4 address through the local ARP cache.
 *
 * @details Defined in ra_net_arp.c. RFC 826.
 *
 * @param[in]  ip      IPv4 address to resolve.
 * @param[out] out_mac Filled with the resolved MAC on cache hit.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok           Cache hit; *out_mac populated.
 * @retval k_ra_err_null_ptr out_mac is NULL.
 * @retval k_ra_err_no_data  Cache miss; ARP request queued.
 *
 * @pre ra_net_open has succeeded.
 * @pre out_mac is non-NULL.
 * @post On hit *out_mac holds the MAC; on miss a request is queued.
 * @post No other state mutation.
 *
 * @note Not thread-safe; called from the network thread.
 *
 * @since 0.1.0
 */
ra_err_t ra_net_arp_resolve(ra_net_ipv4_t ip, ra_net_mac_t* out_mac);

/**
 * @brief Process an inbound ARP frame.
 *
 * @details Defined in ra_net_arp.c. RFC 826.
 *
 * @param[in] frame Ethernet+ARP frame bytes. Must not be NULL.
 * @param[in] len   Frame length.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok                Frame processed.
 * @retval k_ra_err_null_ptr      frame == NULL.
 * @retval k_ra_err_invalid_arg   Frame too short.
 * @retval k_ra_err_not_supported Non-Ethernet / non-IPv4 ARP packet.
 *
 * @pre ra_net_open has succeeded.
 * @pre frame is non-NULL.
 * @post Sender mapping cached; reply queued for ARP requests targeting our IP.
 * @post Otherwise no state mutation.
 *
 * @note Not thread-safe; called from the network thread.
 *
 * @since 0.1.0
 */
ra_err_t ra_net_arp_handle(const uint8_t* frame, uint16_t len);

/**
 * @brief Clear every ARP cache entry.
 *
 * @details Defined in ra_net_arp.c.
 *
 * @pre ra_net_open has succeeded.
 * @pre Caller is the network thread.
 * @post Every ARP cache slot is empty.
 * @post No other state mutation.
 *
 * @note Not thread-safe.
 *
 * @since 0.1.0
 */
void ra_net_arp_reset(void);

/**
 * @brief Process an inbound ICMP frame; reply to echo requests.
 *
 * @details Defined in ra_net_icmp.c. RFC 792.
 *
 * @param[in] frame Ethernet+IPv4+ICMP frame bytes. Must not be NULL.
 * @param[in] len   Frame length.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok               Frame processed.
 * @retval k_ra_err_null_ptr     frame == NULL.
 * @retval k_ra_err_invalid_arg  Frame too short / malformed.
 * @retval k_ra_err_invalid_size ICMP payload exceeds PAL frame max.
 * @retval other                 ra_net_ipv4_send error code.
 *
 * @pre ra_net_open has succeeded.
 * @pre frame is non-NULL.
 * @post Echo-request frames are answered with an echo-reply.
 * @post Other ICMP types are silently dropped.
 *
 * @note Not thread-safe.
 *
 * @since 0.1.0
 */
ra_err_t ra_net_icmp_handle(const uint8_t* frame, uint16_t len);

/**
 * @brief Process an inbound UDP frame; enqueue to a bound socket.
 *
 * @details Defined in ra_net_udp.c. RFC 768. DNS responses are
 *          additionally fed to the resolver state machine.
 *
 * @param[in] frame Ethernet+IPv4+UDP frame bytes. Must not be NULL.
 * @param[in] len   Frame length.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok               Datagram enqueued.
 * @retval k_ra_err_null_ptr     frame == NULL.
 * @retval k_ra_err_invalid_arg  Frame too short / malformed.
 * @retval k_ra_err_not_found    No socket bound to the destination port.
 * @retval k_ra_err_no_mem       Per-socket queue is full.
 *
 * @pre ra_net_open has succeeded.
 * @pre frame is non-NULL.
 * @post On success the matching socket's receive queue gained one row.
 * @post No other state mutation.
 *
 * @note Not thread-safe.
 *
 * @since 0.1.0
 */
ra_err_t ra_net_udp_handle(const uint8_t* frame, uint16_t len);

/**
 * @brief Allocate a new UDP socket bound to local_port.
 *
 * @details Defined in ra_net_udp.c.
 *
 * @param[in]  local_port Local port (must be non-zero, unique).
 * @param[out] out_handle Filled with the socket handle on success.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok              Socket allocated.
 * @retval k_ra_err_null_ptr    out_handle == NULL.
 * @retval k_ra_err_invalid_arg local_port == 0.
 * @retval k_ra_err_exists      Another socket already bound to local_port.
 * @retval k_ra_err_no_mem      Socket table is full.
 *
 * @pre ra_net_open has succeeded.
 * @pre out_handle is non-NULL.
 * @post On success a fresh socket slot holds the binding.
 * @post On failure no state mutation.
 *
 * @note Not thread-safe.
 *
 * @since 0.1.0
 */
ra_err_t ra_net_udp_socket(uint16_t local_port, ra_net_handle_t* out_handle);

/**
 * @brief Send a UDP datagram from a previously allocated socket.
 *
 * @details Defined in ra_net_udp.c. Adds UDP and IPv4 headers and
 *          pushes through the PAL.
 *
 * @param[in] h        Socket handle from ra_net_udp_socket.
 * @param[in] buf      Payload bytes (must not be NULL).
 * @param[in] len      Payload length (> 0).
 * @param[in] dst_ip   Destination IPv4 address.
 * @param[in] dst_port Destination UDP port (> 0).
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok                Datagram queued.
 * @retval k_ra_err_null_ptr      buf == NULL.
 * @retval k_ra_err_invalid_arg   dst_port == 0 or len == 0.
 * @retval k_ra_err_invalid_state Slot is not a UDP socket.
 * @retval k_ra_err_invalid_size  Payload exceeds PAL frame max.
 * @retval other                  ra_net_ipv4_send error code.
 *
 * @pre h was returned by ra_net_udp_socket.
 * @pre buf is non-NULL and len > 0.
 * @post On success a UDP datagram is queued on the PAL.
 * @post On failure no state mutation.
 *
 * @note Not thread-safe.
 *
 * @since 0.1.0
 */
ra_err_t ra_net_udp_send(ra_net_handle_t h,
                         const uint8_t*  buf,
                         uint16_t        len,
                         ra_net_ipv4_t   dst_ip,
                         uint16_t        dst_port);

/**
 * @brief Pop the oldest queued datagram for a UDP socket.
 *
 * @details Defined in ra_net_udp.c.
 *
 * @param[in]  h         Socket handle from ra_net_udp_socket.
 * @param[out] buf       Destination buffer.
 * @param[in]  max_len   Capacity of buf in bytes.
 * @param[out] got_len   Filled with the bytes copied.
 * @param[out] src_ip    Filled with the datagram's source IPv4.
 * @param[out] src_port  Filled with the datagram's source port.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok                 Datagram returned.
 * @retval k_ra_err_invalid_state  Slot is not a UDP socket.
 * @retval k_ra_err_no_data        Queue is empty.
 *
 * @pre h was returned by ra_net_udp_socket.
 * @pre All output pointers are non-NULL.
 * @post On success *got_len and the source fields are populated and
 *       the slot's queue advanced.
 * @post On failure no state mutation.
 *
 * @note Not thread-safe.
 *
 * @since 0.1.0
 */
ra_err_t ra_net_udp_recv(ra_net_handle_t h,
                         uint8_t*        buf,
                         uint16_t        max_len,
                         uint16_t*       got_len,
                         ra_net_ipv4_t*  src_ip,
                         uint16_t*       src_port);

/**
 * @brief Process an inbound TCP frame; drive the per-socket FSM.
 *
 * @details Defined in ra_net_tcp.c. RFC 793 (subset).
 *
 * @param[in] frame Ethernet+IPv4+TCP frame bytes. Must not be NULL.
 * @param[in] len   Frame length.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok               Frame processed.
 * @retval k_ra_err_null_ptr     frame == NULL.
 * @retval k_ra_err_invalid_arg  Frame too short.
 * @retval k_ra_err_not_found    No matching socket.
 * @retval other                 Whatever the per-state FSM returns.
 *
 * @pre ra_net_open has succeeded.
 * @pre frame is non-NULL.
 * @post Matching socket FSM advanced; ACK / SYN-ACK / FIN may be queued.
 * @post Otherwise no state mutation.
 *
 * @note Not thread-safe.
 *
 * @since 0.1.0
 */
ra_err_t ra_net_tcp_handle(const uint8_t* frame, uint16_t len);

/**
 * @brief Allocate a passive (LISTEN) TCP socket on local_port.
 *
 * @details Defined in ra_net_tcp.c. RFC 793 sec 3.5 LISTEN.
 *
 * @param[in]  local_port Local port (must be non-zero).
 * @param[out] out_handle Filled with the socket handle on success.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok              Socket in LISTEN.
 * @retval k_ra_err_null_ptr    out_handle == NULL.
 * @retval k_ra_err_invalid_arg local_port == 0.
 * @retval k_ra_err_no_mem      TCP socket table is full.
 *
 * @pre ra_net_open has succeeded.
 * @pre out_handle is non-NULL.
 * @post On success the socket is in LISTEN.
 * @post On failure no state mutation.
 *
 * @note Not thread-safe.
 *
 * @since 0.1.0
 */
ra_err_t ra_net_tcp_listen(uint16_t local_port, ra_net_handle_t* out_handle);

/**
 * @brief Initiate an active (SYN_SENT) TCP open.
 *
 * @details Defined in ra_net_tcp.c. RFC 793 sec 3.4 / 3.5.
 *
 * @param[in]  remote_ip   Destination IPv4 address.
 * @param[in]  remote_port Destination TCP port (> 0).
 * @param[out] out_handle  Filled with the socket handle on success.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok              SYN sent.
 * @retval k_ra_err_null_ptr    out_handle == NULL.
 * @retval k_ra_err_invalid_arg remote_port == 0.
 * @retval k_ra_err_no_mem      TCP socket table is full.
 * @retval other                ra_net_ipv4_send error code.
 *
 * @pre ra_net_open has succeeded.
 * @pre out_handle is non-NULL.
 * @post On success the socket is in SYN_SENT and a SYN has been queued.
 * @post On failure the socket slot is left clean.
 *
 * @note Not thread-safe.
 *
 * @since 0.1.0
 */
ra_err_t
ra_net_tcp_connect(ra_net_ipv4_t remote_ip, uint16_t remote_port, ra_net_handle_t* out_handle);

/**
 * @brief Queue and transmit application data on an established TCP socket.
 *
 * @details Defined in ra_net_tcp.c. RFC 793 sec 3.7 SEND.
 *
 * @param[in] h   Socket handle.
 * @param[in] buf Payload bytes (must not be NULL).
 * @param[in] len Payload length.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok                Segment queued.
 * @retval k_ra_err_null_ptr      buf == NULL.
 * @retval k_ra_err_invalid_state Socket is not in ESTABLISHED.
 * @retval k_ra_err_no_mem        TX buffer is full.
 * @retval other                  ra_net_ipv4_send error code.
 *
 * @pre h was returned by ra_net_tcp_connect / ra_net_tcp_listen.
 * @pre buf is non-NULL.
 * @post On success snd_nxt advances by len and a PSH+ACK segment is queued.
 * @post On failure no state mutation.
 *
 * @note Not thread-safe.
 *
 * @since 0.1.0
 */
ra_err_t ra_net_tcp_send(ra_net_handle_t h, const uint8_t* buf, uint16_t len);

/**
 * @brief Drain bytes from an established TCP socket's RX buffer.
 *
 * @details Defined in ra_net_tcp.c. RFC 793 sec 3.7 RECEIVE.
 *
 * @param[in]  h         Socket handle.
 * @param[out] buf       Destination buffer.
 * @param[in]  max_len   Capacity of buf in bytes.
 * @param[out] got_len   Filled with the bytes copied.
 * @param[out] peer_ip   Filled with the connected peer IPv4.
 * @param[out] peer_port Filled with the connected peer port.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok                Bytes returned.
 * @retval k_ra_err_invalid_state Socket is not a TCP socket.
 * @retval k_ra_err_no_data       RX buffer is empty.
 *
 * @pre h was returned by a TCP open call.
 * @pre All output pointers are non-NULL.
 * @post On success bytes have been removed from the per-socket RX buffer.
 * @post On failure no state mutation.
 *
 * @note Not thread-safe.
 *
 * @since 0.1.0
 */
ra_err_t ra_net_tcp_recv(ra_net_handle_t h,
                         uint8_t*        buf,
                         uint16_t        max_len,
                         uint16_t*       got_len,
                         ra_net_ipv4_t*  peer_ip,
                         uint16_t*       peer_port);

/**
 * @brief Initiate the active-close path on a TCP socket.
 *
 * @details Defined in ra_net_tcp.c. RFC 793 sec 3.5 CLOSE.
 *
 * @param[in] h Socket handle.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok               Close initiated (or fully torn down).
 * @retval k_ra_err_invalid_arg  Slot is not a TCP socket.
 * @retval other                 ra_net_ipv4_send error code.
 *
 * @pre h was returned by a TCP open call.
 * @pre Caller is the network thread.
 * @post On success the FSM is in FIN_WAIT, LAST_ACK, or fully closed.
 * @post On failure no state mutation.
 *
 * @note Not thread-safe.
 *
 * @since 0.1.0
 */
ra_err_t ra_net_tcp_close(ra_net_handle_t h);

/* =============================================================================
 * Internal helpers exposed for MC/DC test access only.
 *
 * @par MC/DC:
 * These symbols are TU-private in spirit (not part of the public ra_net
 * API surface) but have been promoted to external linkage so that
 * tests/test_ra_net_udp.c can drive their compound boolean decisions
 * directly under -fcoverage-mcdc. Production callers MUST keep using the
 * public ra_net_*() facade. See CLAUDE.md "Test access to internal
 * symbols (MC/DC scope)".
 * =============================================================================
 */

/**
 * @brief Locate the UDP socket bound to a local port.
 *
 * @details Linear scan over the socket table. Promoted from TU-private
 *          static linkage so tests can drive its compound boolean
 *          decision under -fcoverage-mcdc.
 *
 * @param[in] port Local UDP port.
 * @return int16_t Slot index, or -1 on no match.
 * @retval -1 No matching UDP socket.
 * @retval >=0 Slot index of the bound socket.
 * @pre ra_net_open has succeeded.
 * @pre Caller is the network thread.
 * @post No state mutation.
 * @post Returned index is valid for ra_net_internal_state()->socks.
 * @note Test-access only; use ra_net_*() in production.
 * @par MC/DC:
 * Exposed so tests exercise the
 * `if (kind == k_ra_net_sock_udp && local_port == port)` decision on
 * the production source line, not on a mirror copy.
 * @since 0.1.0
 */
int16_t ra_net_udp_internal_find_socket_by_port(uint16_t port);

/**
 * @brief Walk a DNS response and capture the first A record.
 *
 * @details RFC 1035 sec 4.1 -- skips the question section, walks the
 *          answers, and stores the first IN-A record into the
 *          singleton's dns_answer field. Promoted from TU-private
 *          static linkage so tests can drive its qname-loop guard,
 *          root-null check, and A-record selector under -fcoverage-mcdc.
 *
 * @param[in] dns  DNS message bytes (UDP payload). Must not be NULL.
 * @param[in] dlen Message length.
 * @pre dns is non-NULL.
 * @pre s_state.dns_pending == 1.
 * @post On success s_state.dns_answer holds the resolved IPv4 and
 *       s_state.dns_pending == 0.
 * @post On failure s_state.dns_pending may stay 1 (caller times out).
 * @note Test-access only; production goes through ra_net_dns_query().
 * @par MC/DC:
 * Exposed so tests exercise the qname loop guards, root-null check,
 * and 3-condition A-record selector on production source.
 * @since 0.1.0
 */
void ra_net_udp_internal_dns_consume_response(const uint8_t* dns, uint16_t dlen);

/**
 * @brief Locate the TCP socket matching the given 4-tuple.
 *
 * @details Walks the socket table, returning the first slot whose
 *          local_port (and, when not in LISTEN, remote_port + remote_ip)
 *          matches. Promoted from TU-private static linkage so tests
 *          can drive its compound boolean decisions under MC/DC.
 *
 * @param[in] local_port  Local TCP port to match.
 * @param[in] remote_ip   Remote IP to match (ignored for LISTEN slots).
 * @param[in] remote_port Remote TCP port to match (ignored for LISTEN).
 * @param[in] want_state  State to require when match_state != 0.
 * @param[in] match_state Non-zero to require want_state, 0 for any.
 *
 * @return int16_t Slot index, or -1 on no match.
 * @retval -1   No matching socket.
 * @retval >=0  Slot index of the matching socket.
 *
 * @pre ra_net_open has succeeded.
 * @pre Caller is the network thread.
 * @post No state mutation.
 * @post Returned index is a valid index into ra_net_internal_state()->socks.
 *
 * @note Test-access only.
 *
 * @par MC/DC:
 * Exposes the line-111 (match_state && state!=want) and line-117
 * (remote_port match && memcmp ip == 0) decisions on production source.
 *
 * @since 0.1.0
 */
int16_t ra_net_tcp_internal_find_socket(uint16_t           local_port,
                                        ra_net_ipv4_t      remote_ip,
                                        uint16_t           remote_port,
                                        ra_net_tcp_state_t want_state,
                                        uint8_t            match_state);

/**
 * @brief Build and transmit one TCP segment for the given socket.
 *
 * @details Writes the 20-byte TCP header, optional payload, computes
 *          the checksum, and pushes through ra_net_ipv4_send. Promoted
 *          from TU-private static linkage so tests can drive its
 *          payload-copy compound decision under MC/DC.
 *
 * @param[in] t        Socket whose remote/local ports drive the header.
 * @param[in] flags    TCP control flags (SYN/ACK/etc.).
 * @param[in] data     Optional payload (NULL when data_len == 0).
 * @param[in] data_len Payload length.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok               Segment queued.
 * @retval k_ra_err_invalid_size Header+payload exceeds PAL frame max.
 * @retval other                 ra_net_ipv4_send error code.
 *
 * @pre t is non-NULL and references a valid TCP socket.
 * @pre data is non-NULL when data_len > 0.
 * @post On success the PAL has the segment queued.
 * @post On failure no state mutation.
 *
 * @note Test-access only.
 *
 * @par MC/DC:
 * Exposes the line-204 ``(data_len != 0U) && (data != nullptr)``
 * decision on production source.
 *
 * @since 0.1.0
 */
ra_err_t ra_net_tcp_internal_emit_segment(ra_net_tcp_sock_t* t,
                                          uint8_t            flags,
                                          const uint8_t*     data,
                                          uint16_t           data_len);

/* IPv4 helpers used by sub-protocols when constructing TX frames. */
/**
 * @brief Build an IPv4 frame around payload and emit it via the PAL.
 *
 * @details Defined in ra_net_ipv4.c. RFC 791. Resolves the next-hop
 *          MAC through the ARP cache (or falls back to broadcast on
 *          unresolved); fills in IPv4 header (no options, TTL 64);
 *          calls the PAL TX path.
 *
 * @param[in] dst         Destination IPv4 address.
 * @param[in] proto       IPv4 protocol number (ICMP/UDP/TCP).
 * @param[in] payload     Upper-layer payload (must not be NULL).
 * @param[in] payload_len Payload length.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok                Frame queued.
 * @retval k_ra_err_null_ptr      payload == NULL.
 * @retval k_ra_err_invalid_state Stack not opened.
 * @retval k_ra_err_invalid_size  Combined header+payload exceeds PAL frame max.
 * @retval other                  ra_net_pal_send_frame error code.
 *
 * @pre ra_net_open has succeeded.
 * @pre payload is non-NULL.
 * @post On success a complete Ethernet+IPv4 frame is queued on the PAL.
 * @post On failure no state mutation.
 *
 * @note Not thread-safe.
 *
 * @since 0.1.0
 */
ra_err_t ra_net_ipv4_send(ra_net_ipv4_t     dst,
                          ra_net_ip_proto_t proto,
                          const uint8_t*    payload,
                          uint16_t          payload_len);

/* NOLINTEND(readability-magic-numbers,readability-redundant-casting) */

/**
 * @brief Pure predicate: position in range AND DNS byte equals target.
 *
 * @details Reusable for the line-539 ``pos < dlen && dns[pos] == 0``
 *          and line-554 ``pos < dlen && dns[pos] != 0`` decisions in
 *          libs/ra_net/src/ra_net_udp.c.
 *
 * @param[in] in_range     Non-zero iff pos < dlen.
 * @param[in] byte_value   Value of dns[pos] when in_range; ignored otherwise.
 * @param[in] target_value Comparison target (0 for both call sites).
 * @param[in] equal        Non-zero for ==, zero for !=.
 *
 * @return Boolean compound predicate.
 * @retval true  Predicate held.
 * @retval false Predicate failed.
 *
 * @pre None.
 * @pre None.
 * @post No state mutated.
 * @post Return depends solely on the four inputs.
 *
 * @note Test-access only. Pure function.
 *
 * @par MC/DC:
 * 2-condition AND; N+1 = 3 vectors per @p equal mode.
 *
 * @since 0.1.0
 */
bool ra_net_internal_dns_byte_match(uint8_t in_range,
                                    uint8_t byte_value,
                                    uint8_t target_value,
                                    uint8_t equal);

/**
 * @brief Pure predicate: DNS query still pending AND poll budget left.
 *
 * @details Promoted from the inline 3-condition AND at
 *          libs/ra_net/src/ra_net_udp.c inside @c ra_net_dns_query.
 *
 * @param[in] dns_pending Non-zero while the DNS request is outstanding.
 * @param[in] dns_rcode   Non-zero once a reply has set the RCODE.
 * @param[in] poll_budget Remaining poll iterations.
 *
 * @return Boolean continue-loop predicate.
 * @retval true  Loop body must execute another iteration.
 * @retval false Loop terminates.
 *
 * @pre None.
 * @pre None.
 * @post No state mutated.
 * @post Return depends solely on the three inputs.
 *
 * @note Test-access only. Pure function.
 *
 * @par MC/DC:
 * 3-condition AND; N+1 = 4 vectors:
 *  - V1: pending=1, rcode=0,  budget>0 -> true  (control)
 *  - V2: pending=0, rcode=0,  budget>0 -> false (varies pending)
 *  - V3: pending=1, rcode!=0, budget>0 -> false (varies rcode)
 *  - V4: pending=1, rcode=0,  budget=0 -> false (varies budget)
 *
 * @since 0.1.0
 */
bool ra_net_internal_dns_loop_active(uint8_t dns_pending, uint8_t dns_rcode, uint32_t poll_budget);

#ifdef __cplusplus
}
#endif
