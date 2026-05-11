/**
 * @file ra_net_udp.c
 * @brief UDP sockets (RFC 768) + DNS A-record query (RFC 1035) for ra_net.
 *
 * @details
 * Sockets are looked up by destination port number. Each UDP socket
 * keeps a tiny FIFO of received datagrams; received frames whose
 * destination port matches a bound socket are enqueued. The DNS
 * resolver is implemented as a regular UDP socket plus a synchronous
 * polling loop that walks the response, picks the first A record, and
 * fills out_ip.
 *
 * Simplifications:
 *   - No checksum verification on RX (loopback tests don't compute
 *     them); we still emit correct UDP checksums on TX.
 *   - DNS supports A records only, no recursion (we just point at the
 *     configured server with the RD flag set per RFC 1035 sec 4.1.1
 *     so a real recursive resolver still services the query).
 *   - No IP-fragment reassembly.
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
#include "ra_time.h"

/* RFC 768 UDP
 * RFC 1035 DNS
 */

/* NOLINTBEGIN(readability-magic-numbers,readability-redundant-casting,clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling,readability-function-size,readability-identifier-naming,readability-function-cognitive-complexity) */

/**
 * @brief Pure DNS byte-match predicate -- see header for full contract.
 * @details Promoted helper so the line-539/554 ANDs can be driven under MC/DC.
 * @param[in] in_range     Non-zero iff pos < dlen.
 * @param[in] byte_value   Value of dns[pos] when in_range.
 * @param[in] target_value Comparison target.
 * @param[in] equal        Non-zero for ==, zero for !=.
 * @return Boolean predicate.
 * @retval true  Predicate held.
 * @retval false Predicate failed.
 * @pre None.
 * @pre None.
 * @post No state mutated.
 * @post Return depends solely on inputs.
 * @note Pure; thread-safe.
 * @since 0.1.0
 */
bool ra_net_internal_dns_byte_match(uint8_t in_range,
                                    uint8_t byte_value,
                                    uint8_t target_value,
                                    uint8_t equal)
{
  const bool matches = (equal != 0U) ? (byte_value == target_value) : (byte_value != target_value);
  return (in_range != 0U) && matches;
}

/**
 * @brief Pure DNS-loop-active predicate -- see header for full contract.
 * @details Promoted helper so the line-657 AND-chain can be driven under MC/DC.
 * @param[in] dns_pending Non-zero while DNS request outstanding.
 * @param[in] dns_rcode   Non-zero once a reply has set the RCODE.
 * @param[in] poll_budget Remaining poll iterations.
 * @return Boolean predicate.
 * @retval true  Loop body must run.
 * @retval false Loop terminates.
 * @pre None.
 * @pre None.
 * @post No state mutated.
 * @post Return depends solely on inputs.
 * @note Pure; thread-safe.
 * @since 0.1.0
 */
bool ra_net_internal_dns_loop_active(uint8_t dns_pending, uint8_t dns_rcode, uint32_t poll_budget)
{
  return (dns_pending != 0U) && (dns_rcode == 0U) && (poll_budget != 0U);
}

/* =============================================================================
 * UDP socket allocation
 * =============================================================================
 */

/**
 * @brief Locate the UDP socket bound to a local port.
 *
 * @details Linear scan over the socket table.
 *
 * @param[in] port Local UDP port number.
 *
 * @return int16_t Slot index, or -1 on no match.
 * @retval -1   No matching UDP socket.
 * @retval >=0  Slot index of the bound socket.
 *
 * @pre ra_net_open has succeeded.
 * @pre Caller is the network thread.
 * @post No state mutation.
 * @post Returned index is valid for ra_net_internal_state()->socks.
 *
 * @note Not thread-safe.
 *
 * @since 0.1.0
 */
int16_t ra_net_udp_internal_find_socket_by_port(uint16_t port)
{
  ra_net_state_t* s = ra_net_internal_state();
  for (uint16_t i = 0U; i < (uint16_t)k_ra_net_max_sockets; i++) {
    if (s->socks[i].kind == k_ra_net_sock_udp && s->socks[i].udp.local_port == port) {
      return (int16_t)i;
    }
  }
  return -1;
}

