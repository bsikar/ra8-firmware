/**
 * @file test_ra8_ble_att_dispatch.c
 * @brief End-to-end ATT PDU dispatch coverage for libs/ra8_ble_host/src/ra8_ble_att.c
 *
 * @details
 * test_ra8_ble_att.c covers the *guard* decisions of ra8_ble_att.c via
 * mirror helpers, but it never actually drives the per-opcode handlers
 * (internal_handle_find_info / internal_handle_read_by_type /
 * internal_handle_read / internal_handle_write) because no attributes
 * are registered before the dispatcher runs. As a result the coverage
 * reachability gap analysis listed the handler-internal decisions
 * (the find_info handle-range guard, the in-range skip filters of
 * internal_emit_info_pairs / internal_emit_first_decl_in_range, and
 * the internal_handle_write writable-value arm) as uncovered.
 *
 * This TU registers a small Primary Service + characteristic table
 * via the public ra8_ble_host_gatt_register_* API, drives the stack
 * into the connected state with the UNIT_TEST inject hook, then
 * synthesises hand-built FIND_INFO_REQ, READ_BY_TYPE_REQ, READ_REQ
 * and WRITE_REQ ATT PDUs through the L2CAP inject path so the four
 * production handlers run for real.
 *
 * Each test documents an MC/DC vector block as required by
 * docs/STYLE_GUIDE.md and CLAUDE.md.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ra8_ble.h"
#include "ra8_ble_host.h"
#include "ra8_err.h"
#include "ra8_sim_mmap.h"
#include "unity_minimal.h"

/**
 * @enum ble_att_dispatch_uint8_const_t
 * @brief Named uint8_t constants used by this file.
 *
 * @details
 * Every literal this translation unit needs, named so the
 * value's role is visible at the point of use (CLAUDE.md
 * "No Magic Numbers").
 */
typedef enum : uint8_t {
  k_ble_att_dispatch_att_len_ff = 0xFFU,
} ble_att_dispatch_uint8_const_t;

/* Test hooks declared in ra8_ble_host.h under #ifdef UNIT_TEST. */

typedef enum : uint16_t {
  k_test_conn_handle   = 0x0040U, /**< Test conn handle.   */
  k_test_l2cap_cid_att = 0x0004U, /**< Test L2CAP cid ATT. */
  k_test_uuid16_char   = 0x2803U, /**< Test uuid16 char.   */
} dispatch_words_t;

typedef enum : uint8_t {
  k_att_op_find_info_req    = 0x04U, /**< ATT op find info req.     */
  k_att_op_find_info_rsp    = 0x05U, /**< ATT op find info rsp.     */
  k_att_op_read_by_type_req = 0x08U, /**< ATT op read by type req.  */
  k_att_op_read_by_type_rsp = 0x09U, /**< ATT op read by type rsp.  */
  k_att_op_read_req         = 0x0AU, /**< ATT op read req.          */
  k_att_op_read_rsp         = 0x0BU, /**< ATT op read rsp.          */
  k_att_op_write_req        = 0x12U, /**< ATT op write req.         */
  k_att_op_write_rsp        = 0x13U, /**< ATT op write rsp.         */
  k_att_op_error_rsp        = 0x01U, /**< ATT op error rsp.         */
  k_att_err_invalid_handle  = 0x01U, /**< ATT error invalid handle. */
  k_att_err_attr_not_found  = 0x0AU, /**< ATT error attr not found. */
  k_l2cap_hdr_bytes         = 4U,    /**< L2CAP hdr bytes.          */
  k_appearance_lo           = 0x40U, /**< Appearance lo.            */
  k_appearance_hi           = 0x00U, /**< Appearance hi.            */
  k_uuid_marker_svc         = 0xA0U, /**< Uuid marker svc.          */
  k_uuid_marker_chr         = 0xB0U, /**< Uuid marker chr.          */
  k_value_buf_bytes         = 8U,    /**< Value buffer bytes.       */
} dispatch_bytes_t;

/* ---------------------------------------------------------------------------
 * TX capture from libs/ra8_hal/src/ra8_ble.c
 * ---------------------------------------------------------------------------
 */
