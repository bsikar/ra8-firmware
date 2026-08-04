/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_usb_pvnd.h
 * @brief Native USB device-side Vendor-defined class layer
 * @ingroup grp_hal_usb
 *
 * @details
 * Glues the device-mode `ra8_usb` controller driver to a vendor-defined
 * USB function so the EK-RA8D2 enumerates with class code 0xFF, ready
 * to carry application-specific protocols over raw bulk pipes (LibUSB,
 * WinUSB, custom firmware loaders, board-debug bridges, etc.). The
 * implementation is from-scratch; FSP `r_usb_vendor_descriptor.c.template`
 * is reference material only -- nothing is pulled in verbatim. Mirrors
 * the surface that FSP's peripheral-Vendor descriptor template
 * implies:
 *
 *  - Caller-supplied descriptor blob (configuration + interface +
 *    bulk EP descriptors).
 *  - Bulk-IN pipe (PIPE5) for outbound application data.
 *  - Bulk-OUT pipe (PIPE1) for inbound application data.
 *  - No defined class requests; everything goes through the optional
 *    application setup callback as a vendor-recipient SETUP envelope
 *    (USB 2.0 sec 9.3 "USB Device Requests").
 *
 * Reference: USB 2.0 sec 9.3 "USB Device Requests" (vendor request
 * envelope encoding) and USB 2.0 Class Codes registry, class 0xFF =
 * Vendor Specific.
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
 * @enum ra8_usb_pvnd_pipe_t
 * @brief PIPE numbers used by the device-Vendor function for the local
 *        bulk endpoints.
 *
 * @details FSP / RA8D2 PIPE assignment rules constrain bulk pipes to
 * PIPE1..PIPE5. PIPE5 carries bulk IN (device -> host), PIPE1 carries
 * bulk OUT (host -> device). Pipe5 is chosen so a single controller
 * can host MSC (PIPE3/4), CDC (PIPE1/2/6), and Vendor on PIPE5 / PIPE1
 * without colliding.
 */
typedef enum : uint8_t {
  k_ra8_pvnd_pipe_bulk_in  = 5U, /**< PIPE5 -> EP1 IN  (vendor IN).  */
  k_ra8_pvnd_pipe_bulk_out = 1U, /**< PIPE1 -> EP2 OUT (vendor OUT). */
} ra8_usb_pvnd_pipe_t;

/**
 * @enum ra8_usb_pvnd_ep_t
 * @brief USB endpoint addresses used by the device-Vendor function.
 */
typedef enum : uint8_t {
  k_ra8_pvnd_ep_bulk_in_addr  = 1U, /**< EP1 IN  address. */
  k_ra8_pvnd_ep_bulk_out_addr = 2U, /**< EP2 OUT address. */
} ra8_usb_pvnd_ep_t;

/**
 * @enum ra8_usb_pvnd_packet_t
 * @brief Packet sizing for the bulk endpoints.
 *
 * @details Per USB 2.0 sec 5.8.3 "Bulk Transfer Packet Size Constraints",
 * FS bulk = 8/16/32/64 bytes, HS bulk = 512 bytes.
 */
typedef enum : uint16_t {
  k_ra8_pvnd_bulk_max_packet_fs = 64U,  /**< Bulk size at FS. */
  k_ra8_pvnd_bulk_max_packet_hs = 512U, /**< Bulk size at HS. */
} ra8_usb_pvnd_packet_t;

/**
 * @enum ra8_usb_pvnd_class_t
 * @brief Class / subclass / protocol triplet that identifies the local
 *        Vendor function in the configuration descriptor.
 *
 * @details USB-IF Class Codes registry: 0xFF = vendor-specific. Subclass
 * and protocol are wholly application-defined.
 */
typedef enum : uint8_t {
  k_ra8_pvnd_class_vendor = 0xFFU, /**< Vendor-specific interface class. */
} ra8_usb_pvnd_class_t;

