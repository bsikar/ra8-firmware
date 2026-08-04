/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_ble.h
 * @brief BLE HCI transport seam (host-side)
 * @ingroup grp_hal_system
 *
 * @details
 * The host-side HCI transport the NimBLE host stack (via ``port/nimble``)
 * sits on. It is deliberately the controller-agnostic seam:
 *
 *   - Open / close the HCI transport.
 *   - Send raw HCI command and HCI ACL data packets.
 *   - Deliver HCI events and incoming ACL data to user-supplied
 *     callbacks.
 *   - Convenience wrappers for a small set of GAP advertising and
 *     scanning HCI commands so a smoke test can verify packet
 *     framing without pulling in a full Bluetooth host stack
 *     (L2CAP / ATT / GATT / GAP / SM are the host's job, not this seam's).
 *
 * The packet framing matches Bluetooth Core 5.3 Volume 4 Part A 2
 * ("HCI Transport Layer") with the H4-style packet-indicator byte:
 *
 *     | type | opcode (LE16) | param-len | params... |   command (0x01)
 *     | type | event-code | param-len | params... |       event   (0x04)
 *     | type | handle (LE16) | data-len (LE16) | payload | ACL  (0x02)
 *
 * @note The RA8D2 has no on-chip Bluetooth radio, so there is no on-chip
 *       controller backend. The current implementation (``ra8_ble.c``) is an
 *       in-memory loopback used by the host-stack unit tests. The production
 *       backend is an ESP32-C6 companion IC: the C6 runs the BLE controller
 *       (below HCI) and this seam carries HCI over the companion link, with
 *       the host stack above unchanged.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_err.h"

/* =============================================================================
 * Public limits
 * =============================================================================
 */

/**
 * @enum ra8_ble_limits_t
 * @brief Driver-side packet bounds. Picked to match HCI maximum sizes
 *        in Bluetooth Core 5.3 Vol 4 Part E 5.4 "Exchange of HCI-specific
 *        information".
 */
typedef enum : uint16_t {
  k_ra8_ble_max_cmd_params  = 255U, /**< HCI command max parameter byte count. */
  k_ra8_ble_max_evt_params  = 255U, /**< HCI event max parameter byte count.   */
  k_ra8_ble_max_acl_payload = 251U, /**< LE Data Length Extension default cap. */
  k_ra8_ble_addr_bytes      = 6U,   /**< 48-bit BD_ADDR length.                */
  k_ra8_ble_adv_data_max    = 31U,  /**< Legacy advertising-data byte cap.     */
} ra8_ble_limits_t;

/**
 * @enum ra8_ble_hci_pkt_t
 * @brief H4 HCI packet-type indicator bytes (Bluetooth Core 5.3 Vol 4 Part A 2).
 * @details HCI protocol constants -- not hardware registers. Public on the HCI
 *          seam so both the transport and its tests frame H4 packets with them.
 */
typedef enum : uint8_t {
  k_ra8_ble_pkt_cmd      = 0x01U, /**< HCI command packet indicator.  */
  k_ra8_ble_pkt_acl_data = 0x02U, /**< HCI ACL data packet indicator. */
  k_ra8_ble_pkt_event    = 0x04U, /**< HCI event packet indicator.    */
} ra8_ble_hci_pkt_t;

/* =============================================================================
 * Configuration
 * =============================================================================
 */

/**
 * @struct ra8_ble_config_t
 * @brief Configuration passed to ``ra8_ble_open``.
 */
typedef struct {
  uint8_t use_external_osc;  /**< 1 = drive radio from external 32 MHz xtal. */
  uint8_t deep_sleep_enable; /**< 1 = allow controller deep-sleep.           */
} ra8_ble_config_t;

/* =============================================================================
 * Callback typedefs
 * =============================================================================
 */

/**
 * @typedef ra8_ble_event_fn_t
 * @brief Fired when an HCI event packet has been received.
 *
 * @param[in] ctx       User context registered alongside the callback.
 * @param[in] evt_code  HCI event code (Bluetooth Core 5.3 Vol 4 Part E 7.7).
 * @param[in] params    Pointer into a driver-owned scratch buffer; valid
 *                      only for the duration of the callback.
 * @param[in] params_len Number of bytes pointed to by ``params``.
 */
