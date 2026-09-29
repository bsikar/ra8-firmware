/**
 * @file ra8_net_urlguard.c
 * @brief Implementation of the URL and peer-address safety policy.
 * @ingroup grp_net
 *
 * @par Tag
 * [Ring 2 / Interface] {World: Any}
 *
 * @details Pure lexical and numeric policy over caller-owned storage: no
 *          allocation, no network call, no name resolution, no global state.
 *          The address literal is parsed here rather than by the host's
 *          `inet_pton`, so the policy is callable from firmware.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_net_urlguard.h"

/**
 * @enum urlguard_v4_index_t
 * @brief IPv4 address byte layout used while classifying.
 */
typedef enum : uint8_t {
  k_urlguard_v4_o0    = 0U, /**< First octet index.         */
  k_urlguard_v4_o1    = 1U, /**< Second octet index.        */
  k_urlguard_v4_bytes = 4U, /**< Octets in an IPv4 address. */
} urlguard_v4_index_t;

/**
 * @enum urlguard_v4_range_t
 * @brief IPv4 range boundaries that mark non-public address space.
 */
typedef enum : uint16_t {
  k_urlguard_v4_zero_net      = 0U,   /**< 0.0.0.0/8 "this network".              */
  k_urlguard_v4_loopback_net  = 127U, /**< 127.0.0.0/8 loopback.                  */
  k_urlguard_v4_private_a     = 10U,  /**< 10.0.0.0/8.                            */
  k_urlguard_v4_private_b     = 172U, /**< 172.16.0.0/12 first octet.             */
  k_urlguard_v4_private_b_lo  = 16U,  /**< 172.16 low second octet (inclusive).   */
  k_urlguard_v4_private_b_hi  = 31U,  /**< 172.31 high second octet (inclusive).  */
  k_urlguard_v4_private_c     = 192U, /**< 192.168.0.0/16 first octet.            */
  k_urlguard_v4_private_c_2   = 168U, /**< 192.168 second octet.                  */
  k_urlguard_v4_linklocal     = 169U, /**< 169.254.0.0/16 first octet.            */
  k_urlguard_v4_linklocal_2   = 254U, /**< 169.254 second octet.                  */
  k_urlguard_v4_cgnat         = 100U, /**< 100.64.0.0/10 (RFC6598) first octet.   */
  k_urlguard_v4_cgnat_lo      = 64U,  /**< 100.64 low second octet (inclusive).   */
  k_urlguard_v4_cgnat_hi      = 127U, /**< 100.127 high second octet (inclusive). */
  k_urlguard_v4_multicast_min = 224U, /**< 224.0.0.0/4 and up: multicast.         */
  k_urlguard_v4_octet_max     = 255U, /**< Largest value one octet can hold.      */
} urlguard_v4_range_t;

/**
 * @enum urlguard_v6_index_t
 * @brief IPv6 address byte layout and the bytes classification inspects.
 */
typedef enum : uint8_t {
  k_urlguard_v6_bytes       = 16U, /**< Total bytes in an IPv6 address.           */
  k_urlguard_v6_groups      = 8U,  /**< 16-bit groups in an IPv6 address.         */
  k_urlguard_v6_mapped_ff_a = 10U, /**< First 0xFF byte of an IPv4-mapped prefix. */
  k_urlguard_v6_mapped_ff_b = 11U, /**< Second 0xFF byte of the mapped prefix.    */
  k_urlguard_v6_mapped_v4   = 12U, /**< Offset of the embedded IPv4 address.      */
  k_urlguard_v6_last        = 15U, /**< Index of the final byte (for ::1).        */
  k_urlguard_v6_group_hex   = 4U,  /**< Hex digits one group may carry.           */
} urlguard_v6_index_t;

/**
 * @enum urlguard_v6_prefix_t
 * @brief IPv6 prefix byte values that mark non-public address space.
 */
