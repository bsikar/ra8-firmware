/*
 * Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
/**
 * @file mdl_url_guard.c
 * @brief Implementation of the pure URL / address safety predicates.
 */
#include "mdl_url_guard.h"

#include <arpa/inet.h>
#include <string.h>

#include "ra8_attributes.h"

/** @brief IPv4 address byte layout used while classifying. */
typedef enum : uint8_t {
  k_v4_o0 = 0, /**< First octet index.  */
  k_v4_o1 = 1, /**< Second octet index. */
} mdl_v4_index_t;

/** @brief IPv4 range boundaries that mark non-public address space. */
typedef enum : uint16_t {
  k_v4_zero_net      = 0,   /**< 0.0.0.0/8 "this network" (unspecified).  */
  k_v4_loopback_net  = 127, /**< 127.0.0.0/8 loopback.                    */
  k_v4_private_a     = 10,  /**< 10.0.0.0/8.                              */
  k_v4_private_b     = 172, /**< 172.16.0.0/12 first octet.               */
  k_v4_private_b_lo  = 16,  /**< 172.16 low second octet (inclusive).     */
  k_v4_private_b_hi  = 31,  /**< 172.31 high second octet (inclusive).    */
  k_v4_private_c     = 192, /**< 192.168.0.0/16 first octet.              */
  k_v4_private_c_2   = 168, /**< 192.168 second octet.                    */
  k_v4_linklocal     = 169, /**< 169.254.0.0/16 first octet.              */
  k_v4_linklocal_2   = 254, /**< 169.254 second octet.                    */
  k_v4_cgnat         = 100, /**< 100.64.0.0/10 (RFC6598) first octet.     */
  k_v4_cgnat_lo      = 64,  /**< 100.64 low second octet (inclusive).     */
  k_v4_cgnat_hi      = 127, /**< 100.127 high second octet (inclusive).   */
  k_v4_multicast_min = 224, /**< 224.0.0.0/4 and up: multicast/reserved.  */
} mdl_v4_range_t;

/** @brief IPv6 address byte layout and the bytes classification inspects. */
typedef enum : uint8_t {
  k_v6_bytes       = 16, /**< Total bytes in an IPv6 address.          */
  k_v6_mapped_ff_a = 10, /**< First 0xFF byte of an IPv4-mapped prefix. */
  k_v6_mapped_ff_b = 11, /**< Second 0xFF byte of the mapped prefix.    */
  k_v6_mapped_v4   = 12, /**< Offset of the embedded IPv4 address.      */
  k_v6_last        = 15, /**< Index of the final byte (for ::1).        */
} mdl_v6_index_t;

/** @brief IPv6 prefix byte values that mark non-public address space. */
typedef enum : uint16_t {
  k_v6_byte_ff       = 0xFF, /**< Multicast lead byte / mapped filler.  */
  k_v6_ula_mask      = 0xFE, /**< Mask isolating the fc00::/7 prefix.   */
  k_v6_ula_value     = 0xFC, /**< fc00::/7 unique-local value.          */
  k_v6_ll_lead       = 0xFE, /**< fe80::/10 lead byte.                  */
  k_v6_ll_mask       = 0xC0, /**< Mask isolating the /10 boundary.      */
  k_v6_ll_value      = 0x80, /**< fe80::/10 masked value.               */
  k_v6_loopback_last = 0x01, /**< Final byte of ::1.                    */
} mdl_v6_prefix_t;

/** @brief Scheme prefixes accepted by ::mdl_url_scheme_allowed. */
static const char* const s_scheme_http  = "http://";
static const char* const s_scheme_https = "https://";

/** @brief Case-insensitive test that `s` begins with `prefix`. */
RA8_INTERNAL static bool starts_with_ci(const char* s, const char* prefix)
{
  size_t i = 0U;
  while (prefix[i] != '\0') {
    const char a = s[i];
    if (a == '\0') {
      return false;
    }
    const char la = (char)(((a >= 'A') && (a <= 'Z')) ? (a + ('a' - 'A')) : a);
    if (la != prefix[i]) {
      return false;
    }
    ++i;
  }
  return true;
}

bool mdl_url_scheme_allowed(const char* url)
{
  if ((url == nullptr) || (url[0] == '\0')) {
    return false;
  }
  return starts_with_ci(url, s_scheme_http) || starts_with_ci(url, s_scheme_https);
}

/** @brief Classify the four octets of an IPv4 address. */
RA8_INTERNAL static mdl_addr_class_t classify_v4(const unsigned char* o)
{
  const unsigned o0 = o[k_v4_o0];
  const unsigned o1 = o[k_v4_o1];
  if (o0 == (unsigned)k_v4_zero_net) {
    return k_mdl_addr_unknown;
  }
  if (o0 == (unsigned)k_v4_loopback_net) {
    return k_mdl_addr_loopback;
  }
  if (o0 >= (unsigned)k_v4_multicast_min) {
    return k_mdl_addr_unknown;
  }
  if (o0 == (unsigned)k_v4_linklocal) {
    return (o1 == (unsigned)k_v4_linklocal_2) ? k_mdl_addr_linklocal : k_mdl_addr_public;
  }
  const bool priv_a = (o0 == (unsigned)k_v4_private_a);
  const bool priv_b = (o0 == (unsigned)k_v4_private_b) && (o1 >= (unsigned)k_v4_private_b_lo) &&
                      (o1 <= (unsigned)k_v4_private_b_hi);
  const bool priv_c = (o0 == (unsigned)k_v4_private_c) && (o1 == (unsigned)k_v4_private_c_2);
  const bool cgnat  = (o0 == (unsigned)k_v4_cgnat) && (o1 >= (unsigned)k_v4_cgnat_lo) &&
                      (o1 <= (unsigned)k_v4_cgnat_hi);
  return (priv_a || priv_b || priv_c || cgnat) ? k_mdl_addr_private : k_mdl_addr_public;
}

