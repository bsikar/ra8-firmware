/**
 * @file ra8_c6link_wifi_sta.c
 * @brief Station credentials, association, and what the radio reports back.
 * @ingroup grp_net
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details
 * The half of the station API that carries data rather than lifecycle: the
 * credentials go up in `Req_WifiSetConfig`, and the station's own address and
 * its view of the AP come back in `Resp_GetMACAddress` and
 * `Resp_WifiStaGetApInfo`.
 *
 * @par Why the optional sub-messages are always sent
 * `WifiStaConfig` carries a scan threshold and a protected-management-frame
 * configuration as nested messages, and protobuf allows both to be absent.
 * Upstream's own host allocates them unconditionally, so a co-processor that
 * dereferences either without a null check has never been exercised with them
 * missing. Sending them costs a handful of bytes and removes a class of failure
 * this host cannot debug from its side of the wire.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_c6link.h"
#include "ra8_c6link_internal.h"
#include "ra8_c6link_wifi.h"
#include "ra8_secure.h"

/**
 * @enum ra8_c6link_sta_wire_t
 * @brief Co-processor-side numbering the station requests transmit.
 *
 * @details
 * These cross the link as plain integers and belong to ESP-IDF's enumerations
 * on the far side, so they are named here rather than written as literals.
 *
 * @invariant ::k_ra8_c6link_iface_sta is `WIFI_IF_STA` (0): the interface index
 *            both `Req_WifiSetConfig` and `Req_GetMACAddress` select. The
 *            `Req_GetMACAddress.mode` field is named for `wifi_mode_t` but the
 *            co-processor passes it straight to
 *            `esp_wifi_get_mac(wifi_interface_t, ...)`, so it is really an
 *            interface index: `WIFI_IF_STA` (0) returns the address the radio
 *            associates with, while `WIFI_IF_AP` (1) returns the SoftAP address
 *            (the station address plus one), which never associates -- stamping
 *            frames with it is why an associated station could not finish DHCP.
 *
 * @par Example:
 * @code
 * body.iface = (int32_t)k_ra8_c6link_iface_sta;
 * @endcode
 *
 * @see ra8_c6link_wifi_join
 * @since 0.1.0
 */
typedef enum : int32_t {
  k_ra8_c6link_iface_sta   = 0, /**< `WIFI_IF_STA`: STA interface index (also
                                      the `Req_GetMACAddress.mode` selector). */
  k_ra8_c6link_scan_fast   = 0, /**< `WIFI_FAST_SCAN`: stop at the first
                                      acceptable AP, which is what a fixed
                                      bench network wants.                  */
  k_ra8_c6link_sort_signal = 0, /**< `WIFI_CONNECT_AP_BY_SIGNAL`. */
  k_ra8_c6link_auth_open   = 0, /**< `WIFI_AUTH_OPEN` as a threshold means
                                      "impose no minimum".                  */
} ra8_c6link_sta_wire_t;

/**
 * @brief Measure a NUL-terminated string without trusting it to be terminated.
 * @details Bounded by the destination rather than by the string, so a caller
 *        that hands over an unterminated buffer gets a refusal instead of a
 *        read past its end.
 * @param[in] text String to measure; must be non-null.
 * @param[in] cap Octets that may be examined, including the terminator.
 * @return The length in octets, or @p cap when no terminator was found.
 * @retval 0 The string is empty.
 * @pre @p cap octets are readable at @p text.
 * @pre The caller treats a result equal to @p cap as "too long".
 * @post No buffer is modified.
 * @post The result is at most @p cap.
 * @note The loop is bounded by @p cap (NASA Rule 2), which is why `strnlen` is
 *       not used: its bound is the same but its availability is not.
 * @since 0.1.0
 */
RA8_INTERNAL static uint8_t internal_c6link_sta_len(const char* text, uint8_t cap)
{
  uint8_t n = 0U;
  while ((n < cap) && (text[n] != '\0')) {
    n++;
  }
  return n;
}

ra8_err_t ra8_c6link_sta_cfg_set(ra8_c6link_sta_cfg_t* cfg, const char* ssid, const char* pass)
{
  if ((cfg == nullptr) || (ssid == nullptr)) {
    if (cfg != nullptr) {
      ra8_secure_memzero(cfg, sizeof(*cfg));
    }
    return k_ra8_err_null_ptr;
  }
  ra8_secure_memzero(cfg, sizeof(*cfg));

  const uint8_t ssid_len = internal_c6link_sta_len(ssid, (uint8_t)sizeof cfg->ssid);
  if ((ssid_len == 0U) || (ssid_len > (uint8_t)k_ra8_c6link_ssid_max)) {
    return k_ra8_err_invalid_size;
  }
  const uint8_t pass_len =
    (pass == nullptr) ? 0U : internal_c6link_sta_len(pass, (uint8_t)sizeof cfg->pass);
  if (pass_len > (uint8_t)k_ra8_c6link_pass_max) {
    return k_ra8_err_invalid_size;
  }

  for (uint8_t i = 0U; i < ssid_len; i++) {
    cfg->ssid[i] = ssid[i];
  }
  for (uint8_t i = 0U; i < pass_len; i++) {
    cfg->pass[i] = pass[i];
  }
  cfg->ssid_len = ssid_len;
  cfg->pass_len = pass_len;
  return k_ra8_ok;
}

