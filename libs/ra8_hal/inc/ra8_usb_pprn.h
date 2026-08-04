/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_usb_pprn.h
 * @brief Native USB device-side Printer class layer
 * @ingroup grp_hal_usb
 *
 * @details
 * Glues the device-mode `ra8_usb` controller driver to a USB Printer
 * class function so the EK-RA8D2 enumerates as a uni-directional or
 * bi-directional printer gadget. The implementation is from-scratch;
 * FSP `r_usb_pprn_driver.c` is reference material only -- nothing is
 * pulled in verbatim. Mirrors the surface that FSP's peripheral-Printer
 * class exposes:
 *
 *  - Caller-supplied descriptor blob (configuration + interface +
 *    bulk EP descriptors).
 *  - Bulk-OUT pipe (PIPE3) for inbound print-job data.
 *  - Bulk-IN pipe (PIPE4) for outbound printer status (bi-directional
 *    interfaces only; protocol 0x02 / 0x03).
 *  - Caller-supplied class-setup callback so the application answers
 *    GET_DEVICE_ID, GET_PORT_STATUS, and SOFT_RESET (USB Printer Class
 *    1.1 sec 4.2 "Class Specific Requests").
 *  - Local `port_status` shadow (paper-out / select / not-error bits)
 *    that the application updates and the class layer can hand back
 *    on GET_PORT_STATUS without re-entering the application.
 *
 * Reference: USB Device Class Definition for Printing Devices revision
 * 1.1 (USB-IF, 2000-01-25).
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
 * @enum ra8_usb_pprn_pipe_t
 * @brief PIPE numbers used by the device-Printer function for the local
 *        bulk endpoints.
 *
 * @details FSP / RA8D2 PIPE assignment rules constrain bulk pipes to
 * PIPE1..PIPE5. PIPE3 carries bulk OUT (host -> printer print data),
 * PIPE4 carries bulk IN (printer -> host status, bi-directional only).
 * USB Printer 1.1 sec 4.1.1 "Endpoint Descriptors".
 */
typedef enum : uint8_t {
  k_ra8_pprn_pipe_bulk_out = 3U, /**< PIPE3 -> EP1 OUT (print data). */
  k_ra8_pprn_pipe_bulk_in  = 4U, /**< PIPE4 -> EP2 IN  (status).     */
} ra8_usb_pprn_pipe_t;

/**
 * @enum ra8_usb_pprn_ep_t
 * @brief USB endpoint addresses used by the device-Printer function.
 */
typedef enum : uint8_t {
  k_ra8_pprn_ep_bulk_out_addr = 1U, /**< EP1 OUT address. */
  k_ra8_pprn_ep_bulk_in_addr  = 2U, /**< EP2 IN  address. */
} ra8_usb_pprn_ep_t;

/**
 * @enum ra8_usb_pprn_packet_t
 * @brief Packet sizing for the bulk endpoints.
 *
 * @details Per USB 2.0 sec 5.8.3, FS bulk = 8/16/32/64 bytes and HS
 * bulk = 512 bytes.
 */
typedef enum : uint16_t {
  k_ra8_pprn_bulk_max_packet_fs = 64U,  /**< Bulk size at FS. */
  k_ra8_pprn_bulk_max_packet_hs = 512U, /**< Bulk size at HS. */
} ra8_usb_pprn_packet_t;

/**
 * @enum ra8_usb_pprn_class_t
 * @brief Class / subclass / protocol triplet that identifies the local
 *        Printer function in the configuration descriptor.
 *
 * @details Per USB Printer 1.1 sec 4.1 "Standard Descriptor Definition".
 * Subclass 1 is the only defined value. Protocol 1 = uni-directional
 * (OUT only), 2 = bi-directional, 3 = IEEE 1284.4 bi-directional.
 */
typedef enum : uint8_t {
  k_ra8_pprn_class_printer    = 0x07U, /**< Printer interface class. */
  k_ra8_pprn_subclass_default = 0x01U, /**< Printers subclass.       */
  k_ra8_pprn_protocol_unidir  = 0x01U, /**< Uni-directional.         */
  k_ra8_pprn_protocol_bidir   = 0x02U, /**< Bi-directional.          */
  k_ra8_pprn_protocol_1284_4  = 0x03U, /**< IEEE 1284.4 bi-dir.      */
} ra8_usb_pprn_class_t;

/**
 * @enum ra8_usb_pprn_request_t
 * @brief Printer class-specific request codes the host issues to the
 *        device.
 *
 * @details Per USB Printer 1.1 sec 4.2 "Class Specific Requests". The
 * three values are the entirety of the printer class request set.
 */
typedef enum : uint8_t {
  k_ra8_pprn_req_get_device_id   = 0x00U, /**< GET_DEVICE_ID.   */
  k_ra8_pprn_req_get_port_status = 0x01U, /**< GET_PORT_STATUS. */
  k_ra8_pprn_req_soft_reset      = 0x02U, /**< SOFT_RESET.      */
} ra8_usb_pprn_request_t;

