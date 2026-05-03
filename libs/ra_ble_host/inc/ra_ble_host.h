/**
 * @file ra_ble_host.h
 * @brief Starter Bluetooth Low Energy host stack (L2CAP / ATT / GATT)
 *
 * @details
 * Sits on top of libs/ra_hal/ra_ble (the controller HCI driver) and
 * provides a small, opinionated host substrate so applications can
 * advertise GATT services and accept incoming connections without
 * having to hand-roll the full Bluetooth Core protocol stack.
 *
 * Layering, top-down, mirrors Bluetooth Core 5.3 Vol 3:
 *
 *  - GATT (Vol 3 Part G)  -- service / characteristic database, exposed
 *    to apps through ``ra_ble_host_gatt_*`` calls.
 *  - ATT  (Vol 3 Part F)  -- attribute protocol PDUs (Find Information,
 *    Read By Type, Read, Write, Notify).
 *  - L2CAP (Vol 3 Part A) -- fixed channels 0x0004 (ATT) and 0x0005
 *    (LE signaling) over the controller-host HCI ACL pipe.
 *  - HCI / Controller (Vol 4 Part E) -- ra_ble.
 *
 * The intent is "good enough for a development board to expose a
 * couple of read/write/notify characteristics over BLE", not full
 * conformance. Pairing, security manager, GATT-over-EATT, ATT bearer
 * negotiation, and connection-oriented L2CAP channels are all out
 * of scope.
 *
 * Hard limits (Bluetooth Core 5.3 Vol 3 Part G 4 "GATT Feature
 * Requirements" caps are far higher; we cap aggressively so the
 * static attribute table fits in SRAM):
 *
 *   - 8 Primary Services
 *   - 32 Characteristics (across all services)
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra_ble.h"
#include "ra_err.h"

/* =============================================================================
 * Limits and well-known constants
 * =============================================================================
 */

/**
 * @enum ra_ble_host_limits_t
 * @brief Static-allocation upper bounds for the host stack.
 *
 * @details
 * These values size the in-stack attribute table and the L2CAP /
 * ATT scratch buffers. Picked deliberately low so the entire host
 * footprint fits comfortably alongside the rest of the firmware on
 * the EK-RA8D2.
 */
typedef enum : uint8_t {
  k_ra_ble_host_max_services = 8U,  /**< Maximum registered Primary Services.            */
  k_ra_ble_host_max_chars    = 32U, /**< Maximum registered Characteristics.             */
  k_ra_ble_host_uuid_bytes =
    16U, /**< 128-bit UUID byte length (Bluetooth Core Vol 3 Part B 2.5.1). */
} ra_ble_host_limits_t;

/**
 * @enum ra_ble_host_role_t
 * @brief GAP role selection for ``ra_ble_host_init``.
 *
 * @details
 * Bluetooth Core 5.3 Vol 3 Part C 2.2.2 "GAP Roles". The role chosen
 * here gates which advertising / scanning HCI commands the host is
 * allowed to issue.
 */
typedef enum : uint8_t {
  k_ra_ble_host_role_peripheral  = 0U, /**< Connectable, advertises, accepts connections.   */
  k_ra_ble_host_role_central     = 1U, /**< Scans, initiates connections.                   */
  k_ra_ble_host_role_observer    = 2U, /**< Scans only, never connects.                     */
  k_ra_ble_host_role_broadcaster = 3U, /**< Non-connectable advertiser only.                */
} ra_ble_host_role_t;

/**
 * @enum ra_ble_host_char_props_t
 * @brief Characteristic Properties bitmask
 *        (Bluetooth Core 5.3 Vol 3 Part G 3.3.1.1 Table 3.5).
 *
 * @details
 * Used as the ``props`` argument to
 * ``ra_ble_host_gatt_register_char``. The bits combine with bitwise
 * OR. Only the four properties this starter stack actually
 * implements are exposed.
 */
typedef enum : uint8_t {
  k_ra_ble_host_char_prop_read     = 0x02U, /**< Read.                              */
  k_ra_ble_host_char_prop_write_nr = 0x04U, /**< Write Without Response.            */
  k_ra_ble_host_char_prop_write    = 0x08U, /**< Write Request.                     */
  k_ra_ble_host_char_prop_notify   = 0x10U, /**< Handle Value Notification.         */
  k_ra_ble_host_char_prop_indicate = 0x20U, /**< Handle Value Indication.           */
} ra_ble_host_char_props_t;

