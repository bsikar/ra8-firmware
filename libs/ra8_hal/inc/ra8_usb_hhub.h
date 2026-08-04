/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_usb_hhub.h
 * @brief Native USB host-side HUB class layer
 * @ingroup grp_hal_usb
 *
 * @details
 * Glues the host-mode bring-up paths in `ra8_usb` to a USB HUB device
 * (class code 0x09) attached to the EK-RA8D2's USB host port. Mirrors
 * FSP's `r_usb_basic` hub-support pattern: detect a HUB descriptor,
 * read the per-port descriptor, expose `GET_PORT_STATUS` and
 * `SET_PORT_FEATURE` control transfers so the application can sequence
 * port power-on, port reset, and child-device enumeration on
 * downstream ports.
 *
 * Lifecycle:
 *
 *  1. `ra8_usb_hhub_init(speed)` flips the controller to host mode and
 *     leaves the bus in the "wait for attach" state (UACT cleared).
 *  2. The driver runs the chapter-9 enumeration step machine over the
 *     DCP. When the device descriptor reports `bDeviceClass = 0x09`
 *     (HUB) it pulls the HUB class descriptor (`bDescriptorType =
 *     0x29`) and caches `bNbrPorts`.
 *  3. The registered attach callback fires once with the discovered
 *     port count and VID/PID.
 *  4. The application then walks each downstream port:
 *     `ra8_usb_hhub_set_port_feature(port, PORT_POWER)` ->
 *     `ra8_usb_hhub_set_port_feature(port, PORT_RESET)` ->
 *     `ra8_usb_hhub_get_port_status(port, &status)` and waits for
 *     `C_PORT_RESET` to clear before initiating SET_ADDRESS on the
 *     newly attached child.
 *  5. `ra8_usb_hhub_close()` drops bus power and releases the device.
 *
 * Reference: USB 2.0 specification chapter 11 "Hub Specification"
 * (USB-IF, 2000-04-27). Class request encoding follows USB 2.0
 * sec 11.24 "Hub Class Requests".
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_usb.h"

/* =============================================================================
 * Constants
 * =============================================================================
 */

/**
 * @enum ra8_usb_hhub_class_t
 * @brief HUB class code per USB 2.0 sec 11.24.1.
 */
typedef enum : uint8_t {
  k_ra8_hhub_class_hub = 0x09U, /**< bDeviceClass / bInterfaceClass = HUB. */
} ra8_usb_hhub_class_t;

/**
 * @enum ra8_usb_hhub_desc_t
 * @brief HUB descriptor types (USB 2.0 sec 11.23.2).
 */
typedef enum : uint8_t {
  k_ra8_hhub_desc_hub = 0x29U, /**< HUB class descriptor type. */
} ra8_usb_hhub_desc_t;

/**
 * @enum ra8_usb_hhub_request_t
 * @brief HUB class-specific request codes (USB 2.0 sec 11.24.2).
 */
typedef enum : uint8_t {
  k_ra8_hhub_req_get_status      = 0x00U, /**< GET_STATUS.      */
  k_ra8_hhub_req_clear_feature   = 0x01U, /**< CLEAR_FEATURE.   */
  k_ra8_hhub_req_set_feature     = 0x03U, /**< SET_FEATURE.     */
  k_ra8_hhub_req_get_descriptor  = 0x06U, /**< GET_DESCRIPTOR.  */
  k_ra8_hhub_req_set_descriptor  = 0x07U, /**< SET_DESCRIPTOR.  */
  k_ra8_hhub_req_clear_tt_buffer = 0x08U, /**< CLEAR_TT_BUFFER. */
  k_ra8_hhub_req_reset_tt        = 0x09U, /**< RESET_TT.        */
} ra8_usb_hhub_request_t;

/**
 * @enum ra8_usb_hhub_feature_t
 * @brief HUB port feature selectors (USB 2.0 Table 11-17).
 */
