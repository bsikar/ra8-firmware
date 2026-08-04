/**
 * @file ra8_c6link_internal.h
 * @brief Cross-translation-unit contract inside `libs/ra8_c6link`.
 * @ingroup grp_net
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details
 * Not public API. Only the six `ra8_c6link_*.c` translation units include this,
 * plus tests under `tests/` driving MC/DC vectors against helpers that no
 * public entry point can reach with the required argument combinations.
 * Application code must use `ra8_c6link.h` and `ra8_c6link_wifi.h`.
 *
 * This is also the only first-party header that names the vendored esp-hosted
 * types. Keeping the includes here is what lets the public headers stay free of
 * the esp-hosted include path.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_c6link.h"
#include "ra8_err.h"

/* Vendored esp-hosted: the packed payload header, the interface enumeration,
   the transport constants and the generated protobuf codec. Nothing else from
   the vendor tree is reachable from this library. */
#include "esp_hosted_header.h"
#include "esp_hosted_interface.h"
#include "esp_hosted_rpc.pb-c.h"
#include "esp_hosted_transport.h"
#include "protobuf-c/protobuf-c.h"

/**
 * @enum ra8_c6link_frame_class_t
 * @brief What one received transaction turned out to be.
 *
 * @details
 * The order the classifier applies these in is upstream's, from
 * `process_spi_rx_buf()`: zero length first (filler), then the header sanity
 * tests, then the checksum. Judging a filler frame by the rules for a data
 * frame is what once made a healthy link report a failure, so filler is its own
 * verdict and not a malformed data frame.
 *
 * @invariant Exactly one class is returned per transaction.
 * @invariant Only ::k_ra8_c6link_frame_data means the payload may be read.
 *
 * @par Example:
 * @code
 * if (ra8_c6link_priv_frame_classify(rx, &view) == k_ra8_c6link_frame_data) { ... }
 * @endcode
 *
 * @see ra8_c6link_priv_frame_classify
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_ra8_c6link_frame_data = 0U,
  /**< A well-formed frame whose checksum verified; the payload is real. */
  k_ra8_c6link_frame_idle = 1U,
  /**< The co-processor's filler frame: zero length, and legitimately
       `offset = 0`, which is not a defect. */
  k_ra8_c6link_frame_malformed = 2U,
  /**< The offset was not the header size, or the length did not fit. */
  k_ra8_c6link_frame_bad_checksum = 3U,
  /**< The recomputed checksum disagreed with the transmitted one. */
} ra8_c6link_frame_class_t;

/**
 * @struct ra8_c6link_rx_view
 * @brief Where the payload of a classified frame is, and what it claims to be.
 *
 * @details
 * Filled only when the classifier returns ::k_ra8_c6link_frame_data. Holds
 * offsets rather than a pointer so it cannot outlive the buffer it describes.
 *
 * @invariant `offset` equals ::k_ra8_c6link_header_bytes for any data frame.
 * @invariant `offset + len` never exceeds ::k_ra8_c6link_frame_bytes.
 *
 * @par Example:
 * @code
 * const uint8_t* payload = &rx[view.offset];
 * @endcode
 *
 * @see ra8_c6link_frame_class_t
 * @since 0.1.0
 */
typedef struct ra8_c6link_rx_view {
  uint16_t offset;  /**< Byte offset of the payload within the transaction. */
  uint16_t len;     /**< Payload length in bytes.                           */
  uint8_t  if_type; /**< Interface type from the header.                    */
  uint8_t  if_num;  /**< Interface number from the header.                  */
} ra8_c6link_rx_view_t;

/* ==========================================================================
 * ra8_c6link_arena.c -- the fixed decode arena behind the protobuf codec
 * ==========================================================================
 */

/**
 * @brief Take a block from the link's decode arena.
 *
 * @details
 * The `alloc` row of the ::ProtobufCAllocator handed to the generated codec.
 * Bumps a pointer through the caller-supplied arena, eight-byte aligned. There
 * is no fallback to a heap: this firmware has none, and an over-request must
 * fail the decode rather than fault.
 *
 * @param[in] ctx The ::ra8_c6link_t whose arena to draw from; must be non-null.
 * @param[in] size Bytes requested; zero yields a non-null zero-length block,
 *                 which is what protobuf-c expects.
 *
 * @return Pointer to the block, or null when the arena cannot serve it.
 * @retval NULL The arena is exhausted, or @p ctx was null.
 *
 * @pre The link is open, so its arena pointer and size are valid.
 * @pre The caller releases through ::ra8_c6link_priv_arena_free.
 * @post The bump offset advanced by the aligned size, or nothing changed.
 * @post `arena_last` names this block when the call succeeded.
 *
 * @note Not thread-safe; one link, one pump, one decode at a time.
 *
 * @par Example:
 * @code
 * ProtobufCAllocator a = { .alloc = ra8_c6link_priv_arena_alloc, ... };
 * @endcode
 *
 * @see ra8_c6link_priv_arena_reset
 * @since 0.1.0
 */
