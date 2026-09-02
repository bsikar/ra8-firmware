/**
 * @file test_ra8_c6link_transport.c
 * @brief Transport, framing, RPC-guard, and remote-error facade tests.
 *
 * @details
 * These tests drive complete data-plane and failure-path transactions through
 * the bounded co-processor model. Session and Wi-Fi behavior live in
 * `test_ra8_c6link_session.c`.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <string.h>

#include "idf_compat/esp_err.h"
#include "ra8_attributes.h"
#include "ra8_c6_model.h"
#include "ra8_c6link.h"
#include "ra8_c6link_internal.h"
#include "ra8_c6link_model_test_internal.h"
#include "ra8_c6link_test_suites.h"
#include "ra8_err.h"
#include "unity_minimal.h"

/** @enum t_c6_transport_const_t @brief Constants owned by the transport suite. */
typedef enum : uint32_t {
  k_t_stray_len   = 16U,   /**< Length of a payload that is not an envelope.   */
  k_t_junk_len    = 3U,    /**< Length of an envelope body that is not an RPC. */
  k_t_hdr_off_lo  = 4U,    /**< Offset of the payload header's offset field.   */
  k_t_stray_first = 0xF0U, /**< First octet of a non-envelope payload.         */
  k_t_junk_octet  = 0xFFU, /**< Filler for a body that is not an RPC.          */
} t_c6_transport_const_t;

/**
 * @brief A binary field too large for one frame, for the refusal path.
 * @details File scope because the request that carries it must outlive the
 * assertion, and because a frame-sized array is not a stack object this tree
 * wants inside a test function.
 * @note Never transmitted: the request carrying it is refused before staging.
 * @warning Its size must stay above ::k_ra8_c6link_max_payload.
 * @since 0.1.0
 */
static uint8_t s_oversize[k_ra8_c6link_frame_bytes];

/**
 * @par MC/DC:
 * Decision: `(len == 0) || (len > k_ra8_c6link_max_payload)` (2 conditions)
 * - Vector 1: len 64                 -> false (control: the frame is clocked)
 * - Vector 2: len 0                  -> true  (varies the zero test only)
 * - Vector 3: len max_payload + 1    -> true  (varies the maximum test only)
 * Vectors 1+2 and 1+3 prove each condition independently decides.
 * N+1 = 3 vectors for N=2: minimal MC/DC.
 * Decisions: libs/ra8_c6link/src/ra8_c6link.c@ra8_c6link_eth_send
 * Decisions: libs/ra8_c6link/src/ra8_c6link.c@priv_c6link_dispatch @brief Verify eth data plane behavior. @details Executes the eth data plane scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_eth_data_plane(void)
{
  TEST_BEGIN("c6link Ethernet data plane");
  priv_c6link_test_bringup();
  uint8_t frame[(size_t)k_c6m_eth_len];
  for (uint16_t i = 0U; i < (uint16_t)k_c6m_eth_len; i++) {
    frame[i] = (uint8_t)(0x20U + i);
  }
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_c6link_eth_send(priv_c6link_test_link(), frame, (uint16_t)k_c6m_eth_len));
  TEST_ASSERT_EQ(k_c6m_eth_len, ra8_c6_model()->eth_tx_len);
  TEST_ASSERT_EQ(0, memcmp(ra8_c6_model()->eth_tx, frame, sizeof frame));

  TEST_ASSERT_EQ(k_ra8_err_invalid_size, ra8_c6link_eth_send(priv_c6link_test_link(), frame, 0U));
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 ra8_c6link_eth_send(priv_c6link_test_link(),
                                     frame,
                                     (uint16_t)((uint16_t)k_ra8_c6link_max_payload + 1U)));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_c6link_eth_send(priv_c6link_test_link(), nullptr, 1U));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_c6link_eth_send(nullptr, frame, 1U));

  /* Inbound: a station frame reaches the receive callback. */
  priv_c6link_test_bringup();
  ra8_c6_model_emit_eth();
  ra8_c6link_stats_t stats = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_c6link_poll(priv_c6link_test_link(), 2U, &stats));
  TEST_ASSERT_EQ(k_c6m_eth_len, priv_c6link_test_rx_len());
  TEST_ASSERT_EQ(1, stats.eth_in);

  /* A frame on an interface this link does not use is counted, not delivered.
     `ESP_PRIV_IF` is the real case: upstream reads peripheral-side capabilities from it,
     but this co-processor build's only privileged frame fails its own
     checksum (#529), so nothing here may depend on one. */
  priv_c6link_test_bringup();
  uint8_t* slot = ra8_c6_model_slot();
  TEST_ASSERT_NOT_NULL(slot);
  priv_c6link_frame_seal(slot, (uint8_t)ESP_PRIV_IF, 0U, 4U);
  stats = (ra8_c6link_stats_t){};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_c6link_poll(priv_c6link_test_link(), 2U, &stats));
  TEST_ASSERT_EQ(1, stats.unrouted);
  TEST_ASSERT_EQ(0, priv_c6link_test_rx_len());

  TEST_ASSERT_EQ(k_ra8_ok, ra8_c6link_close(priv_c6link_test_link()));
  TEST_ASSERT_EQ(k_ra8_err_not_initialized,
                 ra8_c6link_eth_send(priv_c6link_test_link(), frame, (uint16_t)k_c6m_eth_len));
  TEST_END("c6link Ethernet data plane");
}

