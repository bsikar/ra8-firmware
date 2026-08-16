/**
 * @file ra8_mdl_protocol.h
 * @brief Allocation-free constants shared by the RA8 and C6 media endpoints
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details Defines bounded HTTPS-body transfer semantics plus the selected
 * artifact identity. Version 2 restores the typed format carried by Start and
 * echoed by Accepted. The C6 never receives a destination path; the RA8 still
 * validates and transactionally publishes the returned bytes.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdint.h>

/** @brief Bounded dimensions shared by both protocol endpoints. */
typedef enum : uint16_t {
  k_ra8_mdl_url_max        = 512U,  /**< Maximum URL buffer size, including NUL. */
  k_ra8_mdl_chunk_data_max = 1024U, /**< Maximum raw body bytes in one chunk.    */
  k_ra8_mdl_sha256_bytes   = 32U,   /**< SHA-256 digest size in bytes.           */
} ra8_mdl_dimension_t;

/** @brief Version included in every media RPC request and response. */
typedef enum : uint32_t {
  k_ra8_mdl_protocol_version = 2U, /**< Typed-artifact transfer protocol. */
} ra8_mdl_protocol_version_t;

/** @brief Stable CustomRpc operation IDs (`MD` + version + operation). */
typedef enum : uint32_t {
  k_ra8_mdl_rpc_start  = 0x4D440201U, /**< Start one typed-artifact job.   */
  k_ra8_mdl_rpc_next   = 0x4D440202U, /**< Pull one ordered bounded chunk. */
  k_ra8_mdl_rpc_cancel = 0x4D440203U, /**< Cancel one active job.          */
} ra8_mdl_rpc_id_t;

/** @brief Remote job state. */
typedef enum : uint8_t {
  k_ra8_mdl_state_accepted    = 1U, /**< Job was accepted but has no body bytes yet. */
  k_ra8_mdl_state_downloading = 2U, /**< Response carries non-empty ordered bytes.   */
  k_ra8_mdl_state_complete    = 3U, /**< Response carries terminal size and digest.  */
  k_ra8_mdl_state_cancelled   = 4U, /**< Job ended through explicit cancellation.    */
  k_ra8_mdl_state_failed      = 5U, /**< Job ended with a canonical error status.    */
} ra8_mdl_state_t;
