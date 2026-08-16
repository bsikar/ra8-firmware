/**
 * @file ra8_c6_model.c
 * @brief Implementation of the modelled ESP32-C6 (#490).
 *
 * @details
 * See `ra8_c6_model.h` for what the model is and why it decodes rather than
 * replays. This file is the co-processor's side of every exchange the
 * `ra8_c6link` tests drive.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_c6_model.h"

#include <stdint.h>
#include <string.h>

#include "ra8_attributes.h"
#include "ra8_c6_model_mdl_fault_internal.h"
#include "ra8_c6link.h"
#include "ra8_c6link_internal.h"
#include "ra8_c6link_mdl_msg.h"
#include "ra8_err.h"
#include "ra8_media_download.pb-c.h"
#include "unity_minimal.h"

/** @brief The one modelled co-processor. */
static ra8_c6_model_t s_c6;

/** @brief Deterministic media bytes served through the modelled custom RPC. */
static const uint8_t s_mdl_bytes[] = {'a', 'b', 'c', 'd', 'e', 'f'};

/** @brief State behind the modelled C6 media backend. */
typedef struct {
  const uint8_t*   data;                           /**< Borrowed source bytes.          */
  size_t           len;                            /**< Complete source length.         */
  size_t           at;                             /**< Offset of the next byte.        */
  uint8_t          digest[k_ra8_mdl_sha256_bytes]; /**< Caller-supplied SHA-256.        */
  ra8_mdl_format_t format;                         /**< Most recently requested format. */
} c6m_mdl_backend_t;

static c6m_mdl_backend_t s_mdl_backend;
static ra8_mdl_service_t s_mdl_service;

