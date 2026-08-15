/**
 * @file ra8_c6link_rpc.c
 * @brief The control plane: one protobuf message type, correlated by UID.
 * @ingroup grp_net
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details
 * Everything the co-processor is asked and everything it volunteers is one
 * generated `Rpc` message. Its `msg_type` says request, response or event; its
 * `msg_id` says which; its `uid` correlates an answer with the question; and
 * its payload is a `oneof` whose case number *is* the `msg_id`. Adding a
 * request to this library is therefore naming two enumerators and one generated
 * body type -- which is the whole reason a narrow, RA8-native API costs so
 * little here.
 *
 * The message is packed and unpacked by the vendored generated codec, never by
 * hand. Hand-encoding would prove only that this file and the co-processor
 * agree, which is a far weaker claim than the codec and the co-processor
 * agreeing -- and the codec is the same one the co-processor's own host driver
 * uses.
 *
 * @par One outstanding request at a time
 * The link holds a single wait slot. That is not a simplification to be lifted
 * later: this facade exists to be driven by a network stack that issues control
 * operations from one thread and expects them to complete, and a pipeline of
 * concurrent RPCs would need a queue, a timeout per entry and an ordering
 * policy that nothing in this tree wants. Requests that arrive while one is
 * outstanding are refused with `k_ra8_err_busy` rather than silently serialised.
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

/**
 * @brief Stage a packed request in the link's transmit transaction.
 * @details Packs directly into the transmit transaction behind its envelope,
 *        so the message is never copied twice: the encoder writes where the
 *        transport will read from.
 * @param[in,out] link Open handle; must be non-null.
 * @param[in] req Request to pack; must be non-null with its UID already set.
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok The request is staged and `tx_len` describes it.
 * @retval k_ra8_err_invalid_size The envelope plus message exceeds one frame.
 * @retval k_ra8_err_validation_failed The codec packed a different number of
 *         bytes than it predicted.
 * @pre The transmit transaction is free, which the busy check guarantees.
 * @pre @p req is fully populated.
 * @post On success the payload sits behind the payload header, ready to seal.
 * @post On failure `tx_len` is zero.
 * @note Packs directly into the transaction, so nothing is copied twice.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_c6link_rpc_stage(ra8_c6link_t* link, Rpc* req)
{
  link->tx_len = 0U;

  const size_t packed = rpc__get_packed_size(req);
  if (packed > (size_t)k_ra8_c6link_max_payload) {
    return k_ra8_err_invalid_size;
  }

  uint8_t*        payload = &link->tx[k_ra8_c6link_header_bytes];
  uint16_t        body_at = 0U;
  const ra8_err_t opened =
    priv_c6link_tlv_open(payload, (uint16_t)k_ra8_c6link_max_payload, (uint16_t)packed, &body_at);
  if (opened != k_ra8_ok) {
    return opened;
  }
  if (rpc__pack(req, &payload[body_at]) != packed) {
    return k_ra8_err_validation_failed;
  }

  link->tx_len = (uint16_t)((uint32_t)body_at + (uint32_t)packed);
  link->tx_if  = (uint8_t)ESP_SERIAL_IF;
  return k_ra8_ok;
}

RA8_PRIV ra8_err_t priv_c6link_resp(ra8_c6link_t* link, uint32_t rpc_id, int32_t resp)
{
  if (link == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (resp != 0) {
    link->fault.rpc_id = rpc_id;
    link->fault.resp   = resp;
    return k_ra8_err_protocol_error;
  }
  link->fault.rpc_id = 0U;
  link->fault.resp   = 0;
  return k_ra8_ok;
}

RA8_PRIV ra8_err_t priv_c6link_rpc_call(ra8_c6link_t*        link,
                                        Rpc*                 req,
                                        uint32_t             resp_id,
                                        ra8_c6link_take_fn_t take,
                                        void*                take_ctx)
{
  if ((link == nullptr) || (req == nullptr) || (take == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  if (!link->open) {
    return k_ra8_err_not_initialized;
  }
  if (link->wait.armed || (link->tx_len != 0U)) {
    return k_ra8_err_busy;
  }

  link->next_uid = link->next_uid + 1U;
  req->uid       = link->next_uid;

  const ra8_err_t staged = internal_c6link_rpc_stage(link, req);
  if (staged != k_ra8_ok) {
    return staged;
  }

  link->wait = (ra8_c6link_wait_t){
    .uid       = req->uid,
    .resp_id   = resp_id,
    .take      = take,
    .take_ctx  = take_ctx,
    .result    = k_ra8_ok,
    .armed     = true,
    .satisfied = false,
  };

  ra8_c6link_stats_t stats  = {};
  const ra8_err_t    pumped = priv_c6link_pump(link, (uint16_t)k_ra8_c6link_rpc_transfers, &stats);
  const bool         got    = link->wait.satisfied;
  const ra8_err_t    result = link->wait.result;
  link->wait                = (ra8_c6link_wait_t){};
  link->tx_len              = 0U;

  if (pumped != k_ra8_ok) {
    return pumped;
  }
  if (!got) {
    link->fault.rpc_id = (uint32_t)req->msg_id;
    link->fault.resp   = 0;
    return k_ra8_err_timeout;
  }
  return result;
}

/**
 * @brief Turn a decoded station-connected event into a first-party record.
 * @details The association's own account of which AP it reached, which is not
 *        necessarily the one that was asked for when the request left the
 *        channel and BSSID open.
 * @param[out] ev Record to fill; must be non-null and already cleared.
 * @param[in] body Decoded event body; null leaves @p ev with its kind only.
 * @return Nothing.
 * @pre @p ev has been zero-initialised by the caller.
 * @pre @p body belongs to a message still owned by the decoder.
 * @post @p ev names the AP the station reached.
 * @post Every string in @p ev is NUL-terminated.
 * @note Split out so the event switch stays inside NASA Rule 4.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_c6link_rpc_ev_connected(ra8_c6link_event_t*          ev,
                                                          const WifiEventStaConnected* body)
{
  if (body == nullptr) {
    return;
  }
  ev->ssid_len = priv_c6link_copy_str(ev->ssid, (uint8_t)sizeof ev->ssid, &body->ssid);
  ev->channel  = (uint8_t)body->channel;
  (void)priv_c6link_copy_mac(&ev->bssid, &body->bssid);
}

/**
 * @brief Turn a decoded station-disconnected event into a first-party record.
 * @details Carries the field an IP driver acts on: the 802.11 reason code,
 *        which is how a vanished AP is told from a rejected passphrase.
 * @param[out] ev Record to fill; must be non-null and already cleared.
 * @param[in] body Decoded event body; null leaves @p ev with its kind only.
 * @return Nothing.
 * @pre @p ev has been zero-initialised by the caller.
 * @pre @p body belongs to a message still owned by the decoder.
 * @post @p ev carries the 802.11 reason code the AP or the radio supplied.
 * @post Every string in @p ev is NUL-terminated.
 * @note The reason code is the actionable field: an IP driver distinguishes
 *       "AP went away" from "credentials rejected" by it alone.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_c6link_rpc_ev_disconnected(ra8_c6link_event_t*             ev,
                                                             const WifiEventStaDisconnected* body)
{
  if (body == nullptr) {
    return;
  }
  ev->ssid_len = priv_c6link_copy_str(ev->ssid, (uint8_t)sizeof ev->ssid, &body->ssid);
  ev->reason   = (uint16_t)body->reason;
  ev->rssi     = (int8_t)body->rssi;
  (void)priv_c6link_copy_mac(&ev->bssid, &body->bssid);
}

/**
 * @brief Decode one announcement and hand it to the link's callback.
 * @details One switch over the four announcements this facade models.
 *        Everything else the protocol defines returns before a record is
 *        built.
 * @param[in,out] link Open handle; must be non-null.
 * @param[in] msg Decoded message whose `msg_type` is `Event`; must be non-null.
 * @return Nothing.
 * @pre @p msg is still owned by the decoder and its arena is live.
 * @pre @p link is open.
 * @post Announcements this library models were delivered exactly once.
 * @post Announcements it does not model changed no link state.
 * @note Events outside the four modelled kinds are ignored rather than
 *       half-decoded into a record no caller can interpret.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_c6link_rpc_event(ra8_c6link_t* link, const Rpc* msg)
{
  ra8_c6link_event_t ev = {};

  switch ((int32_t)msg->msg_id) {
    case (int32_t)RPC_ID__Event_ESPInit:
      ev.kind = k_ra8_c6link_event_boot;
      if (msg->event_esp_init != nullptr) {
        ev.reset_reason = msg->event_esp_init->cp_reset_reason;
      }
      break;
    case (int32_t)RPC_ID__Event_StaConnected:
      ev.kind = k_ra8_c6link_event_sta_connected;
      if (msg->event_sta_connected != nullptr) {
        internal_c6link_rpc_ev_connected(&ev, msg->event_sta_connected->sta_connected);
      }
      break;
    case (int32_t)RPC_ID__Event_StaDisconnected:
      ev.kind = k_ra8_c6link_event_sta_disconnected;
      if (msg->event_sta_disconnected != nullptr) {
        internal_c6link_rpc_ev_disconnected(&ev, msg->event_sta_disconnected->sta_disconnected);
      }
      break;
    case (int32_t)RPC_ID__Event_WifiEventNoArgs:
      ev.kind = k_ra8_c6link_event_wifi;
      if (msg->event_wifi_event_no_args != nullptr) {
        ev.wifi_event_id = msg->event_wifi_event_no_args->event_id;
      }
      break;
    default:
      return;
  }
  priv_c6link_emit(link, &ev);
}

/**
 * @brief Offer a decoded message to the outstanding wait.
 * @details The correlation step. An answer is only an answer to the
 *        outstanding request when the wait is armed, the UID is the one that
 *        was sent, and the message id is the one that request is answered by.
 * @param[in,out] link Open handle; must be non-null.
 * @param[in] msg Decoded message; must be non-null.
 * @return true when the wait was satisfied by @p msg.
 * @retval true The answer matched the outstanding request and was extracted.
 * @retval false No request is outstanding, or @p msg is not its answer.
 * @pre @p msg is still owned by the decoder.
 * @pre @p link is open.
 * @post The wait is satisfied at most once.
 * @post The extractor ran exactly once when it matched.
 * @note All three conditions must hold: a UID that matches but an id that does
 *       not is a different question's answer arriving late.
 * @since 0.1.0
 *
 * @par MC/DC:
 * `armed && uid == wait.uid && msg_id == wait.resp_id` is a three-condition
 * decision; `tests/test_ra8_c6link.c` drives the N+1 vectors.
 */
