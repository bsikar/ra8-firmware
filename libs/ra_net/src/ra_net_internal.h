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
static inline void ra_net_copy_bytes(uint8_t* dst, const uint8_t* src, uint16_t n)
{
  for (uint16_t i = 0U; i < n; i++) {
    dst[i] = src[i];
  }
}

static inline void ra_net_zero_bytes(uint8_t* dst, uint16_t n)
{
  for (uint16_t i = 0U; i < n; i++) {
    dst[i] = 0U;
  }
}

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

static inline uint16_t ra_net_be16(const uint8_t* p)
{
  return (uint16_t)((((uint16_t)p[0]) << k_shift_byte_1) | (uint16_t)p[1]);
}

static inline void ra_net_put_be16(uint8_t* p, uint16_t v)
{
  p[0] = (uint8_t)(v >> k_shift_byte_1);
  p[1] = (uint8_t)(v & (uint16_t)k_byte_mask_low);
}

static inline uint32_t ra_net_be32(const uint8_t* p)
{
  return (((uint32_t)p[0]) << k_shift_byte_3) | (((uint32_t)p[1]) << k_shift_byte_2) |
         (((uint32_t)p[2]) << k_shift_byte_1) | ((uint32_t)p[3]);
}

static inline void ra_net_put_be32(uint8_t* p, uint32_t v)
{
  p[0] = (uint8_t)(v >> k_shift_byte_3);
  p[1] = (uint8_t)(v >> k_shift_byte_2);
  p[2] = (uint8_t)(v >> k_shift_byte_1);
  p[3] = (uint8_t)(v & (uint32_t)k_byte_mask_low);
}

uint16_t ra_net_checksum_ones(const uint8_t* data, uint16_t len, uint32_t sum0);

ra_err_t ra_net_arp_resolve(ra_net_ipv4_t ip, ra_net_mac_t* out_mac);
ra_err_t ra_net_arp_handle(const uint8_t* frame, uint16_t len);
void     ra_net_arp_reset(void);

ra_err_t ra_net_icmp_handle(const uint8_t* frame, uint16_t len);

ra_err_t ra_net_udp_handle(const uint8_t* frame, uint16_t len);
ra_err_t ra_net_udp_socket(uint16_t local_port, ra_net_handle_t* out_handle);
ra_err_t ra_net_udp_send(ra_net_handle_t h,
                         const uint8_t*  buf,
                         uint16_t        len,
                         ra_net_ipv4_t   dst_ip,
                         uint16_t        dst_port);
ra_err_t ra_net_udp_recv(ra_net_handle_t h,
                         uint8_t*        buf,
                         uint16_t        max_len,
                         uint16_t*       got_len,
                         ra_net_ipv4_t*  src_ip,
                         uint16_t*       src_port);

ra_err_t ra_net_tcp_handle(const uint8_t* frame, uint16_t len);
ra_err_t ra_net_tcp_listen(uint16_t local_port, ra_net_handle_t* out_handle);
ra_err_t
ra_net_tcp_connect(ra_net_ipv4_t remote_ip, uint16_t remote_port, ra_net_handle_t* out_handle);
ra_err_t ra_net_tcp_send(ra_net_handle_t h, const uint8_t* buf, uint16_t len);
ra_err_t ra_net_tcp_recv(ra_net_handle_t h,
                         uint8_t*        buf,
                         uint16_t        max_len,
                         uint16_t*       got_len,
                         ra_net_ipv4_t*  peer_ip,
                         uint16_t*       peer_port);
ra_err_t ra_net_tcp_close(ra_net_handle_t h);

/* IPv4 helpers used by sub-protocols when constructing TX frames. */
ra_err_t ra_net_ipv4_send(ra_net_ipv4_t     dst,
                          ra_net_ip_proto_t proto,
                          const uint8_t*    payload,
                          uint16_t          payload_len);

/* NOLINTEND(readability-magic-numbers,readability-redundant-casting) */

#ifdef __cplusplus
}
#endif