RA8_PRIV void* ra8_c6link_priv_arena_alloc(void* ctx, size_t size);

/**
 * @brief Return a block to the link's decode arena.
 *
 * @details
 * The `free` row of the allocator. A bump arena cannot free out of order, but
 * it can free the newest block: when @p pointer is the most recent allocation
 * the bump offset rolls back to it, which is what turns the codec's own
 * unwind-on-error path into genuinely reclaimed space rather than waste. Any
 * other pointer is retained until ::ra8_c6link_priv_arena_reset runs, which the
 * RPC layer does after every decode.
 *
 * @param[in] ctx The ::ra8_c6link_t whose arena owns the block; null is
 *                ignored.
 * @param[in] pointer Block to release; null is ignored.
 *
 * @return Nothing.
 *
 * @pre @p pointer came from ::ra8_c6link_priv_arena_alloc on the same link.
 * @pre No other reference to the block survives the call.
 * @post The bump offset is unchanged or rolled back to @p pointer.
 * @post No memory outside the arena is touched.
 *
 * @note Not thread-safe, for the same reason as the allocator.
 *
 * @par Example:
 * @code
 * ra8_c6link_priv_arena_free(link, block);
 * @endcode
 *
 * @see ra8_c6link_priv_arena_alloc
 * @since 0.1.0
 */
RA8_PRIV void ra8_c6link_priv_arena_free(void* ctx, void* pointer);

/**
 * @brief Empty the link's decode arena.
 *
 * @details
 * Called after every message is decoded and released, so each decode starts
 * from a known offset and no leak can accumulate across messages. That is what
 * bounds the arena requirement to one message rather than to a run.
 *
 * @param[in,out] link Link whose arena to empty; null is ignored.
 *
 * @return Nothing.
 *
 * @pre No block from the arena is still referenced.
 * @pre The link is open, or the call is a no-op.
 * @post The bump offset is zero.
 * @post The arena's bytes are left untouched, not scrubbed.
 *
 * @note Not thread-safe.
 *
 * @par Example:
 * @code
 * ra8_c6link_priv_arena_reset(link);
 * @endcode
 *
 * @see ra8_c6link_priv_arena_alloc
 * @since 0.1.0
 */
RA8_PRIV void ra8_c6link_priv_arena_reset(ra8_c6link_t* link);

/**
 * @brief Bind an allocator descriptor to a link's arena.
 *
 * @details
 * Fills the ::ProtobufCAllocator the generated codec is handed. Passing null to
 * the codec instead would select protobuf-c's default allocator, which calls
 * `malloc`; in this firmware `_sbrk` is a strong symbol that reports a fatal
 * error, so that path faults rather than failing.
 *
 * @param[out] out Descriptor to fill; must be non-null.
 * @param[in] link Link whose arena backs it; must be non-null.
 *
 * @return Nothing.
 *
 * @pre @p link is open.
 * @pre @p out outlives every decode it is passed to.
 * @post Both rows and the context of @p out are set.
 * @post No link state is modified.
 *
 * @note Safe from any context; it only assigns.
 *
 * @par Example:
 * @code
 * ProtobufCAllocator a;
 * ra8_c6link_priv_arena_bind(&a, link);
 * @endcode
 *
 * @see ra8_c6link_priv_arena_alloc
 * @since 0.1.0
 */
RA8_PRIV void ra8_c6link_priv_arena_bind(ProtobufCAllocator* out, ra8_c6link_t* link);

/* ==========================================================================
 * ra8_c6link_frame.c -- the twelve-byte payload header
 * ==========================================================================
 */

/**
 * @brief Stamp the transmit transaction as the host's idle filler.
 *
 * @details
 * Field for field what the reference host sends when it has nothing queued:
 * `if_type = ESP_MAX_IF` and every other byte zero, checksum included. The
 * co-processor needs a transaction to answer in, so an idle host still clocks
 * one.
 *
 * @param[out] tx Transmit transaction; must be non-null and
 *                ::k_ra8_c6link_frame_bytes long.
 *
 * @return Nothing.
 *
 * @pre No transfer is in flight on @p tx.
 * @pre @p tx covers a whole transaction.
 * @post Byte zero carries `ESP_MAX_IF`; every other byte is zero.
 * @post The frame is byte-identical to upstream's dummy buffer.
 *
 * @note The clearing loop is bounded by ::k_ra8_c6link_frame_bytes (Rule 2).
 *
 * @par Example:
 * @code
 * ra8_c6link_priv_frame_filler(link->tx);
 * @endcode
 *
 * @see ra8_c6link_priv_frame_seal
 * @since 0.1.0
 */
