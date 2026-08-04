/**
 * @file ra8_usb_hhid.h
 * @brief Native USB host-side HID (Human Interface Device) class layer
 * @ingroup grp_hal_usb
 *
 * @details
 * Glues the host-mode bring-up paths in `ra8_usb` to a USB HID
 * peripheral - typically a keyboard, mouse, or gamepad - attached on
 * the EK-RA8D2's USB-host port. Mirrors FSP's `r_usb_hhid` host-HID
 * class flow but compiled as part of this tree with no FSP / CherryUSB
 * / TinyUSB binaries pulled in.
 *
 * Lifecycle (mirrors `ra8_usb_hcdc.h` / `ra8_usb_hmsc.h` shape):
 *
 *  1. `ra8_usb_hhid_init(speed)` flips the controller to host mode,
 *     primes the internal step machine to idle, and leaves the bus in
 *     the "wait for attach" state (UACT cleared).
 *  2. The driver runs the chapter-9 enumeration step machine
 *     (Reset -> SET_ADDRESS -> GET_DEVICE_DESCRIPTOR ->
 *     GET_CONFIG_DESCRIPTOR -> SET_CONFIG -> SET_INTERFACE) over the
 *     DCP, walks the configuration descriptor for the HID interface
 *     (class=0x03), pulls out the interrupt-IN endpoint plus the HID
 *     class descriptor (descriptor type 0x21).
 *  3. When enumeration completes the registered attach callback fires
 *     once with the discovered interrupt-IN pipe handle, max-packet,
 *     subclass+protocol (1=keyboard / 2=mouse / 0=other), and VID/PID.
 *  4. HID class control transfers (`_get_report`, `_set_report`,
 *     `_set_idle`, `_set_protocol`) are layered on top of the DCP per
 *     USB HID 1.11 sec 7.2.
 *  5. `ra8_usb_hhid_get_input_report` polls the interrupt-IN pipe non-
 *     blockingly to drain the next input report.
 *  6. `ra8_usb_hhid_close()` drops bus power and releases the device.
 *
 * The starter only tracks a single attached HID device; hubs and
 * multi-interface composite HID devices are deferred follow-ups.
 *
 * Reference: USB Device Class Definition for Human Interface Devices
 * (HID) revision 1.11 (USB-IF, 2001-06-27).
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
 * @enum ra8_usb_hhid_pipe_t
 * @brief PIPE numbers used by the host-HID driver for the attached
 *        peripheral's interrupt endpoints.
 *
 * @details FSP / RA8D2 PIPE assignment rules constrain interrupt
 * pipes to PIPE6..PIPE9. The host-HID class only uses two pipes: an
 * interrupt-IN pipe to drain input reports, and an optional
 * interrupt-OUT pipe (used by some keyboards for LED state). See USB
 * HID 1.11 sec 4.4 "Interrupt Pipes".
 */
typedef enum : uint8_t {
  k_ra8_hhid_pipe_intr_in  = 6U, /**< PIPE6 -> attached EP intr IN.  */
  k_ra8_hhid_pipe_intr_out = 7U, /**< PIPE7 -> attached EP intr OUT. */
} ra8_usb_hhid_pipe_t;

/**
 * @enum ra8_usb_hhid_packet_t
 * @brief Packet sizing for the attached device's interrupt endpoints.
 *
 * @details Per USB HID 1.11 sec 8.2 "Maximum Packet Size", a low- /
 * full-speed HID interrupt endpoint is at most 64 bytes, and a high-
 * speed one is at most 1024 bytes. Most keyboards / mice land at 8.
 */
typedef enum : uint16_t {
  k_ra8_hhid_intr_max_packet_default = 8U,    /**< Boot-protocol default. */
  k_ra8_hhid_intr_max_packet_fs      = 64U,   /**< FS ceiling.            */
  k_ra8_hhid_intr_max_packet_hs      = 1024U, /**< HS ceiling.            */
} ra8_usb_hhid_packet_t;

