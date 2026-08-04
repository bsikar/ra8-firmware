/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_usb_phid.h
 * @brief Native USB device-side HID (Human Interface Device) class layer
 * @ingroup grp_hal_usb
 *
 * @details
 * Glues the device-mode `ra8_usb` controller driver to a USB HID
 * function so the host enumerates the EK-RA8D2 board as a keyboard,
 * mouse, gamepad, or vendor-defined HID gadget. The implementation is
 * from-scratch -- no FSP `r_usb_phid_driver.c`, no CherryUSB
 * `usbd_hid.c`, no TinyUSB binaries pulled in. It mirrors the surface
 * that FSP's peripheral-HID class exposes:
 *
 *  - Caller-supplied HID Report Descriptor + HID class descriptor.
 *  - Interrupt-IN endpoint to push input reports.
 *  - Interrupt-OUT endpoint (optional) to pull output reports, plus
 *    SET_REPORT-on-EP0 fallback.
 *  - Idle-rate / protocol shadow updated by SET_IDLE / SET_PROTOCOL.
 *  - Caller-supplied class-setup callback so the application chooses
 *    how to answer GET_REPORT / SET_REPORT / GET_IDLE / SET_IDLE /
 *    GET_PROTOCOL / SET_PROTOCOL (a mouse driver answers GET_REPORT
 *    with the current cursor delta; a keyboard answers with the
 *    current scancode set; a generic HID gadget defers to its
 *    application logic).
 *
 * The descriptor table the application installs typically advertises:
 *
 *  - One configuration with one interface, class = 0x03 (HID).
 *  - EP0 64-byte default control pipe (provided by `ra8_usb`).
 *  - EP1 IN, interrupt, 8..64 bytes (HID input reports).
 *  - EP2 OUT, interrupt, 8..64 bytes (HID output reports, optional).
 *
 * Reference: USB Device Class Definition for Human Interface Devices
 * (HID) revision 1.11 (USB-IF, 2001-06-27).
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
 * @enum ra8_usb_phid_pipe_t
 * @brief PIPE numbers used by the device-HID function for the local
 *        interrupt endpoints.
 *
 * @details FSP / RA8D2 PIPE assignment rules constrain interrupt pipes
 * to PIPE6..PIPE9. Device-side HID uses PIPE6 for input reports
 * (interrupt IN) and PIPE7 for output reports (interrupt OUT). See
 * USB HID 1.11 sec 4.4 "Interrupt Pipes".
 */
typedef enum : uint8_t {
  k_ra8_phid_pipe_intr_in  = 6U, /**< PIPE6 -> EP1 IN  (intr). */
  k_ra8_phid_pipe_intr_out = 7U, /**< PIPE7 -> EP2 OUT (intr). */
} ra8_usb_phid_pipe_t;

/**
 * @enum ra8_usb_phid_ep_t
 * @brief USB endpoint addresses used by the device-HID function.
 */
typedef enum : uint8_t {
  k_ra8_phid_ep_intr_in_addr  = 1U, /**< EP1 IN address.  */
  k_ra8_phid_ep_intr_out_addr = 2U, /**< EP2 OUT address. */
} ra8_usb_phid_ep_t;

/**
 * @enum ra8_usb_phid_packet_t
 * @brief Packet sizing for the device-HID interrupt endpoints.
 *
 * @details Per USB HID 1.11 sec 8.2 "Maximum Packet Size", a low- /
 * full-speed HID interrupt endpoint is at most 64 bytes, and a
 * high-speed one is at most 1024 bytes. Boot-protocol keyboard /
 * mouse devices use 8.
 */
typedef enum : uint16_t {
  k_ra8_phid_intr_max_packet_default = 8U,    /**< Boot-protocol default. */
  k_ra8_phid_intr_max_packet_fs      = 64U,   /**< FS ceiling.            */
  k_ra8_phid_intr_max_packet_hs      = 1024U, /**< HS ceiling.            */
} ra8_usb_phid_packet_t;

/**
 * @enum ra8_usb_phid_class_t
 * @brief Class / subclass / protocol triplet that identifies the local
 *        HID function in the configuration descriptor.
 *
 * @details Numbered from the USB-IF "Class Codes" registry and USB
 * HID 1.11 sec 4.2 "Subclass". Subclass 1 = "Boot Interface
 * Subclass", protocol 1 = keyboard, protocol 2 = mouse.
 */
