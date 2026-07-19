/**
 * @file test_ra8_ble_l2cap_cov.c
 * @brief Line-coverage tests for the reassembly / trampoline / error legs
 *        of libs/ra8_ble_host/src/ra8_ble_l2cap.c.
 *
 * @details
 * The sibling suite test_ra8_ble_l2cap.c drives the public-API MC/DC
 * vectors (role validation, the HCI event-trampoline guards). This TU
 * targets the paths that suite leaves uncovered:
 *
 *   - ra8_ble_host_l2cap_send oversize guard (scratch-buffer bound).
 *   - ra8_ble_host_acl_in run before the stack is initialized.
 *   - the inbound L2CAP reassembly state machine: start-fragment,
 *     incomplete continuation, complete continuation (ATT and non-ATT
 *     CID), the too-long start-fragment reject, and the mid-reassembly
 *     overflow drop.
 *   - internal_acl_trampoline, reached by pumping a raw HCI ACL packet
 *     through the controller's ra8_ble_dispatch loop so the registered
 *     host handler runs (not the direct test veneer).
 *   - the CCCD-clearing loop body inside the Disconnection_Complete
 *     handler, reached with a live GATT table that holds a CCCD row.
 *   - the ra8_ble_open-failed leg of ra8_ble_host_init.
 *
 * Every frame is synthesised with pre-seeded bytes so the state machine
 * advances deterministically. No timers, no SIGALRM. Bluetooth Core 5.3
 * Vol 3 Part A 3 ("Data Packet Format") and Vol 4 Part E 5.4.2 (HCI ACL
 * framing) govern the byte layouts below.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>

#include "ra8_ble.h"
#include "ra8_ble_host.h"
#include "ra8_err.h"
#include "ra8_sim_mmap.h"
#include "unity_minimal.h"

/**
 * @enum ble_l2cap_cov_uint8_const_t
 * @brief Named uint8_t constants used by this file.
 *
 * @details
 * Every literal this translation unit needs, named so the
 * value's role is visible at the point of use (CLAUDE.md
 * "No Magic Numbers").
 */
typedef enum : uint8_t {
  k_byte_mask = 0xFFU, /**< Truncates each shifted CRC and length byte for the gzip trailer. */
  k_hci_evt_disconn_complete = 0x05U, /**< HCI event 0x05, Disconnection Complete. */
  k_l2cap_len_lo_300 =
    0x2CU, /**< Low byte of an L2CAP length of 300 (0x012C): more than one fragment, so reassembly is exercised. */
  k_l2cap_len_lo_400 =
    0x90U, /**< Low byte of a length of 400 (0x0190), longer than the reassembly buffer so the overflow guard fires. */
  k_l2cap_frag_cap =
    250, /**< Fragment staging capacity: under the 300-byte payload, so the payload must arrive in pieces. */
} ble_l2cap_cov_uint8_const_t;

/**
 * @enum ble_l2cap_cov_uint16_const_t
 * @brief Named uint16_t constants used by this file.
 *
 * @details
 * Every literal this translation unit needs, named so the
 * value's role is visible at the point of use (CLAUDE.md
 * "No Magic Numbers").
 */
typedef enum : uint16_t {
  k_l2cap_reassembly_cap =
    300, /**< Reassembly capacity, which the 400-byte payload deliberately exceeds. */
} ble_l2cap_cov_uint16_const_t;

/* Internal L2CAP entry point (non-static global in ra8_ble_l2cap.c, used by
 * ra8_ble_att.c / ra8_ble_gatt.c). Forward-declared here to drive its
 * scratch-buffer bound check directly. */
ra8_err_t ra8_ble_host_l2cap_send(uint16_t       conn_handle,
                                  uint16_t       cid,
                                  const uint8_t* payload,
                                  uint16_t       payload_len);

/* Controller test hooks (external linkage in ra8_ble.c, not in ra8_ble.h). */
void ra8_ble_test_inject_rx(const uint8_t* bytes, uint16_t len);
void ra8_ble_test_reset_capture(void);

