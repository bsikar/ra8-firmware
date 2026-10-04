/**
 * @file ra8_c6link_wifi.c
 * @brief Shared Wi-Fi response extractor and the bare-request machinery.
 * @ingroup grp_net
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details
 * The response extractor every Wi-Fi request hands the RPC layer, plus the
 * shared machinery for the requests whose body is empty and whose answer is a
 * bare result code. The lifecycle exports themselves (start, stop, leave) are
 * Zig now, in `ra8_c6link_wifi_abi.zig`, and the `Req_WifiInit` configuration
 * is documented field by field in `internal/wifi_init.zig`.
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

/**
 * @struct ra8_c6link_bare_body
 * @brief Storage for whichever empty request body a bare call needs.
 *
 * @details
 * All five bodies are a bare `ProtobufCMessage`, but each has its own type and
 * its own generated initialiser, so one aggregate gives the caller a single
 * stack object with no cast between unrelated structure types.
 *
 * A struct rather than a union: only one member is ever used, the members are
 * tiny, and MISRA Rule 19.2 discourages unions for an aliasing hazard that does
 * not arise here -- so a few dozen bytes of stack buys a rule this file simply
 * obeys instead of deviating from.
 *
 * @invariant Exactly one member is initialised per request.
 * @invariant The aggregate outlives the `Rpc` that points into it, which a
 *            caller stack frame guarantees.
 *
 * @par Example:
 * @code
 * ra8_c6link_bare_body_t body;
 * rpc__req__wifi_start__init(&body.start);
 * @endcode
 *
 * @see priv_c6link_bare_req
 * @since 0.1.0
 */
typedef struct ra8_c6link_bare_body {
  RpcReqWifiStart      start;      /**< `Req_WifiStart` body.      */
  RpcReqWifiStop       stop;       /**< `Req_WifiStop` body.       */
  RpcReqWifiDeinit     deinit;     /**< `Req_WifiDeinit` body.     */
  RpcReqWifiConnect    connect;    /**< `Req_WifiConnect` body.    */
  RpcReqWifiDisconnect disconnect; /**< `Req_WifiDisconnect` body. */
} ra8_c6link_bare_body_t;

/**
 * @brief Populate an `Rpc` with the empty body a bare request needs.
 * @details Five requests share the shape 'empty body, result code back' but
 *        not their generated types, so this is where the one that was asked
 *        for is selected.
 * @param[out] req Message to populate; must be non-null.
 * @param[out] body Storage for the empty body; must be non-null.
 * @param[in] req_id `RPC_ID__Req_*` to send.
 * @param[out] resp_id `RPC_ID__Resp_*` that answers it; must be non-null.
 * @return true when @p req_id is one this helper knows.
 * @retval true @p req carries the right body and @p resp_id names its answer.
 * @retval false @p req_id is not a bare request.
 * @pre @p body outlives the request, which a caller stack frame guarantees.
 * @pre @p req has been initialised by `rpc__init()`.
 * @post On true the payload case matches @p req_id.
 * @post On false @p resp_id is zero.
 * @note Which id answers which is not decided here: `priv_c6link_bare_resp`
 *       holds the pairing and is tested on it. This switch carries only the
 *       generated body, so the two must agree on the same five requests; when
 *       they do not the call is refused rather than sent to wait on an answer
 *       that will never match.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_c6link_wifi_bare_body(Rpc*                    req,
                                                        ra8_c6link_bare_body_t* body,
                                                        uint32_t                req_id,
                                                        uint32_t*               resp_id)
{
  switch ((int32_t)req_id) {
    case (int32_t)RPC_ID__Req_WifiStart:
      rpc__req__wifi_start__init(&body->start);
      req->payload_case   = RPC__PAYLOAD_REQ_WIFI_START;
      req->req_wifi_start = &body->start;
      break;
    case (int32_t)RPC_ID__Req_WifiStop:
      rpc__req__wifi_stop__init(&body->stop);
      req->payload_case  = RPC__PAYLOAD_REQ_WIFI_STOP;
      req->req_wifi_stop = &body->stop;
      break;
    case (int32_t)RPC_ID__Req_WifiDeinit:
      rpc__req__wifi_deinit__init(&body->deinit);
      req->payload_case    = RPC__PAYLOAD_REQ_WIFI_DEINIT;
      req->req_wifi_deinit = &body->deinit;
      break;
    case (int32_t)RPC_ID__Req_WifiConnect:
      rpc__req__wifi_connect__init(&body->connect);
      req->payload_case     = RPC__PAYLOAD_REQ_WIFI_CONNECT;
      req->req_wifi_connect = &body->connect;
      break;
    case (int32_t)RPC_ID__Req_WifiDisconnect:
      rpc__req__wifi_disconnect__init(&body->disconnect);
      req->payload_case        = RPC__PAYLOAD_REQ_WIFI_DISCONNECT;
      req->req_wifi_disconnect = &body->disconnect;
      break;
    default:
      *resp_id = 0U;
      return false;
  }
  return priv_c6link_bare_resp(req_id, resp_id);
}

RA8_PRIV ra8_err_t priv_c6link_bare_req(ra8_c6link_t* link, uint32_t req_id)
{
  if (link == nullptr) {
    return k_ra8_err_null_ptr;
  }

  Rpc req;
  rpc__init(&req);
  req.msg_type = RPC_TYPE__Req;
  req.msg_id   = (RpcId)req_id;

  ra8_c6link_bare_body_t body;
  uint32_t               resp_id = 0U;
  if (!internal_c6link_wifi_bare_body(&req, &body, req_id, &resp_id)) {
    return k_ra8_err_not_supported;
  }

  ra8_c6link_take_ctx_t take = {.link = link, .out = nullptr, .rpc_id = req_id};
  return priv_c6link_rpc_call(link, &req, resp_id, priv_c6link_take_resp, &take);
}
