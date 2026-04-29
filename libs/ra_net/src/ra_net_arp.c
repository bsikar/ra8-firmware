/**
 * @file ra_net_arp.c
 * @brief ARP cache + RFC 826 request/reply handling for ra_net.
 *
 * @details
 * Implements a minimal subset of RFC 826:
 *
 *   - Maintain a 16-entry IP -> MAC cache, 5-minute TTL.
 *   - On RX request for our IP, emit a unicast reply.
 *   - On RX reply, populate the cache.
 *   - ``ra_net_arp_resolve`` returns the cached MAC if present and
 *     emits a broadcast request otherwise. The caller deals with the
 *     "unresolved this round" case (this stack falls back to a
 *     broadcast frame in ipv4_send so loopback tests still see traffic).
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

/* RFC 826 ARP */

/* NOLINTBEGIN(readability-magic-numbers,readability-redundant-casting,clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling,readability-function-size)
 * The wire-format constants here are RFC-defined byte offsets into the
 * ARP packet header (see RFC 826 sec 4); naming each via a typed enum
 * already; the lints fire on the individual struct-field offsets that
 * are equally well documented by their RFC names. memcpy/memset are
 * the standard tools for byte-level frame construction. Function size
 * fires on ra_net_arp_handle which is a flat dispatch over RFC 826
 * opcodes; splitting it would obscure the spec mapping. */

/** @brief Hardware/protocol field constants per RFC 826. */
typedef enum : uint16_t {
  k_arp_htype_eth      = 1U,
  k_arp_ptype_ipv4     = 0x0800U,
  k_arp_pkt_offset_op  = 6U,
  k_arp_pkt_offset_sha = 8U,
  k_arp_pkt_offset_spa = 14U,
  k_arp_pkt_offset_tha = 18U,
  k_arp_pkt_offset_tpa = 24U,
} ra_net_arp_offsets_t;

typedef enum : uint8_t {
  k_arp_hlen_eth  = 6U,
  k_arp_plen_ipv4 = 4U,
} ra_net_arp_lens_t;

void ra_net_arp_reset(void)
{
  ra_net_state_t* s = ra_net_internal_state();
  (void)memset(s->arp, 0, sizeof(s->arp));
}

static uint8_t arp_lookup(const ra_net_ipv4_t* ip, ra_net_mac_t* out)
{
  ra_net_state_t* s   = ra_net_internal_state();
  uint32_t        now = ra_time_ms();
  for (uint16_t i = 0U; i < (uint16_t)k_ra_net_arp_cache_size; i++) {
    if (s->arp[i].timestamp_ms == 0U) {
      continue;
    }
    if ((now - s->arp[i].timestamp_ms) > (uint32_t)k_ra_net_arp_ttl_ms) {
      s->arp[i].timestamp_ms = 0U;
      continue;
    }
    if (memcmp(s->arp[i].ip.bytes, ip->bytes, 4U) == 0) {
      *out = s->arp[i].mac;
      return 1U;
    }
  }
  return 0U;
}

static void arp_insert(const ra_net_ipv4_t* ip, const ra_net_mac_t* mac)
{
  ra_net_state_t* s   = ra_net_internal_state();
  uint32_t        now = ra_time_ms();
  if (now == 0U) {
    now = 1U; /* keep zero as the empty-slot sentinel */
  }
  /* Update existing entry if present. */
  for (uint16_t i = 0U; i < (uint16_t)k_ra_net_arp_cache_size; i++) {
    if (s->arp[i].timestamp_ms != 0U && memcmp(s->arp[i].ip.bytes, ip->bytes, 4U) == 0) {
      s->arp[i].mac          = *mac;
      s->arp[i].timestamp_ms = now;
      return;
    }
  }
  /* Else first free slot. */
  for (uint16_t i = 0U; i < (uint16_t)k_ra_net_arp_cache_size; i++) {
    if (s->arp[i].timestamp_ms == 0U) {
      s->arp[i].ip           = *ip;
      s->arp[i].mac          = *mac;
      s->arp[i].timestamp_ms = now;
      return;
    }
  }
  /* Else evict slot 0 (FIFO-ish; the oldest is good enough at 16 slots). */
  s->arp[0].ip           = *ip;
  s->arp[0].mac          = *mac;
  s->arp[0].timestamp_ms = now;
}