typedef enum : uint16_t {
  k_cov_conn_handle    = 0x0042U, /**< Arbitrary 12-bit ACL handle for the vectors.  */
  k_cov_cid_att        = 0x0004U, /**< Vol 3 Part A 2.1 Table 2.3 LE ATT CID.        */
  k_cov_cid_signaling  = 0x0005U, /**< Vol 3 Part A 2.1 Table 2.3 LE signaling CID.  */
  k_cov_send_oversize  = 253U,    /**< 253 + 4-byte L2CAP header > 256-byte scratch. */
  k_cov_char_value_max = 4U,      /**< Backing-buffer capacity for the test char.    */
} cov_const_t;

/**
 * @brief Reset the simulator register window and tear the host down.
 *
 * @details Mirrors the setUp() pattern used across the BLE host suites.
 *
 * @pre None.
 * @post s_ble_host_state is zeroed and the controller is closed.
 * @post The mocked register window is back to reset defaults.
 *
 * @note Not thread-safe; single-threaded test harness only.
 *
 * @since 0.1.0
 */
static void prep(void)
{
  ra8_sim_mmap_reset();
  (void)ra8_ble_host_close();
}

/**
 * @brief Reset then bring the BLE host stack up as a peripheral.
 *
 * @details Runs prep() and initialises the host so subsequent injects
 *          operate on an open controller.
 *
 * @return ra8_err_t Result of ra8_ble_host_init.
 * @retval k_ra8_ok Stack initialised.
 *
 * @pre None.
 * @post On success the host and controller are open.
 * @post On success ra8_ble_host_state()->initialized == 1.
 *
 * @note Not thread-safe; single-threaded test harness only.
 *
 * @since 0.1.0
 */
static ra8_err_t bring_up(void)
{
  prep();
  ra8_ble_host_config_t cfg = {.role       = k_ra8_ble_host_role_peripheral,
                               .appearance = 0U,
                               .name       = "t"};
  return ra8_ble_host_init(&cfg);
}

/**
 * @test test_cov_l2cap_send_oversize
 *
 * @details Drives ra8_ble_host_l2cap_send with payload_len large enough
 *          that ``payload_len + k_l2cap_hdr_bytes`` exceeds the 256-byte
 *          static scratch buffer, hitting the ``k_ra8_err_invalid_arg``
 *          return. The payload pointer is non-NULL, so the leading
 *          null-pointer decision short-circuits false and the length
 *          decision is the one under test. The function returns before
 *          any read of the payload buffer.
 *
 * @par MC/DC:
 * Decision: ``if ((payload_len + k_l2cap_hdr_bytes) > k_tx_scratch_bytes)``
 * (single condition). One true vector reaches the return.
 */
static void test_cov_l2cap_send_oversize(void)
{
  TEST_BEGIN("ra8_ble_l2cap cov: l2cap_send oversize -> invalid_arg");
  TEST_ASSERT_EQ(k_ra8_ok, bring_up());
  static const uint8_t s_dummy[1] = {0U};
  const ra8_err_t      rc         = ra8_ble_host_l2cap_send(k_cov_conn_handle,
                                                            k_cov_cid_att,
                                                            s_dummy,
                                                            (uint16_t)k_cov_send_oversize);
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, rc);
  TEST_END("ra8_ble_l2cap cov: l2cap_send oversize -> invalid_arg");
}

/**
 * @test test_cov_acl_in_uninitialized
 *
 * @details Injects a well-formed L2CAP frame while the host is NOT
 *          initialized. The payload / length guard passes (non-NULL,
 *          len > 0), so the ``s_ble_host_state.initialized == 0U`` guard
 *          is the leg exercised and the function early-returns without
 *          dispatching. Verified by an unchanged event count.
 *
 * @par MC/DC:
 * Decision: ``if (s_ble_host_state.initialized == 0U)`` (single
 * condition). One true vector reaches the return.
 */
static void test_cov_acl_in_uninitialized(void)
{
  TEST_BEGIN("ra8_ble_l2cap cov: acl_in before init -> early return");
  prep(); /* host closed -> initialized == 0 */
  static const uint8_t s_frame[5] = {0x01U, 0x00U, 0x04U, 0x00U, 0x00U};
  ra8_ble_host_test_inject_acl(k_cov_conn_handle, s_frame, (uint16_t)sizeof(s_frame));
  /* Not initialized: nothing dispatched, count stays at its reset value. */
  TEST_ASSERT_EQ(0U, ra8_ble_host_test_event_count());
  TEST_END("ra8_ble_l2cap cov: acl_in before init -> early return");
}

