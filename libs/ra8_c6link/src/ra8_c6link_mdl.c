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
 * @brief Validate one optional HTTP field against its protocol bound.
 * @details Treats null as absent and rejects CR/LF header injection.
 * @param[in] text Optional NUL-terminated field.
 * @param[in] cap Maximum extent including NUL.
 * @return Field validity.
 * @retval true Field is absent or bounded and single-line.
 * @retval false Field is unterminated, too large, or contains CR/LF.
 * @pre @p cap is nonzero.
 * @pre Non-null @p text is readable through its first NUL or @p cap bytes.
 * @post No input or global state is modified.
 * @post True guarantees the field can be encoded within its fixed bound.
 * @note Pure and reentrant.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_mdl_http_field_valid(const char* text, size_t cap)
{
  if (text == nullptr) {
    return true;
  }
  const size_t length = strnlen(text, cap);
  if (length >= cap) {
    return false;
  }
  for (size_t index = 0U; index < length; index++) {
    if ((text[index] == '\r') || (text[index] == '\n')) {
      return false;
    }
  }
  return true;
}

/**
 * @brief Validate terminal HTTP metadata carried by a generated Chunk.
 * @details Requires a real status only on COMPLETE and bounds every selected
 * response header before any caller copy.
 * @param[in] msg Decoded generated chunk.
 * @return Metadata validity.
 * @retval true Metadata matches the chunk state and all string bounds.
 * @retval false Status, presence, termination, or a header bound is invalid.
 * @pre @p msg is non-null and owns decoded string pointers.
 * @pre The protobuf arena remains live for the complete call.
 * @post No decoded or caller-owned state is modified.
 * @post True authorizes bounded response-header copies.
 * @note Pure and reentrant for independent messages.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_mdl_http_response_valid(const Ra8__Mdl__Chunk* msg)
{
  const bool empty = (msg->retry_after != nullptr) && (msg->retry_after[0] == '\0') &&
                     (msg->etag != nullptr) && (msg->etag[0] == '\0') &&
                     (msg->last_modified != nullptr) && (msg->last_modified[0] == '\0') &&
                     (msg->content_type != nullptr) && (msg->content_type[0] == '\0');
  if (msg->state != RA8__MDL__STATE__STATE_COMPLETE) {
    return (msg->http_status == 0) && empty;
  }
  return (msg->http_status >= (int32_t)k_ra8_mdl_http_status_min) &&
         (msg->http_status <= (int32_t)k_ra8_mdl_http_status_max) &&
         internal_mdl_http_field_valid(msg->retry_after, k_ra8_mdl_retry_after_max) &&
         internal_mdl_http_field_valid(msg->etag, k_ra8_mdl_etag_max) &&
         internal_mdl_http_field_valid(msg->last_modified, k_ra8_mdl_http_date_max) &&
         internal_mdl_http_field_valid(msg->content_type, k_ra8_mdl_content_type_max);
}

/**
 * @brief Copy one already-bounded HTTP field into fixed response storage.
 * @details Copies through the validated terminator so the public response
 * retains the exact header spelling selected by the C6 service.
 * @param[out] destination Fixed caller response array.
 * @param[in] source Validated NUL-terminated decoded field.
 * @param[in] cap Capacity of @p destination and validated source bound.
 * @pre Pointers are non-null and @p source terminates before @p cap.
 * @pre Source and destination do not overlap.
 * @post Destination contains one exact NUL-terminated copy.
 * @post Bytes beyond the terminator are unchanged.
 * @note Caller validation makes this helper infallible.
 * @since 0.1.0
 */
RA8_INTERNAL static void
internal_mdl_http_field_copy(char* destination, const char* source, size_t cap)
{
  const size_t length = strnlen(source, cap);
  memcpy(destination, source, length + 1U);
}

