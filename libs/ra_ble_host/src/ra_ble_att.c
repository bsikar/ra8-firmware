/**
 * @file ra_ble_att.c
 * @brief ATT (Attribute Protocol) PDU handling for the BLE host
 *
 * @details
 * Implements a tiny subset of Bluetooth Core 5.3 Vol 3 Part F. The
 * server side handles these PDUs:
 *
 *   | Opcode | Name                          | Spec section |
 *   |--------|-------------------------------|--------------|
 *   | 0x04   | Find_Information_Request      | 3.4.3.1      |
 *   | 0x08   | Read_By_Type_Request          | 3.4.4.1      |
 *   | 0x0A   | Read_Request                  | 3.4.4.3      |
 *   | 0x12   | Write_Request                 | 3.4.5.1      |
 *   | 0x52   | Write_Command                 | 3.4.5.3      |
 *   | 0x1B   | Handle_Value_Notification (TX)| 3.4.7.1      |
 *
 * Anything else gets a generic Error_Response (opcode 0x01) with
 * the "Request Not Supported" error code, per Vol 3 Part F 3.4.1.1.
 *
 * The attribute table is owned by ra_ble_l2cap.c. This TU reaches
 * into it via ra_ble_host_state(), the small accessor exposed for
 * the three host TUs to share singleton state.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

// NOLINTBEGIN(readability-magic-numbers,readability-function-size,readability-function-cognitive-complexity)
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ra_ble_host.h"
#include "ra_err.h"

/* =============================================================================
 * Forward declarations of state shared with ra_ble_l2cap.c.
 * =============================================================================
 */

/**
 * @struct ra_ble_host_attr_t
 * @brief Forward-declared mirror of the attribute-table row defined in
 *        ra_ble_l2cap.c. The full layout is in that file.
 */
typedef struct ra_ble_host_attr_t ra_ble_host_attr_t;

typedef enum : uint8_t {
  k_attr_kind_primary_service = 0U,
  k_attr_kind_char_decl       = 1U,
  k_attr_kind_char_value      = 2U,
  k_attr_kind_cccd            = 3U,
} ra_ble_host_attr_kind_t;

struct ra_ble_host_attr_t {
  uint16_t                handle;
  ra_ble_host_attr_kind_t kind;
  uint8_t                 props;
  uint8_t                 uuid[k_ra_ble_host_uuid_bytes];
  uint8_t*                value;
  uint16_t                value_len;
  uint16_t                value_max;
  uint16_t                cccd_value;
  uint16_t                value_handle_owner;
};

/**
 * @struct ra_ble_host_state_t
 * @brief Forward-declared mirror of the singleton state struct.
 *
 * @details Field order MUST match ra_ble_l2cap.c.
 */
typedef struct {
  uint8_t                initialized;
  ra_ble_host_role_t     role;
  uint16_t               appearance;
  char                   name[32];
  uint16_t               next_handle;
  uint16_t               last_service_handle;
  ra_ble_host_attr_t     attrs[96];
  uint8_t                attr_count;
  uint8_t                service_count;
  uint8_t                char_count;
  uint16_t               conn_handle;
  uint16_t               att_mtu;
  uint8_t                reassembly[256];
  uint16_t               reassembly_len;
  uint16_t               reassembly_expected;
  uint16_t               reassembly_cid;
  uint16_t               reassembly_conn;
  ra_ble_host_event_fn_t evt_fn;
  void*                  evt_ctx;
  uint32_t               evt_count;
} ra_ble_host_state_t;

ra_ble_host_state_t* ra_ble_host_state(void);
void                 ra_ble_host_dispatch_event(const ra_ble_host_event_t* evt);
ra_err_t             ra_ble_host_l2cap_send(uint16_t       conn_handle,
                                            uint16_t       cid,
                                            const uint8_t* payload,
                                            uint16_t       payload_len);
void ra_ble_host_att_handle_pdu(uint16_t conn_handle, const uint8_t* pdu, uint16_t pdu_len);
ra_ble_host_attr_t* ra_ble_host_attr_lookup(uint16_t handle);

/* =============================================================================
 * ATT opcodes / error codes
 * =============================================================================
 */

/**
 * @enum ra_ble_att_op_t
 * @brief ATT PDU opcodes (Bluetooth Core 5.3 Vol 3 Part F 3.4.8 Table 3.37).
 */
