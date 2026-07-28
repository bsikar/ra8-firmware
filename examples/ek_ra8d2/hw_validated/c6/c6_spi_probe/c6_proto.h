/**
 * @file c6_proto.h
 * @brief The esp-hosted SPI full-duplex wire format, hand-decoded.
 *
 * @details
 * Everything in this header is a fact about the **protocol**, not about this
 * probe: the transport dimensions, the twelve-byte payload header and its
 * field offsets, the interface identifiers, and the operations that decode,
 * checksum, classify and print a received frame (implemented by
 * `src/c6_frame.c`). `c6_probe.h` carries the probe's own contract -- its
 * budgets, its side-band map and its transfer flow -- and includes this
 * header for the types they share.
 *
 * The split is a responsibility boundary, not just a size one: a second
 * consumer of this link (the first-party esp-hosted port, when it lands) will
 * need these constants and will not want the probe's diagnostics.
 *
 * Nothing from esp-hosted-mcu is vendored. Every constant below is
 * hand-decoded from the pinned upstream tree (commit `949bb30`, firmware
 * `2.12.11`; see `coprocessor/esp32c6/pins.env`), and each block names the
 * upstream file it came from:
 *
 *   - `common/esp_hosted_header.h` -- `struct esp_payload_header`.
 *   - `common/transport/esp_hosted_transport.h` -- buffer size, interface
 *     identifiers, `compute_checksum()`.
 *   - `host/drivers/transport/spi/spi_drv.c` -- reference host pacing and
 *     receive validation.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * [Ring 6 / App] {World: S}
 *
 * @since 0.1.0
 */
#pragma once

#include <stdint.h>

/* =============================================================================
 * 1. Transport dimensions, header layout and interface identifiers
 * =============================================================================
 */

/**
 * @enum c6_proto_dim_t
 * @brief Fixed transport dimensions of the esp-hosted SPI FD transport.
 *
 * @details
 * Source: esp-hosted-mcu ``common/transport/esp_hosted_transport.h``
 * (``ESP_TRANSPORT_SPI_MAX_BUF_SIZE``) and ``common/esp_hosted_header.h``
 * (``struct esp_payload_header``, twelve packed bytes). Both peers set the
 * transaction length to the full buffer regardless of how much payload is
 * live, so the controller must always clock exactly
 * ::k_c6_proto_buf_size bytes.
 *
 * @invariant ``k_c6_proto_payload_max + k_c6_proto_hdr_size ==
 *            k_c6_proto_buf_size``.
 *
 * @par Example:
 * @code
 * static uint8_t rx[k_c6_proto_buf_size];
 * @endcode
 *
 * @see c6_hdr_offset_t
 */
typedef enum : uint16_t {
  k_c6_proto_buf_size    = 1600U, /**< Bytes clocked per transaction.        */
  k_c6_proto_hdr_size    = 12U,   /**< sizeof(struct esp_payload_header).    */
  k_c6_proto_payload_max = 1588U, /**< Largest payload the header may claim. */
} c6_proto_dim_t;

/**
 * @enum c6_hdr_offset_t
 * @brief Byte offsets of every field in the esp-hosted payload header.
 *
 * @details
 * Hand-decoded from ``struct esp_payload_header`` in esp-hosted-mcu
 * ``common/esp_hosted_header.h``. The struct is packed and every
 * multi-byte field is little-endian on the wire (the reference host wraps
 * each one in ``le16toh`` / ``htole16``). Byte 0 packs two nibbles:
 * ``if_type`` occupies bits 3:0 and ``if_num`` bits 7:4, which is how the
 * little-endian ABI lays out the two four-bit members in declaration
 * order.
 *
 * @invariant Every offset is below ::k_c6_proto_hdr_size.
 *
 * @par Example:
 * @code
 * const uint8_t iface = rx[k_c6_hdr_off_iface];
 * @endcode
 *
 * @see c6_hdr_t
 */