/**
 * @brief Start one modelled media download at the requested URL.
 * @details Accepts only the deterministic fixture URL and rewinds the bound
 * backend cursor.
 * @param[in,out] ctx Model media-backend context supplied by the service.
 * @param[in] request Complete typed HTTP request from the host.
 * @return Bounded media-backend status.
 * @retval k_ra8_ok The fixture URL was accepted and rewound.
 * @retval k_ra8_err_invalid_arg The URL is not the deterministic fixture URL.
 * @pre @p ctx points to initialized backend state. @pre @p request is non-null
 * and already bounded by the portable service.
 * @post Success sets the next-byte offset to zero and records @p request.
 * @post Failure leaves backend state unchanged.
 * @note The fake intentionally models a single stable origin.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_c6m_mdl_begin(void* ctx, const ra8_mdl_request_t* request)
{
  c6m_mdl_backend_t* backend = (c6m_mdl_backend_t*)ctx;
  if (strcmp(request->url, "https://example.test/book") != 0) {
    return k_ra8_err_invalid_arg;
  }
  backend->at                    = 0U;
  backend->format                = request->format;
  ra8_c6_model()->mdl_format     = request->format;
  ra8_c6_model()->mdl_timeout_ms = request->http.timeout_ms;
  (void)memcpy(ra8_c6_model()->mdl_user_agent,
               request->http.user_agent,
               strlen(request->http.user_agent) + 1U);
  (void)memcpy(ra8_c6_model()->mdl_referer,
               request->http.referer,
               strlen(request->http.referer) + 1U);
  (void)memcpy(ra8_c6_model()->mdl_if_none_match,
               request->http.if_none_match,
               strlen(request->http.if_none_match) + 1U);
  (void)memcpy(ra8_c6_model()->mdl_if_modified_since,
               request->http.if_modified_since,
               strlen(request->http.if_modified_since) + 1U);
  return k_ra8_ok;
}

/**
 * @brief Read the next bounded media slice from the modelled backend.
 * @details Copies at most @p cap bytes, advances the cursor, and publishes the
 * digest at completion.
 * @param[in,out] ctx Model media-backend context supplied by the service.
 * @param[out] out Destination buffer or response envelope populated on success.
 * @param[in] cap Writable byte capacity of @p out.
 * @param[out] got Receives the number of media bytes copied.
 * @param[out] total_bytes Receives the complete fixture-media length.
 * @param[out] complete Receives whether the stream has reached its terminal
 * read.
 * @param[out] sha256 Receives the terminal fixture digest when @p complete is
 * true.
 * @param[out] response Receives terminal HTTP status and selected headers.
 * @return Bounded media-backend status.
 * @retval k_ra8_ok The deterministic backend operation completed.
 * @pre All output pointers are non-null and @p ctx is initialized. @pre @p out
 * spans @p cap bytes.
 * @post The cursor advances by @p got. @post The digest is written only on the
 * terminal read.
 * @note A final data-bearing chunk is followed by one zero-byte terminal read.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_c6m_mdl_read(void*     ctx,
                                                    uint8_t*  out,
                                                    uint16_t  cap,
                                                    uint16_t* got,
                                                    uint64_t* total_bytes,
                                                    bool*     complete,
                                                    uint8_t   sha256[k_ra8_mdl_sha256_bytes],
                                                    ra8_mdl_http_response_t* response)
{
  c6m_mdl_backend_t* backend = (c6m_mdl_backend_t*)ctx;
  *response                  = (ra8_mdl_http_response_t){};
  const size_t left          = backend->len - backend->at;
  const size_t take          = (left < cap) ? left : cap;
  if (take != 0U) {
    (void)memcpy(out, &backend->data[backend->at], take);
    backend->at += take;
  }
  *got         = (uint16_t)take;
  *total_bytes = backend->len;
  *complete    = (take == 0U);
  if (*complete) {
    (void)memcpy(sha256, backend->digest, k_ra8_mdl_sha256_bytes);
    response->status = ra8_c6_model()->mdl_http_status;
    (void)memcpy(response->retry_after, "5", sizeof("5"));
    (void)memcpy(response->etag, "\"c6-model-etag\"", sizeof("\"c6-model-etag\""));
    (void)memcpy(response->last_modified,
                 "Wed, 21 Oct 2015 07:28:00 GMT",
                 sizeof("Wed, 21 Oct 2015 07:28:00 GMT"));
    (void)memcpy(response->content_type,
                 "application/octet-stream",
                 sizeof("application/octet-stream"));
  }
  return k_ra8_ok;
}

/**
 * @brief Record cancellation of the current modelled media job.
 * @details Counts cancellation only when the service supplies its bound backend
 * context.
 * @param[in,out] ctx Model media-backend context supplied by the service.
 * @return Bounded media-backend status.
 * @retval k_ra8_ok The deterministic backend operation completed.
 * @pre The singleton model remains alive. @pre Any non-null @p ctx is the
 * initialized backend.
 * @post Non-null context increments `mdl_cancels`. @post Null context leaves
 * counters unchanged.
 * @note Cancellation does not erase the source binding or digest.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_c6m_mdl_cancel(void* ctx)
{
  c6m_mdl_backend_t* backend = (c6m_mdl_backend_t*)ctx;
  ra8_c6_model()->mdl_cancels += (backend != nullptr) ? 1U : 0U;
  return k_ra8_ok;
}

ra8_c6_model_t* ra8_c6_model(void)
{
  return &s_c6;
}

void ra8_c6_model_reset(void)
{
  s_c6                 = (ra8_c6_model_t){};
  s_c6.handshake       = true;
  s_c6.mdl_http_status = 200;
  s_mdl_backend        = (c6m_mdl_backend_t){.data = s_mdl_bytes, .len = sizeof(s_mdl_bytes)};
  (void)memset(s_mdl_backend.digest, k_c6m_mdl_digest_fill, sizeof(s_mdl_backend.digest));
  const ra8_mdl_service_backend_t backend = {.begin  = internal_c6m_mdl_begin,
                                             .read   = internal_c6m_mdl_read,
                                             .cancel = internal_c6m_mdl_cancel,
                                             .ctx    = &s_mdl_backend};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mdl_service_init(&s_mdl_service, &backend));
}

ra8_err_t ra8_c6_model_mdl_source(const uint8_t* data,
                                  uint32_t       len,
                                  const uint8_t  sha256[k_ra8_mdl_sha256_bytes])
{
  if ((data == nullptr) || (sha256 == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  if (len == 0U) {
    return k_ra8_err_invalid_size;
  }
  c6m_mdl_backend_t next = {.data = data, .len = len};
  (void)memcpy(next.digest, sha256, sizeof(next.digest));
  s_mdl_backend = next;
  return k_ra8_ok;
}

uint8_t* ra8_c6_model_slot(void)
{
  if ((uint32_t)s_c6.tail >= (uint32_t)k_c6m_queue) {
    return nullptr;
  }
  uint8_t* slot = s_c6.queue[s_c6.tail];
  s_c6.tail++;
  (void)memset(slot, 0, (size_t)k_ra8_c6link_frame_bytes);
  return slot;
}

/**
 * @brief Frame an `Rpc` the way the co-processor would and queue it.
 * @param[in] msg Message to send; must be non-null.
 * @return Nothing.
 * @pre The queue has room, or the message is silently dropped.
 * @pre @p msg is fully populated including its payload case.
 * @post One frame is queued, envelope and payload header included.
 * @post The packed length the codec predicted is the length it wrote.
 * @note Uses the same envelope writer the host does; the envelope's own byte
 *       layout is asserted against literals in `test_ra8_c6link_wire.c`, so
 *       the two cannot agree on a wrong answer.
 * @since 0.1.0
 * @details Serializes the generated message, wraps it in the private TLV/frame
 * envelope, and queues one bounded slot.
 */
RA8_INTERNAL static void internal_c6m_emit(Rpc* msg)
{
  uint8_t* slot = ra8_c6_model_slot();
  if (slot == nullptr) {
    return;
  }
  uint8_t*     payload = &slot[k_ra8_c6link_header_bytes];
  const size_t packed  = rpc__get_packed_size(msg);
  uint16_t     body_at = 0U;
  TEST_ASSERT_EQ(
    k_ra8_ok,
    priv_c6link_tlv_open(payload, (uint16_t)k_ra8_c6link_max_payload, (uint16_t)packed, &body_at));
  TEST_ASSERT_EQ(packed, rpc__pack(msg, &payload[body_at]));
  priv_c6link_frame_seal(slot,
                         (uint8_t)ESP_SERIAL_IF,
                         0U,
                         (uint16_t)((uint32_t)body_at + (uint32_t)packed));
}

