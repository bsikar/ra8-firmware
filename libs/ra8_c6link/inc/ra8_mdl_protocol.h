/**
 * @file ra8_mdl_protocol.h
 * @brief Allocation-free constants shared by the RA8 and C6 media endpoints.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdint.h>

#define RA8_MDL_URL_MAX          (512U)
#define RA8_MDL_CHUNK_DATA_MAX   (1024U)
#define RA8_MDL_SHA256_BYTES     (32U)
#define RA8_MDL_PROTOCOL_VERSION (1U)

/** @brief Stable CustomRpc operation IDs (`MD` + version + operation). */
typedef enum : uint32_t {
  k_ra8_mdl_rpc_start  = 0x4D440101U,
  k_ra8_mdl_rpc_next   = 0x4D440102U,
  k_ra8_mdl_rpc_cancel = 0x4D440103U,
} ra8_mdl_rpc_id_t;

/** @brief Output format requested from the remote downloader. */
typedef enum : uint8_t {
  k_ra8_mdl_format_rabook = 1U,
  k_ra8_mdl_format_cbz    = 2U,
  k_ra8_mdl_format_epub   = 3U,
} ra8_mdl_format_t;

/** @brief Remote job state. */
typedef enum : uint8_t {
  k_ra8_mdl_state_accepted    = 1U,
  k_ra8_mdl_state_downloading = 2U,
  k_ra8_mdl_state_complete    = 3U,
  k_ra8_mdl_state_cancelled   = 4U,
  k_ra8_mdl_state_failed      = 5U,
} ra8_mdl_state_t;