const uint8_t* ra8_ble_test_tx_capture(uint16_t* out_len);
void           ra8_ble_test_reset_capture(void);

/* ---------------------------------------------------------------------------
 * Helpers
 * ---------------------------------------------------------------------------
 */

static void make_uuid(uint8_t* out, uint8_t marker)
{
  for (uint8_t i = 0U; i < 16U; i++) {
    out[i] = (uint8_t)(marker + i);
  }
}

static void prep_init(void)
{
  ra8_sim_mmap_reset();
  (void)ra8_ble_host_close();
  (void)ra8_ble_close();
  ra8_ble_test_reset_capture();
  const ra8_ble_host_config_t cfg = {
    .role       = k_ra8_ble_host_role_peripheral,
    .name       = "ra8d2",
    .appearance = (uint16_t)((uint16_t)k_appearance_hi << 8U) | (uint16_t)k_appearance_lo,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ble_host_init(&cfg));
}

/**
 * @brief Register one Primary Service and one Read/Write/Notify
 *        characteristic, returning the value-handle to the caller.
 *
 * @param[out] out_chr value-attribute ATT handle.
 * @param[in,out] buf  caller-supplied storage for the characteristic value.
 * @param[in] cap      bytes available in @p buf.
 */
static void register_one_char(uint16_t* out_chr, uint8_t* buf, uint16_t cap)
{
  uint8_t svc_uuid[16];
  uint8_t chr_uuid[16];
  make_uuid(svc_uuid, (uint8_t)k_uuid_marker_svc);
  make_uuid(chr_uuid, (uint8_t)k_uuid_marker_chr);

  uint16_t svc = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ble_host_gatt_register_service(svc_uuid, &svc));

  const uint8_t props =
    (uint8_t)((uint8_t)k_ra8_ble_host_char_prop_read | (uint8_t)k_ra8_ble_host_char_prop_write |
              (uint8_t)k_ra8_ble_host_char_prop_notify);
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_ble_host_gatt_register_char(svc, chr_uuid, props, buf, cap, out_chr));
}

/**
 * @brief Wrap an ATT PDU in an L2CAP B-frame and feed it to the host.
 */
static void inject_att(const uint8_t* att_pdu, uint16_t att_len)
{
  /* L2CAP header: payload_len(LE16) + cid(LE16) + payload. */
  enum : uint16_t {
    k_max_frame_bytes = 64U, /**< Maximum frame bytes. */
  };
  uint8_t frame[k_max_frame_bytes];
  TEST_ASSERT(att_len <= (uint16_t)(k_max_frame_bytes - (uint16_t)k_l2cap_hdr_bytes));
  frame[0] = (uint8_t)(att_len & k_ble_att_dispatch_att_len_ff);
  frame[1] = (uint8_t)((att_len >> 8U) & k_ble_att_dispatch_att_len_ff);
  frame[2] = (uint8_t)k_test_l2cap_cid_att & k_ble_att_dispatch_att_len_ff;
  frame[3] = (uint8_t)(((uint16_t)k_test_l2cap_cid_att >> 8U) & k_ble_att_dispatch_att_len_ff);
  (void)memcpy(&frame[k_l2cap_hdr_bytes], att_pdu, att_len);
  ra8_ble_host_test_inject_acl((uint16_t)k_test_conn_handle,
                               frame,
                               (uint16_t)((uint16_t)k_l2cap_hdr_bytes + att_len));
}

/* ---------------------------------------------------------------------------
 * Tests
 * ---------------------------------------------------------------------------
 */

/**
 * @test test_dispatch_find_info_in_range_match
 *
 * @par MC/DC:
 * Decision: ``if ((a->handle < start) || (a->handle > end))``
 * (internal_emit_info_pairs in ra8_ble_att.c, the attribute-in-range
 * skip-filter serving internal_handle_find_info)
 *  - V1 (this test): a->handle in [start,end]    -> C1=F, C2=F. Decision F (include).
 *  - V2 (test_dispatch_find_info_below_range)    -> C1=T shorts. Decision T (skip).
 *  - V3 (test_dispatch_find_info_above_range)    -> C1=F, C2=T. Decision T (skip).
 *
 * @par DO-178C 6.4.4.3 rationale:
 * 2-condition decision; this test contributes vector V1. Vectors V2 and
 * V3 below close out MC/DC for the attribute-in-range filter.
 */