typedef enum : uint16_t {
  k_urlguard_v6_byte_ff       = 0xFFU, /**< Multicast lead byte / mapped filler. */
  k_urlguard_v6_ula_mask      = 0xFEU, /**< Mask isolating the fc00::/7 prefix.  */
  k_urlguard_v6_ula_value     = 0xFCU, /**< fc00::/7 unique-local value.         */
  k_urlguard_v6_ll_lead       = 0xFEU, /**< fe80::/10 lead byte.                 */
  k_urlguard_v6_ll_mask       = 0xC0U, /**< Mask isolating the /10 boundary.     */
  k_urlguard_v6_ll_value      = 0x80U, /**< fe80::/10 masked value.              */
  k_urlguard_v6_loopback_last = 0x01U, /**< Final byte of ::1.                   */
} urlguard_v6_prefix_t;

/**
 * @enum urlguard_radix_t
 * @brief Numeric bases and shifts used by the address literal parser.
 */
typedef enum : uint16_t {
  /** Decimal, for an IPv4 octet. */
  k_urlguard_radix_dec = 10U,
  /** Hexadecimal, for an IPv6 group. */
  k_urlguard_radix_hex = 16U,
  /** Largest value one group holds. */
  k_urlguard_group_max = 0xFFFFU,
  /** Bits between the two group bytes. */
  k_urlguard_byte_shift = 8U,
  /** Mask isolating one byte. */
  k_urlguard_byte_mask = 0xFFU,
  /** Decimal digits one octet may take. */
  k_urlguard_digits_max = 3U,
} urlguard_radix_t;

/** @brief Scheme prefixes accepted by ::ra8_net_urlguard_scheme_allowed. */
static const char* const s_urlguard_scheme_http = "http://";

/** @brief The TLS flavour of the same allowlist. */
static const char* const s_urlguard_scheme_https = "https://";

/** @brief Authority separator that anchors every parse in this file. */
static const char* const s_urlguard_authority_sep = "://";

/**
 * @brief Case-insensitive test that `s` begins with `prefix`.
 * @details Folds ASCII upper-case only, so the comparison carries no locale.
 * @param[in] s      Readable NUL-terminated text.
 * @param[in] prefix Prefix to compare, NUL-terminated.
 * @return True when @p s begins with @p prefix ignoring ASCII case.
 * @retval true  The prefix matches.
 * @retval false The prefix does not match, or @p s ended first.
 * @pre Both pointers are non-null and remain valid for the call.
 * @post Neither argument is modified.
 * @note Thread-safe: depends only on its arguments.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_starts_with_ci(const char* s, const char* prefix)
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

/**
 * @brief Fold one ASCII byte to lower case.
 * @details Used for the authority copy, which is compared case-insensitively.
 * @param[in] c Byte to fold.
 * @return The lower-case form of @p c, or @p c when it is not A-Z.
 * @pre None; every byte value is accepted.
 * @post No state is modified.
 * @note Thread-safe: depends only on its argument.
 * @since 0.1.0
 */
RA8_INTERNAL static char internal_lower_ascii(char c)
{
  return (char)(((c >= 'A') && (c <= 'Z')) ? (c + ('a' - 'A')) : c);
}

/**
 * @brief Value of one hexadecimal digit, or -1 when @p c is not one.
 * @details Accepts both letter cases; the parser rejects anything else.
 * @param[in] c Candidate digit.
 * @return The digit value in 0..15, or -1.
 * @pre None; every byte value is accepted.
 * @post No state is modified.
 * @note Thread-safe: depends only on its argument.
 * @since 0.1.0
 */
RA8_INTERNAL static int internal_hex_value(char c)
{
  if ((c >= '0') && (c <= '9')) {
    return (int)(c - '0');
  }
  if ((c >= 'a') && (c <= 'f')) {
    return (int)(c - 'a') + (int)k_urlguard_radix_dec;
  }
  if ((c >= 'A') && (c <= 'F')) {
    return (int)(c - 'A') + (int)k_urlguard_radix_dec;
  }
  return -1;
}

