/**
 * @file ra8_ble_gatt_client.h
 * @brief GATT client (central role) API on top of NimBLE.
 * @ingroup grp_net
 *
 * @par Tag
 * [Ring 5 / LIB] {World: NS}
 *
 * @details
 * Thin wrapper over the upstream ``ble_gattc_*`` family that returns
 * ra8_err_t and hides the NimBLE event-callback signature behind a
 * pair of plain C function pointers. Designed for central-role apps
 * that need to:
 *
 *   - Discover a peer's primary services.
 *   - Read / write a characteristic value by handle.
 *   - Subscribe (CCCD write) for notifications or indications.
 *
 * The discovery API surfaces results through a callback because
 * a single Discover All Primary Services exchange returns one row
 * per service (Bluetooth Core 5.3 Vol 3 Part F 3.4.4.10).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_err.h"

/* ============================================================ */
/* Limits */
/* ============================================================ */

/**
 * @enum ra8_ble_gatt_client_limits_t
 * @brief Static-allocation upper bounds.
 */
typedef enum : uint16_t {
  k_ra8_ble_gatt_client_uuid_bytes      = 16U,  /**< 128-bit UUID length.      */
  k_ra8_ble_gatt_client_max_read_bytes  = 512U, /**< ATT MTU max value length. */
  k_ra8_ble_gatt_client_max_write_bytes = 512U, /**< ATT MTU max value length. */
} ra8_ble_gatt_client_limits_t;

/* ============================================================ */
/* Discovery payload */
/* ============================================================ */

/**
 * @struct ra8_ble_gatt_service_t
 * @brief One service row delivered via the discovery callback.
 *
 * @details
 * Bluetooth Core 5.3 Vol 3 Part G 4.4 "Primary Service Discovery"
 * returns the start handle, end handle, and 128-bit UUID of each
 * primary service.
 */
typedef struct {
  uint16_t start_handle;                               /**< First attribute handle. */
  uint16_t end_handle;                                 /**< Last attribute handle.  */
  uint8_t  uuid_128[k_ra8_ble_gatt_client_uuid_bytes]; /**< 128-bit service UUID.   */
} ra8_ble_gatt_service_t;

/**
 * @typedef ra8_ble_gatt_disc_fn_t
 * @brief Discovery progress callback.
 *
 * @details
 * Called once per service while the discovery is in progress, then
 * one final time with ``svc == NULL`` to signal completion. The
 * ``status`` argument carries the SMP / GATT failure code or 0 on
 * a clean finish.
 *
 * @param[in] ctx    User context registered with the request.
 * @param[in] svc    Service row, or NULL on final invocation.
 * @param[in] status 0 on success, BLE_HS_E* on failure.
 */
typedef void (*ra8_ble_gatt_disc_fn_t)(void*                         ctx,
                                       const ra8_ble_gatt_service_t* svc,
                                       uint16_t                      status);

/**
 * @typedef ra8_ble_gatt_read_fn_t
 * @brief Completion callback for a GATT read.
 *
 * @param[in] ctx    User context.
 * @param[in] data   Value bytes (driver-owned, valid for call duration).
 * @param[in] len    Value length.
 * @param[in] status 0 on success, BLE_HS_E* on failure.
 */
typedef void (*ra8_ble_gatt_read_fn_t)(void*          ctx,
                                       const uint8_t* data,
                                       uint16_t       len,
                                       uint16_t       status);

/**
 * @typedef ra8_ble_gatt_write_fn_t
 * @brief Completion callback for a GATT write request.
 *
 * @param[in] ctx    User context.
 * @param[in] status 0 on success, BLE_HS_E* on failure.
 */
typedef void (*ra8_ble_gatt_write_fn_t)(void* ctx, uint16_t status);

/**
 * @typedef ra8_ble_gatt_notify_fn_t
 * @brief Notification / indication delivery callback.
 *
 * @param[in] ctx        User context.
 * @param[in] attr_handle Source value-attribute handle.
 * @param[in] data       Notification payload.
 * @param[in] len        Payload byte count.
 */
typedef void (*ra8_ble_gatt_notify_fn_t)(void*          ctx,
                                         uint16_t       attr_handle,
                                         const uint8_t* data,
                                         uint16_t       len);

/* ============================================================ */
/* Public APIs */
/* ============================================================ */

