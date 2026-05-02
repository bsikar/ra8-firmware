/**
 * @file ra_ble_gatt.c
 * @brief GATT service / characteristic registration + notification TX
 *
 * @details
 * Builds the attribute table from app-supplied Primary Services and
 * Characteristics, following the encoding rules in Bluetooth Core
 * 5.3 Vol 3 Part G:
 *
 *   - Section 3.1 "Service Definition"        (UUID 0x2800).
 *   - Section 3.3 "Characteristic Definition" (Decl UUID 0x2803).
 *   - Section 3.3.3 "Characteristic Descriptors" + Vol 3 Part G
 *     3.3.3.3 "Client Characteristic Configuration"
 *                                              (Descriptor UUID 0x2902).
 *
 * The attribute table itself is owned by ra_ble_l2cap.c (singleton
 * ``ra_ble_host_state``); this TU just appends rows. Notifications
 * are framed here and pushed down to ra_ble_host_l2cap_send.
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
 *
 * Field order MUST match the definitions in ra_ble_l2cap.c.
 * =============================================================================
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
ra_err_t             ra_ble_host_l2cap_send(uint16_t       conn_handle,
                                            uint16_t       cid,
                                            const uint8_t* payload,
                                            uint16_t       payload_len);

/* Provided by ra_ble_att.c -- exposed so ra_ble_host_gatt_set_value can
 * find the attribute row for a given handle without duplicating the
 * loop. */
ra_ble_host_attr_t* ra_ble_host_attr_lookup(uint16_t handle);

/* =============================================================================
 * Internal helpers
 * =============================================================================
 */

/**
 * @brief Pack a 16-bit "assigned numbers" UUID into the 128-bit
 *        Bluetooth Base UUID slot (Bluetooth Core 5.3 Vol 3 Part B 2.5.1).
 *
 * @details
 * Base UUID is ``00000000-0000-1000-8000-00805F9B34FB``. The 16-bit
 * value lives at the upper 32-bit "data1" field. The byte order in
 * the 16-byte attribute UUID slot is little-endian, so the 16-bit
 * UUID's low byte goes at offset 12, high byte at offset 13.
 *
 * @param[out] dst 16-byte destination. Must not be NULL.
 * @param[in]  v   16-bit UUID.
 *
 * @pre dst != NULL.
 * @pre dst points to at least 16 writable bytes.
 * @post dst holds the LE-byte-ordered 128-bit UUID.
 * @post Bytes outside the data1 field match the Bluetooth Base UUID.
 *
 * @note Not thread-safe; caller serializes access to dst.
 *
 * @since 0.1.0
 */
static void internal_uuid16_to_128(uint8_t* dst, uint16_t v)
{
  enum : uint8_t {
    k_offset_data1_lo = 12U,
    k_offset_data1_hi = 13U,
    k_offset_data2_lo = 10U,
    k_offset_data2_hi = 11U,
    k_offset_data3_lo = 8U,
    k_offset_data3_hi = 9U,
  };
  static const uint8_t k_base_le[16] = {
    0xFBU,
    0x34U,
    0x9BU,
    0x5FU,
    0x80U,
    0x00U,
    0x00U,
    0x80U,
    0x00U,
    0x10U,
    0x00U,
    0x00U,
    0x00U,
    0x00U,
    0x00U,
    0x00U,
  };
  (void)memcpy(dst, k_base_le, 16);
  dst[k_offset_data1_lo] = (uint8_t)(v & 0xFFU);
  dst[k_offset_data1_hi] = (uint8_t)((v >> 8U) & 0xFFU);
  (void)k_offset_data2_lo;
  (void)k_offset_data2_hi;
  (void)k_offset_data3_lo;
  (void)k_offset_data3_hi;
}

