/**
 * @file ra8_ble_host_internal.h
 * @brief Host-private contract shared between the BLE host TUs.
 * @ingroup grp_net
 *
 * @details
 * Declares the implementation-internal types and symbols that the
 * L2CAP core (ra8_ble_l2cap.c) shares with the GAP advertising TU
 * (ra8_ble_l2cap_advertise.c):
 *
 *   - The numeric constants enum (Bluetooth Core 5.3 header sizes and
 *     stack-internal capacities).
 *   - The GATT attribute-table row type and its kind enum.
 *   - The singleton host-state struct.
 *   - The single shared-state instance ``s_ble_host_state`` (defined
 *     in ra8_ble_l2cap.c, referenced from ra8_ble_l2cap_advertise.c).
 *   - The little-endian uint16 packer used by both TUs.
 *
 * This header is src/-local: it is not part of the public BLE host
 * API and must not be included outside libs/ra8_ble_host/src/.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_ble_host.h"

/** @brief GATT attribute pool capacity (worst case ~104, rounded). */
typedef enum : uint16_t {
  k_ble_l2cap_attr_cap = 96U, /**< BLE L2CAP attr cap. */
} ble_l2cap_cap_t;

/**
 * @enum ra8_ble_host_constants_t
 * @brief Implementation-internal numeric constants for the host stack.
 *
 * @details
 * Bluetooth Core 5.3 Vol 3 Part A 3 "Data Packet Format" sets the
 * L2CAP B-frame header at 4 bytes; Vol 4 Part E 5.4.2 sets the HCI
 * ACL header at 4 bytes; Vol 3 Part F 3.4.2 caps default ATT_MTU at
 * 23. All other values are sized to keep the host self-contained.
 */
typedef enum : uint16_t {
  k_l2cap_cid_att          = 0x0004U, /**< Vol 3 Part A 2.1 Table 2.3.   */
  k_l2cap_cid_le_signaling = 0x0005U, /**< Vol 3 Part A 2.1 Table 2.3.   */
  k_l2cap_hdr_bytes        = 4U,      /**< len(LE16) + cid(LE16).        */
  k_acl_hdr_bytes          = 4U,      /**< handle/PB/BC + len.           */
  k_att_mtu_default        = 23U,     /**< Vol 3 Part F 3.4.2.           */
  k_att_mtu_max            = 247U,    /**< Stack-internal cap.           */
  k_reassembly_buf_bytes   = 256U,    /**< Inbound L2CAP reassembly.     */
  k_tx_scratch_bytes       = 256U,    /**< Outbound L2CAP scratch.       */
  k_invalid_handle         = 0x0000U, /**< Sentinel for "no connection". */
  k_pb_first_non_flushable = 0x0000U, /**< Vol 4 Part E 5.4.2 PB flags.  */
  k_pb_continuation        = 0x1000U, /**< Pb continuation.              */
  k_pb_first_complete      = 0x2000U, /**< Pb first complete.            */
} ra8_ble_host_constants_t;

/**
 * @enum ra8_ble_host_attr_kind_t
 * @brief Internal attribute-table entry classification.
 */
typedef enum : uint8_t {
  k_attr_kind_primary_service = 0U, /**< UUID 0x2800.        */
  k_attr_kind_char_decl       = 1U, /**< UUID 0x2803.        */
  k_attr_kind_char_value      = 2U, /**< UUID = char's UUID. */
  k_attr_kind_cccd            = 3U, /**< UUID 0x2902.        */
} ra8_ble_host_attr_kind_t;

/**
 * @struct ra8_ble_host_attr_t
 * @brief One row of the GATT attribute table.
 *
 * @details Bluetooth Core 5.3 Vol 3 Part F 3.2 defines an attribute
 *          as (handle, type, permissions, value). We pack a 128-bit
 *          UUID for type (16-bit assigned numbers are stored
 *          big-endian-padded into the same 16-byte field).
 */