/**
 * @brief Discover the primary services hosted on a connected peer.
 *
 * @details
 * Issues a Discover All Primary Services exchange (Bluetooth Core 5.3
 * Vol 3 Part F 3.4.4.9). Each service the peer reports triggers a
 * call to ``cb`` with ``svc != NULL``; once the peer signals "no more
 * attributes" the host calls ``cb`` once more with ``svc == NULL``.
 *
 * Algorithm:
 *  1. Validate ``cb`` is non-NULL and the wrapper is initialized.
 *  2. Map the call onto ``ble_gattc_disc_all_svcs(conn_handle, ...)``.
 *  3. Invoke ``cb`` for each row, then a final NULL.
 *
 * @param[in] conn_handle Active ACL connection handle.
 * @param[in] cb          Per-service callback. Must not be NULL.
 * @param[in] ctx         Opaque user context, forwarded to ``cb``.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                   Discovery started.
 * @retval k_ra8_err_null_ptr         ``cb == NULL``.
 * @retval k_ra8_err_not_initialized  Stack not initialized.
 * @retval k_ra8_err_invalid_arg      ``conn_handle`` not connected.
 *
 * @pre ra8_ble_host_init() succeeded with role==central.
 * @pre ``conn_handle`` references an established connection.
 * @post Callback will be fired at least once (with svc==NULL).
 *
 * @note Thread-safe with respect to the NimBLE host event loop.
 *
 * @see ra8_ble_gatt_read()
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_ble_gatt_discover_services(uint16_t conn_handle, ra8_ble_gatt_disc_fn_t cb, void* ctx);

/**
 * @brief Read a characteristic value by its ATT handle.
 *
 * @details
 * Issues an ATT Read Request (Bluetooth Core 5.3 Vol 3 Part F
 * 3.4.4.3). The peer responds with a Read Response whose value is
 * delivered via ``cb``. Long values are stitched together with
 * sequential Read Blob Requests by the underlying NimBLE call.
 *
 * @param[in] conn_handle Active ACL connection handle.
 * @param[in] value_handle ATT handle of the value attribute.
 * @param[in] cb           Completion callback. Must not be NULL.
 * @param[in] ctx          Opaque user context.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                   Read queued.
 * @retval k_ra8_err_null_ptr         ``cb == NULL``.
 * @retval k_ra8_err_not_initialized  Stack not initialized.
 * @retval k_ra8_err_invalid_arg      Unknown ``conn_handle``.
 *
 * @pre Discovery has yielded ``value_handle``.
 * @post ``cb`` will be invoked exactly once.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_ble_gatt_read(uint16_t               conn_handle,
                                          uint16_t               value_handle,
                                          ra8_ble_gatt_read_fn_t cb,
                                          void*                  ctx);

/**
 * @brief Write a characteristic value by its ATT handle.
 *
 * @details
 * Issues either a Write Request (response-required) or a Write Command
 * (no-response) depending on ``response``. Bluetooth Core 5.3 Vol 3
 * Part F 3.4.5.1 and 3.4.5.3. Long values are split into Prepare Write
 * + Execute Write by NimBLE if ``len`` exceeds the negotiated MTU.
 *
 * @param[in] conn_handle  Active ACL connection handle.
 * @param[in] value_handle ATT handle of the value attribute.
 * @param[in] data         Bytes to write. Must not be NULL if len > 0.
 * @param[in] len          Byte count (0..k_ra8_ble_gatt_client_max_write_bytes).
 * @param[in] response     1 = Write Request, 0 = Write Command.
 * @param[in] cb           Optional completion callback (NULL for write-without-response).
 * @param[in] ctx          Opaque user context.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                   Write queued.
 * @retval k_ra8_err_null_ptr         data NULL with len > 0.
 * @retval k_ra8_err_not_initialized  Stack not initialized.
 * @retval k_ra8_err_invalid_arg      len out of range.
 *
 * @pre Discovery has yielded ``value_handle``.
 * @post On response==1, ``cb`` will fire exactly once.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_ble_gatt_write(uint16_t                conn_handle,
                                           uint16_t                value_handle,
                                           const uint8_t*          data,
                                           uint16_t                len,
                                           uint8_t                 response,
                                           ra8_ble_gatt_write_fn_t cb,
                                           void*                   ctx);

/**
 * @brief Subscribe to notifications or indications on a characteristic.
 *
 * @details
 * Writes the Client Characteristic Configuration Descriptor (CCCD,
 * UUID 0x2902, Bluetooth Core 5.3 Vol 3 Part G 3.3.3.3) at
 * ``cccd_handle`` with the value
 *  - ``0x0001`` for notifications,
 *  - ``0x0002`` for indications,
 *  - ``0x0000`` to unsubscribe.
 *
 * Inbound HVN / HVI PDUs are then delivered through ``notify_cb``.
 *
 * @param[in] conn_handle Active ACL connection handle.
 * @param[in] cccd_handle CCCD descriptor handle.
 * @param[in] enable_notify 1 = subscribe to notifications.
 * @param[in] enable_indicate 1 = subscribe to indications.
 * @param[in] notify_cb   Inbound notification callback. May be NULL
 *                        to receive only the CCCD-write completion.
 * @param[in] ctx         Opaque user context.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                   CCCD write queued.
 * @retval k_ra8_err_not_initialized  Stack not initialized.
 * @retval k_ra8_err_invalid_arg      Unknown handle, or both enable
 *                                   flags zero with NULL notify_cb.
 *
 * @pre Discovery yielded ``cccd_handle``.
 * @post Inbound HVN/HVI on the linked value handle land in notify_cb.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_ble_gatt_subscribe(uint16_t                 conn_handle,
                                               uint16_t                 cccd_handle,
                                               uint8_t                  enable_notify,
                                               uint8_t                  enable_indicate,
                                               ra8_ble_gatt_notify_fn_t notify_cb,
                                               void*                    ctx);

#ifdef __cplusplus
}
#endif