typedef void (*ra8_ble_event_fn_t)(void*          ctx,
                                   uint8_t        evt_code,
                                   const uint8_t* params,
                                   uint8_t        params_len);

/**
 * @typedef ra8_ble_acl_fn_t
 * @brief Fired when an HCI ACL data packet has been received.
 *
 * @param[in] ctx     User context registered alongside the callback.
 * @param[in] handle  ACL connection handle (12-bit, low bits of the LE16).
 * @param[in] payload Pointer into a driver-owned scratch buffer; valid
 *                    only for the duration of the callback.
 * @param[in] len     Number of payload bytes.
 */
typedef void (*ra8_ble_acl_fn_t)(void* ctx, uint16_t handle, const uint8_t* payload, uint16_t len);

/* =============================================================================
 * Lifecycle
 * =============================================================================
 */

/**
 * @brief Power up the BLE controller and open the HCI mailbox.
 *
 * @details
 * Steps (mirrors FSP r_ble open + the production patch-load helper):
 *  1. Drop the controller out of reset (CTRL.reset = 1, then 0).
 *  2. Programme OSCCTL per ``cfg->use_external_osc``.
 *  3. Stub the patch-load loop (PATCHADDR/PATCHDATA writes).
 *  4. Set CTRL.enable | CTRL.hci_enable.
 *  5. Spin-wait for STATUS.ready.
 *
 * @param[in] cfg Driver configuration. Must not be NULL.
 *
 * @return ``k_ra8_err_null_ptr`` if ``cfg == NULL``.
 * @return ``k_ra8_err_invalid_arg`` if controller is already open.
 * @return ``k_ra8_ok`` on success.
 *
 * @pre Caller has clocked the BLE block (MSTPCRC bit, HUM Ch 11).
 * @pre No other thread holds the HCI mailbox.
 * @post STATUS.ready reads back as 1.
 * @post Subsequent ``ra8_ble_hci_send_command`` calls are accepted.
 *
 * @note Not thread-safe.
 * @warning Real silicon needs the Renesas-supplied BLE firmware patch
 *          image; the patch-load loop is currently stubbed.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_ble_open(const ra8_ble_config_t* cfg);

/**
 * @brief Close the HCI mailbox and put the controller back in reset.
 *
 * @return ``k_ra8_err_invalid_arg`` if controller is not open.
 * @return ``k_ra8_ok`` on success.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_ble_close(void);

/* =============================================================================
 * Raw HCI traffic
 * =============================================================================
 */

/**
 * @brief Emit an HCI command packet.
 *
 * @details
 * Frames `[0x01][opcode_lo][opcode_hi][params_len][params...]` and
 * pushes it into the HCI command FIFO. Returns immediately; the
 * matching Command Complete or Command Status event arrives via the
 * event callback.
 *
 * @param[in] opcode     16-bit HCI opcode (Bluetooth Core 5.3 Vol 4 Part E 5.4.1).
 * @param[in] params     Parameter bytes. May be NULL iff ``params_len == 0``.
 * @param[in] params_len Parameter byte count (0..255).
 *
 * @return ``k_ra8_err_not_initialized`` if controller is not open.
 * @return ``k_ra8_err_null_ptr`` if ``params == NULL && params_len > 0``.
 * @return ``k_ra8_err_invalid_arg`` if ``params_len > k_ra8_ble_max_cmd_params``.
 * @return ``k_ra8_ok`` on success.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_ble_hci_send_command(uint16_t opcode, const uint8_t* params, uint8_t params_len);

/**
 * @brief Emit an HCI ACL data packet.
 *
 * @details
 * Frames ``[0x02][handle_lo][handle_hi][len_lo][len_hi][payload]`` and
 * pushes it into the HCI ACL TX FIFO. The ``handle`` field carries the
 * 12-bit connection handle plus PB/BC flags as one LE16 word; callers
 * are expected to merge those fields beforehand.
 *
 * @param[in] handle  Connection handle + PB/BC flags (LE16, 16 bits).
 * @param[in] payload Payload bytes. May be NULL iff ``len == 0``.
 * @param[in] len     Payload byte count (0..k_ra8_ble_max_acl_payload).
 *
 * @return ``k_ra8_err_not_initialized`` if controller is not open.
 * @return ``k_ra8_err_null_ptr`` if ``payload == NULL && len > 0``.
 * @return ``k_ra8_err_invalid_arg`` if ``len > k_ra8_ble_max_acl_payload``.
 * @return ``k_ra8_ok`` on success.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_ble_hci_send_acl_data(uint16_t handle, const uint8_t* payload, uint16_t len);

/* =============================================================================
 * Receive callbacks
 * =============================================================================
 */

