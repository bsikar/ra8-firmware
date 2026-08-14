/**
 * @file ra8_c6link_mdl_msg.h
 * @brief Media service dispatch contract for the ESP32-C6 integration.
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
 * @brief Handle one inner media protobuf request in a bounded response buffer.
 * @return Error when the operation or protobuf is invalid or response overflows.
 */
typedef ra8_err_t (*ra8_mdl_service_dispatch_fn)(void*          ctx,
                                                 uint32_t       operation,
                                                 const uint8_t* request,
                                                 size_t         request_len,
                                                 uint8_t*       response,
                                                 size_t         response_cap,
                                                 size_t*        response_len);

/** @brief Bounded asynchronous HTTP seam implemented by the C6 port. */
typedef struct {
  ra8_err_t (*begin)(void* ctx, const char* url, ra8_mdl_format_t format);
  ra8_err_t (*read)(void*     ctx,
                    uint8_t*  out,
                    uint16_t  cap,
                    uint16_t* got,
                    uint64_t* total_bytes,
                    bool*     complete,
                    uint8_t   sha256[RA8_MDL_SHA256_BYTES]);
  ra8_err_t (*cancel)(void* ctx);
  void* ctx;
} ra8_mdl_service_backend_t;

/** @brief One-job service state; caller allocates it statically. */
typedef struct {
  ra8_mdl_service_backend_t backend;
  uint32_t                  next_job_id;
  uint32_t                  active_job_id;
  uint32_t                  next_sequence;
  uint64_t                  next_offset;
  bool                      active;
} ra8_mdl_service_t;

/** @brief Initialise a service with a complete backend seam. */
[[nodiscard]] ra8_err_t ra8_mdl_service_init(ra8_mdl_service_t*               service,
                                             const ra8_mdl_service_backend_t* backend);

/** @brief Dispatch Start, Next, or Cancel inner protobuf payloads. */
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