/**
 * @enum ra_ble_host_event_kind_t
 * @brief Event kind delivered to the user-supplied event handler.
 */
typedef enum : uint8_t {
  k_ra_ble_host_event_connected    = 0U, /**< Link established (LE_Connection_Complete).  */
  k_ra_ble_host_event_disconnected = 1U, /**< Link torn down (Disconnection_Complete).    */
  k_ra_ble_host_event_write        = 2U, /**< Peer wrote to a Characteristic.             */
  k_ra_ble_host_event_subscribe    = 3U, /**< Peer wrote to a CCCD (notify/indicate on).  */
} ra_ble_host_event_kind_t;

/* =============================================================================
 * Configuration / event payload types
 * =============================================================================
 */

/**
 * @struct ra_ble_host_config_t
 * @brief Configuration passed to ``ra_ble_host_init``.
 *
 * @details
 * Mirrors the Generic Access Service mandatory characteristics
 * (Bluetooth Core 5.3 Vol 3 Part C 12.1 "Device Name" and 12.2
 * "Appearance"). Both are surfaced through GATT once the stack is
 * up.
 */
typedef struct {
  ra_ble_host_role_t role;       /**< GAP role.                                              */
  const char*        name;       /**< NUL-terminated device name (max 31 bytes incl. NUL).   */
  uint16_t           appearance; /**< Appearance value (Assigned Numbers 2.6.2).             */
} ra_ble_host_config_t;

/**
 * @struct ra_ble_host_event_t
 * @brief Event payload delivered by the host to the user handler.
 */
typedef struct {
  ra_ble_host_event_kind_t kind;        /**< What kind of event is this.                        */
  uint16_t                 conn_handle; /**< ACL connection handle (12 bits, host-byte-order).  */
  uint16_t                 attr_handle; /**< ATT handle relevant to the event (writes / sub).   */
  const uint8_t*           data;        /**< For write events: peer-supplied payload.           */
  uint16_t                 data_len;    /**< For write events: payload length.                  */
} ra_ble_host_event_t;

/**
 * @typedef ra_ble_host_event_fn_t
 * @brief User callback for connect / disconnect / write / subscribe.
 *
 * @param[in] ctx Opaque user context.
 * @param[in] evt Event payload, valid only for the duration of the call.
 */
typedef void (*ra_ble_host_event_fn_t)(void* ctx, const ra_ble_host_event_t* evt);

/* =============================================================================
 * Lifecycle
 * =============================================================================
 */

/**
 * @brief Bring up the BLE host stack.
 *
 * @details
 * Calls ``ra_ble_open`` under the hood, attaches the host-private
 * HCI event and ACL data handlers, and zeroes the GATT attribute
 * table. After this returns ``k_ra_ok`` the application may
 * register services / characteristics and start advertising.
 *
 * Algorithm:
 *  1. Validate ``cfg``.
 *  2. ``ra_ble_open`` with controller defaults (external osc, deep sleep).
 *  3. Register internal HCI event + ACL handlers via ``ra_ble_attach_*``.
 *  4. Reset the attribute table to "only the GAP service is registered".
 *
 * @param[in] cfg Configuration. Must not be NULL. ``name`` may be NULL
 *                (treated as empty). ``role`` must be one of the
 *                ``ra_ble_host_role_t`` enumerators.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok                  Stack is up.
 * @retval k_ra_err_null_ptr        ``cfg == NULL``.
 * @retval k_ra_err_invalid_arg     ``cfg->role`` out of range, or stack
 *                                  was already initialized.
 * @retval k_ra_err_invalid_state   Underlying ``ra_ble_open`` refused.
 *
 * @pre BLE clock already gated on (HUM Ch 11 MSTPCRC).
 * @pre Stack is not currently initialized.
 * @post Internal HCI event + ACL callbacks installed.
 * @post GATT table holds the GAP service shell.
 *
 * @note Not thread-safe. Init-time call.
 *
 * @see ra_ble_host_close()
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_ble_host_init(const ra_ble_host_config_t* cfg);

/**
 * @brief Tear down the host stack and put the controller back in reset.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok                Stack closed.
 * @retval k_ra_err_not_initialized Stack was not initialized.
 *
 * @pre Stack is initialized.
 * @post Underlying ``ra_ble_close`` was invoked.
 * @post Subsequent host calls return ``k_ra_err_not_initialized``.
 *
 * @note Not thread-safe.
 *
 * @see ra_ble_host_init()
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_ble_host_close(void);

/* =============================================================================
 * Advertising
 * =============================================================================
 */

