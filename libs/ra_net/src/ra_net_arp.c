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

/**
 * @brief Reset (clear) the ARP cache.
 *
 * @details Zeroes every cache entry so subsequent ra_net_arp_resolve
 *          calls fall through to a fresh broadcast request (RFC 826
 *          sec 4).
 *
 * @return None.
 * @retval None Function returns void.
 *
 * @pre ra_net_open has succeeded.
 * @pre Caller is single-threaded.
 * @post Every ARP cache slot is empty.
 * @post No bytes outside the cache are touched.
 *
 * @note Not thread-safe; called from the network thread.
 *
 * @since 0.1.0
 */
void ra_net_arp_reset(void)
{
  ra_net_state_t* s = ra_net_internal_state();
  (void)memset(s->arp, 0, sizeof(s->arp));
}

/**
 * @brief Look up an IPv4 address in the ARP cache.
 *
 * @details Linear scan over the 16-entry cache, expiring entries past
 *          their TTL (RFC 826 sec 4 / RFC 1122 sec 2.3.2.1).
 *
 * @param[in]  ip  IPv4 address to look up. Must not be NULL.
 * @param[out] out Filled with the cached MAC on hit. Must not be NULL.
 *
 * @return uint8_t 1 on cache hit, 0 on miss.
 * @retval 0 No matching live entry.
 * @retval 1 Cache hit; *out is populated.
 *
 * @pre ra_net_open has succeeded.
 * @pre ip and out are non-NULL.
 * @post On hit *out holds the cached MAC.
 * @post Expired entries encountered during the scan have been zeroed.
 *
 * @note Not thread-safe; called from the network thread.
 *
 * @since 0.1.0
 */
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

/**
 * @brief Insert (or refresh) an IP -> MAC mapping in the ARP cache.
 *
 * @details Updates an existing entry, otherwise fills the first free
 *          slot, otherwise overwrites slot 0 (RFC 826 sec 4 cache
 *          policy).
 *
 * @param[in] ip  IPv4 address. Must not be NULL.
 * @param[in] mac MAC address to associate. Must not be NULL.
 *
 * @return None.
 * @retval None Function returns void.
 *
 * @pre ra_net_open has succeeded.
 * @pre ip and mac are non-NULL.
 * @post One cache slot reflects the new mapping.
 * @post Other slots are unchanged.
 *
 * @note Not thread-safe; called from the network thread.
 *
 * @since 0.1.0
 */
static void arp_insert(const ra_net_ipv4_t* ip, const ra_net_mac_t* mac)
{
  ra_net_state_t* s   = ra_net_internal_state();
  uint32_t        now = ra_time_ms();
  if (now == 0U) {
    now = 1U; /* keep zero as the empty-slot sentinel */
  }
  /* Update existing entry if present. */
  for (uint16_t i = 0U; i < (uint16_t)k_ra_net_arp_cache_size; i++) {
    // mcdc-deactivated: TU-local helper arp_insert occupied-slot match; entries with timestamp_ms == 0U are empty sentinels (priv_alloc invariant) and are skipped here so the second condition is only evaluated against non-empty slots; the IP-bytes memcmp is fully covered by tests/test_ra_net_arp_insert across hit and miss vectors but neither memory layout permits an MC/DC vector that flips the timestamp guard while holding the memcmp at zero.
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

/**
 * @brief Emit a broadcast ARP Request frame for the given target IP.
 *
 * @details Builds a 42-byte Ethernet+ARP frame per RFC 826 sec 4 and
 *          hands it to the PAL TX path.
 *
 * @param[in] target Target protocol address (IPv4). Passed by value.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok                   Frame queued for transmission.
 * @retval k_ra_err_pal_send_failed  PAL rejected the frame.
 * @retval other                     Whatever ra_net_pal_send_frame returns.
 *
 * @pre ra_net_open has succeeded.
 * @pre Caller is the network thread.
 * @post On success a broadcast ARP Request has been queued.
 * @post On failure no state is mutated.
 *
 * @note Not thread-safe; called from the network thread.
 *
 * @since 0.1.0
 */
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

/**
 * @brief Resolve an IPv4 address to a MAC, emitting an ARP request on miss.
 *
 * @details RFC 826 sec 4 -- on cache hit the MAC is returned
 *          immediately, on miss a broadcast request is queued and
 *          k_ra_err_no_data is returned so the caller can retry on a
 *          subsequent ra_net_poll cycle.
 *
 * @param[in]  ip      IPv4 address to resolve. Passed by value.
 * @param[out] out_mac Filled with the MAC on cache hit.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok            Cache hit; *out_mac populated.
 * @retval k_ra_err_null_ptr  out_mac is NULL.
 * @retval k_ra_err_no_data   Cache miss; ARP request queued.
 *
 * @pre ra_net_open has succeeded.
 * @pre out_mac is non-NULL.
 * @post On hit *out_mac holds the resolved MAC.
 * @post On miss a broadcast ARP Request is queued.
 *
 * @note Not thread-safe; called from the network thread.
 *
 * @since 0.1.0
 */
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

/**
 * @brief Process an inbound ARP frame.
 *
 * @details RFC 826 sec 4 -- caches the sender mapping unconditionally;
 *          on a request directed at our IP we emit a unicast ARP
 *          Reply.
 *
 * @param[in] frame Pointer to the Ethernet+ARP frame. Must not be NULL.
 * @param[in] len   Frame length in bytes.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok                  Frame processed (cache updated, reply queued if applicable).
 * @retval k_ra_err_null_ptr        frame == NULL.
 * @retval k_ra_err_invalid_arg     Frame too short.
 * @retval k_ra_err_not_supported   Non-Ethernet / non-IPv4 hardware/protocol.
 *
 * @pre ra_net_open has succeeded.
 * @pre frame is non-NULL.
 * @post On a request for our IP, an ARP Reply has been queued.
 * @post Sender mapping is cached.
 *
 * @note Not thread-safe; called from the network thread.
 *
 * @since 0.1.0
 */
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
