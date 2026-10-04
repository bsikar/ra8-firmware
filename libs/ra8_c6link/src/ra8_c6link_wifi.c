/**
 * @file ra8_c6link_wifi.c
 * @brief The shared Wi-Fi response extractor.
 * @ingroup grp_net
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details
 * The response extractor every Wi-Fi request hands the RPC layer. The
 * lifecycle exports (start, stop, leave) are Zig in
 * `ra8_c6link_wifi_abi.zig`, the empty-body request builder is Zig in
 * `ra8_c6link_bare_abi.zig`, and the `Req_WifiInit` configuration is
 * documented field by field in `internal/wifi_init.zig`.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include "ra8_c6link_wifi.h"

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_c6link.h"
#include "ra8_c6link_internal.h"

RA8_PRIV ra8_err_t priv_c6link_take_resp(void* ctx, const void* msg_v)
{
  ra8_c6link_take_ctx_t* take = (ra8_c6link_take_ctx_t*)ctx;
  const Rpc*             msg  = (const Rpc*)msg_v;
  int32_t                resp = 0;

  switch ((int32_t)msg->msg_id) {
    case (int32_t)RPC_ID__Resp_WifiInit:
      resp = (msg->resp_wifi_init != nullptr) ? msg->resp_wifi_init->resp : -1;
      break;
    case (int32_t)RPC_ID__Resp_SetWifiMode:
      resp = (msg->resp_set_wifi_mode != nullptr) ? msg->resp_set_wifi_mode->resp : -1;
      break;
    case (int32_t)RPC_ID__Resp_WifiSetConfig:
      resp = (msg->resp_wifi_set_config != nullptr) ? msg->resp_wifi_set_config->resp : -1;
      break;
    case (int32_t)RPC_ID__Resp_WifiStart:
      resp = (msg->resp_wifi_start != nullptr) ? msg->resp_wifi_start->resp : -1;
      break;
    case (int32_t)RPC_ID__Resp_WifiStop:
      resp = (msg->resp_wifi_stop != nullptr) ? msg->resp_wifi_stop->resp : -1;
      break;
    case (int32_t)RPC_ID__Resp_WifiDeinit:
      resp = (msg->resp_wifi_deinit != nullptr) ? msg->resp_wifi_deinit->resp : -1;
      break;
    case (int32_t)RPC_ID__Resp_WifiConnect:
      resp = (msg->resp_wifi_connect != nullptr) ? msg->resp_wifi_connect->resp : -1;
      break;
    case (int32_t)RPC_ID__Resp_WifiDisconnect:
      resp = (msg->resp_wifi_disconnect != nullptr) ? msg->resp_wifi_disconnect->resp : -1;
      break;
    default:
      resp = -1;
      break;
  }
  return priv_c6link_resp(take->link, take->rpc_id, resp);
}
