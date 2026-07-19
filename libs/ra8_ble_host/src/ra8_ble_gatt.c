/**
 * @file ra8_ble_gatt.c
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
 * The attribute table itself is owned by ra8_ble_l2cap.c (singleton
 * ``ra8_ble_host_state``); this TU just appends rows. Notifications
 * are framed here and pushed down to ra8_ble_host_l2cap_send.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ra8_attributes.h"
#include "ra8_ble_gatt_internal.h"
#include "ra8_ble_host.h"
#include "ra8_err.h"

/**
 * @brief Pure should-copy predicate -- see header for full contract.
 * @details Promoted helper so the line-480/569 AND can be driven under MC/DC.
 * @param[in] len   Payload length.
 * @param[in] value Attribute value buffer.
 * @return Boolean predicate.
 * @retval true  Caller must memcpy.
 * @retval false Skip memcpy.
 * @pre None.
 * @pre None.
 * @post No state mutated.
 * @post Return depends solely on inputs.
 * @note Pure; thread-safe.
 * @since 0.1.0
 */
bool ra8_ble_gatt_internal_should_copy(uint16_t len, const void* value)
{
  return (len > 0U) && (value != nullptr);
}

/**
 * @brief Pure notify-invalid predicate -- see header for full contract.
 * @details Promoted helper so the line-538 OR can be driven under MC/DC.
 * @param[in] decl_present Non-zero if declaration row exists.
 * @param[in] props        Properties bitmask.
 * @param[in] notify_mask  Notify property bit mask.
 * @return Boolean reject predicate.
 * @retval true  Caller returns invalid-arg.
 * @retval false Notify is permitted.
 * @pre None.
 * @pre None.
 * @post No state mutated.
 * @post Return depends solely on inputs.
 * @note Pure; thread-safe.
 * @since 0.1.0
 */
bool ra8_ble_gatt_internal_notify_invalid(uint8_t decl_present, uint8_t props, uint8_t notify_mask)
{
  return (decl_present == 0U) || ((props & notify_mask) == 0U);
}

/* =============================================================================
 * Forward declarations of state shared with ra8_ble_l2cap.c.
 *
 * Field order MUST match the definitions in ra8_ble_l2cap.c.
 * =============================================================================
 */

typedef struct ra8_ble_host_attr_t ra8_ble_host_attr_t;

/** @brief Low-byte mask for little-endian handle/value packing. */
typedef enum : uint32_t {
  k_ble_byte_mask = 0xFFU, /**< BLE byte mask. */
} ble_gatt_mask_t;

typedef enum : uint16_t {
  k_ble_gatt_attr_table_len = 96U,  /**< BLE GATT attr table length. */
  k_ble_gatt_reassembly_len = 256U, /**< BLE GATT reassembly length. */
} ble_gatt_table_size_t;

typedef enum : uint8_t {
  k_attr_kind_primary_service = 0U, /**< Attr kind primary service. */
  k_attr_kind_char_decl       = 1U, /**< Attr kind char decl.       */
  k_attr_kind_char_value      = 2U, /**< Attr kind char value.      */
  k_attr_kind_cccd            = 3U, /**< Attr kind cccd.            */
} ra8_ble_host_attr_kind_t;

struct ra8_ble_host_attr_t {
  uint16_t                 handle;                          /**< Handle.             */
  ra8_ble_host_attr_kind_t kind;                            /**< Kind.               */
  uint8_t                  props;                           /**< Props.              */
  uint8_t                  uuid[k_ra8_ble_host_uuid_bytes]; /**< Uuid.               */
  uint8_t*                 value;                           /**< Value.              */
  uint16_t                 value_len;                       /**< Value length.       */
  uint16_t                 value_max;                       /**< Value maximum.      */
  uint16_t                 cccd_value;                      /**< Cccd value.         */
  uint16_t                 value_handle_owner;              /**< Value handle owner. */
};

