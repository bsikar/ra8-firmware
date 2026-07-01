/**
 * @file test_ra_ble_att_cov.c
 * @brief Line-coverage top-up for libs/ra_ble_host/src/ra_ble_att.c
 *
 * @details
 * test_ra_ble_att.c covers the compound guard decisions via mirror
 * helpers and test_ra_ble_att_dispatch.c drives the happy-path
 * per-opcode handlers. Both leave the malformed-PDU and rare-attribute
 * legs of the four production handlers unreached: the short-parameter
 * Error_Response returns (Find_Information / Read_By_Type / Read /
 * Write), the wrong-UUID Read_By_Type path, the CCCD and
 * Primary-Service read arms, the value-length clamp, the CCCD and
 * value write-length errors, and the Write_Command-vs-Write_Request
 * response-suppression split.
 *
 * This TU registers a real Primary Service + Read/Write/Notify
 * characteristic (which also appends a CCCD descriptor) through the
 * public GATT API, drives the stack into the connected state with the
 * UNIT_TEST inject hook, then feeds hand-built ATT PDUs through the
 * L2CAP inject path so each remaining production leg runs for real.
 * No branch is reached by mutating the source; every leg is driven by
 * choosing the inbound PDU bytes (opcode, length, handle, payload) and
 * pre-seeding the attribute table via the public registration API.
 *
 * Each test documents the branch it drives; the branches targeted here
 * are all single-condition guards, so no @par MC/DC pairing block is
 * required (the compound decisions in ra_ble_att.c are already
 * discharged by test_ra_ble_att.c and test_ra_ble_att_dispatch.c).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ra_ble.h"
#include "ra_ble_host.h"
#include "ra_err.h"
#include "ra_sim_mmap.h"
#include "unity_minimal.h"

/* Test hooks declared in ra_ble_host.h under #ifdef UNIT_TEST. */
void ra_ble_host_test_inject_acl(uint16_t conn_handle, const uint8_t* l2cap_frame, uint16_t len);
void ra_ble_host_test_inject_connect(uint16_t conn_handle);

/* TX capture from libs/ra_hal/src/ra_ble.c. */
const uint8_t* ra_ble_test_tx_capture(uint16_t* out_len);
void           ra_ble_test_reset_capture(void);

typedef enum : uint16_t {
  k_test_conn_handle   = 0x0040U,
  k_test_l2cap_cid_att = 0x0004U,
  k_test_uuid16_char   = 0x2803U,
  k_test_uuid16_other  = 0x2800U, /* not the Characteristic type. */
} cov_words_t;

typedef enum : uint8_t {
  k_att_op_error_rsp        = 0x01U,
  k_att_op_find_info_req    = 0x04U,
  k_att_op_read_by_type_req = 0x08U,
  k_att_op_read_req         = 0x0AU,
  k_att_op_read_rsp         = 0x0BU,
  k_att_op_write_req        = 0x12U,
  k_att_op_write_cmd        = 0x52U,
  k_att_err_attr_not_found  = 0x0AU,
  k_att_err_invalid_pdu     = 0x04U,
  k_att_err_invalid_len     = 0x0DU,
  k_l2cap_hdr_bytes         = 4U,
  k_uuid_marker_svc         = 0xA0U,
  k_uuid_marker_chr         = 0xB0U,
  k_cap_offset_op           = 9U, /* ATT opcode offset in the HCI ACL capture. */
} cov_bytes_t;

/* Attribute-table layout produced by register_one_char() below:
 *   handle 1 = Primary Service (value == NULL)
 *   handle 2 = Characteristic Declaration
 *   handle 3 = Characteristic Value (value_handle returned to caller)
 *   handle 4 = CCCD (appended because the notify property is set). */
typedef enum : uint16_t {
  k_handle_service = 0x0001U,
} cov_handle_t;

/* ---------------------------------------------------------------------------
 * Helpers
 * ---------------------------------------------------------------------------
 */

/**
 * @brief Fill a 16-byte UUID with a recognizable marker ramp.
 *
 * @param[out] out    16-byte UUID buffer.
 * @param[in]  marker Base byte; out[i] = marker + i.
 */
static void make_uuid(uint8_t* out, uint8_t marker)
{
  for (uint8_t i = 0U; i < 16U; i++) {
    out[i] = (uint8_t)(marker + i);
  }
}