/**
 * @brief Fill an address with the modelled AP's BSSID.
 * @details Generated rather than written out, so the octets are not six
 *        unexplained literals in two places.
 * @param[out] bssid Address to fill; must hold ::k_ra8_c6link_mac_bytes octets.
 * @return Nothing.
 * @pre @p bssid is non-null and long enough.
 * @pre The caller transmits it inside an announcement.
 * @post Every octet was written.
 * @post The value matches what the tests assert against.
 * @note The loop is bounded by ::k_ra8_c6link_mac_bytes (NASA Rule 2).
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_c6m_fill_bssid(uint8_t* bssid)
{
  for (uint8_t i = 0U; i < (uint8_t)k_ra8_c6link_mac_bytes; i++) {
    bssid[i] = (uint8_t)((uint8_t)k_c6m_bssid_first + i);
  }
}

void ra8_c6_model_emit_boot(void)
{
  RpcEventESPInit body;
  rpc__event__espinit__init(&body);

  Rpc msg;
  rpc__init(&msg);
  msg.msg_type       = RPC_TYPE__Event;
  msg.msg_id         = RPC_ID__Event_ESPInit;
  msg.payload_case   = RPC__PAYLOAD_EVENT_ESP_INIT;
  msg.event_esp_init = &body;
  internal_c6m_emit(&msg);
}

void ra8_c6_model_emit_connected(void)
{
  uint8_t ssid[4]                       = {'b', 'e', 'n', 'c'};
  uint8_t bssid[k_ra8_c6link_mac_bytes] = {};
  internal_c6m_fill_bssid(bssid);

  WifiEventStaConnected inner;
  wifi_event_sta_connected__init(&inner);
  inner.ssid.data  = ssid;
  inner.ssid.len   = sizeof ssid;
  inner.bssid.data = bssid;
  inner.bssid.len  = sizeof bssid;
  inner.channel    = (uint32_t)k_c6m_channel;

  RpcEventStaConnected body;
  rpc__event__sta_connected__init(&body);
  body.sta_connected = &inner;

  Rpc msg;
  rpc__init(&msg);
  msg.msg_type            = RPC_TYPE__Event;
  msg.msg_id              = RPC_ID__Event_StaConnected;
  msg.payload_case        = RPC__PAYLOAD_EVENT_STA_CONNECTED;
  msg.event_sta_connected = &body;
  internal_c6m_emit(&msg);
}

void ra8_c6_model_emit_wifi_event(void)
{
  RpcEventWifiEventNoArgs body;
  rpc__event__wifi_event_no_args__init(&body);
  body.event_id = (int32_t)k_c6m_wifi_ev;

  Rpc msg;
  rpc__init(&msg);
  msg.msg_type                 = RPC_TYPE__Event;
  msg.msg_id                   = RPC_ID__Event_WifiEventNoArgs;
  msg.payload_case             = RPC__PAYLOAD_EVENT_WIFI_EVENT_NO_ARGS;
  msg.event_wifi_event_no_args = &body;
  internal_c6m_emit(&msg);
}

void ra8_c6_model_emit_disconnected(void)
{
  uint8_t ssid[4]                       = {'b', 'e', 'n', 'c'};
  uint8_t bssid[k_ra8_c6link_mac_bytes] = {};
  internal_c6m_fill_bssid(bssid);

  WifiEventStaDisconnected inner;
  wifi_event_sta_disconnected__init(&inner);
  inner.ssid.data  = ssid;
  inner.ssid.len   = sizeof ssid;
  inner.bssid.data = bssid;
  inner.bssid.len  = sizeof bssid;
  inner.reason     = (uint32_t)k_c6m_reason;
  inner.rssi       = -(int32_t)k_c6m_rssi_mag;

  RpcEventStaDisconnected body;
  rpc__event__sta_disconnected__init(&body);
  body.sta_disconnected = &inner;

  Rpc msg;
  rpc__init(&msg);
  msg.msg_type               = RPC_TYPE__Event;
  msg.msg_id                 = RPC_ID__Event_StaDisconnected;
  msg.payload_case           = RPC__PAYLOAD_EVENT_STA_DISCONNECTED;
  msg.event_sta_disconnected = &body;
  internal_c6m_emit(&msg);
}

void ra8_c6_model_emit_eth(void)
{
  uint8_t* slot = ra8_c6_model_slot();
  if (slot == nullptr) {
    return;
  }
  for (uint16_t i = 0U; i < (uint16_t)k_c6m_eth_len; i++) {
    slot[(uint16_t)k_ra8_c6link_header_bytes + i] = (uint8_t)((uint16_t)k_c6m_eth_first + i);
  }
  priv_c6link_frame_seal(slot, (uint8_t)ESP_STA_IF, 0U, (uint16_t)k_c6m_eth_len);
}

void ra8_c6_model_emit_hollow_events(void)
{
  RpcEventStaConnected connected;
  rpc__event__sta_connected__init(&connected);

  Rpc first;
  rpc__init(&first);
  first.msg_type            = RPC_TYPE__Event;
  first.msg_id              = RPC_ID__Event_StaConnected;
  first.payload_case        = RPC__PAYLOAD_EVENT_STA_CONNECTED;
  first.event_sta_connected = &connected;
  internal_c6m_emit(&first);

  RpcEventStaDisconnected disconnected;
  rpc__event__sta_disconnected__init(&disconnected);

  Rpc second;
  rpc__init(&second);
  second.msg_type               = RPC_TYPE__Event;
  second.msg_id                 = RPC_ID__Event_StaDisconnected;
  second.payload_case           = RPC__PAYLOAD_EVENT_STA_DISCONNECTED;
  second.event_sta_disconnected = &disconnected;
  internal_c6m_emit(&second);
}

void ra8_c6_model_emit_unmodelled_event(void)
{
  Rpc msg;
  rpc__init(&msg);
  msg.msg_type = RPC_TYPE__Event;
  msg.msg_id   = RPC_ID__Event_Heartbeat;
  internal_c6m_emit(&msg);
}

void ra8_c6_model_emit_inbound_request(void)
{
  RpcReqGetCoprocessorFwVersion body;
  rpc__req__get_coprocessor_fw_version__init(&body);

  Rpc msg;
  rpc__init(&msg);
  msg.msg_type                      = RPC_TYPE__Req;
  msg.msg_id                        = RPC_ID__Req_GetCoprocessorFwVersion;
  msg.uid                           = 1U;
  msg.payload_case                  = RPC__PAYLOAD_REQ_GET_COPROCESSOR_FWVERSION;
  msg.req_get_coprocessor_fwversion = &body;
  internal_c6m_emit(&msg);
}

void ra8_c6_model_emit_stray(void)
{
  RpcRespGetCoprocessorFwVersion body;
  rpc__resp__get_coprocessor_fw_version__init(&body);

  Rpc msg;
  rpc__init(&msg);
  msg.msg_type                       = RPC_TYPE__Resp;
  msg.msg_id                         = RPC_ID__Resp_GetCoprocessorFwVersion;
  msg.uid                            = 1U;
  msg.payload_case                   = RPC__PAYLOAD_RESP_GET_COPROCESSOR_FWVERSION;
  msg.resp_get_coprocessor_fwversion = &body;
  internal_c6m_emit(&msg);
}

/**
 * @brief Fill in the answer for a request whose reply is a bare result code.
 * @param[in,out] out Message being built; must be non-null.
 * @param[in] req_id The request being answered.
 * @param[out] body Storage for the answer body; must be non-null.
 * @param[in] resp The result code to report.
 * @return true when @p req_id is one the model answers this way.
 * @retval true @p out carries the right id, payload case and body.
 * @retval false @p req_id needs a richer answer, or none.
 * @pre @p body outlives @p out, which the caller's stack frame guarantees.
 * @pre @p out has been initialised by `rpc__init()`.
 * @post On true @p out is ready to pack.
 * @post On false @p out is unmodified.
 * @note Every one of these bodies is a bare result code, so one storage object
 *       serves them all -- which is exactly why the facade shares one
 *       extractor for them.
 * @since 0.1.0
 * @details Initializes the shared result body and maps each supported request
 * id to its generated response variant.
 */