typedef enum : uint16_t {
  k_ra8_hhub_feature_port_connection   = 0U,  /**< C_PORT_CONNECTION cleared.    */
  k_ra8_hhub_feature_port_enable       = 1U,  /**< PORT_ENABLE.                  */
  k_ra8_hhub_feature_port_suspend      = 2U,  /**< PORT_SUSPEND.                 */
  k_ra8_hhub_feature_port_overcurrent  = 3U,  /**< PORT_OVER_CURRENT.            */
  k_ra8_hhub_feature_port_reset        = 4U,  /**< PORT_RESET.                   */
  k_ra8_hhub_feature_port_power        = 8U,  /**< PORT_POWER.                   */
  k_ra8_hhub_feature_port_low_speed    = 9U,  /**< PORT_LOW_SPEED.               */
  k_ra8_hhub_feature_c_port_connection = 16U, /**< C_PORT_CONNECTION (change).   */
  k_ra8_hhub_feature_c_port_enable     = 17U, /**< C_PORT_ENABLE (change).       */
  k_ra8_hhub_feature_c_port_suspend    = 18U, /**< C_PORT_SUSPEND (change).      */
  k_ra8_hhub_feature_c_port_overcurr   = 19U, /**< C_PORT_OVER_CURRENT (change). */
  k_ra8_hhub_feature_c_port_reset      = 20U, /**< C_PORT_RESET (change).        */
} ra8_usb_hhub_feature_t;

/**
 * @enum ra8_usb_hhub_port_status_bits_t
 * @brief Bit positions in the 32-bit port status word (USB 2.0 Table 11-21).
 *
 * @details Low 16 bits = wPortStatus, high 16 bits = wPortChange.
 */
typedef enum : uint8_t {
  k_ra8_hhub_status_bit_connection  = 0U,  /**< Current connect status. */
  k_ra8_hhub_status_bit_enable      = 1U,  /**< Port enabled.           */
  k_ra8_hhub_status_bit_suspend     = 2U,  /**< Suspend state.          */
  k_ra8_hhub_status_bit_overcurrent = 3U,  /**< Over-current latch.     */
  k_ra8_hhub_status_bit_reset       = 4U,  /**< Reset in progress.      */
  k_ra8_hhub_status_bit_power       = 8U,  /**< Port power on.          */
  k_ra8_hhub_status_bit_low_speed   = 9U,  /**< Low-speed device.       */
  k_ra8_hhub_status_bit_high_speed  = 10U, /**< High-speed device.      */
} ra8_usb_hhub_port_status_bits_t;

/**
 * @enum ra8_usb_hhub_limits_t
 * @brief Hard ceilings on hub topology this driver tracks.
 *
 * @details FSP's `r_usb_basic` hub support tracks up to 16 downstream
 * ports per hub; we mirror that ceiling.
 */
typedef enum : uint8_t {
  k_ra8_hhub_max_ports  = 16U, /**< FSP-aligned downstream port ceiling. */
  k_ra8_hhub_first_port = 1U,  /**< Port numbering starts at 1.          */
} ra8_usb_hhub_limits_t;

/**
 * @struct ra8_usb_hhub_device_t
 * @brief Snapshot of the attached HUB device, passed to the attach
 *        callback.
 */
typedef struct {
  uint8_t  device_address; /**< Assigned USB address (1..127). */
  uint8_t  port_count;     /**< bNbrPorts from HUB descriptor. */
  uint16_t vendor_id;      /**< idVendor.                      */
  uint16_t product_id;     /**< idProduct.                     */
} ra8_usb_hhub_device_t;

/**
 * @typedef ra8_usb_hhub_attach_fn_t
 * @brief Attach-callback signature.
 *
 * @param[in] ctx Caller-supplied context registered with
 *                `ra8_usb_hhub_attach_callback`.
 * @param[in] device Snapshot of the attached HUB device. The pointer
 *                   remains valid only for the duration of the call;
 *                   copy out anything you need.
 */