/**
 * @enum ra8_usb_pprn_port_status_bit_t
 * @brief Bit positions in the 1-byte GET_PORT_STATUS response.
 *
 * @details Per USB Printer 1.1 sec 4.2.2 "GET_PORT_STATUS". Bits 0..2
 * are reserved-as-0, bits 3 / 4 / 5 carry the printer-state flags.
 */
typedef enum : uint8_t {
  k_ra8_pprn_status_bit_paper_empty = 5U, /**< 1 = paper empty.    */
  k_ra8_pprn_status_bit_select      = 4U, /**< 1 = printer online. */
  k_ra8_pprn_status_bit_not_error   = 3U, /**< 1 = no error.       */
} ra8_usb_pprn_port_status_bit_t;

/**
 * @typedef ra8_usb_pprn_setup_fn_t
 * @brief Caller-supplied printer class-setup handler signature.
 *
 * @details The class layer pre-decodes the SETUP envelope (validates
 * it really is a printer class request on the printer interface) and
 * hands the SETUP packet to the application. The application either
 * lets the default handler answer GET_PORT_STATUS / SOFT_RESET from
 * the local shadow, or installs custom logic (e.g. queueing a
 * device-ID string for GET_DEVICE_ID).
 *
 * @param[in] ctx Caller-supplied context registered alongside the
 *                handler.
 * @param[in] setup The 8-byte SETUP envelope (class request).
 *
 * @return `ra8_err_t` error code; `k_ra8_ok` lets the class layer ACK the
 *         status stage, anything else stalls EP0.
 *
 * @note Invoked from the dispatch site (typically ISR context) when a
 *       printer class SETUP lands.
 */
typedef ra8_err_t (*ra8_usb_pprn_setup_fn_t)(void* ctx, const ra8_usb_setup_t* setup);

/* =============================================================================
 * Lifecycle
 * =============================================================================
 */

/**
 * @brief Bring up the device-Printer function on a chosen USB controller.
 *
 * @details
 * Initialises the underlying `ra8_usb` driver in DEVICE mode for
 * `speed`, configures PIPE3 (bulk OUT) and PIPE4 (bulk IN), seeds the
 * port-status shadow to "select | not-error" (online, no error, no
 * paper-out), and leaves the D+ pull-up dropped. The caller raises
 * it via `ra8_usb_device_attach` once descriptors are set.
 *
 * @param[in] speed Which USB controller (FS or HS).
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok Device-Printer ready.
 * @retval k_ra8_err_invalid_arg `speed` out of range.
 * @retval k_ra8_err_hw_init_failed Underlying `ra8_usb_device_init`
 *         failed.
 *
 * @pre Single-threaded init context.
 * @pre `ra8_mstp_init` and `ra8_pwr_init` already ran.
 *
 * @post Internal state machine armed; descriptor pointer cleared.
 * @post Pipe3 / pipe4 configured at the speed's default packet size.
 *
 * @note Not thread-safe.
 * @see ra8_usb_pprn_set_descriptors
 * @see ra8_usb_pprn_close
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_usb_pprn_init(ra8_usb_speed_t speed);

/**
 * @brief Tear down the device-Printer function and release the controller.
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok Released.
 * @retval k_ra8_err_invalid_state Driver was never initialized.
 *
 * @pre Single-threaded shutdown context.
 *
 * @post `ra8_usb_device_deinit` ran; D+ pull-up dropped; subsequent
 *       device-Printer API calls return `k_ra8_err_invalid_state`.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_usb_pprn_close(void);

/* =============================================================================
 * Descriptor + identity handoff
 * =============================================================================
 */

/**
 * @brief Install the caller-supplied descriptor blob and IEEE 1284
 *        device-ID string.
 *
 * @details
 * The descriptor blob is the configuration / interface / EP descriptor
 * triplet (or quadruplet for bi-directional). The device-ID string is
 * the IEEE 1284-style device-ID payload returned by GET_DEVICE_ID;
 * USB Printer 1.1 sec 4.2.1 "GET_DEVICE_ID" specifies a 2-byte big-endian
 * length followed by the device-ID string. Pass `device_id == NULL` /
 * `device_id_len == 0` if the caller will instead handle GET_DEVICE_ID
 * via its setup callback.
 *
 * @param[in] desc            Pointer to the caller-owned descriptor blob.
 * @param[in] desc_len        Byte length of `desc`.
 * @param[in] device_id       Pointer to the IEEE 1284 device-ID payload
 *                             (caller-owned, may be NULL).
 * @param[in] device_id_len   Byte length of `device_id`.
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok Pointers + lengths stored.
 * @retval k_ra8_err_invalid_state Driver not initialized.
 * @retval k_ra8_err_null_ptr `desc` was NULL.
 * @retval k_ra8_err_invalid_arg `desc_len == 0`, or
 *         `(device_id != NULL) ^ (device_id_len > 0)`.
 *
 * @pre `ra8_usb_pprn_init` succeeded.
 *
 * @post GET_DESCRIPTOR(Configuration) and GET_DEVICE_ID requests are
 *       served from these buffers.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_usb_pprn_set_descriptors(const uint8_t* desc,
                                                     uint16_t       desc_len,
                                                     const uint8_t* device_id,
                                                     uint16_t       device_id_len);

/* =============================================================================
 * Print data transport
 * =============================================================================
 */

