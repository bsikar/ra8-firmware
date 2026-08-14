/**
 * @file ra8_c6link_mdl.c
 * @brief Pull-based media download client over generated protobuf codecs.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_c6link_mdl.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ra8_c6link_internal.h"
#include "ra8_media_download.pb-c.h"

/** @brief Largest encoded inner request (bounded URL plus protobuf overhead). */
typedef enum : uint16_t { k_mdl_request_bytes = RA8_MDL_URL_MAX + 32U } mdl_request_const_t;

/** @brief Response extractor variants. */
typedef enum : uint8_t {
  k_mdl_take_accepted  = 1U,
  k_mdl_take_chunk     = 2U,
  k_mdl_take_cancelled = 3U,
} mdl_take_kind_t;

/** @brief Context consumed synchronously by the CustomRpc response extractor. */
typedef struct {
  ra8_c6link_t*      link;
  ra8_mdl_session_t* session;
  ra8_mdl_chunk_t*   chunk;
  uint32_t           operation;
  uint16_t           requested_bytes;
  mdl_take_kind_t    kind;
} mdl_take_ctx_t;

/** @brief Validate the state-specific fields of a correlated chunk. */
static bool mdl_chunk_semantics_valid(const Ra8__Mdl__Chunk* msg)
{
  if (msg->data.len > (UINT64_MAX - msg->offset)) {
    return false;
  }
  const uint64_t end               = msg->offset + msg->data.len;
  const bool     total_covers_data = (msg->total_bytes == 0U) || (end <= msg->total_bytes);
  if (!total_covers_data) {
    return false;
  }
  switch (msg->state) {
    case RA8__MDL__STATE__STATE_DOWNLOADING:
      return (msg->status == 0) && (msg->data.len != 0U) && (msg->sha256.len == 0U);
    case RA8__MDL__STATE__STATE_COMPLETE:
      return (msg->status == 0) && (msg->data.len == 0U) &&
             (msg->sha256.len == RA8_MDL_SHA256_BYTES) && (msg->sha256.data != nullptr) &&
             ((msg->total_bytes == 0U) || (msg->total_bytes == end));
    case RA8__MDL__STATE__STATE_CANCELLED:
      return (msg->status == 0) && (msg->data.len == 0U) && (msg->sha256.len == 0U);
    case RA8__MDL__STATE__STATE_FAILED:
      return (msg->status > 0) && (msg->status <= UINT16_MAX) && (msg->data.len == 0U) &&
             (msg->sha256.len == 0U);
    default:
      return false;
  }
}

/** @brief Decode and validate a start response. */
static ra8_err_t mdl_take_accepted(mdl_take_ctx_t* take, const ProtobufCBinaryData* data)
{
  ProtobufCAllocator alloc = {};
  ra8_c6link_priv_arena_bind(&alloc, take->link);
  Ra8__Mdl__Accepted* msg = ra8__mdl__accepted__unpack(&alloc, data->len, data->data);
  if (msg == nullptr) {
    return k_ra8_err_protocol_error;
  }
  const bool valid = (msg->protocol_version == RA8_MDL_PROTOCOL_VERSION) && (msg->job_id != 0U) &&
                     (msg->max_chunk_bytes != 0U) &&
                     (msg->max_chunk_bytes <= RA8_MDL_CHUNK_DATA_MAX);
  if (valid) {
    *take->session = (ra8_mdl_session_t){
      .job_id          = msg->job_id,
      .next_sequence   = 0U,
      .next_offset     = 0U,
      .max_chunk_bytes = msg->max_chunk_bytes,
      .active          = true,
    };
  }
  ra8__mdl__accepted__free_unpacked(msg, &alloc);
  return valid ? k_ra8_ok : k_ra8_err_protocol_error;
}

