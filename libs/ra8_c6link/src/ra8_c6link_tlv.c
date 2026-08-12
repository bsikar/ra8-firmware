/**
 * @file ra8_c6link_tlv.c
 * @brief The two-tag envelope the co-processor's serial endpoint speaks.
 * @ingroup grp_net
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details
 * Six bytes of framing sit between the payload header and the protobuf message:
 * a tag naming the endpoint, then a tag introducing the data. Upstream builds
 * and parses them in `compose_tlv()` and `parse_tlv()` inside
 * `drivers/virtual_serial_if/serial_if.c`, which this tree does not compile --
 * its transmit path expands `HOSTED_CALLOC`, whose failure arm is a `goto` to a
 * caller-supplied label, and NASA Power of 10 Rule 1 forbids that. The envelope
 * itself is trivial, so it is restated here rather than dragged in with a rule
 * violation attached.
 *
 * Both endpoint names are accepted on receive. A response arrives on `RPCRsp`
 * and an unsolicited event on `RPCEvt`; upstream requires every registered
 * endpoint name to be the same length, and its own parser checks that, so the
 * two are interchangeable as far as the framing is concerned.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_c6link.h"
#include "ra8_c6link_internal.h"

/* Upstream requires both endpoint names to be the same length, and the
   receive path below relies on it to accept either without a second offset
   calculation. */
static_assert(sizeof(RPC_EP_NAME_RSP) == sizeof(RPC_EP_NAME_EVT),
              "esp-hosted requires every RPC endpoint name to have one length");

/**
 * @brief Write one three-byte tag header at a cursor.
 * @param[out] out Buffer being filled; must be non-null.
 * @details One tag header is a type octet and a little-endian length, and both
 *        tags of the envelope are written by this.
 * @param[in] at Offset to write at.
 * @param[in] type Tag type.
 * @param[in] len Value length that follows.
 * @return The offset just past the tag header, which addresses its value.
 * @retval non-zero The offset of the tag's value, always three past @p at.
 * @pre At least ::k_ra8_c6link_tlv_value bytes are writable at `out + at`.
 * @pre The caller has already checked the buffer capacity.
 * @post Exactly three bytes were written, length low octet first.
 * @post The returned offset addresses the tag's value.
 * @note Matches the byte order `compose_tlv()` writes upstream.
 * @since 0.1.0
 */
RA8_INTERNAL static uint16_t
ra8_c6link_tlv_tag(uint8_t* out, uint16_t at, uint8_t type, uint16_t len)
{
  out[at + (uint16_t)k_ra8_c6link_tlv_type]   = type;
  out[at + (uint16_t)k_ra8_c6link_tlv_len_lo] = (uint8_t)(len & (uint16_t)k_ra8_c6link_tlv_mask);
  out[at + (uint16_t)k_ra8_c6link_tlv_len_hi] =
    (uint8_t)((len >> (uint16_t)k_ra8_c6link_tlv_shift) & (uint16_t)k_ra8_c6link_tlv_mask);
  return (uint16_t)(at + (uint16_t)k_ra8_c6link_tlv_value);
}

/**
 * @brief Read one tag's little-endian 16-bit length.
 * @details Reads what the sender wrote rather than assuming the host's own
 *        byte order, which is what makes the parser portable to a big-endian
 *        host.
 * @param[in] buf Buffer holding the tag; must be non-null.
 * @param[in] at Offset of the tag's type byte.
 * @return The length the tag declares.
 * @retval 0 The tag declares an empty value.
 * @pre At least ::k_ra8_c6link_tlv_value bytes are readable at `buf + at`.
 * @pre The caller has already bounds-checked @p at against the payload.
 * @post No buffer is modified.
 * @post The result is the value the sender wrote, byte order reversed.
 * @note Split out so both tags are read by the same code path.
 * @since 0.1.0
 */
RA8_INTERNAL static uint16_t ra8_c6link_tlv_len(const uint8_t* buf, uint16_t at)
{
  const uint16_t lo = (uint16_t)buf[at + (uint16_t)k_ra8_c6link_tlv_len_lo];
  const uint16_t hi = (uint16_t)buf[at + (uint16_t)k_ra8_c6link_tlv_len_hi];
  return (uint16_t)(lo | (uint16_t)(hi << (uint16_t)k_ra8_c6link_tlv_shift));
}

