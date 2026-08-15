/**
 * @file ra8_c6link.c
 * @brief Link lifecycle, frame routing and the identity round-trip.
 * @ingroup grp_net
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details
 * The handle's own file: opening and closing it, deciding which consumer a
 * received frame belongs to, delivering announcements, and the one request that
 * belongs to the link rather than to Wi-Fi -- asking the co-processor who it
 * is.
 *
 * @par Why the identity request lives here
 * `Req_GetCoprocessorFwVersion` is the cheapest complete proof that the whole
 * stack works, because its answer is a fact this host can check rather than
 * merely receive: this firmware is built against a pinned esp-hosted commit and
 * the co-processor image was built from the same one, so the two versions must
 * agree exactly. A bring-up that gets the right version back has proven
 * framing, checksum, envelope, protobuf encode, protobuf decode and UID
 * correlation in one call.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include "ra8_c6link.h"

#include <stddef.h>
#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_c6link_internal.h"

RA8_PRIV uint8_t priv_c6link_copy_str(char* dst, uint8_t cap, const ProtobufCBinaryData* src)
{
  if ((dst == nullptr) || (cap == 0U)) {
    return 0U;
  }
  dst[0] = '\0';
  if ((src == nullptr) || (src->data == nullptr)) {
    return 0U;
  }

  const size_t room = (size_t)cap - 1U;
  const size_t take = (src->len < room) ? src->len : room;
  for (size_t i = 0U; i < take; i++) {
    dst[i] = (char)src->data[i];
  }
  dst[take] = '\0';
  return (uint8_t)take;
}

RA8_PRIV bool priv_c6link_copy_mac(ra8_c6link_mac_t* dst, const ProtobufCBinaryData* src)
{
  if (dst == nullptr) {
    return false;
  }
  *dst = (ra8_c6link_mac_t){};
  if ((src == nullptr) || (src->data == nullptr) || (src->len != (size_t)k_ra8_c6link_mac_bytes)) {
    return false;
  }
  for (uint8_t i = 0U; i < (uint8_t)k_ra8_c6link_mac_bytes; i++) {
    dst->octet[i] = src->data[i];
  }
  return true;
}

RA8_PRIV void priv_c6link_emit(ra8_c6link_t* link, const ra8_c6link_event_t* ev)
{
  if ((link == nullptr) || (ev == nullptr)) {
    return;
  }
  if (ev->kind == k_ra8_c6link_event_boot) {
    link->boot_seen = true;
  }
  if (link->stats != nullptr) {
    link->stats->events++;
  }
  if (link->event_cb != nullptr) {
    link->event_cb(link->cb_ctx, ev);
  }
}

RA8_PRIV bool priv_c6link_dispatch(ra8_c6link_t* link, const ra8_c6link_rx_view_t* view)
{
  if ((link == nullptr) || (view == nullptr)) {
    return false;
  }
  const uint8_t* payload = &link->rx[view->offset];

  if (view->if_type == (uint8_t)ESP_SERIAL_IF) {
    return priv_c6link_rpc_consume(link, payload, view->len);
  }
  if ((view->if_type == (uint8_t)ESP_STA_IF) || (view->if_type == (uint8_t)ESP_AP_IF)) {
    if (link->stats != nullptr) {
      link->stats->eth_in++;
    }
    if (link->rx_cb != nullptr) {
      link->rx_cb(link->cb_ctx, payload, view->len);
    }
    return false;
  }
  /* `ESP_PRIV_IF` lands here. Upstream reads peripheral-side capabilities from it, but
     this co-processor build transmits its only privileged frame with a
     checksum computed as if `if_num` were zero (#529), so a conformant host
     never sees a valid one and nothing in this library depends on it. */
  if (link->stats != nullptr) {
    link->stats->unrouted++;
  }
  return false;
}