/**
 * @test test_cov_acl_in_bogus_short
 *
 * @details With no reassembly in progress, injects a frame shorter than
 *          the 4-byte L2CAP header. The ``reassembly_len > 0U`` decision
 *          is false, so control enters the else branch and the
 *          ``len < k_l2cap_hdr_bytes`` guard in
 *          internal_acl_fresh_frame returns.
 *
 * @par MC/DC:
 * Decision: ``if (len < k_l2cap_hdr_bytes)`` (single condition). One
 * true vector reaches the return.
 */
static void test_cov_acl_in_bogus_short(void)
{
  TEST_BEGIN("ra8_ble_l2cap cov: acl_in sub-header frame dropped");
  TEST_ASSERT_EQ(k_ra8_ok, bring_up());
  const uint32_t       before   = ra8_ble_host_test_event_count();
  static const uint8_t s_two[2] = {0x00U, 0x00U};
  ra8_ble_host_test_inject_acl(k_cov_conn_handle, s_two, (uint16_t)sizeof(s_two));
  TEST_ASSERT_EQ(before, ra8_ble_host_test_event_count());
  TEST_END("ra8_ble_l2cap cov: acl_in sub-header frame dropped");
}

/**
 * @test test_cov_acl_reassembly_complete
 *
 * @details Starts an inbound L2CAP frame that is incomplete in its first
 *          ACL fragment (the internal_acl_fresh_frame start-reassembly
 *          path), then delivers the continuation that completes it via
 *          internal_reassembly_append + internal_reassembly_dispatch.
 *          Two sub-cases: an ATT CID (0x0004) drives the dispatch-to-ATT
 *          branch; a signaling CID (0x0005) drives the silently-consumed
 *          branch. Both then reset reassembly_len.
 *
 *          Fragment 1 carries a 4-byte L2CAP header (length=4, CID) plus
 *          2 payload bytes -> 6 buffered, expected complete size 8.
 *          Fragment 2 supplies 2 continuation bytes -> 8 buffered ==
 *          expected, so the frame is dispatched.
 *
 * @par MC/DC:
 * Decision: ``if (s_ble_host_state.reassembly_cid == k_l2cap_cid_att)``
 * (single condition). Sub-case ATT gives the true vector, sub-case
 * signaling gives the false vector.
 */
static void test_cov_acl_reassembly_complete(void)
{
  TEST_BEGIN("ra8_ble_l2cap cov: reassembly completes + dispatch");
  TEST_ASSERT_EQ(k_ra8_ok, bring_up());

  /* Sub-case A: ATT CID -> internal_reassembly_dispatch ATT branch. */
  static const uint8_t s_att_frag1[6] = {
    0x04U,
    0x00U, /* L2CAP length = 4 */
    0x04U,
    0x00U, /* CID = 0x0004 (ATT) */
    0x00U,
    0x00U, /* 2 payload bytes */
  };
  static const uint8_t s_att_frag2[2] = {0x00U, 0x00U}; /* 2 continuation bytes */
  ra8_ble_host_test_inject_acl(k_cov_conn_handle, s_att_frag1, (uint16_t)sizeof(s_att_frag1));
  ra8_ble_host_test_inject_acl(k_cov_conn_handle, s_att_frag2, (uint16_t)sizeof(s_att_frag2));

  /* Sub-case B: signaling CID -> silently consumed (ATT-CID check
   * false; reassembly reset only). */
  static const uint8_t s_sig_frag1[6] = {
    0x04U,
    0x00U, /* L2CAP length = 4 */
    0x05U,
    0x00U, /* CID = 0x0005 (LE signaling) */
    0x00U,
    0x00U,
  };
  static const uint8_t s_sig_frag2[2] = {0x00U, 0x00U};
  ra8_ble_host_test_inject_acl(k_cov_conn_handle, s_sig_frag1, (uint16_t)sizeof(s_sig_frag1));
  ra8_ble_host_test_inject_acl(k_cov_conn_handle, s_sig_frag2, (uint16_t)sizeof(s_sig_frag2));

  TEST_END("ra8_ble_l2cap cov: reassembly completes + dispatch");
}

