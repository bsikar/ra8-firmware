/**
 * @file ra8_c6link_mdl_service.c
 * @brief Portable one-job media service state machine for the ESP32-C6 port.
 * @details Decodes one bounded request at a time, delegates body I/O to an
 * injected backend, and packs transactional responses without heap allocation.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ra8_attributes.h"
#include "ra8_c6link_mdl_msg.h"
#include "ra8_c6link_mdl_service_internal.h"
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

/** @brief Decode arena sized for the bounded start request and URL. */
typedef enum : uint16_t {
  k_mdl_decode_arena_bytes = k_ra8_mdl_url_max + k_ra8_mdl_user_agent_max + k_ra8_mdl_referer_max +
                             k_ra8_mdl_etag_max + k_ra8_mdl_http_date_max +
                             512U,                    /**< Per-dispatch arena size. */
  k_mdl_decode_align       = 8U,                      /**< Arena allocation alignment. */
  k_mdl_decode_align_mask  = k_mdl_decode_align - 1U, /**< Mask used to round sizes.   */
} mdl_svc_const_t;

/**
 * @brief Validate one decoded optional request header.
 * @details Requires bounded single-line text so a remote request cannot inject
 * additional ESP-IDF headers.
 * @param[in] text Decoded protobuf string.
 * @param[in] cap Maximum extent including NUL.
 * @return Header validity.
 * @retval true Text terminates before @p cap and contains no CR/LF.
 * @retval false Pointer, bound, termination, or line discipline is invalid.
 * @pre @p cap is nonzero.
 * @pre Non-null @p text belongs to the current decode arena.
 * @post No decoded or service state is modified.
 * @post True authorizes passing the string to the backend.
 * @note Empty strings are valid and mean header absent.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_mdl_request_field_valid(const char* text, size_t cap)
{
  if (text == nullptr) {
    return false;
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
 * @brief Validate fixed terminal response metadata returned by a backend.
 * @param[in] response Candidate status and selected headers.
 * @return Response validity.
 * @retval true Status is HTTP-shaped and every array is bounded single-line text.
 * @retval false Status or a selected header violates the protocol contract.
 * @pre @p response is non-null and fully initialized by the backend.
 * @pre Every array is readable for its declared extent.
 * @post No response or service state is modified.
 * @post True authorizes protobuf packing of every selected header.
 * @note Pure and reentrant.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_mdl_response_valid(const ra8_mdl_http_response_t* response)
{
  return (response->status >= 100) && (response->status <= 599) &&
         internal_mdl_request_field_valid(response->retry_after, sizeof(response->retry_after)) &&
         internal_mdl_request_field_valid(response->etag, sizeof(response->etag)) &&
         internal_mdl_request_field_valid(response->last_modified,
                                          sizeof(response->last_modified)) &&
         internal_mdl_request_field_valid(response->content_type, sizeof(response->content_type));
}

/** @brief Linear allocator used by protobuf-c; reset after every dispatch. */
typedef struct {
  uint8_t bytes[k_mdl_decode_arena_bytes]; /**< Fixed protobuf decode storage. */
  size_t  used;                            /**< Bytes already allocated.       */
} mdl_decode_arena_t;

RA8_PRIV bool priv_c6link_mdl_decode_allocation_fits(size_t used, size_t len, size_t capacity)
{
  if (len > (SIZE_MAX - k_mdl_decode_align_mask)) {
    return false;
  }
  const size_t aligned = (len + k_mdl_decode_align_mask) & ~(size_t)k_mdl_decode_align_mask;
  return (aligned <= capacity) && (used <= (capacity - aligned));
}

/**
 * @brief Allocate one aligned span from a per-dispatch bounded arena
 * @param[in,out] data ::mdl_decode_arena_t owned by the current dispatch.
 * @param[in] len Requested bytes.
 * @return Allocated span or null when the arena is exhausted.
 * @retval nullptr The aligned request exceeds remaining arena capacity.
 * @pre @p data is non-null and exclusively owned.
 * @pre `used` is no larger than the arena byte capacity.
 * @post Success advances `used` by the aligned size.
 * @post Failure leaves `used` unchanged.
 * @note Reentrant for independent arenas.
 * @since 0.1.0
 */
