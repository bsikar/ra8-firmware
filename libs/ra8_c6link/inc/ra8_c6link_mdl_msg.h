/**
 * @file ra8_c6link_mdl_msg.h
 * @brief Media service dispatch contract for the ESP32-C6 integration.
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details Wire messages are generated from `ra8_media_download.proto`; this
 * header intentionally defines no packed C wire structs.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "ra8_err.h"
#include "ra8_mdl_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Dispatch one generated inner media request into caller-owned storage
 * @details The implementation decodes with a bounded arena and returns one
 * packed generated response without retaining either byte span.
 * @param[in] ctx Service context selected by the RPC integration.
 * @param[in] operation One ::ra8_mdl_rpc_id_t value.
 * @param[in] request Packed protobuf request bytes.
 * @param[in] request_len Valid bytes at @p request.
 * @param[out] response Caller-owned packed-response buffer.
 * @param[in] response_cap Capacity of @p response.
 * @param[out] response_len Packed bytes written on success.
 * @return Dispatch status.
 * @retval k_ra8_ok A valid response was packed.
 * @retval k_ra8_err_null_ptr A required pointer is null.
 * @retval k_ra8_err_invalid_arg The operation or decoded fields are invalid.
 * @retval k_ra8_err_invalid_size A request or response exceeds a fixed bound.
 * @retval k_ra8_err_protocol_error Job correlation or state is incoherent.
 * @pre The request and response spans do not overlap.
 * @pre @p ctx remains exclusively owned for the complete call.
 * @post Success sets @p response_len within @p response_cap.
 * @post Failure sets @p response_len to zero after pointer validation.
 * @note Synchronous and not thread-safe for a shared service context.
 * @since 0.1.0
 */
typedef ra8_err_t (*ra8_mdl_service_dispatch_fn)(void*          ctx,
                                                 uint32_t       operation,
                                                 const uint8_t* request,
                                                 size_t         request_len,
                                                 uint8_t*       response,
                                                 size_t         response_cap,
                                                 size_t*        response_len);

/**
 * @struct ra8_mdl_service_backend
 * @brief Bounded raw-byte source seam implemented by the C6 port
 * @details Separates protobuf/job orchestration from the concrete HTTPS and
 * SHA-256 mechanism. Implementations own all network and digest state.
 * @invariant Every callback and `ctx` is non-null after service initialisation.
 * @invariant At most one job is active because the portable service serialises it.
 * @since 0.1.0
 */
typedef struct ra8_mdl_service_backend {
  ra8_err_t (*begin)(void* ctx, const char* url); /**< Validate and begin one HTTPS body. */
  ra8_err_t (*read)(
    void*     ctx,
    uint8_t*  out,
    uint16_t  cap,
    uint16_t* got,
    uint64_t* total_bytes,
    bool*     complete,
    uint8_t   sha256[k_ra8_mdl_sha256_bytes]); /**< Pull bytes or terminal metadata.           */
  ra8_err_t (*cancel)(void* ctx);              /**< Cancel and release the active backend job. */
  void* ctx;                                   /**< Backend callback context.                  */
} ra8_mdl_service_backend_t;

/**
 * @struct ra8_mdl_service
 * @brief Caller-owned one-job portable service state
 * @details Correlates Start, Next, and Cancel independently of the concrete
 * C6 HTTPS backend and retains no request/response pointers.
 * @invariant An active service has non-zero `active_job_id`.
 * @invariant `next_sequence` and `next_offset` advance only after a successful read.
 * @since 0.1.0
 */
typedef struct ra8_mdl_service {
  ra8_mdl_service_backend_t backend;       /**< Injected raw-byte source mechanism.        */
  uint32_t                  next_job_id;   /**< Monotonic id candidate for the next Start. */
  uint32_t                  active_job_id; /**< Correlation id of the current job.         */
  uint32_t                  next_sequence; /**< Sequence required from the next pull.      */
  uint64_t                  next_offset;   /**< Byte offset required from the next pull.   */
  bool                      active;        /**< Whether Next or Cancel is currently valid. */
} ra8_mdl_service_t;

/**
 * @brief Initialise portable service state with a complete backend seam
 * @details Copies the callback table and resets all job correlation fields.
 * @param[out] service Caller-owned service state.
 * @param[in] backend Complete backend callback table and context.
 * @return Initialisation status.
 * @retval k_ra8_ok Service is ready to dispatch.
 * @retval k_ra8_err_null_ptr A pointer, callback, or backend context is null.
 * @pre No request is executing against @p service.
 * @pre @p backend and its context remain valid for the service lifetime.
 * @post Success leaves the service inactive and ready for Start.
 * @post Failure does not invoke a backend callback.
 * @note Not thread-safe with concurrent dispatch on the same service.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_mdl_service_init(ra8_mdl_service_t*               service,
                                             const ra8_mdl_service_backend_t* backend);

/**
 * @brief Dispatch Start, Next, or Cancel generated protobuf payloads
 * @details Decodes into a bounded stack arena, validates job correlation, and
 * packs exactly one response into caller-owned storage.
 * @param[in,out] ctx Initialised ::ra8_mdl_service_t.
 * @param[in] operation One ::ra8_mdl_rpc_id_t value.
 * @param[in] request Packed generated request bytes.
 * @param[in] request_len Valid request bytes.
 * @param[out] response Caller-owned response bytes.
 * @param[in] response_cap Capacity of @p response.
 * @param[out] response_len Packed response length.
 * @return Dispatch status.
 * @retval k_ra8_ok A response was packed successfully.
 * @retval k_ra8_err_null_ptr A required pointer is null.
 * @retval k_ra8_err_invalid_arg Operation or request fields are invalid.
 * @retval k_ra8_err_invalid_state A Start arrived while another job is active.
 * @retval k_ra8_err_invalid_size A fixed request/response bound was exceeded.
 * @retval k_ra8_err_protocol_error Job, sequence, or offset did not correlate.
 * @pre ::ra8_mdl_service_init succeeded for @p ctx.
 * @pre No concurrent call uses @p ctx or its backend.
 * @post Success sets @p response_len no larger than @p response_cap.
 * @post Failure sets @p response_len to zero after pointer validation.
 * @note Synchronous and single-owner.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_mdl_service_dispatch(void*          ctx,
                                                 uint32_t       operation,
                                                 const uint8_t* request,
                                                 size_t         request_len,
                                                 uint8_t*       response,
                                                 size_t         response_cap,
                                                 size_t*        response_len);

#ifdef __cplusplus
}
#endif
