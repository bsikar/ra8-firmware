/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file examples/ek_ra8d2/hw_validated/c6/c6_spi_probe/src/c6_frame.c
 * @brief Hand-decoded esp-hosted payload header for the ESP32-C6 SPI probe
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Unpacks and judges ``struct esp_payload_header`` (esp-hosted-mcu
 * ``common/esp_hosted_header.h``, pinned commit ``949bb30``) straight out of
 * the receive buffer, without vendoring any upstream source. Field offsets,
 * endianness and the acceptance rules all live in ``c6_proto.h`` next to the
 * upstream file they were read from.
 *
 * @since 0.1.0
 */

#include <stdint.h>

#include "c6_probe.h"
#include "c6_proto.h"

/**
 * @brief Read a little-endian 16-bit field out of a byte buffer.
 *
 * @details Matches ``le16toh`` as used throughout esp-hosted-mcu, and is
 * byte-order-correct on any host because it reassembles from bytes rather
 * than aliasing a ``uint16_t``.
 *
 * @param[in] buf    Source buffer.
 * @param[in] off_lo Offset of the low byte; the high byte follows it.
 *
 * @return The reassembled 16-bit value, or zero when ``buf`` is NULL.
 * @retval 0 ``buf`` was NULL or the field genuinely reads zero.
 *
 * @pre ``buf`` holds at least ``off_lo + 2`` bytes.
 * @pre ``off_lo`` is a header offset from ::c6_hdr_offset_t.
 * @post No buffer byte is modified.
 * @post The result is independent of host endianness.
 *
 * @note Pure function; safe from any context.
 * @since 0.1.0
 */
static uint16_t internal_le16(const uint8_t* buf, uint8_t off_lo)
{
  if (buf == nullptr) {
    return 0U;
  }
  return (uint16_t)((uint16_t)buf[off_lo] |
                    (uint16_t)((uint16_t)buf[off_lo + 1U] << (uint16_t)k_c6_shift_byte));
}

uint16_t c6_probe_checksum(const uint8_t* buf, uint16_t count)
{
  if (buf == nullptr) {
    return 0U;
  }
  const uint16_t capped =
    (count > (uint16_t)k_c6_proto_buf_size) ? (uint16_t)k_c6_proto_buf_size : count;
  uint16_t sum = 0U;
  for (uint16_t i = 0U; i < capped; i++) {
    if (i == (uint16_t)k_c6_hdr_off_csum_lo || i == (uint16_t)k_c6_hdr_off_csum_hi) {
      continue;
    }
    sum = (uint16_t)(sum + (uint16_t)buf[i]);
  }
  return sum;
}

void c6_probe_decode_header(const uint8_t* buf, c6_hdr_t* out)
{
  if (buf == nullptr || out == nullptr) {
    return;
  }
  const uint8_t iface = buf[k_c6_hdr_off_iface];
  out->if_type        = (uint8_t)(iface & (uint8_t)k_c6_mask_nibble);
  out->if_num   = (uint8_t)((iface >> (uint8_t)k_c6_shift_nibble) & (uint8_t)k_c6_mask_nibble);
  out->flags    = buf[k_c6_hdr_off_flags];
  out->len      = internal_le16(buf, (uint8_t)k_c6_hdr_off_len_lo);
  out->offset   = internal_le16(buf, (uint8_t)k_c6_hdr_off_off_lo);
  out->checksum = internal_le16(buf, (uint8_t)k_c6_hdr_off_csum_lo);
  out->seq_num  = internal_le16(buf, (uint8_t)k_c6_hdr_off_seq_lo);
  out->throttle = (uint8_t)(buf[k_c6_hdr_off_throttle] & (uint8_t)k_c6_mask_throttle);
  out->pkt_type = buf[k_c6_hdr_off_pkttype];
  out->computed = 0U;
  if (out->offset == (uint16_t)k_c6_proto_hdr_size &&
      out->len <= (uint16_t)k_c6_proto_payload_max) {
    out->computed = c6_probe_checksum(buf, (uint16_t)(out->offset + out->len));
  }
}

/** @brief Implementation of `c6_probe_classify()` -- filler check first. */
c6_frame_kind_t c6_probe_classify(const c6_hdr_t* h)
{
  if (h == nullptr) {
    return k_c6_frame_garbage;
  }
  if (h->if_type == (uint8_t)k_c6_if_max && h->if_num == (uint8_t)k_c6_dummy_if_num &&
      h->len == 0U) {
    return k_c6_frame_idle;
  }
  if (h->offset != (uint16_t)k_c6_proto_hdr_size) {
    return k_c6_frame_garbage;
  }
  if (h->len == 0U || h->len > (uint16_t)k_c6_proto_payload_max) {
    return k_c6_frame_garbage;
  }
  if (h->if_type >= (uint8_t)k_c6_if_max) {
    return k_c6_frame_garbage;
  }
  return (h->computed == h->checksum) ? k_c6_frame_data : k_c6_frame_bad_csum;
}

void c6_probe_print_header(const c6_hdr_t* h)
{
  if (h == nullptr) {
    return;
  }
  c6_probe_puts("    hdr if_type=");
  c6_probe_put_u32((uint32_t)h->if_type);
  c6_probe_puts(" if_num=");
  c6_probe_put_u32((uint32_t)h->if_num);
  c6_probe_puts(" flags=0x");
  c6_probe_put_hex((uint32_t)h->flags, (uint8_t)k_c6_fmt_hex_byte);
  c6_probe_puts(" len=");
  c6_probe_put_u32((uint32_t)h->len);
  c6_probe_puts(" offset=");
  c6_probe_put_u32((uint32_t)h->offset);
  c6_probe_puts(" csum=0x");
  c6_probe_put_hex((uint32_t)h->checksum, (uint8_t)k_c6_fmt_hex_word);
  c6_probe_puts(" calc=0x");
  c6_probe_put_hex((uint32_t)h->computed, (uint8_t)k_c6_fmt_hex_word);
  c6_probe_puts(" seq=");
  c6_probe_put_u32((uint32_t)h->seq_num);
  c6_probe_puts(" pkt=0x");
  c6_probe_put_hex((uint32_t)h->pkt_type, (uint8_t)k_c6_fmt_hex_byte);
  c6_probe_puts("\r\n");
}

void c6_probe_dump_payload(const uint8_t* buf, uint16_t len)
{
  if (buf == nullptr || len == 0U) {
    return;
  }
  const uint16_t shown =
    (len > (uint16_t)k_c6_probe_dump_bytes) ? (uint16_t)k_c6_probe_dump_bytes : len;
  c6_probe_puts("    payload");
  for (uint16_t i = 0U; i < shown; i++) {
    c6_probe_puts(" ");
    c6_probe_put_hex((uint32_t)buf[(uint16_t)k_c6_proto_hdr_size + i], (uint8_t)k_c6_fmt_hex_byte);
  }
  c6_probe_puts("\r\n");
}