/**
 * @brief Reject a configuration the link cannot honour.
 * @details Rejects a seam with a missing row here rather than discovering it
 *        as a null call at the first transaction, and refuses an arena too
 *        small to decode the largest message this library reads.
 * @param[in] cfg Configuration the caller supplied; must be non-null.
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok Every field is usable.
 * @retval k_ra8_err_null_ptr A transport row or the arena pointer was null.
 * @retval k_ra8_err_invalid_size The arena is smaller than the minimum.
 * @pre @p cfg is non-null, which the caller has already checked.
 * @pre The caller has not yet copied anything out of @p cfg.
 * @post No state is modified.
 * @post Exactly one code is returned, naming the first failing check.
 * @note Split out of ::ra8_c6link_open so that function stays inside the NASA
 *       Rule 4 length budget.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_c6link_check_cfg(const ra8_c6link_cfg_t* cfg)
{
  if ((cfg->transport.transfer == nullptr) || (cfg->transport.handshake_active == nullptr) ||
      (cfg->transport.delay_ms == nullptr) || (cfg->arena == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  if (cfg->arena_bytes < (uint32_t)k_ra8_c6link_arena_min) {
    return k_ra8_err_invalid_size;
  }
  return k_ra8_ok;
}

ra8_err_t ra8_c6link_open(ra8_c6link_t* link, const ra8_c6link_cfg_t* cfg)
{
  if ((link == nullptr) || (cfg == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  if (link->open) {
    return k_ra8_err_invalid_state;
  }
  const ra8_err_t bad = internal_c6link_check_cfg(cfg);
  if (bad != k_ra8_ok) {
    return bad;
  }

  link->transport   = cfg->transport;
  link->event_cb    = cfg->event_cb;
  link->rx_cb       = cfg->rx_cb;
  link->cb_ctx      = cfg->cb_ctx;
  link->arena       = cfg->arena;
  link->arena_bytes = cfg->arena_bytes;
  link->arena_used  = 0U;
  link->arena_last  = 0U;
  link->next_uid    = 0U;
  link->wait        = (ra8_c6link_wait_t){};
  link->fault       = (ra8_c6link_fault_t){};
  link->stats       = nullptr;
  link->tx_len      = 0U;
  link->tx_if       = 0U;
  link->boot_seen   = false;
  link->open        = true;
  return k_ra8_ok;
}

ra8_err_t ra8_c6link_close(ra8_c6link_t* link)
{
  if (link == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (!link->open) {
    return k_ra8_err_not_initialized;
  }
  link->transport = (ra8_c6link_transport_t){};
  link->event_cb  = nullptr;
  link->rx_cb     = nullptr;
  link->cb_ctx    = nullptr;
  link->arena     = nullptr;
  link->stats     = nullptr;
  link->tx_len    = 0U;
  link->wait      = (ra8_c6link_wait_t){};
  link->open      = false;
  return k_ra8_ok;
}

bool ra8_c6link_is_open(const ra8_c6link_t* link)
{
  return (link != nullptr) && link->open;
}

ra8_err_t ra8_c6link_last_fault(const ra8_c6link_t* link, ra8_c6link_fault_t* out)
{
  if ((link == nullptr) || (out == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  *out = link->fault;
  return k_ra8_ok;
}

ra8_err_t ra8_c6link_poll(ra8_c6link_t* link, uint16_t max_transactions, ra8_c6link_stats_t* stats)
{
  if (link == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (!link->open) {
    return k_ra8_err_not_initialized;
  }
  if (max_transactions == 0U) {
    return k_ra8_err_invalid_arg;
  }

  ra8_c6link_stats_t local = {};
  const ra8_err_t    err   = priv_c6link_pump(link, max_transactions, &local);
  if (stats != nullptr) {
    *stats = local;
  }
  return err;
}

ra8_err_t
ra8_c6link_await_ready(ra8_c6link_t* link, uint16_t max_transactions, ra8_c6link_fw_version_t* out)
{
  if ((link == nullptr) || (out == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  if (!link->open) {
    return k_ra8_err_not_initialized;
  }
  if (max_transactions == 0U) {
    return k_ra8_err_invalid_arg;
  }
  if (link->tx_len != 0U) {
    return k_ra8_err_busy;
  }

  /* Announce this host first. A co-processor that has just booted services no
     RPC until the host has introduced itself, and one that is already up
     ignores a restatement of capabilities that have not changed -- so sending
     it unconditionally is both necessary and harmless. */
  const uint8_t caps =
    priv_c6link_caps(&link->tx[k_ra8_c6link_header_bytes], (uint8_t)k_ra8_c6link_caps_bytes);
  if (caps == 0U) {
    return k_ra8_err_invalid_size;
  }
  link->tx_len = (uint16_t)caps;
  link->tx_if  = (uint8_t)ESP_PRIV_IF;

  ra8_c6link_stats_t local     = {};
  const ra8_err_t    announced = priv_c6link_pump(link, max_transactions, &local);
  link->tx_len                 = 0U;
  if (announced != k_ra8_ok) {
    return announced;
  }

  /* Readiness is the answer to a question, not the arrival of an event. See the
     header: `Event_ESPInit` fires once when the CO-PROCESSOR boots, which is
     not when this host boots, so waiting for it works exactly once. */
  return ra8_c6link_fw_version(link, out);
}