/**
 * @par MC/DC:
 * (no compound decision under test -- an inactive handshake and a refusing
 * transport are each driven to their own return code)
 * Decisions: libs/ra8_c6link/src/ra8_c6link_pump.c@priv_c6link_pump @brief Verify transport faults behavior. @details Executes the transport faults scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_transport_faults(void)
{
  TEST_BEGIN("c6link transport faults");
  priv_c6link_test_reset();
  ra8_c6_model()->handshake = false;
  ra8_c6link_stats_t stats  = {};
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout, ra8_c6link_poll(priv_c6link_test_link(), 8U, &stats));
  TEST_ASSERT_EQ(0, stats.transfers);
  /* It gives up after three consecutive misses rather than spending the whole
     budget: an absent co-processor must produce a verdict, not a hang. */
  TEST_ASSERT_EQ(k_ra8_c6link_hs_giveup, stats.hs_timeouts);

  priv_c6link_test_reset();
  ra8_c6_model()->fail_transfer = true;
  stats                         = (ra8_c6link_stats_t){};
  TEST_ASSERT_EQ(k_ra8_err_spi_error, ra8_c6link_poll(priv_c6link_test_link(), 8U, &stats));

  priv_c6link_test_reset();
  ra8_c6_model()->fail_transfer = true;
  ra8_c6link_fw_version_t fw    = {};
  TEST_ASSERT_EQ(k_ra8_err_spi_error, ra8_c6link_fw_version(priv_c6link_test_link(), &fw));

  priv_c6link_test_reset();
  ra8_c6_model()->fail_transfer        = true;
  uint8_t frame[(size_t)k_c6m_eth_len] = {};
  TEST_ASSERT_EQ(k_ra8_err_spi_error,
                 ra8_c6link_eth_send(priv_c6link_test_link(), frame, (uint16_t)k_c6m_eth_len));

  priv_c6link_test_reset();
  ra8_c6_model()->handshake = false;
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout,
                 ra8_c6link_eth_send(priv_c6link_test_link(), frame, (uint16_t)k_c6m_eth_len));

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_c6link_poll(nullptr, 1U, nullptr));
  priv_c6link_test_reset();
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_c6link_poll(priv_c6link_test_link(), 0U, nullptr));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_c6link_close(priv_c6link_test_link()));
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_c6link_poll(priv_c6link_test_link(), 1U, nullptr));
  TEST_END("c6link transport faults");
}

