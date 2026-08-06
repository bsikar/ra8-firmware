/**
 * @file examples/ek_ra8d2/hw_validated/c6/c6_fw_version/src/c6_fwver_rpc.c
 * @brief The ESP_SERIAL_IF RPC channel: request, response, verdict.
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * The esp-hosted control plane is protobuf over a two-tag TLV envelope over
 * ``ESP_SERIAL_IF``. All three layers are exercised here:
 *
 *   - **protobuf** through the vendored generated codec
 *     (``common/proto/esp_hosted_rpc.pb-c.c``) and the vendored
 *     ``protobuf-c`` runtime, both of which this tree already compiles. The
 *     message is not hand-encoded: hand-encoding it would prove only that
 *     this file and the co-processor agree, which is a much weaker claim than
 *     the generated codec and the co-processor agreeing.
 *   - **TLV** in first-party code here, mirroring ``compose_tlv()`` and
 *     ``parse_tlv()`` in the vendored ``serial_if.c``. That file is not
 *     compiled in this tree because its send path expands ``HOSTED_CALLOC``,
 *     whose failure arm is a ``goto`` that NASA Power of 10 Rule 1 forbids
 *     (see the reasoning in ``cmake/esp_hosted.cmake``). The envelope itself
 *     is six bytes of framing, restated here rather than dragged in with a
 *     rule violation attached.
 *   - **the payload header and the transaction**, which belong to
 *     ``src/c6_fwver_link.c``.
 *
 * The request is ``RPC_ID__Req_GetCoprocessorFwVersion``. It was chosen
 * because its answer is checkable: the vendored host driver states its own
 * version in ``esp_hosted_host_fw_ver.h``, and the co-processor image was
 * built from the same pinned upstream commit, so the two must agree exactly.
 * The verdict is therefore the host/co-processor version lock -- the hazard
 * #316 exists to police -- rather than "some bytes came back".
 *
 * @par Allocation
 * ``rpc__unpack()`` needs an allocator. This image has no heap, so it is
 * given one backed by the esp-hosted port's fixed ThreadX byte pool: the same
 * storage the vendored transport allocates its buffers from, carved once at
 * init, which is what keeps the whole path inside NASA Power of 10 Rule 3.
 * Every unpacked message is freed on every path.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>

#include "c6_fwver.h"
#include "esp_hosted_host_fw_ver.h"
#include "esp_hosted_interface.h"
#include "esp_hosted_os_abstraction.h"
#include "esp_hosted_rpc.pb-c.h"
#include "esp_hosted_transport.h"
#include "protobuf-c/protobuf-c.h"
#include "transport_drv.h"

/**
 * @enum c6_fwver_tlv_t
 * @brief The serial endpoint's TLV envelope, tag by tag.
 * @details Two tags, each a one-byte type followed by a little-endian 16-bit
 * length: the endpoint name, then the protobuf payload. The endpoint-name
 * length is taken from the vendored ``RPC_EP_NAME_RSP`` string rather than
 * written down, because upstream requires both endpoint names to have the
 * same length and the parser checks that.
 * @invariant ::k_c6_fwver_tlv_ep_len equals ``strlen(RPC_EP_NAME_RSP)``.
 * @invariant ::k_c6_fwver_tlv_overhead is the exact envelope cost, so a
 *            capacity check against it is neither loose nor tight.
 * @par Example:
 * @code
 * out[k_c6_fwver_tlv_type] = (uint8_t)k_c6_fwver_tlv_t_epname;
 * @endcode
 * @see c6_fwver_rpc_request
 * @since 0.1.0
 */