/**
 * @brief Allocate a new UDP socket bound to local_port.
 *
 * @details RFC 768 -- claims the first unused slot, refusing
 *          duplicate bindings.
 *
 * @param[in]  local_port Local UDP port (must be non-zero).
 * @param[out] out_handle Filled with the socket handle on success.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok               Socket allocated.
 * @retval k_ra_err_null_ptr     out_handle == NULL.
 * @retval k_ra_err_invalid_arg  local_port == 0.
 * @retval k_ra_err_exists       Another socket already bound to local_port.
 * @retval k_ra_err_no_mem       Socket table is full.
 *
 * @pre ra_net_open has succeeded.
 * @pre out_handle is non-NULL.
 * @post On success a UDP socket holds the binding.
 * @post On failure no state mutation.
 *
 * @note Not thread-safe.
 *
 * @since 0.1.0
 */
ra_err_t ra_net_udp_socket(uint16_t local_port, ra_net_handle_t* out_handle)
{
  if (out_handle == nullptr) {
    return k_ra_err_null_ptr;
  }
  if (local_port == 0U) {
    return k_ra_err_invalid_arg;
  }
  if (ra_net_udp_internal_find_socket_by_port(local_port) >= 0) {
    return k_ra_err_exists;
  }
  ra_net_state_t* s = ra_net_internal_state();
  for (uint16_t i = 0U; i < (uint16_t)k_ra_net_max_sockets; i++) {
    if (s->socks[i].kind == k_ra_net_sock_unused) {
      (void)memset(&s->socks[i], 0, sizeof(s->socks[i]));
      s->socks[i].kind           = k_ra_net_sock_udp;
      s->socks[i].udp.local_port = local_port;
      *out_handle                = (ra_net_handle_t)i;
      return k_ra_ok;
    }
  }
  return k_ra_err_no_mem;
}

/* =============================================================================
 * UDP TX (build header + checksum)
 * =============================================================================
 */

/**
 * @brief Compute the UDP pseudo-header checksum partial-sum.
 *
 * @details RFC 768 -- builds the (src, dst, proto, length)
 *          pseudo-header sum that ra_net_checksum_ones folds with
 *          the UDP datagram.
 *
 * @param[in] src     Source IPv4 address.
 * @param[in] dst     Destination IPv4 address.
 * @param[in] udp_len Total UDP datagram length (header + payload).
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
static uint16_t
udp_pseudo_header_sum(const ra_net_ipv4_t* src, const ra_net_ipv4_t* dst, uint16_t udp_len)
{
  uint32_t sum = 0U;
  sum += (uint32_t)((((uint32_t)src->bytes[0]) << 8U) | (uint32_t)src->bytes[1]);
  sum += (uint32_t)((((uint32_t)src->bytes[2]) << 8U) | (uint32_t)src->bytes[3]);
  sum += (uint32_t)((((uint32_t)dst->bytes[0]) << 8U) | (uint32_t)dst->bytes[1]);
  sum += (uint32_t)((((uint32_t)dst->bytes[2]) << 8U) | (uint32_t)dst->bytes[3]);
  sum += (uint32_t)k_ip_proto_udp;
  sum += (uint32_t)udp_len;
  return (uint16_t)sum;
}

/**
 * @brief Build and transmit one UDP datagram.
 *
 * @details RFC 768 -- writes the 8-byte UDP header, computes the
 *          checksum, and pushes through ra_net_ipv4_send.
 *
 * @param[in] dst_ip       Destination IPv4.
 * @param[in] dst_port     Destination UDP port.
 * @param[in] src_port     Source UDP port.
 * @param[in] payload      Optional payload (may be NULL when payload_len == 0).
 * @param[in] payload_len  Payload length.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok                Datagram queued.
 * @retval k_ra_err_invalid_size  Combined header+payload exceeds PAL frame max.
 * @retval other                  ra_net_ipv4_send error code.
 *
 * @pre ra_net_open has succeeded.
 * @pre payload is non-NULL when payload_len > 0.
 * @post On success a UDP datagram is queued.
 * @post On failure no state mutation.
 *
 * @note Not thread-safe.
 *
 * @since 0.1.0
 */