RA8_PRIV void ra8_c6link_priv_frame_filler(uint8_t* tx);

/**
 * @brief Wrap an already-staged payload in a payload header.
 *
 * @details
 * The payload is expected to be sitting at `tx + k_ra8_c6link_header_bytes`
 * already -- both the RPC encoder and the Ethernet transmit path write it there
 * directly, so nothing is copied twice. This fills the header in front of it,
 * zeroes the tail, and computes the checksum over header plus payload with the
 * checksum field taken as zero, exactly as `get_next_tx_buffer()` does upstream.
 *
 * @param[in,out] tx Transmit transaction; must be non-null.
 * @param[in] if_type Interface type for the frame, 0..15.
 * @param[in] if_num Interface number for the frame, 0..15.
 * @param[in] len Payload length already staged; at most
 *                ::k_ra8_c6link_max_payload.
 *
 * @return Nothing.
 *
 * @pre @p len bytes are staged at `tx + k_ra8_c6link_header_bytes`.
 * @pre @p len is within ::k_ra8_c6link_max_payload.
 * @post The header describes the payload and the checksum covers both.
 * @post Every byte past the payload is zero.
 *
 * @note The tail-clearing loop is bounded by ::k_ra8_c6link_frame_bytes.
 *
 * @par Example:
 * @code
 * ra8_c6link_priv_frame_seal(link->tx, (uint8_t)ESP_SERIAL_IF, 0U, n);
 * @endcode
 *
 * @see ra8_c6link_priv_frame_classify
 * @since 0.1.0
 */
RA8_PRIV void
ra8_c6link_priv_frame_seal(uint8_t* tx, uint8_t if_type, uint8_t if_num, uint16_t len);

/**
 * @brief Decide what a received transaction is, and where its payload lies.
 *
 * @details
 * Applies upstream's ordering, and verifies the checksum by zeroing the field
 * in place and restoring it, so the value a caller later reports is the one the
 * co-processor actually sent.
 *
 * @param[in,out] rx Received transaction; must be non-null. Momentarily
 *                   modified and restored.
 * @param[out] view Payload location and interface, filled only for a data
 *                  frame; must be non-null.
 *
 * @return ra8_c6link_frame_class_t The verdict.
 * @retval k_ra8_c6link_frame_data The payload is real and @p view describes it.
 * @retval k_ra8_c6link_frame_idle The co-processor had nothing to send.
 * @retval k_ra8_c6link_frame_malformed The header could not be believed.
 * @retval k_ra8_c6link_frame_bad_checksum The integrity check failed.
 *
 * @pre The transfer has completed and @p rx is stable.
 * @pre @p rx covers a whole transaction.
 * @post @p rx holds exactly the bytes it held on entry.
 * @post @p view is written only on ::k_ra8_c6link_frame_data.
 *
 * @note Not thread-safe against a concurrent read of @p rx.
 *
 * @par Example:
 * @code
 * ra8_c6link_rx_view_t view = {};
 * const ra8_c6link_frame_class_t cls = ra8_c6link_priv_frame_classify(rx, &view);
 * @endcode
 *
 * @see ra8_c6link_priv_frame_seal
 * @since 0.1.0
 *
 * @par MC/DC:
 * The malformed test is a two-condition decision and the tests drive N+1
 * vectors against it; see `tests/test_ra8_c6link.c`.
 */
[[nodiscard]] RA8_PRIV ra8_c6link_frame_class_t
ra8_c6link_priv_frame_classify(uint8_t* rx, ra8_c6link_rx_view_t* view);

