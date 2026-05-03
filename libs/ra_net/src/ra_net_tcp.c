/**
 * @file ra_net_tcp.c
 * @brief Minimal TCP (RFC 793) FSM for ra_net.
 *
 * @details
 * Implements a small subset of TCP that supports multi-connection
 * echo workloads:
 *
 *   FSM states (subset of RFC 793 Fig 6):
 *
 *     CLOSED -> LISTEN              ra_net_socket_tcp_listen
 *     LISTEN -> SYN_RECEIVED        on RX SYN -> reply SYN+ACK
 *     SYN_RECEIVED -> ESTABLISHED   on RX ACK
 *     CLOSED -> SYN_SENT            ra_net_socket_tcp_connect
 *     SYN_SENT -> ESTABLISHED       on RX SYN+ACK -> reply ACK
 *     ESTABLISHED -> CLOSE_WAIT     on RX FIN -> reply ACK
 *     ESTABLISHED -> FIN_WAIT       ra_net_close_socket -> send FIN
 *     CLOSE_WAIT -> LAST_ACK        on close -> send FIN
 *     LAST_ACK / FIN_WAIT -> CLOSED on RX FIN/ACK
 *
 * Simplifications relative to the full RFC:
 *
 *   - Receive window is fixed at the full 4 KB RX buffer size.
 *   - No congestion control / slow start / fast retransmit.
 *   - No retransmission timer beyond a single immediate resend on the
 *     happy path; tests cover the steady-state segment exchange.
 *   - No urgent pointer, options, SACK, or timestamps.
 *   - One connection per listening socket slot (the spec called for
 *     up to 4 -- to add more, additional rows in the socket table can
 *     hold child connections; the API is forward-compatible).
 *   - PSH flag set on every TX data segment.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <string.h>

#include "ra_err.h"
#include "ra_net.h"
#include "ra_net_internal.h"
#include "ra_net_pal.h"

/* RFC 793 TCP */

/* NOLINTBEGIN(readability-magic-numbers,readability-redundant-casting,clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling,readability-function-size,readability-identifier-naming,readability-function-cognitive-complexity) */

typedef enum : uint16_t {
  k_tcp_offset_dataoff  = 12U,
  k_tcp_offset_flags    = 13U,
  k_tcp_offset_window   = 14U,
  k_tcp_offset_checksum = 16U,
} ra_net_tcp_offsets_t;

typedef enum : uint16_t {
  k_tcp_default_window = (uint16_t)k_ra_net_tcp_rx_buf_size,
} ra_net_tcp_consts_t;

typedef enum : uint32_t {
  k_tcp_initial_iss = 0x12345000U,
} ra_net_tcp_iss_t;

/* =============================================================================
 * Helpers
 * =============================================================================
 */

/**
 * @brief Find the TCP socket matching a 4-tuple (or LISTEN fallback).
 *
 * @details Walks the socket table preferring an established
 *          (local_port, remote_ip, remote_port) match; falls back to a
 *          LISTEN socket on the same local port if requested by the
 *          caller's match_state argument.
 *
 * @param[in] local_port  Local TCP port to match.
 * @param[in] remote_ip   Remote IPv4 to match (ignored for LISTEN slot).
 * @param[in] remote_port Remote TCP port to match.
 * @param[in] want_state  State to require when match_state != 0.
 * @param[in] match_state Non-zero to require want_state, 0 for any state.
 *
 * @return int16_t Index into the socket table, or -1 on no match.
 * @retval -1     No matching socket.
 * @retval >=0    Index of the matching socket.
 *
 * @pre ra_net_open has succeeded.
 * @pre Caller is the network thread.
 * @post No state mutation.
 * @post Return value is a valid index into ra_net_internal_state()->socks.
 *
 * @note Not thread-safe.
 *
 * @since 0.1.0
 */