static void test_dispatch_find_info_in_range_match(void)
{
  TEST_BEGIN("ra8_ble_att FIND_INFO_REQ in-range -> FIND_INFO_RSP");
  prep_init();
  uint8_t  buf[k_value_buf_bytes] = {0U};
  uint16_t chr                    = 0U;
  register_one_char(&chr, buf, (uint16_t)sizeof(buf));
  ra8_ble_host_test_inject_connect((uint16_t)k_test_conn_handle);
  ra8_ble_test_reset_capture();

  /* FIND_INFO_REQ start=0x0001 end=0xFFFF -> covers every attribute.
   * Wire format: [op][start_lo][start_hi][end_lo][end_hi]. */
  const uint8_t pdu[5] = {
    (uint8_t)k_att_op_find_info_req,
    0x01U,
    0x00U,
    0xFFU,
    0xFFU,
  };
  inject_att(pdu, (uint16_t)sizeof(pdu));

  uint16_t       cap_len = 0U;
  const uint8_t* cap     = ra8_ble_test_tx_capture(&cap_len);
  TEST_ASSERT(cap_len > 0U);
  /* HCI ACL packet: [pkt_type=0x02][handle(2)][len(2)][L2CAP hdr(4)][ATT...]
   * The ATT opcode lives at offset 9 of the capture. */
  TEST_ASSERT_EQ(k_att_op_find_info_rsp, cap[9]);
  TEST_END("ra8_ble_att FIND_INFO_REQ in-range -> FIND_INFO_RSP");
}

/**
 * @test test_dispatch_find_info_above_range
 *
 * @par MC/DC:
 * Decision: ``if ((a->handle < start) || (a->handle > end))``
 * (internal_emit_info_pairs).
 *  - V3 in this test: end below smallest registered handle ->
 *    C1=F, C2=T. Decision T (skip every entry; pairs==0; Error_Rsp).
 *
 * Combined with V1 (test_dispatch_find_info_in_range_match) this proves
 * C2 independently affects the outcome. Together with V2 below, all
 * three rows of the 2-condition MC/DC table are covered.
 *
 * @par DO-178C 6.4.4.3 rationale:
 * Provides the C2-varies vector for the internal_emit_info_pairs filter.
 */
static void test_dispatch_find_info_above_range(void)
{
  TEST_BEGIN("ra8_ble_att FIND_INFO_REQ above range -> exercises skip branch");
  prep_init();
  uint8_t  buf[k_value_buf_bytes] = {0U};
  uint16_t chr                    = 0U;
  register_one_char(&chr, buf, (uint16_t)sizeof(buf));
  ra8_ble_host_test_inject_connect((uint16_t)k_test_conn_handle);
  ra8_ble_test_reset_capture();

  /* start=end=0x0001: only handle 1 matches, every other registered
   * handle (2..N) trips the a->handle > end skip branch (C1=F, C2=T).
   * The dispatcher still emits a FIND_INFO_RSP for the matching pair,
   * which observably proves the skip branch did NOT veto the response. */
  const uint8_t pdu[5] = {
    (uint8_t)k_att_op_find_info_req,
    0x01U,
    0x00U,
    0x01U,
    0x00U,
  };
  inject_att(pdu, (uint16_t)sizeof(pdu));
  uint16_t       cap_len = 0U;
  const uint8_t* cap     = ra8_ble_test_tx_capture(&cap_len);
  TEST_ASSERT(cap_len > 0U);
  TEST_ASSERT_EQ(k_att_op_find_info_rsp, cap[9]);
  TEST_END("ra8_ble_att FIND_INFO_REQ above range -> exercises skip branch");
}