typedef enum : uint8_t {
  k_att_op_error_rsp           = 0x01U, /**< 3.4.1.1.   */
  k_att_op_find_info_req       = 0x04U, /**< 3.4.3.1.   */
  k_att_op_find_info_rsp       = 0x05U, /**< 3.4.3.2.   */
  k_att_op_read_by_type_req    = 0x08U, /**< 3.4.4.1.   */
  k_att_op_read_by_type_rsp    = 0x09U, /**< 3.4.4.2.   */
  k_att_op_read_req            = 0x0AU, /**< 3.4.4.3.   */
  k_att_op_read_rsp            = 0x0BU, /**< 3.4.4.4.   */
  k_att_op_write_req           = 0x12U, /**< 3.4.5.1.   */
  k_att_op_write_rsp           = 0x13U, /**< 3.4.5.2.   */
  k_att_op_handle_value_notify = 0x1BU, /**< 3.4.7.1.   */
  k_att_op_write_cmd           = 0x52U, /**< 3.4.5.3.   */
} ra_ble_att_op_t;

/**
 * @enum ra_ble_att_err_t
 * @brief ATT error codes (Bluetooth Core 5.3 Vol 3 Part F 3.4.1.1
 *        Table 3.4 + Vol 3 Part F 3.4.1).
 */
typedef enum : uint8_t {
  k_att_err_invalid_handle      = 0x01U,
  k_att_err_read_not_permitted  = 0x02U,
  k_att_err_write_not_permitted = 0x03U,
  k_att_err_invalid_pdu         = 0x04U,
  k_att_err_request_not_supp    = 0x06U,
  k_att_err_attr_not_found      = 0x0AU,
  k_att_err_invalid_value_len   = 0x0DU,
} ra_ble_att_err_t;

/* =============================================================================
 * Helpers
 * =============================================================================
 */

/**
 * @brief Pack a little-endian uint16.
 *
 * @details Writes the low byte of ``v`` to ``dst[0]`` and the high
 *          byte to ``dst[1]``. This is the byte order used throughout
 *          the L2CAP / ATT wire format (Bluetooth Core 5.3 Vol 3
 *          Part A 3 "Data Packet Format").
 *
 * @param[out] dst Two-byte destination. Must not be NULL.
 * @param[in]  v   Value.
 *
 * @pre dst != NULL.
 * @pre dst points to at least 2 writable bytes.
 * @post dst[0..1] holds the LE16 encoding of v.
 * @post No other memory is mutated.
 *
 * @note Not thread-safe; caller serializes access to dst.
 *
 * @since 0.1.0
 */
static void internal_pack_le16(uint8_t* dst, uint16_t v)
{
  enum : uint8_t {
    k_byte_lo_idx = 0U,
    k_byte_hi_idx = 1U,
    k_byte_shift  = 8U,
    k_byte_mask   = 0xFFU,
  };
  dst[k_byte_lo_idx] = (uint8_t)(v & k_byte_mask);
  dst[k_byte_hi_idx] = (uint8_t)((v >> k_byte_shift) & k_byte_mask);
}

/**
 * @brief Unpack a little-endian uint16.
 *
 * @details Reads the LE16 layout used throughout the L2CAP / ATT wire
 *          format (Bluetooth Core 5.3 Vol 3 Part A 3).
 *
 * @param[in] src Two-byte source. Must not be NULL.
 * @return Unpacked value.
 * @retval 0     src[0] and src[1] are both zero.
 * @retval other Combined LE16 value.
 *
 * @pre src != NULL.
 * @pre src points to at least 2 readable bytes.
 * @post No memory is modified.
 * @post Return value equals src[0] | (src[1] << 8).
 *
 * @note Not thread-safe; caller serializes access to src.
 *
 * @since 0.1.0
 */
static uint16_t internal_unpack_le16(const uint8_t* src)
{
  enum : uint8_t {
    k_byte_lo_idx = 0U,
    k_byte_hi_idx = 1U,
    k_byte_shift  = 8U,
  };
  return (uint16_t)((uint16_t)src[k_byte_lo_idx] | ((uint16_t)src[k_byte_hi_idx] << k_byte_shift));
}