typedef struct {
  uint8_t                 initialized;                           /**< Initialized.         */
  ra8_ble_host_role_t     role;                                  /**< Role.                */
  uint16_t                appearance;                            /**< Appearance.          */
  char                    name[32];                              /**< Name.                */
  uint16_t                next_handle;                           /**< Next handle.         */
  uint16_t                last_service_handle;                   /**< Last service handle. */
  ra8_ble_host_attr_t     attrs[k_ble_gatt_attr_table_len];      /**< Attrs.               */
  uint8_t                 attr_count;                            /**< Attr count.          */
  uint8_t                 service_count;                         /**< Service count.       */
  uint8_t                 char_count;                            /**< Char count.          */
  uint16_t                conn_handle;                           /**< Conn handle.         */
  uint16_t                att_mtu;                               /**< ATT MTU.             */
  uint8_t                 reassembly[k_ble_gatt_reassembly_len]; /**< Reassembly.          */
  uint16_t                reassembly_len;                        /**< Reassembly length.   */
  uint16_t                reassembly_expected;                   /**< Reassembly expected. */
  uint16_t                reassembly_cid;                        /**< Reassembly cid.      */
  uint16_t                reassembly_conn;                       /**< Reassembly conn.     */
  ra8_ble_host_event_fn_t evt_fn;                                /**< Evt fn.              */
  void*                   evt_ctx;                               /**< Evt ctx.             */
  uint32_t                evt_count;                             /**< Evt count.           */
} ra8_ble_host_state_t;

ra8_ble_host_state_t* ra8_ble_host_state(void);
ra8_err_t             ra8_ble_host_l2cap_send(uint16_t       conn_handle,
                                              uint16_t       cid,
                                              const uint8_t* payload,
                                              uint16_t       payload_len);

/* Provided by ra8_ble_att.c -- exposed so ra8_ble_host_gatt_set_value can
 * find the attribute row for a given handle without duplicating the
 * loop. */