/** @brief Decode a chunk and enforce job, sequence, offset, and size correlation. */
static ra8_err_t mdl_take_chunk(mdl_take_ctx_t* take, const ProtobufCBinaryData* data)
{
  ProtobufCAllocator alloc = {};
  ra8_c6link_priv_arena_bind(&alloc, take->link);
  Ra8__Mdl__Chunk* msg = ra8__mdl__chunk__unpack(&alloc, data->len, data->data);
  if (msg == nullptr) {
    return k_ra8_err_protocol_error;
  }
  const bool valid =
    (msg->protocol_version == RA8_MDL_PROTOCOL_VERSION) && (msg->job_id == take->session->job_id) &&
    (msg->sequence == take->session->next_sequence) &&
    (msg->offset == take->session->next_offset) && (msg->data.len <= take->requested_bytes) &&
    (msg->data.len <= RA8_MDL_CHUNK_DATA_MAX) &&
    ((msg->data.len == 0U) || (msg->data.data != nullptr)) && mdl_chunk_semantics_valid(msg);
  ra8_err_t result = valid ? k_ra8_ok : k_ra8_err_protocol_error;
  if (valid) {
    *take->chunk = (ra8_mdl_chunk_t){
      .job_id      = msg->job_id,
      .sequence    = msg->sequence,
      .offset      = msg->offset,
      .total_bytes = msg->total_bytes,
      .state       = (ra8_mdl_state_t)msg->state,
      .status      = (ra8_err_t)msg->status,
      .data_len    = (uint16_t)msg->data.len,
      .has_sha256  = (msg->sha256.len == RA8_MDL_SHA256_BYTES),
    };
    if (msg->data.len != 0U) {
      memcpy(take->chunk->data, msg->data.data, msg->data.len);
    }
    if (take->chunk->has_sha256) {
      memcpy(take->chunk->sha256, msg->sha256.data, RA8_MDL_SHA256_BYTES);
    }
    take->session->next_offset += msg->data.len;
    take->session->next_sequence += 1U;
    if ((msg->state == RA8__MDL__STATE__STATE_COMPLETE) ||
        (msg->state == RA8__MDL__STATE__STATE_CANCELLED) ||
        (msg->state == RA8__MDL__STATE__STATE_FAILED)) {
      take->session->active = false;
    }
    if (msg->state == RA8__MDL__STATE__STATE_FAILED) {
      result = (ra8_err_t)msg->status;
    }
  }
  ra8__mdl__chunk__free_unpacked(msg, &alloc);
  return result;
}

/** @brief Decode a cancellation acknowledgement for the active job. */
static ra8_err_t mdl_take_cancelled(mdl_take_ctx_t* take, const ProtobufCBinaryData* data)
{
  ProtobufCAllocator alloc = {};
  ra8_c6link_priv_arena_bind(&alloc, take->link);
  Ra8__Mdl__Cancelled* msg = ra8__mdl__cancelled__unpack(&alloc, data->len, data->data);
  if (msg == nullptr) {
    return k_ra8_err_protocol_error;
  }
  const bool valid = (msg->protocol_version == RA8_MDL_PROTOCOL_VERSION) &&
                     (msg->job_id == take->session->job_id) && (msg->status == 0);
  if (valid) {
    take->session->active = false;
  }
  ra8__mdl__cancelled__free_unpacked(msg, &alloc);
  return valid ? k_ra8_ok : k_ra8_err_protocol_error;
}

/** @brief Extract one inner media response from an ESP-hosted CustomRpc response. */
static ra8_err_t mdl_take_response(void* ctx, const void* msg_v)
{
  mdl_take_ctx_t*         take = (mdl_take_ctx_t*)ctx;
  const Rpc*              msg  = (const Rpc*)msg_v;
  const RpcRespCustomRpc* body = msg->resp_custom_rpc;
  if ((body == nullptr) || (body->custom_msg_id != take->operation)) {
    return k_ra8_err_protocol_error;
  }
  const ra8_err_t remote = ra8_c6link_priv_resp(take->link, take->operation, body->resp);
  if (remote != k_ra8_ok) {
    return remote;
  }
  if ((body->data.data == nullptr) || (body->data.len == 0U)) {
    return k_ra8_err_protocol_error;
  }
  switch (take->kind) {
    case k_mdl_take_accepted:
      return mdl_take_accepted(take, &body->data);
    case k_mdl_take_chunk:
      return mdl_take_chunk(take, &body->data);
    case k_mdl_take_cancelled:
      return mdl_take_cancelled(take, &body->data);
    default:
      return k_ra8_err_protocol_error;
  }
}

/** @brief Send one already-encoded inner message through CustomRpc. */
static ra8_err_t mdl_call(ra8_c6link_t*   link,
                          uint32_t        operation,
                          uint8_t*        data,
                          size_t          data_len,
                          mdl_take_ctx_t* take)
{
  RpcReqCustomRpc body = RPC__REQ__CUSTOM_RPC__INIT;
  body.custom_msg_id   = operation;
  body.data            = (ProtobufCBinaryData){.len = data_len, .data = data};

  Rpc req            = RPC__INIT;
  req.msg_type       = RPC_TYPE__Req;
  req.msg_id         = RPC_ID__Req_CustomRpc;
  req.payload_case   = RPC__PAYLOAD_REQ_CUSTOM_RPC;
  req.req_custom_rpc = &body;
  take->link         = link;
  take->operation    = operation;
  return ra8_c6link_priv_rpc_call(link,
                                  &req,
                                  (uint32_t)RPC_ID__Resp_CustomRpc,
                                  mdl_take_response,
                                  take);
}