/**
 * @par MC/DC:
 * (no compound decision under test -- a payload that is not an envelope and an
 * envelope whose body is not a message are each counted, not acted on) @brief Verify undecodable behavior. @details Executes the undecodable scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_undecodable(void)
{
  TEST_BEGIN("c6link undecodable control frames");
  priv_c6link_test_reset();
  uint8_t* slot = ra8_c6_model_slot();
  TEST_ASSERT_NOT_NULL(slot);
  for (uint8_t i = 0U; i < (uint8_t)k_t_stray_len; i++) {
    slot[(uint8_t)k_ra8_c6link_header_bytes + i] = (uint8_t)((uint16_t)k_t_stray_first + i);
  }
  priv_c6link_frame_seal(slot, (uint8_t)ESP_SERIAL_IF, 0U, (uint16_t)k_t_stray_len);

  slot = ra8_c6_model_slot();
  TEST_ASSERT_NOT_NULL(slot);
  uint8_t* payload = &slot[k_ra8_c6link_header_bytes];
  uint16_t body_at = 0U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 priv_c6link_tlv_open(payload,
                                      (uint16_t)k_ra8_c6link_max_payload,
                                      (uint16_t)k_t_junk_len,
                                      &body_at));
  for (uint8_t i = 0U; i < (uint8_t)k_t_junk_len; i++) {
    payload[body_at + i] = (uint8_t)k_t_junk_octet;
  }
  priv_c6link_frame_seal(slot,
                         (uint8_t)ESP_SERIAL_IF,
                         0U,
                         (uint16_t)(body_at + (uint16_t)k_t_junk_len));

  ra8_c6link_stats_t stats = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_c6link_poll(priv_c6link_test_link(), 4U, &stats));
  TEST_ASSERT_EQ(2, stats.undecodable);
  TEST_ASSERT_EQ(0, priv_c6link_test_event_count());
  TEST_END("c6link undecodable control frames");
}

/**
 * @par MC/DC:
 * (no compound decision under test -- each malformed shape is counted in its
 * own bucket, and an announcement the facade does not model is dropped) @brief Verify rejected frames behavior. @details Executes the rejected frames scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_rejected_frames(void)
{
  TEST_BEGIN("c6link rejected and unmodelled frames");
  priv_c6link_test_reset();

  /* A header whose offset is not the payload-header size. */
  uint8_t* slot = ra8_c6_model_slot();
  TEST_ASSERT_NOT_NULL(slot);
  priv_c6link_frame_seal(slot, (uint8_t)ESP_SERIAL_IF, 0U, (uint16_t)k_t_junk_len);
  slot[k_t_hdr_off_lo] = 0U;

  /* A frame whose payload no longer matches its checksum. */
  slot = ra8_c6_model_slot();
  TEST_ASSERT_NOT_NULL(slot);
  priv_c6link_frame_seal(slot, (uint8_t)ESP_SERIAL_IF, 0U, (uint16_t)k_t_junk_len);
  slot[k_ra8_c6link_header_bytes]++;

  ra8_c6_model_emit_unmodelled_event();
  ra8_c6_model_emit_inbound_request();

  ra8_c6link_stats_t stats = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_c6link_poll(priv_c6link_test_link(), 8U, &stats));
  TEST_ASSERT_EQ(1, stats.malformed);
  TEST_ASSERT_EQ(1, stats.bad_checksum);
  TEST_ASSERT_EQ(2, stats.rpc_in);
  /* The inbound request decoded fine and was still refused: "the codec would
     not parse it" and "a host has no handler for it" are different facts, and
     both land in `undecodable` only because neither is ever actionable. */
  TEST_ASSERT_EQ(1, stats.undecodable);
  TEST_ASSERT_EQ(0, priv_c6link_test_event_count());

  /* An announcement whose optional inner message the co-processor omitted:
     protobuf permits it, so the decoder must report the kind and leave every
     other field zero rather than dereference an absent body. */
  priv_c6link_test_reset();
  ra8_c6_model_emit_hollow_events();
  stats = (ra8_c6link_stats_t){};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_c6link_poll(priv_c6link_test_link(), 4U, &stats));
  TEST_ASSERT_EQ(2, priv_c6link_test_event_count());
  TEST_ASSERT_EQ(k_ra8_c6link_event_sta_connected, priv_c6link_test_event(0U)->kind);
  TEST_ASSERT_EQ(0, priv_c6link_test_event(0U)->ssid_len);
  TEST_ASSERT_EQ(k_ra8_c6link_event_sta_disconnected, priv_c6link_test_event(1U)->kind);
  TEST_ASSERT_EQ(0, priv_c6link_test_event(1U)->reason);
  TEST_END("c6link rejected and unmodelled frames");
}