typedef enum : uint8_t {
  k_ra8_phid_class_hid         = 0x03U, /**< HID interface class.     */
  k_ra8_phid_subclass_none     = 0x00U, /**< No subclass.             */
  k_ra8_phid_subclass_boot     = 0x01U, /**< Boot Interface Subclass. */
  k_ra8_phid_protocol_other    = 0x00U, /**< Non keyboard / mouse.    */
  k_ra8_phid_protocol_keyboard = 0x01U, /**< Boot keyboard.           */
  k_ra8_phid_protocol_mouse    = 0x02U, /**< Boot mouse.              */
} ra8_usb_phid_class_t;

/**
 * @enum ra8_usb_phid_request_t
 * @brief HID class-specific request codes the host issues to the
 *        device.
 *
 * @details Per USB HID 1.11 sec 7.2 "Class-Specific Requests". Numeric
 * values match the standard HID class request register so a static
 * assertion in the test suite can pin them to the spec.
 */
typedef enum : uint8_t {
  k_ra8_phid_req_get_report   = 0x01U, /**< GET_REPORT.   */
  k_ra8_phid_req_get_idle     = 0x02U, /**< GET_IDLE.     */
  k_ra8_phid_req_get_protocol = 0x03U, /**< GET_PROTOCOL. */
  k_ra8_phid_req_set_report   = 0x09U, /**< SET_REPORT.   */
  k_ra8_phid_req_set_idle     = 0x0AU, /**< SET_IDLE.     */
  k_ra8_phid_req_set_protocol = 0x0BU, /**< SET_PROTOCOL. */
} ra8_usb_phid_request_t;

/**
 * @enum ra8_usb_phid_report_type_t
 * @brief HID report-type selector encoded in `wValue`'s high byte for
 *        GET_REPORT / SET_REPORT.
 *
 * @details Per USB HID 1.11 sec 7.2.1 "Get_Report Request".
 */
typedef enum : uint8_t {
  k_ra8_phid_report_type_input   = 0x01U, /**< Input report.   */
  k_ra8_phid_report_type_output  = 0x02U, /**< Output report.  */
  k_ra8_phid_report_type_feature = 0x03U, /**< Feature report. */
} ra8_usb_phid_report_type_t;

/**
 * @enum ra8_usb_phid_protocol_select_t
 * @brief `wValue` payload for SET_PROTOCOL.
 *
 * @details Per USB HID 1.11 sec 7.2.6 "Set_Protocol Request". 0 = boot
 * protocol, 1 = report protocol.
 */
typedef enum : uint8_t {
  k_ra8_phid_proto_boot   = 0U, /**< Boot protocol.   */
  k_ra8_phid_proto_report = 1U, /**< Report protocol. */
} ra8_usb_phid_protocol_select_t;

/**
 * @enum ra8_usb_phid_desc_t
 * @brief HID-specific descriptor types (USB HID 1.11 sec 7.1).
 */
typedef enum : uint8_t {
  k_ra8_phid_desc_hid      = 0x21U, /**< HID class descriptor.    */
  k_ra8_phid_desc_report   = 0x22U, /**< HID Report descriptor.   */
  k_ra8_phid_desc_physical = 0x23U, /**< HID Physical descriptor. */
} ra8_usb_phid_desc_t;

/**
 * @typedef ra8_usb_phid_setup_fn_t
 * @brief Caller-supplied HID class-setup handler signature.
 *
 * @details The application registers one of these via
 * `ra8_usb_phid_attach_setup_handler`. The class layer pre-decodes the
 * SETUP envelope (validating it really is a HID class request on the
 * HID interface) and hands the raw SETUP packet plus a pointer to the
 * shadow `idle_rate` / `protocol` to the application. The application
 * may modify `idle_rate` / `protocol` and / or queue a data-stage
 * payload by calling `ra8_usb_queue_in` directly.
 *
 * @param[in] ctx Caller-supplied context registered alongside the
 *                handler.
 * @param[in] setup The 8-byte SETUP envelope (class request).
 *
 * @return `ra8_err_t` error code; `k_ra8_ok` lets the class layer ACK the
 *         status stage, anything else stalls EP0.
 *
 * @note Invoked from the dispatch site (typically ISR context) when a
 * HID class SETUP lands.
 */
typedef ra8_err_t (*ra8_usb_phid_setup_fn_t)(void* ctx, const ra8_usb_setup_t* setup);

/* =============================================================================
 * Lifecycle
 * =============================================================================
 */

