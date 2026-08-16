/**
 * @file ra8_c6link_mdl.c
 * @brief Pull-based media download client over generated protobuf codecs.
 * @details Encodes bounded media requests and validates every correlated
 * response before mutating caller-owned session or chunk state.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_c6link_mdl.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ra8_c6link_internal.h"
#include "ra8_media_download.pb-c.h"

static_assert((uint32_t)k_ra8_mdl_format_loose == RA8__MDL__FORMAT__FORMAT_LOOSE);
static_assert((uint32_t)k_ra8_mdl_format_cbz == RA8__MDL__FORMAT__FORMAT_CBZ);
static_assert((uint32_t)k_ra8_mdl_format_cbt == RA8__MDL__FORMAT__FORMAT_CBT);
static_assert((uint32_t)k_ra8_mdl_format_cbr == RA8__MDL__FORMAT__FORMAT_CBR);
static_assert((uint32_t)k_ra8_mdl_format_cbt_xz == RA8__MDL__FORMAT__FORMAT_CBT_XZ);
static_assert((uint32_t)k_ra8_mdl_format_cbt_gz == RA8__MDL__FORMAT__FORMAT_CBT_GZ);
static_assert((uint32_t)k_ra8_mdl_format_epub == RA8__MDL__FORMAT__FORMAT_EPUB);
static_assert((uint32_t)k_ra8_mdl_format_jof == RA8__MDL__FORMAT__FORMAT_JOF);
static_assert((uint32_t)k_ra8_mdl_format_rabook == RA8__MDL__FORMAT__FORMAT_RABOOK);
static_assert((uint32_t)k_ra8_mdl_format_invalid == RA8__MDL__FORMAT__FORMAT_INVALID);

/** @brief Largest encoded inner request (bounded URL plus protobuf overhead).
 */
typedef enum : uint16_t {
  k_mdl_request_bytes = k_ra8_mdl_url_max + 32U, /**< Maximum packed request bytes. */
} mdl_request_const_t;

/** @brief Response extractor variants. */
typedef enum : uint8_t {
  k_mdl_take_accepted  = 1U, /**< Extract an Accepted response. */
  k_mdl_take_chunk     = 2U, /**< Extract a Chunk response.     */
  k_mdl_take_cancelled = 3U, /**< Extract a Cancelled response. */
} mdl_take_kind_t;

/** @brief Context consumed synchronously by the CustomRpc response extractor.
 */
typedef struct {
  ra8_c6link_t*      link;             /**< Link whose arena decoded the response.  */
  ra8_mdl_session_t* session;          /**< Correlated caller session.              */
  ra8_mdl_chunk_t*   chunk;            /**< Optional caller chunk destination.      */
  uint32_t           operation;        /**< Expected CustomRpc operation id.        */
  uint16_t           requested_bytes;  /**< Maximum accepted response body bytes.   */
  ra8_mdl_format_t   requested_format; /**< Format the Accepted response must echo. */
  mdl_take_kind_t    kind;             /**< Expected generated response variant.    */
} mdl_take_ctx_t;

