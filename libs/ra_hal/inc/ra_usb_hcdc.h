/**
 * @file ra_usb_hcdc.h
 * @brief Native USB host-side CDC ACM (Communications Device Class -
 *        Abstract Control Model) class layer
 *
 * @details
 * Glues the host-mode bring-up paths in `ra_usb` to a CDC-ACM
 * peripheral attached on the EK-RA8D2's USB-host port. Mirrors FSP's
 * `r_usb_hcdc` host-CDC class flow but compiled as part of this tree
 * with no FSP / CherryUSB / TinyUSB binaries pulled in.
 *
 * Lifecycle:
 *
 *  1. `ra_usb_hcdc_init(speed)` flips the controller to host mode,
 *     enables SOF generation and waits for `SYSSTS0.LNST` to report
 *     a J-state attach.
 *  2. The driver runs the chapter-9 enumeration step machine
 *     (Reset -> SET_ADDRESS -> GET_DEVICE_DESCRIPTOR ->
 *     GET_CONFIG_DESCRIPTOR -> SET_CONFIG -> SET_INTERFACE) over
 *     the DCP and walks the configuration descriptor for the CDC
 *     control + data interfaces.
 *  3. When enumeration completes, the registered attach callback
 *     fires once with the bulk-IN / bulk-OUT pipe handles.
 *  4. `ra_usb_hcdc_send` / `ra_usb_hcdc_recv` move bytes through
 *     the bulk pipes, mirroring the device-side `ra_usb_cdc_send` /
 *     `ra_usb_cdc_recv` surface.
 *  5. `ra_usb_hcdc_set_line_coding` issues the CDC class control
 *     transfer to update the attached device's line coding.
 *  6. `ra_usb_hcdc_close()` drops bus power and releases the device.
 *
 * The starter does not support hubs; it tracks a single attached
 * CDC-ACM device. Hub class enumeration is tracked as a deferred
 * follow-up.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra_err.h"
#include "ra_usb.h"

/* =============================================================================
 * Constants
 * =============================================================================
 */

/**
 * @enum ra_usb_hcdc_pipe_t
 * @brief PIPE numbers used by the host-CDC driver for the attached
 *        peripheral's endpoints.
 *
 * @details FSP / RA8D2 PIPE assignment rules constrain bulk pipes to
 * PIPE1..PIPE5 and interrupt pipes to PIPE6..PIPE9.
 */
typedef enum : uint8_t {
  k_ra_hcdc_pipe_bulk_in  = 1U, /**< PIPE1 -> attached EP bulk IN.  */
  k_ra_hcdc_pipe_bulk_out = 2U, /**< PIPE2 -> attached EP bulk OUT. */
  k_ra_hcdc_pipe_intr_in  = 6U, /**< PIPE6 -> attached EP intr IN.  */
} ra_usb_hcdc_pipe_t;

/**
 * @enum ra_usb_hcdc_packet_t
 * @brief Packet sizing for the attached device's bulk + interrupt
 *        endpoints (these are the maxima the host pipes are sized for;
 *        the actual values come from the device's descriptors).
 */
typedef enum : uint16_t {
  k_ra_hcdc_bulk_max_packet_fs = 64U,  /**< Bulk size at full speed. */
  k_ra_hcdc_bulk_max_packet_hs = 512U, /**< Bulk size at high speed. */
  k_ra_hcdc_intr_max_packet    = 8U,   /**< Notification pipe size.  */
} ra_usb_hcdc_packet_t;

/**
 * @enum ra_usb_hcdc_class_t
 * @brief Class / subclass / protocol triplets that identify a CDC ACM
 *        function within an attached USB device's descriptor walk.
 *
 * @details Numbered from the USB CDC PSTN subclass spec rev 1.20 and
 * the USB-IF "Class Codes" registry.
 */