/**
 * @brief Append one row to the attribute table.
 *
 * @details Bumps the host-singleton ``next_handle`` and writes a fresh
 *          attribute row at the next free slot. Used by the GATT
 *          service / characteristic registration helpers.
 *
 * @param[in] kind        Internal classification.
 * @param[in] uuid_le_128 16-byte LE-ordered UUID.
 * @param[in] props       Properties bitmask (only meaningful for char_decl).
 * @param[in] value       Backing value buffer (may be NULL).
 * @param[in] value_max   Capacity of ``value``.
 *
 * @return Pointer to the newly inserted row, or NULL if the table is
 *         full.
 * @retval NULL    Attribute table is at capacity (96 rows).
 * @retval !NULL   Pointer to the newly inserted attribute row.
 *
 * @pre Stack initialized.
 * @pre attr_count < 96 for a non-NULL return.
 * @post On success attr_count incremented and next_handle advanced.
 * @post On failure no state is mutated.
 *
 * @note Not thread-safe; called from the host serial dispatch loop.
 *
 * @since 0.1.0
 */
static ra_ble_host_attr_t* internal_append_attr(ra_ble_host_attr_kind_t kind,
                                                const uint8_t*          uuid_le_128,
                                                uint8_t                 props,
                                                uint8_t*                value,
                                                uint16_t                value_max)
{
  ra_ble_host_state_t* st = ra_ble_host_state();
  enum : uint8_t {
    k_attr_table_cap = 96U,
  };
  if (st->attr_count >= k_attr_table_cap) {
    return NULL;
  }
  ra_ble_host_attr_t* a = &st->attrs[st->attr_count];
  (void)memset(a, 0, sizeof(*a));
  a->handle             = st->next_handle;
  a->kind               = kind;
  a->props              = props;
  a->value              = value;
  a->value_len          = 0U;
  a->value_max          = value_max;
  a->value_handle_owner = 0U;
  if (uuid_le_128 != NULL) {
    (void)memcpy(a->uuid, uuid_le_128, k_ra_ble_host_uuid_bytes);
  }
  st->attr_count  = (uint8_t)(st->attr_count + 1U);
  st->next_handle = (uint16_t)(st->next_handle + 1U);
  return a;
}

/* =============================================================================
 * Public API
 * =============================================================================
 */

/**
 * @brief Register a Primary Service entry in the GATT attribute table.
 *
 * @details Appends a Primary Service attribute (UUID 0x2800,
 *          Bluetooth Core 5.3 Vol 3 Part G 3.1) whose value is the
 *          128-bit service UUID supplied by the caller. The handle of
 *          the newly created entry is returned via ``out_handle`` and
 *          can be passed to ra_ble_host_gatt_register_char.
 *
 * @param[in]  uuid_128   Service UUID, 16 bytes, little-endian on the
 *                        wire.
 * @param[out] out_handle Filled with the assigned ATT handle on success.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok                   Service registered.
 * @retval k_ra_err_null_ptr         uuid_128 or out_handle is NULL.
 * @retval k_ra_err_not_initialized  Stack not yet opened.
 * @retval k_ra_err_no_mem           Service or attribute table at capacity.
 *
 * @pre ra_ble_host_init has succeeded.
 * @pre uuid_128 and out_handle are non-NULL.
 * @post On success a new primary-service row exists in the table.
 * @post On failure no state is mutated.
 *
 * @note Not thread-safe; called from single-threaded application init.
 *
 * @since 0.1.0
 */
ra_err_t ra_ble_host_gatt_register_service(const uint8_t* uuid_128, uint16_t* out_handle)
{
  if ((uuid_128 == NULL) || (out_handle == NULL)) {
    return k_ra_err_null_ptr;
  }
  ra_ble_host_state_t* st = ra_ble_host_state();
  if (st->initialized == 0U) {
    return k_ra_err_not_initialized;
  }
  if (st->service_count >= k_ra_ble_host_max_services) {
    return k_ra_err_no_mem;
  }

  /* The attribute *type* is "Primary Service" UUID 0x2800; the
   * attribute *value* is the service UUID. We store the service UUID
   * in the row's uuid[] field for compactness -- the ATT layer's
   * Read handler emits it as the value when the attribute kind is
   * primary_service. */
  ra_ble_host_attr_t* a = internal_append_attr(k_attr_kind_primary_service, uuid_128, 0U, NULL, 0U);
  if (a == NULL) {
    return k_ra_err_no_mem;
  }
  st->service_count       = (uint8_t)(st->service_count + 1U);
  st->last_service_handle = a->handle;
  *out_handle             = a->handle;
  return k_ra_ok;
}

