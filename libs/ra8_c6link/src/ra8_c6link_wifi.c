/**
 * @file ra8_c6link_wifi.c
 * @brief Radio lifecycle: initialise, choose station mode, start, tear down.
 * @ingroup grp_net
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details
 * Five of the eleven RPC ids a station join needs live here, plus the shared
 * machinery for the requests whose body is empty and whose answer is a bare
 * result code.
 *
 * @par The initialisation configuration, field by field
 * `Req_WifiInit` is the one request that carries real numbers rather than a
 * credential. On an ESP-IDF host those numbers come from
 * `WIFI_INIT_CONFIG_DEFAULT()`, a macro this host does not have; the
 * co-processor validates the `magic` word and then uses the rest to size its
 * own buffers. ::ra8_c6link_wifi_init_t restates that default set for an
 * ESP32-C6 built at the pinned IDF version, with the reason for each value
 * beside it, so a mismatch is visible rather than buried in a macro expansion
 * nobody in this tree can read.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * @since 0.1.0
 */

#include "ra8_c6link_wifi.h"

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_c6link.h"
#include "ra8_c6link_internal.h"

/**
 * @enum ra8_c6link_wifi_mode_t
 * @brief The co-processor's own `wifi_mode_t` values, as transmitted.
 *
 * @details
 * These cross the link as a plain `int32_t`, so they are the co-processor's
 * numbering and not this host's choice. Only the station value is used; the
 * others are named so the transmitted number is never a bare literal.
 *
 * @invariant ::k_ra8_c6link_mode_sta is what `Req_SetWifiMode` must carry for
 *            a station.
 * @invariant The numbering matches ESP-IDF's `wifi_mode_t`, which the
 *            co-processor decodes it as.
 *
 * @par Example:
 * @code
 * body.mode = (int32_t)k_ra8_c6link_mode_sta;
 * @endcode
 *
 * @see ra8_c6link_wifi_start
 * @since 0.1.0
 */
typedef enum : int32_t {
  k_ra8_c6link_mode_null = 0, /**< Radio configured for neither role. */
  k_ra8_c6link_mode_sta  = 1, /**< Station.                           */
  k_ra8_c6link_mode_ap   = 2, /**< Access point.                      */
} ra8_c6link_wifi_mode_t;

/**
 * @enum ra8_c6link_wifi_init_t
 * @brief The `Req_WifiInit` configuration this host transmits.
 *
 * @details
 * ESP-IDF's default set for an ESP32-C6, restated. The co-processor supplies
 * its own OS and crypto function tables -- those are pointers and are not on
 * the wire at all -- and takes these scalars from the request.
 *
 * @invariant ::k_ra8_c6link_wifi_magic is the word `esp_wifi_init()` checks
 *            before it looks at anything else; a wrong value is refused with
 *            `ESP_ERR_INVALID_ARG` and nothing else happens.
 * @invariant Every buffer count is within the range the co-processor's own
 *            build accepts, which is what makes the request succeed rather
 *            than merely arrive.
 *
 * @par Example:
 * @code
 * cfg.magic = (int32_t)k_ra8_c6link_wifi_magic;
 * @endcode
 *
 * @see ra8_c6link_wifi_start
 * @since 0.1.0
 */