/**
 * @par MC/DC:
 * Decision: `(link == nullptr) || (req == nullptr) || (take == nullptr)`
 * (3 conditions, inside ::priv_c6link_rpc_call)
 * - Vector 1: all three supplied -> false (control: the request is issued)
 * - Vector 2: link NULL          -> true  (varies link only)
 * - Vector 3: req NULL           -> true  (varies req only)
 * - Vector 4: take NULL          -> true  (varies take only)
 * Vector 1 paired with each of 2, 3 and 4 proves the corresponding condition
 * independently decides. N+1 = 4 vectors for N=3: minimal MC/DC.
 * Decisions: libs/ra8_c6link/src/ra8_c6link_rpc.c@priv_c6link_rpc_call @brief Verify rpc call guards behavior. @details Executes the rpc call guards scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_rpc_call_guards(void)
{
  TEST_BEGIN("c6link request guards");
  priv_c6link_test_bringup();

  Rpc                   req;
  ra8_c6link_take_ctx_t take = {.link = priv_c6link_test_link(), .out = nullptr, .rpc_id = 0U};
  rpc__init(&req);
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 priv_c6link_rpc_call(nullptr, &req, 0U, priv_c6link_take_resp, &take));
  TEST_ASSERT_EQ(
    k_ra8_err_null_ptr,
    priv_c6link_rpc_call(priv_c6link_test_link(), nullptr, 0U, priv_c6link_take_resp, &take));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 priv_c6link_rpc_call(priv_c6link_test_link(), &req, 0U, nullptr, &take));

  /* A payload already staged means the transmit slot is taken, and this link
     carries exactly one. The second request is refused rather than silently
     serialised behind the first. */
  priv_c6link_test_link()->tx_len = 1U;
  TEST_ASSERT_EQ(
    k_ra8_err_busy,
    priv_c6link_rpc_call(priv_c6link_test_link(), &req, 0U, priv_c6link_take_resp, &take));
  priv_c6link_test_link()->tx_len = 0U;

  TEST_ASSERT_EQ(k_ra8_ok, ra8_c6link_close(priv_c6link_test_link()));
  TEST_ASSERT_EQ(
    k_ra8_err_not_initialized,
    priv_c6link_rpc_call(priv_c6link_test_link(), &req, 0U, priv_c6link_take_resp, &take));

  ra8_c6link_stats_t stats = {};
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, priv_c6link_pump(nullptr, 1U, &stats));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, priv_c6link_pump(priv_c6link_test_link(), 1U, nullptr));

  /* A request that cannot fit one frame is refused before anything is staged,
     rather than truncated onto the wire where the co-processor would have to
     reject it. Nothing this library sends is near the limit, so the case is
     constructed here. */
  priv_c6link_test_bringup();
  RpcEventESPInit huge;
  rpc__event__espinit__init(&huge);
  huge.init_data.data = s_oversize;
  huge.init_data.len  = sizeof s_oversize;

  Rpc big;
  rpc__init(&big);
  big.msg_type       = RPC_TYPE__Req;
  big.msg_id         = RPC_ID__Req_GetCoprocessorFwVersion;
  big.payload_case   = RPC__PAYLOAD_EVENT_ESP_INIT;
  big.event_esp_init = &huge;
  (void)memset(priv_c6link_test_link()->tx,
               (int)k_t_junk_octet,
               sizeof(priv_c6link_test_link()->tx));
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_size,
    priv_c6link_rpc_call(priv_c6link_test_link(), &big, 0U, priv_c6link_take_resp, &take));
  for (size_t index = 0U; index < sizeof(priv_c6link_test_link()->tx); ++index) {
    TEST_ASSERT_EQ(0, priv_c6link_test_link()->tx[index]);
  }
  TEST_END("c6link request guards");
}