/**
 * @brief Copy one validated generated chunk and advance its active session
 * @details Translates generated state and metadata into the allocation-free
 * client contract after correlation and capacity checks have succeeded.
 * @param[in,out] take Active extraction and session context.
 * @param[in] msg Fully validated generated Chunk.
 * @return Remote terminal status.
 * @retval k_ra8_ok Data, completion, or cancellation was accepted.
 * @retval other The exact nonzero FAILED status supplied by the remote service.
 * @pre @p take and @p msg are non-null and correlation validation succeeded.
 * @pre Generated byte/string spans remain live for the complete call.
 * @post Caller output contains the exact bounded decoded fields.
 * @post Session correlation advances once and terminal state deactivates it.
 * @note This helper performs no protobuf allocation or release.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_mdl_accept_chunk(mdl_take_ctx_t*        take,
                                                        const Ra8__Mdl__Chunk* msg)
{
  *take->chunk = (ra8_mdl_chunk_t){.job_id      = msg->job_id,
                                   .sequence    = msg->sequence,
                                   .offset      = msg->offset,
                                   .total_bytes = msg->total_bytes,
                                   .state       = (ra8_mdl_state_t)msg->state,
                                   .status      = (ra8_err_t)msg->status,
                                   .data_len    = (uint16_t)msg->data.len,
                                   .has_sha256  = (msg->sha256.len == k_ra8_mdl_sha256_bytes)};
  if (msg->data.len != 0U) {
    memcpy(take->chunk->data, msg->data.data, msg->data.len);
  }
  if (take->chunk->has_sha256) {
    memcpy(take->chunk->sha256, msg->sha256.data, k_ra8_mdl_sha256_bytes);
  }
  if (msg->state == RA8__MDL__STATE__STATE_COMPLETE) {
    take->chunk->response.status = msg->http_status;
    internal_mdl_http_field_copy(take->chunk->response.retry_after,
                                 msg->retry_after,
                                 sizeof(take->chunk->response.retry_after));
    internal_mdl_http_field_copy(take->chunk->response.etag,
                                 msg->etag,
                                 sizeof(take->chunk->response.etag));
    internal_mdl_http_field_copy(take->chunk->response.last_modified,
                                 msg->last_modified,
                                 sizeof(take->chunk->response.last_modified));
    internal_mdl_http_field_copy(take->chunk->response.content_type,
                                 msg->content_type,
                                 sizeof(take->chunk->response.content_type));
  }
  take->session->next_offset += msg->data.len;
  take->session->next_sequence += 1U;
  if ((msg->state == RA8__MDL__STATE__STATE_COMPLETE) ||
      (msg->state == RA8__MDL__STATE__STATE_CANCELLED) ||
      (msg->state == RA8__MDL__STATE__STATE_FAILED)) {
    take->session->active = false;
  }
  return (msg->state == RA8__MDL__STATE__STATE_FAILED) ? (ra8_err_t)msg->status : k_ra8_ok;
}

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
  if (!internal_mdl_http_response_valid(msg)) {
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
  const ra8_err_t result = valid ? internal_mdl_accept_chunk(take, msg) : k_ra8_err_protocol_error;
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
  // mcdc-deactivated: internal_mdl_take_response empty-body guard; the length operand is constant-false whenever it is evaluated. protobuf-c's unpack sets a bytes field's `data` pointer to NULL for every zero-length field (`else { bd->data = NULL; }` in parse_required_member), so a decoded body of length zero always arrives with a null pointer and is consumed by the first operand; reaching the second requires a non-null pointer, which the same code only produces when `len > 0`. A modelled co-processor that puts a present but empty body on the wire -- exercised by test_ra8_c6link_mdl_decode.c -- decodes to (null, 0) like an omitted one.
  if ((body->data.data == nullptr) || (body->data.len == 0U)) {
    return k_ra8_err_protocol_error;
  }
  switch (take->kind) {
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
  return internal_mdl_http_field_valid(text, cap);
}

RA8_TEST_HELPER bool ra8_c6link_mdl_http_response_valid_test(const Ra8__Mdl__Chunk* msg)
{
  return internal_mdl_http_response_valid(msg);
}

RA8_TEST_HELPER bool ra8_c6link_mdl_chunk_semantics_valid_test(const Ra8__Mdl__Chunk* msg)
{
  return internal_mdl_chunk_semantics_valid(msg);
}

/**
 * @brief Validate every caller-supplied start-request field before staging.
 *
 * @details
 * Split out of `ra8_c6link_mdl_start_request()` so the argument contract and
 * the wire encoding are separately reviewable and each stays inside the NASA
 * Power of 10 Rule 4 length cap. The validation checks and their order are
 * unchanged; the null-pointer guards are split between the caller and this
 * helper, and every one of them still yields `k_ra8_err_null_ptr`.
 *
 * @param[in] request Caller request; may be null.
 * @param[out] out_url_len Receives the validated URL length on success, so the
 *                        caller copies exactly the length that was bounded here
 *                        rather than re-deriving it from caller-owned memory.
 *                        The single call site passes the address of a local, so
 *                        this pointer is not re-checked here.
 * @return Validation status.
 * @retval k_ra8_ok Every field satisfies the documented contract.
 * @retval k_ra8_err_null_ptr @p request or its URL is null.
 * @retval k_ra8_err_invalid_arg A field is out of range or malformed.
 * @pre The caller has already rejected a null link and session.
 * @pre @p out_url_len is non-null.
 * @pre @p request is readable for the whole structure.
 * @post @p out_url_len holds the bounded URL length on success only.
 * @post The return value is one of the documented retvals.
 * @note Not thread-safe for a shared request structure.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_mdl_start_request_valid(const ra8_mdl_request_t* request,
                                                               size_t*                  out_url_len)
{
  if ((request == nullptr) || (request->url == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  const size_t https_prefix_len = sizeof("https://") - 1U;
  const size_t url_len          = strnlen(request->url, k_ra8_mdl_url_max);
  if ((url_len == 0U) || (url_len >= k_ra8_mdl_url_max) ||
      (strncmp(request->url, "https://", https_prefix_len) != 0) ||
      (request->url[https_prefix_len] == '\0')) {
    return k_ra8_err_invalid_arg;
  }
  if (((uint32_t)request->format > (uint32_t)k_mdl_format_rabook) ||
      (request->http.timeout_ms > k_ra8_mdl_timeout_ms_max) ||
      !internal_mdl_http_field_valid(request->http.user_agent, k_ra8_mdl_user_agent_max) ||
      !internal_mdl_http_field_valid(request->http.referer, k_ra8_mdl_referer_max) ||
      !internal_mdl_http_field_valid(request->http.if_none_match, k_ra8_mdl_etag_max) ||
      !internal_mdl_http_field_valid(request->http.if_modified_since, k_ra8_mdl_http_date_max)) {
    return k_ra8_err_invalid_arg;
  }
  *out_url_len = url_len;
  return k_ra8_ok;
}

/**
 * @struct mdl_http_headers_t
 * @brief Fixed-capacity storage for the four optional MDL HTTP request headers.
 * @details Each buffer is sized by its own protocol maximum and zero-initialized,
 *          so an absent header is transmitted as an empty string rather than as
 *          a dangling pointer into caller memory.
 * @invariant Every member is NUL-terminated for its whole declared capacity.
 * @see internal_mdl_stage_headers()
 * @since 0.1.0
 */