/**
 * @enum ra8_usb_hhid_class_t
 * @brief Class / subclass / protocol triplet that identifies an HID
 *        function within an attached USB device's descriptor walk.
 *
 * @details Numbered from the USB-IF "Class Codes" registry and USB
 * HID 1.11 sec 4.2 "Subclass". Subclass 1 = "Boot Interface
 * Subclass", protocol 1 = keyboard, protocol 2 = mouse.
 */
typedef enum : uint8_t {
  k_ra8_hhid_class_hid         = 0x03U, /**< HID interface class.     */
  k_ra8_hhid_subclass_none     = 0x00U, /**< No subclass.             */
  k_ra8_hhid_subclass_boot     = 0x01U, /**< Boot Interface Subclass. */
  k_ra8_hhid_protocol_other    = 0x00U, /**< Non keyboard / mouse.    */
  k_ra8_hhid_protocol_keyboard = 0x01U, /**< Boot keyboard.           */
  k_ra8_hhid_protocol_mouse    = 0x02U, /**< Boot mouse.              */
} ra8_usb_hhid_class_t;

/**
 * @enum ra8_usb_hhid_request_t
 * @brief HID class-specific request codes the host issues to the
 *        attached device.
 *
 * @details Per USB HID 1.11 sec 7.2 "Class-Specific Requests". Values
 * match the standard HID class request register.
 */
typedef enum : uint8_t {
  k_ra8_hhid_req_get_report   = 0x01U, /**< GET_REPORT.   */
  k_ra8_hhid_req_get_idle     = 0x02U, /**< GET_IDLE.     */
  k_ra8_hhid_req_get_protocol = 0x03U, /**< GET_PROTOCOL. */
  k_ra8_hhid_req_set_report   = 0x09U, /**< SET_REPORT.   */
  k_ra8_hhid_req_set_idle     = 0x0AU, /**< SET_IDLE.     */
  k_ra8_hhid_req_set_protocol = 0x0BU, /**< SET_PROTOCOL. */
} ra8_usb_hhid_request_t;

/**
 * @enum ra8_usb_hhid_report_type_t
 * @brief HID report-type selector encoded in wValue's high byte for
 *        GET_REPORT / SET_REPORT.
 *
 * @details Per USB HID 1.11 sec 7.2.1 "Get_Report Request".
 */
typedef enum : uint8_t {
  k_ra8_hhid_report_type_input   = 0x01U, /**< Input report.   */
  k_ra8_hhid_report_type_output  = 0x02U, /**< Output report.  */
  k_ra8_hhid_report_type_feature = 0x03U, /**< Feature report. */
} ra8_usb_hhid_report_type_t;

/**
 * @enum ra8_usb_hhid_protocol_select_t
 * @brief wValue payload for SET_PROTOCOL.
 *
 * @details Per USB HID 1.11 sec 7.2.6 "Set_Protocol Request". 0 =
 * boot protocol, 1 = report protocol.
 */
typedef enum : uint8_t {
  k_ra8_hhid_proto_boot   = 0U, /**< Boot protocol.   */
  k_ra8_hhid_proto_report = 1U, /**< Report protocol. */
} ra8_usb_hhid_protocol_select_t;

/**
 * @enum ra8_usb_hhid_desc_t
 * @brief HID-specific descriptor types (USB HID 1.11 sec 7.1).
 */
typedef enum : uint8_t {
  k_ra8_hhid_desc_hid      = 0x21U, /**< HID class descriptor.    */
  k_ra8_hhid_desc_report   = 0x22U, /**< HID Report descriptor.   */
  k_ra8_hhid_desc_physical = 0x23U, /**< HID Physical descriptor. */
} ra8_usb_hhid_desc_t;

/**
 * @enum ra8_usb_hhid_report_buf_t
 * @brief Compile-time ceiling on the cached HID Report descriptor.
 *
 * @details The HID spec puts an absolute ceiling at 65535 bytes
 * (`wDescriptorLength` is 16-bit), but real-world HID descriptors are
 * essentially always under 256 bytes. We pick a small-but-generous
 * ceiling here so the starter doesn't carry a kilobyte of zeroes
 * around.
 */