static ra_err_t emit_request(ra_net_ipv4_t target)
{
  ra_net_state_t* s                                = ra_net_internal_state();
  uint8_t         f[k_eth_hdr_len + k_arp_pkt_len] = {};

  for (uint8_t i = 0U; i < 6U; i++) {
    f[i] = 0xFFU; /* broadcast */
  }
  (void)memcpy(&f[6], s->cfg.mac.bytes, 6U);
  ra_net_put_be16(&f[12], (uint16_t)k_eth_type_arp);

  uint8_t* a = &f[k_eth_hdr_len];
  ra_net_put_be16(&a[0], (uint16_t)k_arp_htype_eth);
  ra_net_put_be16(&a[2], (uint16_t)k_arp_ptype_ipv4);
  a[4] = (uint8_t)k_arp_hlen_eth;
  a[5] = (uint8_t)k_arp_plen_ipv4;
  ra_net_put_be16(&a[k_arp_pkt_offset_op], (uint16_t)k_arp_op_request);
  (void)memcpy(&a[k_arp_pkt_offset_sha], s->cfg.mac.bytes, 6U);
  (void)memcpy(&a[k_arp_pkt_offset_spa], s->cfg.ip.bytes, 4U);
  /* tha left zero */
  (void)memcpy(&a[k_arp_pkt_offset_tpa], target.bytes, 4U);

  return ra_net_pal_send_frame(f, (uint16_t)sizeof(f));
}

ra_err_t ra_net_arp_resolve(ra_net_ipv4_t ip, ra_net_mac_t* out_mac)
{
  if (out_mac == nullptr) {
    return k_ra_err_null_ptr;
  }
  if (arp_lookup(&ip, out_mac) != 0U) {
    return k_ra_ok;
  }
  (void)emit_request(ip);
  return k_ra_err_no_data;
}

ra_err_t ra_net_arp_handle(const uint8_t* frame, uint16_t len)
{
  if (frame == nullptr) {
    return k_ra_err_null_ptr;
  }
  if (len < (uint16_t)(k_eth_hdr_len + k_arp_pkt_len)) {
    return k_ra_err_invalid_arg;
  }
  const uint8_t* a = &frame[k_eth_hdr_len];
  if (ra_net_be16(&a[0]) != (uint16_t)k_arp_htype_eth) {
    return k_ra_err_not_supported;
  }
  if (ra_net_be16(&a[2]) != (uint16_t)k_arp_ptype_ipv4) {
    return k_ra_err_not_supported;
  }
  uint16_t op = ra_net_be16(&a[k_arp_pkt_offset_op]);

  ra_net_ipv4_t spa;
  ra_net_mac_t  sha;
  (void)memcpy(sha.bytes, &a[k_arp_pkt_offset_sha], 6U);
  (void)memcpy(spa.bytes, &a[k_arp_pkt_offset_spa], 4U);

  /* Cache the sender either way. */
  arp_insert(&spa, &sha);

  if (op == (uint16_t)k_arp_op_request) {
    ra_net_state_t* s = ra_net_internal_state();
    if (memcmp(&a[k_arp_pkt_offset_tpa], s->cfg.ip.bytes, 4U) != 0) {
      return k_ra_ok; /* not us */
    }
    /* Build reply. */
    uint8_t r[k_eth_hdr_len + k_arp_pkt_len] = {};
    (void)memcpy(&r[0], sha.bytes, 6U);
    (void)memcpy(&r[6], s->cfg.mac.bytes, 6U);
    ra_net_put_be16(&r[12], (uint16_t)k_eth_type_arp);
    uint8_t* ar = &r[k_eth_hdr_len];
    ra_net_put_be16(&ar[0], (uint16_t)k_arp_htype_eth);
    ra_net_put_be16(&ar[2], (uint16_t)k_arp_ptype_ipv4);
    ar[4] = (uint8_t)k_arp_hlen_eth;
    ar[5] = (uint8_t)k_arp_plen_ipv4;
    ra_net_put_be16(&ar[k_arp_pkt_offset_op], (uint16_t)k_arp_op_reply);
    (void)memcpy(&ar[k_arp_pkt_offset_sha], s->cfg.mac.bytes, 6U);
    (void)memcpy(&ar[k_arp_pkt_offset_spa], s->cfg.ip.bytes, 4U);
    (void)memcpy(&ar[k_arp_pkt_offset_tha], sha.bytes, 6U);
    (void)memcpy(&ar[k_arp_pkt_offset_tpa], spa.bytes, 4U);
    return ra_net_pal_send_frame(r, (uint16_t)sizeof(r));
  }
  /* Reply: cache already updated. */
  return k_ra_ok;
}

/* NOLINTEND(readability-magic-numbers,readability-redundant-casting,clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling,readability-function-size) */
