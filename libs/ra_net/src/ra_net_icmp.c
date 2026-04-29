/**
 * @file ra_net_icmp.c
 * @brief Minimal ICMP echo (ping) responder per RFC 792.
 *
 * @details
 * Only the echo-request -> echo-reply flow is implemented; all other
 * ICMP types (destination unreachable, redirect, etc) are silently
 * dropped. Replies preserve the request's identifier and sequence
 * fields so a remote ``ping`` round-trip works.
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

/* RFC 792 ICMP */

/* NOLINTBEGIN(readability-magic-numbers,readability-redundant-casting,clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling) */

ra_err_t ra_net_icmp_handle(const uint8_t* frame, uint16_t len)
{
  if (frame == nullptr) {
    return k_ra_err_null_ptr;
  }
  uint16_t min = (uint16_t)(k_eth_hdr_len + k_ipv4_hdr_len + k_icmp_hdr_len);
  if (len < min) {
    return k_ra_err_invalid_arg;
  }
  const uint8_t* ip  = &frame[k_eth_hdr_len];
  uint16_t       ihl = (uint16_t)((uint16_t)(ip[0] & 0x0FU) * 4U);
  if (ihl < k_ipv4_hdr_len) {
    return k_ra_err_invalid_arg;
  }
  uint16_t tlen = ra_net_be16(&ip[2]);
  if ((uint16_t)(k_eth_hdr_len + tlen) > len) {
    return k_ra_err_invalid_arg;
  }

  const uint8_t* icmp = &frame[k_eth_hdr_len + ihl];
  if (icmp[0] != (uint8_t)k_icmp_type_echo_request) {
    return k_ra_ok;
  }

  uint16_t icmp_len = (uint16_t)(tlen - ihl);
  if (icmp_len > (uint16_t)(k_ra_net_pal_frame_max - k_eth_hdr_len - k_ipv4_hdr_len)) {
    return k_ra_err_invalid_size;
  }

  /* Build reply payload (ICMP). */
  static uint8_t s_payload[k_ra_net_pal_frame_max];
  (void)memcpy(s_payload, icmp, icmp_len);
  s_payload[0] = (uint8_t)k_icmp_type_echo_reply;
  s_payload[1] = 0U;
  s_payload[2] = 0U;
  s_payload[3] = 0U;
  uint16_t ck  = ra_net_checksum_ones(s_payload, icmp_len, 0U);
  ra_net_put_be16(&s_payload[2], ck);

  ra_net_ipv4_t dst;
  (void)memcpy(dst.bytes, &ip[12], 4U);
  return ra_net_ipv4_send(dst, k_ip_proto_icmp, s_payload, icmp_len);
}

/* NOLINTEND(readability-magic-numbers,readability-redundant-casting,clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling) */