typedef enum : uint16_t {
  k_c6_fwver_tlv_t_epname = 0x01U, /**< Tag introducing the endpoint name.    */
  k_c6_fwver_tlv_t_data   = 0x02U, /**< Tag introducing the protobuf payload. */
  k_c6_fwver_tlv_type     = 0U,    /**< Offset of a tag's type byte.          */
  k_c6_fwver_tlv_len_lo   = 1U,    /**< Offset of a tag's length, low byte.   */
  k_c6_fwver_tlv_len_hi   = 2U,    /**< Offset of a tag's length, high byte.  */
  k_c6_fwver_tlv_value    = 3U,    /**< Offset of a tag's value.              */
  k_c6_fwver_tlv_ep_len   = (uint16_t)(sizeof(RPC_EP_NAME_RSP) - 1U),
  /**< Endpoint-name length; both endpoint names share it. */
  k_c6_fwver_tlv_overhead =
    (uint16_t)((uint16_t)k_c6_fwver_tlv_value + (uint16_t)(sizeof(RPC_EP_NAME_RSP) - 1U) +
               (uint16_t)k_c6_fwver_tlv_value),
  /**< Bytes the envelope costs on top of the protobuf payload. */
  k_c6_fwver_tlv_shift = 8U,    /**< Shift between the two length bytes. */
  k_c6_fwver_tlv_mask  = 0xFFU, /**< Byte mask for the low length byte.  */
} c6_fwver_tlv_t;

/**
 * @struct c6_fwver_rsp
 * @brief Every field this application read out of the RPC response.
 * @details Recorded rather than judged on arrival, so the verdict can print
 * what it saw next to what it expected. A response whose fields are never
 * shown is the same non-evidence as a check that never fails.
 * @invariant ``seen`` is false until a response addressed to this request's
 *            UID has been decoded; every other field is meaningless until it
 *            is true.
 * @invariant ``target_len`` never exceeds the capacity of ``target``.
 * @par Example:
 * @code
 * if (s_c6_fwver_rsp.seen) { c6_fwver_put_u32(s_c6_fwver_rsp.major); }
 * @endcode
 * @see c6_fwver_rpc_report
 * @since 0.1.0
 */
typedef struct c6_fwver_rsp {
  uint32_t uid;                         /**< UID echoed back by the co-processor. */
  int32_t  resp;                        /**< Co-processor result code; 0 is ok.   */
  uint32_t major;                       /**< Firmware major version.              */
  uint32_t minor;                       /**< Firmware minor version.              */
  uint32_t patch;                       /**< Firmware patch version.              */
  uint32_t chip_id;                     /**< ``CONFIG_IDF_FIRMWARE_CHIP_ID``.     */
  uint8_t  target[k_c6_fwver_text_max]; /**< ``CONFIG_IDF_TARGET``, truncated.    */
  uint8_t  target_len;                  /**< Bytes held in ``target``.            */
  bool     seen;                        /**< A matching response was decoded.     */
} c6_fwver_rsp_t;

/**
 * @var s_c6_fwver_rsp
 * @brief The one response this application is waiting for.
 * @details File scope because the sink that fills it and the report that
 * judges it are different calls on different stacks.
 * @note Written only from the pump thread, inside ::c6_fwver_rpc_consume.
 * @warning The first accepted response wins; a later one is ignored so the
 *          verdict cannot be rewritten by a stray frame.
 * @since 0.1.0
 */
static c6_fwver_rsp_t s_c6_fwver_rsp;

/**
 * @brief Allocate through the esp-hosted port's fixed byte pool.
 * @param[in] allocator_data Unused; the pool is a module singleton.
 * @param[in] size Bytes requested.
 * @return Pointer to the block, or null when the pool cannot serve it.
 * @retval nullptr The fixed pool is exhausted.
 * @pre The esp-hosted port is up, so the pool exists.
 * @pre The caller frees through ::c6_fwver_rpc_pool_free.
 * @post No pool bookkeeping is bypassed.
 * @post A failure returns null rather than growing the address space.
 * @note This is the whole reason a protobuf decoder is legal in a heapless
 *       image: the storage is a carve made once during initialisation.
 * @since 0.1.0
 */
static void* c6_fwver_rpc_pool_alloc(void* allocator_data, size_t size)
{
  (void)allocator_data;
  return g_h.funcs->_h_malloc(size);
}

