/**
 * @file ra8_c6_model_mdl_fault.c
 * @brief Malformed generated-media responses for the C6 model.
 * @details Decodes, mutates, and repacks response shapes used to qualify the
 * client decoder without adding protocol-special cases to the transport model.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <string.h>

#include "ra8_c6_model_mdl_fault_internal.h"
#include "ra8_c6link_mdl_msg.h"
#include "ra8_mdl_protocol.h"
#include "ra8_media_download.pb-c.h"
#include "unity_minimal.h"

/** @brief Unknown protobuf field 15 carrying canonical varint one. */
static const uint8_t s_mdl_unknown_field[] = {0x78U, 0x01U};

/**
 * @brief Clear COMPLETE-only HTTP metadata before changing terminal state
 * @details Restores the nonterminal wire invariant so each injected state
 * mutation tests only its intended protocol fault.
 * @param[in,out] chunk Decoded generated response.
 * @pre @p chunk is non-null and caller-owned.
 * @pre String fields still reference decoder-owned storage.
 * @post HTTP status is zero and selected headers are empty.
 * @post Binary payload and decoder ownership are unchanged.
 * @note Keeps each injected state fault isolated from metadata validation.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_clear_http_metadata(Ra8__Mdl__Chunk* chunk)
{
  chunk->http_status   = 0;
  chunk->retry_after   = (char*)"";
  chunk->etag          = (char*)"";
  chunk->last_modified = (char*)"";
  chunk->content_type  = (char*)"";
}

/**
 * @brief Mutate the state, payload or correlation of one decoded Chunk for
 * the terminal-state fault family.
 * @details Rewrites only the fields the selected terminal-state fault needs,
 * preserving the generated decoder's owned byte spans.
 * @param[in,out] chunk Decoded generated chunk to mutate in place.
 * @param[in] fault Candidate chunk mutation.
 * @return Whether @p fault named a terminal-state mutation.
 * @retval true The selected mutation was applied.
 * @retval false @p fault does not belong to the terminal-state family.
 * @pre @p chunk is non-null and caller-owned.
 * @pre String fields still reference decoder-owned storage.
 * @post A recognised fault malforms exactly the fields it names.
 * @post An unrecognised fault leaves @p chunk untouched.
 * @note Shares ::s_bad_data storage across calls; test exchanges are
 * serialized, so the reused backing byte is safe.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_mutate_chunk_state_terminal(Ra8__Mdl__Chunk*         chunk,
                                                              ra8_c6_model_mdl_fault_t fault)
{
  static uint8_t s_bad_data = k_c6m_mdl_digest_fill;
  switch (fault) {
    case k_c6m_mdl_fault_complete_no_sha:
      chunk->sha256 = (ProtobufCBinaryData){};
      break;
    case k_c6m_mdl_fault_complete_bad_total:
      chunk->total_bytes = chunk->offset + chunk->data.len + 1U;
      break;
    case k_c6m_mdl_fault_failed:
      internal_clear_http_metadata(chunk);
      chunk->state  = RA8__MDL__STATE__STATE_FAILED;
      chunk->status = (int32_t)k_ra8_fail;
      chunk->data   = (ProtobufCBinaryData){};
      chunk->sha256 = (ProtobufCBinaryData){};
      break;
    case k_c6m_mdl_fault_failed_zero_status:
      internal_clear_http_metadata(chunk);
      chunk->state  = RA8__MDL__STATE__STATE_FAILED;
      chunk->status = 0;
      chunk->data   = (ProtobufCBinaryData){};
      chunk->sha256 = (ProtobufCBinaryData){};
      break;
    case k_c6m_mdl_fault_cancelled:
      internal_clear_http_metadata(chunk);
      chunk->state  = RA8__MDL__STATE__STATE_CANCELLED;
      chunk->status = 0;
      chunk->data   = (ProtobufCBinaryData){};
      chunk->sha256 = (ProtobufCBinaryData){};
      break;
    case k_c6m_mdl_fault_cancelled_with_data:
      internal_clear_http_metadata(chunk);
      chunk->state  = RA8__MDL__STATE__STATE_CANCELLED;
      chunk->status = 0;
      chunk->data   = (ProtobufCBinaryData){.len = 1U, .data = &s_bad_data};
      chunk->sha256 = (ProtobufCBinaryData){};
      break;
    case k_c6m_mdl_fault_downloading_error:
      chunk->state  = RA8__MDL__STATE__STATE_DOWNLOADING;
      chunk->status = (int32_t)k_ra8_fail;
      chunk->sha256 = (ProtobufCBinaryData){};
      break;
    default:
      return false;
  }
  return true;
}

/**
 * @brief Mutate the sequencing or payload of one decoded Chunk for the
 * non-terminal fault family.
 * @details Rewrites only the fields the selected sequencing or payload
 * fault needs, preserving the generated decoder's owned byte spans.
 * @param[in,out] chunk Decoded generated chunk to mutate in place.
 * @param[in] fault Candidate chunk mutation.
 * @return Whether @p fault named a sequencing or payload mutation.
 * @retval true The selected mutation was applied.
 * @retval false @p fault does not belong to this family.
 * @pre @p chunk is non-null and caller-owned.
 * @pre String fields still reference decoder-owned storage.
 * @post A recognised fault malforms exactly the fields it names.
 * @post An unrecognised fault leaves @p chunk untouched.
 * @note Corrupt-data injection asserts that decoded data is nonempty.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_mutate_chunk_state_payload(Ra8__Mdl__Chunk*         chunk,
                                                             ra8_c6_model_mdl_fault_t fault)
{
  switch (fault) {
    case k_c6m_mdl_fault_out_of_order:
      chunk->sequence += 1U;
      break;
    case k_c6m_mdl_fault_corrupt_data:
      TEST_ASSERT(chunk->data.len != 0U);
      TEST_ASSERT_NOT_NULL(chunk->data.data);
      chunk->data.data[0] ^= 1U;
      break;
    default:
      return false;
  }
  return true;
}

/**
 * @brief Mutate the state, payload or correlation of one decoded Chunk.
 * @details Dispatches to the terminal-state family first, then the
 * sequencing/payload family, so exactly one mutation is applied per fault.
 * @param[in,out] chunk Decoded generated chunk to mutate in place.
 * @param[in] fault Candidate chunk mutation.
 * @return Whether @p fault named a state, payload or correlation mutation.
 * @retval true The selected mutation was applied.
 * @retval false @p fault belongs to the metadata family instead.
 * @pre @p chunk is non-null and caller-owned.
 * @pre String fields still reference decoder-owned storage.
 * @post A recognised fault malforms exactly the fields it names.
 * @post An unrecognised fault leaves @p chunk untouched.
 * @note Corrupt-data injection asserts that decoded data is nonempty.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_mutate_chunk_state(Ra8__Mdl__Chunk*         chunk,
                                                     ra8_c6_model_mdl_fault_t fault)
{
  if (internal_mutate_chunk_state_terminal(chunk, fault)) {
    return true;
  }
  return internal_mutate_chunk_state_payload(chunk, fault);
}

/**
 * @brief Mutate the HTTP response metadata of one decoded Chunk.
 * @details Rewrites exactly one status or selected header so a client vector
 * varies one response-metadata condition against an otherwise valid chunk.
 * @param[in,out] chunk Decoded generated chunk to mutate in place.
 * @param[in] fault Candidate chunk mutation.
 * @return Whether @p fault named a response-metadata mutation.
 * @retval true The selected mutation was applied.
 * @retval false @p fault is not a metadata mutation.
 * @pre @p chunk is non-null and caller-owned.
 * @pre String fields still reference decoder-owned storage.
 * @post A recognised fault malforms exactly one metadata field.
 * @post An unrecognised fault leaves @p chunk untouched.
 * @note Header literals outlive the repack because the caller restores the
 * decoder's own pointers before releasing the message.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_mutate_chunk_metadata(Ra8__Mdl__Chunk*         chunk,
                                                        ra8_c6_model_mdl_fault_t fault)
{
  switch (fault) {
    case k_c6m_mdl_fault_data_http_status:
      chunk->http_status = (int32_t)k_ra8_mdl_http_status_min;
      break;
    case k_c6m_mdl_fault_data_retry_after:
      chunk->retry_after = (char*)"5";
      break;
    case k_c6m_mdl_fault_data_etag:
      chunk->etag = (char*)"\"data-etag\"";
      break;
    case k_c6m_mdl_fault_data_last_modified:
      chunk->last_modified = (char*)"Wed, 21 Oct 2015 07:28:00 GMT";
      break;
    case k_c6m_mdl_fault_data_content_type:
      chunk->content_type = (char*)"application/octet-stream";
      break;
    case k_c6m_mdl_fault_complete_low_status:
      chunk->http_status = (int32_t)k_ra8_mdl_http_status_min - 1;
      break;
    case k_c6m_mdl_fault_complete_high_status:
      chunk->http_status = (int32_t)k_ra8_mdl_http_status_max + 1;
      break;
    case k_c6m_mdl_fault_complete_split_retry:
      chunk->retry_after = (char*)"5\rX-Injected: 1";
      break;
    case k_c6m_mdl_fault_complete_split_etag:
      chunk->etag = (char*)"\"etag\"\nX-Injected: 1";
      break;
    case k_c6m_mdl_fault_complete_split_date:
      chunk->last_modified = (char*)"Wed, 21 Oct 2015 07:28:00 GMT\rX-Injected: 1";
      break;
    case k_c6m_mdl_fault_complete_split_type:
      chunk->content_type = (char*)"application/octet-stream\nX-Injected: 1";
      break;
    default:
      return false;
  }
  return true;
}

/**
 * @brief Mutate one decoded Chunk according to the selected fault.
 * @details Dispatches to the state family first, then the response-metadata
 * family, so exactly one mutation is applied per injected fault.
 * @param[in,out] chunk Decoded generated chunk to mutate in place.
 * @param[in] fault Malformed or terminal chunk shape to inject.
 * @return Nothing.
 * @pre @p chunk is non-null and caller-owned.
 * @pre @p fault selects a chunk response mutation.
 * @post Exactly the selected fields are malformed.
 * @post Decoder allocation ownership is unchanged.
 * @note A fault belonging to neither family is a fixture assertion.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_mutate_chunk(Ra8__Mdl__Chunk*         chunk,
                                               ra8_c6_model_mdl_fault_t fault)
{
  if (internal_mutate_chunk_state(chunk, fault)) {
    return;
  }
  TEST_ASSERT(internal_mutate_chunk_metadata(chunk, fault));
}

/**
 * @brief Decode, mutate, and repack one generated Chunk.
 * @details Temporarily borrows decoded data and digest pointers across field
 * mutation, then restores them before freeing the generated message.
 * @param[in,out] response Writable packed response bytes.
 * @param[in] response_cap Capacity of @p response.
 * @param[in,out] response_len Packed length before and after mutation.
 * @param[in] fault Chunk mutation to apply.
 * @return Nothing.
 * @pre All pointers are non-null and the packed Chunk is valid.
 * @pre `*response_len <= response_cap`.
 * @post The selected malformed Chunk is packed within the supplied capacity.
 * @post Decoder-owned data and digest storage are released exactly once.
 * @note Decode or pack failure is a fixture assertion.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_repack_chunk(uint8_t*                 response,
                                               size_t                   response_cap,
                                               size_t*                  response_len,
                                               ra8_c6_model_mdl_fault_t fault)
{
  Ra8__Mdl__Chunk* chunk = ra8__mdl__chunk__unpack(nullptr, *response_len, response);
  TEST_ASSERT_NOT_NULL(chunk);
  const ProtobufCBinaryData owned_data          = chunk->data;
  const ProtobufCBinaryData owned_sha           = chunk->sha256;
  char* const               owned_retry_after   = chunk->retry_after;
  char* const               owned_etag          = chunk->etag;
  char* const               owned_last_modified = chunk->last_modified;
  char* const               owned_content_type  = chunk->content_type;
  internal_mutate_chunk(chunk, fault);
  *response_len = ra8__mdl__chunk__get_packed_size(chunk);
  TEST_ASSERT(*response_len <= response_cap);
  TEST_ASSERT_EQ((int64_t)*response_len, (int64_t)ra8__mdl__chunk__pack(chunk, response));
  chunk->data          = owned_data;
  chunk->sha256        = owned_sha;
  chunk->retry_after   = owned_retry_after;
  chunk->etag          = owned_etag;
  chunk->last_modified = owned_last_modified;
  chunk->content_type  = owned_content_type;
  ra8__mdl__chunk__free_unpacked(chunk, nullptr);
}

/**
 * @brief Decode, mutate, and repack one generated Accepted response.
 * @details Changes exactly one validation field and repacks through the same
 * generated codec used by the production response decoder.
 * @param[in,out] response Writable packed response bytes.
 * @param[in] response_cap Capacity of @p response.
 * @param[in,out] response_len Packed length before and after mutation.
 * @param[in] fault Accepted mutation to apply.
 * @return Nothing.
 * @pre All pointers are non-null and the packed Accepted response is valid.
 * @pre @p fault selects one Accepted field mutation.
 * @post The selected malformed Accepted response is packed within capacity.
 * @post Decoder-owned storage is released exactly once.
 * @note Decode or pack failure is a fixture assertion.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_repack_accepted(uint8_t*                 response,
                                                  size_t                   response_cap,
                                                  size_t*                  response_len,
                                                  ra8_c6_model_mdl_fault_t fault)
{
  Ra8__Mdl__Accepted* accepted = ra8__mdl__accepted__unpack(nullptr, *response_len, response);
  TEST_ASSERT_NOT_NULL(accepted);
  switch (fault) {
    case k_c6m_mdl_fault_accepted_bad_version:
      accepted->protocol_version += 1U;
      break;
    case k_c6m_mdl_fault_accepted_zero_job:
      accepted->job_id = 0U;
      break;
    case k_c6m_mdl_fault_accepted_zero_max:
      accepted->max_chunk_bytes = 0U;
      break;
    case k_c6m_mdl_fault_accepted_large_max:
      accepted->max_chunk_bytes = k_ra8_mdl_chunk_data_max + 1U;
      break;
    case k_c6m_mdl_fault_accepted_wrong_format:
      accepted->format = RA8__MDL__FORMAT__FORMAT_LOOSE;
      break;
    default:
      TEST_ASSERT(false);
  }
  *response_len = ra8__mdl__accepted__get_packed_size(accepted);
  TEST_ASSERT(*response_len <= response_cap);
  TEST_ASSERT_EQ((int64_t)*response_len, (int64_t)ra8__mdl__accepted__pack(accepted, response));
  ra8__mdl__accepted__free_unpacked(accepted, nullptr);
}

/**
 * @brief Append the canonical unknown field to one packed inner response.
 * @details Extends an otherwise-valid generated message with a bounded field
 * that protobuf-c preserves in the decoded unknown-field table.
 * @param[in,out] response Writable packed response bytes.
 * @param[in] response_cap Capacity of @p response.
 * @param[in,out] response_len Packed length before and after append.
 * @return Nothing.
 * @pre Every pointer is non-null and `*response_len <= response_cap`.
 * @pre The remaining capacity holds ::s_mdl_unknown_field.
 * @post The original response is followed by exactly one unknown varint field.
 * @post The resulting length remains within @p response_cap.
 * @note The generated decoder retains this field in `n_unknown_fields`.
 * @since 0.1.0
 */