/**
 * @struct ra8_c6link_sta_wire_buf
 * @brief Writable copies of the credentials, for the codec's binary fields.
 *
 * @details
 * `ProtobufCBinaryData::data` is a non-const pointer because packing and
 * unpacking share one type. Copying the caller's const record into this
 * short-lived stack object is what keeps the public API's `const` honest
 * instead of casting it away.
 *
 * @invariant Every field is sized to the protocol maximum, so a copy bounded
 *            by the caller's declared lengths always fits.
 * @invariant The object outlives the `Rpc` that points into it, which the
 *            enclosing stack frame guarantees.
 *
 * @par Example:
 * @code
 * ra8_c6link_sta_wire_buf_t buf = {};
 * internal_c6link_sta_stage(&buf, cfg);
 * @endcode
 *
 * @see internal_c6link_sta_set_config
 * @since 0.1.0
 */
typedef struct ra8_c6link_sta_wire_buf {
  uint8_t ssid[k_ra8_c6link_ssid_max];   /**< SSID octets, as transmitted. */
  uint8_t pass[k_ra8_c6link_pass_max];   /**< Passphrase octets.           */
  uint8_t bssid[k_ra8_c6link_mac_bytes]; /**< Pinned AP address, if any.   */
} ra8_c6link_sta_wire_buf_t;