typedef enum : uint8_t {
  k_c6_hdr_off_iface    = 0U,  /**< if_type[3:0] | if_num[7:4].        */
  k_c6_hdr_off_flags    = 1U,  /**< Fragment / wake / power-save bits. */
  k_c6_hdr_off_len_lo   = 2U,  /**< Payload length, low byte.          */
  k_c6_hdr_off_len_hi   = 3U,  /**< Payload length, high byte.         */
  k_c6_hdr_off_off_lo   = 4U,  /**< Payload offset, low byte.          */
  k_c6_hdr_off_off_hi   = 5U,  /**< Payload offset, high byte.         */
  k_c6_hdr_off_csum_lo  = 6U,  /**< Checksum, low byte.                */
  k_c6_hdr_off_csum_hi  = 7U,  /**< Checksum, high byte.               */
  k_c6_hdr_off_seq_lo   = 8U,  /**< Sequence number, low byte.         */
  k_c6_hdr_off_seq_hi   = 9U,  /**< Sequence number, high byte.        */
  k_c6_hdr_off_throttle = 10U, /**< throttle_cmd[1:0] | reserved[7:2]. */
  k_c6_hdr_off_pkttype  = 11U, /**< reserved3 / hci_ or priv_pkt_type. */
} c6_hdr_offset_t;

/**
 * @enum c6_if_type_t
 * @brief esp-hosted interface identifiers carried in ``if_type``.
 *
 * @details
 * Mirrors ``esp_hosted_if_type_t`` in esp-hosted-mcu
 * ``common/esp_hosted_interface.h``. ::k_c6_if_max doubles as the "this
 * frame carries nothing" marker: both the reference host (``spi_drv.c``)
 * and the C6's peripheral-side SPI driver stamp it into the header of an idle
 * filler transaction.
 *
 * @invariant Values are contiguous from zero, matching the upstream enum.
 *
 * @par Example:
 * @code
 * tx[k_c6_hdr_off_iface] = (uint8_t)k_c6_if_max;  // idle filler frame
 * @endcode
 *
 * @see c6_proto_priv_t
 */
typedef enum : uint8_t {
  k_c6_if_invalid = 0U, /**< Unused slot zero.                    */
  k_c6_if_sta     = 1U, /**< Wi-Fi station data.                  */
  k_c6_if_ap      = 2U, /**< Wi-Fi soft-AP data.                  */
  k_c6_if_serial  = 3U, /**< Control-plane (RPC) channel.         */
  k_c6_if_hci     = 4U, /**< Bluetooth HCI.                       */
  k_c6_if_priv    = 5U, /**< Private events, e.g. the INIT event. */
  k_c6_if_test    = 6U, /**< Raw throughput test channel.         */
  k_c6_if_eth     = 7U, /**< Ethernet data.                       */
  k_c6_if_max     = 8U, /**< Idle filler marker.                  */
} c6_if_type_t;

/**
 * @enum c6_proto_priv_t
 * @brief Private-interface packet and event tags, plus the filler signature.
 *
 * @details
 * ``ESP_PRIV_PACKET_TYPE`` / ``ESP_PRIV_EVENT_TYPE`` come from
 * esp-hosted-mcu ``common/transport/esp_hosted_transport.h``. The C6
 * queues exactly one private event at boot -- an ``ESP_PRIV_EVENT_INIT``
 * carried on ::k_c6_if_priv -- and it stays queued until a host drains it,
 * which is what makes it the ideal first-light payload.
 *
 * ``get_next_tx_buffer()`` in the C6's peripheral-side SPI driver zeroes a
 * buffer and stamps ``if_type = ESP_MAX_IF`` plus ``if_num = 0xF`` for an
 * idle filler frame, leaving length, offset and checksum at zero.
 * Recognising that signature matters: an idle frame proves the wire, the
 * clock polarity and the C6's SPI peripheral are all live even after the
 * queued INIT event has been drained, and it is trivially distinguishable
 * from a dead bus reading all-zero or all-ones.
 *
 * @invariant ``k_c6_dummy_if_num`` fits in the header's four-bit field.
 *
 * @par Example:
 * @code
 * const bool idle = (h.if_type == (uint8_t)k_c6_if_max) &&
 *                   (h.if_num == (uint8_t)k_c6_dummy_if_num);
 * @endcode
 *
 * @see c6_if_type_t
 */
