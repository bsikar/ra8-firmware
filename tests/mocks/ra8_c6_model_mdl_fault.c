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
 * @brief Mutate one decoded Chunk according to the selected fault.
 * @details Rewrites only fields needed for the selected negative or terminal
 * scenario while preserving the generated decoder's owned byte spans.
 * @param[in,out] chunk Decoded generated chunk to mutate in place.
 * @param[in] fault Malformed or terminal chunk shape to inject.
 * @return Nothing.
 * @pre @p chunk is non-null and caller-owned.
 * @pre @p fault selects a chunk response mutation.
 * @post Exactly the selected fields are malformed.
 * @post Decoder allocation ownership is unchanged.
 * @note Corrupt-data injection asserts that decoded data is nonempty.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_mutate_chunk(Ra8__Mdl__Chunk*         chunk,
                                               ra8_c6_model_mdl_fault_t fault)
{
  static uint8_t bad_data = k_c6m_mdl_digest_fill;
  switch (fault) {
    case k_c6m_mdl_fault_complete_no_sha:
      chunk->sha256 = (ProtobufCBinaryData){};
      break;
    case k_c6m_mdl_fault_complete_bad_total:
      chunk->total_bytes = chunk->offset + chunk->data.len + 1U;
      break;
    case k_c6m_mdl_fault_failed:
      chunk->state  = RA8__MDL__STATE__STATE_FAILED;
      chunk->status = (int32_t)k_ra8_fail;
      chunk->data   = (ProtobufCBinaryData){};
      chunk->sha256 = (ProtobufCBinaryData){};
      break;
    case k_c6m_mdl_fault_failed_zero_status:
      chunk->state  = RA8__MDL__STATE__STATE_FAILED;
      chunk->status = 0;
      chunk->data   = (ProtobufCBinaryData){};
      chunk->sha256 = (ProtobufCBinaryData){};
      break;
    case k_c6m_mdl_fault_cancelled:
      chunk->state  = RA8__MDL__STATE__STATE_CANCELLED;
      chunk->status = 0;
      chunk->data   = (ProtobufCBinaryData){};
      chunk->sha256 = (ProtobufCBinaryData){};
      break;
    case k_c6m_mdl_fault_cancelled_with_data:
      chunk->state  = RA8__MDL__STATE__STATE_CANCELLED;
      chunk->status = 0;
      chunk->data   = (ProtobufCBinaryData){.len = 1U, .data = &bad_data};
      chunk->sha256 = (ProtobufCBinaryData){};
      break;
    case k_c6m_mdl_fault_downloading_error:
      chunk->state  = RA8__MDL__STATE__STATE_DOWNLOADING;
      chunk->status = (int32_t)k_ra8_fail;
      chunk->sha256 = (ProtobufCBinaryData){};
      break;
    case k_c6m_mdl_fault_out_of_order:
      chunk->sequence += 1U;
      break;
    case k_c6m_mdl_fault_corrupt_data:
      TEST_ASSERT(chunk->data.len != 0U);
      TEST_ASSERT_NOT_NULL(chunk->data.data);
      chunk->data.data[0] ^= 1U;
      break;
    default:
      TEST_ASSERT(false);
  }
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
  const ProtobufCBinaryData owned_data = chunk->data;
  const ProtobufCBinaryData owned_sha  = chunk->sha256;
  internal_mutate_chunk(chunk, fault);
  *response_len = ra8__mdl__chunk__get_packed_size(chunk);
  TEST_ASSERT(*response_len <= response_cap);
  TEST_ASSERT_EQ((int64_t)*response_len, (int64_t)ra8__mdl__chunk__pack(chunk, response));
  chunk->data   = owned_data;
  chunk->sha256 = owned_sha;
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
    (fault >= k_c6m_mdl_fault_response_no_body) && (fault <= k_c6m_mdl_fault_response_empty_data);
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