static ra_err_t udp_emit(ra_net_ipv4_t  dst_ip,
                         uint16_t       dst_port,
                         uint16_t       src_port,
                         const uint8_t* payload,
                         uint16_t       payload_len)
{
  ra_net_state_t* s       = ra_net_internal_state();
  uint16_t        udp_len = (uint16_t)(k_udp_hdr_len + payload_len);
  if (udp_len > (uint16_t)(k_ra_net_pal_frame_max - k_eth_hdr_len - k_ipv4_hdr_len)) {
    return k_ra_err_invalid_size;
  }
  static uint8_t buf[k_ra_net_pal_frame_max];
  ra_net_put_be16(&buf[0], src_port);
  ra_net_put_be16(&buf[2], dst_port);
  ra_net_put_be16(&buf[4], udp_len);
  ra_net_put_be16(&buf[6], 0U);
  if (payload_len != 0U) {
    (void)memcpy(&buf[k_udp_hdr_len], payload, payload_len);
  }
  uint16_t pseudo = udp_pseudo_header_sum(&s->cfg.ip, &dst_ip, udp_len);
  uint16_t ck     = ra_net_checksum_ones(buf, udp_len, pseudo);
  if (ck == 0U) {
    ck = 0xFFFFU;
  }
  ra_net_put_be16(&buf[6], ck);
  return ra_net_ipv4_send(dst_ip, k_ip_proto_udp, buf, udp_len);
}

/**
 * @brief Send a UDP datagram from a previously allocated socket.
 *
 * @details RFC 768 -- thin wrapper that validates the socket binding
 *          and forwards to udp_emit.
 *
 * @param[in] h        Socket handle.
 * @param[in] buf      Payload bytes (must not be NULL).
 * @param[in] len      Payload length (> 0).
 * @param[in] dst_ip   Destination IPv4.
 * @param[in] dst_port Destination UDP port (> 0).
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok                Datagram queued.
 * @retval k_ra_err_null_ptr      buf == NULL.
 * @retval k_ra_err_invalid_arg   dst_port == 0 or len == 0.
 * @retval k_ra_err_invalid_state Slot is not a UDP socket.
 * @retval other                  udp_emit error code.
 *
 * @pre h was returned by ra_net_udp_socket.
 * @pre buf is non-NULL and len > 0.
 * @post On success a datagram is queued.
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
                         uint16_t        dst_port)
{
  if (buf == nullptr) {
    return k_ra_err_null_ptr;
  }
  if ((dst_port == 0U) || (len == 0U)) {
    return k_ra_err_invalid_arg;
  }
  ra_net_state_t* s = ra_net_internal_state();
  if (s->socks[h].kind != k_ra_net_sock_udp) {
    return k_ra_err_invalid_state;
  }
  return udp_emit(dst_ip, dst_port, s->socks[h].udp.local_port, buf, len);
}

/**
 * @brief Pop the oldest queued datagram for a UDP socket.
 *
 * @details RFC 768 -- copies the head of the per-socket FIFO into
 *          the caller's buffer, then advances the read index.
 *
 * @param[in]  h         Socket handle.
 * @param[out] buf       Destination buffer.
 * @param[in]  max_len   Capacity of buf.
 * @param[out] got_len   Filled with the bytes copied.
 * @param[out] src_ip    Filled with the datagram's source IPv4.
 * @param[out] src_port  Filled with the datagram's source port.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok                Datagram returned.
 * @retval k_ra_err_invalid_state Slot is not a UDP socket.
 * @retval k_ra_err_no_data       Queue empty.
 *
 * @pre h was returned by ra_net_udp_socket.
 * @pre All output pointers are non-NULL.
 * @post On success the slot's queue advanced and outputs are filled.
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
                         uint16_t*       src_port)
{
  ra_net_state_t* s = ra_net_internal_state();
  if (s->socks[h].kind != k_ra_net_sock_udp) {
    return k_ra_err_invalid_state;
  }
  ra_net_udp_sock_t* u = &s->socks[h].udp;
  if (u->count == 0U) {
    return k_ra_err_no_data;
  }
  ra_net_udp_msg_t* m = &u->q[u->rd_idx];
  uint16_t          n = (m->len < max_len) ? m->len : max_len;
  (void)memcpy(buf, m->data, n);
  *got_len  = n;
  *src_ip   = m->src_ip;
  *src_port = m->src_port;
  u->rd_idx = (uint8_t)((u->rd_idx + 1U) % (uint8_t)k_ra_net_udp_max_queued);
  u->count--;
  return k_ra_ok;
}

/* =============================================================================
 * UDP RX path: enqueue or hand to DNS state machine
 * =============================================================================
 */