ra8_ble_host_attr_t* ra8_ble_host_attr_lookup(uint16_t handle);

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
RA8_INTERNAL
static void internal_uuid16_to_128(uint8_t* dst, uint16_t v)
{
  enum : uint8_t {
    k_offset_data1_lo = 12U, /**< Offset data1 lo. */
    k_offset_data1_hi = 13U, /**< Offset data1 hi. */
    k_offset_data2_lo = 10U, /**< Offset data2 lo. */
    k_offset_data2_hi = 11U, /**< Offset data2 hi. */
    k_offset_data3_lo = 8U,  /**< Offset data3 lo. */
    k_offset_data3_hi = 9U,  /**< Offset data3 hi. */
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
  dst[k_offset_data1_lo] = (uint8_t)(v & k_ble_byte_mask);
  dst[k_offset_data1_hi] = (uint8_t)((v >> 8U) & k_ble_byte_mask);
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
RA8_INTERNAL
static ra8_ble_host_attr_t* internal_append_attr(ra8_ble_host_attr_kind_t kind,
                                                 const uint8_t*           uuid_le_128,
                                                 uint8_t                  props,
                                                 uint8_t*                 value,
                                                 uint16_t                 value_max)
{
  ra8_ble_host_state_t* st = ra8_ble_host_state();
  enum : uint8_t {
    k_attr_table_cap = 96U, /**< Attr table cap. */
  };
  if (st->attr_count >= k_attr_table_cap) {
    return nullptr;
  }
  ra8_ble_host_attr_t* a = &st->attrs[st->attr_count];
  (void)memset(a, 0, sizeof(*a));
  a->handle             = st->next_handle;
  a->kind               = kind;
  a->props              = props;
  a->value              = value;
  a->value_len          = 0U;
  a->value_max          = value_max;
  a->value_handle_owner = 0U;
  if (uuid_le_128 != nullptr) {
    (void)memcpy(a->uuid, uuid_le_128, k_ra8_ble_host_uuid_bytes);
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
 *          can be passed to ra8_ble_host_gatt_register_char.
 *
 * @param[in]  uuid_128   Service UUID, 16 bytes, little-endian on the
 *                        wire.
 * @param[out] out_handle Filled with the assigned ATT handle on success.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                   Service registered.
 * @retval k_ra8_err_null_ptr         uuid_128 or out_handle is NULL.
 * @retval k_ra8_err_not_initialized  Stack not yet opened.
 * @retval k_ra8_err_no_mem           Service or attribute table at capacity.
 *
 * @pre ra8_ble_host_init has succeeded.
 * @pre uuid_128 and out_handle are non-NULL.
 * @post On success a new primary-service row exists in the table.
 * @post On failure no state is mutated.
 *
 * @note Not thread-safe; called from single-threaded application init.
 *
 * @since 0.1.0
 */
ra8_err_t ra8_ble_host_gatt_register_service(const uint8_t* uuid_128, uint16_t* out_handle)
{
  if ((uuid_128 == nullptr) || (out_handle == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  ra8_ble_host_state_t* st = ra8_ble_host_state();
  if (st->initialized == 0U) {
    return k_ra8_err_not_initialized;
  }
  if (st->service_count >= k_ra8_ble_host_max_services) {
    return k_ra8_err_no_mem;
  }

  /* The attribute *type* is "Primary Service" UUID 0x2800; the
   * attribute *value* is the service UUID. We store the service UUID
   * in the row's uuid[] field for compactness -- the ATT layer's
   * Read handler emits it as the value when the attribute kind is
   * primary_service. */
  ra8_ble_host_attr_t* a =
    internal_append_attr(k_attr_kind_primary_service, uuid_128, 0U, nullptr, 0U);
  if (a == nullptr) {
    return k_ra8_err_no_mem;
  }
  st->service_count       = (uint8_t)(st->service_count + 1U);
  st->last_service_handle = a->handle;
  *out_handle             = a->handle;
  return k_ra8_ok;
}

/**
 * @brief Validate register_char inputs and svc_handle ownership.
 *
 * @details Centralizes the up-front argument checks (null pointers,
 *          state, props, value_max, svc lookup, and char-count cap)
 *          for ra8_ble_host_gatt_register_char.
 *
 * @param[in] st         Host state singleton.
 * @param[in] svc_handle Parent service handle.
 * @param[in] uuid_128   Characteristic UUID.
 * @param[in] props      Property bitmask.
 * @param[in] value_buf  Backing storage.
 * @param[in] value_max  Capacity of value_buf.
 * @param[in] out_handle Caller's output pointer.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                   All inputs valid.
 * @retval k_ra8_err_null_ptr         uuid_128 / out_handle NULL, or value_buf NULL with value_max>0.
 * @retval k_ra8_err_not_initialized  Stack not yet opened.
 * @retval k_ra8_err_invalid_arg      Bad svc_handle, zero props, or oversized value.
 * @retval k_ra8_err_no_mem           Char count at capacity.
 *
 * @pre st != NULL.
 * @pre Caller has not yet appended any attribute rows for this char.
 * @post No state mutation regardless of outcome.
 * @post Return code reflects only input/state validation.
 *
 * @note Not thread-safe; called from single-threaded application init.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_register_char_validate(const ra8_ble_host_state_t* st,
                                                 uint16_t                    svc_handle,
                                                 const uint8_t*              uuid_128,
                                                 uint8_t                     props,
                                                 const uint8_t*              value_buf,
                                                 uint16_t                    value_max,
                                                 const uint16_t*             out_handle)
{
  enum : uint16_t {
    k_max_value_max = 512U, /**< Maximum value maximum. */
  };
  if ((uuid_128 == nullptr) || (out_handle == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  if ((value_buf == nullptr) && (value_max > 0U)) {
    return k_ra8_err_null_ptr;
  }
  if (st->initialized == 0U) {
    return k_ra8_err_not_initialized;
  }
  if (props == 0U) {
    return k_ra8_err_invalid_arg;
  }
  if (value_max > k_max_value_max) {
    return k_ra8_err_invalid_arg;
  }
  uint8_t svc_ok = 0U;
  for (uint8_t i = 0U; i < st->attr_count; i++) {
    if ((st->attrs[i].handle == svc_handle) && (st->attrs[i].kind == k_attr_kind_primary_service)) {
      svc_ok = 1U;
      break;
    }
  }
  if (svc_ok == 0U) {
    return k_ra8_err_invalid_arg;
  }
  if (st->char_count >= k_ra8_ble_host_max_chars) {
    return k_ra8_err_no_mem;
  }
  return k_ra8_ok;
}

/**
 * @brief Append a CCCD row owned by ``value_handle`` if the supplied
 *        props enable notify or indicate (Vol 3 Part G 3.3.3.3).
 *
 * @details If the property bitmask carries either the notify or indicate
 *          bit, a fresh Client Characteristic Configuration Descriptor
 *          attribute (UUID 0x2902) is appended to the host attribute
 *          table and its ``value_handle_owner`` field is wired to the
 *          parent characteristic's value handle so later HVN/HVI paths
 *          can locate the subscription state. When neither bit is set
 *          the function is a no-op.
 *
 * @param[in] props        Characteristic property bitmask.
 * @param[in] value_handle Handle of the parent characteristic value row.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok          CCCD appended, or props did not require one.
 * @retval k_ra8_err_no_mem  Attribute table at capacity.
 *
 * @pre Stack initialized.
 * @pre value_handle is a freshly registered char-value handle.
 * @post On success the table either gained a CCCD row or was unchanged.
 * @post On failure no state is mutated.
 *
 * @note Not thread-safe; called from single-threaded application init.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_maybe_append_cccd(uint8_t props, uint16_t value_handle)
{
  enum : uint16_t {
    k_uuid16_cccd = 0x2902U, /**< Uuid16 cccd. */
  };
  const uint8_t notify_or_indicate =
    (uint8_t)(k_ra8_ble_host_char_prop_notify | k_ra8_ble_host_char_prop_indicate);
  if ((props & notify_or_indicate) == 0U) {
    return k_ra8_ok;
  }
  uint8_t cccd_uuid[k_ra8_ble_host_uuid_bytes];
  internal_uuid16_to_128(cccd_uuid, k_uuid16_cccd);
  ra8_ble_host_attr_t* cccd = internal_append_attr(k_attr_kind_cccd, cccd_uuid, 0U, nullptr, 0U);
  if (cccd == nullptr) {
    return k_ra8_err_no_mem;
  }
  cccd->value_handle_owner = value_handle;
  return k_ra8_ok;
}

/**
 * @brief Register a GATT characteristic under a previously created service.
 *
 * @details Appends a characteristic-declaration row (UUID 0x2803) and
 *          its paired value row (carrying the supplied UUID and the
 *          caller's backing buffer) to the host attribute table, then
 *          conditionally appends a CCCD row when the property bitmask
 *          enables notify or indicate
 *          (Bluetooth Core 5.3 Vol 3 Part G 3.3). The freshly assigned
 *          value handle is published through ``*out_handle`` so the
 *          caller can later drive ra8_ble_host_gatt_set_value /
 *          ra8_ble_host_gatt_notify.
 *
 * @param[in]  svc_handle Handle returned by ra8_ble_host_gatt_register_service.
 * @param[in]  uuid_128   16-byte little-endian characteristic UUID.
 * @param[in]  props      Property bitmask (k_ra8_ble_host_char_prop_*).
 * @param[in]  value_buf  Backing buffer for the characteristic value
 *                        (must outlive the host); may be NULL only when
 *                        value_max == 0.
 * @param[in]  value_max  Maximum value length in bytes (0..k_ra8_ble_host_attr_value_max).
 * @param[out] out_handle Receives the freshly assigned value handle.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                   Characteristic appended.
 * @retval k_ra8_err_null_ptr         uuid_128, value_buf (with value_max>0), or out_handle is NULL.
 * @retval k_ra8_err_not_initialized  Stack not yet opened.
 * @retval k_ra8_err_invalid_arg      svc_handle is not a service row, or value_max is out of range.
 * @retval k_ra8_err_no_mem           Attribute table at capacity.
 *
 * @pre ra8_ble_host_init has succeeded.
 * @pre svc_handle was returned by ra8_ble_host_gatt_register_service.
 * @post On success the table gained 2 or 3 new rows (decl, value, optional CCCD).
 * @post On failure no state is mutated beyond a possible partial append
 *       that the caller must treat as opaque (the next register call
 *       will return k_ra8_err_no_mem rather than corrupt the table).
 *
 * @note Not thread-safe; called from single-threaded application init.
 *
 * @since 0.1.0
 */
ra8_err_t ra8_ble_host_gatt_register_char(uint16_t       svc_handle,
                                          const uint8_t* uuid_128,
                                          uint8_t        props,
                                          uint8_t*       value_buf,
                                          uint16_t       value_max,
                                          uint16_t*      out_handle)
{
  ra8_ble_host_state_t* st  = ra8_ble_host_state();
  const ra8_err_t       vrc = internal_register_char_validate(st,
                                                              svc_handle,
                                                              uuid_128,
                                                              props,
                                                              value_buf,
                                                              value_max,
                                                              out_handle);
  if (vrc != k_ra8_ok) {
    return vrc;
  }

  /* Insert characteristic declaration (UUID 0x2803). The decl row
   * stores the *characteristic*'s 128-bit UUID in uuid[] so
   * Read_By_Type can splat it into the response; its props field
   * carries the property bits. */
  ra8_ble_host_attr_t* decl =
    internal_append_attr(k_attr_kind_char_decl, uuid_128, props, nullptr, 0U);
  if (decl == nullptr) {
    return k_ra8_err_no_mem;
  }
  ra8_ble_host_attr_t* val =
    internal_append_attr(k_attr_kind_char_value, uuid_128, 0U, value_buf, value_max);
  if (val == nullptr) {
    return k_ra8_err_no_mem;
  }
  /* Decl row keeps the char UUID in uuid[] for Read_By_Type. */
  (void)memcpy(decl->uuid, uuid_128, k_ra8_ble_host_uuid_bytes);

  const ra8_err_t crc = internal_maybe_append_cccd(props, val->handle);
  if (crc != k_ra8_ok) {
    return crc;
  }

  st->char_count = (uint8_t)(st->char_count + 1U);
  *out_handle    = val->handle;
  (void)svc_handle;
  return k_ra8_ok;
}

/**
 * @brief Update the cached value of a registered characteristic.
 *
 * @details Copies up to ``len`` bytes into the characteristic's
 *          backing buffer. Used together with ra8_ble_host_gatt_notify
 *          to push fresh data to subscribed peers
 *          (Bluetooth Core 5.3 Vol 3 Part F 3.4.7 Notifications and
 *          Indications).
 *
 * @param[in] char_handle  Value handle from ra8_ble_host_gatt_register_char.
 * @param[in] value        Source bytes (may be NULL when len == 0).
 * @param[in] len          Number of bytes to copy (<= value_max).
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                   Value updated.
 * @retval k_ra8_err_null_ptr         value is NULL while len > 0.
 * @retval k_ra8_err_not_initialized  Stack not yet opened.
 * @retval k_ra8_err_not_found        char_handle does not name a value row.
 * @retval k_ra8_err_invalid_arg      len exceeds the value_max.
 *
 * @pre ra8_ble_host_init has succeeded.
 * @pre char_handle was returned by ra8_ble_host_gatt_register_char.
 * @post On success the row's value buffer holds len valid bytes.
 * @post On failure no state is mutated.
 *
 * @note Not thread-safe; called from the application thread.
 *
 * @since 0.1.0
 */
ra8_err_t ra8_ble_host_gatt_set_value(uint16_t char_handle, const uint8_t* value, uint16_t len)
{
  if ((value == nullptr) && (len > 0U)) {
    return k_ra8_err_null_ptr;
  }
  ra8_ble_host_state_t* st = ra8_ble_host_state();
  if (st->initialized == 0U) {
    return k_ra8_err_not_initialized;
  }
  ra8_ble_host_attr_t* a = ra8_ble_host_attr_lookup(char_handle);
  if ((a == nullptr) || (a->kind != k_attr_kind_char_value)) {
    return k_ra8_err_not_found;
  }
  if (len > a->value_max) {
    return k_ra8_err_invalid_arg;
  }
  if (ra8_ble_gatt_internal_should_copy(len, a->value)) {
    (void)memcpy(a->value, value, len);
  }
  a->value_len = len;
  return k_ra8_ok;
}

/**
 * @brief Test whether any CCCD row has enabled notifications for the
 *        given characteristic value handle.
 *
 * @details Linearly scans the attribute table for a CCCD row whose
 *          ``value_handle_owner`` matches ``char_handle`` and whose
 *          cached ``cccd_value`` has the notify bit (0x01) set
 *          (Bluetooth Core 5.3 Vol 3 Part G 3.3.3.3). Returns on the
 *          first match; with a fixed-size attribute table the loop is
 *          bounded by ``st->attr_count``.
 *
 * @param[in] st           Host state singleton.
 * @param[in] char_handle  Value handle to query.
 *
 * @return Boolean subscription flag.
 * @retval true  At least one matching CCCD has the notify bit set.
 * @retval false No subscriber for char_handle.
 *
 * @pre st != NULL.
 * @pre Stack initialized.
 * @post No state mutation.
 * @post Return depends solely on the current attribute table.
 *
 * @note Not thread-safe; called from the host serial dispatch loop.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static bool internal_has_notify_subscriber(const ra8_ble_host_state_t* st, uint16_t char_handle)
{
  enum : uint8_t {
    k_cccd_notify_bit = 0x01U, /**< Cccd notify bit. */
  };
  for (uint8_t i = 0U; i < st->attr_count; i++) {
    if ((st->attrs[i].kind == k_attr_kind_cccd) &&
        (st->attrs[i].value_handle_owner == char_handle) &&
        ((st->attrs[i].cccd_value & k_cccd_notify_bit) != 0U)) {
      return true;
    }
  }
  return false;
}

/**
 * @brief Send an ATT Handle_Value_Notification (HVN) for a characteristic.
 *
 * @details Frames an HVN PDU (opcode 0x1B, Bluetooth Core 5.3 Vol 3
 *          Part F 3.4.7.1) carrying the characteristic's current
 *          cached value and pushes it through the L2CAP layer on
 *          CID 0x0004 (LE ATT). Silently succeeds (k_ra8_ok) when no
 *          peer has enabled notifications via the CCCD
 *          (Vol 3 Part G 3.3.3.3) so an application can call notify
 *          unconditionally on every value update.
 *
 * @param[in] char_handle Value handle from ra8_ble_host_gatt_register_char.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                   Notification queued, or silently dropped.
 * @retval k_ra8_err_not_initialized  Stack not yet opened.
 * @retval k_ra8_err_not_found        char_handle does not name a value row.
 * @retval k_ra8_err_invalid_arg      Cached value length exceeds the HVN payload cap.
 * @retval other                     Whatever ra8_ble_host_l2cap_send returns.
 *
 * @pre ra8_ble_host_init has succeeded.
 * @pre Characteristic was registered with the notify property bit.
 * @post On success an HVN PDU was queued, or the call was a no-op
 *       because no subscriber is currently registered.
 * @post On failure no host state is mutated.
 *
 * @note Not thread-safe; called from the application thread.
 *
 * @since 0.1.0
 */
ra8_err_t ra8_ble_host_gatt_notify(uint16_t char_handle)
{
  enum : uint8_t {
    k_att_op_handle_value_notify = 0x1BU, /**< ATT op handle value notify.     */
    k_pdu_hdr_bytes              = 3U,    /**< Pdu hdr bytes.                  */
    k_max_value_bytes            = 20U,   /**< MTU 23 - opcode(1) - handle(2). */
  };
  enum : uint16_t {
    k_l2cap_cid_att = 0x0004U, /**< L2CAP cid ATT. */
  };

  ra8_ble_host_state_t* st = ra8_ble_host_state();
  if (st->initialized == 0U) {
    return k_ra8_err_not_initialized;
  }
  ra8_ble_host_attr_t* a = ra8_ble_host_attr_lookup(char_handle);
  if ((a == nullptr) || (a->kind != k_attr_kind_char_value)) {
    return k_ra8_err_not_found;
  }
  /* Find the matching decl row (handle == char_handle - 1) to read
   * the props bits. */
  ra8_ble_host_attr_t* decl = ra8_ble_host_attr_lookup((uint16_t)(char_handle - 1U));
  if (ra8_ble_gatt_internal_notify_invalid((uint8_t)((decl != nullptr) ? 1U : 0U),
                                           (decl != nullptr) ? decl->props : (uint8_t)0U,
                                           (uint8_t)k_ra8_ble_host_char_prop_notify)) {
    return k_ra8_err_invalid_arg;
  }

  if (st->conn_handle == 0U) {
    /* No connection -- silently succeed. */
    return k_ra8_ok;
  }
  if (!internal_has_notify_subscriber(st, char_handle)) {
    return k_ra8_ok;
  }

  uint16_t value_len = a->value_len;
  if (value_len > k_max_value_bytes) {
    value_len = k_max_value_bytes;
  }
  uint8_t pdu[k_pdu_hdr_bytes + k_max_value_bytes];
  pdu[0] = k_att_op_handle_value_notify;
  pdu[1] = (uint8_t)(char_handle & k_ble_byte_mask);
  pdu[2] = (uint8_t)((char_handle >> 8U) & k_ble_byte_mask);
  if (ra8_ble_gatt_internal_should_copy(value_len, a->value)) {
    (void)memcpy(&pdu[k_pdu_hdr_bytes], a->value, value_len);
  }
  return ra8_ble_host_l2cap_send(st->conn_handle,
                                 k_l2cap_cid_att,
                                 pdu,
                                 (uint16_t)(k_pdu_hdr_bytes + value_len));
}