/**
 * @brief Look up the attribute-table row for a given ATT handle.
 *
 * @details Linear scan over the ra_ble_host_state attribute table
 *          (Bluetooth Core 5.3 Vol 3 Part F 3.2 "Attribute").
 *
 * @param[in] handle ATT handle (1-based).
 * @return Pointer to the row, or NULL if the handle is not registered.
 * @retval NULL  No matching row was found, or handle == 0.
 * @retval !NULL Pointer into the singleton attribute table.
 *
 * @pre Stack initialized (ra_ble_host_init succeeded).
 * @pre handle == 0 returns NULL (no precondition violation).
 * @post No state mutation.
 * @post Returned pointer (if non-NULL) points into the host attribute
 *       table.
 *
 * @note Not thread-safe; caller is the host's serial dispatch loop.
 *
 * @since 0.1.0
 */
ra_ble_host_attr_t* ra_ble_host_attr_lookup(uint16_t handle)
{
  ra_ble_host_state_t* st = ra_ble_host_state();
  for (uint8_t i = 0U; i < st->attr_count; i++) {
    if (st->attrs[i].handle == handle) {
      return &st->attrs[i];
    }
  }
  return nullptr;
}

/**
 * @brief Send an ATT Error_Response on CID 0x0004 (LE ATT).
 *
 * @details Builds the 5-byte Error_Response PDU (opcode 0x01) per
 *          Bluetooth Core 5.3 Vol 3 Part F 3.4.1.1 and queues it via
 *          ra_ble_host_l2cap_send.
 *
 * @param[in] conn_handle ACL handle.
 * @param[in] op_in_error Original request opcode.
 * @param[in] handle      Attribute handle that triggered the error.
 * @param[in] err         ATT error code.
 *
 * @pre Connection is live.
 * @pre Stack is initialized.
 * @post Error_Response queued on the HCI ACL TX FIFO.
 * @post Host attribute state is unchanged.
 *
 * @note Not thread-safe; called from the host serial dispatch loop.
 *
 * @since 0.1.0
 */
static void internal_send_error(uint16_t         conn_handle,
                                uint8_t          op_in_error,
                                uint16_t         handle,
                                ra_ble_att_err_t err)
{
  enum : uint16_t {
    k_l2cap_cid_att = 0x0004U,
  };
  enum : uint8_t {
    k_err_pdu_bytes = 5U,
  };
  uint8_t pdu[k_err_pdu_bytes];
  pdu[0] = k_att_op_error_rsp;
  pdu[1] = op_in_error;
  internal_pack_le16(&pdu[2], handle);
  pdu[4] = (uint8_t)err;
  (void)ra_ble_host_l2cap_send(conn_handle, k_l2cap_cid_att, pdu, k_err_pdu_bytes);
}

/* =============================================================================
 * Per-opcode handlers
 * =============================================================================
 */

/**
 * @brief Handle ATT Find_Information_Request (Vol 3 Part F 3.4.3.1).
 *
 * @details
 * Walks the attribute table for handles in [start, end] and emits a
 * Find_Information_Response listing (handle, UUID) pairs. We always
 * use Format == 0x02 (128-bit UUIDs) since we store every attribute
 * type as a 16-byte UUID.
 *
 * @param[in] conn_handle ACL handle.
 * @param[in] pdu         Inbound PDU bytes (already past the opcode).
 * @param[in] len         Bytes after the opcode.
 *
 * @pre pdu is non-NULL when len > 0.
 * @pre Stack is initialized.
 * @post Either a Find_Information_Response or an Error_Response has
 *       been queued.
 * @post Host attribute table is unchanged.
 *
 * @note Not thread-safe; called from the host serial dispatch loop.
 *
 * @since 0.1.0
 */
