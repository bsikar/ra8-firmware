/**
 * @file test_ra_ble_l2cap_cov.c
 * @brief Line-coverage tests for the reassembly / trampoline / error legs
 *        of libs/ra_ble_host/src/ra_ble_l2cap.c.
 *
 * @details
 * The sibling suite test_ra_ble_l2cap.c drives the public-API MC/DC
 * vectors (role validation, the HCI event-trampoline guards). This TU
 * targets the paths that suite leaves uncovered:
 *
 *   - ra_ble_host_l2cap_send oversize guard (scratch-buffer bound).
 *   - ra_ble_host_acl_in run before the stack is initialized.
 *   - the inbound L2CAP reassembly state machine: start-fragment,
 *     incomplete continuation, complete continuation (ATT and non-ATT
 *     CID), the too-long start-fragment reject, and the mid-reassembly
 *     overflow drop.
 *   - internal_acl_trampoline, reached by pumping a raw HCI ACL packet
 *     through the controller's ra_ble_dispatch loop so the registered
 *     host handler runs (not the direct test veneer).
 *   - the CCCD-clearing loop body inside the Disconnection_Complete
 *     handler, reached with a live GATT table that holds a CCCD row.
 *   - the ra_ble_open-failed leg of ra_ble_host_init.
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

#include "ra_ble.h"
#include "ra_ble_host.h"
#include "ra_err.h"
#include "ra_sim_mmap.h"
#include "unity_minimal.h"

/* Internal L2CAP entry point (non-static global in ra_ble_l2cap.c, used by
 * ra_ble_att.c / ra_ble_gatt.c). Forward-declared here to drive its
 * scratch-buffer bound check directly. */
ra_err_t ra_ble_host_l2cap_send(uint16_t       conn_handle,
                                uint16_t       cid,
                                const uint8_t* payload,
                                uint16_t       payload_len);

/* Controller test hooks (external linkage in ra_ble.c, not in ra_ble.h). */
void ra_ble_test_inject_rx(const uint8_t* bytes, uint16_t len);
void ra_ble_test_reset_capture(void);

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
  ra_sim_mmap_reset();
  (void)ra_ble_host_close();
}

/**
 * @brief Reset then bring the BLE host stack up as a peripheral.
 *
 * @details Runs prep() and initialises the host so subsequent injects
 *          operate on an open controller.
 *
 * @return ra_err_t Result of ra_ble_host_init.
 * @retval k_ra_ok Stack initialised.
 *
 * @pre None.
 * @post On success the host and controller are open.
 * @post On success ra_ble_host_state()->initialized == 1.
 *
 * @note Not thread-safe; single-threaded test harness only.
 *
 * @since 0.1.0
 */
static ra_err_t bring_up(void)
{
  prep();
  ra_ble_host_config_t cfg = {.role = k_ra_ble_host_role_peripheral, .appearance = 0U, .name = "t"};
  return ra_ble_host_init(&cfg);
}

/**
 * @test test_cov_l2cap_send_oversize
 *
 * @details Drives ra_ble_host_l2cap_send with payload_len large enough
 *          that ``payload_len + k_l2cap_hdr_bytes`` exceeds the 256-byte
 *          static scratch buffer, hitting the ``k_ra_err_invalid_arg``
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
  TEST_BEGIN("ra_ble_l2cap cov: l2cap_send oversize -> invalid_arg (222)");
  TEST_ASSERT_EQ(k_ra_ok, bring_up());
  static const uint8_t s_dummy[1] = {0U};
  const ra_err_t       rc         = ra_ble_host_l2cap_send(k_cov_conn_handle,
                                                           k_cov_cid_att,
                                                           s_dummy,
                                                           (uint16_t)k_cov_send_oversize);
  TEST_ASSERT_EQ(k_ra_err_invalid_arg, rc);
  TEST_END("ra_ble_l2cap cov: l2cap_send oversize -> invalid_arg (222)");
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
  TEST_BEGIN("ra_ble_l2cap cov: acl_in before init -> early return (284)");
  prep(); /* host closed -> initialized == 0 */
  static const uint8_t s_frame[5] = {0x01U, 0x00U, 0x04U, 0x00U, 0x00U};
  ra_ble_host_test_inject_acl(k_cov_conn_handle, s_frame, (uint16_t)sizeof(s_frame));
  /* Not initialized: nothing dispatched, count stays at its reset value. */
  TEST_ASSERT_EQ(0U, ra_ble_host_test_event_count());
  TEST_END("ra_ble_l2cap cov: acl_in before init -> early return (284)");
}

