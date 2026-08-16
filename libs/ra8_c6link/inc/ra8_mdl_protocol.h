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

/** @brief Bounded dimensions shared by both protocol endpoints. */
typedef enum : uint16_t {
  k_ra8_mdl_url_max          = 512U,   /**< Maximum URL buffer size, including NUL.     */
  k_ra8_mdl_user_agent_max   = 256U,   /**< Maximum User-Agent size, including NUL.     */
  k_ra8_mdl_referer_max      = 512U,   /**< Maximum Referer size, including NUL.        */
  k_ra8_mdl_etag_max         = 128U,   /**< Maximum ETag size, including NUL.           */
  k_ra8_mdl_http_date_max    = 64U,    /**< Maximum HTTP-date size, including NUL.      */
  k_ra8_mdl_retry_after_max  = 64U,    /**< Maximum Retry-After size, including NUL.    */
  k_ra8_mdl_content_type_max = 128U,   /**< Maximum Content-Type size, including NUL.   */
  k_ra8_mdl_chunk_data_max   = 1024U,  /**< Maximum raw body bytes in one chunk.        */
  k_ra8_mdl_sha256_bytes     = 32U,    /**< SHA-256 digest size in bytes.               */
  k_ra8_mdl_timeout_ms_max   = 60000U, /**< Maximum caller-selected HTTP timeout in ms. */
} ra8_mdl_dimension_t;

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