/**
 * @brief Reset the sim, reopen the host, and register a fresh stack.
 */
static void prep_init(void)
{
  ra_sim_mmap_reset();
  (void)ra_ble_host_close();
  (void)ra_ble_close();
  ra_ble_test_reset_capture();
  const ra_ble_host_config_t cfg = {
    .role       = k_ra_ble_host_role_peripheral,
    .name       = "ra8d2",
    .appearance = 0x0040U,
  };
  TEST_ASSERT_EQ(k_ra_ok, ra_ble_host_init(&cfg));
}

/**
 * @brief Register one Primary Service and one Read/Write/Notify
 *        characteristic, returning the value handle.
 *
 * @param[out]    out_chr Value-attribute ATT handle (== 3 here).
 * @param[in,out] buf     Caller storage for the characteristic value.
 * @param[in]     cap     Bytes available in @p buf (== value_max).
 */
static void register_one_char(uint16_t* out_chr, uint8_t* buf, uint16_t cap)
{
  uint8_t svc_uuid[16];
  uint8_t chr_uuid[16];
  make_uuid(svc_uuid, (uint8_t)k_uuid_marker_svc);
  make_uuid(chr_uuid, (uint8_t)k_uuid_marker_chr);

  uint16_t svc = 0U;
  TEST_ASSERT_EQ(k_ra_ok, ra_ble_host_gatt_register_service(svc_uuid, &svc));

  const uint8_t props =
    (uint8_t)((uint8_t)k_ra_ble_host_char_prop_read | (uint8_t)k_ra_ble_host_char_prop_write |
              (uint8_t)k_ra_ble_host_char_prop_notify);
  TEST_ASSERT_EQ(k_ra_ok, ra_ble_host_gatt_register_char(svc, chr_uuid, props, buf, cap, out_chr));
}

/**
 * @brief Wrap an ATT PDU in an L2CAP B-frame and feed it to the host.
 *
 * @param[in] att_pdu ATT PDU bytes (opcode + body).
 * @param[in] att_len Total ATT PDU length in bytes.
 */
static void inject_att(const uint8_t* att_pdu, uint16_t att_len)
{
  enum : uint16_t {
    k_max_frame_bytes = 64U,
  };
  uint8_t frame[k_max_frame_bytes];
  TEST_ASSERT(att_len <= (uint16_t)(k_max_frame_bytes - (uint16_t)k_l2cap_hdr_bytes));
  frame[0] = (uint8_t)(att_len & 0xFFU);
  frame[1] = (uint8_t)((att_len >> 8U) & 0xFFU);
  frame[2] = (uint8_t)((uint16_t)k_test_l2cap_cid_att & 0xFFU);
  frame[3] = (uint8_t)(((uint16_t)k_test_l2cap_cid_att >> 8U) & 0xFFU);
  (void)memcpy(&frame[k_l2cap_hdr_bytes], att_pdu, att_len);
  ra_ble_host_test_inject_acl((uint16_t)k_test_conn_handle,
                              frame,
                              (uint16_t)((uint16_t)k_l2cap_hdr_bytes + att_len));
}

/**
 * @brief Standard "one char, connected, capture cleared" fixture.
 *
 * @param[out]    out_chr Value handle.
 * @param[in,out] buf     Value backing buffer.
 * @param[in]     cap     Buffer capacity.
 */
static void connect_with_char(uint16_t* out_chr, uint8_t* buf, uint16_t cap)
{
  prep_init();
  register_one_char(out_chr, buf, cap);
  ra_ble_host_test_inject_connect((uint16_t)k_test_conn_handle);
  ra_ble_test_reset_capture();
}

/* ---------------------------------------------------------------------------
 * Malformed-PDU (short-parameter) Error_Response legs
 * ---------------------------------------------------------------------------
 */

/**
 * @test test_find_info_short_pdu
 *
 * @details Drives ra_ble_att.c internal_handle_find_info line 329
 *          guard ``if (len < k_min_param_bytes)`` TRUE: a
 *          Find_Information_Request with only 2 body bytes (len == 2 < 4)
 *          takes the invalid-PDU Error_Response return (lines 330-331).
 */