typedef void (*ra8_usb_hhub_attach_fn_t)(void* ctx, const ra8_usb_hhub_device_t* device);

/* =============================================================================
 * Lifecycle
 * =============================================================================
 */

/**
 * @brief Bring up the host-HUB driver on a chosen USB controller.
 *
 * @details
 * Initialises the underlying `ra8_usb` driver in HOST mode for `speed`,
 * leaves the bus in the "wait for attach" state (UACT cleared), and
 * arms the internal enumeration step machine so the next CTRT
 * interrupt advances enumeration.
 *
 * @param[in] speed Which USB controller (FS or HS).
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok Host-HUB ready, awaiting attach.
 * @retval k_ra8_err_invalid_arg `speed` out of range.
 * @retval k_ra8_err_hw_init_failed Underlying `ra8_usb_host_init` failed.
 *
 * @pre Single-threaded init context.
 * @pre `ra8_mstp_init` and `ra8_pwr_init` already ran.
 *
 * @post `ra8_usb_host_init` succeeded for `speed`.
 * @post Internal step machine armed; attach callback has not fired.
 *
 * @note Not thread-safe.
 * @see ra8_usb_hhub_attach_callback
 * @see ra8_usb_hhub_close
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_usb_hhub_init(ra8_usb_speed_t speed);

/**
 * @brief Tear down the host-HUB driver and release the controller.
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok Released.
 * @retval k_ra8_err_invalid_state Driver was never initialized.
 *
 * @pre Single-threaded shutdown context.
 *
 * @post `ra8_usb_host_deinit` ran, SOF generation halted, bus power
 *       dropped; subsequent host-HUB API calls return
 *       `k_ra8_err_invalid_state`.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_usb_hhub_close(void);

/* =============================================================================
 * Attach callback
 * =============================================================================
 */

/**
 * @brief Register (or detach) the HUB attach callback.
 *
 * @param[in] on_attach Callback. NULL detaches.
 * @param[in] ctx Context pointer threaded back into `on_attach`.
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok Callback installed.
 * @retval k_ra8_err_invalid_state Driver was never initialized.
 *
 * @pre `ra8_usb_hhub_init` has run.
 * @post On a subsequent attach, `on_attach(ctx, &device)` fires once.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_usb_hhub_attach_callback(ra8_usb_hhub_attach_fn_t on_attach, void* ctx);

/* =============================================================================
 * HUB class control transfers
 * =============================================================================
 */