/**
 * @enum ra8_c6link_caps_t
 * @brief The host-capabilities announcement, tag by tag.
 *
 * @details
 * Byte for byte the privileged frame upstream's `transport_drv.c` composes in
 * `send_slave_config()`: an `ESP_PRIV_EVENT_INIT` header followed by five  LEGACY-OK: send_slave_config() is the upstream esp-hosted symbol name
 * one-octet TLVs. Sending it is not optional politeness -- the co-processor
 * treats it as the host's arrival and (re)announces itself with
 * `Event_ESPInit` in reply, and until it has been sent this co-processor build
 * answers no RPC at all. That was measured: a facade that skipped it answered
 * once, on a co-processor another application had already announced to, and
 * then never again.
 *
 * The three policy values are the host's own and are restated here rather than
 * included, because the port headers that define them do not exist in the host
 * test build. They match `port_esp_hosted_host_config_features.h`.
 *
 * @invariant ::k_ra8_c6link_caps_bytes is the exact frame length, so a capacity
 *            check against it is neither loose nor tight.
 * @invariant ::k_ra8_c6link_caps_throttle_high is strictly above
 *            ::k_ra8_c6link_caps_throttle_low, which the co-processor requires.
 *
 * @par Example:
 * @code
 * out[k_ra8_c6link_caps_type] = (uint8_t)ESP_PRIV_EVENT_INIT;
 * @endcode
 *
 * @see ra8_c6link_priv_caps
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_ra8_c6link_caps_type      = 0U, /**< Offset of the event-type octet.     */
  k_ra8_c6link_caps_len       = 1U, /**< Offset of the event-length octet.   */
  k_ra8_c6link_caps_hdr       = 2U, /**< Octets before the first TLV.        */
  k_ra8_c6link_caps_tags      = 5U, /**< TLVs this host emits.               */
  k_ra8_c6link_caps_value_len = 1U, /**< Value width of every tag emitted.   */
  k_ra8_c6link_caps_stride    = 3U, /**< Octets one one-valued TLV occupies. */
  k_ra8_c6link_caps_bytes     = 17U,
  /**< Whole frame: two header octets plus five three-octet TLVs. */
  k_ra8_c6link_caps_host = 0U,
  /**< Host capability word. Zero, as upstream's own host sends. */
  k_ra8_c6link_caps_chip = 0x0DU,
  /**< `ESP_PRIV_FIRMWARE_CHIP_ESP32C6`: the part this board carries. */
  k_ra8_c6link_caps_raw_tp = 0U,
  /**< Raw-throughput test direction; disabled, matching `H_TEST_RAW_TP_DIR`. */
  k_ra8_c6link_caps_throttle_high = 80U,
  /**< Flow-control high-water mark, `H_WIFI_TX_DATA_THROTTLE_HIGH_THRESHOLD`. */
  k_ra8_c6link_caps_throttle_low = 60U,
  /**< Flow-control low-water mark, `H_WIFI_TX_DATA_THROTTLE_LOW_THRESHOLD`. */
} ra8_c6link_caps_t;

/**
 * @brief Build the host-capabilities announcement.
 *
 * @details
 * Pure formatting: no hardware, no link state. Split out so the exact octets
 * can be compared against the protocol in a host test rather than only against
 * the code that wrote them.
 *
 * @param[out] out Buffer to fill; must be non-null.
 * @param[in] cap Octets available at @p out.
 *
 * @return The frame length in octets, or zero when it would not fit.
 * @retval 0 @p out was null or @p cap is below ::k_ra8_c6link_caps_bytes.
 *
 * @pre @p cap octets are writable at @p out.
 * @pre The caller transmits the result on `ESP_PRIV_IF`, interface 0.
 * @post On success exactly ::k_ra8_c6link_caps_bytes octets were written.
 * @post On failure @p out is not modified.
 *
 * @note Safe from any context; it only writes constants.
 *
 * @par Example:
 * @code
 * const uint8_t n = ra8_c6link_priv_caps(&link->tx[k_ra8_c6link_header_bytes],
 *                                        (uint8_t)k_ra8_c6link_caps_bytes);
 * @endcode
 *
 * @see ra8_c6link_await_ready
 * @since 0.1.0
 */
[[nodiscard]] RA8_PRIV uint8_t ra8_c6link_priv_caps(uint8_t* out, uint8_t cap);

/* ==========================================================================
 * ra8_c6link_tlv.c -- the serial endpoint's two-tag envelope
 * ==========================================================================
 */

/**
 * @enum ra8_c6link_tlv_t
 * @brief The serial endpoint's TLV envelope, tag by tag.
 *
 * @details
 * Two tags, each a one-byte type followed by a little-endian 16-bit length: the
 * endpoint name, then the protobuf payload. The endpoint-name length is taken
 * from the vendored `RPC_EP_NAME_RSP` string rather than written down, because
 * upstream requires both endpoint names to be the same length and its parser
 * checks that.
 *
 * @invariant ::k_ra8_c6link_tlv_ep_len equals `strlen(RPC_EP_NAME_RSP)`.
 * @invariant ::k_ra8_c6link_tlv_overhead is the exact envelope cost, so a
 *            capacity check against it is neither loose nor tight.
 *
 * @par Example:
 * @code
 * out[k_ra8_c6link_tlv_type] = (uint8_t)k_ra8_c6link_tlv_t_epname;
 * @endcode
 *
 * @see ra8_c6link_priv_tlv_open
 * @since 0.1.0
 */