/**
 * @test test_cov_acl_in_bogus_short
 *
 * @details With no reassembly in progress, injects a frame shorter than
 *          the 4-byte L2CAP header. The ``reassembly_len > 0U`` decision
 *          is false, so control enters the else branch and the
 *          ``len < k_l2cap_hdr_bytes`` guard returns (lines 301-303).
 *
 * @par MC/DC:
 * Decision: ``if (len < k_l2cap_hdr_bytes)`` (single condition). One
 * true vector reaches the return.
 */
static void test_cov_acl_in_bogus_short(void)
{
  TEST_BEGIN("ra_ble_l2cap cov: acl_in sub-header frame dropped (301-303)");
  TEST_ASSERT_EQ(k_ra_ok, bring_up());
  const uint32_t       before   = ra_ble_host_test_event_count();
  static const uint8_t s_two[2] = {0x00U, 0x00U};
  ra_ble_host_test_inject_acl(k_cov_conn_handle, s_two, (uint16_t)sizeof(s_two));
  TEST_ASSERT_EQ(before, ra_ble_host_test_event_count());
  TEST_END("ra_ble_l2cap cov: acl_in sub-header frame dropped (301-303)");
}

/**
 * @test test_cov_acl_reassembly_complete
 *
 * @details Starts an inbound L2CAP frame that is incomplete in its first
 *          ACL fragment (start-reassembly path, lines 318-326), then
 *          delivers the continuation that completes it (lines 330-335).
 *          Two sub-cases: an ATT CID (0x0004) drives the dispatch-to-ATT
 *          branch (lines 331-333); a signaling CID (0x0005) drives the
 *          silently-consumed branch. Both then reset reassembly_len.
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
  TEST_BEGIN("ra_ble_l2cap cov: reassembly completes + dispatch (318-335)");
  TEST_ASSERT_EQ(k_ra_ok, bring_up());

  /* Sub-case A: ATT CID -> dispatch branch (331-333). */
  static const uint8_t s_att_frag1[6] = {
    0x04U,
    0x00U, /* L2CAP length = 4 */
    0x04U,
    0x00U, /* CID = 0x0004 (ATT) */
    0x00U,
    0x00U, /* 2 payload bytes */
  };
  static const uint8_t s_att_frag2[2] = {0x00U, 0x00U}; /* 2 continuation bytes */
  ra_ble_host_test_inject_acl(k_cov_conn_handle, s_att_frag1, (uint16_t)sizeof(s_att_frag1));
  ra_ble_host_test_inject_acl(k_cov_conn_handle, s_att_frag2, (uint16_t)sizeof(s_att_frag2));

  /* Sub-case B: signaling CID -> silently consumed (330 false, 335). */
  static const uint8_t s_sig_frag1[6] = {
    0x04U,
    0x00U, /* L2CAP length = 4 */
    0x05U,
    0x00U, /* CID = 0x0005 (LE signaling) */
    0x00U,
    0x00U,
  };
  static const uint8_t s_sig_frag2[2] = {0x00U, 0x00U};
  ra_ble_host_test_inject_acl(k_cov_conn_handle, s_sig_frag1, (uint16_t)sizeof(s_sig_frag1));
  ra_ble_host_test_inject_acl(k_cov_conn_handle, s_sig_frag2, (uint16_t)sizeof(s_sig_frag2));

  TEST_END("ra_ble_l2cap cov: reassembly completes + dispatch (318-335)");
}