RA8_INTERNAL static bool internal_c6link_rpc_answer(ra8_c6link_t* link, const Rpc* msg)
{
  if (!link->wait.armed || (msg->uid != link->wait.uid) ||
      ((uint32_t)msg->msg_id != link->wait.resp_id)) {
    return false;
  }
  link->wait.result    = link->wait.take(link->wait.take_ctx, msg);
  link->wait.satisfied = true;
  return true;
}

RA8_PRIV bool priv_c6link_rpc_consume(ra8_c6link_t* link, const uint8_t* payload, uint16_t len)
{
  if ((link == nullptr) || (payload == nullptr)) {
    return false;
  }

  uint16_t       proto_len = 0U;
  const uint8_t* proto     = priv_c6link_tlv_body(payload, len, &proto_len);
  if (proto == nullptr) {
    if (link->stats != nullptr) {
      link->stats->undecodable++;
    }
    return false;
  }

  ProtobufCAllocator alloc = {};
  priv_c6link_arena_bind(&alloc, link);
  priv_c6link_arena_reset(link);

  Rpc* msg = rpc__unpack(&alloc, (size_t)proto_len, proto);
  if (msg == nullptr) {
    if (link->stats != nullptr) {
      link->stats->undecodable++;
    }
    priv_c6link_arena_reset(link);
    return false;
  }
  if (link->stats != nullptr) {
    link->stats->rpc_in++;
  }

  bool stop = false;
  if (msg->msg_type == RPC_TYPE__Event) {
    internal_c6link_rpc_event(link, msg);
  } else if (msg->msg_type == RPC_TYPE__Resp) {
    stop = internal_c6link_rpc_answer(link, msg);
  } else {
    /* A request arriving at a host is a co-processor defect, not a message
       this side has any handler for. Counted, dropped, never acted on. */
    if (link->stats != nullptr) {
      link->stats->undecodable++;
    }
  }

  rpc__free_unpacked(msg, &alloc);
  priv_c6link_arena_reset(link);
  return stop;
}