typedef enum : uint16_t {
  k_ra8_c6link_tlv_t_epname = 0x01U, /**< Tag introducing the endpoint name.    */
  k_ra8_c6link_tlv_t_data   = 0x02U, /**< Tag introducing the protobuf payload. */
  k_ra8_c6link_tlv_type     = 0U,    /**< Offset of a tag's type byte.          */
  k_ra8_c6link_tlv_len_lo   = 1U,    /**< Offset of a tag's length, low byte.   */
  k_ra8_c6link_tlv_len_hi   = 2U,    /**< Offset of a tag's length, high byte.  */
  k_ra8_c6link_tlv_value    = 3U,    /**< Offset of a tag's value.              */
  k_ra8_c6link_tlv_ep_len   = (uint16_t)(sizeof(RPC_EP_NAME_RSP) - 1U),
  /**< Endpoint-name length; both endpoint names share it. */
  k_ra8_c6link_tlv_overhead =
    (uint16_t)((uint16_t)k_ra8_c6link_tlv_value + (uint16_t)(sizeof(RPC_EP_NAME_RSP) - 1U) +
               (uint16_t)k_ra8_c6link_tlv_value),
  /**< Bytes the envelope costs on top of the protobuf payload. */
  k_ra8_c6link_tlv_shift = 8U,    /**< Shift between the two length bytes. */
  k_ra8_c6link_tlv_mask  = 0xFFU, /**< Byte mask for a length byte.        */
} ra8_c6link_tlv_t;

/**
 * @brief Write both envelope tags ahead of a protobuf payload.
 *
 * @param[out] out Buffer to fill; must be non-null.
 * @param[in] cap Bytes available at @p out.
 * @param[in] proto_len Protobuf length that will follow the envelope.
 * @param[out] body_at Offset the protobuf must be written at; must be non-null.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok The envelope is written and @p body_at names its payload.
 * @retval k_ra8_err_null_ptr @p out or @p body_at was null.
 * @retval k_ra8_err_invalid_size @p cap cannot hold envelope plus payload.
 *
 * @pre @p cap bytes are writable at @p out.
 * @pre @p proto_len is the exact packed size, not an estimate.
 * @post On success ::k_ra8_c6link_tlv_overhead bytes were written.
 * @post On failure @p out is not modified and @p body_at is zero.
 *
 * @note Pure formatting; touches no hardware.
 *
 * @par Example:
 * @code
 * uint16_t at = 0U;
 * (void)ra8_c6link_priv_tlv_open(buf, cap, packed, &at);
 * @endcode
 *
 * @see ra8_c6link_priv_tlv_body
 * @since 0.1.0
 */
[[nodiscard]] RA8_PRIV ra8_err_t ra8_c6link_priv_tlv_open(uint8_t*  out,
                                                          uint16_t  cap,
                                                          uint16_t  proto_len,
                                                          uint16_t* body_at);

/**
 * @brief Strip the envelope off a received serial payload.
 *
 * @details
 * Both endpoint names are accepted, as upstream's `parse_tlv()` does: a
 * response arrives on `RPCRsp` and an unsolicited event on `RPCEvt`, and the
 * two are the same length by construction.
 *
 * @param[in] payload Frame payload; must be non-null.
 * @param[in] len Payload length in bytes.
 * @param[out] proto_len Protobuf length found; must be non-null.
 *
 * @return Pointer to the protobuf bytes, or null when the envelope is not one
 *         this endpoint recognises.
 * @retval NULL The tags, the endpoint name or the lengths did not check out.
 *
 * @pre @p len bytes are readable at @p payload.
 * @pre @p proto_len is writable.
 * @post On success the returned range lies wholly inside @p payload.
 * @post On failure @p proto_len is zero.
 *
 * @note Pure parsing; touches no hardware.
 *
 * @par Example:
 * @code
 * uint16_t n = 0U;
 * const uint8_t* proto = ra8_c6link_priv_tlv_body(payload, len, &n);
 * @endcode
 *
 * @see ra8_c6link_priv_tlv_open
 * @since 0.1.0
 *
 * @par MC/DC:
 * The tag test is a three-condition decision and the tests drive N+1 vectors
 * against it; see `tests/test_ra8_c6link.c`.
 */
[[nodiscard]] RA8_PRIV const uint8_t*
ra8_c6link_priv_tlv_body(const uint8_t* payload, uint16_t len, uint16_t* proto_len);

/* ==========================================================================
 * ra8_c6link_rpc.c -- request, response, correlation
 * ==========================================================================
 */

/**
 * @struct ra8_c6link_take_ctx
 * @brief What an answer extractor is given alongside the decoded message.
 *
 * @details
 * Every extractor needs the link -- to record a co-processor result code in the
 * fault slot -- and most also need somewhere to put the fields they read. One
 * shared context type carries both, so the wait slot stays a single `void*` and
 * no extractor has to reach for a file-scope variable.
 *
 * @invariant `link` is the handle the request was issued on.
 * @invariant `out` is null exactly for requests whose answer carries no fields
 *            beyond its result code.
 *
 * @par Example:
 * @code
 * ra8_c6link_take_ctx_t take = { .link = link, .out = &version };
 * @endcode
 *
 * @see ra8_c6link_priv_rpc_call
 * @since 0.1.0
 */
typedef struct ra8_c6link_take_ctx {
  ra8_c6link_t* link;   /**< Handle the request was issued on.            */
  void*         out;    /**< Where the extracted fields go, or null.      */
  uint32_t      rpc_id; /**< `RPC_ID__Req_*` to record in the fault slot. */
} ra8_c6link_take_ctx_t;