/**
 * @brief Parse a dotted-quad IPv4 literal into four octets.
 * @details Rejects a leading zero, an over-long run of digits, an out-of-range
 *          octet, a missing or extra dot, and any trailing byte, so only a
 *          canonical dotted quad is accepted.
 * @param[in]  text Candidate literal, NUL-terminated.
 * @param[out] out  Four octets on success, untouched otherwise.
 * @return Whether @p text is a canonical dotted-quad IPv4 literal.
 * @retval true  @p out holds the four parsed octets.
 * @retval false @p text is not a dotted quad; @p out is unspecified.
 * @pre Both pointers are non-null and remain valid for the call.
 * @pre @p out has room for ::k_urlguard_v4_bytes octets.
 * @post @p text is not modified.
 * @note Thread-safe: writes only caller-provided storage.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_parse_v4(const char* text, unsigned char* out)
{
  size_t at = 0U;
  for (size_t octet = 0U; octet < (size_t)k_urlguard_v4_bytes; ++octet) {
    if (octet != 0U) {
      if (text[at] != '.') {
        return false;
      }
      ++at;
    }
    if ((text[at] < '0') || (text[at] > '9')) {
      return false;
    }
    if ((text[at] == '0') && (text[at + 1U] >= '0') && (text[at + 1U] <= '9')) {
      return false; /* a leading zero is not a canonical octet */
    }
    unsigned value  = 0U;
    size_t   digits = 0U;
    while ((text[at] >= '0') && (text[at] <= '9')) {
      if (digits == (size_t)k_urlguard_digits_max) {
        return false;
      }
      value = (value * (unsigned)k_urlguard_radix_dec) + (unsigned)(text[at] - '0');
      ++digits;
      ++at;
    }
    if (value > (unsigned)k_urlguard_v4_octet_max) {
      return false;
    }
    out[octet] = (unsigned char)value;
  }
  return text[at] == '\0';
}

/**
 * @brief Read one IPv6 group of up to four hex digits.
 * @details Advances @p at past the group it consumed.
 * @param[in]     text  Candidate literal, NUL-terminated.
 * @param[in,out] at    Cursor into @p text, advanced past the group.
 * @param[out]    group Parsed 16-bit group value.
 * @return Whether a group was read.
 * @retval true  @p group holds the parsed value and @p at advanced.
 * @retval false No hex digit was present, or more than four ran together.
 * @pre Every pointer is non-null and remains valid for the call.
 * @post On false, @p at and @p group are unspecified.
 * @note Thread-safe: writes only caller-provided storage.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_read_v6_group(const char* text, size_t* at, uint16_t* group)
{
  uint32_t value  = 0U;
  size_t   digits = 0U;
  while (internal_hex_value(text[*at]) >= 0) {
    if (digits == (size_t)k_urlguard_v6_group_hex) {
      return false;
    }
    value = (value * (uint32_t)k_urlguard_radix_hex) + (uint32_t)internal_hex_value(text[*at]);
    ++digits;
    ++(*at);
  }
  if (digits == 0U) {
    return false;
  }
  *group = (uint16_t)(value & (uint32_t)k_urlguard_group_max);
  return true;
}

/**
 * @brief Write the parsed head and tail groups into the 16 address bytes.
 * @details The `::` run is the zero fill between the two, so the tail lands
 *          flush against the end of the address.
 * @param[in]  head       Groups seen before any `::`.
 * @param[in]  head_count Number of groups in @p head.
 * @param[in]  tail       Groups seen after a `::`.
 * @param[in]  tail_count Number of groups in @p tail.
 * @param[out] out        Sixteen address bytes.
 * @pre Every pointer is non-null and remains valid for the call.
 * @pre `head_count + tail_count` does not exceed ::k_urlguard_v6_groups.
 * @post @p out is fully written.
 * @note Thread-safe: writes only caller-provided storage.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_pack_v6(const uint16_t* head,
                                          size_t          head_count,
                                          const uint16_t* tail,
                                          size_t          tail_count,
                                          unsigned char*  out)
{
  memset(out, 0, (size_t)k_urlguard_v6_bytes);
  for (size_t i = 0U; i < head_count; ++i) {
    out[i * 2U]        = (unsigned char)((head[i] >> (unsigned)k_urlguard_byte_shift) &
                                  (unsigned)k_urlguard_byte_mask);
    out[(i * 2U) + 1U] = (unsigned char)(head[i] & (unsigned)k_urlguard_byte_mask);
  }
  const size_t base = (size_t)k_urlguard_v6_groups - tail_count;
  for (size_t i = 0U; i < tail_count; ++i) {
    const size_t g     = base + i;
    out[g * 2U]        = (unsigned char)((tail[i] >> (unsigned)k_urlguard_byte_shift) &
                                  (unsigned)k_urlguard_byte_mask);
    out[(g * 2U) + 1U] = (unsigned char)(tail[i] & (unsigned)k_urlguard_byte_mask);
  }
}

/**
 * @struct urlguard_v6_parse_t
 * @brief Running state of one IPv6 literal parse.
 * @details The `::` run splits the groups into a head and a tail; the tail is
 *          packed flush against the end of the address, so the zero fill is
 *          whatever sits between them.
 */