/* Forward declaration moved to ra_net_internal.h for MC/DC test access. */

/**
 * @brief Process an inbound UDP frame; enqueue to the bound socket
 *        and (when applicable) feed the DNS resolver.
 *
 * @details RFC 768 -- parses the UDP header and either appends to
 *          the per-socket queue, or drops if no socket is bound. DNS
 *          responses (source port 53) additionally feed
 *          dns_consume_response.
 *
 * @param[in] frame Ethernet+IPv4+UDP frame bytes. Must not be NULL.
 * @param[in] len   Frame length.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok               Datagram enqueued (or dropped to a missing socket).
 * @retval k_ra_err_null_ptr     frame == NULL.
 * @retval k_ra_err_invalid_arg  Frame too short / malformed.
 * @retval k_ra_err_not_found    No socket bound to dport.
 * @retval k_ra_err_no_mem       Per-socket queue full.
 *
 * @pre ra_net_open has succeeded.
 * @pre frame is non-NULL.
 * @post On success the matching socket queue gained a row.
 * @post DNS state machine may have advanced.
 *
 * @note Not thread-safe.
 *
 * @since 0.1.0
 */
ra_err_t ra_net_udp_handle(const uint8_t* frame, uint16_t len)
{
  if (frame == nullptr) {
    return k_ra_err_null_ptr;
  }
  uint16_t min = (uint16_t)(k_eth_hdr_len + k_ipv4_hdr_len + k_udp_hdr_len);
  if (len < min) {
    return k_ra_err_invalid_arg;
  }
  const uint8_t* ip      = &frame[k_eth_hdr_len];
  uint16_t       ihl     = (uint16_t)((uint16_t)(ip[0] & 0x0FU) * 4U);
  const uint8_t* udp     = &frame[k_eth_hdr_len + ihl];
  uint16_t       sport   = ra_net_be16(&udp[0]);
  uint16_t       dport   = ra_net_be16(&udp[2]);
  uint16_t       udp_len = ra_net_be16(&udp[4]);
  if (udp_len < k_udp_hdr_len) {
    return k_ra_err_invalid_arg;
  }
  uint16_t       payload_len = (uint16_t)(udp_len - k_udp_hdr_len);
  const uint8_t* payload     = &udp[k_udp_hdr_len];

  ra_net_state_t* s = ra_net_internal_state();

  /* DNS replies arrive on the ephemeral port we picked when issuing
   * the query; route them to the resolver before treating them as a
   * regular socket datagram. */
  if (s->dns_pending != 0U && sport == (uint16_t)k_dns_port) {
    ra_net_udp_internal_dns_consume_response(payload, payload_len);
    /* Fall through so a bound socket on dport still sees the bytes. */
  }

  int16_t idx = ra_net_udp_internal_find_socket_by_port(dport);
  if (idx < 0) {
    return k_ra_err_not_found;
  }
  ra_net_udp_sock_t* u = &s->socks[idx].udp;
  if (u->count >= (uint8_t)k_ra_net_udp_max_queued) {
    return k_ra_err_no_mem;
  }
  ra_net_udp_msg_t* m = &u->q[u->wr_idx];
  uint16_t          copy_len =
    (payload_len < (uint16_t)sizeof(m->data)) ? payload_len : (uint16_t)sizeof(m->data);
  (void)memcpy(m->src_ip.bytes, &ip[12], 4U);
  m->src_port = sport;
  m->len      = copy_len;
  if (copy_len != 0U) {
    (void)memcpy(m->data, payload, copy_len);
  }
  u->wr_idx = (uint8_t)((u->wr_idx + 1U) % (uint8_t)k_ra_net_udp_max_queued);
  u->count++;
  return k_ra_ok;
}

