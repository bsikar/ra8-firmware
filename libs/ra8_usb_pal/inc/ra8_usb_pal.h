/**
 * @file ra8_usb_pal.h
 * @brief USB device-mode Platform Abstraction Layer
 * @ingroup grp_net
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details
 * scaffold for the USB PAL. Sits between the Ring-3
 * ``ra8_usb`` driver and any higher-level USB stack (CherryUSB
 * today, possibly TinyUSB or a custom stack later).
 *
 * Responsibilities:
 *
 * - Own the choice of FS vs HS controller.
 * - Reset / attach / detach lifecycle.
 * - Translate ra8_usb status masks into PAL-level event bits.
 * - Hide the MSTP / clock-gate dance behind ``ra8_usb_pal_init``.
 *
 * The PAL is intentionally stack-agnostic. CherryUSB's
 * ``usb_dc_ra8d2_*.c`` port (added) wraps this API;
 * no CherryUSB types appear in this header.
 *
 * ## Layering
 *
 * +---------------------------+ ra8_usb_pal_attach
 * | USB stack (CherryUSB) | ra8_usb_pal_set_event_handler
 * +-----------+---------------+
 * |
 * v
 * +---------------------------+
 * | ra8_usb_pal (this file) | wraps Ring-3 ra8_usb
 * +-----------+---------------+
 * |
 * v
 * +---------------------------+
 * | ra8_usb (Ring 3 / HAL) |
 * +---------------------------+
 *
 * ## Threading
 *
 * Single-threaded. Init runs from the boot path; the event handler
 * fires from ra8_usb ISR context.
 *
 * ## Endpoint I/O backing store
 *
 * Each endpoint gets a small software ring (depth
 * ``k_ra8_usb_pal_ring_slots``, per-packet capacity
 * ``k_ra8_usb_pal_pkt_max``) the stack writes to with
 * ``ra8_usb_pal_ep_send`` and drains with ``ra8_usb_pal_ep_recv``.
 * On real hardware the ring is backed by the controller pipe
 * FIFOs; in host tests it is a plain RAM buffer. The stack-facing
 * contract is identical in both paths, so CherryUSB's
 * ``usb_dc_ra8d2_*.c`` port talks to the same API.
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
#include "ra8_usb.h"

/* =============================================================================
 * Constants
 * =============================================================================
 */

/**
 * @enum ra8_usb_pal_limits_t
 * @brief USB sizing constants.
 *
 * @details
 * USB 2.0 endpoint count: the RA8D2 USBFS controller has 10
 * pipes (ENDPN 0..9), USBHS has 16. The PAL exposes the smaller
 * value (10) as the project-wide guarantee; controller-specific
 * extensions land.
 */
typedef enum : uint16_t {
  k_ra8_usb_pal_ep_max         = 10U,   /**< Maximum endpoint number.                   */
  k_ra8_usb_pal_ep0_max_packet = 64U,   /**< EP0 max packet on FS / HS-FS.              */
  k_ra8_usb_pal_bulk_max_fs    = 64U,   /**< Bulk max packet at full-speed.             */
  k_ra8_usb_pal_bulk_max_hs    = 512U,  /**< Bulk max packet at high-speed.             */
  k_ra8_usb_pal_xfer_max       = 1024U, /**< Single-shot xfer cap.                      */
  k_ra8_usb_pal_ep_addr_mask   = 0x7FU, /**< Strip USB-IN dir bit (bit 7) from ep_addr. */
} ra8_usb_pal_limits_t;

/**
 * @enum ra8_usb_pal_ep_dir_t
 * @brief Endpoint direction.
 */
typedef enum : uint8_t {
  k_ra8_usb_pal_ep_dir_out = 0U, /**< Host -> device. */
  k_ra8_usb_pal_ep_dir_in  = 1U, /**< Device -> host. */
} ra8_usb_pal_ep_dir_t;

/**
 * @enum ra8_usb_pal_ep_type_t
 * @brief USB endpoint transfer type.
 */
typedef enum : uint8_t {
  k_ra8_usb_pal_ep_type_control = 0U, /**< RA8 USB pal ep type control. */
  k_ra8_usb_pal_ep_type_iso     = 1U, /**< RA8 USB pal ep type iso.     */
  k_ra8_usb_pal_ep_type_bulk    = 2U, /**< RA8 USB pal ep type bulk.    */
  k_ra8_usb_pal_ep_type_intr    = 3U, /**< RA8 USB pal ep type intr.    */
} ra8_usb_pal_ep_type_t;

/**
 * @enum ra8_usb_pal_state_t
 * @brief USB device state per chapter 9 of USB 2.0.
 */