int16_t ra_net_tcp_internal_find_socket(uint16_t           local_port,
                                        ra_net_ipv4_t      remote_ip,
                                        uint16_t           remote_port,
                                        ra_net_tcp_state_t want_state,
                                        uint8_t            match_state)
{
  ra_net_state_t* s = ra_net_internal_state();
  for (uint16_t i = 0U; i < (uint16_t)k_ra_net_max_sockets; i++) {
    if (s->socks[i].kind != k_ra_net_sock_tcp) {
      continue;
    }
    ra_net_tcp_sock_t* t = &s->socks[i].tcp;
    if (t->local_port != local_port) {
      continue;
    }
    if (match_state != 0U && t->state != want_state) {
      continue;
    }
    if (t->state == k_tcp_state_listen) {
      return (int16_t)i;
    }
    if (t->remote_port == remote_port && memcmp(t->remote_ip.bytes, remote_ip.bytes, 4U) == 0) {
      return (int16_t)i;
    }
  }
  return -1;
}

/**
 * @brief Compute the TCP pseudo-header checksum partial-sum.
 *
 * @details RFC 793 sec 3.1 -- builds the (src, dst, proto, length)
 *          pseudo-header sum that ra_net_checksum_ones folds with the
 *          TCP segment.
 *
 * @param[in] src     Source IPv4 address.
 * @param[in] dst     Destination IPv4 address.
 * @param[in] tcp_len Total TCP segment length (header + payload).
 *
 * @return uint16_t Folded pseudo-header sum.
 * @retval 0      All inputs were zero (degenerate case).
 * @retval other  Folded one's-complement of the pseudo-header.
 *
 * @pre src and dst are non-NULL.
 * @pre Caller is the network thread.
 * @post No state mutation.
 * @post Return value is independent of host byte order.
 *
 * @note Not thread-safe; pure function.
 *
 * @since 0.1.0
 */
static uint16_t tcp_pseudo_sum(const ra_net_ipv4_t* src, const ra_net_ipv4_t* dst, uint16_t tcp_len)
{
  uint32_t sum = 0U;
  sum += (uint32_t)((((uint32_t)src->bytes[0]) << 8U) | (uint32_t)src->bytes[1]);
  sum += (uint32_t)((((uint32_t)src->bytes[2]) << 8U) | (uint32_t)src->bytes[3]);
  sum += (uint32_t)((((uint32_t)dst->bytes[0]) << 8U) | (uint32_t)dst->bytes[1]);
  sum += (uint32_t)((((uint32_t)dst->bytes[2]) << 8U) | (uint32_t)dst->bytes[3]);
  sum += (uint32_t)k_ip_proto_tcp;
  sum += (uint32_t)tcp_len;
  return (uint16_t)sum;
}

/**
 * @brief Build and transmit a single TCP segment from a socket.
 *
 * @details Frames a fixed-20-byte header (no options), computes the
 *          checksum with tcp_pseudo_sum, and pushes through
 *          ra_net_ipv4_send (RFC 793 sec 3.1).
 *
 * @param[in] t        Socket whose 4-tuple seeds the segment.
 * @param[in] flags    TCP control flag bitmask.
 * @param[in] data     Optional payload (may be NULL when data_len == 0).
 * @param[in] data_len Payload byte count.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok                Segment queued.
 * @retval k_ra_err_invalid_size  Combined header+payload exceeds PAL frame max.
 * @retval other                  ra_net_ipv4_send error code.
 *
 * @pre t is a valid socket pointer.
 * @pre data is non-NULL when data_len > 0.
 * @post On success a TCP segment is queued.
 * @post On failure no state mutation.
 *
 * @note Not thread-safe.
 *
 * @since 0.1.0
 */