/**
 * @brief Extract the result code from an answer that carries nothing else.
 *
 * @details
 * Most requests are answered by a message whose only field is `resp`. Which
 * arm of the payload union holds it depends on the message id, so this
 * switches on that rather than existing eight times over.
 *
 * @param[in] ctx A ::ra8_c6link_take_ctx_t naming the link and the request id.
 * @param[in] msg_v The decoded `Rpc`; must be non-null.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok The co-processor reported success.
 * @retval k_ra8_err_protocol_error It reported a failure, or the answer was
 *         one this extractor does not know how to read.
 *
 * @pre @p ctx names a live link.
 * @pre @p msg_v is still owned by the decoder.
 * @post The link's fault slot reflects exactly this answer.
 * @post No other link state is modified.
 *
 * @note Runs inside the pump, on the polling thread.
 *
 * @par Example:
 * @code
 * ra8_c6link_take_ctx_t take = { .link = link, .rpc_id = RPC_ID__Req_WifiStart };
 * @endcode
 *
 * @see ra8_c6link_priv_resp
 * @since 0.1.0
 */
[[nodiscard]] RA8_PRIV ra8_err_t ra8_c6link_priv_take_resp(void* ctx, const void* msg_v);

/**
 * @brief Issue a request whose body carries no fields and whose answer carries
 *        only a result code.
 *
 * @details
 * `Req_WifiStart`, `Req_WifiStop`, `Req_WifiDeinit`, `Req_WifiConnect` and
 * `Req_WifiDisconnect` are all this shape: an empty body, a result code back.
 * They differ only in which generated initialiser and which pair of ids they
 * name, so they share one implementation.
 *
 * @param[in,out] link Open handle; must be non-null.
 * @param[in] req_id `RPC_ID__Req_*` to send.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok The co-processor reported success.
 * @retval k_ra8_err_not_supported @p req_id is not one of the five.
 * @retval k_ra8_err_timeout No answer arrived within the budget.
 * @retval k_ra8_err_hw_timeout The co-processor never armed HANDSHAKE, so no
 *         transaction was clocked.
 * @retval k_ra8_err_protocol_error The answer reported a failure.
 * @retval k_ra8_err_spi_error The transport refused a transfer.
 *
 * @pre @p link is open.
 * @pre No other request is outstanding.
 * @post The link's fault slot reflects the outcome.
 * @post The wait slot is disarmed.
 *
 * @note Not thread-safe; it pumps.
 *
 * @par Example:
 * @code
 * return ra8_c6link_priv_bare_req(link, (uint32_t)RPC_ID__Req_WifiStart);
 * @endcode
 *
 * @see ra8_c6link_priv_rpc_call
 * @since 0.1.0
 */
[[nodiscard]] RA8_PRIV ra8_err_t ra8_c6link_priv_bare_req(ra8_c6link_t* link, uint32_t req_id);

/**
 * @brief Issue one request and pump until its answer arrives.
 *
 * @details
 * Assigns a fresh UID, packs @p req into the transmit transaction behind its
 * TLV envelope, arms the link's wait slot, and pumps. Announcements and
 * Ethernet frames that arrive meanwhile are dispatched normally; the pump stops
 * as soon as the answer is extracted.
 *
 * @param[in,out] link Open handle; must be non-null.
 * @param[in,out] req Request to send; must be non-null and fully populated
 *                    apart from its UID, which this call assigns.
 * @param[in] resp_id `RPC_ID__Resp_*` that answers @p req.
 * @param[in] take Extractor for the answer's fields; must be non-null.
 * @param[in] take_ctx Context handed to @p take.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok The answer arrived and @p take accepted it.
 * @retval k_ra8_err_null_ptr An argument was null.
 * @retval k_ra8_err_not_initialized @p link is not open.
 * @retval k_ra8_err_busy A request is already outstanding.
 * @retval k_ra8_err_invalid_size The packed request does not fit a frame.
 * @retval k_ra8_err_validation_failed The codec packed a different number of
 *         bytes than it predicted.
 * @retval k_ra8_err_timeout No answer arrived within the budget.
 * @retval k_ra8_err_hw_timeout The co-processor never armed HANDSHAKE, so no
 *         transaction was clocked.
 * @retval k_ra8_err_protocol_error @p take rejected the answer.
 * @retval k_ra8_err_spi_error The transport refused a transfer.
 *
 * @pre The transport is up.
 * @pre No other request is outstanding on @p link.
 * @post The wait slot is disarmed however the call ends.
 * @post On failure the link's last fault names @p req.
 *
 * @note Not thread-safe; it pumps.
 *
 * @par Example:
 * @code
 * (void)ra8_c6link_priv_rpc_call(link, &req, RPC_ID__Resp_WifiStart, take, &out);
 * @endcode
 *
 * @see ra8_c6link_priv_rpc_consume
 * @since 0.1.0
 */
