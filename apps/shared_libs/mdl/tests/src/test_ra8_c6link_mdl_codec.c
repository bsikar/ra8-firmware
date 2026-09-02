/**
 * @file test_ra8_c6link_mdl_codec.c
 * @brief Round-trip qualification of the generated media-download protobuf
 * codec.
 * @details Drives every generated message through both published packers and
 * the unpacker: each message is populated so no field carries its proto3
 * default, packed in place, packed again through a ::ProtobufCBufferSimple,
 * proven byte-identical, decoded, compared field by field, and released. The
 * null-message early-out of every generated release helper is executed too.
 * These are the descriptor tables the C6 link depends on, so a mis-declared
 * field number, wire type, or offset fails here rather than on the wire.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "ra8_attributes.h"
#include "ra8_mdl_protocol.h"
#include "ra8_media_download.pb-c.h"
#include "unity_minimal.h"

/** @brief Narrow field values pinned by the generated-codec round trips. */
typedef enum : uint32_t {
  k_t_pb_scratch_bytes   = 512U,        /**< Encoder scratch capacity.    */
  k_t_pb_job_id          = 0xC0FFEE01U, /**< Non-default job identifier.  */
  k_t_pb_timeout_ms      = 4321U,       /**< Non-default request timeout. */
  k_t_pb_max_chunk_bytes = 1400U,       /**< Non-default chunk ceiling.   */
  k_t_pb_sequence        = 9U,          /**< Non-default chunk sequence.  */
  k_t_pb_max_bytes       = 777U,        /**< Non-default pull ceiling.    */
  k_t_pb_body_bytes      = 12U,         /**< Chunk body byte count.       */
  k_t_pb_body_seed       = 0x5AU,       /**< First chunk-body octet.      */
  k_t_pb_digest_octet    = 0x3CU,       /**< Round-tripped digest octet.  */
} t_pb_value_t;

/** @brief Wide field values pinned by the generated-codec round trips. */
typedef enum : uint64_t {
  k_t_pb_offset      = 0x00000001FFFFFFFEULL, /**< Non-default 64-bit offset. */
  k_t_pb_total_bytes = 0x0000000300000005ULL, /**< Non-default total length.  */
} t_pb_wide_t;

/** @brief Signed field values pinned by the generated-codec round trips. */
typedef enum : int32_t {
  k_t_pb_status        = -9,  /**< Negative transfer status.     */
  k_t_pb_http_status   = 206, /**< Partial-content HTTP status.  */
  k_t_pb_cancel_status = -3,  /**< Negative cancellation status. */
} t_pb_signed_t;

/** @brief Canonical artifact URL used by the StartRequest round trip. */
static const char s_pb_url[] = "https://example.test/book";
/** @brief Canonical User-Agent used by the StartRequest round trip. */
static const char s_pb_user_agent[] = "ra8-test/3";
/** @brief Canonical Referer used by the StartRequest round trip. */
static const char s_pb_referer[] = "https://example.test/catalog";
/** @brief Canonical ETag condition used by the StartRequest round trip. */
static const char s_pb_if_none_match[] = "\"cached-etag\"";
/** @brief Canonical date condition used by the StartRequest round trip. */
static const char s_pb_if_modified_since[] = "Tue, 20 Oct 2015 07:28:00 GMT";
/** @brief Canonical Retry-After used by the Chunk round trip. */
static const char s_pb_retry_after[] = "3";
/** @brief Canonical ETag used by the Chunk round trip. */
static const char s_pb_etag[] = "\"fixture-etag\"";
/** @brief Canonical Last-Modified used by the Chunk round trip. */
static const char s_pb_last_modified[] = "Wed, 21 Oct 2015 07:28:00 GMT";
/** @brief Canonical Content-Type used by the Chunk round trip. */
static const char s_pb_content_type[] = "application/x-rabook";

/** @brief Bytes produced by the plain in-place packer. */
static uint8_t s_pb_packed[k_t_pb_scratch_bytes];
/** @brief Scratch storage backing the simple append buffer. */
static uint8_t s_pb_pad[k_t_pb_scratch_bytes];
/** @brief Non-constant chunk body carried through the codec. */
static uint8_t s_pb_body[k_t_pb_body_bytes];
/** @brief Terminal digest carried through the codec. */
static uint8_t s_pb_digest[k_ra8_mdl_sha256_bytes];