static void internal_handle_find_info(uint16_t conn_handle, const uint8_t* pdu, uint16_t len)
{
  enum : uint8_t {
    k_min_param_bytes = 4U,
    k_format_128bit   = 0x02U,
    k_pair_bytes      = 18U, /* handle(2) + uuid(16). */
  };
  enum : uint16_t {
    k_l2cap_cid_att     = 0x0004U,
    k_max_pairs_per_pdu = 4U, /* default ATT_MTU = 23 -> 2 + 4*18 = 74 too large; cap at 1. */
  };

  if (len < k_min_param_bytes) {
    internal_send_error(conn_handle, k_att_op_find_info_req, 0U, k_att_err_invalid_pdu);
    return;
  }
  const uint16_t start = internal_unpack_le16(&pdu[0]);
  const uint16_t end   = internal_unpack_le16(&pdu[2]);
  if ((start == 0U) || (start > end)) {
    internal_send_error(conn_handle, k_att_op_find_info_req, start, k_att_err_invalid_handle);
    return;
  }

  ra_ble_host_state_t* st = ra_ble_host_state();
  /* Build response: opcode + format + N x (handle, uuid). At MTU 23 we
   * can fit one 128-bit pair (2 + 18 = 20 bytes payload). */
  enum : uint8_t {
    k_max_resp_bytes = 22U,
  };
  uint8_t  resp[k_max_resp_bytes];
  uint16_t pos = 0U;
  resp[pos++]  = k_att_op_find_info_rsp;
  resp[pos++]  = k_format_128bit;

  uint8_t pairs = 0U;
  for (uint8_t i = 0U; i < st->attr_count; i++) {
    const ra_ble_host_attr_t* a = &st->attrs[i];
    if ((a->handle < start) || (a->handle > end)) {
      continue;
    }
    if (((uint32_t)pos + (uint32_t)k_pair_bytes) > (uint32_t)k_max_resp_bytes) {
      break;
    }
    internal_pack_le16(&resp[pos], a->handle);
    pos = (uint16_t)(pos + 2U);
    (void)memcpy(&resp[pos], a->uuid, k_ra_ble_host_uuid_bytes);
    pos = (uint16_t)(pos + k_ra_ble_host_uuid_bytes);
    pairs++;
    if (pairs >= k_max_pairs_per_pdu) {
      break;
    }
  }
  if (pairs == 0U) {
    internal_send_error(conn_handle, k_att_op_find_info_req, start, k_att_err_attr_not_found);
    return;
  }
  (void)ra_ble_host_l2cap_send(conn_handle, k_l2cap_cid_att, resp, pos);
}

/**
 * @brief Handle ATT Read_By_Type_Request (Vol 3 Part F 3.4.4.1).
 *
 * @details
 * Used by clients during service discovery to read characteristic
 * declarations. We support the 16-bit-UUID short form (request length
 * 7 bytes including opcode) only -- iOS / Android Bluetooth stacks
 * always issue this form for the 0x2803 ("Characteristic") UUID.
 *
 * @param[in] conn_handle ACL handle.
 * @param[in] pdu         Inbound PDU bytes (already past the opcode).
 * @param[in] len         Bytes after the opcode.
 *
 * @pre pdu is non-NULL when len > 0.
 * @pre Stack is initialized.
 * @post Either a Read_By_Type_Response or an Error_Response has been
 *       queued.
 * @post Host attribute table is unchanged.
 *
 * @note Not thread-safe; called from the host serial dispatch loop.
 *
 * @since 0.1.0
 */
/**
 * @brief Append one char-decl pair (handle + decl value) to a
 *        Read_By_Type response buffer.
 *
 * @details Encodes the 21-byte (handle(2) + props(1) + value_handle(2) +
 *          uuid(16)) char-declaration pair per Vol 3 Part G 3.3.
 *
 * @param[out]    resp Response buffer (already populated with header).
 * @param[in,out] pos  Cursor into resp, advanced by 21.
 * @param[in]     a    Char-decl attribute row.
 *
 * @pre resp != NULL, pos != NULL, a != NULL.
 * @pre resp has at least *pos + 21 writable bytes.
 * @post resp[*pos_in .. *pos_out - 1] holds the encoded pair.
 * @post *pos advances by exactly 21 bytes.
 *
 * @note Not thread-safe; caller serializes access to resp.
 *
 * @since 0.1.0
 */
static void internal_emit_char_decl_pair(uint8_t* resp, uint16_t* pos, const ra_ble_host_attr_t* a)
{
  uint16_t p = *pos;
  internal_pack_le16(&resp[p], a->handle);
  p         = (uint16_t)(p + 2U);
  resp[p++] = a->props;
  internal_pack_le16(&resp[p], (uint16_t)(a->handle + 1U));
  p = (uint16_t)(p + 2U);
  (void)memcpy(&resp[p], a->uuid, k_ra_ble_host_uuid_bytes);
  p    = (uint16_t)(p + k_ra_ble_host_uuid_bytes);
  *pos = p;
}