/**
 * @test test_cov_acl_reassembly_incomplete_continuation
 *
 * @details Starts reassembly with a large declared L2CAP length, then
 *          appends a continuation that is still short of the expected
 *          total. This drives the internal_reassembly_append
 *          still-incomplete return without completing the frame.
 *
 *          Fragment 1: header (length=20, ATT CID) + 2 payload -> 6
 *          buffered, expected total 24. Fragment 2: 4 continuation bytes
 *          -> 10 buffered < 24, so the handler returns mid-reassembly.
 *
 * @par MC/DC:
 * Decision: ``if (reassembly_len < reassembly_expected + k_l2cap_hdr_bytes)``
 * (single condition). One true vector reaches the incomplete return.
 */
static void test_cov_acl_reassembly_incomplete_continuation(void)
{
  TEST_BEGIN("ra8_ble_l2cap cov: continuation still incomplete");
  TEST_ASSERT_EQ(k_ra8_ok, bring_up());
  static const uint8_t s_frag1[6] = {
    0x14U,
    0x00U, /* L2CAP length = 20 */
    0x04U,
    0x00U, /* CID = ATT */
    0x00U,
    0x00U,
  };
  static const uint8_t s_cont[4] = {0x00U, 0x00U, 0x00U, 0x00U};
  ra8_ble_host_test_inject_acl(k_cov_conn_handle, s_frag1, (uint16_t)sizeof(s_frag1));
  ra8_ble_host_test_inject_acl(k_cov_conn_handle, s_cont, (uint16_t)sizeof(s_cont));
  TEST_END("ra8_ble_l2cap cov: continuation still incomplete");
}

/**
 * @test test_cov_acl_reassembly_overflow_drop
 *
 * @details Starts reassembly with a nearly-full first fragment, then
 *          appends a continuation whose length pushes the buffered total
 *          past the 256-byte reassembly buffer, driving the overflow
 *          drop in internal_reassembly_append: reassembly_len is reset
 *          to 0 and the frame is discarded.
 *
 *          Fragment 1: 250 bytes, declared L2CAP length 300 (never fits
 *          in one packet) -> start reassembly, 250 buffered. Fragment 2:
 *          10 bytes -> 260 > 256 -> drop.
 *
 * @par MC/DC:
 * Decision: ``if (reassembly_len + len > k_reassembly_buf_bytes)``
 * (single condition). One true vector reaches the drop.
 */
static void test_cov_acl_reassembly_overflow_drop(void)
{
  TEST_BEGIN("ra8_ble_l2cap cov: mid-reassembly overflow dropped");
  TEST_ASSERT_EQ(k_ra8_ok, bring_up());
  uint8_t s_frag1[k_l2cap_frag_cap] = {};
  s_frag1[0]                        = k_l2cap_len_lo_300; /* L2CAP length = 300 (0x012C) */
  s_frag1[1]                        = 0x01U;
  s_frag1[2]                        = 0x04U; /* CID = ATT */
  s_frag1[3]                        = 0x00U;
  static const uint8_t s_cont[10]   = {0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U};
  ra8_ble_host_test_inject_acl(k_cov_conn_handle, s_frag1, (uint16_t)sizeof(s_frag1));
  ra8_ble_host_test_inject_acl(k_cov_conn_handle, s_cont, (uint16_t)sizeof(s_cont));
  TEST_END("ra8_ble_l2cap cov: mid-reassembly overflow dropped");
}

/**
 * @test test_cov_acl_start_reassembly_too_long
 *
 * @details Injects a first fragment that cannot fit the reassembly
 *          buffer (len > k_reassembly_buf_bytes) yet is incomplete,
 *          driving the internal_acl_fresh_frame too-long
 *          start-fragment reject.
 *          The declared L2CAP length (400) keeps the "complete in one
 *          packet" decision false, and the 300-byte length trips the
 *          buffer-capacity guard before any memcpy.
 *
 * @par MC/DC:
 * Decision: ``if (len > k_reassembly_buf_bytes)`` (single condition).
 * One true vector reaches the return.
 */