/**
 * @brief Copy the caller's credentials into writable transmit storage.
 * @details The codec's binary fields are non-const because packing and
 *        unpacking share one type, so the credentials are copied rather than
 *        const-cast out of the caller's record.
 * @param[out] buf Staging storage; must be non-null and zero-initialised.
 * @param[in] cfg Station configuration; must be non-null and consistent.
 * @return Nothing.
 * @pre @p cfg's lengths are within the protocol maxima, which
 *      ::ra8_c6link_wifi_join has already checked.
 * @pre @p buf has been zero-initialised, so unused octets are zero.
 * @post Every declared octet of the credentials was copied.
 * @post @p cfg is not modified.
 * @note The loops are bounded by the caller's declared lengths, which are in
 *       turn bounded by the field sizes (NASA Rule 2).
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_c6link_sta_stage(ra8_c6link_sta_wire_buf_t*  buf,
                                                   const ra8_c6link_sta_cfg_t* cfg)
{
  for (uint8_t i = 0U; i < cfg->ssid_len; i++) {
    buf->ssid[i] = (uint8_t)cfg->ssid[i];
  }
  for (uint8_t i = 0U; i < cfg->pass_len; i++) {
    buf->pass[i] = (uint8_t)cfg->pass[i];
  }
  for (uint8_t i = 0U; i < (uint8_t)k_ra8_c6link_mac_bytes; i++) {
    buf->bssid[i] = cfg->bssid.octet[i];
  }
}

/**
 * @brief Send `Req_WifiSetConfig` carrying the station credentials.
 * @details Sends the credentials and the search hints together: a known
 *        channel skips a full scan and a known BSSID pins the association to
 *        one radio.
 * @param[in,out] link Open handle; must be non-null.
 * @param[in] cfg Station configuration; must be non-null and consistent.
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok The co-processor stored the configuration.
 * @retval k_ra8_err_timeout It did not answer within the budget.
 * @retval k_ra8_err_protocol_error It refused the configuration.
 * @retval k_ra8_err_spi_error The transport refused a transfer.
 * @pre ::ra8_c6link_wifi_start has succeeded.
 * @pre @p cfg's lengths match its strings.
 * @post On success the credentials are held by the co-processor.
 * @post On failure the fault slot names this request.
 * @note The strings are transmitted as counted binary fields, so an SSID
 *       containing a zero octet -- which 802.11 permits -- survives.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_c6link_sta_set_config(ra8_c6link_t*               link,
                                                             const ra8_c6link_sta_cfg_t* cfg)
{
  WifiScanThreshold threshold;
  wifi_scan_threshold__init(&threshold);
  threshold.authmode = (int32_t)k_ra8_c6link_auth_open;

  WifiPmfConfig pmf;
  wifi_pmf_config__init(&pmf);
  pmf.capable = true;

  ra8_c6link_sta_wire_buf_t buf = {};
  internal_c6link_sta_stage(&buf, cfg);

  WifiStaConfig sta;
  wifi_sta_config__init(&sta);
  sta.ssid.data     = buf.ssid;
  sta.ssid.len      = (size_t)cfg->ssid_len;
  sta.password.data = buf.pass;
  sta.password.len  = (size_t)cfg->pass_len;
  sta.scan_method   = (int32_t)k_ra8_c6link_scan_fast;
  sta.sort_method   = (int32_t)k_ra8_c6link_sort_signal;
  sta.channel       = (uint32_t)cfg->channel;
  sta.bssid_set     = cfg->bssid_set;
  sta.bssid.data    = buf.bssid;
  sta.bssid.len     = cfg->bssid_set ? (size_t)k_ra8_c6link_mac_bytes : 0U;
  sta.threshold     = &threshold;
  sta.pmf_cfg       = &pmf;

  WifiConfig wcfg;
  wifi_config__init(&wcfg);
  wcfg.u_case = WIFI_CONFIG__U_STA;
  wcfg.sta    = &sta;

  RpcReqWifiSetConfig body;
  rpc__req__wifi_set_config__init(&body);
  body.iface = (int32_t)k_ra8_c6link_iface_sta;
  body.cfg   = &wcfg;

  Rpc req;
  rpc__init(&req);
  req.msg_type            = RPC_TYPE__Req;
  req.msg_id              = RPC_ID__Req_WifiSetConfig;
  req.payload_case        = RPC__PAYLOAD_REQ_WIFI_SET_CONFIG;
  req.req_wifi_set_config = &body;

  ra8_c6link_take_ctx_t take   = {.link   = link,
                                  .out    = nullptr,
                                  .rpc_id = (uint32_t)RPC_ID__Req_WifiSetConfig};
  const ra8_err_t       result = priv_c6link_rpc_call(link,
                                                      &req,
                                                      (uint32_t)RPC_ID__Resp_WifiSetConfig,
                                                      priv_c6link_take_resp,
                                                      &take);
  ra8_secure_memzero(&buf, sizeof(buf));
  return result;
}

ra8_err_t ra8_c6link_wifi_join(ra8_c6link_t* link, const ra8_c6link_sta_cfg_t* cfg)
{
  if ((link == nullptr) || (cfg == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  if (!ra8_c6link_is_open(link)) {
    return k_ra8_err_not_initialized;
  }
  if ((cfg->ssid_len == 0U) || (cfg->ssid_len > (uint8_t)k_ra8_c6link_ssid_max) ||
      (cfg->pass_len > (uint8_t)k_ra8_c6link_pass_max)) {
    return k_ra8_err_invalid_size;
  }

  const ra8_err_t configured = internal_c6link_sta_set_config(link, cfg);
  if (configured != k_ra8_ok) {
    return configured;
  }
  return priv_c6link_bare_req(link, (uint32_t)RPC_ID__Req_WifiConnect);
}

/**
 * @brief Extract the station address from its answer.
 * @details Checks the co-processor's result code before the address, so a
 *        refusal is reported as a refusal rather than as a malformed address.
 * @param[in] ctx A ::ra8_c6link_take_ctx_t whose `out` is a MAC address.
 * @param[in] msg_v The decoded `Rpc`; must be non-null.
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok The address was copied out.
 * @retval k_ra8_err_protocol_error The answer carried no body, reported a
 *         failure, or held an address of the wrong length.
 * @pre @p ctx names a live link and a writable address.
 * @pre @p msg_v is still owned by the decoder.
 * @post On success the address holds six octets.
 * @post On failure the address is cleared.
 * @note Runs inside the pump, on the polling thread.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_c6link_take_mac(void* ctx, const void* msg_v)
{
  ra8_c6link_take_ctx_t* take = (ra8_c6link_take_ctx_t*)ctx;
  const Rpc*             msg  = (const Rpc*)msg_v;
  ra8_c6link_mac_t*      out  = (ra8_c6link_mac_t*)take->out;

  const RpcRespGetMacAddress* body = msg->resp_get_mac_address;
  if (body == nullptr) {
    return k_ra8_err_protocol_error;
  }
  const ra8_err_t reported = priv_c6link_resp(take->link, take->rpc_id, body->resp);
  if (reported != k_ra8_ok) {
    return reported;
  }
  return priv_c6link_copy_mac(out, &body->mac) ? k_ra8_ok : k_ra8_err_protocol_error;
}

ra8_err_t ra8_c6link_wifi_mac(ra8_c6link_t* link, ra8_c6link_mac_t* out)
{
  if ((link == nullptr) || (out == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  if (!ra8_c6link_is_open(link)) {
    return k_ra8_err_not_initialized;
  }
  *out = (ra8_c6link_mac_t){};

  RpcReqGetMacAddress body;
  rpc__req__get_mac_address__init(&body);
  /* `Req_GetMACAddress.mode` is a `wifi_interface_t`, not a `wifi_mode_t`: the
     co-processor passes it straight to `esp_wifi_get_mac()`. Select the station
     interface (0) so the address returned is the one the radio associates with.
     `WIFI_IF_AP` (1) hands back the SoftAP address (station address plus one),
     which no access point ever sees, so DHCP for the station never completes. */
  body.mode = (int32_t)k_ra8_c6link_iface_sta;

  Rpc req;
  rpc__init(&req);
  req.msg_type            = RPC_TYPE__Req;
  req.msg_id              = RPC_ID__Req_GetMACAddress;
  req.payload_case        = RPC__PAYLOAD_REQ_GET_MAC_ADDRESS;
  req.req_get_mac_address = &body;

  ra8_c6link_take_ctx_t take = {.link   = link,
                                .out    = out,
                                .rpc_id = (uint32_t)RPC_ID__Req_GetMACAddress};
  return priv_c6link_rpc_call(link,
                              &req,
                              (uint32_t)RPC_ID__Resp_GetMACAddress,
                              internal_c6link_take_mac,
                              &take);
}

