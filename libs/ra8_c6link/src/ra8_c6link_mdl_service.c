#include "ra8_c6link_mdl_msg.h"
#include <string.h>
#include <stdbool.h>

/* ESP32-C6 Service Stub */

static ra8_mdl_download_progress_t s_progress;
static bool s_download_active = false;

void ra8_c6link_mdl_service_handle_rx(const uint8_t* rx_buf, uint16_t rx_len, uint8_t* tx_buf) {
    if (!rx_buf || !tx_buf || rx_len == 0) return;
    
    uint8_t msg_id = rx_buf[0];
    
    memset(tx_buf, 0, 1600);
    
    switch (msg_id) {
        case 1: { /* Req */
            ra8_mdl_download_req_t req;
            if (rx_len >= 1 + sizeof(req)) {
                memcpy(&req, rx_buf + 1, sizeof(req));
                /* Start fake download */
                s_download_active = true;
                s_progress.bytes_received = 0;
                s_progress.total_bytes = 1024 * 1024; /* 1MB */
                s_progress.status = 1; /* Downloading */
            }
            break;
        }
        case 2: { /* Cancel */
            s_download_active = false;
            s_progress.status = 0;
            break;
        }
        case 0: { /* Poll (empty frame with msg_id 0) */
            if (s_download_active) {
                s_progress.bytes_received += 1024;
                if (s_progress.bytes_received >= s_progress.total_bytes) {
                    s_progress.status = 2; /* Done */
                    s_download_active = false;
                }
                
                tx_buf[0] = 3; /* Progress */
                memcpy(tx_buf + 1, &s_progress, sizeof(s_progress));
            }
            break;
        }
        default:
            break;
    }
}