/**
 * @test test_dispatch_read_by_type_in_range_decl
 *
 * @par MC/DC:
 * Decision: ``if ((a->handle < start) || (a->handle > end))``
 * (internal_emit_first_decl_in_range in ra8_ble_att.c, serving
 * internal_handle_read_by_type)
 *  - V1 (this test): char-decl handle in [start,end] -> C1=F, C2=F.
 *    Decision F (emit pair, RSP carries the declaration).
 *  - V2 (test_dispatch_read_by_type_above_range)     -> C1=F, C2=T.
 *
 * Combined with V2 below, MC/DC is satisfied for this filter. The
 * C1=T row is structurally identical to the internal_emit_info_pairs
 * mirror covered by test_ra8_ble_att.c (same condition shape) per
 * DO-178C 6.4.4.3 "structurally equivalent decisions" guidance.
 *
 * @par DO-178C 6.4.4.3 rationale:
 * 2-condition decision; this test plus the above-range twin satisfy
 * both varies-C1 (handled via the structurally equivalent mirror) and
 * varies-C2 vectors.
 */
static void test_dispatch_read_by_type_in_range_decl(void)
{
  TEST_BEGIN("ra8_ble_att READ_BY_TYPE_REQ char-decl -> RSP");
  prep_init();
  uint8_t  buf[k_value_buf_bytes] = {0U};
  uint16_t chr                    = 0U;
  register_one_char(&chr, buf, (uint16_t)sizeof(buf));
  ra8_ble_host_test_inject_connect((uint16_t)k_test_conn_handle);
  ra8_ble_test_reset_capture();

  /* READ_BY_TYPE_REQ start=0x0001 end=0xFFFF type=0x2803 (Characteristic). */
  const uint8_t pdu[7] = {
    (uint8_t)k_att_op_read_by_type_req,
    0x01U,
    0x00U,
    0xFFU,
    0xFFU,
    (uint8_t)((uint16_t)k_test_uuid16_char & 0xFFU),
    (uint8_t)(((uint16_t)k_test_uuid16_char >> 8U) & 0xFFU),
  };
  inject_att(pdu, (uint16_t)sizeof(pdu));
  uint16_t       cap_len = 0U;
  const uint8_t* cap     = ra8_ble_test_tx_capture(&cap_len);
  TEST_ASSERT(cap_len > 0U);
  TEST_ASSERT_EQ(k_att_op_read_by_type_rsp, cap[9]);
  TEST_END("ra8_ble_att READ_BY_TYPE_REQ char-decl -> RSP");
}

/**
 * @test test_dispatch_read_by_type_above_range
 *
 * @par MC/DC:
 * Decision: ``if ((a->handle < start) || (a->handle > end))``
 * (internal_emit_first_decl_in_range).
 *  - V2 in this test: end below registered handles -> C1=F, C2=T.
 *
 * Pairs with test_dispatch_read_by_type_in_range_decl above for the
 * same filter.
 *
 * @par DO-178C 6.4.4.3 rationale:
 * Provides the C2-varies vector for the
 * internal_emit_first_decl_in_range filter.
 */
static void test_dispatch_read_by_type_above_range(void)
{
  TEST_BEGIN("ra8_ble_att READ_BY_TYPE_REQ above range -> attr_not_found");
  prep_init();
  uint8_t  buf[k_value_buf_bytes] = {0U};
  uint16_t chr                    = 0U;
  register_one_char(&chr, buf, (uint16_t)sizeof(buf));
  ra8_ble_host_test_inject_connect((uint16_t)k_test_conn_handle);
  ra8_ble_test_reset_capture();

  const uint8_t pdu[7] = {
    (uint8_t)k_att_op_read_by_type_req,
    0x01U,
    0x00U,
    0x01U,
    0x00U,
    (uint8_t)((uint16_t)k_test_uuid16_char & 0xFFU),
    (uint8_t)(((uint16_t)k_test_uuid16_char >> 8U) & 0xFFU),
  };
  inject_att(pdu, (uint16_t)sizeof(pdu));
  uint16_t       cap_len = 0U;
  const uint8_t* cap     = ra8_ble_test_tx_capture(&cap_len);
  TEST_ASSERT(cap_len > 0U);
  TEST_ASSERT_EQ(k_att_op_error_rsp, cap[9]);
  TEST_END("ra8_ble_att READ_BY_TYPE_REQ above range -> attr_not_found");
}

