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

#include "ra8_attributes.h"
#include "ra8_c6link_internal.h"
#include "ra8_c6link_mdl_internal.h"
#include "ra8_media_download.pb-c.h"

static_assert((uint32_t)k_mdl_format_loose == RA8__MDL__FORMAT__FORMAT_LOOSE);
static_assert((uint32_t)k_mdl_format_cbz == RA8__MDL__FORMAT__FORMAT_CBZ);
static_assert((uint32_t)k_mdl_format_cbt == RA8__MDL__FORMAT__FORMAT_CBT);
static_assert((uint32_t)k_mdl_format_cbr == RA8__MDL__FORMAT__FORMAT_CBR);
static_assert((uint32_t)k_mdl_format_cbt_xz == RA8__MDL__FORMAT__FORMAT_CBT_XZ);
static_assert((uint32_t)k_mdl_format_cbt_gz == RA8__MDL__FORMAT__FORMAT_CBT_GZ);
static_assert((uint32_t)k_mdl_format_epub == RA8__MDL__FORMAT__FORMAT_EPUB);
static_assert((uint32_t)k_mdl_format_jof == RA8__MDL__FORMAT__FORMAT_JOF);
static_assert((uint32_t)k_mdl_format_rabook == RA8__MDL__FORMAT__FORMAT_RABOOK);
static_assert((uint32_t)k_mdl_format_invalid == RA8__MDL__FORMAT__FORMAT_INVALID);

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
  mdl_format_t       requested_format; /**< Format the Accepted response must echo. */
  mdl_take_kind_t    kind;             /**< Expected generated response variant.    */
} mdl_take_ctx_t;

/**
 * @brief Flatten one decoded generated chunk into the Zig rule layer's view.
 * @details The generated message layout is protoc-c output, so the rules live
 * in `src/internal/mdl_chunk.zig` over flat field values rather than over a
 * hand-mirrored copy of that layout. This is the only place the two meet.
 * @param[in] msg Decoded generated chunk.
 * @return A view borrowing @p msg's decoded spans.
 * @pre @p msg is non-null and its arena stays live for the whole call.
 * @post No decoded or caller-owned state is modified.
 * @note Pure and reentrant for independent messages.
 * @since 0.1.0
 */
