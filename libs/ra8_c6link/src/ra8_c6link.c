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

/* The public header restates the transaction geometry so consumers need no
   esp-hosted include path, and `src/internal/frame.zig` reproduces it again on
   the Zig side. These assertions are what keeps all three in step: if upstream
   ever changes either size, the build stops here rather than mis-framing on the
   wire. They live in this translation unit because it is the facade that still
   includes the vendored declaration; the frame layer they were written beside
   is now Zig. */
static_assert((uint16_t)k_ra8_c6link_header_bytes == (uint16_t)sizeof(struct esp_payload_header),
              "k_ra8_c6link_header_bytes must equal sizeof(struct esp_payload_header)");
static_assert((uint16_t)k_ra8_c6link_frame_bytes == (uint16_t)ESP_TRANSPORT_SPI_MAX_BUF_SIZE,
              "k_ra8_c6link_frame_bytes must equal ESP_TRANSPORT_SPI_MAX_BUF_SIZE");
static_assert((uint16_t)k_ra8_c6link_max_payload ==
                ((uint16_t)k_ra8_c6link_frame_bytes - (uint16_t)k_ra8_c6link_header_bytes),
              "k_ra8_c6link_max_payload must be the frame size less the header");

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