/**
 * @brief Extract the co-processor identity from its answer.
 * @details The co-processor's identity is the host/co-processor version lock,
 *        so every field is copied out for the caller to compare rather than
 *        judged here.
 * @param[in] ctx A ::ra8_c6link_take_ctx_t whose `out` is the identity record.
 * @param[in] msg_v The decoded `Rpc`; must be non-null.
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok The identity was copied out.
 * @retval k_ra8_err_protocol_error The answer carried no body, or the
 *         co-processor reported a failure.
 * @pre @p ctx names a live link and a writable identity record.
 * @pre @p msg_v is still owned by the decoder.
 * @post On success every field of the record is set.
 * @post On failure the link's fault slot names the request.
 * @note Runs inside the pump, on the polling thread.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_c6link_take_fw(void* ctx, const void* msg_v)
{
  ra8_c6link_take_ctx_t*         take = (ra8_c6link_take_ctx_t*)ctx;
  const Rpc*                     msg  = (const Rpc*)msg_v;
  ra8_c6link_fw_version_t* const out  = (ra8_c6link_fw_version_t*)take->out;

  const RpcRespGetCoprocessorFwVersion* body = msg->resp_get_coprocessor_fwversion;
  if (body == nullptr) {
    return k_ra8_err_protocol_error;
  }
  out->major   = body->major1;
  out->minor   = body->minor1;
  out->patch   = body->patch1;
  out->chip_id = body->chip_id;
  out->target_len =
    priv_c6link_copy_str(out->target, (uint8_t)sizeof out->target, &body->idf_target);
  return priv_c6link_resp(take->link, (uint32_t)RPC_ID__Req_GetCoprocessorFwVersion, body->resp);
}

ra8_err_t ra8_c6link_fw_version(ra8_c6link_t* link, ra8_c6link_fw_version_t* out)
{
  if ((link == nullptr) || (out == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  if (!link->open) {
    return k_ra8_err_not_initialized;
  }
  *out = (ra8_c6link_fw_version_t){};

  RpcReqGetCoprocessorFwVersion body;
  rpc__req__get_coprocessor_fw_version__init(&body);

  Rpc req;
  rpc__init(&req);
  req.msg_type                      = RPC_TYPE__Req;
  req.msg_id                        = RPC_ID__Req_GetCoprocessorFwVersion;
  req.payload_case                  = RPC__PAYLOAD_REQ_GET_COPROCESSOR_FWVERSION;
  req.req_get_coprocessor_fwversion = &body;

  ra8_c6link_take_ctx_t take = {.link = link, .out = out};
  return priv_c6link_rpc_call(link,
                              &req,
                              (uint32_t)RPC_ID__Resp_GetCoprocessorFwVersion,
                              internal_c6link_take_fw,
                              &take);
}

ra8_err_t ra8_c6link_eth_send(ra8_c6link_t* link, const uint8_t* frame, uint16_t len)
{
  if ((link == nullptr) || (frame == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  if (!link->open) {
    return k_ra8_err_not_initialized;
  }
  if ((len == 0U) || (len > (uint16_t)k_ra8_c6link_max_payload)) {
    return k_ra8_err_invalid_size;
  }
  if (link->tx_len != 0U) {
    return k_ra8_err_busy;
  }

  for (uint16_t i = 0U; i < len; i++) {
    link->tx[(uint16_t)k_ra8_c6link_header_bytes + i] = frame[i];
  }
  link->tx_len = len;
  link->tx_if  = (uint8_t)ESP_STA_IF;

  ra8_c6link_stats_t local  = {};
  const ra8_err_t    pumped = priv_c6link_pump(link, (uint16_t)k_ra8_c6link_hs_giveup, &local);
  if (pumped != k_ra8_ok) {
    link->tx_len = 0U;
    return pumped;
  }
  if (link->tx_len != 0U) {
    link->tx_len = 0U;
    return k_ra8_err_hw_timeout;
  }
  return k_ra8_ok;
}