RA8_INTERNAL static mdl_chunk_view_t internal_mdl_view(const Ra8__Mdl__Chunk* msg)
{
  return (mdl_chunk_view_t){
    .job_id        = msg->job_id,
    .sequence      = msg->sequence,
    .offset        = msg->offset,
    .total_bytes   = msg->total_bytes,
    .state         = (uint8_t)msg->state,
    .status        = msg->status,
    .data          = msg->data.data,
    .data_len      = msg->data.len,
    .sha256        = msg->sha256.data,
    .sha256_len    = msg->sha256.len,
    .http_status   = msg->http_status,
    .retry_after   = msg->retry_after,
    .etag          = msg->etag,
    .last_modified = msg->last_modified,
    .content_type  = msg->content_type,
  };
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
  const mdl_accepted_view_t view = {
    .protocol_version = msg->protocol_version,
    .job_id           = msg->job_id,
    .max_chunk_bytes  = msg->max_chunk_bytes,
    .format           = (uint32_t)msg->format,
    .unknown_fields   = (uint32_t)msg->base.n_unknown_fields,
  };
  const bool valid = priv_c6link_mdl_accepted_valid(&view, (uint32_t)take->requested_format);
  if (valid) {
    priv_c6link_mdl_session_activate(&view, take->session, (uint8_t)take->requested_format);
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
  const mdl_chunk_view_t     view = internal_mdl_view(msg);
  const mdl_chunk_key_view_t key  = {
     .protocol_version = msg->protocol_version,
     .job_id           = msg->job_id,
     .sequence         = msg->sequence,
     .offset           = msg->offset,
     .data_len         = (uint32_t)msg->data.len,
     .data_present     = (msg->data.data != nullptr),
     .unknown_fields   = (uint32_t)msg->base.n_unknown_fields,
  };
  const bool valid =
    priv_c6link_mdl_chunk_admissible(&key, &view, take->session, take->requested_bytes);
  const ra8_err_t result =
    valid ? priv_c6link_mdl_accept_chunk(&view, take->session, take->chunk) : k_ra8_err_protocol_error;
  ra8__mdl__chunk__free_unpacked(msg, &alloc);
  return result;
}

/**
 * @brief Decode a cancellation acknowledgement for the active job
 * @details Rejects acknowledgements for another job or protocol version.
 * @param[in,out] take Active extraction/session context.
 * @param[in] data Packed generated Cancelled response.
 * @param[in] len Valid bytes at @p data; the decoder reads no further.
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
RA8_INTERNAL static ra8_err_t
internal_mdl_take_cancelled(mdl_take_ctx_t* take, const uint8_t* data, size_t len)
{
  ProtobufCAllocator alloc = {};
  priv_c6link_arena_bind(&alloc, take->link);
  Ra8__Mdl__Cancelled* msg = ra8__mdl__cancelled__unpack(&alloc, len, data);
  if (msg == nullptr) {
    return k_ra8_err_protocol_error;
  }
  const mdl_cancelled_view_t view = {
    .protocol_version = msg->protocol_version,
    .job_id           = msg->job_id,
    .status           = msg->status,
    .unknown_fields   = (uint32_t)msg->base.n_unknown_fields,
  };
  const bool valid = priv_c6link_mdl_cancelled_valid(&view, take->session);
  if (valid) {
    priv_c6link_mdl_session_deactivate(take->session);
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
  if (body == nullptr) {
    return k_ra8_err_protocol_error;
  }
  const ra8_err_t remote = priv_c6link_resp(take->link, take->operation, body->resp);
  if (remote != k_ra8_ok) {
    return remote;
  }
  const mdl_envelope_view_t view = {
    .custom_msg_id = body->custom_msg_id,
    .operation     = take->operation,
    .body_len      = body->data.len,
    .body_present  = (body->data.data != nullptr),
  };
  const uint8_t selected = priv_c6link_mdl_take_selected(&view, (uint8_t)take->kind);
  if (selected == 0U) {
    return k_ra8_err_protocol_error;
  }
  switch ((mdl_take_kind_t)selected) {
    case k_mdl_take_accepted:
      return internal_mdl_take_accepted(take, &body->data);
    case k_mdl_take_chunk:
      return internal_mdl_take_chunk(take, &body->data);
    case k_mdl_take_cancelled:
      return internal_mdl_take_cancelled(take, body->data.data, body->data.len);
    default:
      return k_ra8_err_protocol_error;
  }
}

/* protobuf-c's pack-only binary-data ABI still declares its byte pointer
 * mutable. */
// NOLINTBEGIN(readability-non-const-parameter) -- interface contract fixes this writable pointer type.
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

RA8_TEST_HELPER ra8_err_t ra8_c6link_mdl_take_cancelled_test(ra8_c6link_t*      link,
                                                             ra8_mdl_session_t* session,
                                                             const uint8_t*     packed,
                                                             size_t             len)
{
  mdl_take_ctx_t take = {.link = link, .session = session, .kind = k_mdl_take_cancelled};
  return internal_mdl_take_cancelled(&take, packed, len);
}

RA8_TEST_HELPER bool ra8_c6link_mdl_http_field_valid_test(const char* text, size_t cap)
{
  return priv_c6link_mdl_http_field_valid(text, cap);
}

RA8_TEST_HELPER bool ra8_c6link_mdl_http_response_valid_test(const Ra8__Mdl__Chunk* msg)
{
  const mdl_chunk_view_t view = internal_mdl_view(msg);
  return priv_c6link_mdl_http_response_valid(&view);
}

RA8_TEST_HELPER bool ra8_c6link_mdl_chunk_semantics_valid_test(const Ra8__Mdl__Chunk* msg)
{
  const mdl_chunk_view_t view = internal_mdl_view(msg);
  return priv_c6link_mdl_chunk_semantics_valid(&view);
}

ra8_err_t ra8_c6link_mdl_start_request(ra8_c6link_t*            link,
                                       const ra8_mdl_request_t* request,
                                       ra8_mdl_session_t*       session)
{
  if ((link == nullptr) || (session == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  size_t          url_len = 0U;
  const ra8_err_t valid   = priv_c6link_mdl_start_request_valid(request, &url_len);
  if (valid != k_ra8_ok) {
    return valid;
  }
  *session = (ra8_mdl_session_t){};
  uint8_t* const data = link->mdl_request;
  size_t         packed = 0U;
  if (!priv_c6link_mdl_encode_start(request, url_len, data, sizeof(link->mdl_request), &packed)) {
    return k_ra8_err_invalid_size;
  }
  mdl_take_ctx_t take = {.session          = session,
                         .requested_format = request->format,
                         .kind             = k_mdl_take_accepted};
  return internal_mdl_call(link, k_ra8_mdl_rpc_start, data, packed, &take);
}

ra8_err_t ra8_c6link_mdl_start(ra8_c6link_t*      link,
                               const char*        url,
                               mdl_format_t       format,
                               ra8_mdl_session_t* session)
{
  const ra8_mdl_request_t request = {.url = url, .format = format};
  return ra8_c6link_mdl_start_request(link, &request, session);
}

ra8_err_t ra8_c6link_mdl_next(ra8_c6link_t*      link,
                              ra8_mdl_session_t* session,
                              uint16_t           max_bytes,
                              ra8_mdl_chunk_t*   chunk)
{
  if ((link == nullptr) || (session == nullptr) || (chunk == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  const ra8_err_t allowed = priv_c6link_mdl_next_allowed(session, max_bytes);
  if (allowed != k_ra8_ok) {
    return allowed;
  }
  *chunk                = (ra8_mdl_chunk_t){};
  uint8_t* const data   = link->mdl_request;
  size_t         packed = 0U;
  if (!priv_c6link_mdl_encode_next(session->job_id,
                                   session->next_offset,
                                   max_bytes,
                                   data,
                                   sizeof(link->mdl_request),
                                   &packed)) {
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
  const ra8_err_t allowed = priv_c6link_mdl_cancel_allowed(session);
  if (allowed != k_ra8_ok) {
    return allowed;
  }
  uint8_t* const data   = link->mdl_request;
  size_t         packed = 0U;
  if (!priv_c6link_mdl_encode_cancel(session->job_id, data, sizeof(link->mdl_request), &packed)) {
    return k_ra8_err_invalid_size;
  }
  mdl_take_ctx_t take = {.session = session, .kind = k_mdl_take_cancelled};
  return internal_mdl_call(link, k_ra8_mdl_rpc_cancel, data, packed, &take);
}