typedef enum : uint16_t {
  k_ra8_hhid_report_desc_max = 256U, /**< Cached Report desc max len. */
} ra8_usb_hhid_report_buf_t;

/**
 * @struct ra8_usb_hhid_device_t
 * @brief Snapshot of the attached HID device, passed to the attach
 *        callback.
 *
 * @details Populated by the host-HID driver once the descriptor walk
 * completes. The pipe number is the controller-side handle wired up
 * against the attached device's interrupt-IN endpoint. The recipient
 * should hold onto this struct (or copy it) so subsequent get/set
 * report calls land on the right interface.
 */
typedef struct {
  uint8_t        device_address;        /**< Assigned USB address (1..127).    */
  uint8_t        intr_in_ep;            /**< Attached device's intr IN EP num. */
  uint8_t        intr_out_ep;           /**< Attached device's intr OUT EP num
                                          (0 if absent).                      */
  uint8_t        interface_number;      /**< HID bInterfaceNumber.              */
  uint8_t        subclass;              /**< bInterfaceSubClass (boot=1).       */
  uint8_t        protocol;              /**< bInterfaceProtocol (kb=1, ms=2).   */
  uint16_t       intr_in_max_packet;    /**< Attached intr-IN wMaxPacketSize.   */
  uint16_t       intr_out_max_packet;   /**< Attached intr-OUT wMaxPacketSize.  */
  uint16_t       vendor_id;             /**< idVendor from device descriptor.   */
  uint16_t       product_id;            /**< idProduct from device descriptor.  */
  uint16_t       report_descriptor_len; /**< Cached Report desc length.         */
  const uint8_t* hid_descriptor;        /**< Pointer to cached HID class desc.  */
  const uint8_t* report_descriptor;     /**< Pointer to cached HID Report desc. */
} ra8_usb_hhid_device_t;

/**
 * @typedef ra8_usb_hhid_attach_fn_t
 * @brief Attach-callback signature.
 *
 * @param[in] ctx Caller-supplied context registered with
 *                `ra8_usb_hhid_attach_callback`.
 * @param[in] device Snapshot of the attached HID device. The pointer
 *                   remains valid only for the duration of the call;
 *                   copy out anything you need.
 *
 * @note Invoked from the dispatch site (typically ISR context) after
 * enumeration completes successfully.
 */
typedef void (*ra8_usb_hhid_attach_fn_t)(void* ctx, const ra8_usb_hhid_device_t* device);

/* =============================================================================
 * Lifecycle
 * =============================================================================
 */

/**
 * @brief Bring up the host-HID driver on a chosen USB controller.
 *
 * @details
 * Initialises the underlying `ra8_usb` driver in HOST mode for
 * `speed`, leaves the bus in the "wait for attach" state (UACT
 * cleared), and arms the internal enumeration step machine.
 *
 * @param[in] speed Which USB controller (FS or HS).
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok Host-HID ready, awaiting attach.
 * @retval k_ra8_err_invalid_arg `speed` out of range.
 * @retval k_ra8_err_hw_init_failed Underlying `ra8_usb_host_init`
 *         failed.
 *
 * @pre Single-threaded init context.
 * @pre `ra8_mstp_init` and `ra8_pwr_init` already ran.
 *
 * @post `ra8_usb_host_init` succeeded for `speed`.
 * @post Internal step machine armed; attach callback has not fired.
 *
 * @note Not thread-safe.
 * @see ra8_usb_hhid_attach_callback
 * @see ra8_usb_hhid_close
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_usb_hhid_init(ra8_usb_speed_t speed);

/**
 * @brief Tear down the host-HID driver and release the controller.
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok Released.
 * @retval k_ra8_err_invalid_state Driver was never initialized.
 *
 * @pre Single-threaded shutdown context.
 *
 * @post `ra8_usb_host_deinit` ran; SOF generation halted; bus power
 *       dropped; subsequent host-HID API calls return
 *       `k_ra8_err_invalid_state`.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_usb_hhid_close(void);

/* =============================================================================
 * Attach callback
 * =============================================================================
 */