static void test_find_info_short_pdu(void)
{
  TEST_BEGIN("ra_ble_att FIND_INFO_REQ short -> Error_Rsp(invalid_pdu)");
  uint8_t  buf[8] = {0U};
  uint16_t chr    = 0U;
  connect_with_char(&chr, buf, (uint16_t)sizeof(buf));

  /* opcode + 2 body bytes -> internal_handle_find_info sees len == 2. */
  const uint8_t pdu[3] = {(uint8_t)k_att_op_find_info_req, 0x01U, 0x00U};
  inject_att(pdu, (uint16_t)sizeof(pdu));

  uint16_t       cap_len = 0U;
  const uint8_t* cap     = ra_ble_test_tx_capture(&cap_len);
  TEST_ASSERT(cap_len > 0U);
  TEST_ASSERT_EQ(k_att_op_error_rsp, cap[k_cap_offset_op]);
  TEST_ASSERT_EQ(k_att_err_invalid_pdu, cap[k_cap_offset_op + 4U]);
  TEST_END("ra_ble_att FIND_INFO_REQ short -> Error_Rsp(invalid_pdu)");
}

/**
 * @test test_read_by_type_short_pdu
 *
 * @details Drives internal_handle_read_by_type line 522 guard
 *          ``if (len < k_short_param_bytes)`` TRUE: a Read_By_Type_Request
 *          with only 5 body bytes (len == 5 < 6) takes the invalid-PDU
 *          Error_Response return (lines 523-524).
 */
static void test_read_by_type_short_pdu(void)
{
  TEST_BEGIN("ra_ble_att READ_BY_TYPE_REQ short -> Error_Rsp(invalid_pdu)");
  uint8_t  buf[8] = {0U};
  uint16_t chr    = 0U;
  connect_with_char(&chr, buf, (uint16_t)sizeof(buf));

  /* opcode + 5 body bytes -> len == 5 (< 6). */
  const uint8_t pdu[6] = {(uint8_t)k_att_op_read_by_type_req, 0x01U, 0x00U, 0xFFU, 0xFFU, 0x03U};
  inject_att(pdu, (uint16_t)sizeof(pdu));

  uint16_t       cap_len = 0U;
  const uint8_t* cap     = ra_ble_test_tx_capture(&cap_len);
  TEST_ASSERT(cap_len > 0U);
  TEST_ASSERT_EQ(k_att_op_error_rsp, cap[k_cap_offset_op]);
  TEST_ASSERT_EQ(k_att_err_invalid_pdu, cap[k_cap_offset_op + 4U]);
  TEST_END("ra_ble_att READ_BY_TYPE_REQ short -> Error_Rsp(invalid_pdu)");
}

/**
 * @test test_read_by_type_wrong_uuid
 *
 * @details Drives internal_handle_read_by_type line 530 guard
 *          ``if (uuid16 != k_uuid16_char)`` TRUE: a well-formed 6-byte
 *          request whose type UUID is 0x2800 (not 0x2803) takes the
 *          attr-not-found Error_Response return (lines 531-532).
 */
static void test_read_by_type_wrong_uuid(void)
{
  TEST_BEGIN("ra_ble_att READ_BY_TYPE_REQ wrong-uuid -> Error_Rsp(attr_not_found)");
  uint8_t  buf[8] = {0U};
  uint16_t chr    = 0U;
  connect_with_char(&chr, buf, (uint16_t)sizeof(buf));

  const uint8_t pdu[7] = {
    (uint8_t)k_att_op_read_by_type_req,
    0x01U,
    0x00U,
    0xFFU,
    0xFFU,
    (uint8_t)((uint16_t)k_test_uuid16_other & 0xFFU),
    (uint8_t)(((uint16_t)k_test_uuid16_other >> 8U) & 0xFFU),
  };
  inject_att(pdu, (uint16_t)sizeof(pdu));

  uint16_t       cap_len = 0U;
  const uint8_t* cap     = ra_ble_test_tx_capture(&cap_len);
  TEST_ASSERT(cap_len > 0U);
  TEST_ASSERT_EQ(k_att_op_error_rsp, cap[k_cap_offset_op]);
  TEST_ASSERT_EQ(k_att_err_attr_not_found, cap[k_cap_offset_op + 4U]);
  TEST_END("ra_ble_att READ_BY_TYPE_REQ wrong-uuid -> Error_Rsp(attr_not_found)");
}