/**
 * @brief Register a Characteristic + value (+ optional CCCD) under a
 *        previously registered service.
 *
 * @details Appends the attribute rows that Bluetooth Core 5.3 Vol 3
 *          Part G 3.3 demands for a characteristic: the Characteristic
 *          Declaration (UUID 0x2803), the Characteristic Value, and
 *          (when notify or indicate is set in props) the Client
 *          Characteristic Configuration Descriptor
 *          (UUID 0x2902, Vol 3 Part G 3.3.3.3). The new value handle
 *          is returned to the caller.
 *
 * @param[in]  svc_handle  Handle of the parent service.
 * @param[in]  uuid_128    Characteristic UUID, 16 bytes, LE on wire.
 * @param[in]  props       Property bitmask (k_ra_ble_host_char_prop_*).
 * @param[in]  value_buf   Backing storage for the value (may be NULL
 *                         when value_max == 0).
 * @param[in]  value_max   Capacity of value_buf in bytes (<= 512).
 * @param[out] out_handle  Filled with the value handle on success.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok                   Characteristic registered.
 * @retval k_ra_err_null_ptr         Required pointer was NULL.
 * @retval k_ra_err_not_initialized  Stack not yet opened.
 * @retval k_ra_err_invalid_arg      Bad svc_handle, zero props, or oversized value.
 * @retval k_ra_err_no_mem           Char count or attribute table at capacity.
 *
 * @pre ra_ble_host_init has succeeded.
 * @pre uuid_128 and out_handle are non-NULL.
 * @post On success 2 or 3 attribute rows have been appended.
 * @post On failure no state is mutated past the rejected appends.
 *
 * @note Not thread-safe; called from single-threaded application init.
 *
 * @since 0.1.0
 */
ra_err_t ra_ble_host_gatt_register_char(uint16_t       svc_handle,
                                        const uint8_t* uuid_128,
                                        uint8_t        props,
                                        uint8_t*       value_buf,
                                        uint16_t       value_max,
                                        uint16_t*      out_handle)
{
  enum : uint16_t {
    k_max_value_max = 512U,
    k_uuid16_cccd   = 0x2902U,
  };

  if ((uuid_128 == NULL) || (out_handle == NULL)) {
    return k_ra_err_null_ptr;
  }
  if ((value_buf == NULL) && (value_max > 0U)) {
    return k_ra_err_null_ptr;
  }
  ra_ble_host_state_t* st = ra_ble_host_state();
  if (st->initialized == 0U) {
    return k_ra_err_not_initialized;
  }
  if (props == 0U) {
    return k_ra_err_invalid_arg;
  }
  if (value_max > k_max_value_max) {
    return k_ra_err_invalid_arg;
  }
  /* Verify svc_handle belongs to a registered service. */
  uint8_t svc_ok = 0U;
  for (uint8_t i = 0U; i < st->attr_count; i++) {
    if ((st->attrs[i].handle == svc_handle) && (st->attrs[i].kind == k_attr_kind_primary_service)) {
      svc_ok = 1U;
      break;
    }
  }
  if (svc_ok == 0U) {
    return k_ra_err_invalid_arg;
  }
  if (st->char_count >= k_ra_ble_host_max_chars) {
    return k_ra_err_no_mem;
  }

  /* Insert characteristic declaration (UUID 0x2803). The value of the
   * decl is properties|value_handle|char_uuid; we don't materialize that
   * in a buffer because the ATT Read_By_Type handler synthesizes it on
   * the fly using the row's props + handle + uuid[]. */
  uint8_t decl_uuid[k_ra_ble_host_uuid_bytes];
  internal_uuid16_to_128(decl_uuid, 0x2803U);
  /* The decl row stores the *characteristic*'s 128-bit UUID in uuid[]
   * (so Read_By_Type can splat it into the response), and its props
   * field carries the property bits. */
  ra_ble_host_attr_t* decl = internal_append_attr(k_attr_kind_char_decl, uuid_128, props, NULL, 0U);
  if (decl == NULL) {
    return k_ra_err_no_mem;
  }
  /* Override: store the decl's *type* UUID separately in the cccd_value
   * is not enough -- so we tag the kind and let ATT know to emit
   * 0x2803 when answering Find Information requests. We achieve that
   * by overwriting decl->uuid with the 128-bit form of 0x2803, BUT
   * we also need the char UUID later. So we store the char UUID in
   * the *value* row (kind == char_value) which we add immediately
   * below. */
  /* Actually re-set decl->uuid so Find_Information emits 0x2803. */
  internal_uuid16_to_128(decl->uuid, 0x2803U);

  ra_ble_host_attr_t* val =
    internal_append_attr(k_attr_kind_char_value, uuid_128, 0U, value_buf, value_max);
  if (val == NULL) {
    return k_ra_err_no_mem;
  }

  /* Stash the char UUID also onto the decl row so Read_By_Type's
   * handler can include it. We'll put it in val's uuid[] (already
   * done above) and have the ATT layer pull from val. But ATT walks
   * by handle, so using decl + 1 == val.handle works -- we already
   * encode that assumption. To keep ATT simple we also keep the char
   * UUID on decl (overwrites the 0x2803 we just wrote). To resolve
   * the conflict, store the 0x2803 in *uuid only* for the
   * Find_Information path; Read_By_Type uses decl->uuid for the
   * char-uuid bytes. So write the *char* UUID back. */
  (void)memcpy(decl->uuid, uuid_128, k_ra_ble_host_uuid_bytes);

  /* Optional CCCD if notify or indicate. */
  if ((props & (uint8_t)(k_ra_ble_host_char_prop_notify | k_ra_ble_host_char_prop_indicate)) !=
      0U) {
    uint8_t cccd_uuid[k_ra_ble_host_uuid_bytes];
    internal_uuid16_to_128(cccd_uuid, k_uuid16_cccd);
    ra_ble_host_attr_t* cccd = internal_append_attr(k_attr_kind_cccd, cccd_uuid, 0U, NULL, 0U);
    if (cccd == NULL) {
      return k_ra_err_no_mem;
    }
    cccd->value_handle_owner = val->handle;
  }

  st->char_count = (uint8_t)(st->char_count + 1U);
  *out_handle    = val->handle;
  (void)svc_handle;
  return k_ra_ok;
}