/**
 * @brief Register (or detach) the attach callback.
 *
 * @details
 * The supplied callback fires exactly once per attach event, after
 * the descriptor walk identifies an HID interface (class=0x03) and
 * the matching interrupt-IN endpoint and HID class descriptor are
 * cached. Pass `NULL` to detach.
 *
 * @param[in] on_attach Callback. NULL detaches.
 * @param[in] ctx Context pointer threaded back into `on_attach`.
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok Callback installed.
 * @retval k_ra8_err_invalid_state Driver was never initialized.
 *
 * @pre `ra8_usb_hhid_init` has run.
 *
 * @post On a subsequent attach, `on_attach(ctx, &device)` fires once.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_usb_hhid_attach_callback(ra8_usb_hhid_attach_fn_t on_attach, void* ctx);

/* =============================================================================
 * Class control transfers
 * =============================================================================
 */

/**
 * @brief Issue GET_REPORT (USB HID 1.11 sec 7.2.1) over the DCP.
 *
 * @details
 * Builds an 8-byte SETUP packet with bmRequestType = 0xA1 (Class |
 * Interface | Device-to-Host), bRequest = 0x01 (GET_REPORT), and
 * wValue = (report_type << 8) | report_id. Hands it to
 * `ra8_usb_host_setup_request`; the controller drives the data + status
 * stages.
 *
 * @param[in] target_report_type Input / Output / Feature.
 * @param[in] target_report_id Report ID (0 if the device uses a single
 *                             unnamed report).
 * @param[out] out_buf Destination buffer for the report payload.
 * @param[in] max_len Capacity of `out_buf`, > 0.
 * @param[out] got_len Receives the number of bytes actually received.
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok Control transfer queued; on completion `*got_len`
 *         reflects the count.
 * @retval k_ra8_err_null_ptr `out_buf` or `got_len` was NULL.
 * @retval k_ra8_err_invalid_state Driver not initialized, or no device
 *         attached.
 * @retval k_ra8_err_invalid_arg Bogus `target_report_type` or
 *         `max_len == 0`.
 * @retval k_ra8_err_busy Controller busy with a prior SETUP.
 *
 * @pre `ra8_usb_hhid_init` succeeded.
 * @pre Attach callback already fired.
 *
 * @post On success the SETUP mirror registers hold the GET_REPORT
 *       request envelope.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_usb_hhid_get_report(ra8_usb_hhid_report_type_t target_report_type,
                                                uint8_t                    target_report_id,
                                                uint8_t*                   out_buf,
                                                uint16_t                   max_len,
                                                uint16_t*                  got_len);

/**
 * @brief Issue SET_REPORT (USB HID 1.11 sec 7.2.2) over the DCP.
 *
 * @details
 * Builds an 8-byte SETUP packet with bmRequestType = 0x21 (Class |
 * Interface | Host-to-Device), bRequest = 0x09 (SET_REPORT), and
 * wValue = (report_type << 8) | report_id. The data stage payload is
 * `in_buf[0..len-1]`.
 *
 * @param[in] target_report_type Input / Output / Feature.
 * @param[in] target_report_id Report ID.
 * @param[in] in_buf Report payload.
 * @param[in] len Payload length.
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok Control transfer queued.
 * @retval k_ra8_err_null_ptr `in_buf` was NULL with non-zero `len`.
 * @retval k_ra8_err_invalid_state Driver not initialized, or no device
 *         attached.
 * @retval k_ra8_err_invalid_arg Bogus `target_report_type`.
 * @retval k_ra8_err_busy Controller busy with a prior SETUP.
 *
 * @pre `ra8_usb_hhid_init` succeeded.
 * @pre Attach callback already fired.
 *
 * @post On success the SETUP mirror registers hold the SET_REPORT
 *       request envelope.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_usb_hhid_set_report(ra8_usb_hhid_report_type_t target_report_type,
                                                uint8_t                    target_report_id,
                                                const uint8_t*             in_buf,
                                                uint16_t                   len);

/**
 * @brief Issue SET_IDLE (USB HID 1.11 sec 7.2.4) over the DCP.
 *
 * @details
 * Tells the device how often to silently re-send an unchanged input
 * report. `duration` is in 4 ms units (0 = "only report on change").
 * Builds bmRequestType = 0x21, bRequest = 0x0A,
 * wValue = (duration << 8) | report_id.
 *
 * @param[in] duration Idle rate, in 4 ms ticks (0..255).
 * @param[in] report_id Report ID, or 0 for "all reports".
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok Control transfer queued.
 * @retval k_ra8_err_invalid_state Driver not initialized, or no device
 *         attached.
 * @retval k_ra8_err_busy Controller busy with a prior SETUP.
 *
 * @pre `ra8_usb_hhid_init` succeeded.
 * @pre Attach callback already fired.
 *
 * @post On success the SETUP mirror registers hold the SET_IDLE
 *       request envelope.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_usb_hhid_set_idle(uint8_t duration, uint8_t report_id);

/**
 * @brief Issue SET_PROTOCOL (USB HID 1.11 sec 7.2.6) over the DCP.
 *
 * @details
 * Switches the device between boot protocol (wValue = 0) and report
 * protocol (wValue = 1). Builds bmRequestType = 0x21, bRequest = 0x0B.
 *
 * @param[in] boot_or_report Boot or report protocol selector.
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok Control transfer queued.
 * @retval k_ra8_err_invalid_state Driver not initialized, or no device
 *         attached.
 * @retval k_ra8_err_invalid_arg `boot_or_report` out of range.
 * @retval k_ra8_err_busy Controller busy with a prior SETUP.
 *
 * @pre `ra8_usb_hhid_init` succeeded.
 * @pre Attach callback already fired.
 *
 * @post On success the SETUP mirror registers hold the SET_PROTOCOL
 *       request envelope.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_usb_hhid_set_protocol(ra8_usb_hhid_protocol_select_t boot_or_report);

/* =============================================================================
 * Interrupt-IN polling
 * =============================================================================
 */