typedef struct {
  uint16_t head[k_urlguard_v6_groups]; /**< Groups seen before any `::`. */
  uint16_t tail[k_urlguard_v6_groups]; /**< Groups seen after a `::`.    */
  size_t   head_count;                 /**< Groups written to head.      */
  size_t   tail_count;                 /**< Groups written to tail.      */
  bool     seen_run;                   /**< A `::` has been consumed.    */
} urlguard_v6_parse_t;

/**
 * @brief Append one group to whichever side of the `::` is active.
 * @details Refuses rather than overruns when the side is already full.
 * @param[in,out] st    Parse state to append to.
 * @param[in]     group Group value to append.
 * @return Whether the group was appended.
 * @retval true  The group was stored.
 * @retval false The active side already holds every group it may.
 * @pre @p st is non-null and remains valid for the call.
 * @post On false, @p st is unchanged.
 * @note Thread-safe: writes only caller-provided storage.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_v6_push(urlguard_v6_parse_t* st, uint16_t group)
{
  uint16_t* dst   = st->seen_run ? st->tail : st->head;
  size_t*   count = st->seen_run ? &st->tail_count : &st->head_count;
  if (*count == (size_t)k_urlguard_v6_groups) {
    return false;
  }
  dst[*count] = group;
  ++(*count);
  return true;
}

/**
 * @brief Consume a trailing dotted quad as the final two groups.
 * @details This is the IPv4-mapped tail; classification later unwraps it.
 * @param[in]     text Candidate literal, NUL-terminated.
 * @param[in]     at   Cursor at the first byte of the quad.
 * @param[in,out] st   Parse state the two groups are appended to.
 * @return Whether the quad parsed and fitted.
 * @retval true  Both groups were appended.
 * @retval false The quad is malformed, or it does not fit.
 * @pre Every pointer is non-null and remains valid for the call.
 * @post On false, @p st may hold one appended group and is abandoned.
 * @note Thread-safe: writes only caller-provided storage.
 * @since 0.1.0
 */
RA8_INTERNAL static bool
internal_v6_take_quad(const char* text, size_t at, urlguard_v6_parse_t* st)
{
  unsigned char quad[k_urlguard_v4_bytes] = {};
  if (!internal_parse_v4(&text[at], quad)) {
    return false;
  }
  const uint16_t hi =
    (uint16_t)(((uint16_t)quad[0] << (unsigned)k_urlguard_byte_shift) | (uint16_t)quad[1]);
  const uint16_t lo =
    (uint16_t)(((uint16_t)quad[2] << (unsigned)k_urlguard_byte_shift) | (uint16_t)quad[3]);
  return internal_v6_push(st, hi) && internal_v6_push(st, lo);
}