typedef struct {
  uint16_t                 handle; /**< ATT handle (1-based).                       */
  ra8_ble_host_attr_kind_t kind;   /**< Internal classification.                    */
  uint8_t                  props;  /**< Properties (only meaningful for char_decl). */
  uint8_t                  uuid[k_ra8_ble_host_uuid_bytes]; /**< Type UUID (LE). */
  uint8_t*                 value;              /**< Backing storage. NULL = no value.      */
  uint16_t                 value_len;          /**< Current valid bytes in ``value``.      */
  uint16_t                 value_max;          /**< Capacity of ``value``.                 */
  uint16_t                 cccd_value;         /**< Mirror of CCCD bits when kind == cccd. */
  uint16_t                 value_handle_owner; /**< For cccd: which char value handle.     */
} ra8_ble_host_attr_t;

/**
 * @struct ra8_ble_host_state_t
 * @brief Singleton runtime state shared across the three host TUs.
 *
 * @details
 * Sized for the limits in ``ra8_ble_host_limits_t``. Each
 * characteristic produces 2 or 3 attribute-table rows (decl + value
 * + optional CCCD), so the table is sized 1 + 8 + 32*3 == 105 to
 * leave headroom; we cap at 96 here, which covers the worst case
 * (all 32 chars carry CCCDs: 8 + 32*3 = 104; round to a 96-attribute
 * cap with a runtime check that rejects further inserts when full).
 */
typedef struct {
  uint8_t                 initialized;                        /**< Initialized.         */
  ra8_ble_host_role_t     role;                               /**< Role.                */
  uint16_t                appearance;                         /**< Appearance.          */
  char                    name[32];                           /**< Name.                */
  uint16_t                next_handle;                        /**< Next handle.         */
  uint16_t                last_service_handle;                /**< Last service handle. */
  ra8_ble_host_attr_t     attrs[k_ble_l2cap_attr_cap];        /**< Attrs.               */
  uint8_t                 attr_count;                         /**< Attr count.          */
  uint8_t                 service_count;                      /**< Service count.       */
  uint8_t                 char_count;                         /**< Char count.          */
  uint16_t                conn_handle;                        /**< Conn handle.         */
  uint16_t                att_mtu;                            /**< ATT MTU.             */
  uint8_t                 reassembly[k_reassembly_buf_bytes]; /**< Reassembly.          */
  uint16_t                reassembly_len;                     /**< Reassembly length.   */
  uint16_t                reassembly_expected;                /**< Reassembly expected. */
  uint16_t                reassembly_cid;                     /**< Reassembly cid.      */
  uint16_t                reassembly_conn;                    /**< Reassembly conn.     */
  ra8_ble_host_event_fn_t evt_fn;                             /**< Evt fn.              */
  void*                   evt_ctx;                            /**< Evt ctx.             */
  uint32_t                evt_count;                          /**< Evt count.           */
} ra8_ble_host_state_t;

/**
 * @var s_ble_host_state
 * @brief The single shared-state instance for the BLE host stack.
 *
 * @details Defined in ra8_ble_l2cap.c. Referenced directly by the GAP
 *          advertising TU (ra8_ble_l2cap_advertise.c) so the advertise
 *          path can read the initialized flag and the configured GAP
 *          role without an extra accessor call.
 *
 * @note Access restricted to the libs/ra8_ble_host/src TUs.
 *
 * @warning Not lock-guarded; mutate only from the single-threaded host
 *          dispatch / application contexts.
 *
 * @since 0.1.0
 */
extern ra8_ble_host_state_t s_ble_host_state;

/**
 * @brief Pack a little-endian uint16 into a byte buffer.
 *
 * @details Bluetooth Core 5.3 Vol 3 Part A 3 specifies LE byte order
 *          for L2CAP and HCI fields; this helper centralises the
 *          packing and is shared between the L2CAP core and the GAP
 *          advertising TU.
 *
 * @param[out] dst Two-byte destination.
 * @param[in]  v   Value to pack.
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
RA8_PRIV void internal_pack_le16(uint8_t* dst, uint16_t v);

#ifdef __cplusplus
}
#endif