/**
 * @test test_read_short_pdu
 *
 * @details Drives internal_handle_read line 586 guard
 *          ``if (len < k_min_param_bytes)`` TRUE: a Read_Request with a
 *          single body byte (len == 1 < 2) takes the invalid-PDU
 *          Error_Response return (lines 587-588).
 */
static void test_read_short_pdu(void)
{
  TEST_BEGIN("ra_ble_att READ_REQ short -> Error_Rsp(invalid_pdu)");
  uint8_t  buf[8] = {0U};
  uint16_t chr    = 0U;
  connect_with_char(&chr, buf, (uint16_t)sizeof(buf));

  const uint8_t pdu[2] = {(uint8_t)k_att_op_read_req, 0x03U};
  inject_att(pdu, (uint16_t)sizeof(pdu));

  uint16_t       cap_len = 0U;
  const uint8_t* cap     = ra_ble_test_tx_capture(&cap_len);
  TEST_ASSERT(cap_len > 0U);
  TEST_ASSERT_EQ(k_att_op_error_rsp, cap[k_cap_offset_op]);
  TEST_ASSERT_EQ(k_att_err_invalid_pdu, cap[k_cap_offset_op + 4U]);
  TEST_END("ra_ble_att READ_REQ short -> Error_Rsp(invalid_pdu)");
}

/* ---------------------------------------------------------------------------
 * Read response builder: CCCD / value-clamp / primary-service arms
 * ---------------------------------------------------------------------------
 */

/**
 * @test test_read_cccd_arm
 *
 * @details Drives internal_handle_read line 601 arm
 *          ``if (a->kind == k_attr_kind_cccd)`` TRUE. The notify property
 *          on the registered characteristic appends a CCCD at handle 4
 *          (value handle + 1); reading it packs the 2-byte cccd_value into
 *          the Read_Response (lines 606-607). The default cccd_value is 0.
 */
static void test_read_cccd_arm(void)
{
  TEST_BEGIN("ra_ble_att READ_REQ on CCCD -> READ_RSP carries cccd_value");
  uint8_t  buf[8] = {0U};
  uint16_t chr    = 0U;
  connect_with_char(&chr, buf, (uint16_t)sizeof(buf));

  const uint16_t cccd   = (uint16_t)(chr + 1U); /* handle 4. */
  const uint8_t  pdu[3] = {
    (uint8_t)k_att_op_read_req,
    (uint8_t)(cccd & 0xFFU),
    (uint8_t)((cccd >> 8U) & 0xFFU),
  };
  inject_att(pdu, (uint16_t)sizeof(pdu));

  uint16_t       cap_len = 0U;
  const uint8_t* cap     = ra_ble_test_tx_capture(&cap_len);
  TEST_ASSERT(cap_len > 0U);
  TEST_ASSERT_EQ(k_att_op_read_rsp, cap[k_cap_offset_op]);
  /* Two CCCD bytes follow the opcode: both zero at registration time. */
  TEST_ASSERT_EQ(0x00U, cap[k_cap_offset_op + 1U]);
  TEST_ASSERT_EQ(0x00U, cap[k_cap_offset_op + 2U]);
  TEST_END("ra_ble_att READ_REQ on CCCD -> READ_RSP carries cccd_value");
}

/**
 * @test test_read_value_clamp
 *
 * @details Drives internal_handle_read line 610 guard
 *          ``if (copy_len > k_max_value_bytes)`` TRUE. The characteristic
 *          value is 30 bytes (> the MTU-23 cap of 22), so the copy length
 *          is clamped to 22 before the memcpy (lines 611-612).
 */