/**
 * @brief Validate state-specific fields of one correlated chunk
 * @details Enforces data/digest/status combinations and overflow-safe totals.
 * @param[in] msg Decoded generated chunk.
 * @return Whether the semantic combination is valid.
 * @retval true State-specific fields and totals are coherent.
 * @retval false A state, size, status, or digest rule is violated.
 * @pre @p msg is non-null and decoded by the bounded link arena.
 * @pre Binary-data lengths describe their decoded buffers.
 * @post No caller or decoded state is modified.
 * @post True guarantees later bounded copies are size-safe.
 * @note Reentrant for independent decoded messages.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_mdl_chunk_semantics_valid(const Ra8__Mdl__Chunk* msg)
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
             (msg->sha256.len == k_ra8_mdl_sha256_bytes) && (msg->sha256.data != nullptr) &&
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

/**
 * @brief Decode and validate one accepted-job response
 * @details Uses the link-owned bounded arena and updates the session only after
 * validation.
 * @param[in,out] take Response extraction context.
 * @param[in] data Packed generated Accepted response.
 * @return Decode status.
 * @retval k_ra8_ok Session was activated with bounded correlation state.
 * @retval k_ra8_err_protocol_error Decode or field validation failed.
 * @pre @p take, its link/session, and @p data are non-null.
 * @pre The link arena is exclusively owned for this synchronous callback.
 * @post Success initializes an active session.
 * @post Failure leaves the caller's session unchanged.
 * @note Not thread-safe for a shared c6link arena.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_mdl_take_accepted(mdl_take_ctx_t*            take,
                                                         const ProtobufCBinaryData* data)
{
  ProtobufCAllocator alloc = {};
  priv_c6link_arena_bind(&alloc, take->link);
  Ra8__Mdl__Accepted* msg = ra8__mdl__accepted__unpack(&alloc, data->len, data->data);
  if (msg == nullptr) {
    return k_ra8_err_protocol_error;
  }
  const bool valid = (msg->base.n_unknown_fields == 0U) &&
                     (msg->protocol_version == k_ra8_mdl_protocol_version) && (msg->job_id != 0U) &&
                     (msg->max_chunk_bytes != 0U) &&
                     (msg->max_chunk_bytes <= k_ra8_mdl_chunk_data_max) &&
                     ((uint32_t)msg->format == (uint32_t)take->requested_format);
  if (valid) {
    *take->session = (ra8_mdl_session_t){
      .job_id          = msg->job_id,
      .next_sequence   = 0U,
      .next_offset     = 0U,
      .max_chunk_bytes = msg->max_chunk_bytes,
      .format          = take->requested_format,
      .active          = true,
    };
  }
  ra8__mdl__accepted__free_unpacked(msg, &alloc);
  return valid ? k_ra8_ok : k_ra8_err_protocol_error;
}

/**
 * @brief Decode a chunk and enforce correlation and size bounds
 * @details Accepts only the exact active job, sequence, offset, and requested
 * span.
 * @param[in,out] take Active extraction/session context.
 * @param[in] data Packed generated Chunk response.
 * @return Decode or remote terminal status.
 * @retval k_ra8_ok A valid data or successful terminal chunk was copied.
 * @retval k_ra8_err_protocol_error Decode or correlation validation failed.
 * @pre @p take owns an active session and non-null output chunk.
 * @pre The link arena is exclusively owned for this callback.
 * @post Success advances offset/sequence by exactly the decoded data length.
 * @post A valid terminal response deactivates the session.
 * @note Not thread-safe for a shared session or c6link arena.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_mdl_take_chunk(mdl_take_ctx_t*            take,
                                                      const ProtobufCBinaryData* data)
{
  ProtobufCAllocator alloc = {};
  priv_c6link_arena_bind(&alloc, take->link);
  Ra8__Mdl__Chunk* msg = ra8__mdl__chunk__unpack(&alloc, data->len, data->data);
  if (msg == nullptr) {
    return k_ra8_err_protocol_error;
  }
  const bool valid =
    (msg->base.n_unknown_fields == 0U) && (msg->protocol_version == k_ra8_mdl_protocol_version) &&
    (msg->job_id == take->session->job_id) && (msg->sequence == take->session->next_sequence) &&
    (msg->offset == take->session->next_offset) && (msg->data.len <= take->requested_bytes) &&
    (msg->data.len <= k_ra8_mdl_chunk_data_max) &&
    ((msg->data.len == 0U) || (msg->data.data != nullptr)) &&
    internal_mdl_chunk_semantics_valid(msg);
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
      .has_sha256  = (msg->sha256.len == k_ra8_mdl_sha256_bytes),
    };
    if (msg->data.len != 0U) {
      memcpy(take->chunk->data, msg->data.data, msg->data.len);
    }
    if (take->chunk->has_sha256) {
      memcpy(take->chunk->sha256, msg->sha256.data, k_ra8_mdl_sha256_bytes);
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

/**
 * @brief Decode a cancellation acknowledgement for the active job
 * @details Rejects acknowledgements for another job or protocol version.
 * @param[in,out] take Active extraction/session context.
 * @param[in] data Packed generated Cancelled response.
 * @return Decode status.
 * @retval k_ra8_ok Matching cancellation deactivated the session.
 * @retval k_ra8_err_protocol_error Decode or correlation validation failed.
 * @pre @p take and its active session are non-null.
 * @pre The link arena is exclusively owned for this callback.
 * @post Success makes the session inactive.
 * @post Failure preserves session activity for caller recovery.
 * @note Not thread-safe for a shared session or c6link arena.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_mdl_take_cancelled(mdl_take_ctx_t*            take,
                                                          const ProtobufCBinaryData* data)
{
  ProtobufCAllocator alloc = {};
  priv_c6link_arena_bind(&alloc, take->link);
  Ra8__Mdl__Cancelled* msg = ra8__mdl__cancelled__unpack(&alloc, data->len, data->data);
  if (msg == nullptr) {
    return k_ra8_err_protocol_error;
  }
  const bool valid = (msg->base.n_unknown_fields == 0U) &&
                     (msg->protocol_version == k_ra8_mdl_protocol_version) &&
                     (msg->job_id == take->session->job_id) && (msg->status == 0);
  if (valid) {
    take->session->active = false;
  }
  ra8__mdl__cancelled__free_unpacked(msg, &alloc);
  return valid ? k_ra8_ok : k_ra8_err_protocol_error;
}

/**
 * @brief Extract one generated media payload from a CustomRpc response
 * @details Validates outer response identity/status before selecting the
 * expected inner type.
 * @param[in,out] ctx ::mdl_take_ctx_t selected by the initiating call.
 * @param[in] msg_v Decoded ESP-hosted Rpc response.
 * @return Extraction status.
 * @retval k_ra8_ok Expected inner response was accepted.
 * @retval k_ra8_err_protocol_error Outer or inner response is incoherent.
 * @pre @p ctx and @p msg_v are non-null for the synchronous callback.
 * @pre `kind` matches the initiating media operation.
 * @post Success applies exactly one expected state transition.
 * @post Failure does not select a different inner response type.
 * @note Not thread-safe for a shared c6link/session.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_mdl_take_response(void* ctx, const void* msg_v)
{
  mdl_take_ctx_t*         take = (mdl_take_ctx_t*)ctx;
  const Rpc*              msg  = (const Rpc*)msg_v;
  const RpcRespCustomRpc* body = msg->resp_custom_rpc;
  if ((body == nullptr) || (body->custom_msg_id != take->operation)) {
    return k_ra8_err_protocol_error;
  }
  const ra8_err_t remote = priv_c6link_resp(take->link, take->operation, body->resp);
  if (remote != k_ra8_ok) {
    return remote;
  }
  if ((body->data.data == nullptr) || (body->data.len == 0U)) {
    return k_ra8_err_protocol_error;
  }
  switch (take->kind) {
    case k_mdl_take_accepted:
      return internal_mdl_take_accepted(take, &body->data);
    case k_mdl_take_chunk:
      return internal_mdl_take_chunk(take, &body->data);
    case k_mdl_take_cancelled:
      return internal_mdl_take_cancelled(take, &body->data);
    default:
      return k_ra8_err_protocol_error;
  }
}

/* protobuf-c's pack-only binary-data ABI still declares its byte pointer
 * mutable. */