RA8_INTERNAL static void* internal_mdl_decode_alloc(void* data, size_t len)
{
  mdl_decode_arena_t* arena = (mdl_decode_arena_t*)data;
  if (!priv_c6link_mdl_decode_allocation_fits(arena->used, len, sizeof(arena->bytes))) {
    return nullptr;
  }
  const size_t aligned = (len + k_mdl_decode_align_mask) & ~(size_t)k_mdl_decode_align_mask;
  void*        out     = &arena->bytes[arena->used];
  arena->used += aligned;
  return out;
}

/**
 * @brief Ignore individual protobuf frees for the linear dispatch arena
 * @details The complete arena is discarded when dispatch returns.
 * @param[in] data Arena context retained for allocator ABI compatibility.
 * @param[in] ptr Previously returned span retained for allocator ABI
 * compatibility.
 * @pre @p data identifies the current dispatch arena.
 * @pre @p ptr is null or belongs to that arena.
 * @post Arena state is unchanged.
 * @post No system allocator is invoked.
 * @note Reentrant for independent arenas.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_mdl_decode_free(void* data, void* ptr)
{
  (void)data;
  (void)ptr;
}

/**
 * @brief Reject a response that cannot fit before invoking the backend
 * @details Centralises the no-partial-pack capacity contract.
 * @param[in] len Required packed bytes.
 * @param[in] response_cap Caller-owned response capacity.
 * @return Capacity status.
 * @retval k_ra8_ok The complete response fits.
 * @retval k_ra8_err_invalid_size Length is zero or exceeds capacity.
 * @pre Both values are expressed in bytes.
 * @pre No backend side effect has occurred for the candidate response.
 * @post No state or output buffer is modified.
 * @post Success guarantees a bounded pack is possible.
 * @note Pure and thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_mdl_check_response_size(size_t len, size_t response_cap)
{
  if ((len == 0U) || (len > response_cap)) {
    return k_ra8_err_invalid_size;
  }
  return k_ra8_ok;
}

/**
 * @brief Pack an Accepted response into a bounded caller buffer
 * @details Verifies encoded size before invoking the generated packer.
 * @param[in] msg Valid generated Accepted message.
 * @param[out] response Caller-owned packed bytes.
 * @param[in] response_cap Capacity of @p response.
 * @param[out] response_len Exact packed length.
 * @return Packing status.
 * @retval k_ra8_ok Response was packed exactly.
 * @retval k_ra8_err_invalid_size Response does not fit.
 * @retval k_ra8_err_validation_failed Codec length and write disagree.
 * @pre Every pointer is non-null.
 * @pre @p response_len is writable and response spans do not overlap @p msg.
 * @post Success sets @p response_len within capacity.
 * @post Failure does not report a successful length.
 * @note Reentrant for independent buffers.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_mdl_pack_accepted(const Ra8__Mdl__Accepted* msg,
                                                         uint8_t*                  response,
                                                         size_t                    response_cap,
                                                         size_t*                   response_len)
{
  const size_t    len      = ra8__mdl__accepted__get_packed_size(msg);
  const ra8_err_t capacity = internal_mdl_check_response_size(len, response_cap);
  if (capacity != k_ra8_ok) {
    return capacity;
  }
  if (ra8__mdl__accepted__pack(msg, response) != len) {
    return k_ra8_err_validation_failed;
  }
  *response_len = len;
  return k_ra8_ok;
}

/**
 * @brief Pack a Chunk response into a bounded caller buffer
 * @details Verifies encoded size before invoking the generated packer.
 * @param[in] msg Valid generated Chunk message.
 * @param[out] response Caller-owned packed bytes.
 * @param[in] response_cap Capacity of @p response.
 * @param[out] response_len Exact packed length.
 * @return Packing status.
 * @retval k_ra8_ok Response was packed exactly.
 * @retval k_ra8_err_invalid_size Response does not fit.
 * @retval k_ra8_err_validation_failed Codec length and write disagree.
 * @pre Every pointer is non-null.
 * @pre @p response_len is writable and response spans do not overlap @p msg.
 * @post Success sets @p response_len within capacity.
 * @post Failure does not report a successful length.
 * @note Reentrant for independent buffers.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_mdl_pack_chunk(const Ra8__Mdl__Chunk* msg,
                                                      uint8_t*               response,
                                                      size_t                 response_cap,
                                                      size_t*                response_len)
{
  const size_t    len      = ra8__mdl__chunk__get_packed_size(msg);
  const ra8_err_t capacity = internal_mdl_check_response_size(len, response_cap);
  if (capacity != k_ra8_ok) {
    return capacity;
  }
  if (ra8__mdl__chunk__pack(msg, response) != len) {
    return k_ra8_err_validation_failed;
  }
  *response_len = len;
  return k_ra8_ok;
}

/**
 * @brief Validate and begin one typed-artifact service job
 * @details Pre-packs the bounded response before allowing backend side effects.
 * @param[in,out] service Initialised portable service.
 * @param[in,out] alloc Bounded per-dispatch protobuf allocator.
 * @param[in] request Packed StartRequest.
 * @param[in] request_len Valid request bytes.
 * @param[out] response Caller-owned Accepted bytes.
 * @param[in] response_cap Response capacity.
 * @param[out] response_len Packed response length.
 * @return Start status.
 * @retval k_ra8_ok Backend accepted a correlated job.
 * @retval k_ra8_err_protocol_error Decode failed or unknown fields were
 * present.
 * @retval k_ra8_err_invalid_arg Version or URL is invalid.
 * @retval k_ra8_err_busy A job is already active.
 * @pre All pointers are non-null and service access is exclusive.
 * @pre @p alloc owns a fresh bounded arena.
 * @post Success activates exactly one non-zero job id.
 * @post Backend failure leaves the service inactive and response length zero.
 * @note Not thread-safe for a shared service.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_mdl_dispatch_start(ra8_mdl_service_t*  service,
                                                          ProtobufCAllocator* alloc,
                                                          const uint8_t*      request,
                                                          size_t              request_len,
                                                          uint8_t*            response,
                                                          size_t              response_cap,
                                                          size_t*             response_len)
{
  Ra8__Mdl__StartRequest* req = ra8__mdl__start_request__unpack(alloc, request_len, request);
  if (req == nullptr) {
    return k_ra8_err_protocol_error;
  }
  if (req->base.n_unknown_fields != 0U) {
    return k_ra8_err_protocol_error;
  }
  if (req->url == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  const size_t https_prefix_len = sizeof("https://") - 1U;
  const size_t url_len          = strnlen(req->url, k_ra8_mdl_url_max);
  const bool   valid =
    (req->protocol_version == k_ra8_mdl_protocol_version) && (url_len != 0U) &&
    (url_len < k_ra8_mdl_url_max) && (strncmp(req->url, "https://", https_prefix_len) == 0) &&
    (req->url[https_prefix_len] != '\0') &&
    ((uint32_t)req->format <= (uint32_t)RA8__MDL__FORMAT__FORMAT_RABOOK) &&
    (req->timeout_ms <= k_ra8_mdl_timeout_ms_max) &&
    internal_mdl_request_field_valid(req->user_agent, k_ra8_mdl_user_agent_max) &&
    internal_mdl_request_field_valid(req->referer, k_ra8_mdl_referer_max) &&
    internal_mdl_request_field_valid(req->if_none_match, k_ra8_mdl_etag_max) &&
    internal_mdl_request_field_valid(req->if_modified_since, k_ra8_mdl_http_date_max);
  if (!valid) {
    return k_ra8_err_invalid_arg;
  }
  if (service->active) {
    return k_ra8_err_busy;
  }
  uint32_t next_job_id = service->next_job_id + 1U;
  if (next_job_id == 0U) {
    next_job_id = 1U;
  }
  Ra8__Mdl__Accepted out = RA8__MDL__ACCEPTED__INIT;
  out.protocol_version   = k_ra8_mdl_protocol_version;
  out.job_id             = next_job_id;
  out.max_chunk_bytes    = k_ra8_mdl_chunk_data_max;
  out.format             = req->format;
  const ra8_err_t prepacked =
    internal_mdl_pack_accepted(&out, response, response_cap, response_len);
  if (prepacked != k_ra8_ok) {
    return prepacked;
  }
  const ra8_mdl_request_t backend_request = {
    .url    = req->url,
    .format = (ra8_mdl_format_t)req->format,
    .http   = {.user_agent        = req->user_agent,
               .referer           = req->referer,
               .if_none_match     = req->if_none_match,
               .if_modified_since = req->if_modified_since,
               .timeout_ms        = req->timeout_ms},
  };
  const ra8_err_t begun = service->backend.begin(service->backend.ctx, &backend_request);
  if (begun != k_ra8_ok) {
    *response_len = 0U;
    return begun;
  }
  service->next_job_id   = next_job_id;
  service->active_job_id = service->next_job_id;
  service->next_sequence = 0U;
  service->next_offset   = 0U;
  service->active_format = (ra8_mdl_format_t)req->format;
  service->active        = true;
  return k_ra8_ok;
}

/** @brief Caller-bounded bytes and metadata returned by one backend pull. */
typedef struct internal_mdl_next_read_t {
  uint8_t                 bytes[k_ra8_mdl_chunk_data_max]; /**< Returned body bytes.        */
  uint8_t                 digest[k_ra8_mdl_sha256_bytes];  /**< Terminal SHA-256 digest.    */
  uint16_t                got;                             /**< Valid body byte count.      */
  uint64_t                total;                           /**< Declared complete size.     */
  uint64_t                end_offset;                      /**< Offset after returned data. */
  bool                    complete;                        /**< Whether this is terminal.   */
  ra8_mdl_http_response_t response;                        /**< Terminal HTTP metadata.     */
} internal_mdl_next_read_t;

