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

void ra8_c6link_mdl_service_handle_rx(const uint8_t* rx_buf, uint16_t rx_len, uint8_t* tx_buf);

void ra8_c6link_mdl_service_handle_rx(const uint8_t* rx_buf, uint16_t rx_len, uint8_t* tx_buf)
{
  static ra8_mdl_download_progress_t s_progress;
  static bool                        s_download_active = false;

  /* mcdc-deactivated: stub service handler, null guards unreachable via RPC */
  if ((rx_buf != nullptr) && (tx_buf != nullptr) && (rx_len != 0U)) {
    const uint8_t msg_id = rx_buf[0];

    for (uint16_t i = 0U; i < 1600U; ++i) {
      tx_buf[i] = 0U;
    }

    switch (msg_id) {
      case 1U: { /* Req */
        ra8_mdl_download_req_t req;
        if (rx_len >= (1U + (uint16_t)sizeof(req))) {
          uint8_t* req_ptr = (uint8_t*)&req; /* cppcheck-suppress misra-c2012-11.5 */
          for (size_t i = 0U; i < sizeof(req); ++i) {
            req_ptr[i] = rx_buf[1U + i];
          }
          /* Start fake download */
          s_download_active         = true;
          s_progress.bytes_received = 0U;
          s_progress.total_bytes    = (1024U * 1024U); /* 1MB */
          s_progress.status         = 1U;              /* Downloading */
        }
        break;
      }
      case 2U: { /* Cancel */
        s_download_active = false;
        s_progress.status = 0U;
        break;
      }
      case 0U: { /* Poll (empty frame with msg_id 0) */
        if (s_download_active) {
          s_progress.bytes_received += 1024U;
          if (s_progress.bytes_received >= s_progress.total_bytes) {
            s_progress.status = 2U; /* Done */
            s_download_active = false;
          }

          tx_buf[0] = 3U; /* Progress */
          const uint8_t* const prog_ptr =
            (const uint8_t*)&s_progress; /* cppcheck-suppress misra-c2012-11.5 */
          for (size_t i = 0U; i < sizeof(s_progress); ++i) {
            tx_buf[1U + i] = prog_ptr[i];
          }
        }
        break;
      }
      default:
        break;
    }
  }
}