/**
 * @brief Step the cursor past the separator that follows one group.
 * @details A single trailing colon is not a run, and any other trailing byte
 *          (a zone identifier, for one) ends the parse in refusal.
 * @param[in]     text Candidate literal, NUL-terminated.
 * @param[in,out] at   Cursor just past the group, advanced past a separator.
 * @param[out]    done Set when the literal ended cleanly here.
 * @return Whether the byte at the cursor is a legal continuation.
 * @retval true  The cursor advanced, or @p done was set.
 * @retval false A trailing colon or a stray byte follows the group.
 * @pre Every pointer is non-null and remains valid for the call.
 * @post On false, @p at and @p done are unspecified.
 * @note Thread-safe: writes only caller-provided storage.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_v6_step_separator(const char* text, size_t* at, bool* done)
{
  *done = false;
  if (text[*at] == '\0') {
    *done = true;
    return true;
  }
  if (text[*at] != ':') {
    return false;
  }
  if (text[*at + 1U] == '\0') {
    return false;
  }
  if (text[*at + 1U] != ':') {
    ++(*at);
  }
  return true;
}

/**
 * @brief Check the group budget and pack the parse state into 16 bytes.
 * @details Without a `::` the literal must carry every group; with one it must
 *          carry strictly fewer, since the run stands for at least one group.
 * @param[in]  st  Completed parse state.
 * @param[out] out Sixteen address bytes on success.
 * @return Whether the group counts describe a legal address.
 * @retval true  @p out holds the packed address.
 * @retval false The group budget was wrong; @p out is untouched.
 * @pre Both pointers are non-null and remain valid for the call.
 * @post On false, @p out is not written.
 * @note Thread-safe: writes only caller-provided storage.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_v6_finish(const urlguard_v6_parse_t* st, unsigned char* out)
{
  const size_t total = st->head_count + st->tail_count;
  if (st->seen_run) {
    if (total >= (size_t)k_urlguard_v6_groups) {
      return false;
    }
  } else if (total != (size_t)k_urlguard_v6_groups) {
    return false;
  }
  internal_pack_v6(st->head, st->head_count, st->tail, st->tail_count, out);
  return true;
}

/**
 * @brief Parse an IPv6 literal, including the `::` run and a trailing quad.
 * @details Accepts at most one `::`, rejects a zone identifier outright, and
 *          folds a trailing dotted quad into the final two groups so the
 *          IPv4-mapped form classifies as IPv4.
 * @param[in]  text Candidate literal, NUL-terminated.
 * @param[out] out  Sixteen address bytes on success.
 * @return Whether @p text is a readable IPv6 literal.
 * @retval true  @p out holds the sixteen parsed bytes.
 * @retval false @p text is not an IPv6 literal; @p out is unspecified.
 * @pre Both pointers are non-null and remain valid for the call.
 * @pre @p out has room for ::k_urlguard_v6_bytes bytes.
 * @post @p text is not modified.
 * @note Thread-safe: writes only caller-provided storage.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_parse_v6(const char* text, unsigned char* out)
{
  urlguard_v6_parse_t st = {};
  size_t              at = 0U;

  if ((text[0] == ':') && (text[1] != ':')) {
    return false;
  }
  if ((text[0] == ':') && (text[1] == ':')) {
    st.seen_run = true;
    at          = 2U;
  }

  while (text[at] != '\0') {
    if (text[at] == ':') {
      if (st.seen_run || (text[at + 1U] != ':')) {
        return false;
      }
      st.seen_run = true;
      at += 2U;
      continue;
    }
    const char* dot   = strchr(&text[at], '.');
    const char* colon = strchr(&text[at], ':');
    if ((dot != nullptr) && ((colon == nullptr) || (dot < colon))) {
      /* This group is the trailing dotted quad, not a later one. */
      return internal_v6_take_quad(text, at, &st) && internal_v6_finish(&st, out);
    }
    uint16_t group = 0U;
    if (!internal_read_v6_group(text, &at, &group) || !internal_v6_push(&st, group)) {
      return false;
    }
    bool done = false;
    if (!internal_v6_step_separator(text, &at, &done)) {
      return false;
    }
    if (done) {
      break;
    }
  }
  return internal_v6_finish(&st, out);
}

