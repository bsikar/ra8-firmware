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
#include "ra8_secure.h"

/* The public header restates the transaction geometry so consumers need no
   esp-hosted include path, and `src/internal/frame.zig` reproduces it again on
   the Zig side. These assertions are what keeps all three in step: if upstream
   ever changes either size, the build stops here rather than mis-framing on the
   wire. They live in the RPC layer because it is the C translation unit that
   includes the vendored declaration; the frame layer and the facade they
   were written beside are now Zig. */
static_assert((uint16_t)k_ra8_c6link_header_bytes == (uint16_t)sizeof(struct esp_payload_header),
              "k_ra8_c6link_header_bytes must equal sizeof(struct esp_payload_header)");
static_assert((uint16_t)k_ra8_c6link_frame_bytes == (uint16_t)ESP_TRANSPORT_SPI_MAX_BUF_SIZE,
              "k_ra8_c6link_frame_bytes must equal ESP_TRANSPORT_SPI_MAX_BUF_SIZE");
static_assert((uint16_t)k_ra8_c6link_max_payload ==
                ((uint16_t)k_ra8_c6link_frame_bytes - (uint16_t)k_ra8_c6link_header_bytes),
              "k_ra8_c6link_max_payload must be the frame size less the header");

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

RA8_PRIV ra8_err_t priv_c6link_rpc_call(ra8_c6link_t*        link,
                                        Rpc*                 req,
                                        uint32_t             resp_id,
                                        ra8_c6link_take_fn_t take,
                                        void*                take_ctx)
{
  if ((link == nullptr) || (req == nullptr) || (take == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  const ra8_err_t issuable = priv_c6link_rpc_issuable(link->open, link->wait.armed, link->tx_len);
  if (issuable != k_ra8_ok) {
    return issuable;
  }

  link->next_uid = link->next_uid + 1U;
  req->uid       = link->next_uid;

  const ra8_err_t staged = internal_c6link_rpc_stage(link, req);
  if (staged != k_ra8_ok) {
    ra8_secure_memzero(link->tx, sizeof(link->tx));
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
  ra8_secure_memzero(link->tx, sizeof(link->tx));

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
 * @note The three correlation conditions live in `internal/rpc_wait.zig`,
 *       which host tests drive directly.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_c6link_rpc_answer(ra8_c6link_t* link, const Rpc* msg)
{
  if (!priv_c6link_rpc_answers(link->wait.armed, link->wait.uid, link->wait.resp_id, msg->uid,
                               (uint32_t)msg->msg_id)) {
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
    priv_c6link_rpc_event(link, msg);
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