/**
 * @brief Bring up the device-HID function on a chosen USB controller.
 *
 * @details
 * Initialises the underlying `ra8_usb` driver in DEVICE mode for
 * `speed`, configures PIPE6 (interrupt IN) and PIPE7 (interrupt OUT),
 * resets the idle-rate / protocol shadow to spec defaults
 * (idle_rate = 0 = "report only on change", protocol = report), and
 * leaves the D+ pull-up dropped. The caller raises it via
 * `ra8_usb_device_attach` once descriptors are set.
 *
 * @param[in] speed Which USB controller (FS or HS).
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok Device-HID ready.
 * @retval k_ra8_err_invalid_arg `speed` out of range.
 * @retval k_ra8_err_hw_init_failed Underlying `ra8_usb_device_init`
 *         failed.
 *
 * @pre Single-threaded init context.
 * @pre `ra8_mstp_init` and `ra8_pwr_init` already ran.
 *
 * @post Internal state machine armed; descriptor pointers cleared.
 * @post Pipe6 / pipe7 configured at the speed's default packet size.
 *
 * @note Not thread-safe.
 * @see ra8_usb_phid_set_descriptors
 * @see ra8_usb_phid_close
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_usb_phid_init(ra8_usb_speed_t speed);

/**
 * @brief Tear down the device-HID function and release the controller.
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok Released.
 * @retval k_ra8_err_invalid_state Driver was never initialized.
 *
 * @pre Single-threaded shutdown context.
 *
 * @post `ra8_usb_device_deinit` ran; D+ pull-up dropped; subsequent
 *       device-HID API calls return `k_ra8_err_invalid_state`.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_usb_phid_close(void);

/* =============================================================================
 * Descriptor handoff
 * =============================================================================
 */

/**
 * @brief Install the caller-supplied HID Report descriptor and HID
 *        class descriptor.
 *
 * @details
 * The class layer keeps just a pointer + length pair for each
 * descriptor; the application owns the storage. When the host issues
 * GET_DESCRIPTOR(Report) on the HID interface, the class layer hands
 * the buffered Report descriptor to the EP0 data stage. When the host
 * issues GET_DESCRIPTOR(HID), the same is done for the HID class
 * descriptor.
 *
 * @param[in] report_desc Pointer to the caller-owned Report descriptor.
 * @param[in] report_desc_len Byte length of `report_desc`.
 * @param[in] hid_desc Pointer to the caller-owned HID class descriptor.
 * @param[in] hid_desc_len Byte length of `hid_desc`.
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok Pointers + lengths stored.
 * @retval k_ra8_err_invalid_state Driver not initialized.
 * @retval k_ra8_err_null_ptr Either pointer was NULL.
 * @retval k_ra8_err_invalid_arg Either length was 0.
 *
 * @pre `ra8_usb_phid_init` succeeded.
 * @pre Both pointers non-NULL, both lengths > 0.
 *
 * @post Subsequent GET_DESCRIPTOR(Report) / GET_DESCRIPTOR(HID)
 *       requests serve the supplied buffers.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_usb_phid_set_descriptors(const uint8_t* report_desc,
                                                     uint16_t       report_desc_len,
                                                     const uint8_t* hid_desc,
                                                     uint16_t       hid_desc_len);

/* =============================================================================
 * Input / output reports
 * =============================================================================
 */

/**
 * @brief Push a HID input report on the interrupt-IN endpoint.
 *
 * @details
 * If `report_id == 0` the device uses a single unnamed report and the
 * payload goes straight onto PIPE6. If `report_id != 0` the byte is
 * prepended to the payload as the spec requires (USB HID 1.11 sec 8
 * "Report Protocol"). The caller is responsible for not exceeding the
 * configured pipe max-packet.
 *
 * @param[in] report_id HID report ID (0 if device uses a single report).
 * @param[in] payload Report payload.
 * @param[in] len Payload length.
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok Bytes queued onto interrupt-IN.
 * @retval k_ra8_err_invalid_state Driver not initialized.
 * @retval k_ra8_err_null_ptr `payload` was NULL with `len > 0`.
 * @retval k_ra8_err_invalid_arg `len` zero with `report_id == 0`, or
 *         too large for the pipe max-packet.
 *
 * @pre `ra8_usb_phid_init` succeeded.
 * @pre `payload` non-NULL when `len > 0`.
 *
 * @post `len` (or `len + 1` if `report_id != 0`) bytes sit on PIPE6.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_usb_phid_send_report(uint8_t report_id, const uint8_t* payload, uint16_t len);

/**
 * @brief Drain a HID output report from the interrupt-OUT endpoint.
 *
 * @details
 * Pulls bytes off PIPE7 (interrupt OUT) into `buf`. Some HID gadgets
 * (notably keyboards announcing LED state) instead deliver output
 * reports via SET_REPORT on EP0; that path is handled by the
 * caller-installed setup callback.
 *
 * @param[in] report_id Report ID (informational; not consumed here).
 * @param[out] buf Receive buffer.
 * @param[in] max_len Capacity of `buf`, > 0.
 * @param[out] got_len Receives the number of bytes actually placed.
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok Bytes drained; `*got_len` reflects the count.
 * @retval k_ra8_err_no_data Pipe was empty.
 * @retval k_ra8_err_null_ptr `buf` or `got_len` was NULL.
 * @retval k_ra8_err_invalid_state Driver not initialized.
 * @retval k_ra8_err_invalid_arg `max_len == 0`.
 *
 * @pre `ra8_usb_phid_init` succeeded.
 * @pre `buf` non-NULL, `got_len` non-NULL.
 *
 * @post On success `*got_len` reflects the actual byte count.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_usb_phid_recv_report(uint8_t report_id, uint8_t* buf, uint16_t max_len, uint16_t* got_len);

/* =============================================================================
 * Setup-handler attach
 * =============================================================================
 */

