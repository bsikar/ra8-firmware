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
 * `Resp_WifiStaGetApInfo`. The address query, `ra8_c6link_wifi_mac`, is
 * Zig now (`ra8_c6link_mac_abi.zig`, RA8FW-509).
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


ra8_err_t ra8_c6link_sta_cfg_set(ra8_c6link_sta_cfg_t* cfg, const char* ssid, const char* pass)
{
  if ((cfg == nullptr) || (ssid == nullptr)) {
    if (cfg != nullptr) {
      ra8_secure_memzero(cfg, sizeof(*cfg));
    }
    return k_ra8_err_null_ptr;
  }
  ra8_secure_memzero(cfg, sizeof(*cfg));

  const uint8_t ssid_len = priv_c6link_sta_len(ssid, (uint8_t)sizeof cfg->ssid);
  const uint8_t pass_len =
    (pass == nullptr) ? 0U : priv_c6link_sta_len(pass, (uint8_t)sizeof cfg->pass);
  if (!priv_c6link_sta_credentials_valid(ssid_len, pass_len)) {
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
  priv_c6link_sta_policy_t policy;
  priv_c6link_sta_policy(&policy);

  WifiScanThreshold threshold;
  wifi_scan_threshold__init(&threshold);
  threshold.authmode = policy.auth_threshold;

  WifiPmfConfig pmf;
  wifi_pmf_config__init(&pmf);
  pmf.capable = (policy.pmf_capable != 0);

  ra8_c6link_sta_wire_buf_t buf = {};
  internal_c6link_sta_stage(&buf, cfg);

  WifiStaConfig sta;
  wifi_sta_config__init(&sta);
  sta.ssid.data     = buf.ssid;
  sta.ssid.len      = (size_t)cfg->ssid_len;
  sta.password.data = buf.pass;
  sta.password.len  = (size_t)cfg->pass_len;
  sta.scan_method   = policy.scan_method;
  sta.sort_method   = policy.sort_method;
  sta.channel       = (uint32_t)cfg->channel;
  sta.bssid_set     = cfg->bssid_set;
  sta.bssid.data    = buf.bssid;
  sta.bssid.len     = priv_c6link_sta_bssid_len(cfg->bssid_set);
  sta.threshold     = &threshold;
  sta.pmf_cfg       = &pmf;

  WifiConfig wcfg;
  wifi_config__init(&wcfg);
  wcfg.u_case = WIFI_CONFIG__U_STA;
  wcfg.sta    = &sta;

  RpcReqWifiSetConfig body;
  rpc__req__wifi_set_config__init(&body);
  body.iface = policy.iface;
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
  if (!priv_c6link_sta_credentials_valid(cfg->ssid_len, cfg->pass_len)) {
    return k_ra8_err_invalid_size;
  }

  const ra8_err_t configured = internal_c6link_sta_set_config(link, cfg);
  if (configured != k_ra8_ok) {
    return configured;
  }
  return priv_c6link_bare_req(link, (uint32_t)RPC_ID__Req_WifiConnect);
}