/**
 * @brief Cancel and deactivate one backend job after a terminal error.
 * @details Best-effort cancellation prevents another pull from observing a
 * partially advanced backend after the service detects a terminal failure.
 * @param[in,out] service Active portable service context.
 * @param[in] error Canonical terminal status to preserve.
 * @return The unchanged caller-supplied terminal status.
 * @retval error The exact value supplied in @p error.
 * @pre @p service is non-null and owns a bound backend.
 * @pre The backend job is active or may safely accept idempotent cancellation.
 * @post The backend cancellation callback was attempted exactly once.
 * @post `service->active` is false regardless of cancellation status.
 * @note Cancellation errors cannot replace the protocol/backend root cause.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_mdl_fail_job(ra8_mdl_service_t* service, ra8_err_t error)
{
  (void)service->backend.cancel(service->backend.ctx);
  service->active = false;
  return error;
}

/**
 * @brief Prove the worst-case Chunk for one request fits before backend I/O.
 * @details Measures a maximally encoded response using caller-requested data
 * length and worst-case protobuf varints and digest fields.
 * @param[in] max_data Maximum body bytes permitted for this pull.
 * @param[in] response_cap Capacity of the caller's packed-response buffer.
 * @return Canonical capacity status.
 * @retval k_ra8_ok Every legal response for this pull fits.
 * @retval k_ra8_err_invalid_size Worst-case packed response exceeds capacity.
 * @pre @p max_data does not exceed ::k_ra8_mdl_chunk_data_max.
 * @pre @p response_cap is the actual writable response capacity.
 * @post No backend callback is invoked.
 * @post Service and caller buffers are unchanged.
 * @note Local arrays provide addresses required by protobuf sizing only.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_mdl_next_capacity(uint32_t max_data, size_t response_cap)
{
  uint8_t max_bytes[k_ra8_mdl_chunk_data_max];
  uint8_t max_digest[k_ra8_mdl_sha256_bytes];
  char    retry_after[k_ra8_mdl_retry_after_max];
  char    etag[k_ra8_mdl_etag_max];
  char    last_modified[k_ra8_mdl_http_date_max];
  char    content_type[k_ra8_mdl_content_type_max];
  memset(retry_after, 'x', sizeof(retry_after) - 1U);
  memset(etag, 'x', sizeof(etag) - 1U);
  memset(last_modified, 'x', sizeof(last_modified) - 1U);
  memset(content_type, 'x', sizeof(content_type) - 1U);
  retry_after[sizeof(retry_after) - 1U]     = '\0';
  etag[sizeof(etag) - 1U]                   = '\0';
  last_modified[sizeof(last_modified) - 1U] = '\0';
  content_type[sizeof(content_type) - 1U]   = '\0';
  Ra8__Mdl__Chunk data                      = RA8__MDL__CHUNK__INIT;
  data.protocol_version                     = UINT32_MAX;
  data.job_id                               = UINT32_MAX;
  data.sequence                             = UINT32_MAX;
  data.offset                               = UINT64_MAX;
  data.data        = (ProtobufCBinaryData){.len = max_data, .data = max_bytes};
  data.total_bytes = UINT64_MAX;
  data.state       = RA8__MDL__STATE__STATE_DOWNLOADING;
  const ra8_err_t data_fit =
    internal_mdl_check_response_size(ra8__mdl__chunk__get_packed_size(&data), response_cap);
  if (data_fit != k_ra8_ok) {
    return data_fit;
  }

  Ra8__Mdl__Chunk terminal  = RA8__MDL__CHUNK__INIT;
  terminal.protocol_version = UINT32_MAX;
  terminal.job_id           = UINT32_MAX;
  terminal.sequence         = UINT32_MAX;
  terminal.offset           = UINT64_MAX;
  terminal.total_bytes      = UINT64_MAX;
  terminal.state            = RA8__MDL__STATE__STATE_COMPLETE;
  terminal.sha256           = (ProtobufCBinaryData){.len = sizeof(max_digest), .data = max_digest};
  terminal.http_status      = 599;
  terminal.retry_after      = retry_after;
  terminal.etag             = etag;
  terminal.last_modified    = last_modified;
  terminal.content_type     = content_type;
  return internal_mdl_check_response_size(ra8__mdl__chunk__get_packed_size(&terminal),
                                          response_cap);
}

/**
 * @brief Pull and validate one backend response without advancing service
 * state.
 * @details Reads into fixed storage, proves byte/count/offset/terminal
 * invariants, and cancels the job on any backend or protocol failure.
 * @param[in,out] service Active portable service context.
 * @param[in] max_data Maximum permitted body bytes.
 * @param[out] result Receives bounded backend data and derived end offset.
 * @return Canonical backend or protocol status.
 * @retval k_ra8_ok Result is coherent and ready to pack.
 * @retval k_ra8_err_protocol_error Backend metadata violates the wire contract.
 * @retval other Backend read failure returned after cancellation.
 * @pre @p service and @p result are non-null.
 * @pre Service owns an active job and @p max_data fits the result buffer.
 * @post Success does not change sequence or offset service state.
 * @post Failure attempts cancellation and deactivates the job.
 * @note Terminal responses carry no data and must exactly close total length.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_mdl_read_next(ra8_mdl_service_t*        service,
                                        uint32_t                  max_data,
                                        internal_mdl_next_read_t* result)
{
  const ra8_err_t read = service->backend.read(service->backend.ctx,
                                               result->bytes,
                                               (uint16_t)max_data,
                                               &result->got,
                                               &result->total,
                                               &result->complete,
                                               result->digest,
                                               &result->response);
  if (read != k_ra8_ok) {
    return internal_mdl_fail_job(service, read);
  }
  const bool offset_overflow = service->next_offset > (UINT64_MAX - result->got);
  result->end_offset         = offset_overflow ? 0U : service->next_offset + result->got;
  bool total_invalid         = false;
  if (result->complete) {
    total_invalid = result->end_offset != result->total;
  } else if (result->total != 0U) {
    total_invalid = result->end_offset > result->total;
  }
  if ((result->got > max_data) || ((!result->complete) && (result->got == 0U)) ||
      (result->complete && (result->got != 0U)) || offset_overflow || total_invalid ||
      (service->next_sequence == UINT32_MAX) ||
      (result->complete && !internal_mdl_response_valid(&result->response)) ||
      (!result->complete && (result->response.status != 0))) {
    return internal_mdl_fail_job(service, k_ra8_err_protocol_error);
  }
  return k_ra8_ok;
}

/**
 * @brief Validate one pull, read bounded bytes, and pack a correlated Chunk
 * @details Proves worst-case response capacity before consuming backend bytes.
 * @param[in,out] service Active portable service.
 * @param[in,out] alloc Bounded per-dispatch protobuf allocator.
 * @param[in] request Packed NextRequest.
 * @param[in] request_len Valid request bytes.
 * @param[out] response Caller-owned Chunk bytes.
 * @param[in] response_cap Response capacity.
 * @param[out] response_len Packed response length.
 * @return Pull status.
 * @retval k_ra8_ok One ordered data or terminal response was packed.
 * @retval k_ra8_err_protocol_error Decode, unknown fields, or backend fields
 * are incoherent.
 * @retval k_ra8_err_invalid_state Job correlation or requested bound is
 * invalid.
 * @retval k_ra8_err_invalid_size Worst-case response does not fit.
 * @pre All pointers are non-null and one job is active.
 * @pre @p alloc owns a fresh bounded arena.
 * @post Success advances sequence/offset by exactly returned body bytes.
 * @post Terminal success deactivates the service job.
 * @note Not thread-safe for a shared service/backend.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_mdl_dispatch_next(ra8_mdl_service_t*  service,
                                                         ProtobufCAllocator* alloc,
                                                         const uint8_t*      request,
                                                         size_t              request_len,
                                                         uint8_t*            response,
                                                         size_t              response_cap,
                                                         size_t*             response_len)
{
  Ra8__Mdl__NextRequest* req = ra8__mdl__next_request__unpack(alloc, request_len, request);
  if (req == nullptr) {
    return k_ra8_err_protocol_error;
  }
  if (req->base.n_unknown_fields != 0U) {
    return k_ra8_err_protocol_error;
  }
  if ((req->protocol_version != k_ra8_mdl_protocol_version) || !service->active ||
      (req->job_id != service->active_job_id) ||
      (req->acknowledged_offset != service->next_offset) || (req->max_bytes == 0U) ||
      (req->max_bytes > k_ra8_mdl_chunk_data_max)) {
    return k_ra8_err_invalid_state;
  }

  const ra8_err_t capacity = internal_mdl_next_capacity(req->max_bytes, response_cap);
  if (capacity != k_ra8_ok) {
    return capacity;
  }

  internal_mdl_next_read_t result = {};
  const ra8_err_t          read   = internal_mdl_read_next(service, req->max_bytes, &result);
  if (read != k_ra8_ok) {
    return read;
  }

  Ra8__Mdl__Chunk out  = RA8__MDL__CHUNK__INIT;
  out.protocol_version = k_ra8_mdl_protocol_version;
  out.job_id           = service->active_job_id;
  out.sequence         = service->next_sequence;
  out.offset           = service->next_offset;
  out.data             = (ProtobufCBinaryData){.len = result.got, .data = result.bytes};
  out.total_bytes      = result.total;
  out.state =
    result.complete ? RA8__MDL__STATE__STATE_COMPLETE : RA8__MDL__STATE__STATE_DOWNLOADING;
  out.status = 0;
  if (result.complete) {
    out.sha256        = (ProtobufCBinaryData){.len = k_ra8_mdl_sha256_bytes, .data = result.digest};
    out.http_status   = result.response.status;
    out.retry_after   = result.response.retry_after;
    out.etag          = result.response.etag;
    out.last_modified = result.response.last_modified;
    out.content_type  = result.response.content_type;
  }
  const ra8_err_t packed = internal_mdl_pack_chunk(&out, response, response_cap, response_len);
  if (packed != k_ra8_ok) {
    return internal_mdl_fail_job(service, packed);
  }
  service->next_offset += result.got;
  service->next_sequence += 1U;
  if (result.complete) {
    service->active = false;
  }
  return k_ra8_ok;
}

/**
 * @brief Validate and cancel one correlated active service job
 * @details Packs the acknowledgement before asking the backend to release
 * state.
 * @param[in,out] service Active portable service.
 * @param[in,out] alloc Bounded per-dispatch protobuf allocator.
 * @param[in] request Packed CancelRequest.
 * @param[in] request_len Valid request bytes.
 * @param[out] response Caller-owned Cancelled bytes.
 * @param[in] response_cap Response capacity.
 * @param[out] response_len Packed response length.
 * @return Cancellation status.
 * @retval k_ra8_ok Backend cancelled and acknowledgement was packed.
 * @retval k_ra8_err_protocol_error Decode failed or unknown fields were
 * present.
 * @retval k_ra8_err_invalid_state Job correlation is invalid.
 * @retval k_ra8_err_invalid_size Acknowledgement does not fit.
 * @retval k_ra8_err_validation_failed Codec length and write disagree.
 * @pre All pointers are non-null and one job is active.
 * @pre @p alloc owns a fresh bounded arena.
 * @post Success deactivates the service.
 * @post Backend failure leaves response length zero.
 * @note Not thread-safe for a shared service/backend.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_mdl_dispatch_cancel(ra8_mdl_service_t*  service,
                                                           ProtobufCAllocator* alloc,
                                                           const uint8_t*      request,
                                                           size_t              request_len,
                                                           uint8_t*            response,
                                                           size_t              response_cap,
                                                           size_t*             response_len)
{
  Ra8__Mdl__CancelRequest* req = ra8__mdl__cancel_request__unpack(alloc, request_len, request);
  if (req == nullptr) {
    return k_ra8_err_protocol_error;
  }
  if (req->base.n_unknown_fields != 0U) {
    return k_ra8_err_protocol_error;
  }
  if ((req->protocol_version != k_ra8_mdl_protocol_version) || !service->active ||
      (req->job_id != service->active_job_id)) {
    return k_ra8_err_invalid_state;
  }
  Ra8__Mdl__Cancelled out  = RA8__MDL__CANCELLED__INIT;
  out.protocol_version     = k_ra8_mdl_protocol_version;
  out.job_id               = req->job_id;
  out.status               = 0;
  const size_t    len      = ra8__mdl__cancelled__get_packed_size(&out);
  const ra8_err_t capacity = internal_mdl_check_response_size(len, response_cap);
  if (capacity != k_ra8_ok) {
    return capacity;
  }
  if (ra8__mdl__cancelled__pack(&out, response) != len) {
    return k_ra8_err_validation_failed;
  }
  const ra8_err_t cancelled = service->backend.cancel(service->backend.ctx);
  if (cancelled != k_ra8_ok) {
    return cancelled;
  }
  service->active = false;
  *response_len   = len;
  return k_ra8_ok;
}

ra8_err_t ra8_mdl_service_init(ra8_mdl_service_t* service, const ra8_mdl_service_backend_t* backend)
{
  if ((service == nullptr) || (backend == nullptr) || (backend->begin == nullptr) ||
      (backend->read == nullptr) || (backend->cancel == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  *service = (ra8_mdl_service_t){.backend = *backend};
  return k_ra8_ok;
}

ra8_err_t ra8_mdl_service_dispatch(void*          ctx,
                                   uint32_t       operation,
                                   const uint8_t* request,
                                   size_t         request_len,
                                   uint8_t*       response,
                                   size_t         response_cap,
                                   size_t*        response_len)
{
  if ((ctx == nullptr) || (request == nullptr) || (request_len == 0U) || (response == nullptr) ||
      (response_len == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  *response_len              = 0U;
  mdl_decode_arena_t arena   = {};
  ProtobufCAllocator alloc   = {.alloc          = internal_mdl_decode_alloc,
                                .free           = internal_mdl_decode_free,
                                .allocator_data = &arena};
  ra8_mdl_service_t* service = (ra8_mdl_service_t*)ctx;
  switch (operation) {
    case k_ra8_mdl_rpc_start:
      return internal_mdl_dispatch_start(service,
                                         &alloc,
                                         request,
                                         request_len,
                                         response,
                                         response_cap,
                                         response_len);
    case k_ra8_mdl_rpc_next:
      return internal_mdl_dispatch_next(service,
                                        &alloc,
                                        request,
                                        request_len,
                                        response,
                                        response_cap,
                                        response_len);
    case k_ra8_mdl_rpc_cancel:
      return internal_mdl_dispatch_cancel(service,
                                          &alloc,
                                          request,
                                          request_len,
                                          response,
                                          response_cap,
                                          response_len);
    default:
      return k_ra8_err_not_supported;
  }
}