/**
 * @test test_cov_acl_reassembly_incomplete_continuation
 *
 * @details Starts reassembly with a large declared L2CAP length, then
 *          appends a continuation that is still short of the expected
 *          total. This drives the append + still-incomplete return
 *          (lines 295-299) without completing the frame.
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
  TEST_BEGIN("ra_ble_l2cap cov: continuation still incomplete (295-299)");
  TEST_ASSERT_EQ(k_ra_ok, bring_up());
  static const uint8_t s_frag1[6] = {
    0x14U,
    0x00U, /* L2CAP length = 20 */
    0x04U,
    0x00U, /* CID = ATT */
    0x00U,
    0x00U,
  };
  static const uint8_t s_cont[4] = {0x00U, 0x00U, 0x00U, 0x00U};
  ra_ble_host_test_inject_acl(k_cov_conn_handle, s_frag1, (uint16_t)sizeof(s_frag1));
  ra_ble_host_test_inject_acl(k_cov_conn_handle, s_cont, (uint16_t)sizeof(s_cont));
  TEST_END("ra_ble_l2cap cov: continuation still incomplete (295-299)");
}

/**
 * @test test_cov_acl_reassembly_overflow_drop
 *
 * @details Starts reassembly with a nearly-full first fragment, then
 *          appends a continuation whose length pushes the buffered total
 *          past the 256-byte reassembly buffer, driving the overflow
 *          drop (lines 289, 292, 293): reassembly_len is reset to 0 and
 *          the frame is discarded.
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
  TEST_BEGIN("ra_ble_l2cap cov: mid-reassembly overflow dropped (289-293)");
  TEST_ASSERT_EQ(k_ra_ok, bring_up());
  uint8_t s_frag1[250]            = {};
  s_frag1[0]                      = 0x2CU; /* L2CAP length = 300 (0x012C) */
  s_frag1[1]                      = 0x01U;
  s_frag1[2]                      = 0x04U; /* CID = ATT */
  s_frag1[3]                      = 0x00U;
  static const uint8_t s_cont[10] = {0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U};
  ra_ble_host_test_inject_acl(k_cov_conn_handle, s_frag1, (uint16_t)sizeof(s_frag1));
  ra_ble_host_test_inject_acl(k_cov_conn_handle, s_cont, (uint16_t)sizeof(s_cont));
  TEST_END("ra_ble_l2cap cov: mid-reassembly overflow dropped (289-293)");
}

/**
 * @test test_cov_acl_start_reassembly_too_long
 *
 * @details Injects a first fragment that cannot fit the reassembly
 *          buffer (len > k_reassembly_buf_bytes) yet is incomplete,
 *          driving the too-long start-fragment reject (lines 318-319).
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
  TEST_BEGIN("ra_ble_l2cap cov: start fragment too long dropped (318-319)");
  TEST_ASSERT_EQ(k_ra_ok, bring_up());
  uint8_t s_frag[300] = {};
  s_frag[0]           = 0x90U; /* L2CAP length = 400 (0x0190) */
  s_frag[1]           = 0x01U;
  s_frag[2]           = 0x04U; /* CID = ATT */
  s_frag[3]           = 0x00U;
  ra_ble_host_test_inject_acl(k_cov_conn_handle, s_frag, (uint16_t)sizeof(s_frag));
  TEST_END("ra_ble_l2cap cov: start fragment too long dropped (318-319)");
}

/**
 * @test test_cov_acl_trampoline_via_dispatch
 *
 * @details Reaches internal_acl_trampoline (lines 340-347) by pumping a
 *          raw H4-framed HCI ACL packet through the controller's
 *          ra_ble_dispatch drain loop. ra_ble_host_init registered the
 *          trampoline as the controller ACL handler, so the drained
 *          packet routes handle & 0x0FFF, payload and len into
 *          ra_ble_host_acl_in. The payload is a complete single-packet
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
  TEST_BEGIN("ra_ble_l2cap cov: acl trampoline via ra_ble_dispatch (340-347)");
  TEST_ASSERT_EQ(k_ra_ok, bring_up());
  ra_ble_test_reset_capture();

  static const uint8_t s_hci_acl[10] = {
    (uint8_t)k_ra_ble_pkt_acl_data, /* H4 packet indicator: ACL data */
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
  ra_ble_test_inject_rx(s_hci_acl, (uint16_t)sizeof(s_hci_acl));
  TEST_ASSERT_EQ(k_ra_ok, ra_ble_dispatch());
  TEST_END("ra_ble_l2cap cov: acl trampoline via ra_ble_dispatch (340-347)");
}

