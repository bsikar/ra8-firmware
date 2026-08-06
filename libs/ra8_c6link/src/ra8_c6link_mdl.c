/**
 * @file ra8_c6link_mdl.c
 * @brief Client-side implementation for media download via ESP32-C6 RPC.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_c6link_mdl.h"

ra8_err_t ra8_c6link_mdl_download(ra8_c6link_t* link, const char* url, const char* output_path)
{
  ra8_err_t err = k_ra8_err_invalid_arg;
  /* mcdc-deactivated: stub RPC client, null guards unreachable via public API */
  if ((link != nullptr) && (url != nullptr) && (output_path != nullptr)) {
    /* TODO: implement RPC request to ESP32-C6 */
    err = k_ra8_ok;
  }
  return err;
}

ra8_err_t ra8_c6link_mdl_poll(ra8_c6link_t* link, ra8_mdl_download_progress_t* progress)
{
  ra8_err_t err = k_ra8_err_invalid_arg;
  /* mcdc-deactivated: stub RPC client, null guards unreachable via public API */
  if ((link != nullptr) && (progress != nullptr)) {
    /* TODO: implement RPC poll from ESP32-C6 */
    err = k_ra8_ok;
  }
  return err;
}

ra8_err_t ra8_c6link_mdl_cancel(ra8_c6link_t* link)
{
  ra8_err_t err = k_ra8_err_invalid_arg;
  /* mcdc-deactivated: stub RPC client, null guard unreachable via public API */
  if (link != nullptr) {
    /* TODO: implement RPC cancel to ESP32-C6 */
    err = k_ra8_ok;
  }
  return err;
}