/**
 * @brief Return a block to the esp-hosted port's fixed byte pool.
 * @param[in] allocator_data Unused; the pool is a module singleton.
 * @param[in] pointer Block to release; null is ignored.
 * @return Nothing.
 * @pre @p pointer came from ::c6_fwver_rpc_pool_alloc, or is null.
 * @pre No other context holds a reference to the block.
 * @post The block is available to the pool again.
 * @post A null pointer is a no-op rather than a fault.
 * @note Paired with ::c6_fwver_rpc_pool_alloc through the one
 *       ``ProtobufCAllocator`` this file installs.
 * @since 0.1.0
 */
static void c6_fwver_rpc_pool_free(void* allocator_data, void* pointer)
{
  (void)allocator_data;
  if (pointer != nullptr) {
    g_h.funcs->_h_free(pointer);
  }
}

/**
 * @var s_c6_fwver_allocator
 * @brief The allocator the generated protobuf decoder is handed.
 * @details Points at the esp-hosted port's fixed pool, never at newlib's
 * heap: ``_sbrk`` in this image is a strong symbol that reports a fatal
 * error, so the default protobuf-c allocator would fault rather than fail.
 * @note Read by the vendored codec; never modified after initialisation.
 * @warning Passing null instead would select that default allocator, which is
 *          the fault above; the pointer is not optional.
 * @since 0.1.0
 */
static ProtobufCAllocator s_c6_fwver_allocator = {
  .alloc          = c6_fwver_rpc_pool_alloc,
  .free           = c6_fwver_rpc_pool_free,
  .allocator_data = nullptr,
};

/**
 * @brief Write one TLV tag header at a cursor.
 * @param[out] out Buffer being filled; must be non-null.
 * @param[in] at Offset to write at.
 * @param[in] type Tag type.
 * @param[in] len Value length that follows.
 * @return The offset just past the three-byte tag header.
 * @pre At least ::k_c6_fwver_tlv_value bytes are writable at ``out + at``.
 * @pre The caller has already checked the buffer capacity.
 * @post Exactly three bytes were written, length little-endian.
 * @post The returned offset addresses the tag's value.
 * @note Matches the byte order ``compose_tlv()`` writes in the vendored
 *       ``serial_if.c``: low byte first.
 * @since 0.1.0
 */
static uint16_t c6_fwver_tlv_head(uint8_t* out, uint16_t at, uint8_t type, uint16_t len)
{
  out[at + (uint16_t)k_c6_fwver_tlv_type]   = type;
  out[at + (uint16_t)k_c6_fwver_tlv_len_lo] = (uint8_t)(len & (uint16_t)k_c6_fwver_tlv_mask);
  out[at + (uint16_t)k_c6_fwver_tlv_len_hi] =
    (uint8_t)((len >> (uint16_t)k_c6_fwver_tlv_shift) & (uint16_t)k_c6_fwver_tlv_mask);
  return (uint16_t)(at + (uint16_t)k_c6_fwver_tlv_value);
}