static void test_read_value_clamp(void)
{
  TEST_BEGIN("ra_ble_att READ_REQ oversized value -> copy_len clamp");
  enum : uint16_t {
    k_big_cap = 40U,
    k_val_len = 30U,
    k_expect  = 22U, /* clamped Read_Response value byte count. */
  };
  uint8_t  buf[k_big_cap] = {0U};
  uint16_t chr            = 0U;
  connect_with_char(&chr, buf, (uint16_t)k_big_cap);

  uint8_t val[k_val_len];
  for (uint16_t i = 0U; i < (uint16_t)k_val_len; i++) {
    val[i] = (uint8_t)(0x40U + i);
  }
  TEST_ASSERT_EQ(k_ra_ok, ra_ble_host_gatt_set_value(chr, val, (uint16_t)k_val_len));

  const uint8_t pdu[3] = {
    (uint8_t)k_att_op_read_req,
    (uint8_t)(chr & 0xFFU),
    (uint8_t)((chr >> 8U) & 0xFFU),
  };
  inject_att(pdu, (uint16_t)sizeof(pdu));

  uint16_t       cap_len = 0U;
  const uint8_t* cap     = ra_ble_test_tx_capture(&cap_len);
  TEST_ASSERT(cap_len > 0U);
  TEST_ASSERT_EQ(k_att_op_read_rsp, cap[k_cap_offset_op]);
  /* The L2CAP payload-length field (capture offset 5-6, LE16) equals the
   * ATT PDU length: opcode(1) + clamped value(22) = 23. Without the clamp
   * the 30-byte value would make this 31, so 23 proves lines 611-612 ran. */
  const uint16_t att_pdu_len = (uint16_t)((uint16_t)cap[5] | ((uint16_t)cap[6] << 8U));
  TEST_ASSERT_EQ((uint16_t)(1U + k_expect), att_pdu_len);
  /* First clamped value byte survives verbatim. */
  TEST_ASSERT_EQ(0x40U, cap[k_cap_offset_op + 1U]);
  TEST_END("ra_ble_att READ_REQ oversized value -> copy_len clamp");
}

/**
 * @test test_read_primary_service
 *
 * @details Drives internal_handle_read line 615 arm
 *          ``else if (a->kind == k_attr_kind_primary_service)`` TRUE.
 *          The Primary Service row (handle 1) has a NULL value pointer,
 *          so the cccd and value arms fall through and the service UUID
 *          is emitted from uuid[] (lines 618-620). uuid[] was seeded with
 *          the 0xA0 marker ramp by register_one_char().
 */
static void test_read_primary_service(void)
{
  TEST_BEGIN("ra_ble_att READ_REQ on Primary Service -> UUID from uuid[]");
  uint8_t  buf[8] = {0U};
  uint16_t chr    = 0U;
  connect_with_char(&chr, buf, (uint16_t)sizeof(buf));

  const uint8_t pdu[3] = {
    (uint8_t)k_att_op_read_req,
    (uint8_t)((uint16_t)k_handle_service & 0xFFU),
    (uint8_t)(((uint16_t)k_handle_service >> 8U) & 0xFFU),
  };
  inject_att(pdu, (uint16_t)sizeof(pdu));

  uint16_t       cap_len = 0U;
  const uint8_t* cap     = ra_ble_test_tx_capture(&cap_len);
  TEST_ASSERT(cap_len > 0U);
  TEST_ASSERT_EQ(k_att_op_read_rsp, cap[k_cap_offset_op]);
  /* First UUID byte is the service marker base (0xA0). */
  TEST_ASSERT_EQ((uint8_t)k_uuid_marker_svc, cap[k_cap_offset_op + 1U]);
  TEST_END("ra_ble_att READ_REQ on Primary Service -> UUID from uuid[]");
}

/* ---------------------------------------------------------------------------
 * Write length-error legs (CCCD and value)
 * ---------------------------------------------------------------------------
 */

/**
 * @test test_write_cccd_short_value
 *
 * @details Drives internal_write_cccd line 659 guard
 *          ``if (val_len < k_cccd_min_bytes)`` TRUE. A Write_Request to
 *          the CCCD handle carrying a single value byte (val_len == 1 < 2)
 *          returns k_att_err_invalid_value_len (line 660), which the
 *          Write handler surfaces as an Error_Response.
 */