ra_err_t ra_net_tcp_internal_emit_segment(ra_net_tcp_sock_t* t,
                                          uint8_t            flags,
                                          const uint8_t*     data,
                                          uint16_t           data_len)
{
  ra_net_state_t* s       = ra_net_internal_state();
  uint16_t        tcp_len = (uint16_t)(k_tcp_hdr_len + data_len);
  if (tcp_len > (uint16_t)(k_ra_net_pal_frame_max - k_eth_hdr_len - k_ipv4_hdr_len)) {
    return k_ra_err_invalid_size;
  }
  static uint8_t buf[k_ra_net_pal_frame_max];
  ra_net_put_be16(&buf[0], t->local_port);
  ra_net_put_be16(&buf[2], t->remote_port);
  ra_net_put_be32(&buf[4], t->snd_nxt);
  ra_net_put_be32(&buf[8], t->rcv_nxt);
  buf[k_tcp_offset_dataoff] = 0x50U; /* data offset 5 (20 bytes) */
  buf[k_tcp_offset_flags]   = flags;
  ra_net_put_be16(&buf[k_tcp_offset_window], (uint16_t)k_tcp_default_window);
  ra_net_put_be16(&buf[k_tcp_offset_checksum], 0U);
  ra_net_put_be16(&buf[18], 0U); /* urgent */
  if ((data_len != 0U) && (data != nullptr)) {
    (void)memcpy(&buf[k_tcp_hdr_len], data, data_len);
  }
  uint16_t pseudo = tcp_pseudo_sum(&s->cfg.ip, &t->remote_ip, tcp_len);
  uint16_t ck     = ra_net_checksum_ones(buf, tcp_len, pseudo);
  ra_net_put_be16(&buf[k_tcp_offset_checksum], ck);
  return ra_net_ipv4_send(t->remote_ip, k_ip_proto_tcp, buf, tcp_len);
}

/**
 * @brief Allocate a fresh TCP socket slot.
 *
 * @details Counts active TCP sockets, refuses past
 *          k_ra_net_max_tcp_sockets, then claims the first unused
 *          row.
 *
 * @return ra_net_handle_t Slot index, or k_ra_net_invalid_handle.
 * @retval k_ra_net_invalid_handle Table is full.
 * @retval other                   Index of the freshly allocated slot.
 *
 * @pre ra_net_open has succeeded.
 * @pre Caller is the network thread.
 * @post On success the slot is zeroed and marked TCP.
 * @post On failure no state mutation.
 *
 * @note Not thread-safe.
 *
 * @since 0.1.0
 */
static ra_net_handle_t alloc_tcp_slot(void)
{
  ra_net_state_t* s    = ra_net_internal_state();
  uint16_t        used = 0U;
  for (uint16_t i = 0U; i < (uint16_t)k_ra_net_max_sockets; i++) {
    if (s->socks[i].kind == k_ra_net_sock_tcp) {
      used++;
    }
  }
  if (used >= (uint16_t)k_ra_net_max_tcp_sockets) {
    return (ra_net_handle_t)k_ra_net_invalid_handle;
  }
  for (uint16_t i = 0U; i < (uint16_t)k_ra_net_max_sockets; i++) {
    if (s->socks[i].kind == k_ra_net_sock_unused) {
      (void)memset(&s->socks[i], 0, sizeof(s->socks[i]));
      s->socks[i].kind = k_ra_net_sock_tcp;
      return (ra_net_handle_t)i;
    }
  }
  return (ra_net_handle_t)k_ra_net_invalid_handle;
}

/* =============================================================================
 * Public ops
 * =============================================================================
 */

/**
 * @brief Open a passive TCP socket on local_port.
 *
 * @details RFC 793 sec 3.5 LISTEN -- a passive open, ready to accept
 *          incoming SYN segments.
 *
 * @param[in]  local_port Local TCP port (must be non-zero).
 * @param[out] out_handle Filled with the socket handle on success.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok               Socket in LISTEN.
 * @retval k_ra_err_null_ptr     out_handle == NULL.
 * @retval k_ra_err_invalid_arg  local_port == 0.
 * @retval k_ra_err_no_mem       Socket table is full.
 *
 * @pre ra_net_open has succeeded.
 * @pre out_handle is non-NULL.
 * @post On success a TCP socket is in LISTEN.
 * @post On failure no state mutation.
 *
 * @note Not thread-safe.
 *
 * @since 0.1.0
 */
