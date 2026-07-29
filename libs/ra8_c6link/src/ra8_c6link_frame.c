/**
 * @file ra8_c6link_frame.c
 * @brief The twelve-byte esp-hosted payload header: build it, believe it or not.
 * @ingroup grp_net
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details
 * Every transaction on this link carries the same header in front of whatever
 * it is transporting: an interface nibble pair, a length, an offset, a checksum
 * and a sequence number. This file is the only place that reads or writes it.
 *
 * The header is assembled in a local `struct esp_payload_header` and copied
 * into the transaction, rather than reached by casting the byte buffer to a
 * header pointer. That keeps the vendored declaration as the single source of
 * truth for the layout while staying inside what C actually defines: the cast
 * would be an aliasing violation, and the `(T*)(void*)` spelling that silences
 * the compiler is exactly what clang-tidy's `bugprone-casting-through-void`
 * exists to catch.
 *
 * @par Verifying a checksum without touching the buffer
 * The frame checksum is a plain 16-bit sum of every byte with the checksum
 * field taken as zero. Upstream verifies it by zeroing the field in place,
 * summing, and putting it back. Subtracting the two checksum bytes from the sum
 * of the whole span gives the identical value and leaves the received buffer
 * untouched, which is what lets a frame be classified without a write.
 *
 * @par Sequence numbers
 * The transmitted `seq_num` stays zero. Upstream increments it, but the bench
 * run that proved this link answered a request whose sequence number was zero,
 * and nothing in the co-processor's reply depended on it. Changing a proven
 * variable for cosmetic parity is how a working link acquires an unexplained
 * failure, so it stays as it was measured.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * @since 0.1.0
 */

#include <stdint.h>
#include <string.h>

#include "ra8_attributes.h"
#include "ra8_c6link.h"
#include "ra8_c6link_internal.h"

/**
 * @enum ra8_c6link_hdr_t
 * @brief Field-level constants the header writer and reader need.
 *
 * @details
 * The nibble mask exists because `if_type` and `if_num` share one byte: an
 * unmasked assignment to a four-bit bitfield is a narrowing conversion this
 * project's `-Wconversion` rejects.
 *
 * @invariant ::k_ra8_c6link_hdr_nibble is the widest value either interface
 *            field can hold.
 * @invariant ::k_ra8_c6link_hdr_csum_at is where the checksum's two octets sit
 *            in the header, which the verifier subtracts rather than zeroes.
 *
 * @par Example:
 * @code
 * hdr.if_type = (uint8_t)(if_type & (uint8_t)k_ra8_c6link_hdr_nibble);
 * @endcode
 *
 * @see ra8_c6link_priv_frame_seal
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_ra8_c6link_hdr_nibble  = 0x0FU, /**< Mask for a four-bit header field.       */
  k_ra8_c6link_hdr_seq     = 0U,    /**< Sequence number transmitted; see above. */
  k_ra8_c6link_hdr_csum_at = 6U,    /**< Offset of the checksum's low octet.     */
  k_ra8_c6link_hdr_csum_sz = 2U,    /**< Octets the checksum field occupies.     */
} ra8_c6link_hdr_t;

/* The public header restates these so consumers need no esp-hosted include
   path. If upstream ever changes either, the build stops here rather than
   mis-framing on the wire. */
static_assert((uint16_t)k_ra8_c6link_header_bytes == (uint16_t)sizeof(struct esp_payload_header),
              "k_ra8_c6link_header_bytes must equal sizeof(struct esp_payload_header)");
static_assert((uint16_t)k_ra8_c6link_frame_bytes == (uint16_t)ESP_TRANSPORT_SPI_MAX_BUF_SIZE,
              "k_ra8_c6link_frame_bytes must equal ESP_TRANSPORT_SPI_MAX_BUF_SIZE");
static_assert((uint16_t)k_ra8_c6link_max_payload ==
                ((uint16_t)k_ra8_c6link_frame_bytes - (uint16_t)k_ra8_c6link_header_bytes),
              "k_ra8_c6link_max_payload must be the frame size less the header");