/**
 * @par MC/DC:
 * Decision: `(link == nullptr) || (ev == nullptr)` (2 conditions,
 * ::priv_c6link_emit); `(link == nullptr) || (view == nullptr)`
 * (::priv_c6link_dispatch); `(link == nullptr) || (payload == nullptr)`
 * (::priv_c6link_rpc_consume); `(link == nullptr) || (out == nullptr)`
 * (::ra8_c6link_last_fault). Each is driven with the same three vectors:
 * - Vector 1: both supplied  -> false (control)
 * - Vector 2: first NULL     -> true  (varies the first condition only)
 * - Vector 3: second NULL    -> true  (varies the second condition only)
 * Decision: `(if_type == ESP_STA_IF) || (if_type == ESP_AP_IF)` (2 conditions,
 * ::priv_c6link_dispatch)
 * - Vector 4: a station frame       -> true  (varies the station test)
 * - Vector 5: an access-point frame -> true  (varies the AP test)
 * - Vector 6: a privileged frame    -> false (control: neither)
 * Decision: `armed || (tx_len != 0U)` (2 conditions, ::priv_c6link_rpc_call)
 * - Vector 7: neither         -> false (control: the request is issued)
 * - Vector 8: armed only      -> true  (varies armed only)
 * - Vector 9: staged only     -> true  (varies tx_len only)
 * Each control paired with each varied vector proves that condition
 * independently decides. N+1 vectors per decision: minimal MC/DC.
 * Decisions: libs/ra8_c6link/src/ra8_c6link.c@priv_c6link_emit
 * Decisions: libs/ra8_c6link/src/ra8_c6link.c@priv_c6link_dispatch
 * Decisions: libs/ra8_c6link/src/ra8_c6link.c@ra8_c6link_last_fault
 * Decisions: libs/ra8_c6link/src/ra8_c6link_rpc.c@priv_c6link_rpc_consume
 * Decisions: libs/ra8_c6link/src/ra8_c6link_rpc.c@priv_c6link_resp @brief Verify mcdc facade guards behavior. @details Executes the mcdc facade guards scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_mcdc_facade_guards(void)
{
  TEST_BEGIN("c6link facade guard vectors");
  priv_c6link_test_reset();

  /* Every private entry point tolerates either argument being absent. These
     are reachable from no public call, so they are driven directly. */
  const ra8_c6link_event_t ev         = {.kind = k_ra8_c6link_event_boot};
  ra8_c6link_rx_view_t     view       = {.offset  = (uint16_t)k_ra8_c6link_header_bytes,
                                         .len     = 1U,
                                         .if_type = (uint8_t)ESP_STA_IF};
  uint8_t                  payload[4] = {};

  priv_c6link_emit(priv_c6link_test_link(), &ev);
  TEST_ASSERT_EQ(1, priv_c6link_test_event_count());
  priv_c6link_emit(nullptr, &ev);
  priv_c6link_emit(priv_c6link_test_link(), nullptr);
  TEST_ASSERT_EQ(1, priv_c6link_test_event_count());

  TEST_ASSERT(!priv_c6link_dispatch(priv_c6link_test_link(), &view));
  TEST_ASSERT(!priv_c6link_dispatch(nullptr, &view));
  TEST_ASSERT(!priv_c6link_dispatch(priv_c6link_test_link(), nullptr));

  TEST_ASSERT(!priv_c6link_rpc_consume(priv_c6link_test_link(), payload, (uint16_t)sizeof payload));
  TEST_ASSERT(!priv_c6link_rpc_consume(nullptr, payload, (uint16_t)sizeof payload));
  TEST_ASSERT(!priv_c6link_rpc_consume(priv_c6link_test_link(), nullptr, (uint16_t)sizeof payload));

  ra8_c6link_fault_t fault = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_c6link_last_fault(priv_c6link_test_link(), &fault));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_c6link_last_fault(nullptr, &fault));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_c6link_last_fault(priv_c6link_test_link(), nullptr));

  /* An access-point frame routes to the same receive callback a station frame
     does: the facade carries the data plane for both interfaces. */
  priv_c6link_test_reset();
  uint8_t* slot = ra8_c6_model_slot();
  TEST_ASSERT_NOT_NULL(slot);
  priv_c6link_frame_seal(slot, (uint8_t)ESP_AP_IF, 0U, (uint16_t)k_c6m_eth_len);
  ra8_c6link_stats_t stats = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_c6link_poll(priv_c6link_test_link(), 2U, &stats));
  TEST_ASSERT_EQ(1, stats.eth_in);
  TEST_ASSERT_EQ(k_c6m_eth_len, priv_c6link_test_rx_len());

  /* The busy test is two conditions: a request already outstanding, or a
     payload already staged. Either alone refuses. */
  priv_c6link_test_reset();
  Rpc                   req;
  ra8_c6link_take_ctx_t take = {.link = priv_c6link_test_link(), .out = nullptr, .rpc_id = 0U};
  rpc__init(&req);
  priv_c6link_test_link()->wait.armed = true;
  priv_c6link_test_link()->tx_len     = 0U;
  TEST_ASSERT_EQ(
    k_ra8_err_busy,
    priv_c6link_rpc_call(priv_c6link_test_link(), &req, 0U, priv_c6link_take_resp, &take));
  priv_c6link_test_link()->wait.armed = false;
  TEST_END("c6link facade guard vectors");
}

