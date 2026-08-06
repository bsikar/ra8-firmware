/**
 * @file ra8_c6link_mdl_service.c
 * @brief ESP32-C6 RPC service handling for media download.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>

#include "ra8_c6link_mdl_msg.h"

/* ESP32-C6 Service Stub */

/** @brief Named constants for the media download service. */
enum {
  k_mdl_svc_frame_len  = 1600U, /**< Maximum RPC frame length in bytes. */
  k_mdl_svc_chunk_size = 1024U, /**< Simulated download chunk size.     */
};

/** @brief Simulated total download size (1 MiB). */
enum { k_mdl_svc_total_size = (k_mdl_svc_chunk_size * k_mdl_svc_chunk_size) };

/** @brief RPC message IDs. */
enum {
  k_mdl_msg_poll     = 0U, /**< Poll for download progress. */
  k_mdl_msg_req      = 1U, /**< Start a download request.   */
  k_mdl_msg_cancel   = 2U, /**< Cancel current download.    */
  k_mdl_msg_progress = 3U, /**< Progress response.          */
};

/** @brief Download status codes. */
enum {
  k_mdl_status_idle        = 0U, /**< No active download.   */
  k_mdl_status_downloading = 1U, /**< Download in progress. */
  k_mdl_status_done        = 2U, /**< Download complete.    */
};

static ra8_mdl_download_progress_t s_progress;
static bool                        s_download_active = false;

static void mdl_svc_handle_req(const uint8_t* rx_buf, uint16_t rx_len)
{
  ra8_mdl_download_req_t req;
  if (rx_len >= (1U + (uint16_t)sizeof(req))) {
    uint8_t* const req_bytes = (uint8_t*)&req; /* cppcheck-suppress misra-c2012-11.5 */
    for (size_t i = 0U; i < sizeof(req); ++i) {
      req_bytes[i] = rx_buf[1U + i];
    }
    s_download_active         = true;
    s_progress.bytes_received = 0U;
    s_progress.total_bytes    = k_mdl_svc_total_size;
    s_progress.status         = k_mdl_status_downloading;
  }
}

static void mdl_svc_handle_poll(uint8_t* tx_buf)
{
  if (s_download_active) {
    s_progress.bytes_received += k_mdl_svc_chunk_size;
    if (s_progress.bytes_received >= s_progress.total_bytes) {
      s_progress.status = k_mdl_status_done;
      s_download_active = false;
    }
    tx_buf[0] = k_mdl_msg_progress;
    const uint8_t* const prog_bytes =
      (const uint8_t*)&s_progress; /* cppcheck-suppress misra-c2012-11.5 */
    for (size_t i = 0U; i < sizeof(s_progress); ++i) {
      tx_buf[1U + i] = prog_bytes[i];
    }
  }
}

void ra8_c6link_mdl_service_handle_rx(const uint8_t* rx_buf, uint16_t rx_len, uint8_t* tx_buf)
{
  /* mcdc-deactivated: stub service handler, null guards unreachable via RPC */
  if ((rx_buf != nullptr) && (tx_buf != nullptr) && (rx_len != 0U)) {
    for (uint16_t i = 0U; i < (uint16_t)k_mdl_svc_frame_len; ++i) {
      tx_buf[i] = 0U;
    }

    switch (rx_buf[0]) {
      case k_mdl_msg_req:
        mdl_svc_handle_req(rx_buf, rx_len);
        break;
      case k_mdl_msg_cancel:
        s_download_active = false;
        s_progress.status = k_mdl_status_idle;
        break;
      case k_mdl_msg_poll:
        mdl_svc_handle_poll(tx_buf);
        break;
      default:
        break;
    }
  }
}