[[nodiscard]] RA8_PRIV ra8_err_t ra8_c6link_priv_rpc_call(ra8_c6link_t*        link,
                                                          Rpc*                 req,
                                                          uint32_t             resp_id,
                                                          ra8_c6link_take_fn_t take,
                                                          void*                take_ctx);

/**
 * @brief Decode one control-plane payload and act on it.
 *
 * @details
 * Unwraps the envelope, decodes the message into the arena, then either
 * satisfies the outstanding wait, delivers an announcement, or drops it. The
 * arena is reset before returning however that goes.
 *
 * @param[in,out] link Open handle; must be non-null.
 * @param[in] payload Frame payload; must be non-null.
 * @param[in] len Payload length in bytes.
 *
 * @return true when the outstanding wait was satisfied and the pump should stop.
 * @retval true The awaited answer arrived and was extracted.
 * @retval false Anything else, including a delivered announcement.
 *
 * @pre @p len bytes are readable at @p payload.
 * @pre The link's arena is empty.
 * @post The arena is empty again.
 * @post At most one wait is satisfied per call.
 *
 * @note Not thread-safe; runs inside the pump.
 *
 * @par Example:
 * @code
 * if (ra8_c6link_priv_rpc_consume(link, &link->rx[view.offset], view.len)) { break; }
 * @endcode
 *
 * @see ra8_c6link_priv_rpc_call
 * @since 0.1.0
 */
[[nodiscard]] RA8_PRIV bool
ra8_c6link_priv_rpc_consume(ra8_c6link_t* link, const uint8_t* payload, uint16_t len);

/**
 * @brief Map a co-processor result code onto an ra8 error, recording it.
 *
 * @details
 * Every `Resp_*` message carries an `int32_t resp` that is an `esp_err_t` on
 * the far side. This turns a non-zero one into ::k_ra8_err_protocol_error and
 * records both it and the request id in the link's fault slot, so a bring-up
 * can say which request the co-processor refused and what it said.
 *
 * @param[in,out] link Open handle; must be non-null.
 * @param[in] rpc_id `RPC_ID__Req_*` the answer belongs to.
 * @param[in] resp The co-processor's result code.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok @p resp was zero and the fault slot was cleared.
 * @retval k_ra8_err_protocol_error @p resp was non-zero; the fault slot names
 *         @p rpc_id and @p resp.
 *
 * @pre @p link is open.
 * @pre @p resp came from a decoded answer, not from a default.
 * @post The fault slot reflects exactly this call.
 * @post No other link state is modified.
 *
 * @note Not thread-safe.
 *
 * @par Example:
 * @code
 * return ra8_c6link_priv_resp(link, RPC_ID__Req_WifiStart, body->resp);
 * @endcode
 *
 * @see ra8_c6link_last_fault
 * @since 0.1.0
 */
[[nodiscard]] RA8_PRIV ra8_err_t ra8_c6link_priv_resp(ra8_c6link_t* link,
                                                      uint32_t      rpc_id,
                                                      int32_t       resp);

/* ==========================================================================
 * ra8_c6link_pump.c -- the transaction loop
 * ==========================================================================
 */

/**
 * @brief Clock transactions until the budget or the wait says stop.
 *
 * @details
 * The implementation behind ::ra8_c6link_poll, and the loop every request runs
 * inside. Split out so the public entry point stays a validation wrapper and so
 * tests can drive it against a co-processor model directly.
 *
 * @param[in,out] link Open handle; must be non-null.
 * @param[in] max_transactions Transactions this call may clock; non-zero.
 * @param[out] stats Counters describing the run; must be non-null.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok The pump ran to its budget or until the wait was satisfied.
 * @retval k_ra8_err_hw_timeout HANDSHAKE never went active.
 * @retval k_ra8_err_spi_error The transport refused a transfer.
 *
 * @pre @p link is open and its transport is bound.
 * @pre @p stats has been cleared by the caller.
 * @post At most @p max_transactions transactions were clocked.
 * @post Any staged payload was transmitted, or is still staged.
 *
 * @note Not thread-safe; one pump at a time owns the transport.
 *
 * @par Example:
 * @code
 * (void)ra8_c6link_priv_pump(link, (uint16_t)k_ra8_c6link_rpc_transfers, &stats);
 * @endcode
 *
 * @see ra8_c6link_priv_dispatch
 * @since 0.1.0
 */
[[nodiscard]] RA8_PRIV ra8_err_t ra8_c6link_priv_pump(ra8_c6link_t*       link,
                                                      uint16_t            max_transactions,
                                                      ra8_c6link_stats_t* stats);