RA8_INTERNAL static bool
internal_c6m_bare(Rpc* out, uint32_t req_id, RpcRespWifiStart* body, int32_t resp)
{
  rpc__resp__wifi_start__init(body);
  body->resp = resp;

  switch ((int32_t)req_id) {
    case (int32_t)RPC_ID__Req_WifiInit:
      out->msg_id       = RPC_ID__Resp_WifiInit;
      out->payload_case = RPC__PAYLOAD_RESP_WIFI_INIT;
      break;
    case (int32_t)RPC_ID__Req_SetWifiMode:
      out->msg_id       = RPC_ID__Resp_SetWifiMode;
      out->payload_case = RPC__PAYLOAD_RESP_SET_WIFI_MODE;
      break;
    case (int32_t)RPC_ID__Req_WifiSetConfig:
      out->msg_id       = RPC_ID__Resp_WifiSetConfig;
      out->payload_case = RPC__PAYLOAD_RESP_WIFI_SET_CONFIG;
      break;
    case (int32_t)RPC_ID__Req_WifiStart:
      out->msg_id       = RPC_ID__Resp_WifiStart;
      out->payload_case = RPC__PAYLOAD_RESP_WIFI_START;
      break;
    case (int32_t)RPC_ID__Req_WifiStop:
      out->msg_id       = RPC_ID__Resp_WifiStop;
      out->payload_case = RPC__PAYLOAD_RESP_WIFI_STOP;
      break;
    case (int32_t)RPC_ID__Req_WifiDeinit:
      out->msg_id       = RPC_ID__Resp_WifiDeinit;
      out->payload_case = RPC__PAYLOAD_RESP_WIFI_DEINIT;
      break;
    case (int32_t)RPC_ID__Req_WifiConnect:
      out->msg_id       = RPC_ID__Resp_WifiConnect;
      out->payload_case = RPC__PAYLOAD_RESP_WIFI_CONNECT;
      break;
    case (int32_t)RPC_ID__Req_WifiDisconnect:
      out->msg_id       = RPC_ID__Resp_WifiDisconnect;
      out->payload_case = RPC__PAYLOAD_RESP_WIFI_DISCONNECT;
      break;
    default:
      return false;
  }
  out->resp_wifi_start = body;
  return true;
}

