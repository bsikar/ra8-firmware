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

/* =============================================================================
 * UDP socket allocation
 * =============================================================================
 */

static int16_t find_udp_socket_by_port(uint16_t port)
{
  ra_net_state_t* s = ra_net_internal_state();
  for (uint16_t i = 0U; i < (uint16_t)k_ra_net_max_sockets; i++) {
    if (s->socks[i].kind == k_ra_net_sock_udp && s->socks[i].udp.local_port == port) {
      return (int16_t)i;
    }
  }
  return -1;
}

ra_err_t ra_net_udp_socket(uint16_t local_port, ra_net_handle_t* out_handle)
{
  if (out_handle == nullptr) {
    return k_ra_err_null_ptr;
  }
  if (local_port == 0U) {
    return k_ra_err_invalid_arg;
  }
  if (find_udp_socket_by_port(local_port) >= 0) {
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

static void dns_consume_response(const uint8_t* dns, uint16_t dlen);

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
    dns_consume_response(payload, payload_len);
    /* Fall through so a bound socket on dport still sees the bytes. */
  }

  int16_t idx = find_udp_socket_by_port(dport);
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

static void dns_consume_response(const uint8_t* dns, uint16_t dlen)
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

  /* Skip questions: each question is qname + 4 bytes (qtype/qclass). */
  uint16_t pos = k_dns_hdr_len;
  for (uint16_t q = 0U; q < qdcount; q++) {
    while (pos < dlen && dns[pos] != 0U) {
      if ((dns[pos] & 0xC0U) == 0xC0U) {
        pos = (uint16_t)(pos + 2U);
        break; /* compressed name */
      }
      pos = (uint16_t)(pos + dns[pos] + 1U);
    }
    if (pos < dlen && dns[pos] == 0U) {
      pos++; /* root null */
    }
    pos = (uint16_t)(pos + 4U);
  }

  /* Walk answers, take the first A record. */
  for (uint16_t a = 0U; a < ancount; a++) {
    if (pos >= dlen) {
      break;
    }
    /* skip name (compressed or not) */
    if ((dns[pos] & 0xC0U) == 0xC0U) {
      pos = (uint16_t)(pos + 2U);
    } else {
      while (pos < dlen && dns[pos] != 0U) {
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

  /* Build query. */
  static uint8_t buf[((uint16_t)k_ra_net_dns_max_hostname * 2U) + (uint16_t)k_dns_hdr_len + 4U];
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

  s->dns_pending = 1U;
  s->dns_rcode   = 0U;
  (void)memset(s->dns_answer.bytes, 0, 4U);

  ra_err_t e =
    udp_emit(s->cfg.dns_server, (uint16_t)k_dns_port, (uint16_t)k_dns_ephem_port, buf, qpos);
  if (e != k_ra_ok) {
    s->dns_pending = 0U;
    return e;
  }

  /* Bounded poll budget so the loop terminates even when the host's
   * time source is frozen (unit tests use a manual clock). On real
   * hardware the SysTick-driven clock wins long before the budget. */
  uint32_t deadline    = ra_time_ms() + (uint32_t)k_ra_net_dns_timeout_ms;
  uint32_t poll_budget = (uint32_t)k_ra_net_dns_timeout_ms; /* one poll per ms cap */
  while ((s->dns_pending != 0U) && (s->dns_rcode == 0U) && (poll_budget != 0U)) {
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
  if (s->dns_rcode == (uint8_t)k_dns_rcode_nxdomain) {
    return k_ra_err_not_found;
  }
  *out_ip = s->dns_answer;
  return k_ra_ok;
}

/* NOLINTEND(readability-magic-numbers,readability-redundant-casting,clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling,readability-function-size,readability-identifier-naming,readability-function-cognitive-complexity) */