/**
 * @test internal_test_remote_error_mapping
 * @brief Preserve actionable C6 general errors without losing raw fault evidence.
 * @details Drives every portable mapping plus generic and success controls
 * directly through the shared response-status seam.
 * @pre The facade fixture exposes one reset caller-owned link.
 * @pre ESP-IDF compatibility constants match the C6 wire ABI.
 * @post Every known status returns its closest RA8 error.
 * @post The last fault retains the exact final nonzero remote status.
 * @note Component-specific unknown values deliberately remain protocol errors.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_remote_error_mapping(void)
{
  TEST_BEGIN("c6link remote error mapping");
  priv_c6link_test_reset();
  ra8_c6link_t* link = priv_c6link_test_link();
  TEST_ASSERT_EQ(k_ra8_err_no_mem, priv_c6link_resp(link, 1U, ESP_ERR_NO_MEM));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, priv_c6link_resp(link, 2U, ESP_ERR_INVALID_ARG));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, priv_c6link_resp(link, 3U, ESP_ERR_INVALID_STATE));
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, priv_c6link_resp(link, 4U, ESP_ERR_INVALID_SIZE));
  TEST_ASSERT_EQ(k_ra8_err_not_found, priv_c6link_resp(link, 5U, ESP_ERR_NOT_FOUND));
  TEST_ASSERT_EQ(k_ra8_err_not_supported, priv_c6link_resp(link, 6U, ESP_ERR_NOT_SUPPORTED));
  TEST_ASSERT_EQ(k_ra8_err_timeout, priv_c6link_resp(link, 7U, ESP_ERR_TIMEOUT));
  TEST_ASSERT_EQ(k_ra8_err_protocol_error, priv_c6link_resp(link, 8U, ESP_ERR_INVALID_RESPONSE));
  TEST_ASSERT_EQ(k_ra8_err_checksum_mismatch, priv_c6link_resp(link, 9U, ESP_ERR_INVALID_CRC));
  TEST_ASSERT_EQ(k_ra8_err_access_denied, priv_c6link_resp(link, 10U, ESP_ERR_NOT_ALLOWED));
  TEST_ASSERT_EQ(k_ra8_err_protocol_error, priv_c6link_resp(link, 11U, ESP_FAIL));
  ra8_c6link_fault_t fault = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_c6link_last_fault(link, &fault));
  TEST_ASSERT_EQ(11U, fault.rpc_id);
  TEST_ASSERT_EQ(ESP_FAIL, fault.resp);
  TEST_ASSERT_EQ(k_ra8_ok, priv_c6link_resp(link, 12U, ESP_OK));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_c6link_last_fault(link, &fault));
  TEST_ASSERT_EQ(0U, fault.rpc_id);
  TEST_ASSERT_EQ(ESP_OK, fault.resp);
  TEST_END("c6link remote error mapping");
}

/**
 * @brief Run the transport, framing, and RPC failure scenarios.
 * @return Nothing.
 * @pre The bounded C6 model fixture is linked into this test executable.
 * @post Every transport-facing facade scenario has completed its assertions.
 * @since 0.1.0
 */
void ra8_test_c6link_transport(void)
{
  internal_test_eth_data_plane();
  internal_test_transport_faults();
  internal_test_undecodable();
  internal_test_rejected_frames();
  internal_test_rpc_call_guards();
  internal_test_mcdc_facade_guards();
  internal_test_remote_error_mapping();
}
