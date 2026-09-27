/**
 * @file ra8_mdl_protocol.h
 * @brief Allocation-free constants shared by the RA8 and C6 media endpoints
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details Defines bounded HTTPS-body transfer semantics plus the selected
 * artifact identity and downloader request/response policy. Version 3 carries
 * the conditional request fields and response metadata required by portable
 * `mdl_fetch` instead of fabricating an HTTP status on the RA8. The C6 never
 * receives a destination path; the RA8 still validates and transactionally
 * publishes the returned bytes.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdint.h>

#include "ra8_mdl_http.h"

/**
 * @brief Bounded framing dimensions shared by both protocol endpoints.
 * @details The HTTP header capacities this protocol packs live in
 * `ra8_mdl_http.h` with the records they size (::ra8_mdl_http_capacity_t);
 * what stays here is the framing the RPC itself owns.
 */
typedef enum : uint16_t {
  k_ra8_mdl_url_max        = 512U,  /**< Maximum URL buffer size, including NUL. */
  k_ra8_mdl_chunk_data_max = 1024U, /**< Maximum raw body bytes in one chunk.    */
  k_ra8_mdl_sha256_bytes   = 32U,   /**< SHA-256 digest size in bytes.           */
} ra8_mdl_dimension_t;

/**
 * @brief Largest packed inner request either endpoint has to hold
 * @details A Start request carries the bounded URL and every bounded HTTP
 * header; the constant adds the generated encoder's per-field tag and varint
 * overhead on top. Next and Cancel are far smaller and share the buffer.
 *
 * @note The operands are cast to `uint16_t` because the URL bound is framing
 * (::ra8_mdl_dimension_t) while the header bounds are contract
 * (::ra8_mdl_http_capacity_t), and adding two enumeration types is refused
 * under `-Wenum-enum-conversion`. The cast is the sum's width, not a narrowing.
 * @invariant No legal packed inner request exceeds this bound.
 * @see ra8_c6link_mdl_start_request
 * @since 0.1.0
 */
typedef enum : uint16_t {
  k_ra8_mdl_request_bytes_max =
      (uint16_t)k_ra8_mdl_url_max + (uint16_t)k_ra8_mdl_user_agent_max +
      (uint16_t)k_ra8_mdl_referer_max + (uint16_t)k_ra8_mdl_etag_max +
      (uint16_t)k_ra8_mdl_http_date_max + 96U, /**< Maximum packed request bytes. */
} ra8_mdl_request_bound_t;

/** @brief Version included in every media RPC request and response. */
typedef enum : uint32_t {
  k_ra8_mdl_protocol_version = 3U, /**< Typed HTTP-artifact transfer protocol. */
} ra8_mdl_protocol_version_t;

/** @brief Stable CustomRpc operation IDs (`MD` + version + operation). */
typedef enum : uint32_t {
  k_ra8_mdl_rpc_start  = 0x4D440301U, /**< Start one typed-artifact job.   */
  k_ra8_mdl_rpc_next   = 0x4D440302U, /**< Pull one ordered bounded chunk. */
  k_ra8_mdl_rpc_cancel = 0x4D440303U, /**< Cancel one active job.          */
} ra8_mdl_rpc_id_t;

/** @brief Remote job state. */
typedef enum : uint8_t {
  k_ra8_mdl_state_accepted    = 1U, /**< Job was accepted but has no body bytes yet. */
  k_ra8_mdl_state_downloading = 2U, /**< Response carries non-empty ordered bytes.   */
  k_ra8_mdl_state_complete    = 3U, /**< Response carries terminal size and digest.  */
  k_ra8_mdl_state_cancelled   = 4U, /**< Job ended through explicit cancellation.    */
  k_ra8_mdl_state_failed      = 5U, /**< Job ended with a canonical error status.    */
} ra8_mdl_state_t;