typedef enum : uint8_t {
  k_ra8_usb_pal_state_detached  = 0U, /**< D+ pull-up off.          */
  k_ra8_usb_pal_state_attached  = 1U, /**< D+ pull-up on, no reset. */
  k_ra8_usb_pal_state_default   = 2U, /**< Reset received.          */
  k_ra8_usb_pal_state_addressed = 3U, /**< Address assigned.        */
  k_ra8_usb_pal_state_configd   = 4U, /**< SET_CONFIGURATION done.  */
  k_ra8_usb_pal_state_suspended = 5U, /**< Bus idle > 3 ms.         */
} ra8_usb_pal_state_t;

/* =============================================================================
 * Event bits (relayed from ra8_usb status mask)
 * =============================================================================
 */

/**
 * @enum ra8_usb_pal_event_t
 * @brief Event mask bits passed to ``ra8_usb_pal_event_fn_t``.
 */
typedef enum : uint16_t {
  k_ra8_usb_pal_event_none    = 0x0000U, /**< RA8 USB pal event none.   */
  k_ra8_usb_pal_event_reset   = 0x0001U, /**< Bus reset.                */
  k_ra8_usb_pal_event_suspend = 0x0002U, /**< Bus suspend (idle).       */
  k_ra8_usb_pal_event_resume  = 0x0004U, /**< Bus resume.               */
  k_ra8_usb_pal_event_setup   = 0x0008U, /**< SETUP packet on EP0.      */
  k_ra8_usb_pal_event_ep_in   = 0x0010U, /**< IN endpoint complete.     */
  k_ra8_usb_pal_event_ep_out  = 0x0020U, /**< OUT endpoint complete.    */
  k_ra8_usb_pal_event_sof     = 0x0040U, /**< Start-of-Frame.           */
  k_ra8_usb_pal_event_attach  = 0x0080U, /**< VBUS rose / cable in.     */
  k_ra8_usb_pal_event_detach  = 0x0100U, /**< VBUS dropped / cable out. */
  k_ra8_usb_pal_event_error   = 0x8000U, /**< Controller error.         */
} ra8_usb_pal_event_t;

/**
 * @typedef ra8_usb_pal_event_fn_t
 * @brief Async event callback shape.
 *
 * @param[in] ctx Caller-supplied context.
 * @param[in] speed Which controller fired the event.
 * @param[in] event_mask OR of ``k_ra8_usb_pal_event_*`` bits.
 *
 * @note Invoked from ra8_usb ISR context. Must return quickly and
 * must not call back into ra8_usb_pal_init / deinit.
 */
typedef void (*ra8_usb_pal_event_fn_t)(void* ctx, ra8_usb_speed_t speed, uint16_t event_mask);

/* =============================================================================
 * Lifecycle
 * =============================================================================
 */

/**
 * @brief Initialise the USB PAL on a specific controller speed.
 *
 * @details
 * Calls ``ra8_usb_device_init(speed)`` to power on the chosen
 * controller, attaches a status-translation handler, and resets
 * the PAL state machine to ``detached``. Does NOT raise the D+
 * pull-up; the caller must call ``ra8_usb_pal_attach(true)`` to
 * advertise the device to the host.
 *
 * @param[in] speed Which controller (FS or HS) to bring up.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok PAL ready, state = detached.
 * @retval k_ra8_err_invalid_arg ``speed`` out of range.
 * @retval k_ra8_err_hw_init_failed Underlying ``ra8_usb_device_init``.
 *
 * @pre IRQs masked or single-threaded init context.
 * @pre ``ra8_mstp_init`` and ``ra8_pwr_init`` have been called.
 *
 * @post On success, the controller is clocked but D+ pull-up off.
 * @post ``ra8_usb_pal_get_state`` returns ``detached``.
 *
 * @note Thread safety: not thread-safe.
 * @see ra8_usb_pal_deinit
 * @see ra8_usb_pal_attach
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_usb_pal_init(ra8_usb_speed_t speed);

/**
 * @brief Tear down the USB PAL.
 *
 * @details
 * Drops the D+ pull-up, detaches the event handler, and calls
 * ``ra8_usb_device_deinit`` to drop the controller MSTP reference.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok PAL released.
 * @retval k_ra8_err_invalid_state PAL was never initialized.
 *
 * @pre IRQs masked or single-threaded shutdown context.
 *
 * @post Subsequent send/recv calls return ``k_ra8_err_invalid_state``.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_usb_pal_deinit(void);

/* =============================================================================
 * Bus state
 * =============================================================================
 */