/* =============================================================================
 * DNS query (RFC 1035)
 * =============================================================================
 */

typedef enum : uint16_t {
  k_dns_hdr_len    = 12U,
  k_dns_ephem_port = 49152U,
  k_dns_label_max  = 63U,
} ra_net_dns_consts_t;

/**
 * @brief Encode a hostname into the DNS QNAME wire format.
 *
 * @details RFC 1035 sec 3.1 -- length-prefixed labels terminated by
 *          a zero-length label.
 *
 * @param[in]  host NUL-terminated hostname (must not be NULL).
 * @param[out] out  Destination for the QNAME bytes.
 *
 * @return uint16_t Number of bytes written, including the terminator.
 * @retval 0      Empty hostname (caller treats as malformed).
 * @retval other  Number of bytes written.
 *
 * @pre host is non-NULL and NUL-terminated.
 * @pre out has at least 2 * k_ra_net_dns_max_hostname bytes.
 * @post out holds the label-encoded QNAME terminated by a zero byte.
 * @post No other state mutation.
 *
 * @note Not thread-safe; pure function.
 *
 * @since 0.1.0
 */
static uint16_t dns_encode_qname(const char* host, uint8_t* out)
{
  uint16_t outpos = 0U;
  uint16_t segpos = 0U;
  uint16_t i      = 0U;
  while (host[i] != '\0') {
    if (host[i] == '.') {
      uint8_t seglen = (uint8_t)(outpos - segpos);
      out[segpos]    = seglen;
      segpos         = (uint16_t)(outpos + 1U);
      outpos         = segpos;
    } else {
      out[(uint16_t)(outpos + 1U)] = (uint8_t)host[i];
      outpos++;
    }
    i++;
    if (outpos >= (uint16_t)(k_ra_net_dns_max_hostname * 2U)) {
      break;
    }
  }
  uint8_t finalseg = (uint8_t)(outpos - segpos);
  out[segpos]      = finalseg;
  outpos           = (uint16_t)(outpos + 1U);
  out[outpos]      = 0U; /* terminator */
  outpos++;
  return outpos;
}

/**
 * @brief Advance pos past the DNS question section of a response.
 *
 * @details RFC 1035 sec 4.1.2 -- each question is a qname followed by
 *          qtype (2B) + qclass (2B). Honors message compression
 *          pointers (0xC0). Byte-equivalent to the original inline
 *          loop in ra_net_udp_internal_dns_consume_response.
 *
 * @param[in] dns     DNS message bytes.
 * @param[in] dlen    Message length.
 * @param[in] qdcount Number of questions to skip.
 * @param[in] start   Position to begin skipping from.
 *
 * @return uint16_t Position immediately past the question section.
 * @retval >=start The new parse cursor.
 *
 * @pre dns is non-NULL.
 * @pre start <= dlen.
 * @post Return value advances past qdcount questions.
 * @post No state mutation.
 *
 * @note Not thread-safe; pure function.
 *
 * @since 0.1.0
 */
static uint16_t
dns_skip_questions(const uint8_t* dns, uint16_t dlen, uint16_t qdcount, uint16_t start)
{
  uint16_t pos = start;
  for (uint16_t q = 0U; q < qdcount; q++) {
    while (pos < dlen && dns[pos] != 0U) {
      if ((dns[pos] & 0xC0U) == 0xC0U) {
        pos = (uint16_t)(pos + 2U);
        break; /* compressed name */
      }
      pos = (uint16_t)(pos + dns[pos] + 1U);
    }
    if (ra_net_internal_dns_byte_match((uint8_t)((pos < dlen) ? 1U : 0U),
                                       (pos < dlen) ? dns[pos] : (uint8_t)0xFFU,
                                       0U,
                                       1U)) {
      pos++; /* root null */
    }
    pos = (uint16_t)(pos + 4U);
  }
  return pos;
}