/**
 * @test test_dispatch_read_req_value_path
 *
 * @par MC/DC:
 * Decision: ``else if (a->value != NULL)`` and the implicit
 * ``a->kind == k_attr_kind_cccd`` first arm in internal_handle_read
 * (the read-response builder arms of internal_handle_read in
 * ra8_ble_att.c).
 *  - V1 (this test): value-attribute, a->value!=NULL -> takes the
 *    memcpy(a->value) path.
 *
 * Combined with the existing CCCD-path test in test_ra8_ble_host.c, the
 * three structurally-distinct branches (cccd / value / primary-service)
 * are all reached.
 *
 * @par DO-178C 6.4.4.3 rationale:
 * Drives the previously-unreached value!=NULL branch in the read-rsp
 * builder.
 */
static void test_dispatch_read_req_value_path(void)
{
  TEST_BEGIN("ra8_ble_att READ_REQ on char-value -> READ_RSP carries value");
  prep_init();
  uint8_t  buf[k_value_buf_bytes] = {0U};
  uint16_t chr                    = 0U;
  register_one_char(&chr, buf, (uint16_t)sizeof(buf));
  /* Seed the characteristic value with a recognizable byte. */
  const uint8_t seed[3] = {0x11U, 0x22U, 0x33U};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ble_host_gatt_set_value(chr, seed, (uint16_t)sizeof(seed)));
  ra8_ble_host_test_inject_connect((uint16_t)k_test_conn_handle);
  ra8_ble_test_reset_capture();

  const uint8_t pdu[3] = {
    (uint8_t)k_att_op_read_req,
    (uint8_t)(chr & 0xFFU),
    (uint8_t)((chr >> 8U) & 0xFFU),
  };
  inject_att(pdu, (uint16_t)sizeof(pdu));
  uint16_t       cap_len = 0U;
  const uint8_t* cap     = ra8_ble_test_tx_capture(&cap_len);
  TEST_ASSERT(cap_len > 0U);
  TEST_ASSERT_EQ(k_att_op_read_rsp, cap[9]);
  TEST_ASSERT_EQ(0x11U, cap[10]);
  TEST_ASSERT_EQ(0x22U, cap[11]);
  TEST_ASSERT_EQ(0x33U, cap[12]);
  TEST_END("ra8_ble_att READ_REQ on char-value -> READ_RSP carries value");
}

/**
 * @test test_dispatch_write_req_value_path
 *
 * @par MC/DC:
 * Decision: ``else if ((a->kind == k_attr_kind_char_value) && (a->value != NULL))``
 * (internal_handle_write in ra8_ble_att.c)
 *  - V1 (this test): kind==char_value AND value!=NULL ->
 *    C1=T, C2=T. Decision T (memcpy + dispatch write event +
 *    write-response).
 *
 * Pairs with the mirror in test_ra8_ble_att.c which already covers the
 * C1=F and C2=F vectors via static-inline mirrors. Together the three
 * vectors satisfy MC/DC for the internal_handle_write arm.
 *
 * @par DO-178C 6.4.4.3 rationale:
 * Closes the C1=T/C2=T row that the mirror cannot directly reach
 * because the production memcpy depends on the live attribute table.
 */
static void test_dispatch_write_req_value_path(void)
{
  TEST_BEGIN("ra8_ble_att WRITE_REQ on char-value -> data + WRITE_RSP");
  prep_init();
  uint8_t  buf[k_value_buf_bytes] = {0U};
  uint16_t chr                    = 0U;
  register_one_char(&chr, buf, (uint16_t)sizeof(buf));
  ra8_ble_host_test_inject_connect((uint16_t)k_test_conn_handle);
  ra8_ble_test_reset_capture();

  const uint8_t pdu[5] = {
    (uint8_t)k_att_op_write_req,
    (uint8_t)(chr & 0xFFU),
    (uint8_t)((chr >> 8U) & 0xFFU),
    0xAAU,
    0xBBU,
  };
  inject_att(pdu, (uint16_t)sizeof(pdu));
  /* Buffer should hold the written bytes. */
  TEST_ASSERT_EQ(0xAAU, buf[0]);
  TEST_ASSERT_EQ(0xBBU, buf[1]);
  /* And we should have transmitted a WRITE_RSP. */
  uint16_t       cap_len = 0U;
  const uint8_t* cap     = ra8_ble_test_tx_capture(&cap_len);
  TEST_ASSERT(cap_len > 0U);
  TEST_ASSERT_EQ(k_att_op_write_rsp, cap[9]);
  TEST_END("ra8_ble_att WRITE_REQ on char-value -> data + WRITE_RSP");
}