/**
 * @brief Record the credentials a `Req_WifiSetConfig` carried.
 * @param[in] body The decoded request body; must be non-null.
 * @return Nothing.
 * @pre @p body is still owned by the decoder.
 * @pre The model has been reset since the last configuration.
 * @post The recorded SSID and passphrase are NUL-terminated.
 * @post The nested threshold and PMF messages were asserted present.
 * @note Upstream's own host always sends those nested messages, so a facade
 *       that omitted them would rely on a co-processor null check nobody has
 *       exercised.
 * @since 0.1.0
 * @details Copies bounded station credentials and verifies the nested threshold
 * and PMF messages the real peer requires.
 */
RA8_INTERNAL static void internal_c6m_take_config(const RpcReqWifiSetConfig* body)
{
  s_c6.ssid_len = 0U;
  s_c6.pass_len = 0U;
  if ((body->cfg == nullptr) || (body->cfg->sta == nullptr)) {
    return;
  }
  const WifiStaConfig* sta = body->cfg->sta;
  s_c6.ssid_len            = priv_c6link_copy_str(s_c6.ssid, (uint8_t)sizeof s_c6.ssid, &sta->ssid);
  s_c6.pass_len = priv_c6link_copy_str(s_c6.pass, (uint8_t)sizeof s_c6.pass, &sta->password);
  TEST_ASSERT_NOT_NULL(sta->threshold);
  TEST_ASSERT_NOT_NULL(sta->pmf_cfg);
}

/**
 * @struct c6m_rich
 * @brief Storage for the three answers that carry more than a result code.
 *
 * @details
 * File scope rather than stack, so the builder that fills it can be a function
 * of its own: every member is pointed at by the `Rpc` the caller then packs,
 * and a stack-local set would die at that builder's return.
 *
 * @invariant Exactly one arm is populated per answer.
 * @invariant The model is a singleton, so one instance is enough.
 *
 * @par Example:
 * @code
 * static c6m_rich_t s_rich;
 * @endcode
 *
 * @see internal_c6m_rich_answer
 * @since 0.1.0
 */
typedef struct c6m_rich {
  RpcRespGetCoprocessorFwVersion fw;      /**< `Resp_GetCoprocessorFwVersion` body. */
  RpcRespGetMacAddress           mac;     /**< `Resp_GetMACAddress` body.           */
  RpcRespWifiStaGetApInfo        ap;      /**< `Resp_WifiStaGetApInfo` body.        */
  WifiApRecord                   rec;     /**< The AP record inside that answer.    */
  uint8_t octets[k_ra8_c6link_mac_bytes]; /**< The modelled station address.        */
  uint8_t ssid[4];                        /**< The modelled AP's SSID.              */
  char    target[8];                      /**< The modelled `CONFIG_IDF_TARGET`.    */
} c6m_rich_t;

/** @brief Backing storage for whichever rich answer is being built. */
static c6m_rich_t s_rich;

/**
 * @brief Fill in the answer describing the AP the station is associated with.
 * @details The richest answer the model builds: a nested record inside the
 *        response, which is what makes the decoder's nested-message path worth
 *        exercising at all.
 * @param[in,out] out Message being built; must be non-null.
 * @param[in] resp The result code to report.
 * @return Nothing.
 * @pre @p out has been initialised by `rpc__init()`.
 * @pre ::s_rich already holds the generated address and SSID octets.
 * @post @p out points at ::s_rich for both the answer and its nested record.
 * @post No other model state is modified.
 * @note Split out of ::internal_c6m_rich_answer so that function stays inside
 * the complexity thresholds clang-tidy holds the whole tree to.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_c6m_ap_answer(Rpc* out, int32_t resp)
{
  wifi_ap_record__init(&s_rich.rec);
  s_rich.rec.ssid.data  = s_rich.ssid;
  s_rich.rec.ssid.len   = sizeof s_rich.ssid;
  s_rich.rec.bssid.data = s_rich.octets;
  s_rich.rec.bssid.len  = sizeof s_rich.octets;
  s_rich.rec.primary    = (uint32_t)k_c6m_channel;
  s_rich.rec.rssi       = -(int32_t)k_c6m_rssi_mag;
  rpc__resp__wifi_sta_get_ap_info__init(&s_rich.ap);
  s_rich.ap.resp                 = resp;
  s_rich.ap.ap_record            = &s_rich.rec;
  out->msg_id                    = RPC_ID__Resp_WifiStaGetApInfo;
  out->payload_case              = RPC__PAYLOAD_RESP_WIFI_STA_GET_AP_INFO;
  out->resp_wifi_sta_get_ap_info = &s_rich.ap;
}

/**
 * @brief Fill in an answer that carries fields beyond its result code.
 * @param[in,out] out Message being built; must be non-null.
 * @param[in] req_id The request being answered.
 * @param[in] resp The result code to report.
 * @return true when @p req_id is one this builder answers.
 * @retval true @p out carries the right id, payload case and body.
 * @retval false @p req_id is answered by a bare result code, or not at all.
 * @pre @p out has been initialised by `rpc__init()`.
 * @pre No other answer is mid-construction, which a single-threaded test
 *      binary guarantees.
 * @post On true ::s_rich holds every object @p out points at.
 * @post On false @p out is unmodified.
 * @note Split out of ::internal_c6m_answer so that function stays inside the
 * sixty-line cap the whole tree is held to, tests included.
 * @since 0.1.0
 * @details Builds firmware, MAC, or AP-info payloads in model-owned storage
 * that outlives synchronous packing.
 */