/**
 * @brief Configure and start connectable undirected advertising.
 *
 * @details
 * Bluetooth Core 5.3 Vol 4 Part E 7.8.5 / 7.8.7 / 7.8.8 / 7.8.9.
 * Issues, in order:
 *   1. LE_Set_Advertising_Parameters (opcode 0x2006) with the
 *      caller-supplied ``interval_ms`` converted to 0.625 ms units.
 *   2. LE_Set_Advertising_Data       (opcode 0x2008).
 *   3. LE_Set_Scan_Response_Data     (opcode 0x2009) -- only if a
 *      scan-response payload was supplied.
 *   4. LE_Set_Advertising_Enable     (opcode 0x200A) with enable=1.
 *
 * @param[in] adv_data       Advertising payload. NULL iff ``adv_data_len == 0``.
 * @param[in] adv_data_len   Bytes (0..k_ra_ble_adv_data_max).
 * @param[in] scan_resp      Scan-response payload. NULL iff ``scan_resp_len == 0``.
 * @param[in] scan_resp_len  Bytes (0..k_ra_ble_adv_data_max).
 * @param[in] interval_ms    Advertising interval in milliseconds (20..10240).
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok                  Advertising started.
 * @retval k_ra_err_not_initialized Stack not initialized.
 * @retval k_ra_err_invalid_arg     Length / interval out of range, or role
 *                                  is observer/central.
 * @retval k_ra_err_null_ptr        Length>0 but pointer NULL.
 *
 * @pre Stack initialized via ra_ble_host_init().
 * @pre Role is peripheral or broadcaster.
 * @post Controller is advertising on the standard primary channels.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_ble_host_advertise_start(const uint8_t* adv_data,
                                                   uint8_t        adv_data_len,
                                                   const uint8_t* scan_resp,
                                                   uint8_t        scan_resp_len,
                                                   uint16_t       interval_ms);

/**
 * @brief Stop advertising.
 *
 * @details
 * Issues LE_Set_Advertising_Enable with enable=0 (Bluetooth Core 5.3
 * Vol 4 Part E 7.8.9).
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok                   Advertising stopped.
 * @retval k_ra_err_not_initialized  Stack not initialized.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_ble_host_advertise_stop(void);

/* =============================================================================
 * GATT registration
 * =============================================================================
 */