/**
 * @brief Find the first char-decl row in [start, end] and emit it into
 *        the Read_By_Type response buffer.
 *
 * @details Scans the host attribute table; on the first matching row
 *          (kind == char_decl with handle in range) appends the 21-byte
 *          pair and returns true. The MTU-23 cap allows only one pair
 *          per response, so the search terminates after the first hit.
 *
 * @param[out]    resp  Response buffer (pre-populated with 2-byte hdr).
 * @param[in,out] pos   Cursor into resp; advanced by 21 on success.
 * @param[in]     start First handle in the requested range (inclusive).
 * @param[in]     end   Last handle in the requested range (inclusive).
 *
 * @return Boolean emit-success flag.
 * @retval true  A matching row was found and the pair was appended.
 * @retval false No matching row in range.
 *
 * @pre resp != NULL with >= *pos + 21 writable bytes.
 * @pre pos != NULL; Stack initialized.
 * @post On success *pos has advanced by 21; otherwise *pos is unchanged.
 * @post No host state is mutated.
 *
 * @note Not thread-safe; called from the host serial dispatch loop.
 *
 * @since 0.1.0
 */
static bool
internal_emit_first_decl_in_range(uint8_t* resp, uint16_t* pos, uint16_t start, uint16_t end)
{
  enum : uint8_t {
    k_pair_bytes     = 21U,
    k_max_resp_bytes = 23U,
  };
  const ra_ble_host_state_t* st = ra_ble_host_state();
  for (uint8_t i = 0U; i < st->attr_count; i++) {
    const ra_ble_host_attr_t* a = &st->attrs[i];
    if (a->kind != k_attr_kind_char_decl) {
      continue;
    }
    if ((a->handle < start) || (a->handle > end)) {
      continue;
    }
    if (((uint32_t)*pos + (uint32_t)k_pair_bytes) > (uint32_t)k_max_resp_bytes) {
      return false;
    }
    internal_emit_char_decl_pair(resp, pos, a);
    return true;
  }
  return false;
}

/**
 * @brief Handle ATT Read_By_Type_Request for Characteristic declarations.
 *
 * @details Parses the 6-byte parameter block (start-handle, end-handle,
 *          UUID-16). The only attribute type currently supported is the
 *          Characteristic declaration (UUID 0x2803, Vol 3 Part G 3.3.1),
 *          which is the GATT-discovery path most LE peers exercise.
 *          Locates the first matching declaration row in range, emits a
 *          single 21-byte (handle + decl) pair into a Read_By_Type
 *          response, and pushes it through L2CAP CID 0x0004. On any
 *          parse or lookup failure an ATT Error_Response is queued
 *          instead (Bluetooth Core 5.3 Vol 3 Part F 3.4.4.1 / 3.4.1.1).
 *
 * @param[in] conn_handle 12-bit ACL connection handle.
 * @param[in] pdu         Bytes past the opcode (non-NULL when len > 0).
 * @param[in] len         Parameter length in bytes
 *                        (must be >= 6 for a well-formed request).
 *
 * @pre pdu is non-NULL when len > 0.
 * @pre Stack is initialized.
 * @post Either a Read_By_Type_Response or an Error_Response has been queued.
 * @post Host attribute table is unchanged.
 *
 * @note Not thread-safe; called from the host serial dispatch loop.
 *
 * @since 0.1.0
 */
static void internal_handle_read_by_type(uint16_t conn_handle, const uint8_t* pdu, uint16_t len)
{
  enum : uint8_t {
    k_short_param_bytes = 6U,
    k_format_uuid16     = 0x03U,
  };
  enum : uint16_t {
    k_l2cap_cid_att = 0x0004U,
    k_uuid16_char   = 0x2803U,
  };

  if (len < k_short_param_bytes) {
    internal_send_error(conn_handle, k_att_op_read_by_type_req, 0U, k_att_err_invalid_pdu);
    return;
  }
  const uint16_t start  = internal_unpack_le16(&pdu[0]);
  const uint16_t end    = internal_unpack_le16(&pdu[2]);
  const uint16_t uuid16 = internal_unpack_le16(&pdu[4]);

  if (uuid16 != k_uuid16_char) {
    internal_send_error(conn_handle, k_att_op_read_by_type_req, start, k_att_err_attr_not_found);
    return;
  }

  /* Response: opcode + length + N x (handle, char_decl_value).
   * char_decl value is properties(1) + value_handle(2) + uuid(16) = 19. */
  enum : uint8_t {
    k_decl_value_bytes = 19U,
    k_pair_bytes       = 21U, /* handle(2) + decl(19). */
    k_max_resp_bytes   = 23U,
  };
  uint8_t  resp[k_max_resp_bytes];
  uint16_t pos = 0U;
  resp[pos++]  = k_att_op_read_by_type_rsp;
  resp[pos++]  = k_pair_bytes;

  if (!internal_emit_first_decl_in_range(resp, &pos, start, end)) {
    internal_send_error(conn_handle, k_att_op_read_by_type_req, start, k_att_err_attr_not_found);
    return;
  }
  (void)ra_ble_host_l2cap_send(conn_handle, k_l2cap_cid_att, resp, pos);
}