/** @brief True if the 16 IPv6 bytes carry an IPv4-mapped `::ffff:a.b.c.d`. */
RA8_INTERNAL static bool is_v4_mapped(const unsigned char* b)
{
  for (size_t i = 0U; i < (size_t)k_v6_mapped_ff_a; ++i) {
    if (b[i] != 0U) {
      return false;
    }
  }
  return (b[k_v6_mapped_ff_a] == (unsigned char)k_v6_byte_ff) &&
         (b[k_v6_mapped_ff_b] == (unsigned char)k_v6_byte_ff);
}

/** @brief True if the 16 IPv6 bytes are the loopback address `::1`. */
RA8_INTERNAL static bool is_v6_loopback(const unsigned char* b)
{
  for (size_t i = 0U; i < (size_t)k_v6_last; ++i) {
    if (b[i] != 0U) {
      return false;
    }
  }
  return b[k_v6_last] == (unsigned char)k_v6_loopback_last;
}

/** @brief Classify the sixteen bytes of an IPv6 address. */
RA8_INTERNAL static mdl_addr_class_t classify_v6(const unsigned char* b)
{
  if (is_v4_mapped(b)) {
    return classify_v4(b + (size_t)k_v6_mapped_v4);
  }
  if (is_v6_loopback(b)) {
    return k_mdl_addr_loopback;
  }
  if (b[k_v4_o0] == (unsigned char)k_v6_byte_ff) {
    return k_mdl_addr_unknown; /* multicast */
  }
  if ((b[k_v4_o0] == (unsigned char)k_v6_ll_lead) &&
      ((b[k_v4_o1] & (unsigned char)k_v6_ll_mask) == (unsigned char)k_v6_ll_value)) {
    return k_mdl_addr_linklocal;
  }
  if ((b[k_v4_o0] & (unsigned char)k_v6_ula_mask) == (unsigned char)k_v6_ula_value) {
    return k_mdl_addr_private;
  }
  return k_mdl_addr_public;
}

mdl_addr_class_t mdl_classify_ip(const char* ip)
{
  if (ip == nullptr) {
    return k_mdl_addr_unknown;
  }
  unsigned char v4[sizeof(struct in_addr)] = {};
  if (inet_pton(AF_INET, ip, v4) == 1) {
    return classify_v4(v4);
  }
  unsigned char v6[sizeof(struct in6_addr)] = {};
  if (inet_pton(AF_INET6, ip, v6) == 1) {
    return classify_v6(v6);
  }
  return k_mdl_addr_unknown;
}

bool mdl_addr_is_fetchable(mdl_addr_class_t cls, bool allow_private)
{
  if (cls == k_mdl_addr_public) {
    return true;
  }
  if (cls == k_mdl_addr_unknown) {
    return false;
  }
  return allow_private;
}

bool mdl_size_exceeds(uint64_t have, uint64_t add, uint64_t cap)
{
  if (cap == 0U) {
    return false;
  }
  if (have > cap) {
    return true;
  }
  return add > (cap - have);
}

bool mdl_url_host(const char* url, char* out, size_t cap)
{
  if ((out == nullptr) || (cap == 0U)) {
    return false;
  }
  out[0] = '\0';
  if (url == nullptr) {
    return false;
  }
  const char* sep = strstr(url, "://");
  if (sep == nullptr) {
    return false;
  }
  const char* host = sep + (sizeof("://") - 1U);
  const char* at   = strchr(host, '@');
  const char* path = strpbrk(host, "/?#");
  if (at != nullptr) {
    const bool at_in_authority = (path == nullptr) || (at < path);
    if (at_in_authority) {
      host = at + 1U;
    }
  }
  size_t n = 0U;
  /* Keep the port: robots.txt is scoped per (scheme, host, port) authority, and
   * a redirect to a different port is a different origin. Only userinfo is
   * dropped (handled above). */
  while ((host[n] != '\0') && (host[n] != '/') && (host[n] != '?') && (host[n] != '#')) {
    if ((n + 1U) >= cap) {
      out[0] = '\0';
      return false;
    }
    const char c = host[n];
    out[n]       = (char)(((c >= 'A') && (c <= 'Z')) ? (c + ('a' - 'A')) : c);
    ++n;
  }
  out[n] = '\0';
  return n > 0U;
}

bool mdl_url_path(const char* url, char* out, size_t cap)
{
  if ((out == nullptr) || (cap == 0U)) {
    return false;
  }
  out[0] = '\0';
  if (url == nullptr) {
    return false;
  }
  const char* sep = strstr(url, "://");
  if (sep == nullptr) {
    return false;
  }
  const char* host = sep + (sizeof("://") - 1U);
  const char* path = strchr(host, '/');
  if (path == nullptr) {
    if (cap < 2U) {
      return false;
    }
    out[0] = '/';
    out[1] = '\0';
    return true;
  }
  size_t n = 0U;
  while ((path[n] != '\0') && (path[n] != '?') && (path[n] != '#')) {
    if ((n + 1U) >= cap) {
      out[0] = '\0';
      return false;
    }
    out[n] = path[n];
    ++n;
  }
  out[n] = '\0';
  return true;
}