/**
 * @brief Register a Primary Service.
 *
 * @details
 * Allocates one entry in the attribute table whose Type is
 * "Primary Service" (UUID 0x2800, Bluetooth Core 5.3 Vol 3 Part G
 * 3.1) and whose Value is the supplied 128-bit service UUID.
 * The handle returned is the ATT handle of the service declaration
 * itself; subsequent characteristic registrations attach under it
 * until another service is registered.
 *
 * @param[in]  uuid_128   16-byte service UUID, little-endian per
 *                        Bluetooth Core 5.3 Vol 3 Part B 2.5.1.
 *                        Must not be NULL.
 * @param[out] out_handle Receives the new ATT handle. Must not be NULL.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok                  Service registered.
 * @retval k_ra_err_null_ptr        Either argument NULL.
 * @retval k_ra_err_not_initialized Stack not initialized.
 * @retval k_ra_err_no_mem          Service table full.
 *
 * @pre Stack initialized via ra_ble_host_init().
 * @pre Service table not full.
 * @post Service table size increased by one.
 * @post ``*out_handle`` is monotonically greater than any previously
 *       returned handle.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_ble_host_gatt_register_service(const uint8_t* uuid_128,
                                                         uint16_t*      out_handle);

/**
 * @brief Register a Characteristic under the most recent service.
 *
 * @details
 * Adds three attribute-table entries (Bluetooth Core 5.3 Vol 3
 * Part G 3.3):
 *   - Characteristic Declaration (UUID 0x2803).
 *   - Characteristic Value       (UUID = ``uuid_128``).
 *   - Client Characteristic Configuration Descriptor (UUID 0x2902)
 *     -- only if ``props`` includes notify or indicate.
 *
 * The handle returned is the ATT handle of the **value** attribute,
 * which is what apps pass to ``ra_ble_host_gatt_set_value`` and
 * ``ra_ble_host_gatt_notify``.
 *
 * @param[in]  svc_handle  ATT handle of the parent service (as returned
 *                         from ``ra_ble_host_gatt_register_service``).
 * @param[in]  uuid_128    16-byte characteristic UUID. Must not be NULL.
 * @param[in]  props       Property bitmask. See
 *                         ``ra_ble_host_char_props_t``.
 * @param[in]  value_buf   Caller-owned storage backing the characteristic
 *                         value. May be NULL iff value_max==0.
 * @param[in]  value_max   Capacity of ``value_buf`` in bytes (0..512,
 *                         Bluetooth Core 5.3 Vol 3 Part F 3.2.9 caps
 *                         attribute value at 512).
 * @param[out] out_handle  Receives the value-attribute ATT handle. Must
 *                         not be NULL.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok                  Characteristic registered.
 * @retval k_ra_err_null_ptr        ``uuid_128`` or ``out_handle`` NULL,
 *                                  or value_buf NULL with value_max>0.
 * @retval k_ra_err_invalid_arg     ``svc_handle`` is not a registered
 *                                  service handle, ``props`` is zero,
 *                                  or ``value_max`` exceeds 512.
 * @retval k_ra_err_no_mem          Characteristic table full.
 * @retval k_ra_err_not_initialized Stack not initialized.
 *
 * @pre Stack initialized.
 * @pre At least one service has been registered.
 * @post Characteristic count increased by one.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_ble_host_gatt_register_char(uint16_t       svc_handle,
                                                      const uint8_t* uuid_128,
                                                      uint8_t        props,
                                                      uint8_t*       value_buf,
                                                      uint16_t       value_max,
                                                      uint16_t*      out_handle);

/**
 * @brief Update the locally cached value of a Characteristic.
 *
 * @details
 * Writes ``len`` bytes from ``value`` into the characteristic's
 * backing buffer. Does **not** transmit anything over the air;
 * call ``ra_ble_host_gatt_notify`` afterwards if subscribers
 * should be informed.
 *
 * @param[in] char_handle  ATT value handle returned by
 *                         ``ra_ble_host_gatt_register_char``.
 * @param[in] value        Source bytes. Must not be NULL if ``len > 0``.
 * @param[in] len          Number of bytes (0..value_max).
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok                  Value updated.
 * @retval k_ra_err_null_ptr        value NULL with len>0.
 * @retval k_ra_err_not_found       Handle does not match any registered
 *                                  characteristic.
 * @retval k_ra_err_invalid_arg     ``len`` exceeds the buffer capacity.
 * @retval k_ra_err_not_initialized Stack not initialized.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t
ra_ble_host_gatt_set_value(uint16_t char_handle, const uint8_t* value, uint16_t len);

/**
 * @brief Send a Handle Value Notification to the subscribed peer.
 *
 * @details
 * Bluetooth Core 5.3 Vol 3 Part F 3.4.7.1 "Handle Value
 * Notification". Builds the ATT PDU
 * ``[0x1B][handle_lo][handle_hi][value...]``, wraps it in an L2CAP
 * B-frame on CID 0x0004, and pushes it down ``ra_ble_hci_send_acl_data``.
 *
 * If no peer is currently connected, or the peer has not enabled
 * notifications via the CCCD, the call returns ``k_ra_ok`` and no
 * packet is sent (this matches the Bluetooth Core "no Indication
 * is required" semantics).
 *
 * @param[in] char_handle ATT value handle.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok                  Notification sent (or skipped silently).
 * @retval k_ra_err_not_found       Handle does not match.
 * @retval k_ra_err_not_initialized Stack not initialized.
 * @retval k_ra_err_invalid_arg     Characteristic does not advertise
 *                                  the notify property.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_ble_host_gatt_notify(uint16_t char_handle);

/* =============================================================================
 * Event handler registration
 * =============================================================================
 */

/**
 * @brief Register the application's event callback.
 *
 * @param[in] fn  Callback. NULL detaches.
 * @param[in] ctx Opaque user context.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok                  Always.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_ble_host_attach_event_handler(ra_ble_host_event_fn_t fn, void* ctx);

/* =============================================================================
 * Test hooks (implementation detail, not part of the API surface)
 * =============================================================================
 */