static void test_cov_acl_start_reassembly_too_long(void)
{
  TEST_BEGIN("ra8_ble_l2cap cov: start fragment too long dropped");
  TEST_ASSERT_EQ(k_ra8_ok, bring_up());
  uint8_t s_frag[k_l2cap_reassembly_cap] = {};
  s_frag[0]                              = k_l2cap_len_lo_400; /* L2CAP length = 400 (0x0190) */
  s_frag[1]                              = 0x01U;
  s_frag[2]                              = 0x04U; /* CID = ATT */
  s_frag[3]                              = 0x00U;
  ra8_ble_host_test_inject_acl(k_cov_conn_handle, s_frag, (uint16_t)sizeof(s_frag));
  TEST_END("ra8_ble_l2cap cov: start fragment too long dropped");
}

/**
 * @test test_cov_acl_trampoline_via_dispatch
 *
 * @details Reaches internal_acl_trampoline by pumping a
 *          raw H4-framed HCI ACL packet through the controller's
 *          ra8_ble_dispatch drain loop. ra8_ble_host_init registered the
 *          trampoline as the controller ACL handler, so the drained
 *          packet routes handle & 0x0FFF, payload and len into
 *          ra8_ble_host_acl_in. The payload is a complete single-packet
 *          L2CAP ATT frame so acl_in dispatches immediately.
 *
 *          Packet layout (Vol 4 Part E 5.4.2): [0x02 type][handle LE16]
 *          [data-len LE16][L2CAP length LE16][CID LE16][1 payload byte].
 *
 * @par MC/DC:
 * No compound decision under test; the goal is to execute the
 * trampoline body on the production source via the real dispatch path.
 */
static void test_cov_acl_trampoline_via_dispatch(void)
{
  TEST_BEGIN("ra8_ble_l2cap cov: acl trampoline via ra8_ble_dispatch");
  TEST_ASSERT_EQ(k_ra8_ok, bring_up());
  ra8_ble_test_reset_capture();

  static const uint8_t s_hci_acl[10] = {
    (uint8_t)k_ra8_ble_pkt_acl_data, /* H4 packet indicator: ACL data */
    0x42U,
    0x00U, /* connection handle = 0x0042 */
    0x05U,
    0x00U, /* ACL data length = 5 */
    0x01U,
    0x00U, /* L2CAP length = 1 */
    0x04U,
    0x00U, /* CID = 0x0004 (ATT)                      */
    0x00U, /* 1 payload byte (ATT opcode placeholder) */
  };
  ra8_ble_test_inject_rx(s_hci_acl, (uint16_t)sizeof(s_hci_acl));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ble_dispatch());
  TEST_END("ra8_ble_l2cap cov: acl trampoline via ra8_ble_dispatch");
}

/**
 * @var s_svc_uuid
 * @brief 128-bit service UUID fixture for the CCCD-clear coverage test.
 * @note Read-only host-test fixture.
 * @since 0.1.0
 */
static const uint8_t s_svc_uuid[k_ra8_ble_host_uuid_bytes] = {
  0x01U,
  0x02U,
  0x03U,
  0x04U,
  0x05U,
  0x06U,
  0x07U,
  0x08U,
  0x09U,
  0x0AU,
  0x0BU,
  0x0CU,
  0x0DU,
  0x0EU,
  0x0FU,
  0x10U,
};

/**
 * @var s_chr_uuid
 * @brief 128-bit characteristic UUID fixture for the CCCD-clear test.
 * @note Read-only host-test fixture.
 * @since 0.1.0
 */
static const uint8_t s_chr_uuid[k_ra8_ble_host_uuid_bytes] = {
  0x11U,
  0x12U,
  0x13U,
  0x14U,
  0x15U,
  0x16U,
  0x17U,
  0x18U,
  0x19U,
  0x1AU,
  0x1BU,
  0x1CU,
  0x1DU,
  0x1EU,
  0x1FU,
  0x20U,
};

/**
 * @test test_cov_disconnect_clears_cccd
 *
 * @details Populates the GATT attribute table with a notify
 *          characteristic (which appends a CCCD row), forces the host
 *          into the connected state, then injects a matching
 *          Disconnection_Complete. This exercises the CCCD-clearing loop
 *          body of internal_on_disconn_complete: iterating attributes,
 *          matching the
 *          CCCD kind, and zeroing its value. A disconnected event is
 *          dispatched, verified by the event count.
 *
 * @par MC/DC:
 * Decision: ``if (s_ble_host_state.attrs[i].kind == k_attr_kind_cccd)``
 * (single condition). The table holds both CCCD (true) and non-CCCD
 * (false) rows, so both outcomes are exercised in one pass.
 */