/**
 * @brief Fill the binary fixtures the Chunk round trip transports.
 * @details Uses a varying body sequence so a truncated or shifted `bytes`
 * field cannot compare equal, and a fixed digest octet for the fixed-extent
 * SHA-256 field.
 * @pre The fixed-capacity body and digest fixtures are addressable.
 * @pre No other test is reading the binary fixtures.
 * @post Every body byte differs from its neighbours.
 * @post Every digest byte equals the pinned octet.
 * @note File-local helper; no ownership escapes this focused test executable.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_pb_fill_binary(void)
{
  for (uint32_t i = 0U; i < (uint32_t)sizeof(s_pb_body); ++i) {
    s_pb_body[i] = (uint8_t)(k_t_pb_body_seed + i);
  }
  (void)memset(s_pb_digest, (int)k_t_pb_digest_octet, sizeof(s_pb_digest));
}

/**
 * @brief Assert both generated packers produced one identical encoding.
 * @details Compares the returned lengths, the length the simple buffer
 * accumulated, and every encoded byte, so a descriptor walked differently by
 * the buffer path cannot pass.
 * @param[in] packed_len Length returned by the plain in-place packer.
 * @param[in] buffer_len Length returned by the buffer packer.
 * @param[in] simple Simple append buffer holding the second encoding.
 * @pre @p simple received exactly one complete message.
 * @pre ::s_pb_packed holds the first encoding of the same message.
 * @post Both encodings are proven byte-identical and non-empty.
 * @post Neither encoding is modified.
 * @note File-local helper; no ownership escapes this focused test executable.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_pb_expect_identical(size_t                       packed_len,
                                                      size_t                       buffer_len,
                                                      const ProtobufCBufferSimple* simple)
{
  TEST_ASSERT(packed_len != 0U);
  TEST_ASSERT(packed_len <= sizeof(s_pb_packed));
  TEST_ASSERT_EQ(packed_len, buffer_len);
  TEST_ASSERT_EQ(packed_len, simple->len);
  TEST_ASSERT(memcmp(s_pb_packed, simple->data, packed_len) == 0);
}

/**
 * @test internal_test_pb_start_request_round_trip
 * @brief Prove the StartRequest descriptor packs, buffers, and decodes exactly.
 * @details Populates every field with a non-default value so proto3 implicit
 * presence cannot hide a descriptor defect, then requires the two packers to
 * agree byte for byte and the decode to restore each value.
 * @par MC/DC:
 * Decision `if (!message)` in `ra8__mdl__start_request__free_unpacked` (N=1):
 * the decoded message gives false and releases storage; the explicit null call
 * gives true and returns without dereferencing. Both outcomes executed.
 * @pre The encoder scratch fixtures are addressable.
 * @pre The default protobuf-c allocator is available on the host.
 * @post Every field decodes to the exact value that was encoded.
 * @post All decoder storage is released.
 * @note Host-only test; the firmware never allocates through this path.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_test_pb_start_request_round_trip(void)
{
  TEST_BEGIN("mdl codec StartRequest round trip");
  Ra8__Mdl__StartRequest message = RA8__MDL__START_REQUEST__INIT;
  message.protocol_version       = k_ra8_mdl_protocol_version;
  message.url                    = (char*)s_pb_url;
  message.format                 = RA8__MDL__FORMAT__FORMAT_RABOOK;
  message.user_agent             = (char*)s_pb_user_agent;
  message.referer                = (char*)s_pb_referer;
  message.if_none_match          = (char*)s_pb_if_none_match;
  message.if_modified_since      = (char*)s_pb_if_modified_since;
  message.timeout_ms             = k_t_pb_timeout_ms;
  const size_t packed_len        = ra8__mdl__start_request__pack(&message, s_pb_packed);
  TEST_ASSERT_EQ(ra8__mdl__start_request__get_packed_size(&message), packed_len);
  ProtobufCBufferSimple simple = PROTOBUF_C_BUFFER_SIMPLE_INIT(s_pb_pad);
  const size_t buffer_len      = ra8__mdl__start_request__pack_to_buffer(&message, &simple.base);
  internal_pb_expect_identical(packed_len, buffer_len, &simple);
  Ra8__Mdl__StartRequest* decoded =
    ra8__mdl__start_request__unpack(nullptr, packed_len, s_pb_packed);
  TEST_ASSERT(decoded != nullptr);
  TEST_ASSERT_EQ(k_ra8_mdl_protocol_version, decoded->protocol_version);
  TEST_ASSERT(strcmp(decoded->url, s_pb_url) == 0);
  TEST_ASSERT_EQ(RA8__MDL__FORMAT__FORMAT_RABOOK, decoded->format);
  TEST_ASSERT(strcmp(decoded->user_agent, s_pb_user_agent) == 0);
  TEST_ASSERT(strcmp(decoded->referer, s_pb_referer) == 0);
  TEST_ASSERT(strcmp(decoded->if_none_match, s_pb_if_none_match) == 0);
  TEST_ASSERT(strcmp(decoded->if_modified_since, s_pb_if_modified_since) == 0);
  TEST_ASSERT_EQ(k_t_pb_timeout_ms, decoded->timeout_ms);
  ra8__mdl__start_request__free_unpacked(decoded, nullptr);
  ra8__mdl__start_request__free_unpacked(nullptr, nullptr);
  PROTOBUF_C_BUFFER_SIMPLE_CLEAR(&simple);
  TEST_END("mdl codec StartRequest round trip");
}

/**
 * @test internal_test_pb_accepted_round_trip
 * @brief Prove the Accepted descriptor packs, buffers, and decodes exactly.
 * @details Sets a non-default job identifier, chunk ceiling, and typed format
 * so every declared field participates in the encoding.
 * @par MC/DC:
 * Decision `if (!message)` in `ra8__mdl__accepted__free_unpacked` (N=1): the
 * decoded message gives false, the explicit null call gives true.
 * @pre The encoder scratch fixtures are addressable.
 * @pre The default protobuf-c allocator is available on the host.
 * @post Every field decodes to the exact value that was encoded.
 * @post All decoder storage is released.
 * @note Host-only test; the firmware never allocates through this path.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_test_pb_accepted_round_trip(void)
{
  TEST_BEGIN("mdl codec Accepted round trip");
  Ra8__Mdl__Accepted message = RA8__MDL__ACCEPTED__INIT;
  message.protocol_version   = k_ra8_mdl_protocol_version;
  message.job_id             = k_t_pb_job_id;
  message.max_chunk_bytes    = k_t_pb_max_chunk_bytes;
  message.format             = RA8__MDL__FORMAT__FORMAT_EPUB;
  const size_t packed_len    = ra8__mdl__accepted__pack(&message, s_pb_packed);
  TEST_ASSERT_EQ(ra8__mdl__accepted__get_packed_size(&message), packed_len);
  ProtobufCBufferSimple simple     = PROTOBUF_C_BUFFER_SIMPLE_INIT(s_pb_pad);
  const size_t          buffer_len = ra8__mdl__accepted__pack_to_buffer(&message, &simple.base);
  internal_pb_expect_identical(packed_len, buffer_len, &simple);
  Ra8__Mdl__Accepted* decoded = ra8__mdl__accepted__unpack(nullptr, packed_len, s_pb_packed);
  TEST_ASSERT(decoded != nullptr);
  TEST_ASSERT_EQ(k_ra8_mdl_protocol_version, decoded->protocol_version);
  TEST_ASSERT_EQ(k_t_pb_job_id, decoded->job_id);
  TEST_ASSERT_EQ(k_t_pb_max_chunk_bytes, decoded->max_chunk_bytes);
  TEST_ASSERT_EQ(RA8__MDL__FORMAT__FORMAT_EPUB, decoded->format);
  ra8__mdl__accepted__free_unpacked(decoded, nullptr);
  ra8__mdl__accepted__free_unpacked(nullptr, nullptr);
  PROTOBUF_C_BUFFER_SIMPLE_CLEAR(&simple);
  TEST_END("mdl codec Accepted round trip");
}

/**
 * @test internal_test_pb_next_request_round_trip
 * @brief Prove the NextRequest descriptor packs, buffers, and decodes exactly.
 * @details Carries a 64-bit acknowledged offset above the 32-bit range so a
 * mis-declared field width in the descriptor table cannot round-trip.
 * @par MC/DC:
 * Decision `if (!message)` in `ra8__mdl__next_request__free_unpacked` (N=1):
 * the decoded message gives false, the explicit null call gives true.
 * @pre The encoder scratch fixtures are addressable.
 * @pre The default protobuf-c allocator is available on the host.
 * @post Every field decodes to the exact value that was encoded.
 * @post All decoder storage is released.
 * @note Host-only test; the firmware never allocates through this path.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_test_pb_next_request_round_trip(void)
{
  TEST_BEGIN("mdl codec NextRequest round trip");
  Ra8__Mdl__NextRequest message = RA8__MDL__NEXT_REQUEST__INIT;
  message.protocol_version      = k_ra8_mdl_protocol_version;
  message.job_id                = k_t_pb_job_id;
  message.acknowledged_offset   = k_t_pb_offset;
  message.max_bytes             = k_t_pb_max_bytes;
  const size_t packed_len       = ra8__mdl__next_request__pack(&message, s_pb_packed);
  TEST_ASSERT_EQ(ra8__mdl__next_request__get_packed_size(&message), packed_len);
  ProtobufCBufferSimple simple     = PROTOBUF_C_BUFFER_SIMPLE_INIT(s_pb_pad);
  const size_t          buffer_len = ra8__mdl__next_request__pack_to_buffer(&message, &simple.base);
  internal_pb_expect_identical(packed_len, buffer_len, &simple);
  Ra8__Mdl__NextRequest* decoded = ra8__mdl__next_request__unpack(nullptr, packed_len, s_pb_packed);
  TEST_ASSERT(decoded != nullptr);
  TEST_ASSERT_EQ(k_ra8_mdl_protocol_version, decoded->protocol_version);
  TEST_ASSERT_EQ(k_t_pb_job_id, decoded->job_id);
  TEST_ASSERT(decoded->acknowledged_offset == (uint64_t)k_t_pb_offset);
  TEST_ASSERT_EQ(k_t_pb_max_bytes, decoded->max_bytes);
  ra8__mdl__next_request__free_unpacked(decoded, nullptr);
  ra8__mdl__next_request__free_unpacked(nullptr, nullptr);
  PROTOBUF_C_BUFFER_SIMPLE_CLEAR(&simple);
  TEST_END("mdl codec NextRequest round trip");
}

/**
 * @brief Populate every declared Chunk field with a non-default value.
 * @details Covers the widest generated descriptor: two `bytes` fields, a
 * 64-bit offset and total, a typed state, two signed status fields, and four
 * strings.
 * @param[out] message Chunk initialized with ::RA8__MDL__CHUNK__INIT.
 * @pre The binary fixtures were filled by ::internal_pb_fill_binary.
 * @pre @p message is non-null and freshly initialized.
 * @post Every declared field differs from its proto3 default.
 * @post The message borrows, and never copies, the binary fixtures.
 * @note File-local helper; no ownership escapes this focused test executable.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_pb_fill_chunk(Ra8__Mdl__Chunk* message)
{
  message->protocol_version = k_ra8_mdl_protocol_version;
  message->job_id           = k_t_pb_job_id;
  message->sequence         = k_t_pb_sequence;
  message->offset           = k_t_pb_offset;
  message->data.len         = sizeof(s_pb_body);
  message->data.data        = s_pb_body;
  message->total_bytes      = k_t_pb_total_bytes;
  message->state            = RA8__MDL__STATE__STATE_COMPLETE;
  message->status           = k_t_pb_status;
  message->sha256.len       = sizeof(s_pb_digest);
  message->sha256.data      = s_pb_digest;
  message->http_status      = k_t_pb_http_status;
  message->retry_after      = (char*)s_pb_retry_after;
  message->etag             = (char*)s_pb_etag;
  message->last_modified    = (char*)s_pb_last_modified;
  message->content_type     = (char*)s_pb_content_type;
}

/**
 * @brief Assert a decoded Chunk restored every populated field exactly.
 * @details Compares both binary payloads byte for byte, so a descriptor that
 * confused the two `bytes` offsets cannot pass on length alone.
 * @param[in] decoded Chunk produced by the generated unpacker.
 * @pre @p decoded came from an encoding of ::internal_pb_fill_chunk output.
 * @pre The binary fixtures still hold the encoded values.
 * @post Every declared field is proven equal to the encoded value.
 * @post No decoder storage is released here.
 * @note File-local helper; no ownership escapes this focused test executable.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_pb_expect_chunk(const Ra8__Mdl__Chunk* decoded)
{
  TEST_ASSERT_EQ(k_ra8_mdl_protocol_version, decoded->protocol_version);
  TEST_ASSERT_EQ(k_t_pb_job_id, decoded->job_id);
  TEST_ASSERT_EQ(k_t_pb_sequence, decoded->sequence);
  TEST_ASSERT(decoded->offset == (uint64_t)k_t_pb_offset);
  TEST_ASSERT_EQ(sizeof(s_pb_body), decoded->data.len);
  TEST_ASSERT(memcmp(decoded->data.data, s_pb_body, sizeof(s_pb_body)) == 0);
  TEST_ASSERT(decoded->total_bytes == (uint64_t)k_t_pb_total_bytes);
  TEST_ASSERT_EQ(RA8__MDL__STATE__STATE_COMPLETE, decoded->state);
  TEST_ASSERT_EQ(k_t_pb_status, decoded->status);
  TEST_ASSERT_EQ(sizeof(s_pb_digest), decoded->sha256.len);
  TEST_ASSERT(memcmp(decoded->sha256.data, s_pb_digest, sizeof(s_pb_digest)) == 0);
  TEST_ASSERT_EQ(k_t_pb_http_status, decoded->http_status);
  TEST_ASSERT(strcmp(decoded->retry_after, s_pb_retry_after) == 0);
  TEST_ASSERT(strcmp(decoded->etag, s_pb_etag) == 0);
  TEST_ASSERT(strcmp(decoded->last_modified, s_pb_last_modified) == 0);
  TEST_ASSERT(strcmp(decoded->content_type, s_pb_content_type) == 0);
}

/**
 * @test internal_test_pb_chunk_round_trip
 * @brief Prove the Chunk descriptor packs, buffers, and decodes exactly.
 * @details The widest generated message, carrying both binary fields, both
 * signed fields, and every string, through both packers and the unpacker.
 * @par MC/DC:
 * Decision `if (!message)` in `ra8__mdl__chunk__free_unpacked` (N=1): the
 * decoded message gives false, the explicit null call gives true.
 * @pre The encoder scratch fixtures are addressable.
 * @pre The default protobuf-c allocator is available on the host.
 * @post Every field decodes to the exact value that was encoded.
 * @post All decoder storage is released.
 * @note Host-only test; the firmware never allocates through this path.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_test_pb_chunk_round_trip(void)
{
  TEST_BEGIN("mdl codec Chunk round trip");
  internal_pb_fill_binary();
  Ra8__Mdl__Chunk message = RA8__MDL__CHUNK__INIT;
  internal_pb_fill_chunk(&message);
  const size_t packed_len = ra8__mdl__chunk__pack(&message, s_pb_packed);
  TEST_ASSERT_EQ(ra8__mdl__chunk__get_packed_size(&message), packed_len);
  ProtobufCBufferSimple simple     = PROTOBUF_C_BUFFER_SIMPLE_INIT(s_pb_pad);
  const size_t          buffer_len = ra8__mdl__chunk__pack_to_buffer(&message, &simple.base);
  internal_pb_expect_identical(packed_len, buffer_len, &simple);
  Ra8__Mdl__Chunk* decoded = ra8__mdl__chunk__unpack(nullptr, packed_len, s_pb_packed);
  TEST_ASSERT(decoded != nullptr);
  internal_pb_expect_chunk(decoded);
  ra8__mdl__chunk__free_unpacked(decoded, nullptr);
  ra8__mdl__chunk__free_unpacked(nullptr, nullptr);
  PROTOBUF_C_BUFFER_SIMPLE_CLEAR(&simple);
  TEST_END("mdl codec Chunk round trip");
}

/**
 * @test internal_test_pb_cancel_request_round_trip
 * @brief Prove the CancelRequest descriptor packs, buffers, and decodes.
 * @details The narrowest request message; both declared fields are set to
 * non-default values so neither is skipped by implicit presence.
 * @par MC/DC:
 * Decision `if (!message)` in `ra8__mdl__cancel_request__free_unpacked` (N=1):
 * the decoded message gives false, the explicit null call gives true.
 * @pre The encoder scratch fixtures are addressable.
 * @pre The default protobuf-c allocator is available on the host.
 * @post Every field decodes to the exact value that was encoded.
 * @post All decoder storage is released.
 * @note Host-only test; the firmware never allocates through this path.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_test_pb_cancel_request_round_trip(void)
{
  TEST_BEGIN("mdl codec CancelRequest round trip");
  Ra8__Mdl__CancelRequest message = RA8__MDL__CANCEL_REQUEST__INIT;
  message.protocol_version        = k_ra8_mdl_protocol_version;
  message.job_id                  = k_t_pb_job_id;
  const size_t packed_len         = ra8__mdl__cancel_request__pack(&message, s_pb_packed);
  TEST_ASSERT_EQ(ra8__mdl__cancel_request__get_packed_size(&message), packed_len);
  ProtobufCBufferSimple simple = PROTOBUF_C_BUFFER_SIMPLE_INIT(s_pb_pad);
  const size_t buffer_len      = ra8__mdl__cancel_request__pack_to_buffer(&message, &simple.base);
  internal_pb_expect_identical(packed_len, buffer_len, &simple);
  Ra8__Mdl__CancelRequest* decoded =
    ra8__mdl__cancel_request__unpack(nullptr, packed_len, s_pb_packed);
  TEST_ASSERT(decoded != nullptr);
  TEST_ASSERT_EQ(k_ra8_mdl_protocol_version, decoded->protocol_version);
  TEST_ASSERT_EQ(k_t_pb_job_id, decoded->job_id);
  ra8__mdl__cancel_request__free_unpacked(decoded, nullptr);
  ra8__mdl__cancel_request__free_unpacked(nullptr, nullptr);
  PROTOBUF_C_BUFFER_SIMPLE_CLEAR(&simple);
  TEST_END("mdl codec CancelRequest round trip");
}

/**
 * @test internal_test_pb_cancelled_round_trip
 * @brief Prove the Cancelled descriptor packs, buffers, and decodes exactly.
 * @details Uses a negative status so the signed varint encoding, not just the
 * field ordering, is proven by the round trip.
 * @par MC/DC:
 * Decision `if (!message)` in `ra8__mdl__cancelled__free_unpacked` (N=1): the
 * decoded message gives false, the explicit null call gives true.
 * @pre The encoder scratch fixtures are addressable.
 * @pre The default protobuf-c allocator is available on the host.
 * @post Every field decodes to the exact value that was encoded.
 * @post All decoder storage is released.
 * @note Host-only test; the firmware never allocates through this path.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_test_pb_cancelled_round_trip(void)
{
  TEST_BEGIN("mdl codec Cancelled round trip");
  Ra8__Mdl__Cancelled message = RA8__MDL__CANCELLED__INIT;
  message.protocol_version    = k_ra8_mdl_protocol_version;
  message.job_id              = k_t_pb_job_id;
  message.status              = k_t_pb_cancel_status;
  const size_t packed_len     = ra8__mdl__cancelled__pack(&message, s_pb_packed);
  TEST_ASSERT_EQ(ra8__mdl__cancelled__get_packed_size(&message), packed_len);
  ProtobufCBufferSimple simple     = PROTOBUF_C_BUFFER_SIMPLE_INIT(s_pb_pad);
  const size_t          buffer_len = ra8__mdl__cancelled__pack_to_buffer(&message, &simple.base);
  internal_pb_expect_identical(packed_len, buffer_len, &simple);
  Ra8__Mdl__Cancelled* decoded = ra8__mdl__cancelled__unpack(nullptr, packed_len, s_pb_packed);
  TEST_ASSERT(decoded != nullptr);
  TEST_ASSERT_EQ(k_ra8_mdl_protocol_version, decoded->protocol_version);
  TEST_ASSERT_EQ(k_t_pb_job_id, decoded->job_id);
  TEST_ASSERT_EQ(k_t_pb_cancel_status, decoded->status);
  ra8__mdl__cancelled__free_unpacked(decoded, nullptr);
  ra8__mdl__cancelled__free_unpacked(nullptr, nullptr);
  PROTOBUF_C_BUFFER_SIMPLE_CLEAR(&simple);
  TEST_END("mdl codec Cancelled round trip");
}

int main(void)
{
  internal_test_pb_start_request_round_trip();
  internal_test_pb_accepted_round_trip();
  internal_test_pb_next_request_round_trip();
  internal_test_pb_chunk_round_trip();
  internal_test_pb_cancel_request_round_trip();
  internal_test_pb_cancelled_round_trip();
  return 0;
}