typedef enum : uint8_t {
  k_ra_hcdc_class_comms     = 0x02U, /**< CDC control interface class. */
  k_ra_hcdc_subclass_acm    = 0x02U, /**< Abstract Control Model.      */
  k_ra_hcdc_protocol_at_v25 = 0x01U, /**< AT command set (V.25ter).    */
  k_ra_hcdc_class_data      = 0x0AU, /**< CDC data interface class.    */
  k_ra_hcdc_subclass_zero   = 0x00U, /**< Data subclass = 0.           */
  k_ra_hcdc_protocol_zero   = 0x00U, /**< Data protocol = 0.           */
} ra_usb_hcdc_class_t;

/**
 * @enum ra_usb_hcdc_request_t
 * @brief CDC class-specific request codes the host issues to the
 *        attached device.
 */
typedef enum : uint8_t {
  k_ra_hcdc_req_set_line_coding        = 0x20U, /**< 7-byte payload. */
  k_ra_hcdc_req_get_line_coding        = 0x21U, /**< 7-byte payload. */
  k_ra_hcdc_req_set_control_line_state = 0x22U, /**< 0-byte payload. */
} ra_usb_hcdc_request_t;

/**
 * @enum ra_usb_hcdc_parity_t
 * @brief Parity selector used by `ra_usb_hcdc_set_line_coding`.
 *
 * @details Wire-level value matches USB CDC PSTN spec rev 1.20 table
 * 17 (SET_LINE_CODING bParityType field).
 */
typedef enum : uint8_t {
  k_ra_hcdc_parity_none  = 0U, /**< No parity.    */
  k_ra_hcdc_parity_odd   = 1U, /**< Odd parity.   */
  k_ra_hcdc_parity_even  = 2U, /**< Even parity.  */
  k_ra_hcdc_parity_mark  = 3U, /**< Mark parity.  */
  k_ra_hcdc_parity_space = 4U, /**< Space parity. */
} ra_usb_hcdc_parity_t;

/**
 * @enum ra_usb_hcdc_stop_bits_t
 * @brief Stop-bit selector used by `ra_usb_hcdc_set_line_coding`.
 *
 * @details Wire-level value matches USB CDC PSTN spec rev 1.20 table
 * 17 (SET_LINE_CODING bCharFormat field).
 */
typedef enum : uint8_t {
  k_ra_hcdc_stop_1   = 0U, /**< 1 stop bit.    */
  k_ra_hcdc_stop_1_5 = 1U, /**< 1.5 stop bits. */
  k_ra_hcdc_stop_2   = 2U, /**< 2 stop bits.   */
} ra_usb_hcdc_stop_bits_t;

/**
 * @struct ra_usb_hcdc_device_t
 * @brief Snapshot of the attached CDC-ACM device, passed to the
 *        attach callback.
 *
 * @details Populated by the host-CDC driver once the descriptor walk
 * completes. The pipe numbers are the controller-side handles wired
 * up against the attached device's bulk + interrupt endpoints. The
 * attach-callback recipient should hold onto this struct (or copy
 * it) so subsequent `ra_usb_hcdc_send` / `_recv` calls land on the
 * right pipes.
 */
typedef struct {
  // cppcheck-suppress unusedStructMember
  uint8_t device_address; /**< Assigned USB address (1..127). */
  // cppcheck-suppress unusedStructMember
  uint8_t bulk_in_ep; /**< Attached device's bulk IN EP num. */
  // cppcheck-suppress unusedStructMember
  uint8_t bulk_out_ep; /**< Attached device's bulk OUT EP num. */
  // cppcheck-suppress unusedStructMember
  uint8_t intr_in_ep; /**< Attached device's intr IN EP num. */
  // cppcheck-suppress unusedStructMember
  uint16_t bulk_in_max_packet; /**< Attached bulk-IN wMaxPacketSize. */
  // cppcheck-suppress unusedStructMember
  uint16_t bulk_out_max_packet; /**< Attached bulk-OUT wMaxPacketSize. */
  // cppcheck-suppress unusedStructMember
  uint16_t vendor_id; /**< idVendor from device descriptor. */
  // cppcheck-suppress unusedStructMember
  uint16_t product_id; /**< idProduct from device descriptor. */
} ra_usb_hcdc_device_t;