/**
 * @brief Zero part of a transaction buffer.
 * @details Takes a starting offset so a payload already staged in the
 *        transaction survives, which is what lets the encoder write in place.
 * @param[out] frame Buffer to clear; must be non-null.
 * @param[in] from First byte to clear, so a staged payload can be preserved.
 * @return Nothing.
 * @pre @p frame is ::k_ra8_c6link_frame_bytes long.
 * @pre @p from is at most ::k_ra8_c6link_frame_bytes.
 * @post Every byte from @p from onwards is zero.
 * @post Bytes before @p from are untouched.
 * @note The loop is bounded by ::k_ra8_c6link_frame_bytes (NASA Rule 2).
 * @since 0.1.0
 */
RA8_INTERNAL static void ra8_c6link_frame_clear(uint8_t* frame, uint16_t from)
{
  for (uint16_t i = from; i < (uint16_t)k_ra8_c6link_frame_bytes; i++) {
    frame[i] = 0U;
  }
}

/**
 * @brief Recompute a frame's checksum as if its checksum field were zero.
 * @details Subtracts the transmitted checksum octets from the sum of the whole
 *        span, which is arithmetically identical to upstream's zero-the-field
 *        method and leaves the received buffer untouched.
 * @param[in] frame Transaction buffer; must be non-null and not modified.
 * @param[in] span Bytes the checksum is defined over: header plus payload.
 * @return The checksum the sender should have transmitted.
 * @retval 0 Every octet in the span, and the checksum field, were zero.
 * @pre @p span is at least ::k_ra8_c6link_header_bytes, so the checksum field
 *      is inside the summed range.
 * @pre @p span is at most ::k_ra8_c6link_frame_bytes.
 * @post @p frame is unmodified.
 * @post The result equals what upstream computes by zeroing the field.
 * @note The arithmetic is deliberately 16-bit and wrapping, matching the
 *       accumulator in the vendored `compute_checksum()`.
 * @since 0.1.0
 */
RA8_INTERNAL static uint16_t ra8_c6link_frame_sum(uint8_t* frame, uint16_t span)
{
  uint16_t sum = compute_checksum(frame, span);
  for (uint8_t i = 0U; i < (uint8_t)k_ra8_c6link_hdr_csum_sz; i++) {
    sum = (uint16_t)(sum - frame[(uint8_t)k_ra8_c6link_hdr_csum_at + i]);
  }
  return sum;
}

RA8_PRIV void ra8_c6link_priv_frame_filler(uint8_t* tx)
{
  if (tx == nullptr) {
    return;
  }
  struct esp_payload_header hdr = {};
  hdr.if_type                   = (uint8_t)((uint8_t)ESP_MAX_IF & (uint8_t)k_ra8_c6link_hdr_nibble);

  ra8_c6link_frame_clear(tx, 0U);
  (void)memcpy(tx, &hdr, sizeof hdr);
}

RA8_PRIV void ra8_c6link_priv_frame_seal(uint8_t* tx, uint8_t if_type, uint8_t if_num, uint16_t len)
{
  if ((tx == nullptr) || (len > (uint16_t)k_ra8_c6link_max_payload)) {
    return;
  }
  const uint16_t span = (uint16_t)((uint16_t)k_ra8_c6link_header_bytes + len);

  struct esp_payload_header hdr = {};
  hdr.if_type                   = (uint8_t)(if_type & (uint8_t)k_ra8_c6link_hdr_nibble);
  hdr.if_num                    = (uint8_t)(if_num & (uint8_t)k_ra8_c6link_hdr_nibble);
  hdr.len                       = len;
  hdr.offset                    = (uint16_t)k_ra8_c6link_header_bytes;
  hdr.seq_num                   = (uint16_t)k_ra8_c6link_hdr_seq;

  ra8_c6link_frame_clear(tx, span);
  (void)memcpy(tx, &hdr, sizeof hdr);
  hdr.checksum = compute_checksum(tx, span);
  (void)memcpy(tx, &hdr, sizeof hdr);
}