/**
 * @brief Classify the four octets of an IPv4 address.
 * @details Applies the non-public ranges in the order that makes each test
 *          independent of the ones before it.
 * @param[in] o Four parsed IPv4 octets.
 * @return Address class of the four octets in @p o.
 * @retval k_ra8_net_addr_public    Routable public address.
 * @retval k_ra8_net_addr_private   Private or carrier-grade NAT address.
 * @retval k_ra8_net_addr_loopback  Loopback address.
 * @retval k_ra8_net_addr_linklocal Link-local address.
 * @retval k_ra8_net_addr_unknown   Unspecified, multicast, or reserved.
 * @pre @p o is non-null and holds at least two readable octets.
 * @post No state is modified.
 * @note Thread-safe: depends only on its argument.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_net_addr_class_t internal_classify_v4(const unsigned char* o)
{
  const unsigned o0 = o[k_urlguard_v4_o0];
  const unsigned o1 = o[k_urlguard_v4_o1];
  if (o0 == (unsigned)k_urlguard_v4_zero_net) {
    return k_ra8_net_addr_unknown;
  }
  if (o0 == (unsigned)k_urlguard_v4_loopback_net) {
    return k_ra8_net_addr_loopback;
  }
  if (o0 >= (unsigned)k_urlguard_v4_multicast_min) {
    return k_ra8_net_addr_unknown;
  }
  if (o0 == (unsigned)k_urlguard_v4_linklocal) {
    return (o1 == (unsigned)k_urlguard_v4_linklocal_2) ? k_ra8_net_addr_linklocal
                                                       : k_ra8_net_addr_public;
  }
  const bool priv_a = (o0 == (unsigned)k_urlguard_v4_private_a);
  const bool priv_b = (o0 == (unsigned)k_urlguard_v4_private_b) &&
                      (o1 >= (unsigned)k_urlguard_v4_private_b_lo) &&
                      (o1 <= (unsigned)k_urlguard_v4_private_b_hi);
  const bool priv_c = (o0 == (unsigned)k_urlguard_v4_private_c) &&
                      (o1 == (unsigned)k_urlguard_v4_private_c_2);
  const bool cgnat = (o0 == (unsigned)k_urlguard_v4_cgnat) &&
                     (o1 >= (unsigned)k_urlguard_v4_cgnat_lo) &&
                     (o1 <= (unsigned)k_urlguard_v4_cgnat_hi);
  return (priv_a || priv_b || priv_c || cgnat) ? k_ra8_net_addr_private : k_ra8_net_addr_public;
}

/**
 * @brief True if the 16 IPv6 bytes carry an IPv4-mapped `::ffff:a.b.c.d`.
 * @details Ten zero bytes then two 0xFF bytes is the whole mapped prefix.
 * @param[in] b Sixteen parsed IPv6 address bytes.
 * @return True when @p b contains an IPv4-mapped IPv6 address.
 * @retval true  The mapped prefix is present.
 * @retval false The prefix is absent.
 * @pre @p b is non-null and holds ::k_urlguard_v6_bytes readable bytes.
 * @post No state is modified.
 * @note Thread-safe: depends only on its argument.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_is_v4_mapped(const unsigned char* b)
{
  for (size_t i = 0U; i < (size_t)k_urlguard_v6_mapped_ff_a; ++i) {
    if (b[i] != 0U) {
      return false;
    }
  }
  return (b[k_urlguard_v6_mapped_ff_a] == (unsigned char)k_urlguard_v6_byte_ff) &&
         (b[k_urlguard_v6_mapped_ff_b] == (unsigned char)k_urlguard_v6_byte_ff);
}

/**
 * @brief True if the 16 IPv6 bytes are the loopback address `::1`.
 * @details Fifteen zero bytes then a final 0x01.
 * @param[in] b Sixteen parsed IPv6 address bytes.
 * @return True when @p b is exactly the IPv6 loopback address.
 * @retval true  The address is `::1`.
 * @retval false The address is something else.
 * @pre @p b is non-null and holds ::k_urlguard_v6_bytes readable bytes.
 * @post No state is modified.
 * @note Thread-safe: depends only on its argument.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_is_v6_loopback(const unsigned char* b)
{
  for (size_t i = 0U; i < (size_t)k_urlguard_v6_last; ++i) {
    if (b[i] != 0U) {
      return false;
    }
  }
  return b[k_urlguard_v6_last] == (unsigned char)k_urlguard_v6_loopback_last;
}

/**
 * @brief True if the 16 IPv6 bytes are the unspecified address `::`.
 * @details The unspecified address is never fetchable, so it is classified
 *          alongside the literals the parser could not read.
 * @param[in] b Sixteen parsed IPv6 address bytes.
 * @return True when every byte is zero.
 * @retval true  The address is `::`.
 * @retval false At least one byte is set.
 * @pre @p b is non-null and holds ::k_urlguard_v6_bytes readable bytes.
 * @post No state is modified.
 * @note Thread-safe: depends only on its argument.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_is_v6_unspecified(const unsigned char* b)
{
  for (size_t i = 0U; i < (size_t)k_urlguard_v6_bytes; ++i) {
    if (b[i] != 0U) {
      return false;
    }
  }
  return true;
}

/**
 * @brief Classify the sixteen bytes of an IPv6 address.
 * @details An IPv4-mapped address is unwrapped and classified as IPv4, so a
 *          mapped loopback cannot slip past the loopback refusal.
 * @param[in] b Sixteen parsed IPv6 address bytes.
 * @return Address class of the sixteen bytes in @p b.
 * @retval k_ra8_net_addr_public    Routable public address.
 * @retval k_ra8_net_addr_private   Unique-local or mapped private address.
 * @retval k_ra8_net_addr_loopback  IPv6 or mapped IPv4 loopback address.
 * @retval k_ra8_net_addr_linklocal Link-local address.
 * @retval k_ra8_net_addr_unknown   Unspecified, multicast, or unsupported.
 * @pre @p b is non-null and holds ::k_urlguard_v6_bytes readable bytes.
 * @post No state is modified.
 * @note Thread-safe: depends only on its argument.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_net_addr_class_t internal_classify_v6(const unsigned char* b)
{
  if (internal_is_v4_mapped(b)) {
    return internal_classify_v4(b + (size_t)k_urlguard_v6_mapped_v4);
  }
  if (internal_is_v6_loopback(b)) {
    return k_ra8_net_addr_loopback;
  }
  if (internal_is_v6_unspecified(b)) {
    return k_ra8_net_addr_unknown;
  }
  if (b[k_urlguard_v4_o0] == (unsigned char)k_urlguard_v6_byte_ff) {
    return k_ra8_net_addr_unknown; /* multicast */
  }
  if ((b[k_urlguard_v4_o0] == (unsigned char)k_urlguard_v6_ll_lead) &&
      ((b[k_urlguard_v4_o1] & (unsigned char)k_urlguard_v6_ll_mask) ==
       (unsigned char)k_urlguard_v6_ll_value)) {
    return k_ra8_net_addr_linklocal;
  }
  if ((b[k_urlguard_v4_o0] & (unsigned char)k_urlguard_v6_ula_mask) ==
      (unsigned char)k_urlguard_v6_ula_value) {
    return k_ra8_net_addr_private;
  }
  return k_ra8_net_addr_public;
}