/**
 * @typedef ra_usb_hcdc_attach_fn_t
 * @brief Attach-callback signature.
 *
 * @param[in] ctx Caller-supplied context registered with
 *                `ra_usb_hcdc_attach_callback`.
 * @param[in] device Snapshot of the attached CDC-ACM device. The
 *                   pointer remains valid only for the duration of
 *                   the call; copy out anything you need.
 *
 * @note Invoked from the dispatch site (typically ISR context) after
 * enumeration completes successfully.
 */
typedef void (*ra_usb_hcdc_attach_fn_t)(void* ctx, const ra_usb_hcdc_device_t* device);

/* =============================================================================
 * Lifecycle
 * =============================================================================
 */

/**
 * @brief Bring up the host-CDC driver on a chosen USB controller.
 *
 * @details
 * Initialises the underlying `ra_usb` driver in HOST mode for
 * `speed`, programs the DCP for 64-byte EP0 control transfers,
 * leaves the bus in the "wait for attach" state (UACT cleared), and
 * arms the internal enumeration step machine.
 *
 * The `ra_usb_dispatch` callback path drives enumeration once a
 * device attaches: detection from `SYSSTS0.LNST`, then the chapter-9
 * sequence (bus reset, SET_ADDRESS, GET_DEVICE_DESCRIPTOR,
 * GET_CONFIG_DESCRIPTOR, SET_CONFIG, SET_INTERFACE), then the
 * CDC-class descriptor walk.
 *
 * @param[in] speed Which USB controller (FS or HS).
 *
 * @return `ra_err_t` error code.
 * @retval k_ra_ok Host-CDC ready, awaiting attach.
 * @retval k_ra_err_invalid_arg `speed` out of range.
 * @retval k_ra_err_hw_init_failed Underlying `ra_usb_host_init`
 *         failed.
 *
 * @pre Single-threaded init context.
 * @pre `ra_mstp_init` and `ra_pwr_init` already ran.
 *
 * @post `ra_usb_host_init` succeeded for `speed`.
 * @post Internal step machine armed; attach callback has not fired
 *       yet.
 *
 * @note Not thread-safe.
 * @see ra_usb_hcdc_attach_callback
 * @see ra_usb_hcdc_close
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_usb_hcdc_init(ra_usb_speed_t speed);

/**
 * @brief Tear down the host-CDC driver and release the controller.
 *
 * @return `ra_err_t` error code.
 * @retval k_ra_ok Released.
 * @retval k_ra_err_invalid_state Driver was never initialized.
 *
 * @pre Single-threaded shutdown context.
 *
 * @post `ra_usb_host_deinit` ran; SOF generation halted; bus power
 *       dropped.
 * @post Subsequent host-CDC API calls return
 *       `k_ra_err_invalid_state`.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_usb_hcdc_close(void);

/* =============================================================================
 * Attach callback
 * =============================================================================
 */

/**
 * @brief Register (or detach) the attach callback.
 *
 * @details
 * The supplied callback fires exactly once per attach event, after
 * the descriptor walk identifies a CDC-ACM control + data interface
 * pair on the attached device. Pass `NULL` to detach.
 *
 * @param[in] on_attach Callback. NULL detaches.
 * @param[in] ctx Context pointer threaded back into `on_attach`.
 *
 * @return `ra_err_t` error code.
 * @retval k_ra_ok Callback installed.
 * @retval k_ra_err_invalid_state Driver was never initialized.
 *
 * @pre `ra_usb_hcdc_init` has run.
 *
 * @post On a subsequent attach, `on_attach(ctx, &device)` fires once.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_usb_hcdc_attach_callback(ra_usb_hcdc_attach_fn_t on_attach, void* ctx);

/* =============================================================================
 * Bulk byte pipe
 * =============================================================================
 */