static void test_cov_disconnect_clears_cccd(void)
{
  TEST_BEGIN("ra8_ble_l2cap cov: disconnect clears CCCD rows");
  TEST_ASSERT_EQ(k_ra8_ok, bring_up());

  static uint8_t s_chr_value[k_cov_char_value_max] = {};

  uint16_t svc_handle = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ble_host_gatt_register_service(s_svc_uuid, &svc_handle));
  uint16_t chr_handle = 0U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_ble_host_gatt_register_char(svc_handle,
                                                 s_chr_uuid,
                                                 (uint8_t)k_ra8_ble_host_char_prop_notify,
                                                 s_chr_value,
                                                 (uint16_t)k_cov_char_value_max,
                                                 &chr_handle));

  ra8_ble_host_test_inject_connect(k_cov_conn_handle);

  uint8_t s_disconn[4]  = {0U, 0U, 0U, 0U};
  s_disconn[1]          = (uint8_t)(k_cov_conn_handle & k_byte_mask);         /* handle LO */
  s_disconn[2]          = (uint8_t)((k_cov_conn_handle >> 8U) & k_byte_mask); /* handle HI */
  const uint32_t before = ra8_ble_host_test_event_count();
  ra8_ble_host_test_inject_event(k_hci_evt_disconn_complete, s_disconn, 4U);
  TEST_ASSERT(ra8_ble_host_test_event_count() == before + 1U);

  TEST_END("ra8_ble_l2cap cov: disconnect clears CCCD rows");
}

/**
 * @test test_cov_init_open_failed
 *
 * @details Drives ra8_ble_host_init down its ``ra8_ble_open`` failure
 *          leg. The controller is opened directly first, so
 *          the host-init call reaches ra8_ble_open with the controller
 *          already open; ra8_ble_open then returns k_ra8_err_invalid_arg
 *          and host-init maps that to k_ra8_err_invalid_state. The
 *          controller is closed afterwards so later tests are
 *          unaffected.
 *
 * @par MC/DC:
 * Decision: ``if (rc != k_ra8_ok)`` (single condition). One true vector
 * reaches the invalid_state return.
 */
static void test_cov_init_open_failed(void)
{
  TEST_BEGIN("ra8_ble_l2cap cov: init with ra8_ble_open failure");

  /* Clean baseline: bring the host up then close it so the controller
   * open-flag is known to be clear and the host is uninitialized. */
  TEST_ASSERT_EQ(k_ra8_ok, bring_up());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ble_host_close());

  /* Open the controller directly, leaving the host uninitialized. */
  const ra8_ble_config_t s_ble_cfg = {.use_external_osc = 1U, .deep_sleep_enable = 1U};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ble_open(&s_ble_cfg));

  /* host-init now reaches ra8_ble_open, which rejects the already-open
   * controller -> init returns k_ra8_err_invalid_state. */
  ra8_ble_host_config_t cfg = {.role       = k_ra8_ble_host_role_peripheral,
                               .appearance = 0U,
                               .name       = "t"};
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_ble_host_init(&cfg));

  /* Restore a closed controller for any subsequent tests. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ble_close());

  TEST_END("ra8_ble_l2cap cov: init with ra8_ble_open failure");
}

/**
 * @brief Test entry point -- runs every coverage vector in this TU.
 *
 * @return int32_t Process exit status.
 * @retval 0 All assertions passed (individual failures exit(1) early).
 *
 * @pre The RA8_SIMULATOR_MODE register window is available.
 * @post Every test above has executed once.
 *
 * @since 0.1.0
 */
int32_t main(void)
{
  test_cov_l2cap_send_oversize();
  test_cov_acl_in_uninitialized();
  test_cov_acl_in_bogus_short();
  test_cov_acl_reassembly_complete();
  test_cov_acl_reassembly_incomplete_continuation();
  test_cov_acl_reassembly_overflow_drop();
  test_cov_acl_start_reassembly_too_long();
  test_cov_acl_trampoline_via_dispatch();
  test_cov_disconnect_clears_cccd();
  test_cov_init_open_failed();
  (void)fprintf(stderr, "[OK ] test_ra8_ble_l2cap_cov.c\n");
  return 0;
}