/**
 * @brief Locate the start of the authority in an http(s) URL.
 * @details The authority begins after the first `://` and after any userinfo
 *          that sits ahead of the first path separator.
 * @param[in] url URL to inspect, NUL-terminated.
 * @return Pointer into @p url at the first authority byte, or NULL.
 * @retval NULL @p url carries no `://`.
 * @pre @p url is non-null and remains valid for the call.
 * @post @p url is not modified.
 * @note Thread-safe: returns a pointer into the caller's own string.
 * @since 0.1.0
 */
RA8_INTERNAL static const char* internal_authority_of(const char* url)
{
  const char* sep = strstr(url, s_urlguard_authority_sep);
  if (sep == nullptr) {
    return nullptr;
  }
  const char* host = sep + strlen(s_urlguard_authority_sep);
  const char* at   = strchr(host, '@');
  const char* path = strpbrk(host, "/?#");
  if ((at != nullptr) && ((path == nullptr) || (at < path))) {
    host = at + 1U;
  }
  return host;
}

bool ra8_net_urlguard_scheme_allowed(const char* url)
{
  if ((url == nullptr) || (url[0] == '\0')) {
    return false;
  }
  return internal_starts_with_ci(url, s_urlguard_scheme_http) ||
         internal_starts_with_ci(url, s_urlguard_scheme_https);
}