/**
 * @brief Send a chunk of bytes OUT to the attached CDC-ACM device.
 *
 * @details
 * Mirrors `ra_usb_cdc_send` but on the host side: sources the bytes
 * from `data` and queues them on PIPE2 (bulk OUT) so the controller
 * delivers them at the next OUT token. Short / zero-length packets
 * are handled.
 *
 * @param[in] data Buffer to transmit. NULL allowed iff `len == 0`.
 * @param[in] len Byte count, 0..bulk_max_packet of the negotiated
 *                speed.
 *
 * @return `ra_err_t` error code.
 * @retval k_ra_ok Transfer queued.
 * @retval k_ra_err_invalid_state Driver not initialized, or no
 *         device attached.
 * @retval k_ra_err_invalid_arg Bad `data` / `len`.
 *
 * @pre `ra_usb_hcdc_init` succeeded.
 * @pre Attach callback already fired (a device is enumerated).
 *
 * @post Bytes will be delivered on the next bulk OUT token.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_usb_hcdc_send(const uint8_t* data, uint16_t len);

/**
 * @brief Drain a chunk of bytes IN from the attached CDC-ACM device.
 *
 * @details
 * Polling / non-blocking. Mirrors `ra_usb_cdc_recv` on the host
 * side: pulls bytes from PIPE1 (bulk IN) into `out_buf`. Returns
 * `k_ra_err_no_data` if the pipe has no bytes ready.
 *
 * @param[out] out_buf Destination buffer.
 * @param[in] max_len Capacity of `out_buf`, > 0.
 * @param[out] got_len Receives the number of bytes actually placed.
 *
 * @return `ra_err_t` error code.
 * @retval k_ra_ok Bytes drained; `*got_len` reflects the count.
 * @retval k_ra_err_no_data Pipe was empty.
 * @retval k_ra_err_invalid_state Driver not initialized, or no
 *         device attached.
 * @retval k_ra_err_invalid_arg Bad `out_buf` / `max_len` / `got_len`.
 *
 * @pre `ra_usb_hcdc_init` succeeded.
 * @pre Attach callback already fired.
 * @pre `out_buf`, `got_len` non-NULL, `max_len > 0`.
 *
 * @post On success `*got_len` reflects the actual byte count.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_usb_hcdc_recv(uint8_t* out_buf, uint16_t max_len, uint16_t* got_len);

/* =============================================================================
 * Class control transfer
 * =============================================================================
 */

/**
 * @brief Issue SET_LINE_CODING to the attached CDC-ACM device.
 *
 * @details
 * Builds a 7-byte SET_LINE_CODING payload (USB CDC PSTN spec rev
 * 1.20 table 17), formats the matching SETUP packet, and hands it
 * to `ra_usb_host_setup_request`. The driver's enumeration step
 * machine handles the data + status stages.
 *
 * @param[in] baud Bits/sec (e.g. 9600, 115200, 921600).
 * @param[in] parity Parity selection.
 * @param[in] stop_bits Stop-bit selection.
 *
 * @return `ra_err_t` error code.
 * @retval k_ra_ok Control transfer queued.
 * @retval k_ra_err_invalid_state Driver not initialized, or no
 *         device attached.
 * @retval k_ra_err_invalid_arg `parity` / `stop_bits` out of range
 *         or `baud` is zero.
 * @retval k_ra_err_busy A control transfer is already in flight.
 *
 * @pre `ra_usb_hcdc_init` succeeded.
 * @pre Attach callback already fired.
 *
 * @post DCP control transfer queued at the controller.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_usb_hcdc_set_line_coding(uint32_t                baud,
                                                   ra_usb_hcdc_parity_t    parity,
                                                   ra_usb_hcdc_stop_bits_t stop_bits);

/* =============================================================================
 * Test / introspection helpers
 * =============================================================================
 */

/**
 * @brief Drive the enumeration step machine forward by one step.
 *
 * @details
 * Test / debug entry point. The production path drives this from the
 * `ra_usb_dispatch` callback when the controller fires a CTRT or
 * BRDY interrupt; tests call it directly to walk the state machine
 * deterministically.
 *
 * @return `ra_err_t` error code.
 * @retval k_ra_ok Step advanced.
 * @retval k_ra_err_invalid_state Driver not initialized.
 *
 * @pre `ra_usb_hcdc_init` succeeded.
 *
 * @post Internal enumeration step counter advances by one. When the
 *       step machine reaches the terminal state, the registered
 *       attach callback fires.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_usb_hcdc_step(void);

#ifdef __cplusplus
}
#endif