RA8_PRIV ra8_err_t ra8_c6link_priv_tlv_open(uint8_t*  out,
                                            uint16_t  cap,
                                            uint16_t  proto_len,
                                            uint16_t* body_at)
{
  if ((out == nullptr) || (body_at == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  *body_at = 0U;
  if ((uint32_t)cap < ((uint32_t)proto_len + (uint32_t)k_ra8_c6link_tlv_overhead)) {
    return k_ra8_err_invalid_size;
  }

  uint16_t at = ra8_c6link_tlv_tag(out,
                                   0U,
                                   (uint8_t)k_ra8_c6link_tlv_t_epname,
                                   (uint16_t)k_ra8_c6link_tlv_ep_len);
  for (uint16_t i = 0U; i < (uint16_t)k_ra8_c6link_tlv_ep_len; i++) {
    out[at + i] = (uint8_t)RPC_EP_NAME_RSP[i];
  }
  at       = (uint16_t)(at + (uint16_t)k_ra8_c6link_tlv_ep_len);
  *body_at = ra8_c6link_tlv_tag(out, at, (uint8_t)k_ra8_c6link_tlv_t_data, proto_len);
  return k_ra8_ok;
}

/**
 * @brief Check that a payload names one of the two RPC endpoints.
 * @details Compares against both registered endpoint names at once, so a
 *        response and an unsolicited event are accepted by one pass rather
 *        than two.
 * @param[in] payload Frame payload; must be non-null and long enough.
 * @return true when every octet matches `RPCRsp` or matches `RPCEvt`.
 * @retval true The envelope is addressed to this host's RPC endpoint.
 * @retval false At least one octet matched neither name.
 * @pre ::k_ra8_c6link_tlv_overhead bytes are readable at @p payload.
 * @pre Both endpoint names have the same length, which a `static_assert`
 *      above guarantees.
 * @post No buffer is modified.
 * @post The loop ran exactly ::k_ra8_c6link_tlv_ep_len times.
 * @note The loop is bounded by ::k_ra8_c6link_tlv_ep_len (NASA Rule 2).
 * @since 0.1.0
 */
RA8_INTERNAL static bool ra8_c6link_tlv_named(const uint8_t* payload)
{
  bool named = true;
  for (uint16_t i = 0U; i < (uint16_t)k_ra8_c6link_tlv_ep_len; i++) {
    const uint8_t got = payload[(uint16_t)k_ra8_c6link_tlv_value + i];
    if ((got != (uint8_t)RPC_EP_NAME_RSP[i]) && (got != (uint8_t)RPC_EP_NAME_EVT[i])) {
      named = false;
    }
  }
  return named;
}

RA8_PRIV const uint8_t*
ra8_c6link_priv_tlv_body(const uint8_t* payload, uint16_t len, uint16_t* proto_len)
{
  if ((payload == nullptr) || (proto_len == nullptr)) {
    return nullptr;
  }
  *proto_len = 0U;
  const uint16_t data_tag =
    (uint16_t)((uint16_t)k_ra8_c6link_tlv_value + (uint16_t)k_ra8_c6link_tlv_ep_len);

  if ((len < (uint16_t)k_ra8_c6link_tlv_overhead) ||
      (payload[k_ra8_c6link_tlv_type] != (uint8_t)k_ra8_c6link_tlv_t_epname) ||
      (payload[data_tag] != (uint8_t)k_ra8_c6link_tlv_t_data)) {
    return nullptr;
  }
  if (ra8_c6link_tlv_len(payload, 0U) != (uint16_t)k_ra8_c6link_tlv_ep_len) {
    return nullptr;
  }
  if (!ra8_c6link_tlv_named(payload)) {
    return nullptr;
  }

  const uint16_t body = ra8_c6link_tlv_len(payload, data_tag);
  if ((body == 0U) || (((uint32_t)body + (uint32_t)k_ra8_c6link_tlv_overhead) > (uint32_t)len)) {
    return nullptr;
  }
  *proto_len = body;
  return &payload[k_ra8_c6link_tlv_overhead];
}