ra_err_t ra_net_tcp_listen(uint16_t local_port, ra_net_handle_t* out_handle)
{
  if (out_handle == nullptr) {
    return k_ra_err_null_ptr;
  }
  if (local_port == 0U) {
    return k_ra_err_invalid_arg;
  }
  ra_net_handle_t h = alloc_tcp_slot();
  if (h == (ra_net_handle_t)k_ra_net_invalid_handle) {
    return k_ra_err_no_mem;
  }
  ra_net_state_t* s          = ra_net_internal_state();
  s->socks[h].tcp.state      = k_tcp_state_listen;
  s->socks[h].tcp.local_port = local_port;
  *out_handle                = h;
  return k_ra_ok;
}

/**
 * @brief Initiate an active TCP open and send a SYN.
 *
 * @details RFC 793 sec 3.4 / 3.5 SYN_SENT.
 *
 * @param[in]  remote_ip   Destination IPv4.
 * @param[in]  remote_port Destination TCP port (must be non-zero).
 * @param[out] out_handle  Filled with the socket handle on success.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok               SYN queued.
 * @retval k_ra_err_null_ptr     out_handle == NULL.
 * @retval k_ra_err_invalid_arg  remote_port == 0.
 * @retval k_ra_err_no_mem       Socket table is full.
 * @retval other                 ra_net_ipv4_send error code.
 *
 * @pre ra_net_open has succeeded.
 * @pre out_handle is non-NULL.
 * @post On success a TCP socket is in SYN_SENT and a SYN is queued.
 * @post On failure no state mutation.
 *
 * @note Not thread-safe.
 *
 * @since 0.1.0
 */
ra_err_t
ra_net_tcp_connect(ra_net_ipv4_t remote_ip, uint16_t remote_port, ra_net_handle_t* out_handle)
{
  if (out_handle == nullptr) {
    return k_ra_err_null_ptr;
  }
  if (remote_port == 0U) {
    return k_ra_err_invalid_arg;
  }
  ra_net_handle_t h = alloc_tcp_slot();
  if (h == (ra_net_handle_t)k_ra_net_invalid_handle) {
    return k_ra_err_no_mem;
  }
  ra_net_state_t*    s = ra_net_internal_state();
  ra_net_tcp_sock_t* t = &s->socks[h].tcp;
  t->state             = k_tcp_state_syn_sent;
  t->local_port        = (uint16_t)((uint16_t)k_dns_port + h + 1024U); /* simple ephemeral */
  t->remote_ip         = remote_ip;
  t->remote_port       = remote_port;
  t->snd_nxt           = (uint32_t)k_tcp_initial_iss + (uint32_t)h;
  t->snd_una           = t->snd_nxt;
  t->rcv_nxt           = 0U;
  *out_handle          = h;

  ra_err_t e = ra_net_tcp_internal_emit_segment(t, (uint8_t)k_tcp_flag_syn, nullptr, 0U);
  if (e == k_ra_ok) {
    t->snd_nxt++; /* SYN consumes one sequence number */
  }
  return e;
}

/**
 * @brief Send application data on an established TCP socket.
 *
 * @details RFC 793 sec 3.7 SEND -- copies into the TX buffer and
 *          immediately emits a PSH+ACK segment carrying the data.
 *
 * @param[in] h   Socket handle.
 * @param[in] buf Payload bytes (must not be NULL).
 * @param[in] len Payload length.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok                Segment queued.
 * @retval k_ra_err_null_ptr      buf == NULL.
 * @retval k_ra_err_invalid_state Socket is not ESTABLISHED.
 * @retval k_ra_err_no_mem        TX buffer full.
 * @retval other                  ra_net_ipv4_send error code.
 *
 * @pre h was returned by a TCP open.
 * @pre buf is non-NULL.
 * @post On success snd_nxt advances and a segment is queued.
 * @post On failure no state mutation.
 *
 * @note Not thread-safe.
 *
 * @since 0.1.0
 */