/**
 * @brief Write one one-octet TLV of the capabilities frame at a cursor.
 * @details Every tag the announcement carries has a one-octet value, so the
 *        three-octet shape is written once here rather than five times.
 * @param[out] out Buffer being filled; must be non-null.
 * @param[in] at Offset to write at.
 * @param[in] tag Tag identifier.
 * @param[in] value The tag's single-octet value.
 * @return The offset just past this TLV.
 * @retval non-zero Always three past @p at.
 * @pre Three octets are writable at `out + at`.
 * @pre The caller has already checked the buffer capacity.
 * @post Exactly three octets were written.
 * @post The returned offset addresses the next TLV.
 * @note Matches the layout upstream's `send_slave_config()` composes.  LEGACY-OK: send_slave_config() is the upstream esp-hosted symbol name
 * @since 0.1.0
 */
RA8_INTERNAL static uint8_t
ra8_c6link_caps_tlv(uint8_t* out, uint8_t at, uint8_t tag, uint8_t value)
{
  out[at]      = tag;
  out[at + 1U] = (uint8_t)k_ra8_c6link_caps_value_len;
  out[at + 2U] = value;
  return (uint8_t)(at + (uint8_t)k_ra8_c6link_caps_stride);
}

RA8_PRIV uint8_t ra8_c6link_priv_caps(uint8_t* out, uint8_t cap)
{
  if ((out == nullptr) || (cap < (uint8_t)k_ra8_c6link_caps_bytes)) {
    return 0U;
  }
  const uint8_t tlv_bytes =
    (uint8_t)((uint8_t)k_ra8_c6link_caps_tags * (uint8_t)k_ra8_c6link_caps_stride);

  out[k_ra8_c6link_caps_type] = (uint8_t)ESP_PRIV_EVENT_INIT;
  out[k_ra8_c6link_caps_len]  = tlv_bytes;

  uint8_t at = (uint8_t)k_ra8_c6link_caps_hdr;
  at = ra8_c6link_caps_tlv(out, at, (uint8_t)HOST_CAPABILITIES, (uint8_t)k_ra8_c6link_caps_host);
  at = ra8_c6link_caps_tlv(out,
                           at,
                           (uint8_t)RCVD_ESP_FIRMWARE_CHIP_ID,
                           (uint8_t)k_ra8_c6link_caps_chip);
  at = ra8_c6link_caps_tlv(out,
                           at,
                           (uint8_t)SLV_CONFIG_TEST_RAW_TP,
                           (uint8_t)k_ra8_c6link_caps_raw_tp);
  at = ra8_c6link_caps_tlv(out,
                           at,
                           (uint8_t)SLV_CONFIG_THROTTLE_HIGH_THRESHOLD,
                           (uint8_t)k_ra8_c6link_caps_throttle_high);
  at = ra8_c6link_caps_tlv(out,
                           at,
                           (uint8_t)SLV_CONFIG_THROTTLE_LOW_THRESHOLD,
                           (uint8_t)k_ra8_c6link_caps_throttle_low);
  return at;
}

RA8_PRIV ra8_c6link_frame_class_t ra8_c6link_priv_frame_classify(uint8_t*              rx,
                                                                 ra8_c6link_rx_view_t* view)
{
  if ((rx == nullptr) || (view == nullptr)) {
    return k_ra8_c6link_frame_malformed;
  }
  struct esp_payload_header hdr = {};
  (void)memcpy(&hdr, rx, sizeof hdr);

  if (hdr.len == 0U) {
    return k_ra8_c6link_frame_idle;
  }
  if ((hdr.offset != (uint16_t)k_ra8_c6link_header_bytes) ||
      (hdr.len > (uint16_t)k_ra8_c6link_max_payload)) {
    return k_ra8_c6link_frame_malformed;
  }
  if (ra8_c6link_frame_sum(rx, (uint16_t)(hdr.offset + hdr.len)) != hdr.checksum) {
    return k_ra8_c6link_frame_bad_checksum;
  }

  view->offset  = hdr.offset;
  view->len     = hdr.len;
  view->if_type = (uint8_t)hdr.if_type;
  view->if_num  = (uint8_t)hdr.if_num;
  return k_ra8_c6link_frame_data;
}