/**
 * @brief Read the cached downstream port count from the HUB descriptor.
 *
 * @details Returns the value of `bNbrPorts` collected during
 * enumeration (USB 2.0 sec 11.23.2.1). Does NOT issue a fresh
 * GET_DESCRIPTOR(HUB) on every call -- the descriptor is cached at
 * attach time.
 *
 * @param[out] count Receives port count, in range
 *                   [`k_ra8_hhub_first_port` .. `k_ra8_hhub_max_ports`].
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok Count copied to `*count`.
 * @retval k_ra8_err_null_ptr `count` was NULL.
 * @retval k_ra8_err_invalid_state Driver not initialized, or no HUB
 *         attached.
 *
 * @pre `ra8_usb_hhub_init` succeeded.
 * @pre Attach callback already fired.
 *
 * @post `*count <= k_ra8_hhub_max_ports` on success.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_usb_hhub_get_port_count(uint8_t* count);

/**
 * @brief Issue GET_PORT_STATUS (USB 2.0 sec 11.24.2.7) over the DCP.
 *
 * @details
 * Builds an 8-byte SETUP packet with bmRequestType = 0xA3 (D2H |
 * Class | Other), bRequest = 0x00 (GET_STATUS), wValue = 0,
 * wIndex = port, wLength = 4. The data stage delivers a 32-bit word
 * whose low half is wPortStatus and high half is wPortChange.
 *
 * @param[in] port Downstream port number, in
 *                 [`k_ra8_hhub_first_port` .. `k_ra8_hhub_max_ports`].
 * @param[out] status Receives the 32-bit status word once the data
 *                    stage lands. The starter zeroes this on entry;
 *                    the production path overwrites it from the data
 *                    stage handler.
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok Control transfer queued.
 * @retval k_ra8_err_null_ptr `status` was NULL.
 * @retval k_ra8_err_invalid_state Driver not initialized, or no HUB
 *         attached.
 * @retval k_ra8_err_invalid_arg `port` out of range.
 * @retval k_ra8_err_busy Controller busy with a prior SETUP.
 *
 * @pre `ra8_usb_hhub_init` succeeded.
 * @pre Attach callback already fired.
 *
 * @post On success the SETUP mirror registers hold the GET_STATUS
 *       request envelope.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_usb_hhub_get_port_status(uint8_t port, uint32_t* status);

/**
 * @brief Issue SET_PORT_FEATURE (USB 2.0 sec 11.24.2.13) over the DCP.
 *
 * @details
 * Builds an 8-byte SETUP packet with bmRequestType = 0x23 (H2D |
 * Class | Other), bRequest = 0x03 (SET_FEATURE), wValue = feature,
 * wIndex = port, wLength = 0. Used to drive PORT_POWER, PORT_RESET,
 * PORT_SUSPEND, PORT_ENABLE, etc.
 *
 * @param[in] port Downstream port number.
 * @param[in] feature Feature selector (see ::ra8_usb_hhub_feature_t).
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok Control transfer queued.
 * @retval k_ra8_err_invalid_state Driver not initialized, or no HUB
 *         attached.
 * @retval k_ra8_err_invalid_arg `port` out of range.
 * @retval k_ra8_err_busy Controller busy with a prior SETUP.
 *
 * @pre `ra8_usb_hhub_init` succeeded.
 * @pre Attach callback already fired.
 *
 * @post On success the SETUP mirror registers hold the SET_FEATURE
 *       request envelope.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_usb_hhub_set_port_feature(uint8_t port, ra8_usb_hhub_feature_t feature);

/**
 * @brief Issue CLEAR_PORT_FEATURE (USB 2.0 sec 11.24.2.2) over the DCP.
 *
 * @details
 * Builds an 8-byte SETUP packet with bmRequestType = 0x23 (H2D |
 * Class | Other), bRequest = 0x01 (CLEAR_FEATURE), wValue = feature,
 * wIndex = port, wLength = 0. Most commonly used to acknowledge
 * change bits like C_PORT_RESET / C_PORT_CONNECTION.
 *
 * @param[in] port Downstream port number.
 * @param[in] feature Feature selector.
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok Control transfer queued.
 * @retval k_ra8_err_invalid_state Driver not initialized, or no HUB
 *         attached.
 * @retval k_ra8_err_invalid_arg `port` out of range.
 * @retval k_ra8_err_busy Controller busy with a prior SETUP.
 *
 * @pre `ra8_usb_hhub_init` succeeded.
 * @pre Attach callback already fired.
 *
 * @post On success the SETUP mirror registers hold the CLEAR_FEATURE
 *       request envelope.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_usb_hhub_clear_port_feature(uint8_t                port,
                                                        ra8_usb_hhub_feature_t feature);

/* =============================================================================
 * Test / introspection helpers
 * =============================================================================
 */

/**
 * @brief Drive the enumeration step machine forward by one step.
 *
 * @details
 * Test / debug entry point. Production drives this from the
 * `ra8_usb_dispatch` callback when the controller fires a CTRT or BRDY
 * interrupt; tests call it directly to walk the state machine
 * deterministically.
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok Step advanced.
 * @retval k_ra8_err_invalid_state Driver not initialized.
 *
 * @pre `ra8_usb_hhub_init` succeeded.
 *
 * @post Internal enumeration step counter advances by one. On the
 *       terminal step the registered attach callback fires.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_usb_hhub_step(void);

#ifdef __cplusplus
}
#endif
