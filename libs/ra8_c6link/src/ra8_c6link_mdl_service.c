/**
 * @file ra8_c6link_mdl_service.c
 * @brief ESP32-C6 RPC service handling for media download.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdbool.h>
#include <string.h>

#include "ra8_c6link_mdl_msg.h"

/* ESP32-C6 Service Stub */

/** @brief Maximum RPC frame length in bytes. */
#define k_mdl_svc_frame_len ((uint16_t)1600U)

/** @brief Simulated download chunk size in bytes. */
#define k_mdl_svc_chunk_size ((uint32_t)1024U)

/** @brief Simulated total download size (1 MiB). */
#define k_mdl_svc_total_size (k_mdl_svc_chunk_size * k_mdl_svc_chunk_size)

/** @brief RPC message IDs. */
#define k_mdl_msg_poll     ((uint8_t)0U)
#define k_mdl_msg_req      ((uint8_t)1U)
#define k_mdl_msg_cancel   ((uint8_t)2U)
#define k_mdl_msg_progress ((uint8_t)3U)

/** @brief Download status codes. */
#define k_mdl_status_idle        ((uint8_t)0U)
#define k_mdl_status_downloading ((uint8_t)1U)
#define k_mdl_status_done        ((uint8_t)2U)

static ra8_mdl_download_progress_t s_progress;
static bool                        s_download_active = false;

static void mdl_svc_handle_req(const uint8_t* rx_buf, uint16_t rx_len)
{
  ra8_mdl_download_req_t req;
  if (rx_len >= (1U + (uint16_t)sizeof(req))) {
    (void)memcpy(&req, &rx_buf[1], sizeof(req));
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
    (void)memcpy(&tx_buf[1], &s_progress, sizeof(s_progress));
  }
}

void ra8_c6link_mdl_service_handle_rx(const uint8_t* rx_buf, uint16_t rx_len, uint8_t* tx_buf)
{
  /* mcdc-deactivated: stub service handler, null guards unreachable via RPC */
  if ((rx_buf != nullptr) && (tx_buf != nullptr) && (rx_len != 0U)) {
    (void)memset(tx_buf, 0, k_mdl_svc_frame_len);

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