/**
 * @test test_cov_disconnect_clears_cccd
 *
 * @details Populates the GATT attribute table with a notify
 *          characteristic (which appends a CCCD row), forces the host
 *          into the connected state, then injects a matching
 *          Disconnection_Complete. This exercises the CCCD-clearing loop
 *          body (lines 424-428): iterating attributes, matching the
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
  TEST_BEGIN("ra_ble_l2cap cov: disconnect clears CCCD rows (424-428)");
  TEST_ASSERT_EQ(k_ra_ok, bring_up());

  static const uint8_t s_svc_uuid[k_ra_ble_host_uuid_bytes] = {
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
  static const uint8_t s_chr_uuid[k_ra_ble_host_uuid_bytes] = {
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
  static uint8_t s_chr_value[k_cov_char_value_max] = {};

  uint16_t svc_handle = 0U;
  TEST_ASSERT_EQ(k_ra_ok, ra_ble_host_gatt_register_service(s_svc_uuid, &svc_handle));
  uint16_t chr_handle = 0U;
  TEST_ASSERT_EQ(k_ra_ok,
                 ra_ble_host_gatt_register_char(svc_handle,
                                                s_chr_uuid,
                                                (uint8_t)k_ra_ble_host_char_prop_notify,
                                                s_chr_value,
                                                (uint16_t)k_cov_char_value_max,
                                                &chr_handle));

  ra_ble_host_test_inject_connect(k_cov_conn_handle);

  uint8_t s_disconn[4]  = {0U, 0U, 0U, 0U};
  s_disconn[1]          = (uint8_t)(k_cov_conn_handle & 0xFFU);         /* handle LO */
  s_disconn[2]          = (uint8_t)((k_cov_conn_handle >> 8U) & 0xFFU); /* handle HI */
  const uint32_t before = ra_ble_host_test_event_count();
  ra_ble_host_test_inject_event(0x05U, s_disconn, 4U);
  TEST_ASSERT(ra_ble_host_test_event_count() == before + 1U);

  TEST_END("ra_ble_l2cap cov: disconnect clears CCCD rows (424-428)");
}

/**
 * @test test_cov_init_open_failed
 *
 * @details Drives ra_ble_host_init down its ``ra_ble_open`` failure leg
 *          (lines 489-490). The controller is opened directly first, so
 *          the host-init call reaches ra_ble_open with the controller
 *          already open; ra_ble_open then returns k_ra_err_invalid_arg
 *          and host-init maps that to k_ra_err_invalid_state. The
 *          controller is closed afterwards so later tests are
 *          unaffected.
 *
 * @par MC/DC:
 * Decision: ``if (rc != k_ra_ok)`` (single condition). One true vector
 * reaches the invalid_state return.
 */
static void test_cov_init_open_failed(void)
{
  TEST_BEGIN("ra_ble_l2cap cov: init with ra_ble_open failure (489-490)");

  /* Clean baseline: bring the host up then close it so the controller
   * open-flag is known to be clear and the host is uninitialized. */
  TEST_ASSERT_EQ(k_ra_ok, bring_up());
  TEST_ASSERT_EQ(k_ra_ok, ra_ble_host_close());

  /* Open the controller directly, leaving the host uninitialized. */
  const ra_ble_config_t s_ble_cfg = {.use_external_osc = 1U, .deep_sleep_enable = 1U};
  TEST_ASSERT_EQ(k_ra_ok, ra_ble_open(&s_ble_cfg));

  /* host-init now reaches ra_ble_open, which rejects the already-open
   * controller -> init returns k_ra_err_invalid_state. */
  ra_ble_host_config_t cfg = {.role = k_ra_ble_host_role_peripheral, .appearance = 0U, .name = "t"};
  TEST_ASSERT_EQ(k_ra_err_invalid_state, ra_ble_host_init(&cfg));

  /* Restore a closed controller for any subsequent tests. */
  TEST_ASSERT_EQ(k_ra_ok, ra_ble_close());

  TEST_END("ra_ble_l2cap cov: init with ra_ble_open failure (489-490)");
}

/**
 * @brief Test entry point -- runs every coverage vector in this TU.
 *
 * @return int32_t Process exit status.
 * @retval 0 All assertions passed (individual failures exit(1) early).
 *
 * @pre The RA_SIMULATOR_MODE register window is available.
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
  (void)fprintf(stderr, "[OK ] test_ra_ble_l2cap_cov.c\n");
  return 0;
}