ra8_err_t ra8_c6link_mdl_start(ra8_c6link_t*      link,
                               const char*        url,
                               ra8_mdl_format_t   format,
                               ra8_mdl_session_t* session)
{
  if ((link == nullptr) || (url == nullptr) || (session == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  const size_t url_len = strnlen(url, RA8_MDL_URL_MAX);
  if ((url_len == 0U) || (url_len >= RA8_MDL_URL_MAX) || (format < k_ra8_mdl_format_rabook) ||
      (format > k_ra8_mdl_format_epub)) {
    return k_ra8_err_invalid_arg;
  }
  *session = (ra8_mdl_session_t){};
  char url_copy[RA8_MDL_URL_MAX];
  memcpy(url_copy, url, url_len + 1U);
  Ra8__Mdl__StartRequest inner;
  ra8__mdl__start_request__init(&inner);
  inner.protocol_version = RA8_MDL_PROTOCOL_VERSION;
  inner.url              = url_copy;
  inner.format           = (Ra8__Mdl__Format)format;
  uint8_t      data[k_mdl_request_bytes];
  const size_t packed = ra8__mdl__start_request__get_packed_size(&inner);
  if ((packed == 0U) || (packed > sizeof(data)) ||
      (ra8__mdl__start_request__pack(&inner, data) != packed)) {
    return k_ra8_err_invalid_size;
  }
  mdl_take_ctx_t take = {.session = session, .kind = k_mdl_take_accepted};
  return mdl_call(link, k_ra8_mdl_rpc_start, data, packed, &take);
}

ra8_err_t ra8_c6link_mdl_next(ra8_c6link_t*      link,
                              ra8_mdl_session_t* session,
                              uint16_t           max_bytes,
                              ra8_mdl_chunk_t*   chunk)
{
  if ((link == nullptr) || (session == nullptr) || (chunk == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  if (!session->active || (session->job_id == 0U)) {
    return k_ra8_err_invalid_state;
  }
  if ((max_bytes == 0U) || (max_bytes > session->max_chunk_bytes) ||
      (max_bytes > RA8_MDL_CHUNK_DATA_MAX)) {
    return k_ra8_err_invalid_size;
  }
  *chunk                      = (ra8_mdl_chunk_t){};
  Ra8__Mdl__NextRequest inner = RA8__MDL__NEXT_REQUEST__INIT;
  inner.protocol_version      = RA8_MDL_PROTOCOL_VERSION;
  inner.job_id                = session->job_id;
  inner.acknowledged_offset   = session->next_offset;
  inner.max_bytes             = max_bytes;
  uint8_t      data[k_mdl_request_bytes];
  const size_t packed = ra8__mdl__next_request__get_packed_size(&inner);
  if ((packed == 0U) || (packed > sizeof(data)) ||
      (ra8__mdl__next_request__pack(&inner, data) != packed)) {
    return k_ra8_err_invalid_size;
  }
  mdl_take_ctx_t take = {.session         = session,
                         .chunk           = chunk,
                         .requested_bytes = max_bytes,
                         .kind            = k_mdl_take_chunk};
  return mdl_call(link, k_ra8_mdl_rpc_next, data, packed, &take);
}

ra8_err_t ra8_c6link_mdl_cancel(ra8_c6link_t* link, ra8_mdl_session_t* session)
{
  if ((link == nullptr) || (session == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  if (!session->active || (session->job_id == 0U)) {
    return k_ra8_err_invalid_state;
  }
  Ra8__Mdl__CancelRequest inner = RA8__MDL__CANCEL_REQUEST__INIT;
  inner.protocol_version        = RA8_MDL_PROTOCOL_VERSION;
  inner.job_id                  = session->job_id;
  uint8_t      data[k_mdl_request_bytes];
  const size_t packed = ra8__mdl__cancel_request__get_packed_size(&inner);
  if ((packed == 0U) || (packed > sizeof(data)) ||
      (ra8__mdl__cancel_request__pack(&inner, data) != packed)) {
    return k_ra8_err_invalid_size;
  }
  mdl_take_ctx_t take = {.session = session, .kind = k_mdl_take_cancelled};
  return mdl_call(link, k_ra8_mdl_rpc_cancel, data, packed, &take);
}