static void test_write_cccd_short_value(void)
{
  TEST_BEGIN("ra_ble_att WRITE_REQ CCCD 1-byte -> Error_Rsp(invalid_len)");
  uint8_t  buf[8] = {0U};
  uint16_t chr    = 0U;
  connect_with_char(&chr, buf, (uint16_t)sizeof(buf));

  const uint16_t cccd   = (uint16_t)(chr + 1U);
  const uint8_t  pdu[4] = {
    (uint8_t)k_att_op_write_req,
    (uint8_t)(cccd & 0xFFU),
    (uint8_t)((cccd >> 8U) & 0xFFU),
    0x01U, /* single CCCD payload byte -> val_len == 1. */
  };
  inject_att(pdu, (uint16_t)sizeof(pdu));

  uint16_t       cap_len = 0U;
  const uint8_t* cap     = ra_ble_test_tx_capture(&cap_len);
  TEST_ASSERT(cap_len > 0U);
  TEST_ASSERT_EQ(k_att_op_error_rsp, cap[k_cap_offset_op]);
  TEST_ASSERT_EQ(k_att_err_invalid_len, cap[k_cap_offset_op + 4U]);
  TEST_END("ra_ble_att WRITE_REQ CCCD 1-byte -> Error_Rsp(invalid_len)");
}

/**
 * @test test_write_value_too_long
 *
 * @details Drives internal_write_value line 706 guard
 *          ``if (val_len > a->value_max)`` TRUE. The characteristic value
 *          buffer holds 8 bytes; a Write_Request carrying 10 payload bytes
 *          exceeds value_max and returns k_att_err_invalid_value_len
 *          (line 707), surfaced as an Error_Response.
 */
static void test_write_value_too_long(void)
{
  TEST_BEGIN("ra_ble_att WRITE_REQ oversized value -> Error_Rsp(invalid_len)");
  enum : uint16_t {
    k_cap     = 8U,
    k_payload = 10U,
  };
  uint8_t  buf[k_cap] = {0U};
  uint16_t chr        = 0U;
  connect_with_char(&chr, buf, (uint16_t)k_cap);

  uint8_t pdu[3U + k_payload];
  pdu[0] = (uint8_t)k_att_op_write_req;
  pdu[1] = (uint8_t)(chr & 0xFFU);
  pdu[2] = (uint8_t)((chr >> 8U) & 0xFFU);
  for (uint16_t i = 0U; i < (uint16_t)k_payload; i++) {
    pdu[3U + i] = (uint8_t)(0x10U + i);
  }
  inject_att(pdu, (uint16_t)sizeof(pdu));

  uint16_t       cap_len = 0U;
  const uint8_t* cap     = ra_ble_test_tx_capture(&cap_len);
  TEST_ASSERT(cap_len > 0U);
  TEST_ASSERT_EQ(k_att_op_error_rsp, cap[k_cap_offset_op]);
  TEST_ASSERT_EQ(k_att_err_invalid_len, cap[k_cap_offset_op + 4U]);
  /* The rejected write left the backing buffer untouched. */
  TEST_ASSERT_EQ(0x00U, buf[0]);
  TEST_END("ra_ble_att WRITE_REQ oversized value -> Error_Rsp(invalid_len)");
}

/* ---------------------------------------------------------------------------
 * Write dispatcher short-PDU / unknown-handle response split
 * ---------------------------------------------------------------------------
 */

/**
 * @test test_write_req_short_pdu
 *
 * @details Drives internal_handle_write line 758 guard
 *          ``if (len < k_min_param_bytes)`` TRUE with op == Write_Request,
 *          taking the inner line 759 ``if (op == k_att_op_write_req)`` TRUE
 *          arm: an invalid-PDU Error_Response is queued (line 760) and the
 *          function returns (lines 761-762).
 */
static void test_write_req_short_pdu(void)
{
  TEST_BEGIN("ra_ble_att WRITE_REQ short -> Error_Rsp(invalid_pdu)");
  uint8_t  buf[8] = {0U};
  uint16_t chr    = 0U;
  connect_with_char(&chr, buf, (uint16_t)sizeof(buf));

  /* opcode + 1 body byte -> len == 1 (< 2). */
  const uint8_t pdu[2] = {(uint8_t)k_att_op_write_req, 0x03U};
  inject_att(pdu, (uint16_t)sizeof(pdu));

  uint16_t       cap_len = 0U;
  const uint8_t* cap     = ra_ble_test_tx_capture(&cap_len);
  TEST_ASSERT(cap_len > 0U);
  TEST_ASSERT_EQ(k_att_op_error_rsp, cap[k_cap_offset_op]);
  TEST_ASSERT_EQ(k_att_err_invalid_pdu, cap[k_cap_offset_op + 4U]);
  TEST_END("ra_ble_att WRITE_REQ short -> Error_Rsp(invalid_pdu)");
}