/**
 * @brief Register the application's HID class-setup handler.
 *
 * @details
 * The class layer drains the SETUP envelope from the controller, then
 * forwards it to the registered handler if its `bmRequestType`
 * indicates a class-recipient-interface request and its `bRequest` is
 * one of GET_REPORT / SET_REPORT / GET_IDLE / SET_IDLE / GET_PROTOCOL
 * / SET_PROTOCOL. Pass `NULL` for `setup_fn` to detach.
 *
 * @param[in] setup_fn Application's handler. NULL detaches.
 * @param[in] ctx Context pointer threaded back into `setup_fn`.
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok Handler installed.
 * @retval k_ra8_err_invalid_state Driver not initialized.
 *
 * @pre `ra8_usb_phid_init` succeeded.
 *
 * @post On the next class SETUP, `setup_fn(ctx, &setup)` fires.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_usb_phid_attach_setup_handler(ra8_usb_phid_setup_fn_t setup_fn,
                                                          void*                   ctx);

/* =============================================================================
 * Class SETUP dispatch
 * =============================================================================
 */

/**
 * @brief Process a class-specific SETUP packet on EP0.
 *
 * @details
 * Switches on `b_request` and (a) updates the local idle-rate /
 * protocol shadow for SET_IDLE / SET_PROTOCOL, then (b) forwards the
 * SETUP to the registered application handler if any. Standard
 * (non-class) SETUPs are rejected with `k_ra8_err_not_supported` so
 * the caller can fall back to its own standard-request handler.
 *
 * @param[in] setup The SETUP packet returned by `ra8_usb_read_setup_if_valid`.
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok SETUP handled (status stage queued internally).
 * @retval k_ra8_err_invalid_state Driver not initialized.
 * @retval k_ra8_err_null_ptr `setup` was NULL.
 * @retval k_ra8_err_not_supported `bRequest` is not a HID class request
 *         this layer cares about.
 *
 * @pre `ra8_usb_phid_init` succeeded.
 *
 * @post Idle-rate / protocol shadow updated as appropriate.
 *
 * @note Call from the `CTRT` ISR path of `ra8_usb`.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_usb_phid_handle_setup(const ra8_usb_setup_t* setup);

/* =============================================================================
 * Introspection
 * =============================================================================
 */

/**
 * @brief Read the most-recently negotiated idle rate.
 *
 * @param[out] out_idle_rate Receives the 4 ms-tick idle rate (0 =
 *                            "report only on change").
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok Value copied.
 * @retval k_ra8_err_invalid_state Driver not initialized.
 * @retval k_ra8_err_null_ptr `out_idle_rate` was NULL.
 *
 * @pre `out_idle_rate` non-NULL.
 *
 * @post No internal state mutated.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_usb_phid_get_idle(uint8_t* out_idle_rate);

/**
 * @brief Read the most-recently negotiated protocol.
 *
 * @param[out] out_protocol Receives the protocol selector.
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok Value copied.
 * @retval k_ra8_err_invalid_state Driver not initialized.
 * @retval k_ra8_err_null_ptr `out_protocol` was NULL.
 *
 * @pre `out_protocol` non-NULL.
 *
 * @post No internal state mutated.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_usb_phid_get_protocol(ra8_usb_phid_protocol_select_t* out_protocol);

#ifdef __cplusplus
}
#endif