/**
 * @enum ra8_usb_pvnd_envelope_t
 * @brief `bmRequestType` envelopes that are vendor-recipient SETUPs.
 *
 * @details Per USB 2.0 sec 9.3 "USB Device Requests": the type field is
 * 0b10 (vendor) and the recipient field varies (0=device, 1=interface,
 * 2=endpoint). The class layer accepts all four common combinations.
 */
typedef enum : uint8_t {
  k_ra8_pvnd_bm_vendor_dev_in    = 0xC0U, /**< Vendor | Device    | In.  */
  k_ra8_pvnd_bm_vendor_dev_out   = 0x40U, /**< Vendor | Device    | Out. */
  k_ra8_pvnd_bm_vendor_iface_in  = 0xC1U, /**< Vendor | Interface | In.  */
  k_ra8_pvnd_bm_vendor_iface_out = 0x41U, /**< Vendor | Interface | Out. */
  k_ra8_pvnd_bm_vendor_ep_in     = 0xC2U, /**< Vendor | Endpoint  | In.  */
  k_ra8_pvnd_bm_vendor_ep_out    = 0x42U, /**< Vendor | Endpoint  | Out. */
} ra8_usb_pvnd_envelope_t;

/**
 * @typedef ra8_usb_pvnd_setup_fn_t
 * @brief Caller-supplied vendor class-setup handler signature.
 *
 * @details The class layer pre-decodes the SETUP envelope (validates
 * it really is a vendor-recipient request) and hands it to the
 * application. Vendor requests have no spec-defined semantics; the
 * application owns the entire request set.
 *
 * @param[in] ctx Caller-supplied context registered alongside the
 *                handler.
 * @param[in] setup The 8-byte SETUP envelope (vendor request).
 *
 * @return `ra8_err_t` error code; `k_ra8_ok` lets the class layer ACK the
 *         status stage, anything else stalls EP0.
 *
 * @note Invoked from the dispatch site (typically ISR context) when a
 *       vendor SETUP lands.
 */
typedef ra8_err_t (*ra8_usb_pvnd_setup_fn_t)(void* ctx, const ra8_usb_setup_t* setup);

/* =============================================================================
 * Lifecycle
 * =============================================================================
 */

/**
 * @brief Bring up the device-Vendor function on a chosen USB controller.
 *
 * @details
 * Initialises the underlying `ra8_usb` driver in DEVICE mode for
 * `speed`, configures PIPE5 (bulk IN) and PIPE1 (bulk OUT), and leaves
 * the D+ pull-up dropped. The caller raises it via
 * `ra8_usb_device_attach` once descriptors are set.
 *
 * @param[in] speed Which USB controller (FS or HS).
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok Device-Vendor ready.
 * @retval k_ra8_err_invalid_arg `speed` out of range.
 * @retval k_ra8_err_hw_init_failed Underlying `ra8_usb_device_init` failed.
 *
 * @pre Single-threaded init context.
 * @pre `ra8_mstp_init` and `ra8_pwr_init` already ran.
 *
 * @post Internal state machine armed; descriptor pointer cleared.
 * @post Pipe5 / pipe1 configured at the speed's default packet size.
 *
 * @note Not thread-safe.
 * @see ra8_usb_pvnd_set_descriptors
 * @see ra8_usb_pvnd_close
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_usb_pvnd_init(ra8_usb_speed_t speed);

/**
 * @brief Tear down the device-Vendor function and release the controller.
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok Released.
 * @retval k_ra8_err_invalid_state Driver was never initialized.
 *
 * @pre Single-threaded shutdown context.
 *
 * @post `ra8_usb_device_deinit` ran; D+ pull-up dropped; subsequent
 *       device-Vendor API calls return `k_ra8_err_invalid_state`.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_usb_pvnd_close(void);

/* =============================================================================
 * Descriptor handoff
 * =============================================================================
 */