typedef enum : int32_t {
  k_ra8_c6link_wifi_magic = 0x1F2F3F4F,
  /**< `WIFI_INIT_CONFIG_MAGIC`; the first thing the far side validates. */
  k_ra8_c6link_wifi_static_rx = 10,
  /**< Static receive buffers. */
  k_ra8_c6link_wifi_dynamic_rx = 32,
  /**< Dynamic receive buffers. */
  k_ra8_c6link_wifi_tx_type = 1,
  /**< Transmit buffer type: dynamic, which is the IDF default. */
  k_ra8_c6link_wifi_static_tx = 0,
  /**< Static transmit buffers; zero because the type above is dynamic. */
  k_ra8_c6link_wifi_dynamic_tx = 32,
  /**< Dynamic transmit buffers. */
  k_ra8_c6link_wifi_rx_mgmt_type = 0,
  /**< Management receive buffers are static by default. */
  k_ra8_c6link_wifi_rx_mgmt_num = 5,
  /**< Management receive buffers. */
  k_ra8_c6link_wifi_ampdu_on = 1,
  /**< Aggregation enabled in both directions, as IDF defaults it. */
  k_ra8_c6link_wifi_nvs_on = 1,
  /**< Let the co-processor persist calibration in its own NVS. */
  k_ra8_c6link_wifi_ba_win = 6,
  /**< Block-ack window. */
  k_ra8_c6link_wifi_beacon_max = 752,
  /**< Longest beacon the soft-AP path would build; unused by a station but
       part of the validated set. */
  k_ra8_c6link_wifi_mgmt_sbuf = 32,
  /**< Management short-buffer count. */
  k_ra8_c6link_wifi_feature_caps = 1,
  /**< Feature bitmap; bit zero is WPA3-SAE, which the bench network does not
       use but which costs nothing to advertise. */
  k_ra8_c6link_wifi_espnow_keys = 7,
  /**< ESP-NOW encrypted peer slots. */
  k_ra8_c6link_wifi_hetb_queues = 3,
  /**< HE trigger-based queues. */
} ra8_c6link_wifi_init_t;

RA8_PRIV ra8_err_t ra8_c6link_priv_take_resp(void* ctx, const void* msg_v)
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
  return ra8_c6link_priv_resp(take->link, take->rpc_id, resp);
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
 * @see ra8_c6link_priv_bare_req
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
 * @post On false @p req is left with no payload and @p resp_id is zero.
 * @note The answer id is stated per arm rather than derived by arithmetic on
 *       the request id: the two happen to differ by a constant today, and a
 *       facade that silently depended on that would break quietly.
 * @since 0.1.0
 */
RA8_INTERNAL static bool ra8_c6link_wifi_bare_body(Rpc*                    req,
                                                   ra8_c6link_bare_body_t* body,
                                                   uint32_t                req_id,
                                                   uint32_t*               resp_id)
{
  *resp_id = 0U;
  switch ((int32_t)req_id) {
    case (int32_t)RPC_ID__Req_WifiStart:
      rpc__req__wifi_start__init(&body->start);
      req->payload_case   = RPC__PAYLOAD_REQ_WIFI_START;
      req->req_wifi_start = &body->start;
      *resp_id            = (uint32_t)RPC_ID__Resp_WifiStart;
      break;
    case (int32_t)RPC_ID__Req_WifiStop:
      rpc__req__wifi_stop__init(&body->stop);
      req->payload_case  = RPC__PAYLOAD_REQ_WIFI_STOP;
      req->req_wifi_stop = &body->stop;
      *resp_id           = (uint32_t)RPC_ID__Resp_WifiStop;
      break;
    case (int32_t)RPC_ID__Req_WifiDeinit:
      rpc__req__wifi_deinit__init(&body->deinit);
      req->payload_case    = RPC__PAYLOAD_REQ_WIFI_DEINIT;
      req->req_wifi_deinit = &body->deinit;
      *resp_id             = (uint32_t)RPC_ID__Resp_WifiDeinit;
      break;
    case (int32_t)RPC_ID__Req_WifiConnect:
      rpc__req__wifi_connect__init(&body->connect);
      req->payload_case     = RPC__PAYLOAD_REQ_WIFI_CONNECT;
      req->req_wifi_connect = &body->connect;
      *resp_id              = (uint32_t)RPC_ID__Resp_WifiConnect;
      break;
    case (int32_t)RPC_ID__Req_WifiDisconnect:
      rpc__req__wifi_disconnect__init(&body->disconnect);
      req->payload_case        = RPC__PAYLOAD_REQ_WIFI_DISCONNECT;
      req->req_wifi_disconnect = &body->disconnect;
      *resp_id                 = (uint32_t)RPC_ID__Resp_WifiDisconnect;
      break;
    default:
      return false;
  }
  return true;
}

