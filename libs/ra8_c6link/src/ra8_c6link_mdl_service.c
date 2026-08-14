/**
 * @file ra8_c6link_mdl_service.c
 * @brief Portable one-job media service state machine for the ESP32-C6 port.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ra8_c6link_mdl_msg.h"
#include "ra8_media_download.pb-c.h"

/** @brief Decode arena sized for the bounded start request and URL. */
typedef enum : uint16_t { k_mdl_decode_arena_bytes = RA8_MDL_URL_MAX + 384U } mdl_svc_const_t;

/** @brief Linear allocator used by protobuf-c; reset after every dispatch. */
typedef struct {
  uint8_t bytes[k_mdl_decode_arena_bytes];
  size_t  used;
} mdl_decode_arena_t;

static void* mdl_decode_alloc(void* data, size_t len)
{
  mdl_decode_arena_t* arena   = (mdl_decode_arena_t*)data;
  const size_t        aligned = (len + 7U) & ~(size_t)7U;
  if ((aligned > sizeof(arena->bytes)) || (arena->used > (sizeof(arena->bytes) - aligned))) {
    return nullptr;
  }
  void* out = &arena->bytes[arena->used];
  arena->used += aligned;
  return out;
}

static void mdl_decode_free(void* data, void* ptr)
{
  (void)data;
  (void)ptr;
}

/** @brief Reject a response that cannot fit before invoking the backend. */
static ra8_err_t mdl_check_response_size(size_t len, size_t response_cap)
{
  if ((len == 0U) || (len > response_cap)) {
    return k_ra8_err_invalid_size;
  }
  return k_ra8_ok;
}

/** @brief Pack an Accepted response without incompatible function-pointer casts. */
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

/** @brief Pack a Chunk response without incompatible function-pointer casts. */
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
  const size_t url_len = strnlen(req->url, RA8_MDL_URL_MAX);
  const bool   valid   = (req->protocol_version == RA8_MDL_PROTOCOL_VERSION) && (url_len != 0U) &&
                         (url_len < RA8_MDL_URL_MAX) &&
                         (req->format >= RA8__MDL__FORMAT__FORMAT_RABOOK) &&
                         (req->format <= RA8__MDL__FORMAT__FORMAT_EPUB);
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
  out.protocol_version      = RA8_MDL_PROTOCOL_VERSION;
  out.job_id                = next_job_id;
  out.max_chunk_bytes       = RA8_MDL_CHUNK_DATA_MAX;
  const ra8_err_t prepacked = mdl_pack_accepted(&out, response, response_cap, response_len);
  if (prepacked != k_ra8_ok) {
    return prepacked;
  }
  const ra8_err_t begun =
    service->backend.begin(service->backend.ctx, req->url, (ra8_mdl_format_t)req->format);
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
  if ((req->protocol_version != RA8_MDL_PROTOCOL_VERSION) || !service->active ||
      (req->job_id != service->active_job_id) ||
      (req->acknowledged_offset != service->next_offset) || (req->max_bytes == 0U) ||
      (req->max_bytes > RA8_MDL_CHUNK_DATA_MAX)) {
    return k_ra8_err_invalid_state;
  }

  /* Prove the caller can hold the largest possible answer before read() can
   * consume bytes. Maximal varints make this an upper bound for every real
   * response with the requested data length. */
  uint8_t         max_bytes[RA8_MDL_CHUNK_DATA_MAX];
  uint8_t         max_digest[RA8_MDL_SHA256_BYTES];
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

  uint8_t         bytes[RA8_MDL_CHUNK_DATA_MAX];
  uint8_t         digest[RA8_MDL_SHA256_BYTES] = {};
  uint16_t        got                          = 0U;
  uint64_t        total                        = 0U;
  bool            complete                     = false;
  const ra8_err_t read =
    service->backend
      .read(service->backend.ctx, bytes, (uint16_t)req->max_bytes, &got, &total, &complete, digest);
  if (read != k_ra8_ok) {
    service->active = false;
    return read;
  }
  if ((got > req->max_bytes) || ((!complete) && (got == 0U))) {
    service->active = false;
    return k_ra8_err_protocol_error;
  }

  Ra8__Mdl__Chunk out  = RA8__MDL__CHUNK__INIT;
  out.protocol_version = RA8_MDL_PROTOCOL_VERSION;
  out.job_id           = service->active_job_id;
  out.sequence         = service->next_sequence;
  out.offset           = service->next_offset;
  out.data             = (ProtobufCBinaryData){.len = got, .data = bytes};
  out.total_bytes      = total;
  out.state  = complete ? RA8__MDL__STATE__STATE_COMPLETE : RA8__MDL__STATE__STATE_DOWNLOADING;
  out.status = 0;
  if (complete) {
    out.sha256 = (ProtobufCBinaryData){.len = RA8_MDL_SHA256_BYTES, .data = digest};
  }
  const ra8_err_t packed = mdl_pack_chunk(&out, response, response_cap, response_len);
  if (packed != k_ra8_ok) {
    return packed;
  }
  service->next_offset += got;
  service->next_sequence += 1U;
  if (complete) {
    service->active = false;
  }
  return k_ra8_ok;
}

static ra8_err_t mdl_dispatch_cancel(ra8_mdl_service_t*  service,
                                     ProtobufCAllocator* alloc,
                                     const uint8_t*      request,
                                     size_t              request_len,
                                     uint8_t*            response,
                                     size_t              response_cap,
                                     size_t*             response_len)
{
  Ra8__Mdl__CancelRequest* req = ra8__mdl__cancel_request__unpack(alloc, request_len, request);
  if ((req == nullptr) || (req->protocol_version != RA8_MDL_PROTOCOL_VERSION) || !service->active ||
      (req->job_id != service->active_job_id)) {
    return k_ra8_err_invalid_state;
  }
  Ra8__Mdl__Cancelled out  = RA8__MDL__CANCELLED__INIT;
  out.protocol_version     = RA8_MDL_PROTOCOL_VERSION;
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
