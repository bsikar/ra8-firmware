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

#include "ra8_attributes.h"
#include "ra8_c6link_mdl_msg.h"
#include "ra8_c6link_mdl_service_internal.h"

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
  return priv_c6link_mdl_service_response_size_ok(len, response_cap) ? k_ra8_ok
                                                                    : k_ra8_err_invalid_size;
}

/**
 * @brief Admit and begin one typed-artifact service job
 * @details Encodes the bounded reply before allowing backend side effects.
 * @param[in,out] service Initialised portable service.
 * @param[in] request Packed StartRequest.
 * @param[in] request_len Valid request bytes.
 * @param[out] response Caller-owned Accepted bytes.
 * @param[in] response_cap Response capacity.
 * @param[out] response_len Encoded response length.
 * @return Start status.
 * @retval k_ra8_ok Backend accepted a correlated job.
 * @retval k_ra8_err_protocol_error Decode failed or unknown fields were
 * present.
 * @retval k_ra8_err_invalid_arg Version, URL, format, timeout, or a header is
 * invalid.
 * @retval k_ra8_err_busy A job is already active.
 * @retval k_ra8_err_invalid_size The reply does not fit.
 * @pre All pointers are non-null and service access is exclusive.
 * @post Success activates exactly one non-zero job id.
 * @post Backend failure leaves the service inactive and response length zero.
 * @note Not thread-safe for a shared service.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_mdl_dispatch_start(ra8_mdl_service_t* service,
                                                          const uint8_t*     request,
                                                          size_t             request_len,
                                                          uint8_t*           response,
                                                          size_t             response_cap,
                                                          size_t*            response_len)
{
  ra8_mdl_request_t backend_request = {};
  const ra8_err_t   admitted        = priv_c6link_mdl_service_start_admit(
    request, request_len, service->active, &service->start_text, &backend_request);
  if (admitted != k_ra8_ok) {
    return admitted;
  }
  uint32_t next_job_id = service->next_job_id + 1U;
  if (next_job_id == 0U) {
    next_job_id = 1U;
  }
  const ra8_err_t replied = priv_c6link_mdl_service_accepted(
    next_job_id, backend_request.format, response, response_cap, response_len);
  if (replied != k_ra8_ok) {
    return replied;
  }
  const ra8_err_t begun = service->backend.begin(service->backend.ctx, &backend_request);
  if (begun != k_ra8_ok) {
    *response_len = 0U;
    return begun;
  }
  service->next_job_id   = next_job_id;
  service->active_job_id = service->next_job_id;
  service->next_sequence = 0U;
  service->next_offset   = 0U;
  service->active_format = backend_request.format;
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
  bool           overflowed = false;
  const uint64_t end = priv_c6link_mdl_pull_end_offset(service->next_offset, result->got,
                                                       &overflowed);
  result->end_offset = end;
  const mdl_pull_view_t view = {
    .next_offset     = service->next_offset,
    .total           = result->total,
    .next_sequence   = service->next_sequence,
    .max_data        = max_data,
    .got             = result->got,
    .complete        = result->complete,
    .response_valid  = priv_c6link_mdl_service_response_valid(&result->response),
    .response_status = result->response.status,
  };
  if (!priv_c6link_mdl_pull_coherent(&view)) {
    return internal_mdl_fail_job(service, k_ra8_err_protocol_error);
  }
  return k_ra8_ok;
}

/**
 * @brief Admit one pull, read bounded bytes, and encode a correlated Chunk
 * @details Proves worst-case response capacity before consuming backend bytes.
 * @param[in,out] service Active portable service.
 * @param[in] request Packed NextRequest.
 * @param[in] request_len Valid request bytes.
 * @param[out] response Caller-owned Chunk bytes.
 * @param[in] response_cap Response capacity.
 * @param[out] response_len Encoded response length.
 * @return Pull status.
 * @retval k_ra8_ok One ordered data or terminal response was encoded.
 * @retval k_ra8_err_protocol_error Decode, unknown fields, or backend fields
 * are incoherent.
 * @retval k_ra8_err_invalid_state Job correlation or requested bound is
 * invalid.
 * @retval k_ra8_err_invalid_size Worst-case response does not fit.
 * @pre All pointers are non-null.
 * @post Success advances sequence/offset by exactly returned body bytes.
 * @post Terminal success deactivates the service job.
 * @note Not thread-safe for a shared service/backend.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_mdl_dispatch_next(ra8_mdl_service_t* service,
                                                         const uint8_t*     request,
                                                         size_t             request_len,
                                                         uint8_t*           response,
                                                         size_t             response_cap,
                                                         size_t*            response_len)
{
  const mdl_job_view_t job = {
    .next_offset   = service->next_offset,
    .active_job_id = service->active_job_id,
    .active        = service->active,
  };
  uint32_t        max_bytes = 0U;
  const ra8_err_t admitted =
    priv_c6link_mdl_service_next_admit(request, request_len, &job, response_cap, &max_bytes);
  if (admitted != k_ra8_ok) {
    return admitted;
  }

  internal_mdl_next_read_t result = {};
  const ra8_err_t          read   = internal_mdl_read_next(service, max_bytes, &result);
  if (read != k_ra8_ok) {
    return read;
  }

  const mdl_chunk_reply_t reply = {
    .offset   = service->next_offset,
    .total    = result.total,
    .data     = result.bytes,
    .digest   = result.digest,
    .response = &result.response,
    .job_id   = service->active_job_id,
    .sequence = service->next_sequence,
    .got      = result.got,
    .complete = result.complete,
  };
  const ra8_err_t packed =
    priv_c6link_mdl_service_pack_chunk(&reply, response, response_cap, response_len);
  if (packed != k_ra8_ok) {
    return internal_mdl_fail_job(service, packed);
  }
  const mdl_advance_t advanced =
    priv_c6link_mdl_pull_advance(service->next_offset, service->next_sequence, result.got,
                                 result.complete);
  service->next_offset   = advanced.next_offset;
  service->next_sequence = advanced.next_sequence;
  service->active        = advanced.active;
  return k_ra8_ok;
}

/**
 * @brief Validate and cancel one correlated active service job
 * @details Packs the acknowledgement before asking the backend to release
 * state.
 * @param[in,out] service Active portable service.
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
 * @pre All pointers are non-null and one job is active.
 * @post Success deactivates the service.
 * @post Backend failure leaves response length zero.
 * @note Not thread-safe for a shared service/backend.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_mdl_dispatch_cancel(ra8_mdl_service_t* service,
                                                           const uint8_t*     request,
                                                           size_t             request_len,
                                                           uint8_t*           response,
                                                           size_t             response_cap,
                                                           size_t*            response_len)
{
  const mdl_job_view_t job = {
    .next_offset   = service->next_offset,
    .active_job_id = service->active_job_id,
    .active        = service->active,
  };
  size_t          len     = 0U;
  const ra8_err_t replied =
    priv_c6link_mdl_service_cancel(request, request_len, &job, response, response_cap, &len);
  if (replied != k_ra8_ok) {
    return replied;
  }
  const ra8_err_t cancelled = service->backend.cancel(service->backend.ctx);
  if (cancelled != k_ra8_ok) {
    return cancelled;
  }
  service->active = false;
  *response_len   = len;
  return k_ra8_ok;
}

RA8_TEST_HELPER bool ra8_mdl_service_field_valid_test(const char* text, size_t cap)
{
  return priv_c6link_mdl_service_field_valid(text, cap);
}

RA8_TEST_HELPER bool ra8_mdl_service_response_valid_test(const ra8_mdl_http_response_t* response)
{
  return priv_c6link_mdl_service_response_valid(response);
}

RA8_TEST_HELPER ra8_err_t ra8_mdl_service_check_size_test(size_t len, size_t response_cap)
{
  return internal_mdl_check_response_size(len, response_cap);
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
  ra8_mdl_service_t* service = (ra8_mdl_service_t*)ctx;
  switch (operation) {
    case k_ra8_mdl_rpc_start:
      return internal_mdl_dispatch_start(service,
                                         request,
                                         request_len,
                                         response,
                                         response_cap,
                                         response_len);
    case k_ra8_mdl_rpc_next:
      return internal_mdl_dispatch_next(service,
                                        request,
                                        request_len,
                                        response,
                                        response_cap,
                                        response_len);
    case k_ra8_mdl_rpc_cancel:
      return internal_mdl_dispatch_cancel(service,
                                          request,
                                          request_len,
                                          response,
                                          response_cap,
                                          response_len);
    default:
      return k_ra8_err_not_supported;
  }
}