/**
 * @brief Handle ATT Read_Request (Vol 3 Part F 3.4.4.3).
 *
 * @details Looks up the requested attribute, then emits a
 *          Read_Response (opcode 0x0B) carrying its current value, or
 *          an Error_Response if the handle is unknown
 *          (Bluetooth Core 5.3 Vol 3 Part F 3.4.4.3 / 3.4.1.1).
 *
 * @param[in] conn_handle ACL handle.
 * @param[in] pdu         Bytes past the opcode.
 * @param[in] len         Length.
 *
 * @pre pdu is non-NULL when len > 0.
 * @pre Stack is initialized.
 * @post Either a Read_Response or an Error_Response has been queued.
 * @post Host attribute table is unchanged.
 *
 * @note Not thread-safe; called from the host serial dispatch loop.
 *
 * @since 0.1.0
 */
static void internal_handle_read(uint16_t conn_handle, const uint8_t* pdu, uint16_t len)
{
  enum : uint8_t {
    k_min_param_bytes = 2U,
    k_max_value_bytes = 22U, /* MTU 23 - opcode(1). */
    k_resp_hdr_bytes  = 1U,
  };
  enum : uint16_t {
    k_l2cap_cid_att = 0x0004U,
  };

  if (len < k_min_param_bytes) {
    internal_send_error(conn_handle, k_att_op_read_req, 0U, k_att_err_invalid_pdu);
    return;
  }
  const uint16_t            handle = internal_unpack_le16(&pdu[0]);
  const ra_ble_host_attr_t* a      = ra_ble_host_attr_lookup(handle);
  if (a == nullptr) {
    internal_send_error(conn_handle, k_att_op_read_req, handle, k_att_err_invalid_handle);
    return;
  }

  uint8_t  resp[k_resp_hdr_bytes + k_max_value_bytes];
  uint16_t pos = 0U;
  resp[pos++]  = k_att_op_read_rsp;

  if (a->kind == k_attr_kind_cccd) {
    /* CCCD value is two bytes (notify | indicate enable bits). */
    enum : uint8_t {
      k_cccd_bytes = 2U,
    };
    internal_pack_le16(&resp[pos], a->cccd_value);
    pos = (uint16_t)(pos + k_cccd_bytes);
  } else if (a->value != nullptr) {
    uint16_t copy_len = a->value_len;
    if (copy_len > k_max_value_bytes) {
      copy_len = k_max_value_bytes;
    }
    (void)memcpy(&resp[pos], a->value, copy_len);
    pos = (uint16_t)(pos + copy_len);
  } else if (a->kind == k_attr_kind_primary_service) {
    /* Primary Service value is the 128-bit service UUID stored in
     * uuid[]. */
    (void)memcpy(&resp[pos], a->uuid, k_ra_ble_host_uuid_bytes);
    pos = (uint16_t)(pos + k_ra_ble_host_uuid_bytes);
  }
  /* else: empty value response (legal). */
  (void)ra_ble_host_l2cap_send(conn_handle, k_l2cap_cid_att, resp, pos);
}