/**
 * @brief Register a handler for inbound HCI events.
 *
 * @param[in] fn  Callback. Pass NULL to detach.
 * @param[in] ctx Opaque user context, passed back on each event.
 *
 * @return ``k_ra8_ok`` always.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_ble_attach_event_handler(ra8_ble_event_fn_t fn, void* ctx);

/**
 * @brief Register a handler for inbound HCI ACL data.
 *
 * @param[in] fn  Callback. Pass NULL to detach.
 * @param[in] ctx Opaque user context, passed back on each ACL packet.
 *
 * @return ``k_ra8_ok`` always.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_ble_attach_acl_handler(ra8_ble_acl_fn_t fn, void* ctx);

/**
 * @brief Pump the HCI receive path -- call from ISR or main loop.
 *
 * @details
 * Drains both the event FIFO and the ACL RX FIFO, dispatching each
 * frame through the registered callbacks. Bounded by a static loop
 * cap so it never blocks indefinitely.
 *
 * @return ``k_ra8_err_not_initialized`` if controller is not open.
 * @return ``k_ra8_ok`` on success.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_ble_dispatch(void);

/* =============================================================================
 * Convenience wrappers (HCI LE commands)
 * =============================================================================
 */

/**
 * @brief Send LE_Set_Random_Address (opcode 0x2005).
 *
 * @param[in] addr 6-byte random Bluetooth address (LSB first per spec).
 *
 * @return ``k_ra8_err_null_ptr`` if ``addr == NULL``.
 * @return ``k_ra8_err_not_initialized`` if controller is not open.
 * @return ``k_ra8_ok`` on success.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_ble_set_random_address(const uint8_t addr[k_ra8_ble_addr_bytes]);

/**
 * @brief Send LE_Set_Advertising_Data (opcode 0x2008).
 *
 * @param[in] data Advertising-data bytes.
 * @param[in] len  Byte count (0..k_ra8_ble_adv_data_max).
 *
 * @return ``k_ra8_err_invalid_arg`` if ``len > k_ra8_ble_adv_data_max``.
 * @return ``k_ra8_err_null_ptr`` if ``data == NULL && len > 0``.
 * @return ``k_ra8_err_not_initialized`` if controller is not open.
 * @return ``k_ra8_ok`` on success.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_ble_set_advertising_data(const uint8_t* data, uint8_t len);

/**
 * @brief Send LE_Set_Advertising_Enable (opcode 0x200A).
 *
 * @param[in] enable 0 = disable advertising, 1 = enable.
 *
 * @return ``k_ra8_err_not_initialized`` if controller is not open.
 * @return ``k_ra8_ok`` on success.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_ble_set_advertising_enable(uint8_t enable);

/**
 * @brief Send LE_Set_Scan_Parameters then LE_Set_Scan_Enable.
 *
 * @details
 * Bluetooth Core 5.3 Vol 4 Part E 7.8.10 / 7.8.11. Two HCI commands
 * fly back-to-back; the controller answers with two Command Complete
 * events, both delivered through the registered event handler.
 *
 * @param[in] active   1 = active scanning (with SCAN_REQ), 0 = passive.
 * @param[in] interval Scan interval in 0.625 ms units (0x0004..0x4000).
 * @param[in] window   Scan window   in 0.625 ms units (0x0004..0x4000), <= interval.
 *
 * @return ``k_ra8_err_invalid_arg`` if window > interval or values out of range.
 * @return ``k_ra8_err_not_initialized`` if controller is not open.
 * @return ``k_ra8_ok`` on success.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_ble_scan_start(uint8_t active, uint16_t interval, uint16_t window);

#ifdef __cplusplus
}
#endif