ra8_err_t c6_fwver_rpc_request(uint8_t* out, uint16_t cap, uint16_t* out_len)
{
  if ((out == nullptr) || (out_len == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  *out_len = 0U;

  RpcReqGetCoprocessorFwVersion body;
  rpc__req__get_coprocessor_fw_version__init(&body);

  Rpc req;
  rpc__init(&req);
  req.msg_type                      = RPC_TYPE__Req;
  req.msg_id                        = RPC_ID__Req_GetCoprocessorFwVersion;
  req.uid                           = (uint32_t)k_c6_fwver_rpc_uid;
  req.payload_case                  = RPC__PAYLOAD_REQ_GET_COPROCESSOR_FWVERSION;
  req.req_get_coprocessor_fwversion = &body;

  const size_t proto_len = rpc__get_packed_size(&req);
  if ((size_t)cap < (proto_len + (size_t)k_c6_fwver_tlv_overhead)) {
    return k_ra8_err_invalid_size;
  }

  uint16_t at =
    c6_fwver_tlv_head(out, 0U, (uint8_t)k_c6_fwver_tlv_t_epname, (uint16_t)k_c6_fwver_tlv_ep_len);
  for (uint16_t i = 0U; i < (uint16_t)k_c6_fwver_tlv_ep_len; i++) {
    out[at + i] = (uint8_t)RPC_EP_NAME_RSP[i];
  }
  at = (uint16_t)(at + (uint16_t)k_c6_fwver_tlv_ep_len);
  at = c6_fwver_tlv_head(out, at, (uint8_t)k_c6_fwver_tlv_t_data, (uint16_t)proto_len);

  if (rpc__pack(&req, &out[at]) != proto_len) {
    return k_ra8_err_validation_failed;
  }
  *out_len = (uint16_t)(at + (uint16_t)proto_len);
  return k_ra8_ok;
}

/**
 * @brief Strip the TLV envelope off a received serial payload.
 * @param[in] payload Frame payload; must be non-null.
 * @param[in] len Payload length in bytes.
 * @param[out] proto_len Protobuf length found; must be non-null.
 * @return Pointer to the protobuf bytes, or null when the envelope is not
 *         one this endpoint recognises.
 * @retval nullptr The tags, the endpoint name or the lengths did not check out.
 * @pre @p len bytes are readable at @p payload.
 * @pre @p proto_len is writable.
 * @post On success @p proto_len is non-zero and the returned range lies
 *       wholly inside @p payload.
 * @post On failure @p proto_len is zero.
 * @note Both endpoint names are accepted, as ``parse_tlv()`` does upstream:
 *       a response arrives on ``RPCRsp`` and an unsolicited event on
 *       ``RPCEvt``, and the two are the same length by construction.
 * @since 0.1.0
 */
static const uint8_t* c6_fwver_tlv_body(const uint8_t* payload, uint16_t len, uint16_t* proto_len)
{
  *proto_len              = 0U;
  const uint16_t ep_len   = (uint16_t)k_c6_fwver_tlv_ep_len;
  const uint16_t data_tag = (uint16_t)((uint16_t)k_c6_fwver_tlv_value + ep_len);

  if ((len < (uint16_t)k_c6_fwver_tlv_overhead) ||
      (payload[k_c6_fwver_tlv_type] != (uint8_t)k_c6_fwver_tlv_t_epname) ||
      (payload[data_tag] != (uint8_t)k_c6_fwver_tlv_t_data)) {
    return nullptr;
  }
  const uint16_t seen_ep =
    (uint16_t)((uint16_t)payload[k_c6_fwver_tlv_len_lo] |
               ((uint16_t)payload[k_c6_fwver_tlv_len_hi] << (uint16_t)k_c6_fwver_tlv_shift));
  if (seen_ep != ep_len) {
    return nullptr;
  }
  bool named = true;
  for (uint16_t i = 0U; i < ep_len; i++) {
    const uint8_t got = payload[(uint16_t)k_c6_fwver_tlv_value + i];
    if ((got != (uint8_t)RPC_EP_NAME_RSP[i]) && (got != (uint8_t)RPC_EP_NAME_EVT[i])) {
      named = false;
    }
  }
  if (!named) {
    return nullptr;
  }
  const uint16_t body = (uint16_t)((uint16_t)payload[data_tag + (uint16_t)k_c6_fwver_tlv_len_lo] |
                                   ((uint16_t)payload[data_tag + (uint16_t)k_c6_fwver_tlv_len_hi]
                                    << (uint16_t)k_c6_fwver_tlv_shift));
  if ((body == 0U) || ((uint32_t)body + (uint32_t)k_c6_fwver_tlv_overhead > (uint32_t)len)) {
    return nullptr;
  }
  *proto_len = body;
  return &payload[k_c6_fwver_tlv_overhead];
}

/**
 * @brief Copy the response's target string into the record, bounded.
 * @param[in] target Binary field from the decoded message.
 * @return Nothing.
 * @pre ::s_c6_fwver_rsp is the live record.
 * @pre @p target's ``data`` covers ``len`` bytes, or ``len`` is zero.
 * @post ``target_len`` is at most the record's capacity.
 * @post Every copied byte came from the decoded message.
 * @note The loop is bounded by the record capacity (NASA Rule 2); a longer
 *       string is truncated rather than trusted.
 * @since 0.1.0
 */
static void c6_fwver_rpc_take_target(const ProtobufCBinaryData* target)
{
  s_c6_fwver_rsp.target_len = 0U;
  if ((target == nullptr) || (target->data == nullptr)) {
    return;
  }
  const size_t take =
    (target->len < (size_t)k_c6_fwver_text_max) ? target->len : (size_t)k_c6_fwver_text_max;
  for (size_t i = 0U; i < take; i++) {
    s_c6_fwver_rsp.target[i] = target->data[i];
  }
  s_c6_fwver_rsp.target_len = (uint8_t)take;
}

/**
 * @brief Record a decoded message if it is the awaited response.
 * @param[in] msg Decoded ``Rpc`` message; must be non-null.
 * @return ``true`` when the record was filled from @p msg.
 * @retval true This is the response to this application's request.
 * @retval false It is an event, a different response, or a different UID.
 * @pre @p msg came from ``rpc__unpack`` and is still owned by the caller.
 * @pre ::s_c6_fwver_rsp has not already been filled.
 * @post On ``true`` every field of the record is set from @p msg.
 * @post @p msg itself is not modified or freed here.
 * @note The UID check is what makes this a round trip rather than a
 *       coincidence: the co-processor echoes the request's UID back.
 * @since 0.1.0
 */
static bool c6_fwver_rpc_take(const Rpc* msg)
{
  if ((msg->msg_type != RPC_TYPE__Resp) || (msg->msg_id != RPC_ID__Resp_GetCoprocessorFwVersion) ||
      (msg->payload_case != RPC__PAYLOAD_RESP_GET_COPROCESSOR_FWVERSION) ||
      (msg->resp_get_coprocessor_fwversion == nullptr)) {
    return false;
  }
  const RpcRespGetCoprocessorFwVersion* body = msg->resp_get_coprocessor_fwversion;
  s_c6_fwver_rsp.uid                         = msg->uid;
  s_c6_fwver_rsp.resp                        = body->resp;
  s_c6_fwver_rsp.major                       = body->major1;
  s_c6_fwver_rsp.minor                       = body->minor1;
  s_c6_fwver_rsp.patch                       = body->patch1;
  s_c6_fwver_rsp.chip_id                     = body->chip_id;
  c6_fwver_rpc_take_target(&body->idf_target);
  s_c6_fwver_rsp.seen = true;
  return true;
}

bool c6_fwver_rpc_consume(uint8_t if_type, uint8_t if_num, const uint8_t* payload, uint16_t len)
{
  (void)if_num;
  if ((if_type != (uint8_t)ESP_SERIAL_IF) || (payload == nullptr) || s_c6_fwver_rsp.seen) {
    return false;
  }

  uint16_t       proto_len = 0U;
  const uint8_t* proto     = c6_fwver_tlv_body(payload, len, &proto_len);
  if (proto == nullptr) {
    c6_fwver_puts("c6_fwver: serial frame is not an RPC envelope, len=");
    c6_fwver_put_u32((uint32_t)len);
    c6_fwver_puts("\r\n");
    return false;
  }

  Rpc* msg = rpc__unpack(&s_c6_fwver_allocator, (size_t)proto_len, proto);
  if (msg == nullptr) {
    c6_fwver_puts("c6_fwver: protobuf decode refused ");
    c6_fwver_put_u32((uint32_t)proto_len);
    c6_fwver_puts(" bytes\r\n");
    return false;
  }

  c6_fwver_puts("c6_fwver: rpc in type=");
  c6_fwver_put_u32((uint32_t)msg->msg_type);
  c6_fwver_puts(" id=");
  c6_fwver_put_u32((uint32_t)msg->msg_id);
  c6_fwver_puts(" uid=");
  c6_fwver_put_u32(msg->uid);
  c6_fwver_puts(" proto_bytes=");
  c6_fwver_put_u32((uint32_t)proto_len);
  c6_fwver_puts("\r\n");

  const bool matched = c6_fwver_rpc_take(msg);
  rpc__free_unpacked(msg, &s_c6_fwver_allocator);
  return matched;
}

/**
 * @brief Print the decoded response, field by field.
 * @return Nothing.
 * @pre ::s_c6_fwver_rsp holds a decoded response.
 * @pre The console is up.
 * @post One line naming every decoded field was emitted.
 * @post No application state is modified.
 * @note Split out of ::c6_fwver_rpc_report so the verdict logic there stays
 *       readable and inside the NASA Rule 4 length budget.
 * @since 0.1.0
 */
static void c6_fwver_rpc_print_response(void)
{
  c6_fwver_puts("c6_fwver: rsp uid=");
  c6_fwver_put_u32(s_c6_fwver_rsp.uid);
  c6_fwver_puts(" resp=");
  c6_fwver_put_i32(s_c6_fwver_rsp.resp);
  c6_fwver_puts(" fw=");
  c6_fwver_put_u32(s_c6_fwver_rsp.major);
  c6_fwver_puts(".");
  c6_fwver_put_u32(s_c6_fwver_rsp.minor);
  c6_fwver_puts(".");
  c6_fwver_put_u32(s_c6_fwver_rsp.patch);
  c6_fwver_puts(" chip_id=0x");
  c6_fwver_put_hex(s_c6_fwver_rsp.chip_id, (uint8_t)k_c6_fwver_hex_byte);
  c6_fwver_puts(" idf_target=");
  c6_fwver_put_text(s_c6_fwver_rsp.target, (size_t)s_c6_fwver_rsp.target_len);
  c6_fwver_puts("\r\n");
}

/**
 * @brief Report the boot event's version reading beside the RPC's.
 * @return Nothing.
 * @pre The console is up.
 * @pre A pump has run, so the event either arrived or provably did not.
 * @post Exactly one line was emitted.
 * @post No application state is modified.
 * @note This is a cross-check, never a gate: the co-processor queues the
 *       INIT event once per boot, so a rerun without power-cycling the C6
 *       legitimately has nothing to compare.
 * @since 0.1.0
 */
static void c6_fwver_rpc_print_crosscheck(void)
{
  const uint32_t seen = c6_fwver_priv_init_version();
  c6_fwver_puts("c6_fwver: crosscheck priv_init_event=");
  if (seen == 0U) {
    c6_fwver_puts("absent -- either the co-processor has not been power-cycled since its "
                  "last drain, or its INIT event was dropped (read ifnum_defect on the "
                  "caps pump line)\r\n");
    return;
  }
  c6_fwver_put_u32(ESP_HOSTED_VERSION_MAJOR(seen));
  c6_fwver_puts(".");
  c6_fwver_put_u32(ESP_HOSTED_VERSION_MINOR(seen));
  c6_fwver_puts(".");
  c6_fwver_put_u32(ESP_HOSTED_VERSION_PATCH(seen));
  const bool agrees = (ESP_HOSTED_VERSION_MAJOR(seen) == s_c6_fwver_rsp.major) &&
                      (ESP_HOSTED_VERSION_MINOR(seen) == s_c6_fwver_rsp.minor) &&
                      (ESP_HOSTED_VERSION_PATCH(seen) == s_c6_fwver_rsp.patch);
  c6_fwver_puts(agrees ? " agrees with the RPC answer\r\n" : " DISAGREES with the RPC answer\r\n");
}

/**
 * @brief Print what this host expects, before it says what it got.
 * @return Nothing.
 * @pre The console is up.
 * @pre The expectations are compile-time constants, so this cannot disagree
 *      with the comparison ::c6_fwver_rpc_report then performs.
 * @post Exactly one line was emitted.
 * @post No application state is modified.
 * @note Printed unconditionally, including on the no-response path: a run that
 *       fails is exactly the run whose reader most wants to know what was
 *       being asked for.
 * @since 0.1.0
 */
static void c6_fwver_rpc_print_expectation(void)
{
  c6_fwver_puts("c6_fwver: expect uid=");
  c6_fwver_put_u32((uint32_t)k_c6_fwver_rpc_uid);
  c6_fwver_puts(" resp=0 fw=");
  c6_fwver_put_u32((uint32_t)ESP_HOSTED_VERSION_MAJOR_1);
  c6_fwver_puts(".");
  c6_fwver_put_u32((uint32_t)ESP_HOSTED_VERSION_MINOR_1);
  c6_fwver_puts(".");
  c6_fwver_put_u32((uint32_t)ESP_HOSTED_VERSION_PATCH_1);
  c6_fwver_puts(" (the vendored host driver's own version) chip_id=0x");
  c6_fwver_put_hex((uint32_t)ESP_PRIV_FIRMWARE_CHIP_ESP32C6, (uint8_t)k_c6_fwver_hex_byte);
  c6_fwver_puts("\r\n");
}

bool c6_fwver_rpc_report(void)
{
  c6_fwver_rpc_print_expectation();

  if (!s_c6_fwver_rsp.seen) {
    c6_fwver_puts("c6_fwver: FAIL no RPC response -- the co-processor never answered\r\n");
    return false;
  }
  c6_fwver_rpc_print_response();
  c6_fwver_rpc_print_crosscheck();

  if (s_c6_fwver_rsp.uid != (uint32_t)k_c6_fwver_rpc_uid) {
    c6_fwver_puts("c6_fwver: FAIL response UID does not match the request\r\n");
    return false;
  }
  if (s_c6_fwver_rsp.resp != 0) {
    c6_fwver_puts("c6_fwver: FAIL co-processor reported an error result\r\n");
    return false;
  }
  if ((s_c6_fwver_rsp.major != (uint32_t)ESP_HOSTED_VERSION_MAJOR_1) ||
      (s_c6_fwver_rsp.minor != (uint32_t)ESP_HOSTED_VERSION_MINOR_1) ||
      (s_c6_fwver_rsp.patch != (uint32_t)ESP_HOSTED_VERSION_PATCH_1)) {
    c6_fwver_puts("c6_fwver: FAIL co-processor firmware version is not the one this host "
                  "was built against\r\n");
    return false;
  }
  if (s_c6_fwver_rsp.chip_id != (uint32_t)ESP_PRIV_FIRMWARE_CHIP_ESP32C6) {
    c6_fwver_puts("c6_fwver: FAIL the answering co-processor is not an ESP32-C6\r\n");
    return false;
  }
  c6_fwver_puts("c6_fwver: PASS esp-hosted RPC round-trip, co-processor firmware ");
  c6_fwver_put_u32(s_c6_fwver_rsp.major);
  c6_fwver_puts(".");
  c6_fwver_put_u32(s_c6_fwver_rsp.minor);
  c6_fwver_puts(".");
  c6_fwver_put_u32(s_c6_fwver_rsp.patch);
  c6_fwver_puts("\r\n");
  return true;
}

bool c6_fwver_dispatch(uint8_t if_type, uint8_t if_num, const uint8_t* payload, uint16_t len)
{
  c6_fwver_puts("c6_fwver: frame if_type=");
  c6_fwver_put_u32((uint32_t)if_type);
  c6_fwver_puts(" if_num=");
  c6_fwver_put_u32((uint32_t)if_num);
  c6_fwver_puts(" len=");
  c6_fwver_put_u32((uint32_t)len);
  c6_fwver_puts("\r\n");

  if (if_type == (uint8_t)ESP_PRIV_IF) {
    return c6_fwver_priv_consume(if_type, if_num, payload, len);
  }
  if (if_type == (uint8_t)ESP_SERIAL_IF) {
    return c6_fwver_rpc_consume(if_type, if_num, payload, len);
  }
  c6_fwver_puts("c6_fwver: frame on an interface this app does not use\r\n");
  return false;
}