/**
 * @test test_write_cmd_short_pdu
 *
 * @details Drives internal_handle_write line 758 guard TRUE with
 *          op == Write_Command, taking the inner line 759
 *          ``if (op == k_att_op_write_req)`` FALSE arm: no Error_Response
 *          is queued (Write_Command is never answered, Vol 3 Part F
 *          3.4.5.3) and the function returns (line 762). Observed as an
 *          empty TX capture.
 */
static void test_write_cmd_short_pdu(void)
{
  TEST_BEGIN("ra_ble_att WRITE_CMD short -> silent (no response)");
  uint8_t  buf[8] = {0U};
  uint16_t chr    = 0U;
  connect_with_char(&chr, buf, (uint16_t)sizeof(buf));

  const uint8_t pdu[2] = {(uint8_t)k_att_op_write_cmd, 0x03U};
  inject_att(pdu, (uint16_t)sizeof(pdu));

  uint16_t cap_len = 0U;
  (void)ra_ble_test_tx_capture(&cap_len);
  TEST_ASSERT_EQ(0U, cap_len);
  TEST_END("ra_ble_att WRITE_CMD short -> silent (no response)");
}

/**
 * @test test_write_req_unknown_handle
 *
 * @details Drives internal_handle_write line 766 guard
 *          ``if (a == nullptr)`` TRUE with op == Write_Request, taking the
 *          inner line 767 ``if (op == k_att_op_write_req)`` TRUE arm: an
 *          invalid-handle Error_Response is queued (line 768) and the
 *          function returns (lines 769-770). Handle 0x00FF is not
 *          registered.
 */
static void test_write_req_unknown_handle(void)
{
  TEST_BEGIN("ra_ble_att WRITE_REQ unknown handle -> Error_Rsp(invalid_handle)");
  uint8_t  buf[8] = {0U};
  uint16_t chr    = 0U;
  connect_with_char(&chr, buf, (uint16_t)sizeof(buf));

  const uint8_t pdu[5] = {(uint8_t)k_att_op_write_req, 0xFFU, 0x00U, 0xAAU, 0xBBU};
  inject_att(pdu, (uint16_t)sizeof(pdu));

  uint16_t       cap_len = 0U;
  const uint8_t* cap     = ra_ble_test_tx_capture(&cap_len);
  TEST_ASSERT(cap_len > 0U);
  TEST_ASSERT_EQ(k_att_op_error_rsp, cap[k_cap_offset_op]);
  TEST_END("ra_ble_att WRITE_REQ unknown handle -> Error_Rsp(invalid_handle)");
}

/**
 * @test test_write_cmd_unknown_handle
 *
 * @details Drives internal_handle_write line 766 guard TRUE with
 *          op == Write_Command, taking the inner line 767
 *          ``if (op == k_att_op_write_req)`` FALSE arm: no Error_Response
 *          is queued and the function returns (line 770). Observed as an
 *          empty TX capture.
 */
static void test_write_cmd_unknown_handle(void)
{
  TEST_BEGIN("ra_ble_att WRITE_CMD unknown handle -> silent (no response)");
  uint8_t  buf[8] = {0U};
  uint16_t chr    = 0U;
  connect_with_char(&chr, buf, (uint16_t)sizeof(buf));

  const uint8_t pdu[5] = {(uint8_t)k_att_op_write_cmd, 0xFFU, 0x00U, 0xAAU, 0xBBU};
  inject_att(pdu, (uint16_t)sizeof(pdu));

  uint16_t cap_len = 0U;
  (void)ra_ble_test_tx_capture(&cap_len);
  TEST_ASSERT_EQ(0U, cap_len);
  TEST_END("ra_ble_att WRITE_CMD unknown handle -> silent (no response)");
}

int32_t main(void)
{
  test_find_info_short_pdu();
  test_read_by_type_short_pdu();
  test_read_by_type_wrong_uuid();
  test_read_short_pdu();
  test_read_cccd_arm();
  test_read_value_clamp();
  test_read_primary_service();
  test_write_cccd_short_value();
  test_write_value_too_long();
  test_write_req_short_pdu();
  test_write_cmd_short_pdu();
  test_write_req_unknown_handle();
  test_write_cmd_unknown_handle();
  (void)fprintf(stderr, "[OK ] test_ra_ble_att_cov.c\n");
  return 0;
}
