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

#include "ra8_c6link_mdl_msg.h"
#include "ra8_media_download.pb-c.h"

/** @brief Decode arena sized for the bounded start request and URL. */
typedef enum : uint16_t {
  k_mdl_decode_arena_bytes = k_ra8_mdl_url_max + 384U, /**< Per-dispatch arena size. */
  k_mdl_decode_align       = 8U,                       /**< Arena allocation alignment. */
  k_mdl_decode_align_mask  = k_mdl_decode_align - 1U,  /**< Mask used to round sizes. */
} mdl_svc_const_t;

/** @brief Linear allocator used by protobuf-c; reset after every dispatch. */
typedef struct {
  uint8_t bytes[k_mdl_decode_arena_bytes]; /**< Fixed protobuf decode storage. */
  size_t  used;                            /**< Bytes already allocated.       */
} mdl_decode_arena_t;

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
static void* mdl_decode_alloc(void* data, size_t len)
{
  mdl_decode_arena_t* arena   = (mdl_decode_arena_t*)data;
  const size_t        aligned = (len + k_mdl_decode_align_mask) & ~(size_t)k_mdl_decode_align_mask;
  if ((aligned > sizeof(arena->bytes)) || (arena->used > (sizeof(arena->bytes) - aligned))) {
    return nullptr;
  }
  void* out = &arena->bytes[arena->used];
  arena->used += aligned;
  return out;
}

/**
 * @brief Ignore individual protobuf frees for the linear dispatch arena
 * @details The complete arena is discarded when dispatch returns.
 * @param[in] data Arena context retained for allocator ABI compatibility.
 * @param[in] ptr Previously returned span retained for allocator ABI compatibility.
 * @pre @p data identifies the current dispatch arena.
 * @pre @p ptr is null or belongs to that arena.
 * @post Arena state is unchanged.
 * @post No system allocator is invoked.
 * @note Reentrant for independent arenas.
 * @since 0.1.0
 */