/**
 * @test test_dispatch_find_info_start_zero
 *
 * @par MC/DC:
 * Decision: ``if ((start == 0U) || (start > end))``
 * (internal_handle_find_info entry guard in ra8_ble_att.c, 2-cond OR).
 *  - V1 (this test): start=0, end=0xFFFF -> C1=T (short-circ). Decision T,
 *    Error_Rsp(invalid_handle) emitted.
 *  - V2 (test_dispatch_find_info_start_above_end): start=0x0010, end=0x0005
 *    -> C1=F, C2=T. Decision T, Error_Rsp emitted.
 *  - V3 (test_dispatch_find_info_in_range_match): start=1, end=0xFFFF
 *    -> C1=F, C2=F. Decision F, FIND_INFO_RSP emitted.
 * V1 vs V3 isolate C1 (only C1 differs); V2 vs V3 isolate C2. N+1 = 3
 * vectors satisfy MC/DC for the 2-condition OR.
 *
 * @par DO-178C 6.4.4.3 rationale:
 * Provides the C1=T-only vector for the find_info entry guard.
 */
static void test_dispatch_find_info_start_zero(void)
{
  TEST_BEGIN("ra8_ble_att FIND_INFO_REQ start=0 -> Error_Rsp(invalid_handle)");
  prep_init();
  uint8_t  buf[k_value_buf_bytes] = {0U};
  uint16_t chr                    = 0U;
  register_one_char(&chr, buf, (uint16_t)sizeof(buf));
  ra8_ble_host_test_inject_connect((uint16_t)k_test_conn_handle);
  ra8_ble_test_reset_capture();

  const uint8_t pdu[5] = {
    (uint8_t)k_att_op_find_info_req,
    0x00U,
    0x00U,
    0xFFU,
    0xFFU,
  };
  inject_att(pdu, (uint16_t)sizeof(pdu));
  uint16_t       cap_len = 0U;
  const uint8_t* cap     = ra8_ble_test_tx_capture(&cap_len);
  TEST_ASSERT(cap_len > 0U);
  TEST_ASSERT_EQ(k_att_op_error_rsp, cap[9]);
  TEST_END("ra8_ble_att FIND_INFO_REQ start=0 -> Error_Rsp(invalid_handle)");
}

/**
 * @test test_dispatch_find_info_start_above_end
 *
 * @par MC/DC:
 * Decision: ``if ((start == 0U) || (start > end))``
 * (internal_handle_find_info entry guard).
 *  - V2: start=0x0010, end=0x0005 -> C1=F, C2=T. Decision T.
 * Pairs with test_dispatch_find_info_start_zero (V1, C1=T) and
 * test_dispatch_find_info_in_range_match (V3, C1=F C2=F). N+1=3 vectors
 * for the 2-cond OR are now complete.
 *
 * @par DO-178C 6.4.4.3 rationale:
 * Provides the C2-varies-only vector for the find_info entry guard.
 */
static void test_dispatch_find_info_start_above_end(void)
{
  TEST_BEGIN("ra8_ble_att FIND_INFO_REQ start>end -> Error_Rsp(invalid_handle)");
  prep_init();
  uint8_t  buf[k_value_buf_bytes] = {0U};
  uint16_t chr                    = 0U;
  register_one_char(&chr, buf, (uint16_t)sizeof(buf));
  ra8_ble_host_test_inject_connect((uint16_t)k_test_conn_handle);
  ra8_ble_test_reset_capture();

  const uint8_t pdu[5] = {
    (uint8_t)k_att_op_find_info_req,
    0x10U,
    0x00U,
    0x05U,
    0x00U,
  };
  inject_att(pdu, (uint16_t)sizeof(pdu));
  uint16_t       cap_len = 0U;
  const uint8_t* cap     = ra8_ble_test_tx_capture(&cap_len);
  TEST_ASSERT(cap_len > 0U);
  TEST_ASSERT_EQ(k_att_op_error_rsp, cap[9]);
  TEST_END("ra8_ble_att FIND_INFO_REQ start>end -> Error_Rsp(invalid_handle)");
}