/**
 * @brief Route one well-formed received frame to whatever understands it.
 *
 * @details
 * Control-plane frames go to the RPC decoder, station and access-point frames
 * to the Ethernet receive callback, and everything else is counted. `ESP_PRIV_IF`
 * frames are counted rather than decoded: this co-processor build transmits its
 * only privileged frame with a checksum that does not match its own header
 * (#529), so a conformant host never sees a valid one.
 *
 * @param[in,out] link Open handle; must be non-null.
 * @param[in] view Payload location from the classifier; must be non-null.
 *
 * @return true when the outstanding wait was satisfied and the pump should stop.
 * @retval true The awaited answer arrived.
 * @retval false Keep clocking.
 *
 * @pre The frame classified as data, so @p view is populated.
 * @pre The link's receive transaction still holds the frame.
 * @post Exactly one consumer was offered the payload.
 * @post The running counters reflect where it went.
 *
 * @note Not thread-safe; runs inside the pump.
 *
 * @par Example:
 * @code
 * if (ra8_c6link_priv_dispatch(link, &view)) { break; }
 * @endcode
 *
 * @see ra8_c6link_priv_pump
 * @since 0.1.0
 */
[[nodiscard]] RA8_PRIV bool ra8_c6link_priv_dispatch(ra8_c6link_t*               link,
                                                     const ra8_c6link_rx_view_t* view);

/**
 * @brief Deliver one decoded announcement to the registered callback.
 *
 * @details
 * Lives in `ra8_c6link.c` beside the rest of the handle's state, and is called
 * from the RPC decoder once it has turned an `Event_*` message into a
 * first-party record.
 *
 * @param[in,out] link Open handle; must be non-null.
 * @param[in] ev Decoded announcement; must be non-null.
 *
 * @return Nothing.
 *
 * @pre @p ev is fully populated for its kind.
 * @pre The callback, if any, does not re-enter the link.
 * @post The announcement counter advanced.
 * @post `boot_seen` is set when @p ev is the boot announcement.
 *
 * @note Not thread-safe; runs inside the pump.
 *
 * @par Example:
 * @code
 * ra8_c6link_priv_emit(link, &ev);
 * @endcode
 *
 * @see ra8_c6link_event_cb_t
 * @since 0.1.0
 */
RA8_PRIV void ra8_c6link_priv_emit(ra8_c6link_t* link, const ra8_c6link_event_t* ev);

/**
 * @brief Copy a length-counted binary field into a NUL-terminated string.
 *
 * @details
 * Shared by the event decoder and the AP-record decoder. The co-processor
 * supplies the bytes, so nothing about them is trusted: the copy is bounded by
 * the destination and terminated whatever the source did.
 *
 * @param[out] dst Destination; must be non-null and @p cap bytes long.
 * @param[in] cap Bytes available at @p dst, including the terminator.
 * @param[in] src Binary field from a decoded message; null copies nothing.
 * @return The number of octets copied, excluding the terminator.
 * @retval 0 The field was absent, empty, or @p cap left no room.
 *
 * @pre @p cap is at least one, so a terminator always fits.
 * @pre @p dst does not overlap @p src.
 * @post @p dst is NUL-terminated.
 * @post At most `cap - 1` octets were copied.
 *
 * @note Pure copying; safe from any context.
 *
 * @par Example:
 * @code
 * ev.ssid_len = ra8_c6link_priv_copy_str(ev.ssid, sizeof ev.ssid, &body->ssid);
 * @endcode
 *
 * @see ra8_c6link_priv_copy_mac
 * @since 0.1.0
 */
RA8_PRIV uint8_t ra8_c6link_priv_copy_str(char* dst, uint8_t cap, const ProtobufCBinaryData* src);

/**
 * @brief Copy a binary field into a MAC address, all-or-nothing.
 *
 * @param[out] dst Address to fill; must be non-null.
 * @param[in] src Binary field from a decoded message; null clears @p dst.
 * @return true when the field held exactly ::k_ra8_c6link_mac_bytes octets.
 * @retval true @p dst holds the address.
 * @retval false The field was absent or the wrong length; @p dst is cleared.
 *
 * @pre @p dst does not overlap @p src.
 * @pre The caller treats a false return as a protocol failure, not a default.
 * @post @p dst is either fully written or fully cleared.
 * @post @p src is not modified.
 *
 * @note Pure copying; safe from any context.
 *
 * @par Example:
 * @code
 * (void)ra8_c6link_priv_copy_mac(&ev.bssid, &body->bssid);
 * @endcode
 *
 * @see ra8_c6link_priv_copy_str
 * @since 0.1.0
 */
[[nodiscard]] RA8_PRIV bool ra8_c6link_priv_copy_mac(ra8_c6link_mac_t*          dst,
                                                     const ProtobufCBinaryData* src);

#ifdef __cplusplus
}
#endif