typedef enum : uint8_t {
  k_c6_priv_pkt_event  = 0x33U, /**< priv_pkt_type of an event frame.         */
  k_c6_priv_event_init = 0x22U, /**< event_type of the boot INIT event.       */
  k_c6_dummy_if_num    = 0x0FU, /**< if_num the C6 stamps into filler frames. */
} c6_proto_priv_t;

/**
 * @enum c6_bitop_t
 * @brief Shifts and masks used to unpack the header.
 *
 * @details Every one of these names a field boundary in
 * ``struct esp_payload_header``; none is a bare literal at a use site.
 *
 * @invariant Each shift is smaller than the width of the field it moves.
 *
 * @par Example:
 * @code
 * const uint8_t if_num = (uint8_t)((iface >> k_c6_shift_nibble) & k_c6_mask_nibble);
 * @endcode
 *
 * @see c6_hdr_offset_t
 */
typedef enum : uint8_t {
  k_c6_shift_nibble  = 4U,    /**< if_num sits in the upper nibble. */
  k_c6_mask_nibble   = 0x0FU, /**< Four-bit field mask.             */
  k_c6_shift_byte    = 8U,    /**< Little-endian high-byte shift.   */
  k_c6_mask_throttle = 0x03U, /**< throttle_cmd occupies bits 1:0.  */
} c6_bitop_t;

/* =============================================================================
 * 2. Frame decode, checksum and classification
 * =============================================================================
 */

/**
 * @enum c6_frame_kind_t
 * @brief Classification of one received 1600-byte transaction.
 *
 * @details
 * Mirrors the acceptance rules of ``process_spi_rx_buf()`` in
 * esp-hosted-mcu ``host/drivers/transport/spi/spi_drv.c``, with the C6's
 * idle filler frame recognised ahead of them so a quiet-but-alive link is
 * never mistaken for a dead one.
 *
 * @invariant Exactly one kind describes any received buffer.
 *
 * @par Example:
 * @code
 * if (c6_probe_classify(&hdr) == k_c6_frame_data) { ... }
 * @endcode
 *
 * @see c6_probe_classify
 */
typedef enum : uint8_t {
  k_c6_frame_garbage  = 0U, /**< No recognisable esp-hosted structure.   */
  k_c6_frame_idle     = 1U, /**< The C6's idle filler frame.             */
  k_c6_frame_data     = 2U, /**< Real frame, checksum verified.          */
  k_c6_frame_bad_csum = 3U, /**< Header shape sane, checksum mismatched. */
} c6_frame_kind_t;

/**
 * @struct c6_hdr_t
 * @brief Decoded esp-hosted payload header plus the locally recomputed sum.
 *
 * @invariant ``offset`` and ``len`` are taken verbatim from the wire and are
 *            judged by ::c6_probe_classify, not by the decoder.
 *
 * @par Example:
 * @code
 * c6_hdr_t h = {};
 * c6_probe_decode_header(rx, &h);
 * @endcode
 *
 * @see c6_probe_decode_header
 */
typedef struct {
  uint8_t  if_type;  /**< Interface identifier (::c6_if_type_t).       */
  uint8_t  if_num;   /**< Interface instance number.                   */
  uint8_t  flags;    /**< Fragment / wake / power-save bits.           */
  uint16_t len;      /**< Payload length claimed by the sender.        */
  uint16_t offset;   /**< Payload offset; always the header size.      */
  uint16_t checksum; /**< Checksum carried on the wire.                */
  uint16_t seq_num;  /**< Sender sequence number.                      */
  uint8_t  throttle; /**< Wi-Fi transmit throttle command.             */
  uint8_t  pkt_type; /**< HCI / private packet sub-type.               */
  uint16_t computed; /**< Checksum recomputed over the receive buffer. */
} c6_hdr_t;

/**
 * @brief Unpack the twelve-byte esp-hosted payload header.
 *
 * @param[in]  buf Receive buffer holding a completed transaction.
 * @param[out] out Decoded header; ignored when NULL.
 *
 * @pre ``buf`` holds at least ::k_c6_proto_hdr_size bytes.
 * @pre ``out`` is non-NULL for anything to be stored.
 * @post Every ``out`` field is populated from the wire bytes.
 * @post ``out->computed`` holds the locally recomputed checksum.
 *
 * @note Performs no validation; ::c6_probe_classify judges the result.
 *
 * @par Example:
 * @code
 * c6_hdr_t h = {};
 * c6_probe_decode_header(rx, &h);
 * @endcode
 *
 * @see c6_probe_classify
 * @since 0.1.0
 */
