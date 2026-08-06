/**
 * @file ra8_c6link_mdl.h
 * @brief Client-side API for media download via ESP32-C6 RPC.
 */
#pragma once

#include <stdint.h>
#include "ra8_err.h"
#include "ra8_c6link.h"

/** @brief RPC message: download request */
typedef struct {
    char url[256];
    char output_path[256];
    uint8_t format;
} MdlDownloadRequest;

/** @brief RPC message: download progress */
typedef struct {
    uint32_t bytes_fetched;
    uint32_t total_bytes;
    uint8_t state;
} MdlDownloadProgress;

/** @brief RPC message: download result */
typedef struct {
    ra8_err_t status;
    char output_path[256];
    uint32_t file_size;
} MdlDownloadResult;

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initiates a download.
 * @param link Open handle to c6link.
 * @param url The URL to download.
 * @param output_path Path to save the downloaded file.
 * @return ra8_err_t Success or error code.
 */
ra8_err_t ra8_c6link_mdl_download(ra8_c6link_t* link, const char* url, const char* output_path);

/**
 * @brief Polls for the progress of an active download.
 * @param link Open handle to c6link.
 * @param progress Pointer to receive the progress data.
 * @return ra8_err_t Success or error code.
 */
ra8_err_t ra8_c6link_mdl_poll(ra8_c6link_t* link, MdlDownloadProgress* progress);

/**
 * @brief Cancels an in-flight download.
 * @param link Open handle to c6link.
 * @return ra8_err_t Success or error code.
 */
ra8_err_t ra8_c6link_mdl_cancel(ra8_c6link_t* link);

#ifdef __cplusplus
}
#endif