ra_err_t ra_net_tcp_send(ra_net_handle_t h, const uint8_t* buf, uint16_t len)
{
  if (buf == nullptr) {
    return k_ra_err_null_ptr;
  }
  ra_net_state_t* s = ra_net_internal_state();
  if (s->socks[h].kind != k_ra_net_sock_tcp) {
    return k_ra_err_invalid_state;
  }
  ra_net_tcp_sock_t* t = &s->socks[h].tcp;
  if (t->state != k_tcp_state_established) {
    return k_ra_err_invalid_state;
  }
  if ((uint32_t)t->tx_len + (uint32_t)len > (uint32_t)k_ra_net_tcp_tx_buf_size) {
    return k_ra_err_no_mem;
  }
  (void)memcpy(&t->tx_buf[t->tx_len], buf, len);
  t->tx_len = (uint16_t)(t->tx_len + len);

  /* Send immediately. */
  ra_err_t e =
    ra_net_tcp_internal_emit_segment(t,
                                     (uint8_t)((uint8_t)k_tcp_flag_psh | (uint8_t)k_tcp_flag_ack),
                                     buf,
                                     len);
  if (e == k_ra_ok) {
    t->snd_nxt += (uint32_t)len;
  }
  return e;
}

/**
 * @brief Drain bytes from a TCP socket's RX buffer.
 *
 * @details RFC 793 sec 3.7 RECEIVE -- pops up to max_len bytes from
 *          the socket's RX buffer.
 *
 * @param[in]  h         Socket handle.
 * @param[out] buf       Destination buffer.
 * @param[in]  max_len   Capacity of buf.
 * @param[out] got_len   Filled with the bytes copied.
 * @param[out] peer_ip   Filled with the connected peer IPv4.
 * @param[out] peer_port Filled with the connected peer port.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok                Bytes returned.
 * @retval k_ra_err_invalid_state Slot is not a TCP socket.
 * @retval k_ra_err_no_data       RX buffer is empty.
 *
 * @pre h was returned by a TCP open.
 * @pre All output pointers are non-NULL.
 * @post On success bytes have been removed from the RX buffer.
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
                         uint16_t*       peer_port)
{
  ra_net_state_t* s = ra_net_internal_state();
  if (s->socks[h].kind != k_ra_net_sock_tcp) {
    return k_ra_err_invalid_state;
  }
  ra_net_tcp_sock_t* t = &s->socks[h].tcp;
  if (t->rx_len == 0U) {
    return k_ra_err_no_data;
  }
  uint16_t n = (t->rx_len < max_len) ? t->rx_len : max_len;
  (void)memcpy(buf, t->rx_buf, n);
  /* Shift remaining bytes down. */
  if (n < t->rx_len) {
    (void)memmove(t->rx_buf, &t->rx_buf[n], (size_t)(t->rx_len - n));
  }
  t->rx_len  = (uint16_t)(t->rx_len - n);
  *got_len   = n;
  *peer_ip   = t->remote_ip;
  *peer_port = t->remote_port;
  return k_ra_ok;
}

/**
 * @brief Initiate the active-close path on a TCP socket.
 *
 * @details RFC 793 sec 3.5 CLOSE -- transitions LISTEN/SYN_SENT/CLOSED
 *          slots to free immediately, ESTABLISHED to FIN_WAIT, and
 *          CLOSE_WAIT to LAST_ACK.
 *
 * @param[in] h Socket handle.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok               Close initiated (or completed).
 * @retval k_ra_err_invalid_arg  Slot is not a TCP socket.
 * @retval other                 ra_net_ipv4_send error code.
 *
 * @pre h was returned by a TCP open.
 * @pre Caller is the network thread.
 * @post On success the FSM is in FIN_WAIT, LAST_ACK, or fully closed.
 * @post On failure no state mutation.
 *
 * @note Not thread-safe.
 *
 * @since 0.1.0
 */