/**
 * @brief Raise / drop the D+ pull-up to advertise the device.
 *
 * @param[in] attached ``true`` to assert pull-up, ``false`` to drop.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Pull-up state updated.
 * @retval k_ra8_err_invalid_state PAL not initialized.
 *
 * @pre PAL has been initialized.
 *
 * @post On success, ``ra8_usb_pal_get_state`` returns ``attached``
 * or ``detached`` to match.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_usb_pal_attach(bool attached);

/**
 * @brief Read the current PAL state machine.
 *
 * @param[out] out_state Receives the state.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Copied.
 * @retval k_ra8_err_null_ptr ``out_state`` was NULL.
 * @retval k_ra8_err_invalid_state PAL not initialized.
 *
 * @pre ``out_state`` is non-NULL.
 * @pre PAL has been initialized.
 *
 * @post No PAL state is modified.
 *
 * @note Thread safety: not thread-safe; the state can be updated
 * from ra8_usb ISR context concurrently.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_usb_pal_get_state(ra8_usb_pal_state_t* out_state);

/* =============================================================================
 * Endpoint I/O
 * =============================================================================
 */

/**
 * @brief Open a non-control endpoint with a given type / direction / size.
 *
 * @details
 * Stores the EP configuration in the PAL's per-endpoint slot and
 * resets its software queue to empty. Subsequent ``ep_send`` /
 * ``ep_recv`` calls route through this slot.
 *
 * @param[in] ep_addr Endpoint address (1..k_ra8_usb_pal_ep_max).
 * @param[in] dir IN or OUT.
 * @param[in] type Bulk / interrupt / iso (control = EP0 only).
 * @param[in] max_packet Maximum packet size in bytes.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Endpoint configured.
 * @retval k_ra8_err_invalid_arg Bad ep_addr / dir / type / size.
 * @retval k_ra8_err_invalid_state PAL not initialized.
 *
 * @pre PAL has been initialized.
 *
 * @post On success, the endpoint is ready for transfers.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_usb_pal_ep_open(uint8_t               ep_addr,
                                            ra8_usb_pal_ep_dir_t  dir,
                                            ra8_usb_pal_ep_type_t type,
                                            uint16_t              max_packet);

/**
 * @brief Submit data on an IN endpoint (device -> host).
 *
 * @param[in] ep_addr Endpoint number 1..k_ra8_usb_pal_ep_max.
 * @param[in] data Buffer to send.
 * @param[in] len Number of bytes; 0..max_packet.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Transfer queued.
 * @retval k_ra8_err_null_ptr ``data`` was NULL with non-zero len.
 * @retval k_ra8_err_invalid_arg ep_addr or len out of range.
 * @retval k_ra8_err_invalid_state PAL not initialized or EP not opened.
 * @retval k_ra8_err_no_mem EP TX ring full; try again later.
 *
 * @pre PAL has been initialized.
 * @pre Endpoint previously opened via ``ra8_usb_pal_ep_open``.
 *
 * @post On success, the data has been queued for TX.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_usb_pal_ep_send(uint8_t ep_addr, const uint8_t* data, uint16_t len);

/**
 * @brief Receive data from an OUT endpoint (host -> device).
 *
 * @param[in] ep_addr Endpoint number 1..k_ra8_usb_pal_ep_max.
 * @param[out] out_buf Destination buffer.
 * @param[in,out] inout_len On entry: capacity. On exit: bytes received.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Bytes received.
 * @retval k_ra8_err_no_data No data ready (poll-friendly).
 * @retval k_ra8_err_null_ptr ``out_buf`` / ``inout_len`` NULL.
 * @retval k_ra8_err_invalid_arg ep_addr or capacity bad.
 * @retval k_ra8_err_invalid_state PAL not initialized or EP not opened.
 *
 * @pre PAL has been initialized.
 * @pre Endpoint previously opened via ``ra8_usb_pal_ep_open``.
 *
 * @post On success, ``*inout_len`` holds the byte count.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_usb_pal_ep_recv(uint8_t ep_addr, uint8_t* out_buf, uint16_t* inout_len);

/* =============================================================================
 * Async event handler
 * =============================================================================
 */

/**
 * @brief Attach a single event handler for bus / endpoint events.
 *
 * @details
 * Replaces any previously installed handler. The PAL relays
 * ra8_usb ISR events into this callback after translating them
 * into the PAL-level ``k_ra8_usb_pal_event_*`` bit set.
 *
 * @param[in] fn Callback. Pass NULL to detach.
 * @param[in] ctx Context passed to the callback.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Handler installed (or detached).
 * @retval k_ra8_err_invalid_state PAL not initialized.
 *
 * @pre PAL has been initialized.
 *
 * @post Subsequent ra8_usb events are routed through ``fn``.
 *
 * @note Thread safety: not thread-safe; only call from
 * single-threaded init or with IRQs masked.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_usb_pal_set_event_handler(ra8_usb_pal_event_fn_t fn, void* ctx);

#ifdef __cplusplus
}
#endif