#ifdef UNIT_TEST
/**
 * @brief Test hook: synthesize an inbound L2CAP/ATT PDU as if it had
 *        arrived over HCI ACL.
 *
 * @details Only present in UNIT_TEST builds. Bypasses the controller
 *          and feeds bytes straight into the host's L2CAP layer
 *          (Bluetooth Core 5.3 Vol 3 Part A 3 "Data Packet Format").
 *
 * @param[in] conn_handle 12-bit ACL connection handle to attribute the
 *                        PDU to.
 * @param[in] l2cap_frame L2CAP B-frame bytes (length(LE16) + cid(LE16)
 *                        + payload).
 * @param[in] len         Total byte count in l2cap_frame.
 *
 * @return None.
 * @retval None Function returns void.
 *
 * @pre ra_ble_host_init has succeeded.
 * @pre l2cap_frame is non-NULL when len > 0.
 * @post The L2CAP layer has consumed the frame and dispatched it.
 * @post Internal reassembly state may have advanced.
 *
 * @note Not thread-safe; intended for the unit-test harness.
 *
 * @since 0.1.0
 */
void ra_ble_host_test_inject_acl(uint16_t conn_handle, const uint8_t* l2cap_frame, uint16_t len);

/**
 * @brief Test hook: report whether the application event handler has
 *        been called and, if so, the most recent event kind.
 *
 * @details Returns the running count of host events dispatched since
 *          ra_ble_host_init.
 *
 * @return uint32_t Cumulative dispatched-event count.
 * @retval 0      No events dispatched yet.
 * @retval >0     One or more events delivered.
 *
 * @pre ra_ble_host_init has succeeded.
 * @pre Caller is single-threaded.
 * @post No state change.
 * @post Return value is monotonically non-decreasing.
 *
 * @note Not thread-safe; intended for the unit-test harness.
 *
 * @since 0.1.0
 */
uint32_t ra_ble_host_test_event_count(void);

/**
 * @brief Test hook: synthesize an LE_Connection_Complete subevent to
 *        drive the stack into the "connected" state.
 *
 * @details Bypasses the HCI transport and forces the host's connection
 *          handle / MTU bookkeeping as if Bluetooth Core 5.3 Vol 4
 *          Part E 7.7.65.1 (LE_Connection_Complete) had been received.
 *
 * @param[in] conn_handle 12-bit ACL connection handle to assign.
 *
 * @return None.
 * @retval None Function returns void.
 *
 * @pre ra_ble_host_init has succeeded.
 * @pre Caller is single-threaded.
 * @post The host's conn_handle is set to conn_handle.
 * @post A k_ra_ble_host_event_connected event has been dispatched.
 *
 * @note Not thread-safe; intended for the unit-test harness.
 *
 * @since 0.1.0
 */
void ra_ble_host_test_inject_connect(uint16_t conn_handle);

/**
 * @brief Test hook: synthesize an inbound HCI event as if delivered by
 *        the controller through the host event-handler trampoline.
 *
 * @details Only present in UNIT_TEST builds. Bypasses the HCI transport
 *          and feeds (evt_code, params, params_len) directly into the
 *          host's internal HCI event trampoline. Used to drive MC/DC
 *          coverage of the params==NULL / initialized==0 guard, the
 *          LE_Connection_Complete subevent decision (Bluetooth Core
 *          5.3 Vol 4 Part E 7.7.65.1), and the Disconnection_Complete
 *          decision (7.7.5).
 *
 * @param[in] evt_code   HCI event code.
 * @param[in] params     Parameter bytes (may be NULL when @p params_len
 *                       is 0).
 * @param[in] params_len Parameter byte count.
 *
 * @return None.
 * @retval None Function returns void.
 *
 * @pre Caller is single-threaded.
 * @pre @p params is non-NULL when @p params_len > 0 and the trampoline
 *      is expected to dispatch.
 * @post Connection bookkeeping (conn_handle / att_mtu / CCCDs) may have
 *       been updated and a connect/disconnect event dispatched.
 * @post No state change for unrecognised events or when the host is
 *       not yet initialised.
 *
 * @note Not thread-safe; intended for the unit-test harness.
 *
 * @since 0.1.0
 */
void ra_ble_host_test_inject_event(uint8_t evt_code, const uint8_t* params, uint8_t params_len);
#endif

#ifdef __cplusplus
}
#endif