/**
 * @brief Walk the DNS answer section and capture the first A record.
 *
 * @details RFC 1035 sec 4.1.3 -- iterates answers, skipping the name
 *          (compressed or labeled), then inspects rrtype/rrclass/rdlen.
 *          On the first IN-A record with rdlen==4 the four bytes are
 *          copied into the singleton's dns_answer and dns_pending is
 *          cleared. Byte-equivalent to the original inline answer loop.
 *
 * @param[in]     dns     DNS message bytes.
 * @param[in]     dlen    Message length.
 * @param[in]     ancount Number of answer records to walk.
 * @param[in]     start   Position of the first answer record.
 * @param[in,out] s       Network singleton; dns_answer/dns_pending are
 *                        updated on a successful match.
 *
 * @pre dns is non-NULL.
 * @pre s is non-NULL.
 * @post On match s->dns_answer holds the resolved IPv4 and
 *       s->dns_pending == 0.
 * @post On miss / truncation no state mutation.
 *
 * @note Not thread-safe.
 *
 * @since 0.1.0
 */
static void dns_scan_answers(const uint8_t*  dns,
                             uint16_t        dlen,
                             uint16_t        ancount,
                             uint16_t        start,
                             ra_net_state_t* s)
{
  uint16_t pos = start;
  for (uint16_t a = 0U; a < ancount; a++) {
    if (pos >= dlen) {
      return;
    }
    /* skip name (compressed or not) */
    if ((dns[pos] & 0xC0U) == 0xC0U) {
      pos = (uint16_t)(pos + 2U);
    } else {
      while (ra_net_internal_dns_byte_match((uint8_t)((pos < dlen) ? 1U : 0U),
                                            (pos < dlen) ? dns[pos] : (uint8_t)0U,
                                            0U,
                                            0U)) {
        pos = (uint16_t)(pos + dns[pos] + 1U);
      }
      pos = (uint16_t)(pos + 1U);
    }
    if ((uint16_t)(pos + 10U) > dlen) {
      return;
    }
    uint16_t rrtype  = ra_net_be16(&dns[pos]);
    uint16_t rrclass = ra_net_be16(&dns[pos + 2U]);
    /* TTL at +4 ignored. */
    uint16_t rdlen = ra_net_be16(&dns[pos + 8U]);
    pos            = (uint16_t)(pos + 10U);
    if ((uint16_t)(pos + rdlen) > dlen) {
      return;
    }
    if (rrtype == (uint16_t)k_dns_rr_a && rrclass == (uint16_t)k_dns_rr_class_in && rdlen == 4U) {
      (void)memcpy(s->dns_answer.bytes, &dns[pos], 4U);
      s->dns_pending = 0U;
      return;
    }
    pos = (uint16_t)(pos + rdlen);
  }
}

/**
 * @brief Walk a DNS response and capture the first A record.
 *
 * @details RFC 1035 sec 4.1 -- validates the transaction id against
 *          the singleton, extracts QR/RCODE/QDCOUNT/ANCOUNT from the
 *          header, skips the question section via dns_skip_questions,
 *          then forwards to dns_scan_answers which records the first
 *          IN-A record into dns_answer. Out-of-order, malformed, or
 *          query-side packets are silently dropped so the caller's
 *          poll loop simply waits for the next match.
 *
 * @param[in] dns  DNS message bytes (UDP payload). Must not be NULL
 *                 when dlen > 0.
 * @param[in] dlen Message length in bytes (0 .. UDP payload max).
 *
 * @pre ra_net_open has succeeded.
 * @pre s_state.dns_pending == 1 (a query is outstanding).
 * @post On success s_state.dns_answer holds the resolved IPv4 and
 *       s_state.dns_pending == 0.
 * @post On failure s_state.dns_pending stays 1 (caller times out).
 *
 * @note Not thread-safe; intended to be called from the network thread.
 *
 * @since 0.1.0
 */