RA8_INTERNAL static bool internal_c6m_rich_answer(Rpc* out, uint32_t req_id, int32_t resp)
{
  for (uint8_t i = 0U; i < (uint8_t)k_ra8_c6link_mac_bytes; i++) {
    s_rich.octets[i] = (uint8_t)((uint8_t)k_c6m_mac_first - i);
  }
  (void)memcpy(s_rich.ssid, "benc", sizeof s_rich.ssid);
  (void)memcpy(s_rich.target, "esp32c6", sizeof "esp32c6");

  if (req_id == (uint32_t)RPC_ID__Req_GetCoprocessorFwVersion) {
    rpc__resp__get_coprocessor_fw_version__init(&s_rich.fw);
    s_rich.fw.resp                      = resp;
    s_rich.fw.major1                    = (uint32_t)k_c6m_fw_major;
    s_rich.fw.minor1                    = (uint32_t)k_c6m_fw_minor;
    s_rich.fw.patch1                    = (uint32_t)k_c6m_fw_patch;
    s_rich.fw.chip_id                   = (uint32_t)k_c6m_chip_id;
    s_rich.fw.idf_target.data           = (uint8_t*)s_rich.target;
    s_rich.fw.idf_target.len            = sizeof "esp32c6" - 1U;
    out->msg_id                         = RPC_ID__Resp_GetCoprocessorFwVersion;
    out->payload_case                   = RPC__PAYLOAD_RESP_GET_COPROCESSOR_FWVERSION;
    out->resp_get_coprocessor_fwversion = &s_rich.fw;
    return true;
  }
  if (req_id == (uint32_t)RPC_ID__Req_GetMACAddress) {
    rpc__resp__get_mac_address__init(&s_rich.mac);
    s_rich.mac.resp           = resp;
    s_rich.mac.mac.data       = s_rich.octets;
    s_rich.mac.mac.len        = sizeof s_rich.octets;
    out->msg_id               = RPC_ID__Resp_GetMACAddress;
    out->payload_case         = RPC__PAYLOAD_RESP_GET_MAC_ADDRESS;
    out->resp_get_mac_address = &s_rich.mac;
    return true;
  }
  if (req_id != (uint32_t)RPC_ID__Req_WifiStaGetApInfo) {
    return false;
  }
  internal_c6m_ap_answer(out, resp);
  return true;
}

/** @brief Run an inner media message through the portable C6 service.
 * @details Dispatches one inner media request through the portable bounded
 * service and returns its packed generated response.
 * @param[out] out Destination buffer or response envelope populated on success.
 * @param[in] req Decoded outer request whose custom payload is dispatched.
 * @param[in] scripted_resp Forced outer result code, or zero to dispatch
 * normally.
 * @return Whether this request was recognized and answered.
 * @retval true A custom RPC response was prepared.
 * @retval false The request is not a custom RPC request.
 * @pre @p out and @p req are initialized. @pre The model media service has been
 * reset.
 * @post True leaves response storage valid through packing. @post False leaves
 * @p out unchanged.
 * @note Static response storage is safe because model exchanges are serialized.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_c6m_custom_answer(Rpc* out, const Rpc* req, int32_t scripted_resp)
{
  if ((uint32_t)req->msg_id != (uint32_t)RPC_ID__Req_CustomRpc) {
    return false;
  }
  static uint8_t          response_bytes[k_c6m_custom_response_bytes];
  static RpcRespCustomRpc response_body;
  rpc__resp__custom_rpc__init(&response_body);
  out->msg_id          = RPC_ID__Resp_CustomRpc;
  out->payload_case    = RPC__PAYLOAD_RESP_CUSTOM_RPC;
  out->resp_custom_rpc = &response_body;
  if (req->req_custom_rpc == nullptr) {
    response_body.resp = (int32_t)k_ra8_err_protocol_error;
    return true;
  }
  response_body.custom_msg_id = req->req_custom_rpc->custom_msg_id;
  if (scripted_resp != 0) {
    response_body.resp = scripted_resp;
    return true;
  }
  size_t response_len = 0U;
  response_body.resp  = (int32_t)ra8_mdl_service_dispatch(&s_mdl_service,
                                                          response_body.custom_msg_id,
                                                          req->req_custom_rpc->data.data,
                                                          req->req_custom_rpc->data.len,
                                                          response_bytes,
                                                          sizeof(response_bytes),
                                                          &response_len);
  response_body.data  = (ProtobufCBinaryData){.len = response_len, .data = response_bytes};
  if (response_body.resp == (int32_t)k_ra8_ok) {
    priv_c6_model_mdl_fault_apply(&s_c6.mdl_fault,
                                  out,
                                  &response_body,
                                  response_bytes,
                                  sizeof(response_bytes),
                                  &response_len);
  }
  return true;
}

/**
 * @brief Build and queue the answer to one decoded request.
 * @param[in] req The decoded request; must be non-null.
 * @return Nothing.
 * @pre @p req is still owned by the decoder.
 * @pre The queue has room, or the answer is dropped.
 * @post The request id was recorded in arrival order.
 * @post Exactly one answer is queued unless the model was told to stay mute.
 * @note The scripted UID and message-id corruptions prove the host correlates
 * rather than merely receives.
 * @since 0.1.0
 * @details Records correlation state, applies scripted faults, selects the
 * response builder, and queues at most one reply.
 */