ra_err_t ra_net_tcp_close(ra_net_handle_t h)
{
  ra_net_state_t* s = ra_net_internal_state();
  if (s->socks[h].kind != k_ra_net_sock_tcp) {
    return k_ra_err_invalid_arg;
  }
  ra_net_tcp_sock_t* t = &s->socks[h].tcp;
  switch (t->state) {
    case k_tcp_state_listen:
    case k_tcp_state_syn_sent:
    case k_tcp_state_closed:
      (void)memset(&s->socks[h], 0, sizeof(s->socks[h]));
      return k_ra_ok;
    case k_tcp_state_established:
    case k_tcp_state_syn_received: {
      ra_err_t e = ra_net_tcp_internal_emit_segment(
        t,
        (uint8_t)((uint8_t)k_tcp_flag_fin | (uint8_t)k_tcp_flag_ack),
        nullptr,
        0U);
      if (e == k_ra_ok) {
        t->snd_nxt++;
        t->state = k_tcp_state_fin_wait;
      }
      return e;
    }
    case k_tcp_state_close_wait: {
      ra_err_t e = ra_net_tcp_internal_emit_segment(
        t,
        (uint8_t)((uint8_t)k_tcp_flag_fin | (uint8_t)k_tcp_flag_ack),
        nullptr,
        0U);
      if (e == k_ra_ok) {
        t->snd_nxt++;
        t->state = k_tcp_state_last_ack;
      }
      return e;
    }
    default:
      return k_ra_ok;
  }
}

/* =============================================================================
 * RX path
 * =============================================================================
 */