// NOLINTBEGIN(readability-non-const-parameter)
/**
 * @brief Send one already-encoded generated message through CustomRpc
 * @details Wraps caller-owned inner bytes without retaining them after the
 * synchronous call.
 * @param[in,out] link Already-open exclusively owned c6link.
 * @param[in] operation Stable media operation identifier.
 * @param[in] data Packed inner request bytes.
 * @param[in] data_len Valid bytes at @p data.
 * @param[in,out] take Expected response extraction context.
 * @return Transport, remote, or response-validation status.
 * @retval k_ra8_ok The expected response was extracted.
 * @retval k_ra8_err_protocol_error Response identity or payload was invalid.
 * @pre Every pointer is non-null and @p data_len is within the local buffer.
 * @pre ::ra8_c6link_open succeeded and no concurrent caller uses @p link.
 * @post Inner request storage is no longer referenced when the call returns.
 * @post Response state changes only through ::internal_mdl_take_response.
 * @note Synchronous and not thread-safe for a shared link.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_mdl_call(ra8_c6link_t*   link,
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
  return priv_c6link_rpc_call(link,
                              &req,
                              (uint32_t)RPC_ID__Resp_CustomRpc,
                              internal_mdl_take_response,
                              take);
}
// NOLINTEND(readability-non-const-parameter)

ra8_err_t ra8_c6link_mdl_start(ra8_c6link_t*      link,
                               const char*        url,
                               ra8_mdl_format_t   format,
                               ra8_mdl_session_t* session)
{
  if ((link == nullptr) || (url == nullptr) || (session == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  const size_t https_prefix_len = sizeof("https://") - 1U;
  const size_t url_len          = strnlen(url, k_ra8_mdl_url_max);
  if ((url_len == 0U) || (url_len >= k_ra8_mdl_url_max) ||
      (strncmp(url, "https://", https_prefix_len) != 0) || (url[https_prefix_len] == '\0')) {
    return k_ra8_err_invalid_arg;
  }
  if ((uint32_t)format > (uint32_t)k_ra8_mdl_format_rabook) {
    return k_ra8_err_invalid_arg;
  }
  *session = (ra8_mdl_session_t){};
  char url_copy[k_ra8_mdl_url_max];
  memcpy(url_copy, url, url_len + 1U);
  Ra8__Mdl__StartRequest inner;
  ra8__mdl__start_request__init(&inner);
  inner.protocol_version = k_ra8_mdl_protocol_version;
  inner.url              = url_copy;
  inner.format           = (Ra8__Mdl__Format)format;
  uint8_t      data[k_mdl_request_bytes];
  const size_t packed = ra8__mdl__start_request__get_packed_size(&inner);
  if ((packed == 0U) || (packed > sizeof(data)) ||
      (ra8__mdl__start_request__pack(&inner, data) != packed)) {
    return k_ra8_err_invalid_size;
  }
  mdl_take_ctx_t take = {.session          = session,
                         .requested_format = format,
                         .kind             = k_mdl_take_accepted};
  return internal_mdl_call(link, k_ra8_mdl_rpc_start, data, packed, &take);
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
      (max_bytes > k_ra8_mdl_chunk_data_max)) {
    return k_ra8_err_invalid_size;
  }
  *chunk                      = (ra8_mdl_chunk_t){};
  Ra8__Mdl__NextRequest inner = RA8__MDL__NEXT_REQUEST__INIT;
  inner.protocol_version      = k_ra8_mdl_protocol_version;
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
  return internal_mdl_call(link, k_ra8_mdl_rpc_next, data, packed, &take);
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
  inner.protocol_version        = k_ra8_mdl_protocol_version;
  inner.job_id                  = session->job_id;
  uint8_t      data[k_mdl_request_bytes];
  const size_t packed = ra8__mdl__cancel_request__get_packed_size(&inner);
  if ((packed == 0U) || (packed > sizeof(data)) ||
      (ra8__mdl__cancel_request__pack(&inner, data) != packed)) {
    return k_ra8_err_invalid_size;
  }
  mdl_take_ctx_t take = {.session = session, .kind = k_mdl_take_cancelled};
  return internal_mdl_call(link, k_ra8_mdl_rpc_cancel, data, packed, &take);
}