ra8_net_addr_class_t ra8_net_urlguard_classify_ip(const char* ip)
{
  if ((ip == nullptr) || (ip[0] == '\0')) {
    return k_ra8_net_addr_unknown;
  }
  unsigned char v4[k_urlguard_v4_bytes] = {};
  if (internal_parse_v4(ip, v4)) {
    return internal_classify_v4(v4);
  }
  unsigned char v6[k_urlguard_v6_bytes] = {};
  if (internal_parse_v6(ip, v6)) {
    return internal_classify_v6(v6);
  }
  return k_ra8_net_addr_unknown;
}

bool ra8_net_urlguard_addr_fetchable(ra8_net_addr_class_t cls, bool allow_private)
{
  if (cls == k_ra8_net_addr_public) {
    return true;
  }
  if (cls == k_ra8_net_addr_unknown) {
    return false;
  }
  return allow_private;
}

bool ra8_net_urlguard_size_exceeds(uint64_t have, uint64_t add, uint64_t cap)
{
  if (cap == 0U) {
    return false;
  }
  if (have > cap) {
    return true;
  }
  return add > (cap - have);
}

ra8_err_t ra8_net_urlguard_host(const char* url, char* out, size_t cap)
{
  if ((out == nullptr) || (cap == 0U)) {
    return k_ra8_err_invalid_arg;
  }
  out[0] = '\0';
  if (url == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  const char* host = internal_authority_of(url);
  if (host == nullptr) {
    return k_ra8_err_not_found;
  }
  size_t n = 0U;
  /* Keep the port: a politeness policy and a same-origin redirect check are
   * both scoped per (scheme, host, port), so a different port is a different
   * origin. Only userinfo is dropped, in internal_authority_of(). */
  while ((host[n] != '\0') && (host[n] != '/') && (host[n] != '?') && (host[n] != '#')) {
    if ((n + 1U) >= cap) {
      out[0] = '\0';
      return k_ra8_err_no_mem;
    }
    out[n] = internal_lower_ascii(host[n]);
    ++n;
  }
  out[n] = '\0';
  if (n == 0U) {
    return k_ra8_err_not_found;
  }
  return k_ra8_ok;
}

ra8_err_t ra8_net_urlguard_path(const char* url, char* out, size_t cap)
{
  if ((out == nullptr) || (cap == 0U)) {
    return k_ra8_err_invalid_arg;
  }
  out[0] = '\0';
  if (url == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  const char* sep = strstr(url, s_urlguard_authority_sep);
  if (sep == nullptr) {
    return k_ra8_err_not_found;
  }
  const char* host = sep + strlen(s_urlguard_authority_sep);
  const char* path = strchr(host, '/');
  if (path == nullptr) {
    if (cap < 2U) {
      return k_ra8_err_no_mem;
    }
    out[0] = '/';
    out[1] = '\0';
    return k_ra8_ok;
  }
  size_t n = 0U;
  while ((path[n] != '\0') && (path[n] != '?') && (path[n] != '#')) {
    if ((n + 1U) >= cap) {
      out[0] = '\0';
      return k_ra8_err_no_mem;
    }
    out[n] = path[n];
    ++n;
  }
  out[n] = '\0';
  return k_ra8_ok;
}