/**
 * @brief Apply a Write to a CCCD row and dispatch a subscribe event.
 *
 * @details Vol 3 Part G 3.3.3.3 -- CCCD payload is a 2-byte LE bitmask.
 *          Returns the ATT error code to surface to the peer, or 0
 *          when the write succeeded.
 *
 * @param[in,out] a           CCCD attribute row.
 * @param[in]     conn_handle ACL handle (passed verbatim to the event).
 * @param[in]     val         Raw payload bytes.
 * @param[in]     val_len     Payload length.
 *
 * @return 0 on success, ATT error code on failure.
 * @retval 0                                Subscribe event dispatched.
 * @retval k_att_err_invalid_value_len      val_len < 2.
 *
 * @pre a != NULL and a->kind == k_attr_kind_cccd.
 * @pre val != NULL when val_len > 0.
 * @post On success the row's cccd_value mirrors the LE16 payload and a
 *       subscribe event has been dispatched.
 * @post On failure no host state is mutated.
 *
 * @note Not thread-safe; called from the host serial dispatch loop.
 *
 * @since 0.1.0
 */
static uint8_t internal_write_cccd(ra_ble_host_attr_t* a,
                                   uint16_t            conn_handle,
                                   const uint8_t*      val,
                                   uint16_t            val_len)
{
  enum : uint8_t {
    k_cccd_min_bytes = 2U,
  };
  if (val_len < k_cccd_min_bytes) {
    return (uint8_t)k_att_err_invalid_value_len;
  }
  a->cccd_value               = internal_unpack_le16(val);
  const ra_ble_host_event_t e = {
    .kind        = k_ra_ble_host_event_subscribe,
    .conn_handle = conn_handle,
    .attr_handle = a->value_handle_owner,
    .data        = val,
    .data_len    = val_len,
  };
  ra_ble_host_dispatch_event(&e);
  return 0U;
}

/**
 * @brief Apply a Write to a characteristic-value row and dispatch the
 *        write event.
 *
 * @details Vol 3 Part F 3.4.5.1 / 3.4.5.3.
 *
 * @param[in,out] a           Char-value attribute row.
 * @param[in]     conn_handle ACL handle (passed verbatim to the event).
 * @param[in]     handle      Attribute handle being written.
 * @param[in]     val         Raw payload bytes.
 * @param[in]     val_len     Payload length.
 *
 * @return 0 on success, ATT error code on failure.
 * @retval 0                                Value updated and event dispatched.
 * @retval k_att_err_invalid_value_len      val_len > value_max.
 *
 * @pre a != NULL, a->kind == k_attr_kind_char_value, a->value != NULL.
 * @pre val != NULL when val_len > 0.
 * @post On success the value buffer holds val_len bytes and a write
 *       event has been dispatched.
 * @post On failure no host state is mutated.
 *
 * @note Not thread-safe; called from the host serial dispatch loop.
 *
 * @since 0.1.0
 */
static uint8_t internal_write_value(ra_ble_host_attr_t* a,
                                    uint16_t            conn_handle,
                                    uint16_t            handle,
                                    const uint8_t*      val,
                                    uint16_t            val_len)
{
  if (val_len > a->value_max) {
    return (uint8_t)k_att_err_invalid_value_len;
  }
  (void)memcpy(a->value, val, val_len);
  a->value_len                = val_len;
  const ra_ble_host_event_t e = {
    .kind        = k_ra_ble_host_event_write,
    .conn_handle = conn_handle,
    .attr_handle = handle,
    .data        = val,
    .data_len    = val_len,
  };
  ra_ble_host_dispatch_event(&e);
  return 0U;
}

/**
 * @brief Handle ATT Write_Request / Write_Command (Vol 3 Part F 3.4.5.1 / 3.4.5.3).
 *
 * @details
 * For Write_Request we send a Write_Response back; for Write_Command
 * we never respond (per spec). CCCD writes are mirrored into the
 * attribute row's ``cccd_value`` field and surfaced as a
 * ``subscribe`` event; data writes hit the value buffer and are
 * surfaced as a ``write`` event.
 *
 * @param[in] conn_handle  ACL handle.
 * @param[in] op           Original opcode (write_req or write_cmd).
 * @param[in] pdu          Bytes past the opcode.
 * @param[in] len          Length.
 *
 * @pre pdu is non-NULL when len > 0.
 * @pre Stack is initialized.
 * @post On Write_Request the attribute value (or CCCD) has been
 *       updated and a Write_Response or Error_Response queued.
 * @post On Write_Command the attribute is updated and no response is
 *       sent (per Vol 3 Part F 3.4.5.3).
 *
 * @note Not thread-safe; called from the host serial dispatch loop.
 *
 * @since 0.1.0
 */