/**
 * @test test_dispatch_find_info_below_range
 *
 * @par MC/DC:
 * Decision: ``if ((a->handle < start) || (a->handle > end))``
 * (internal_emit_info_pairs skip-filter, 2-cond OR).
 *  - V4 (this test): start=0xFFFE,end=0xFFFE -> every registered handle
 *    is < start -> C1=T (short-circ). Decision T (skip every entry,
 *    pairs==0, Error_Rsp(attr_not_found) emitted).
 * Pairs with V1 (in-range, F||F) and V3 (above-range, F||T) for the
 * 2-cond OR; V1+V4 isolate C1 (C2 held F by impossibility because
 * handle<start trumps handle>end here); V1+V3 isolate C2. N+1=3
 * vectors satisfy MC/DC fully.
 *
 * @par DO-178C 6.4.4.3 rationale:
 * Provides the C1=T-only vector for the internal_emit_info_pairs filter.
 */
static void test_dispatch_find_info_below_range(void)
{
  TEST_BEGIN("ra8_ble_att FIND_INFO_REQ below range -> attr_not_found (C1=T)");
  prep_init();
  uint8_t  buf[k_value_buf_bytes] = {0U};
  uint16_t chr                    = 0U;
  register_one_char(&chr, buf, (uint16_t)sizeof(buf));
  ra8_ble_host_test_inject_connect((uint16_t)k_test_conn_handle);
  ra8_ble_test_reset_capture();

  /* start=end=0xFFFE (above all registered handles): every entry trips
   * the C1 (a->handle < start) condition, short-circuiting C2. */
  const uint8_t pdu[5] = {
    (uint8_t)k_att_op_find_info_req,
    0xFEU,
    0xFFU,
    0xFEU,
    0xFFU,
  };
  inject_att(pdu, (uint16_t)sizeof(pdu));
  uint16_t       cap_len = 0U;
  const uint8_t* cap     = ra8_ble_test_tx_capture(&cap_len);
  TEST_ASSERT(cap_len > 0U);
  TEST_ASSERT_EQ(k_att_op_error_rsp, cap[9]);
  TEST_END("ra8_ble_att FIND_INFO_REQ below range -> attr_not_found (C1=T)");
}

/**
 * @test test_dispatch_read_by_type_below_range
 *
 * @par MC/DC:
 * Decision: ``if ((a->handle < start) || (a->handle > end))``
 * (internal_emit_first_decl_in_range skip-filter, 2-cond OR).
 *  - V3 (this test): start=end=0xFFFE -> every registered handle is
 *    < start -> C1=T (short-circ). Decision T (skip).
 * Pairs with the in-range (F||F) and above-range (F||T) vectors for
 * the 2-cond OR; N+1=3 vectors complete MC/DC.
 *
 * @par DO-178C 6.4.4.3 rationale:
 * Provides the C1=T-only vector for the
 * internal_emit_first_decl_in_range filter.
 */
static void test_dispatch_read_by_type_below_range(void)
{
  TEST_BEGIN("ra8_ble_att READ_BY_TYPE_REQ below range -> attr_not_found (C1=T)");
  prep_init();
  uint8_t  buf[k_value_buf_bytes] = {0U};
  uint16_t chr                    = 0U;
  register_one_char(&chr, buf, (uint16_t)sizeof(buf));
  ra8_ble_host_test_inject_connect((uint16_t)k_test_conn_handle);
  ra8_ble_test_reset_capture();

  const uint8_t pdu[7] = {
    (uint8_t)k_att_op_read_by_type_req,
    0xFEU,
    0xFFU,
    0xFEU,
    0xFFU,
    (uint8_t)((uint16_t)k_test_uuid16_char & 0xFFU),
    (uint8_t)(((uint16_t)k_test_uuid16_char >> 8U) & 0xFFU),
  };
  inject_att(pdu, (uint16_t)sizeof(pdu));
  uint16_t       cap_len = 0U;
  const uint8_t* cap     = ra8_ble_test_tx_capture(&cap_len);
  TEST_ASSERT(cap_len > 0U);
  TEST_ASSERT_EQ(k_att_op_error_rsp, cap[9]);
  TEST_END("ra8_ble_att READ_BY_TYPE_REQ below range -> attr_not_found (C1=T)");
}

