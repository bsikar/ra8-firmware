/**
 * @file ra8_c6link_mdl_msg.h
 * @brief Media download RPC message definitions.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#define RA8_MDL_MSG_URL_MAX    (256U)
#define RA8_MDL_MSG_PATH_MAX   (256U)
#define RA8_MDL_CHUNK_DATA_MAX (1024U)

#pragma pack(push, 1)

typedef struct {
  char     url[RA8_MDL_MSG_URL_MAX];
  char     dest_path[RA8_MDL_MSG_PATH_MAX];
  uint32_t chapter_info;
} ra8_mdl_download_req_t;

typedef struct {
  uint32_t bytes_received;
  uint32_t total_bytes;
  uint32_t status;
} ra8_mdl_download_progress_t;

typedef struct {
  uint32_t chunk_length;
  uint8_t  data[RA8_MDL_CHUNK_DATA_MAX];
} ra8_mdl_download_chunk_t;

typedef struct {
  uint8_t dummy; /* Empty struct is not allowed in standard C, using a dummy field */
} ra8_mdl_cancel_req_t;

#pragma pack(pop)

/**
 * @brief Handle received RPC message.
 * @since 0.1.0
 */
void ra8_c6link_mdl_service_handle_rx(const uint8_t* rx_buf, uint16_t rx_len, uint8_t* tx_buf);

#ifdef __cplusplus
}
#endif