/**
 * @brief Drain the next input report from the interrupt-IN pipe.
 *
 * @details
 * Polling / non-blocking. Pulls bytes from the configured PIPE6
 * (interrupt IN) into `out_buf`. Returns `k_ra8_err_no_data` if the
 * pipe has no bytes ready.
 *
 * @param[out] out_buf Destination buffer.
 * @param[in] max_len Capacity of `out_buf`, > 0.
 * @param[out] got_len Receives the number of bytes actually placed.
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok Bytes drained; `*got_len` reflects the count.
 * @retval k_ra8_err_no_data Pipe was empty.
 * @retval k_ra8_err_null_ptr `out_buf` or `got_len` was NULL.
 * @retval k_ra8_err_invalid_state Driver not initialized, or no device
 *         attached.
 * @retval k_ra8_err_invalid_arg `max_len == 0`.
 *
 * @pre `ra8_usb_hhid_init` succeeded.
 * @pre Attach callback already fired.
 *
 * @post On success `*got_len` reflects the actual byte count.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_usb_hhid_get_input_report(uint8_t* out_buf, uint16_t max_len, uint16_t* got_len);

/* =============================================================================
 * Test / introspection helpers
 * =============================================================================
 */

/**
 * @brief Drive the enumeration step machine forward by one step.
 *
 * @details
 * Test / debug entry point. The production path drives this from the
 * `ra8_usb_dispatch` callback when the controller fires a CTRT or BRDY
 * interrupt; tests call it directly to walk the state machine
 * deterministically.
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok Step advanced.
 * @retval k_ra8_err_invalid_state Driver not initialized.
 *
 * @pre `ra8_usb_hhid_init` succeeded.
 *
 * @post Internal enumeration step counter advances by one. When the
 *       step machine reaches the terminal state the registered
 *       attach callback fires.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_usb_hhid_step(void);

#ifdef __cplusplus
}
#endif