RA8_INTERNAL static void internal_c6m_answer(const Rpc* req)
{
  const uint32_t req_id = (uint32_t)req->msg_id;
  if (s_c6.seen_n < (uint8_t)k_c6m_seen) {
    s_c6.seen[s_c6.seen_n] = req_id;
    s_c6.seen_n++;
  }
  /* Before the host has announced itself on ESP_PRIV_IF the co-processor
     services nothing. Modelling that is what turns a bench finding into a
     host-test guarantee: a facade that forgets the announcement now fails
     here instead of only on silicon. */
  if (s_c6.mute || !s_c6.caps_seen) {
    return;
  }
  const int32_t resp = (s_c6.fail_req == req_id) ? (int32_t)k_c6m_esp_fail : 0;

  Rpc out;
  rpc__init(&out);
  out.msg_type = RPC_TYPE__Resp;
  out.uid      = s_c6.wrong_uid ? (req->uid + 1U) : req->uid;

  if (req_id == (uint32_t)RPC_ID__Req_WifiSetConfig) {
    internal_c6m_take_config(req->req_wifi_set_config);
  }

  RpcRespWifiStart bare;
  if (!internal_c6m_custom_answer(&out, req, resp) &&
      !internal_c6m_rich_answer(&out, req_id, resp) &&
      !internal_c6m_bare(&out, req_id, &bare, resp)) {
    return;
  }

  if (s_c6.wrong_id) {
    out.msg_id = RPC_ID__Resp_WifiStop;
  }
  internal_c6m_emit(&out);
}

/**
 * @brief Check the host's framing independently of the code that wrote it.
 * @param[in] tx The host's transmit transaction; must be non-null.
 * @param[in] hdr Its header, already copied out; must be non-null.
 * @return Nothing.
 * @pre @p hdr was copied from the first octets of @p tx.
 * @pre The frame carries a payload, so its checksum is defined.
 * @post The transmitted checksum was proven to cover header plus payload.
 * @post No model state is modified.
 * @note The checksum is recomputed over a field-zeroed copy, as upstream's
 * `process_spi_rx_buf()` does, rather than using the host's subtraction
 * shortcut.
 * @since 0.1.0
 * @details Checks the envelope bytes and declared payload size before generated
 * RPC decode.
 */
RA8_INTERNAL static void internal_c6m_verify_framing(const uint8_t*                   tx,
                                                     const struct esp_payload_header* hdr)
{
  uint8_t copy[k_ra8_c6link_frame_bytes];
  (void)memcpy(copy, tx, sizeof copy);
  struct esp_payload_header zeroed = *hdr;
  zeroed.checksum                  = 0U;
  (void)memcpy(copy, &zeroed, sizeof zeroed);
  TEST_ASSERT_EQ(hdr->checksum, compute_checksum(copy, (uint16_t)(hdr->offset + hdr->len)));
  TEST_ASSERT_EQ(k_ra8_c6link_header_bytes, hdr->offset);
}

/**
 * @brief Decode whatever the host just transmitted and react to it.
 * @param[in] tx The host's transmit transaction; must be non-null.
 * @return Nothing.
 * @pre The transaction is ::k_ra8_c6link_frame_bytes long.
 * @pre The model has been reset at least once.
 * @post A control request was answered, data was recorded, or idle filler
 * caused no change.
 * @post The host's framing was verified for every non-idle frame.
 * @note Decode failure proves the host encoded a request incorrectly.
 * @since 0.1.0
 * @details Classifies private-interface announcements separately and records
 * capabilities.
 */