static void
internal_handle_write(uint16_t conn_handle, uint8_t op, const uint8_t* pdu, uint16_t len)
{
  enum : uint8_t {
    k_min_param_bytes = 2U,
  };
  enum : uint16_t {
    k_l2cap_cid_att = 0x0004U,
  };

  if (len < k_min_param_bytes) {
    if (op == k_att_op_write_req) {
      internal_send_error(conn_handle, op, 0U, k_att_err_invalid_pdu);
    }
    return;
  }
  const uint16_t      handle = internal_unpack_le16(&pdu[0]);
  ra_ble_host_attr_t* a      = ra_ble_host_attr_lookup(handle);
  if (a == nullptr) {
    if (op == k_att_op_write_req) {
      internal_send_error(conn_handle, op, handle, k_att_err_invalid_handle);
    }
    return;
  }
  const uint8_t* val     = &pdu[2];
  const uint16_t val_len = (uint16_t)(len - 2U);

  uint8_t att_err = 0U;
  // mcdc-deactivated: TU-local helper internal_handle_read; characteristic-value attributes registered via the public API always provide a non-NULL backing buffer (validated at registration), so the second condition cannot independently flip on any reachable path.
  if (a->kind == k_attr_kind_cccd) {
    att_err = internal_write_cccd(a, conn_handle, val, val_len);
  } else if ((a->kind == k_attr_kind_char_value) && (a->value != nullptr)) {
    att_err = internal_write_value(a, conn_handle, handle, val, val_len);
  } else {
    att_err = (uint8_t)k_att_err_write_not_permitted;
  }

  if (att_err != 0U) {
    if (op == k_att_op_write_req) {
      internal_send_error(conn_handle, op, handle, (ra_ble_att_err_t)att_err);
    }
    return;
  }

  if (op == k_att_op_write_req) {
    const uint8_t rsp = (uint8_t)k_att_op_write_rsp;
    (void)ra_ble_host_l2cap_send(conn_handle, k_l2cap_cid_att, &rsp, 1U);
  }
}

/* =============================================================================
 * Top-level dispatcher (called from L2CAP).
 * =============================================================================
 */

/**
 * @brief Top-level ATT PDU dispatcher invoked by the L2CAP layer.
 *
 * @details Switches on the first byte (the ATT opcode per
 *          Bluetooth Core 5.3 Vol 3 Part F 3.4.8 Table 3.37) and
 *          forwards the remainder to the matching handler. Unknown
 *          opcodes are answered with an Error_Response (Request Not
 *          Supported, Vol 3 Part F 3.4.1.1).
 *
 * @param[in] conn_handle ACL handle the PDU arrived on.
 * @param[in] pdu         ATT PDU bytes (opcode + body).
 * @param[in] pdu_len     Total PDU byte count including the opcode.
 *
 * @pre Stack is initialized.
 * @pre pdu is non-NULL when pdu_len > 0.
 * @post The matching per-opcode handler has run, or an Error_Response
 *       has been queued for an unsupported opcode.
 * @post No host state outside the attribute table is mutated.
 *
 * @note Not thread-safe; called from the host serial dispatch loop.
 *
 * @since 0.1.0
 */
void ra_ble_host_att_handle_pdu(uint16_t conn_handle, const uint8_t* pdu, uint16_t pdu_len)
{
  if ((pdu == nullptr) || (pdu_len == 0U)) {
    return;
  }
  const uint8_t  op   = pdu[0];
  const uint8_t* body = &pdu[1];
  const uint16_t blen = (uint16_t)(pdu_len - 1U);

  switch (op) {
    case k_att_op_find_info_req:
      internal_handle_find_info(conn_handle, body, blen);
      break;
    case k_att_op_read_by_type_req:
      internal_handle_read_by_type(conn_handle, body, blen);
      break;
    case k_att_op_read_req:
      internal_handle_read(conn_handle, body, blen);
      break;
    case k_att_op_write_req:
    case k_att_op_write_cmd:
      internal_handle_write(conn_handle, op, body, blen);
      break;
    default:
      internal_send_error(conn_handle, op, 0U, k_att_err_request_not_supp);
      break;
  }
}
// NOLINTEND(readability-magic-numbers,readability-function-size,readability-function-cognitive-complexity)