/**
 * @brief Extract the associated AP's record from its answer.
 * @details An unassociated station is answered with a failure code rather than
 *        an empty record, so the result code is checked before the record is
 *        read.
 * @param[in] ctx A ::ra8_c6link_take_ctx_t whose `out` is an AP record.
 * @param[in] msg_v The decoded `Rpc`; must be non-null.
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok The record was copied out.
 * @retval k_ra8_err_protocol_error The answer carried no body or no record, or
 *         reported a failure -- which is what an unassociated station returns.
 * @pre @p ctx names a live link and a writable record.
 * @pre @p msg_v is still owned by the decoder.
 * @post On success every field of the record is set.
 * @post On failure the record is left cleared by the caller.
 * @note Runs inside the pump, on the polling thread.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_c6link_take_ap(void* ctx, const void* msg_v)
{
  ra8_c6link_take_ctx_t* take = (ra8_c6link_take_ctx_t*)ctx;
  const Rpc*             msg  = (const Rpc*)msg_v;
  ra8_c6link_ap_info_t*  out  = (ra8_c6link_ap_info_t*)take->out;

  const RpcRespWifiStaGetApInfo* body = msg->resp_wifi_sta_get_ap_info;
  if (body == nullptr) {
    return k_ra8_err_protocol_error;
  }
  const ra8_err_t reported = priv_c6link_resp(take->link, take->rpc_id, body->resp);
  if (reported != k_ra8_ok) {
    return reported;
  }
  if (body->ap_record == nullptr) {
    return k_ra8_err_protocol_error;
  }

  const WifiApRecord* rec = body->ap_record;
  out->ssid_len           = priv_c6link_copy_str(out->ssid, (uint8_t)sizeof out->ssid, &rec->ssid);
  out->channel            = (uint8_t)rec->primary;
  out->rssi               = (int8_t)rec->rssi;
  out->authmode           = rec->authmode;
  (void)priv_c6link_copy_mac(&out->bssid, &rec->bssid);
  return k_ra8_ok;
}

ra8_err_t ra8_c6link_wifi_ap_info(ra8_c6link_t* link, ra8_c6link_ap_info_t* out)
{
  if ((link == nullptr) || (out == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  if (!ra8_c6link_is_open(link)) {
    return k_ra8_err_not_initialized;
  }
  *out = (ra8_c6link_ap_info_t){};

  RpcReqWifiStaGetApInfo body;
  rpc__req__wifi_sta_get_ap_info__init(&body);

  Rpc req;
  rpc__init(&req);
  req.msg_type                 = RPC_TYPE__Req;
  req.msg_id                   = RPC_ID__Req_WifiStaGetApInfo;
  req.payload_case             = RPC__PAYLOAD_REQ_WIFI_STA_GET_AP_INFO;
  req.req_wifi_sta_get_ap_info = &body;

  ra8_c6link_take_ctx_t take = {.link   = link,
                                .out    = out,
                                .rpc_id = (uint32_t)RPC_ID__Req_WifiStaGetApInfo};
  return priv_c6link_rpc_call(link,
                              &req,
                              (uint32_t)RPC_ID__Resp_WifiStaGetApInfo,
                              internal_c6link_take_ap,
                              &take);
}