RA8_INTERNAL static void
internal_append_unknown(uint8_t* response, size_t response_cap, size_t* response_len)
{
  TEST_ASSERT(response_cap >= *response_len);
  TEST_ASSERT((response_cap - *response_len) >= sizeof(s_mdl_unknown_field));
  memcpy(&response[*response_len], s_mdl_unknown_field, sizeof(s_mdl_unknown_field));
  *response_len += sizeof(s_mdl_unknown_field);
}

/**
 * @brief Consume and apply one malformed media-response request.
 * @pre Every pointer is non-null and `*response_len <= response_cap`.
 * @pre @p body initially references @p response for `*response_len` bytes.
 * @post A compatible non-none fault is consumed exactly once.
 * @post A fault for a later operation remains pending.
 * @post Any retained inner payload remains packed within @p response_cap.
 * @note Test-only, synchronous, and not thread-safe.
 * @since 0.1.0
 */
RA8_PRIV void priv_c6_model_mdl_fault_apply(ra8_c6_model_mdl_fault_t* fault_slot,
                                            Rpc*                      outer,
                                            RpcRespCustomRpc*         body,
                                            uint8_t*                  response,
                                            size_t                    response_cap,
                                            size_t*                   response_len)
{
  const ra8_c6_model_mdl_fault_t fault = *fault_slot;
  if (fault == k_c6m_mdl_fault_none) {
    return;
  }
  const bool accepted_fault = (fault >= k_c6m_mdl_fault_accepted_bad_version) &&
                              (fault <= k_c6m_mdl_fault_accepted_wrong_format);
  const bool outer_fault =
    (fault >= k_c6m_mdl_fault_response_no_body) && (fault <= k_c6m_mdl_fault_response_zero_len);
  const bool start_response = body->custom_msg_id == (uint32_t)k_ra8_mdl_rpc_start;
  const bool next_response  = body->custom_msg_id == (uint32_t)k_ra8_mdl_rpc_next;
  if ((accepted_fault && !start_response) ||
      (!accepted_fault && !outer_fault && (fault != k_c6m_mdl_fault_unknown_field) &&
       !next_response)) {
    return;
  }
  *fault_slot = k_c6m_mdl_fault_none;
  if (fault == k_c6m_mdl_fault_response_no_body) {
    outer->payload_case    = RPC__PAYLOAD__NOT_SET;
    outer->resp_custom_rpc = nullptr;
    return;
  }
  if (fault == k_c6m_mdl_fault_response_wrong_id) {
    body->custom_msg_id ^= 1U;
    return;
  }
  if (fault == k_c6m_mdl_fault_response_zero_len) {
    /* A present-but-empty bytes field: protobuf-c treats only a NULL pointer
       as absent, so this is packed onto the wire as a zero-length field and
       the client decodes it to a non-null span of length zero. */
    body->data.len = 0U;
    return;
  }
  if (fault == k_c6m_mdl_fault_response_empty_data) {
    body->data = (ProtobufCBinaryData){};
    return;
  }
  if (fault == k_c6m_mdl_fault_unknown_field) {
    internal_append_unknown(response, response_cap, response_len);
  } else if (accepted_fault) {
    internal_repack_accepted(response, response_cap, response_len, fault);
  } else {
    internal_repack_chunk(response, response_cap, response_len, fault);
  }
  body->data = (ProtobufCBinaryData){.len = *response_len, .data = response};
}