typedef struct {
  char user_agent[k_ra8_mdl_user_agent_max];       /**< User-Agent staging.        */
  char referer[k_ra8_mdl_referer_max];             /**< Referer staging.           */
  char if_none_match[k_ra8_mdl_etag_max];          /**< If-None-Match staging.     */
  char if_modified_since[k_ra8_mdl_http_date_max]; /**< If-Modified-Since staging. */
} mdl_http_headers_t;

/**
 * @brief Copy every present optional HTTP header into bounded local storage.
 *
 * @details
 * Split out of `ra8_c6link_mdl_start_request()` so that entry point stays under
 * the reviewed statement-count threshold. Each field was already length-checked
 * by `internal_mdl_start_request_valid()`, so each copy is bounded by its own
 * protocol maximum; an absent field keeps the zero-initialized empty string.
 *
 * @param[in] http Caller-supplied optional headers; individual members may be null.
 * @param[out] out Zero-initialized staging storage to fill.
 * @return Nothing.
 * @pre @p http and @p out are non-null.
 * @pre Every non-null member of @p http fits its protocol maximum.
 * @post Every present member is copied and NUL-terminated in @p out.
 * @post Absent members are left as the caller's zero initialization.
 * @note Not thread-safe for a shared @p out.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_mdl_stage_headers(const ra8_mdl_http_policy_t* http,
                                                    mdl_http_headers_t*          out)
{
  if (http->user_agent != nullptr) {
    memcpy(out->user_agent, http->user_agent, strlen(http->user_agent) + 1U);
  }
  if (http->referer != nullptr) {
    memcpy(out->referer, http->referer, strlen(http->referer) + 1U);
  }
  if (http->if_none_match != nullptr) {
    memcpy(out->if_none_match, http->if_none_match, strlen(http->if_none_match) + 1U);
  }
  if (http->if_modified_since != nullptr) {
    memcpy(out->if_modified_since, http->if_modified_since, strlen(http->if_modified_since) + 1U);
  }
}

ra8_err_t ra8_c6link_mdl_start_request(ra8_c6link_t*            link,
                                       const ra8_mdl_request_t* request,
                                       ra8_mdl_session_t*       session)
{
  if ((link == nullptr) || (session == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  size_t          url_len = 0U;
  const ra8_err_t valid   = internal_mdl_start_request_valid(request, &url_len);
  if (valid != k_ra8_ok) {
    return valid;
  }
  *session = (ra8_mdl_session_t){};
  char url_copy[k_ra8_mdl_url_max];
  memcpy(url_copy, request->url, url_len + 1U);
  mdl_http_headers_t headers = {};
  internal_mdl_stage_headers(&request->http, &headers);
  Ra8__Mdl__StartRequest inner;
  ra8__mdl__start_request__init(&inner);
  inner.protocol_version  = k_ra8_mdl_protocol_version;
  inner.url               = url_copy;
  inner.format            = (Ra8__Mdl__Format)request->format;
  inner.user_agent        = headers.user_agent;
  inner.referer           = headers.referer;
  inner.if_none_match     = headers.if_none_match;
  inner.if_modified_since = headers.if_modified_since;
  inner.timeout_ms        = request->http.timeout_ms;
  uint8_t* const data     = link->mdl_request;
  const size_t   packed   = ra8__mdl__start_request__get_packed_size(&inner);
  // mcdc-deactivated: ra8_c6link_mdl_start_request codec self-consistency guard; all three conditions are constant-false on every reachable path, so this is a fail-closed backstop against a codec defect rather than an input class. get_packed_size() counts a message whose protocol_version is the non-zero k_ra8_mdl_protocol_version, so it never reports 0; k_ra8_mdl_request_bytes_max is the exact sum of every bounded field plus 96 bytes of tag and varint headroom, and each field was bounded before this point, so the packed message cannot exceed link->mdl_request; and protobuf-c pack() returns exactly what get_packed_size() computed for the same message.
  if ((packed == 0U) || (packed > sizeof(link->mdl_request)) ||
      (ra8__mdl__start_request__pack(&inner, data) != packed)) {
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
  uint8_t* const data         = link->mdl_request;
  const size_t   packed       = ra8__mdl__next_request__get_packed_size(&inner);
  // mcdc-deactivated: ra8_c6link_mdl_next codec self-consistency guard; all three conditions are constant-false on every reachable path, so this is a fail-closed backstop against a codec defect rather than an input class. get_packed_size() counts a message whose protocol_version is the non-zero k_ra8_mdl_protocol_version, so it never reports 0; k_ra8_mdl_request_bytes_max is the exact sum of every bounded field plus 96 bytes of tag and varint headroom, and each field was bounded before this point, so the packed message cannot exceed link->mdl_request; and protobuf-c pack() returns exactly what get_packed_size() computed for the same message.
  if ((packed == 0U) || (packed > sizeof(link->mdl_request)) ||
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
  uint8_t* const data           = link->mdl_request;
  const size_t   packed         = ra8__mdl__cancel_request__get_packed_size(&inner);
  // mcdc-deactivated: ra8_c6link_mdl_cancel codec self-consistency guard; all three conditions are constant-false on every reachable path, so this is a fail-closed backstop against a codec defect rather than an input class. get_packed_size() counts a message whose protocol_version is the non-zero k_ra8_mdl_protocol_version, so it never reports 0; k_ra8_mdl_request_bytes_max is the exact sum of every bounded field plus 96 bytes of tag and varint headroom, and each field was bounded before this point, so the packed message cannot exceed link->mdl_request; and protobuf-c pack() returns exactly what get_packed_size() computed for the same message.
  if ((packed == 0U) || (packed > sizeof(link->mdl_request)) ||
      (ra8__mdl__cancel_request__pack(&inner, data) != packed)) {
    return k_ra8_err_invalid_size;
  }
  mdl_take_ctx_t take = {.session = session, .kind = k_mdl_take_cancelled};
  return internal_mdl_call(link, k_ra8_mdl_rpc_cancel, data, packed, &take);
}
