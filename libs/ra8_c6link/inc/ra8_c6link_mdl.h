/**
 * @file ra8_c6link_mdl.h
 * @brief Client-side API for media download via ESP32-C6 RPC.
 * @ingroup grp_ereader
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */
#pragma once

#include <stdint.h>

#include "ra8_c6link.h"
#include "ra8_err.h"

/** Maximum URL length for a download request. */
#define RA8_MDL_URL_MAX 256U

/** Maximum output path length for a download request. */
#define RA8_MDL_PATH_MAX 256U

/** @brief RPC message: download request.
 *  @since 0.1.0 */
typedef struct {
  char    url[RA8_MDL_URL_MAX];
  char    output_path[RA8_MDL_PATH_MAX];
  uint8_t format;
} ra8_mdl_download_req_t;

/** @brief RPC message: download progress.
 *  @since 0.1.0 */
typedef struct {
  uint32_t bytes_fetched;
  uint32_t total_bytes;
  uint8_t  state;
} ra8_mdl_download_progress_t;

/** @brief RPC message: download result.
 *  @since 0.1.0 */
typedef struct {
  ra8_err_t status;
  char      output_path[RA8_MDL_PATH_MAX];
  uint32_t  file_size;
} ra8_mdl_download_result_t;

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initiates a download.
 *
 * @param[in] link        Open handle to c6link.
 * @param[in] url         The URL to download.
 * @param[in] output_path Path to save the downloaded file.
 *
 * @return ra8_err_t Success or error code.
 *
 * @since 0.1.0
 */
ra8_err_t ra8_c6link_mdl_download(ra8_c6link_t* link, const char* url, const char* output_path);

/**
 * @brief Polls for the progress of an active download.
 *
 * @param[in]  link     Open handle to c6link.
 * @param[out] progress Pointer to receive the progress data.
 *
 * @return ra8_err_t Success or error code.
 *
 * @since 0.1.0
 */
ra8_err_t ra8_c6link_mdl_poll(ra8_c6link_t* link, ra8_mdl_download_progress_t* progress);

/**
 * @brief Cancels an in-flight download.
 *
 * @param[in] link Open handle to c6link.
 *
 * @return ra8_err_t Success or error code.
 *
 * @since 0.1.0
 */
ra8_err_t ra8_c6link_mdl_cancel(ra8_c6link_t* link);

#ifdef __cplusplus
}
#endif