RA8_PRIV ra8_err_t ra8_c6link_priv_bare_req(ra8_c6link_t* link, uint32_t req_id)
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
  if (!ra8_c6link_wifi_bare_body(&req, &body, req_id, &resp_id)) {
    return k_ra8_err_not_supported;
  }

  ra8_c6link_take_ctx_t take = {.link = link, .out = nullptr, .rpc_id = req_id};
  return ra8_c6link_priv_rpc_call(link, &req, resp_id, ra8_c6link_priv_take_resp, &take);
}

/**
 * @brief Send `Req_WifiInit` carrying the default configuration set.
 * @details The one request that carries real numbers rather than a credential.
 *        Every value is documented in ::ra8_c6link_wifi_init_t, and the
 *        co-processor's own `esp_wifi_init()` is what validates them.
 * @param[in,out] link Open handle; must be non-null.
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok The co-processor initialised its Wi-Fi stack.
 * @retval k_ra8_err_timeout It did not answer within the budget.
 * @retval k_ra8_err_protocol_error It refused; the fault slot carries its
 *         `esp_err_t`, and `ESP_ERR_INVALID_ARG` there means the magic word or
 *         a buffer count was not one this co-processor build accepts.
 * @retval k_ra8_err_spi_error The transport refused a transfer.
 * @pre @p link is open and the co-processor has booted.
 * @pre The radio is not already initialised.
 * @post On success the co-processor's Wi-Fi stack exists but is not started.
 * @post On failure the fault slot names this request.
 * @note Every value transmitted is documented in ::ra8_c6link_wifi_init_t.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t ra8_c6link_wifi_do_init(ra8_c6link_t* link)
{
  WifiInitConfig cfg;
  wifi_init_config__init(&cfg);
  cfg.static_rx_buf_num      = (int32_t)k_ra8_c6link_wifi_static_rx;
  cfg.dynamic_rx_buf_num     = (int32_t)k_ra8_c6link_wifi_dynamic_rx;
  cfg.tx_buf_type            = (int32_t)k_ra8_c6link_wifi_tx_type;
  cfg.static_tx_buf_num      = (int32_t)k_ra8_c6link_wifi_static_tx;
  cfg.dynamic_tx_buf_num     = (int32_t)k_ra8_c6link_wifi_dynamic_tx;
  cfg.rx_mgmt_buf_type       = (int32_t)k_ra8_c6link_wifi_rx_mgmt_type;
  cfg.rx_mgmt_buf_num        = (int32_t)k_ra8_c6link_wifi_rx_mgmt_num;
  cfg.ampdu_rx_enable        = (int32_t)k_ra8_c6link_wifi_ampdu_on;
  cfg.ampdu_tx_enable        = (int32_t)k_ra8_c6link_wifi_ampdu_on;
  cfg.nvs_enable             = (int32_t)k_ra8_c6link_wifi_nvs_on;
  cfg.rx_ba_win              = (int32_t)k_ra8_c6link_wifi_ba_win;
  cfg.beacon_max_len         = (int32_t)k_ra8_c6link_wifi_beacon_max;
  cfg.mgmt_sbuf_num          = (int32_t)k_ra8_c6link_wifi_mgmt_sbuf;
  cfg.feature_caps           = (uint64_t)k_ra8_c6link_wifi_feature_caps;
  cfg.sta_disconnected_pm    = true;
  cfg.espnow_max_encrypt_num = (int32_t)k_ra8_c6link_wifi_espnow_keys;
  cfg.tx_hetb_queue_num      = (int32_t)k_ra8_c6link_wifi_hetb_queues;
  cfg.magic                  = (int32_t)k_ra8_c6link_wifi_magic;

  RpcReqWifiInit body;
  rpc__req__wifi_init__init(&body);
  body.cfg = &cfg;

  Rpc req;
  rpc__init(&req);
  req.msg_type      = RPC_TYPE__Req;
  req.msg_id        = RPC_ID__Req_WifiInit;
  req.payload_case  = RPC__PAYLOAD_REQ_WIFI_INIT;
  req.req_wifi_init = &body;

  ra8_c6link_take_ctx_t take = {.link   = link,
                                .out    = nullptr,
                                .rpc_id = (uint32_t)RPC_ID__Req_WifiInit};
  return ra8_c6link_priv_rpc_call(link,
                                  &req,
                                  (uint32_t)RPC_ID__Resp_WifiInit,
                                  ra8_c6link_priv_take_resp,
                                  &take);
}

/**
 * @brief Send `Req_SetWifiMode` selecting station mode.
 * @details Fixes the radio's role before any credential is sent, which is the
 *        order ESP-IDF's own station bring-up uses.
 * @param[in,out] link Open handle; must be non-null.
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok The co-processor is in station mode.
 * @retval k_ra8_err_timeout It did not answer within the budget.
 * @retval k_ra8_err_protocol_error It refused the mode.
 * @retval k_ra8_err_spi_error The transport refused a transfer.
 * @pre ::ra8_c6link_wifi_do_init has succeeded.
 * @pre @p link is open.
 * @post On success the radio's role is fixed until it is set again.
 * @post On failure the fault slot names this request.
 * @note The mode number is the co-processor's `wifi_mode_t`, not a local id.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t ra8_c6link_wifi_do_mode(ra8_c6link_t* link)
{
  RpcReqSetMode body;
  rpc__req__set_mode__init(&body);
  body.mode = (int32_t)k_ra8_c6link_mode_sta;

  Rpc req;
  rpc__init(&req);
  req.msg_type          = RPC_TYPE__Req;
  req.msg_id            = RPC_ID__Req_SetWifiMode;
  req.payload_case      = RPC__PAYLOAD_REQ_SET_WIFI_MODE;
  req.req_set_wifi_mode = &body;

  ra8_c6link_take_ctx_t take = {.link   = link,
                                .out    = nullptr,
                                .rpc_id = (uint32_t)RPC_ID__Req_SetWifiMode};
  return ra8_c6link_priv_rpc_call(link,
                                  &req,
                                  (uint32_t)RPC_ID__Resp_SetWifiMode,
                                  ra8_c6link_priv_take_resp,
                                  &take);
}

ra8_err_t ra8_c6link_wifi_start(ra8_c6link_t* link)
{
  if (link == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (!ra8_c6link_is_open(link)) {
    return k_ra8_err_not_initialized;
  }

  const ra8_err_t inited = ra8_c6link_wifi_do_init(link);
  if (inited != k_ra8_ok) {
    return inited;
  }
  const ra8_err_t moded = ra8_c6link_wifi_do_mode(link);
  if (moded != k_ra8_ok) {
    return moded;
  }
  return ra8_c6link_priv_bare_req(link, (uint32_t)RPC_ID__Req_WifiStart);
}

ra8_err_t ra8_c6link_wifi_stop(ra8_c6link_t* link)
{
  if (link == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (!ra8_c6link_is_open(link)) {
    return k_ra8_err_not_initialized;
  }

  const ra8_err_t stopped = ra8_c6link_priv_bare_req(link, (uint32_t)RPC_ID__Req_WifiStop);
  const ra8_err_t deinit  = ra8_c6link_priv_bare_req(link, (uint32_t)RPC_ID__Req_WifiDeinit);
  return (stopped != k_ra8_ok) ? stopped : deinit;
}

ra8_err_t ra8_c6link_wifi_leave(ra8_c6link_t* link)
{
  if (link == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (!ra8_c6link_is_open(link)) {
    return k_ra8_err_not_initialized;
  }
  return ra8_c6link_priv_bare_req(link, (uint32_t)RPC_ID__Req_WifiDisconnect);
}