static void mdl_decode_free(void* data, void* ptr)
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
static ra8_err_t mdl_check_response_size(size_t len, size_t response_cap)
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
static ra8_err_t mdl_pack_accepted(const Ra8__Mdl__Accepted* msg,
                                   uint8_t*                  response,
                                   size_t                    response_cap,
                                   size_t*                   response_len)
{
  const size_t    len      = ra8__mdl__accepted__get_packed_size(msg);
  const ra8_err_t capacity = mdl_check_response_size(len, response_cap);
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
static ra8_err_t mdl_pack_chunk(const Ra8__Mdl__Chunk* msg,
                                uint8_t*               response,
                                size_t                 response_cap,
                                size_t*                response_len)
{
  const size_t    len      = ra8__mdl__chunk__get_packed_size(msg);
  const ra8_err_t capacity = mdl_check_response_size(len, response_cap);
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
 * @brief Validate and begin one raw-body service job
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
 * @retval k_ra8_err_protocol_error Request decoding failed.
 * @retval k_ra8_err_invalid_arg Version or URL is invalid.
 * @retval k_ra8_err_busy A job is already active.
 * @pre All pointers are non-null and service access is exclusive.
 * @pre @p alloc owns a fresh bounded arena.
 * @post Success activates exactly one non-zero job id.
 * @post Backend failure leaves the service inactive and response length zero.
 * @note Not thread-safe for a shared service.
 * @since 0.1.0
 */
static ra8_err_t mdl_dispatch_start(ra8_mdl_service_t*  service,
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
  if (req->url == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  const size_t https_prefix_len = sizeof("https://") - 1U;
  const size_t url_len          = strnlen(req->url, k_ra8_mdl_url_max);
  const bool   valid = (req->protocol_version == k_ra8_mdl_protocol_version) && (url_len != 0U) &&
                       (url_len < k_ra8_mdl_url_max) &&
                       (strncmp(req->url, "https://", https_prefix_len) == 0) &&
                       (req->url[https_prefix_len] != '\0');
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
  Ra8__Mdl__Accepted out    = RA8__MDL__ACCEPTED__INIT;
  out.protocol_version      = k_ra8_mdl_protocol_version;
  out.job_id                = next_job_id;
  out.max_chunk_bytes       = k_ra8_mdl_chunk_data_max;
  const ra8_err_t prepacked = mdl_pack_accepted(&out, response, response_cap, response_len);
  if (prepacked != k_ra8_ok) {
    return prepacked;
  }
  const ra8_err_t begun = service->backend.begin(service->backend.ctx, req->url);
  if (begun != k_ra8_ok) {
    *response_len = 0U;
    return begun;
  }
  service->next_job_id   = next_job_id;
  service->active_job_id = service->next_job_id;
  service->next_sequence = 0U;
  service->next_offset   = 0U;
  service->active        = true;
  return k_ra8_ok;
}

// Kept linear so capacity is proven before the sole backend read side effect.
// NOLINTBEGIN(readability-function-size)
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
 * @retval k_ra8_err_protocol_error Decode or backend fields are incoherent.
 * @retval k_ra8_err_invalid_state Job correlation or requested bound is invalid.
 * @retval k_ra8_err_invalid_size Worst-case response does not fit.
 * @pre All pointers are non-null and one job is active.
 * @pre @p alloc owns a fresh bounded arena.
 * @post Success advances sequence/offset by exactly returned body bytes.
 * @post Terminal success deactivates the service job.
 * @note Not thread-safe for a shared service/backend.
 * @since 0.1.0
 */
static ra8_err_t mdl_dispatch_next(ra8_mdl_service_t*  service,
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
  if ((req->protocol_version != k_ra8_mdl_protocol_version) || !service->active ||
      (req->job_id != service->active_job_id) ||
      (req->acknowledged_offset != service->next_offset) || (req->max_bytes == 0U) ||
      (req->max_bytes > k_ra8_mdl_chunk_data_max)) {
    return k_ra8_err_invalid_state;
  }

  /* Prove the caller can hold the largest possible answer before read() can
   * consume bytes. Maximal varints make this an upper bound for every real
   * response with the requested data length. */
  uint8_t         max_bytes[k_ra8_mdl_chunk_data_max];
  uint8_t         max_digest[k_ra8_mdl_sha256_bytes];
  Ra8__Mdl__Chunk largest  = RA8__MDL__CHUNK__INIT;
  largest.protocol_version = UINT32_MAX;
  largest.job_id           = UINT32_MAX;
  largest.sequence         = UINT32_MAX;
  largest.offset           = UINT64_MAX;
  largest.data             = (ProtobufCBinaryData){.len = req->max_bytes, .data = max_bytes};
  largest.total_bytes      = UINT64_MAX;
  largest.state            = RA8__MDL__STATE__STATE_COMPLETE;
  largest.status           = INT32_MAX;
  largest.sha256           = (ProtobufCBinaryData){.len = sizeof(max_digest), .data = max_digest};
  const size_t    largest_len = ra8__mdl__chunk__get_packed_size(&largest);
  const ra8_err_t capacity    = mdl_check_response_size(largest_len, response_cap);
  if (capacity != k_ra8_ok) {
    return capacity;
  }

  uint8_t         bytes[k_ra8_mdl_chunk_data_max];
  uint8_t         digest[k_ra8_mdl_sha256_bytes] = {};
  uint16_t        got                            = 0U;
  uint64_t        total                          = 0U;
  bool            complete                       = false;
  const ra8_err_t read =
    service->backend
      .read(service->backend.ctx, bytes, (uint16_t)req->max_bytes, &got, &total, &complete, digest);
  if (read != k_ra8_ok) {
    (void)service->backend.cancel(service->backend.ctx);
    service->active = false;
    return read;
  }
  const bool     offset_overflow = service->next_offset > (UINT64_MAX - got);
  const uint64_t end_offset      = offset_overflow ? 0U : service->next_offset + got;
  const bool     total_invalid =
    (total != 0U) && ((end_offset > total) || (complete && (end_offset != total)));
  if ((got > req->max_bytes) || ((!complete) && (got == 0U)) || (complete && (got != 0U)) ||
      offset_overflow || total_invalid || (service->next_sequence == UINT32_MAX)) {
    (void)service->backend.cancel(service->backend.ctx);
    service->active = false;
    return k_ra8_err_protocol_error;
  }

  Ra8__Mdl__Chunk out  = RA8__MDL__CHUNK__INIT;
  out.protocol_version = k_ra8_mdl_protocol_version;
  out.job_id           = service->active_job_id;
  out.sequence         = service->next_sequence;
  out.offset           = service->next_offset;
  out.data             = (ProtobufCBinaryData){.len = got, .data = bytes};
  out.total_bytes      = total;
  out.state  = complete ? RA8__MDL__STATE__STATE_COMPLETE : RA8__MDL__STATE__STATE_DOWNLOADING;
  out.status = 0;
  if (complete) {
    out.sha256 = (ProtobufCBinaryData){.len = k_ra8_mdl_sha256_bytes, .data = digest};
  }
  const ra8_err_t packed = mdl_pack_chunk(&out, response, response_cap, response_len);
  if (packed != k_ra8_ok) {
    (void)service->backend.cancel(service->backend.ctx);
    service->active = false;
    return packed;
  }
  service->next_offset += got;
  service->next_sequence += 1U;
  if (complete) {
    service->active = false;
  }
  return k_ra8_ok;
}
// NOLINTEND(readability-function-size)

/**
 * @brief Validate and cancel one correlated active service job
 * @details Packs the acknowledgement before asking the backend to release state.
 * @param[in,out] service Active portable service.
 * @param[in,out] alloc Bounded per-dispatch protobuf allocator.
 * @param[in] request Packed CancelRequest.
 * @param[in] request_len Valid request bytes.
 * @param[out] response Caller-owned Cancelled bytes.
 * @param[in] response_cap Response capacity.
 * @param[out] response_len Packed response length.
 * @return Cancellation status.
 * @retval k_ra8_ok Backend cancelled and acknowledgement was packed.
 * @retval k_ra8_err_invalid_state Decode or job correlation is invalid.
 * @retval k_ra8_err_invalid_size Acknowledgement does not fit.
 * @retval k_ra8_err_validation_failed Codec length and write disagree.
 * @pre All pointers are non-null and one job is active.
 * @pre @p alloc owns a fresh bounded arena.
 * @post Success deactivates the service.
 * @post Backend failure leaves response length zero.
 * @note Not thread-safe for a shared service/backend.
 * @since 0.1.0
 */
static ra8_err_t mdl_dispatch_cancel(ra8_mdl_service_t*  service,
                                     ProtobufCAllocator* alloc,
                                     const uint8_t*      request,
                                     size_t              request_len,
                                     uint8_t*            response,
                                     size_t              response_cap,
                                     size_t*             response_len)
{
  Ra8__Mdl__CancelRequest* req = ra8__mdl__cancel_request__unpack(alloc, request_len, request);
  if ((req == nullptr) || (req->protocol_version != k_ra8_mdl_protocol_version) ||
      !service->active || (req->job_id != service->active_job_id)) {
    return k_ra8_err_invalid_state;
  }
  Ra8__Mdl__Cancelled out  = RA8__MDL__CANCELLED__INIT;
  out.protocol_version     = k_ra8_mdl_protocol_version;
  out.job_id               = req->job_id;
  out.status               = 0;
  const size_t    len      = ra8__mdl__cancelled__get_packed_size(&out);
  const ra8_err_t capacity = mdl_check_response_size(len, response_cap);
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
  ProtobufCAllocator alloc   = {.alloc          = mdl_decode_alloc,
                                .free           = mdl_decode_free,
                                .allocator_data = &arena};
  ra8_mdl_service_t* service = (ra8_mdl_service_t*)ctx;
  switch (operation) {
    case k_ra8_mdl_rpc_start:
      return mdl_dispatch_start(service,
                                &alloc,
                                request,
                                request_len,
                                response,
                                response_cap,
                                response_len);
    case k_ra8_mdl_rpc_next:
      return mdl_dispatch_next(service,
                               &alloc,
                               request,
                               request_len,
                               response,
                               response_cap,
                               response_len);
    case k_ra8_mdl_rpc_cancel:
      return mdl_dispatch_cancel(service,
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