/**
 * @brief Drain inbound print-job data from the bulk-OUT endpoint.
 *
 * @param[out] buf      Receive buffer.
 * @param[in]  max_len  Capacity of `buf`, > 0.
 * @param[out] got_len  Receives the number of bytes actually placed.
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok Bytes drained.
 * @retval k_ra8_err_no_data Pipe was empty.
 * @retval k_ra8_err_null_ptr `buf` or `got_len` was NULL.
 * @retval k_ra8_err_invalid_state Driver not initialized.
 * @retval k_ra8_err_invalid_arg `max_len == 0`.
 *
 * @pre `ra8_usb_pprn_init` succeeded.
 *
 * @post On success `*got_len` reflects the actual byte count.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_usb_pprn_recv(uint8_t* buf, uint16_t max_len, uint16_t* got_len);

/**
 * @brief Push outbound printer-status bytes on the bulk-IN endpoint.
 *
 * @details Only meaningful on a bi-directional printer interface
 * (protocol 0x02 / 0x03). Uni-directional printers should ignore this
 * function.
 *
 * @param[in] data Status payload.
 * @param[in] len  Payload length.
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok Bytes queued onto bulk-IN.
 * @retval k_ra8_err_invalid_state Driver not initialized.
 * @retval k_ra8_err_null_ptr `data` was NULL with `len > 0`.
 * @retval k_ra8_err_invalid_arg `len == 0` or larger than the pipe max.
 *
 * @pre `ra8_usb_pprn_init` succeeded.
 *
 * @post `len` bytes sit on PIPE4.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_usb_pprn_send(const uint8_t* data, uint16_t len);

/* =============================================================================
 * Port-status shadow
 * =============================================================================
 */

/**
 * @brief Update the local port-status byte returned by GET_PORT_STATUS.
 *
 * @details The shape of the byte is documented in
 * `ra8_usb_pprn_port_status_bit_t`. The application typically calls this
 * from its job-scheduler (e.g. when the paper sensor flips state).
 *
 * @param[in] status_byte The new port-status byte.
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok Shadow updated.
 * @retval k_ra8_err_invalid_state Driver not initialized.
 *
 * @pre `ra8_usb_pprn_init` succeeded.
 *
 * @post `ra8_usb_pprn_get_port_status` reflects the new value.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_usb_pprn_set_port_status(uint8_t status_byte);

/**
 * @brief Read the current port-status byte.
 *
 * @param[out] out_status Receives the current status byte.
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok Value copied.
 * @retval k_ra8_err_invalid_state Driver not initialized.
 * @retval k_ra8_err_null_ptr `out_status` was NULL.
 *
 * @pre `out_status` non-NULL.
 *
 * @post No internal state mutated.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_usb_pprn_get_port_status(uint8_t* out_status);

/* =============================================================================
 * Setup-handler attach
 * =============================================================================
 */

/**
 * @brief Register the application's printer class-setup handler.
 *
 * @details Pass `NULL` for `setup_fn` to detach. The class layer
 * pre-decodes the SETUP envelope and forwards it if its
 * `bmRequestType` indicates a class-recipient-interface request and
 * its `bRequest` is one of GET_DEVICE_ID / GET_PORT_STATUS / SOFT_RESET.
 *
 * @param[in] setup_fn Application's handler. NULL detaches.
 * @param[in] ctx Context pointer threaded back into `setup_fn`.
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok Handler installed.
 * @retval k_ra8_err_invalid_state Driver not initialized.
 *
 * @pre `ra8_usb_pprn_init` succeeded.
 *
 * @post On the next class SETUP, `setup_fn(ctx, &setup)` fires.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_usb_pprn_attach_setup_handler(ra8_usb_pprn_setup_fn_t setup_fn,
                                                          void*                   ctx);

/* =============================================================================
 * Class SETUP dispatch
 * =============================================================================
 */

/**
 * @brief Process a class-specific SETUP packet on EP0.
 *
 * @details
 * Switches on `b_request`: SOFT_RESET re-arms the bulk pipes
 * internally; GET_DEVICE_ID / GET_PORT_STATUS forward to the
 * registered application handler if any (the application normally
 * queues the device-ID payload or the port-status byte itself).
 * Standard (non-class) SETUPs are rejected with
 * `k_ra8_err_not_supported`.
 *
 * @param[in] setup The SETUP packet returned by `ra8_usb_read_setup_if_valid`.
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok SETUP handled (status stage queued internally).
 * @retval k_ra8_err_invalid_state Driver not initialized.
 * @retval k_ra8_err_null_ptr `setup` was NULL.
 * @retval k_ra8_err_not_supported `bRequest` is not a printer class
 *         request.
 *
 * @pre `ra8_usb_pprn_init` succeeded.
 *
 * @post Internal port-status shadow may have been refreshed by the
 *       application callback.
 *
 * @note Call from the `CTRT` ISR path of `ra8_usb`.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_usb_pprn_handle_setup(const ra8_usb_setup_t* setup);

#ifdef __cplusplus
}
#endif