void c6_probe_decode_header(const uint8_t* buf, c6_hdr_t* out);

/**
 * @brief Recompute the esp-hosted checksum over a received frame.
 *
 * @details
 * ``compute_checksum()`` in esp-hosted-mcu
 * ``common/transport/esp_hosted_transport.h`` is a plain 16-bit sum of
 * every byte from the start of the header through the end of the payload.
 * Both peers zero the checksum field before summing
 * (``process_spi_rx_buf`` in ``host/drivers/transport/spi/spi_drv.c``), so
 * the two checksum bytes are skipped here instead of being cleared, which
 * leaves the receive buffer intact for the hex dump.
 *
 * @param[in] buf   Receive buffer.
 * @param[in] count Bytes to sum: header offset plus payload length.
 *
 * @return The 16-bit sum, or zero when ``buf`` is NULL.
 * @retval 0 ``buf`` was NULL, ``count`` was zero, or the sum is zero.
 *
 * @pre ``buf`` holds at least ``count`` bytes.
 * @pre ``count`` is no larger than ::k_c6_proto_buf_size.
 * @post The buffer is unmodified.
 * @post Overflow wraps at 16 bits, exactly as upstream does.
 *
 * @note Requires ``buf`` to be stable for the duration of the call.
 *
 * @par Example:
 * @code
 * const uint16_t sum = c6_probe_checksum(rx, (uint16_t)(h.offset + h.len));
 * @endcode
 *
 * @see c6_probe_decode_header
 * @since 0.1.0
 */
uint16_t c6_probe_checksum(const uint8_t* buf, uint16_t count);

/**
 * @brief Judge a decoded header against the upstream receive rules.
 *
 * @param[in] h Decoded header.
 *
 * @return The frame classification.
 * @retval k_c6_frame_idle     Idle filler frame from the C6.
 * @retval k_c6_frame_data     Real frame whose checksum verified.
 * @retval k_c6_frame_bad_csum Real-looking frame with a bad checksum.
 * @retval k_c6_frame_garbage  No esp-hosted structure at all, or NULL input.
 *
 * @pre ``h`` was produced by ::c6_probe_decode_header.
 * @pre ``h`` is non-NULL for a real verdict.
 * @post ``h`` is unmodified.
 * @post Exactly one classification is returned.
 *
 * @note Pure function; safe from any context.
 *
 * @par Example:
 * @code
 * const c6_frame_kind_t kind = c6_probe_classify(&h);
 * @endcode
 *
 * @see c6_frame_kind_t
 * @since 0.1.0
 */
c6_frame_kind_t c6_probe_classify(const c6_hdr_t* h);

/**
 * @brief Print the decoded header fields on one console line.
 *
 * @param[in] h Decoded header; ignored when NULL.
 *
 * @pre The board UART console has been initialised.
 * @pre ``h`` is non-NULL for anything to be printed.
 * @post Exactly one console line was emitted when ``h`` is non-NULL.
 * @post ``h`` is unmodified.
 *
 * @note Not thread-safe.
 *
 * @par Example:
 * @code
 * c6_probe_print_header(&h);
 * @endcode
 *
 * @see c6_probe_dump_payload
 * @since 0.1.0
 */
void c6_probe_print_header(const c6_hdr_t* h);

/**
 * @brief Hex-dump the leading payload bytes of a received frame.
 *
 * @param[in] buf Receive buffer.
 * @param[in] len Payload length claimed by the header.
 *
 * @pre The board UART console has been initialised.
 * @pre ``buf`` holds a completed transaction.
 * @post At most ::k_c6_probe_dump_bytes payload bytes were printed.
 * @post ``buf`` is unmodified.
 *
 * @note Not thread-safe.
 *
 * @par Example:
 * @code
 * c6_probe_dump_payload(rx, h.len);
 * @endcode
 *
 * @see c6_probe_print_header
 * @since 0.1.0
 */
void c6_probe_dump_payload(const uint8_t* buf, uint16_t len);