/**
 * @brief Update the cached value of a registered characteristic.
 *
 * @details Copies up to ``len`` bytes into the characteristic's
 *          backing buffer. Used together with ra_ble_host_gatt_notify
 *          to push fresh data to subscribed peers
 *          (Bluetooth Core 5.3 Vol 3 Part F 3.4.7 Notifications and
 *          Indications).
 *
 * @param[in] char_handle  Value handle from ra_ble_host_gatt_register_char.
 * @param[in] value        Source bytes (may be NULL when len == 0).
 * @param[in] len          Number of bytes to copy (<= value_max).
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok                   Value updated.
 * @retval k_ra_err_null_ptr         value is NULL while len > 0.
 * @retval k_ra_err_not_initialized  Stack not yet opened.
 * @retval k_ra_err_not_found        char_handle does not name a value row.
 * @retval k_ra_err_invalid_arg      len exceeds the value_max.
 *
 * @pre ra_ble_host_init has succeeded.
 * @pre char_handle was returned by ra_ble_host_gatt_register_char.
 * @post On success the row's value buffer holds len valid bytes.
 * @post On failure no state is mutated.
 *
 * @note Not thread-safe; called from the application thread.
 *
 * @since 0.1.0
 */
ra_err_t ra_ble_host_gatt_set_value(uint16_t char_handle, const uint8_t* value, uint16_t len)
{
  if ((value == NULL) && (len > 0U)) {
    return k_ra_err_null_ptr;
  }
  ra_ble_host_state_t* st = ra_ble_host_state();
  if (st->initialized == 0U) {
    return k_ra_err_not_initialized;
  }
  ra_ble_host_attr_t* a = ra_ble_host_attr_lookup(char_handle);
  if ((a == NULL) || (a->kind != k_attr_kind_char_value)) {
    return k_ra_err_not_found;
  }
  if (len > a->value_max) {
    return k_ra_err_invalid_arg;
  }
  if ((len > 0U) && (a->value != NULL)) {
    (void)memcpy(a->value, value, len);
  }
  a->value_len = len;
  return k_ra_ok;
}