/**
 * @brief Install the caller-supplied descriptor blob.
 *
 * @details The blob is concatenation of configuration / interface /
 * bulk EP descriptors. The class layer keeps just a pointer + length
 * pair; the caller owns the storage.
 *
 * @param[in] desc      Pointer to the caller-owned descriptor blob.
 * @param[in] desc_len  Byte length of `desc`.
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok Pointer + length stored.
 * @retval k_ra8_err_invalid_state Driver not initialized.
 * @retval k_ra8_err_null_ptr `desc` was NULL.
 * @retval k_ra8_err_invalid_arg `desc_len == 0`.
 *
 * @pre `ra8_usb_pvnd_init` succeeded.
 *
 * @post GET_DESCRIPTOR(Configuration) requests can use this blob.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_usb_pvnd_set_descriptors(const uint8_t* desc, uint16_t desc_len);

/* =============================================================================
 * Vendor data transport
 * =============================================================================
 */

/**
 * @brief Push a payload on the vendor bulk-IN endpoint.
 *
 * @param[in] data Payload buffer.
 * @param[in] len  Payload byte length.
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok Bytes queued onto bulk-IN.
 * @retval k_ra8_err_invalid_state Driver not initialized.
 * @retval k_ra8_err_null_ptr `data` was NULL with `len > 0`.
 * @retval k_ra8_err_invalid_arg `len == 0` or larger than the pipe max.
 *
 * @pre `ra8_usb_pvnd_init` succeeded.
 *
 * @post `len` bytes sit on PIPE5.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_usb_pvnd_send(const uint8_t* data, uint16_t len);

/**
 * @brief Drain a payload from the vendor bulk-OUT endpoint.
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
 * @pre `ra8_usb_pvnd_init` succeeded.
 *
 * @post On success `*got_len` reflects the actual byte count.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_usb_pvnd_recv(uint8_t* buf, uint16_t max_len, uint16_t* got_len);

/* =============================================================================
 * Setup-handler attach
 * =============================================================================
 */

/**
 * @brief Register the application's vendor setup handler.
 *
 * @details Pass `NULL` for `setup_fn` to detach. The class layer
 * pre-decodes the SETUP envelope and forwards it if its
 * `bmRequestType` indicates a vendor-recipient request.
 *
 * @param[in] setup_fn Application's handler. NULL detaches.
 * @param[in] ctx Context pointer threaded back into `setup_fn`.
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok Handler installed.
 * @retval k_ra8_err_invalid_state Driver not initialized.
 *
 * @pre `ra8_usb_pvnd_init` succeeded.
 *
 * @post On the next vendor SETUP, `setup_fn(ctx, &setup)` fires.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_usb_pvnd_attach_setup_handler(ra8_usb_pvnd_setup_fn_t setup_fn,
                                                          void*                   ctx);

/* =============================================================================
 * Vendor SETUP dispatch
 * =============================================================================
 */

/**
 * @brief Process a vendor-specific SETUP packet on EP0.
 *
 * @details
 * The class layer accepts every `bmRequestType` whose type field is
 * 0b10 (vendor) and forwards the SETUP to the registered application
 * handler. If no handler is registered, the SETUP is stalled.
 * Standard / class SETUPs are rejected with `k_ra8_err_not_supported`
 * so the caller can fall back to its own standard / class handler.
 *
 * @param[in] setup The SETUP packet returned by `ra8_usb_read_setup_if_valid`.
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok SETUP handled (status stage queued internally).
 * @retval k_ra8_err_invalid_state Driver not initialized.
 * @retval k_ra8_err_null_ptr `setup` was NULL.
 * @retval k_ra8_err_not_supported `bmRequestType` is not a vendor envelope.
 *
 * @pre `ra8_usb_pvnd_init` succeeded.
 *
 * @post Internal state may have been updated by the application
 *       callback.
 *
 * @note Call from the `CTRT` ISR path of `ra8_usb`.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_usb_pvnd_handle_setup(const ra8_usb_setup_t* setup);

#ifdef __cplusplus
}
#endif