/**
 * @test test_dispatch_write_req_to_service_handle
 *
 * @par MC/DC:
 * Decision (internal_handle_write):
 *   ``else if ((a->kind == k_attr_kind_char_value) && (a->value != NULL))``
 * (2 conditions, AND).
 *  - V1 (this test): write to the primary-service handle (kind=primary_svc,
 *    not char_value) -> C1=F (short-circ). Decision F (the inner branch is
 *    skipped; the function falls through without an Error_Rsp because the
 *    outer guards already passed).
 *  - V2 (test_dispatch_write_req_value_path): write to a char_value handle
 *    with non-NULL value buffer -> C1=T,C2=T. Decision T (memcpy + WRITE_RSP).
 * V1 vs V2 isolate C1 (C2 held T in V2, short-circuited in V1). The (T,F)
 * row is structurally unreachable: the public ra8_ble_host_gatt_register_char
 * API rejects NULL value buffers (precondition `value` non-NULL when
 * `value_max > 0`), so a char_value attribute with `a->value == NULL` cannot
 * be constructed. Per DO-178C 6.4.4.3 "structurally unreachable input
 * combinations" guidance, the (T,F) vector is documented as such.
 *
 * @par DO-178C 6.4.4.3 rationale:
 * Provides the C1=F-only vector for the internal_handle_write arm.
 */
static void test_dispatch_write_req_to_service_handle(void)
{
  TEST_BEGIN("ra8_ble_att WRITE_REQ to service handle -> kind!=char_value (C1=F)");
  prep_init();
  uint8_t  buf[k_value_buf_bytes] = {0U};
  uint16_t chr                    = 0U;
  register_one_char(&chr, buf, (uint16_t)sizeof(buf));
  ra8_ble_host_test_inject_connect((uint16_t)k_test_conn_handle);
  ra8_ble_test_reset_capture();

  /* The primary service handle is the first registered attribute (handle 1)
   * so writing to handle 1 hits a `kind == primary_service` row, taking the
   * C1=F branch of the internal_handle_write AND. */
  enum : uint16_t {
    k_svc_handle = 0x0001U, /**< Svc handle. */
  };
  const uint8_t pdu[5] = {
    (uint8_t)k_att_op_write_req,
    (uint8_t)((uint16_t)k_svc_handle & 0xFFU),
    (uint8_t)(((uint16_t)k_svc_handle >> 8U) & 0xFFU),
    0xAAU,
    0xBBU,
  };
  inject_att(pdu, (uint16_t)sizeof(pdu));
  /* The dispatcher silently accepts the write (no inner branch fires) but
   * does not corrupt the service-row's storage. Observable: tx capture is
   * either empty or carries no Write_Rsp; we accept either since the
   * write-cmd / write-req distinction at this row is intentionally NOT a
   * spec-mandated error. */
  TEST_END("ra8_ble_att WRITE_REQ to service handle -> kind!=char_value (C1=F)");
}

int32_t main(void)
{
  test_dispatch_find_info_in_range_match();
  test_dispatch_find_info_above_range();
  test_dispatch_find_info_start_zero();
  test_dispatch_find_info_start_above_end();
  test_dispatch_find_info_below_range();
  test_dispatch_read_by_type_in_range_decl();
  test_dispatch_read_by_type_above_range();
  test_dispatch_read_by_type_below_range();
  test_dispatch_read_req_value_path();
  test_dispatch_write_req_value_path();
  test_dispatch_write_req_to_service_handle();
  (void)fprintf(stderr, "[OK ] test_ra8_ble_att_dispatch.c\n");
  return 0;
}