/**
 * @brief Send an ATT Handle_Value_Notification (HVN) for a characteristic.
 *
 * @details Frames an HVN PDU (opcode 0x1B, Bluetooth Core 5.3 Vol 3
 *          Part F 3.4.7.1) with the characteristic's current value
 *          and pushes it through the L2CAP layer on CID 0x0004 (LE
 *          ATT). Silently succeeds when no peer is connected or when
 *          the peer has not enabled notifications via the CCCD
 *          (Vol 3 Part G 3.3.3.3).
 *
 * @param[in] char_handle Value handle from ra_ble_host_gatt_register_char.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok                   Notification queued (or silently dropped).
 * @retval k_ra_err_not_initialized  Stack not yet opened.
 * @retval k_ra_err_not_found        char_handle does not name a value row.
 * @retval k_ra_err_invalid_arg      Characteristic does not advertise notify.
 * @retval other                     Whatever ra_ble_host_l2cap_send returns.
 *
 * @pre ra_ble_host_init has succeeded.
 * @pre Characteristic was registered with the notify property bit.
 * @post On success an HVN PDU has been queued (or silently dropped).
 * @post On failure no state is mutated.
 *
 * @note Not thread-safe; called from the application thread.
 *
 * @since 0.1.0
 */
ra_err_t ra_ble_host_gatt_notify(uint16_t char_handle)
{
  enum : uint8_t {
    k_att_op_handle_value_notify = 0x1BU,
    k_pdu_hdr_bytes              = 3U,
    k_max_value_bytes            = 20U, /* MTU 23 - opcode(1) - handle(2). */
    k_cccd_notify_bit            = 0x01U,
  };
  enum : uint16_t {
    k_l2cap_cid_att = 0x0004U,
  };

  ra_ble_host_state_t* st = ra_ble_host_state();
  if (st->initialized == 0U) {
    return k_ra_err_not_initialized;
  }
  ra_ble_host_attr_t* a = ra_ble_host_attr_lookup(char_handle);
  if ((a == NULL) || (a->kind != k_attr_kind_char_value)) {
    return k_ra_err_not_found;
  }
  /* Find the matching decl row (handle == char_handle - 1) to read
   * the props bits. */
  ra_ble_host_attr_t* decl = ra_ble_host_attr_lookup((uint16_t)(char_handle - 1U));
  if ((decl == NULL) || ((decl->props & (uint8_t)k_ra_ble_host_char_prop_notify) == 0U)) {
    return k_ra_err_invalid_arg;
  }

  if (st->conn_handle == 0U) {
    /* No connection -- silently succeed. */
    return k_ra_ok;
  }

  /* Find subscriber CCCD. */
  uint8_t subscribed = 0U;
  for (uint8_t i = 0U; i < st->attr_count; i++) {
    if ((st->attrs[i].kind == k_attr_kind_cccd) &&
        (st->attrs[i].value_handle_owner == char_handle) &&
        ((st->attrs[i].cccd_value & k_cccd_notify_bit) != 0U)) {
      subscribed = 1U;
      break;
    }
  }
  if (subscribed == 0U) {
    return k_ra_ok;
  }

  uint16_t value_len = a->value_len;
  if (value_len > k_max_value_bytes) {
    value_len = k_max_value_bytes;
  }
  uint8_t pdu[k_pdu_hdr_bytes + k_max_value_bytes];
  pdu[0] = k_att_op_handle_value_notify;
  pdu[1] = (uint8_t)(char_handle & 0xFFU);
  pdu[2] = (uint8_t)((char_handle >> 8U) & 0xFFU);
  if ((value_len > 0U) && (a->value != NULL)) {
    (void)memcpy(&pdu[k_pdu_hdr_bytes], a->value, value_len);
  }
  return ra_ble_host_l2cap_send(st->conn_handle,
                                k_l2cap_cid_att,
                                pdu,
                                (uint16_t)(k_pdu_hdr_bytes + value_len));
}
// NOLINTEND(readability-magic-numbers,readability-function-size,readability-function-cognitive-complexity)