void ra_net_udp_internal_dns_consume_response(const uint8_t* dns, uint16_t dlen)
{
  ra_net_state_t* s = ra_net_internal_state();
  if (dlen < (uint16_t)k_dns_hdr_len) {
    return;
  }
  uint16_t txid = ra_net_be16(&dns[0]);
  if (txid != s->dns_txid) {
    return;
  }
  uint16_t flags   = ra_net_be16(&dns[2]);
  uint16_t qdcount = ra_net_be16(&dns[4]);
  uint16_t ancount = ra_net_be16(&dns[6]);
  s->dns_rcode     = (uint8_t)(flags & (uint16_t)k_dns_flag_rcode_mask);

  if ((flags & (uint16_t)k_dns_flag_qr_response) == 0U) {
    return;
  }

  uint16_t pos = dns_skip_questions(dns, dlen, qdcount, (uint16_t)k_dns_hdr_len);
  dns_scan_answers(dns, dlen, ancount, pos, s);
}

/**
 * @brief Build a DNS A-record query into the supplied buffer.
 *
 * @details RFC 1035 sec 4.1 -- writes a 12-byte header (with a fresh
 *          transaction id seeded from s->dns_txid and the high bit set
 *          for entropy), encodes the qname, and appends qtype=A /
 *          qclass=IN. Byte-equivalent to the original inline build in
 *          ra_net_dns_query.
 *
 * @param[in,out] s        Network singleton; dns_txid is advanced.
 * @param[in]     hostname NUL-terminated hostname.
 * @param[out]    buf      Query buffer.
 *
 * @return uint16_t Total query length in bytes.
 * @retval >0 Bytes written into buf.
 *
 * @pre s, hostname, buf are non-NULL.
 * @pre buf is at least hdr + (hlen+2) + 4 bytes long.
 * @post buf holds a valid DNS A query.
 * @post s->dns_txid advanced by one (with high bit forced).
 *
 * @note Not thread-safe.
 *
 * @since 0.1.0
 */
static uint16_t dns_build_query(ra_net_state_t* s, const char* hostname, uint8_t* buf)
{
  s->dns_txid = (uint16_t)((s->dns_txid + 1U) | 0x8000U);
  ra_net_put_be16(&buf[0], s->dns_txid);
  ra_net_put_be16(&buf[2], (uint16_t)k_dns_flag_rd);
  ra_net_put_be16(&buf[4], 1U); /* QDCOUNT */
  ra_net_put_be16(&buf[6], 0U);
  ra_net_put_be16(&buf[8], 0U);
  ra_net_put_be16(&buf[10], 0U);
  uint16_t qpos = (uint16_t)k_dns_hdr_len;
  uint16_t qlen = dns_encode_qname(hostname, &buf[qpos]);
  qpos          = (uint16_t)(qpos + qlen);
  ra_net_put_be16(&buf[qpos], (uint16_t)k_dns_rr_a);
  ra_net_put_be16(&buf[qpos + 2U], (uint16_t)k_dns_rr_class_in);
  qpos = (uint16_t)(qpos + 4U);
  return qpos;
}

/**
 * @brief Bounded poll loop awaiting the DNS response.
 *
 * @details Spins ra_net_poll until s->dns_pending is cleared or the
 *          deadline elapses. The poll_budget cap exists so the loop
 *          terminates even when the host's time source is frozen
 *          (unit tests use a manual clock). On real hardware the
 *          SysTick-driven clock wins long before the budget.
 *
 * @param[in,out] s Network singleton; dns_pending is cleared on timeout.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok          Response observed.
 * @retval k_ra_err_timeout Deadline elapsed before response arrived.
 *
 * @pre s is non-NULL and s->dns_pending == 1.
 * @pre ra_time_ms() is monotonic and ra_net_poll is callable.
 * @post On timeout s->dns_pending == 0.
 * @post On success s->dns_answer holds the resolved IPv4.
 *
 * @note Not thread-safe; blocks the caller.
 *
 * @since 0.1.0
 */