/**
 * @brief Process an inbound TCP frame; drive the per-socket FSM.
 *
 * @details RFC 793 -- finds the matching socket by 4-tuple (or
 *          LISTEN on the destination port), then advances its FSM.
 *
 * @param[in] frame Ethernet+IPv4+TCP frame bytes. Must not be NULL.
 * @param[in] len   Frame length.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok               Frame processed.
 * @retval k_ra_err_null_ptr     frame == NULL.
 * @retval k_ra_err_invalid_arg  Frame too short / malformed.
 * @retval k_ra_err_not_found    No matching socket.
 * @retval other                 Whatever the per-state path returns.
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
ra_err_t ra_net_tcp_handle(const uint8_t* frame, uint16_t len)
{
  if (frame == nullptr) {
    return k_ra_err_null_ptr;
  }
  uint16_t min = (uint16_t)(k_eth_hdr_len + k_ipv4_hdr_len + k_tcp_hdr_len);
  if (len < min) {
    return k_ra_err_invalid_arg;
  }
  const uint8_t* ip   = &frame[k_eth_hdr_len];
  uint16_t       ihl  = (uint16_t)((uint16_t)(ip[0] & 0x0FU) * 4U);
  uint16_t       tlen = ra_net_be16(&ip[2]);
  if ((uint16_t)(k_eth_hdr_len + tlen) > len) {
    return k_ra_err_invalid_arg;
  }
  const uint8_t* tcp     = &frame[k_eth_hdr_len + ihl];
  uint16_t       sport   = ra_net_be16(&tcp[0]);
  uint16_t       dport   = ra_net_be16(&tcp[2]);
  uint32_t       seq     = ra_net_be32(&tcp[4]);
  uint32_t       ack     = ra_net_be32(&tcp[8]);
  uint8_t        doff    = (uint8_t)((tcp[k_tcp_offset_dataoff] >> 4U) * 4U);
  uint8_t        flags   = tcp[k_tcp_offset_flags];
  uint16_t       seg_len = (uint16_t)(tlen - ihl - doff);
  const uint8_t* payload = &tcp[doff];

  ra_net_ipv4_t src_ip = {};
  (void)memcpy(src_ip.bytes, &ip[12], 4U);

  ra_net_state_t* s = ra_net_internal_state();

  /* Match: established connection first, then listening socket. */
  int16_t idx = ra_net_tcp_internal_find_socket(dport, src_ip, sport, k_tcp_state_closed, 0U);
  if (idx < 0) {
    return k_ra_err_not_found;
  }
  ra_net_tcp_sock_t* t = &s->socks[idx].tcp;

  /* LISTEN: accept SYN, transition to SYN_RECEIVED. */
  if (t->state == k_tcp_state_listen) {
    if ((flags & (uint8_t)k_tcp_flag_syn) == 0U) {
      return k_ra_ok;
    }
    /* Bind the listening slot to the new peer. */
    t->remote_ip   = src_ip;
    t->remote_port = sport;
    t->rcv_nxt     = seq + 1U;
    t->snd_nxt     = (uint32_t)k_tcp_initial_iss + (uint32_t)idx;
    t->snd_una     = t->snd_nxt;
    t->state       = k_tcp_state_syn_received;
    ra_err_t e =
      ra_net_tcp_internal_emit_segment(t,
                                       (uint8_t)((uint8_t)k_tcp_flag_syn | (uint8_t)k_tcp_flag_ack),
                                       nullptr,
                                       0U);
    if (e == k_ra_ok) {
      t->snd_nxt++;
    }
    return e;
  }

  if (t->state == k_tcp_state_syn_sent) {
    if ((flags & ((uint8_t)k_tcp_flag_syn | (uint8_t)k_tcp_flag_ack)) ==
        ((uint8_t)k_tcp_flag_syn | (uint8_t)k_tcp_flag_ack)) {
      t->rcv_nxt = seq + 1U;
      t->snd_una = ack;
      t->state   = k_tcp_state_established;
      return ra_net_tcp_internal_emit_segment(t, (uint8_t)k_tcp_flag_ack, nullptr, 0U);
    }
    return k_ra_ok;
  }

  if (t->state == k_tcp_state_syn_received) {
    if ((flags & (uint8_t)k_tcp_flag_ack) != 0U) {
      t->snd_una = ack;
      t->state   = k_tcp_state_established;
    }
    return k_ra_ok;
  }

  if (t->state == k_tcp_state_established) {
    if (seg_len != 0U) {
      uint16_t copy_len = seg_len;
      if ((uint32_t)t->rx_len + (uint32_t)copy_len > (uint32_t)k_ra_net_tcp_rx_buf_size) {
        copy_len = (uint16_t)(k_ra_net_tcp_rx_buf_size - t->rx_len);
      }
      (void)memcpy(&t->rx_buf[t->rx_len], payload, copy_len);
      t->rx_len  = (uint16_t)(t->rx_len + copy_len);
      t->rcv_nxt = seq + (uint32_t)seg_len;
      (void)ra_net_tcp_internal_emit_segment(t, (uint8_t)k_tcp_flag_ack, nullptr, 0U);
    }
    if ((flags & (uint8_t)k_tcp_flag_fin) != 0U) {
      t->rcv_nxt++;
      t->state = k_tcp_state_close_wait;
      (void)ra_net_tcp_internal_emit_segment(t, (uint8_t)k_tcp_flag_ack, nullptr, 0U);
    }
    if ((flags & (uint8_t)k_tcp_flag_ack) != 0U) {
      t->snd_una = ack;
    }
    return k_ra_ok;
  }

  if (t->state == k_tcp_state_fin_wait) {
    if ((flags & (uint8_t)k_tcp_flag_fin) != 0U) {
      t->rcv_nxt++;
      (void)ra_net_tcp_internal_emit_segment(t, (uint8_t)k_tcp_flag_ack, nullptr, 0U);
      t->state = k_tcp_state_closed;
      (void)memset(&s->socks[idx], 0, sizeof(s->socks[idx]));
    }
    return k_ra_ok;
  }

  if (t->state == k_tcp_state_last_ack) {
    if ((flags & (uint8_t)k_tcp_flag_ack) != 0U) {
      t->state = k_tcp_state_closed;
      (void)memset(&s->socks[idx], 0, sizeof(s->socks[idx]));
    }
    return k_ra_ok;
  }

  return k_ra_ok;
}

/* NOLINTEND(readability-magic-numbers,readability-redundant-casting,clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling,readability-function-size,readability-identifier-naming,readability-function-cognitive-complexity) */