RA8_INTERNAL static void internal_c6m_observe(const uint8_t* tx)
{
  struct esp_payload_header hdr = {};
  (void)memcpy(&hdr, tx, sizeof hdr);
  if (hdr.len == 0U) {
    return;
  }
  internal_c6m_verify_framing(tx, &hdr);

  if (hdr.if_type == (uint8_t)ESP_STA_IF) {
    s_c6.eth_tx_len     = hdr.len;
    const uint16_t take = (hdr.len < (uint16_t)k_c6m_eth_len) ? hdr.len : (uint16_t)k_c6m_eth_len;
    (void)memcpy(s_c6.eth_tx, &tx[hdr.offset], (size_t)take);
    return;
  }
  if (hdr.if_type == (uint8_t)ESP_PRIV_IF) {
    /* The host announcing itself. The silicon answers this with its boot
       event, and until it arrives the co-processor services no RPC -- so the
       model refuses requests before it, exactly as the hardware does. */
    s_c6.caps_seen = true;
    const uint16_t take =
      (hdr.len < (uint16_t)k_c6m_caps_bytes) ? hdr.len : (uint16_t)k_c6m_caps_bytes;
    (void)memcpy(s_c6.caps, &tx[hdr.offset], (size_t)take);
    s_c6.caps_len = (uint8_t)take;
    if (!s_c6.silent_boot) {
      ra8_c6_model_emit_boot();
    }
    return;
  }
  if (hdr.if_type != (uint8_t)ESP_SERIAL_IF) {
    return;
  }

  uint16_t       proto_len = 0U;
  const uint8_t* proto     = priv_c6link_tlv_body(&tx[hdr.offset], hdr.len, &proto_len);
  TEST_ASSERT_NOT_NULL(proto);
  Rpc* req = rpc__unpack(nullptr, (size_t)proto_len, proto);
  TEST_ASSERT_NOT_NULL(req);
  TEST_ASSERT_EQ(RPC_TYPE__Req, req->msg_type);
  internal_c6m_answer(req);
  rpc__free_unpacked(req, nullptr);
}

/**
 * @brief The seam's transfer row, backed by the model.
 * @param[in] ctx Unused; the model is a singleton.
 * @param[in] tx The host's transmit transaction.
 * @param[out] rx Where the model's transmit transaction lands.
 * @param[in] len Transaction length; always the frame size.
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok The transaction completed.
 * @retval k_ra8_err_spi_error A bus fault was scripted.
 * @pre The model has been reset.
 * @pre Both buffers are ::k_ra8_c6link_frame_bytes long.
 * @post The receive buffer holds a queued frame or idle filler.
 * @post The transmit buffer was decoded and reacted to.
 * @note Serves the receive side BEFORE decoding the transmit side, because
 *       the co-processor latches its transmit buffer before it sees the
 *       host's -- an answer can never ride the same transaction as its
 *       question.
 * @since 0.1.0
 * @details Models one full-duplex frame exchange by observing host output and
 * copying the next queued response into receive storage.
 */
RA8_INTERNAL static ra8_err_t
internal_c6m_transfer(void* ctx, const uint8_t* tx, uint8_t* rx, uint16_t len)
{
  (void)ctx;
  TEST_ASSERT_EQ(k_ra8_c6link_frame_bytes, len);
  if (s_c6.fail_transfer) {
    return k_ra8_err_spi_error;
  }
  s_c6.transfers++;

  if (s_c6.head < s_c6.tail) {
    (void)memcpy(rx, s_c6.queue[s_c6.head], (size_t)len);
    s_c6.head++;
  } else {
    priv_c6link_frame_filler(rx);
  }
  internal_c6m_observe(tx);
  return k_ra8_ok;
}

/**
 * @brief The seam's handshake row, backed by the model.
 * @param[in] ctx Unused.
 * @return Whatever the model has been told HANDSHAKE reads.
 * @retval true The modelled co-processor is armed.
 * @retval false It is not, so no transaction may start.
 * @pre The model has been reset.
 * @pre The caller polls rather than blocks on this.
 * @post No model state is modified.
 * @post The value reflects the scripted flag exactly.
 * @note Not thread-safe.
 * @since 0.1.0
 * @details Returns the modelled data-ready state so tests can drive both
 * successful and unavailable-link branches.
 */
RA8_INTERNAL static bool internal_c6m_handshake(void* ctx)
{
  (void)ctx;
  return s_c6.handshake;
}

/**
 * @brief The seam's delay row; the model's clock is instantaneous but counted.
 * @details It records the request and returns, so a test can assert that a
 *        caller paced itself without any test paying the wall time for it.
 * @param[in] ctx Unused.
 * @param[in] ms Milliseconds the caller asked to wait for; recorded, not slept.
 * @return Nothing.
 * @pre The caller tolerates a delay that does not actually delay.
 * @pre The model has been reset.
 * @post ::ra8_c6_model_t::delays counted this call.
 * @post The call returns immediately, so a bounded wait costs no wall time.
 * @note A real backend sleeps here; a host test must not.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_c6m_delay(void* ctx, uint16_t ms)
{
  (void)ctx;
  s_c6.delays        = s_c6.delays + 1U;
  s_c6.last_delay_ms = ms;
}

void ra8_c6_model_bind(ra8_c6link_transport_t* out)
{
  if (out == nullptr) {
    return;
  }
  out->transfer         = internal_c6m_transfer;
  out->handshake_active = internal_c6m_handshake;
  out->delay_ms         = internal_c6m_delay;
  out->ctx              = &s_c6;
}