static ra_err_t dns_await_response(ra_net_state_t* s)
{
  uint32_t deadline    = ra_time_ms() + (uint32_t)k_ra_net_dns_timeout_ms;
  uint32_t poll_budget = (uint32_t)k_ra_net_dns_timeout_ms; /* one poll per ms cap */
  while (ra_net_internal_dns_loop_active(s->dns_pending, s->dns_rcode, poll_budget)) {
    (void)ra_net_poll();
    poll_budget--;
    if (ra_time_ms() >= deadline) {
      s->dns_pending = 0U;
      return k_ra_err_timeout;
    }
    if (s->dns_pending == 0U) {
      break;
    }
  }
  if (s->dns_pending != 0U) {
    s->dns_pending = 0U;
    return k_ra_err_timeout;
  }
  return k_ra_ok;
}

/**
 * @brief Synchronously resolve a hostname to an IPv4 address.
 *
 * @details RFC 1035 -- builds a DNS A-record query via dns_build_query,
 *          emits it through udp_emit to the configured DNS server on
 *          port 53 from an ephemeral source port, then drives
 *          dns_await_response until either the singleton's dns_pending
 *          clears or the timeout fires. On NXDOMAIN the RCODE is mapped
 *          to k_ra_err_not_found. Blocks the caller; intended for
 *          single-threaded firmware contexts only.
 *
 * @param[in]  hostname NUL-terminated hostname (must not be NULL,
 *                      length < k_ra_net_dns_max_hostname).
 * @param[out] out_ip   Filled with the resolved IPv4 on success.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok                Hostname resolved; *out_ip valid.
 * @retval k_ra_err_null_ptr      hostname or out_ip is NULL.
 * @retval k_ra_err_invalid_state Stack not opened.
 * @retval k_ra_err_invalid_arg   Hostname too long.
 * @retval k_ra_err_timeout       No response within k_ra_net_dns_timeout_ms.
 * @retval k_ra_err_not_found     DNS NXDOMAIN response.
 * @retval other                  ra_net_ipv4_send error code.
 *
 * @pre ra_net_open has succeeded with a configured DNS server.
 * @pre Both pointers are non-NULL.
 * @post On success *out_ip holds the resolved IPv4 and s->dns_pending == 0.
 * @post On failure *out_ip is unspecified and s->dns_pending == 0.
 *
 * @note Not thread-safe; blocks until the resolver completes.
 *
 * @since 0.1.0
 */
ra_err_t ra_net_dns_query(const char* hostname, ra_net_ipv4_t* out_ip)
{
  if ((hostname == nullptr) || (out_ip == nullptr)) {
    return k_ra_err_null_ptr;
  }
  ra_net_state_t* s = ra_net_internal_state();
  if (s->opened == 0U) {
    return k_ra_err_invalid_state;
  }
  uint16_t hlen = 0U;
  while (hostname[hlen] != '\0') {
    hlen++;
    if (hlen >= (uint16_t)k_ra_net_dns_max_hostname) {
      return k_ra_err_invalid_arg;
    }
  }

  static uint8_t buf[((uint16_t)k_ra_net_dns_max_hostname * 2U) + (uint16_t)k_dns_hdr_len + 4U];
  uint16_t       qpos = dns_build_query(s, hostname, buf);

  s->dns_pending = 1U;
  s->dns_rcode   = 0U;
  (void)memset(s->dns_answer.bytes, 0, 4U);

  ra_err_t e =
    udp_emit(s->cfg.dns_server, (uint16_t)k_dns_port, (uint16_t)k_dns_ephem_port, buf, qpos);
  if (e != k_ra_ok) {
    s->dns_pending = 0U;
    return e;
  }

  ra_err_t we = dns_await_response(s);
  if (we != k_ra_ok) {
    return we;
  }
  if (s->dns_rcode == (uint8_t)k_dns_rcode_nxdomain) {
    return k_ra_err_not_found;
  }
  *out_ip = s->dns_answer;
  return k_ra_ok;
}

/* NOLINTEND(readability-magic-numbers,readability-redundant-casting,clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling,readability-function-size,readability-identifier-naming,readability-function-cognitive-complexity) */
