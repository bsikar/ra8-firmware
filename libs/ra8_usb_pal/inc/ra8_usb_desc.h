/**
 * @file ra8_usb_desc.h
 * @brief Synthesise the three USB device frameworks from a config struct (RA8FW-317).
 * @ingroup grp_net
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details
 * To expose a USB device an application currently hand-assembles three raw
 * byte arrays: the device framework (device descriptor followed by the
 * configuration block), the string framework, and the language-id framework.
 * Twenty-nine example files in this tree do exactly that, and every one of
 * them re-types the same chapter-9 byte layout by hand, with the
 * `wTotalLength` field of the configuration descriptor counted by a human.
 *
 * This header is the synthesiser half of that work: pure functions, bytes in
 * a caller-owned buffer out, no USBX types, no allocation, no global state.
 * That is deliberate, because it is the half that can be proven off-target.
 * The USBX handshake (`_ux_system_initialize`, `_ux_device_stack_initialize`,
 * `_ux_device_stack_class_register`) is a separate seam and is not here.
 *
 * @code
 * static uint8_t framework[k_ra8_usb_desc_framework_bytes_max];
 * uint32_t       used = 0U;
 *
 * const ra8_usb_desc_device_t dev = {
 *   .vid = 0x1209U, .pid = 0x000AU, .bcd_device = 0x0100U,
 *   .max_power_ma = 100U,
 * };
 * const ra8_usb_desc_cdc_acm_t cdc = {
 *   .notify_ep = 0x83U, .notify_bytes = 8U, .notify_interval_ms = 255U,
 *   .out_ep = 0x02U, .in_ep = 0x81U, .data_bytes = 64U,
 * };
 * if (ra8_usb_desc_build_cdc_acm(&dev, &cdc, framework, sizeof(framework), &used)
 *     == k_ra8_ok) {
 *   // framework[0 .. used) is the s_device_framework an app used to type out.
 * }
 * @endcode
 *
 * ## What the builders do NOT do
 *
 * They synthesise the layout the twenty-nine copies already share. An app with
 * an exotic descriptor need (a vendor-specific interface, a composite layout
 * this header does not model) keeps declaring its own arrays exactly as today.
 * Nothing here removes that option, and nothing here talks to a controller:
 * a synthesised framework is bytes, not an attached device.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

#include "ra8_err.h"

/* =============================================================================
 * Constants
 * =============================================================================
 */

/**
 * @enum ra8_usb_desc_limit_t
 * @brief Sizing and layout constants for the synthesised frameworks.
 *
 * @details The `_bytes` values are the exact on-the-wire lengths chapter 9 of
 * USB 2.0 fixes for each descriptor, so they are constants rather than
 * policy. ::k_ra8_usb_desc_string_chars_max is this header's own cap: a
 * string descriptor carries its length in one byte, and the two-byte header
 * this tree's string framework prepends leaves 253 usable characters. The cap
 * is set well below that, at the longest product string any current consumer
 * declares plus room to grow.
 */
typedef enum : uint16_t {
  k_ra8_usb_desc_device_bytes     = 18U, /**< Device descriptor, USB 2.0 sec 9.6.1.   */
  k_ra8_usb_desc_config_bytes     = 9U,  /**< Configuration descriptor, sec 9.6.3.    */
  k_ra8_usb_desc_iface_bytes      = 9U,  /**< Interface descriptor, sec 9.6.5.        */
  k_ra8_usb_desc_endpoint_bytes   = 7U,  /**< Endpoint descriptor, sec 9.6.6.         */
  k_ra8_usb_desc_iad_bytes        = 8U,  /**< Interface association, IAD ECN.         */
  k_ra8_usb_desc_langid_bytes     = 2U,  /**< The whole language-id framework.        */
  k_ra8_usb_desc_string_hdr       = 4U,  /**< langid lo, langid hi, index, length.    */
  k_ra8_usb_desc_string_chars_max = 64U, /**< Longest string this header will encode. */
  k_ra8_usb_desc_string_slots     = 3U,  /**< Manufacturer, product, serial.          */
  /** Buffer a caller must provide for the largest framework built here. */
  k_ra8_usb_desc_framework_bytes_max = 128U,
  /** Buffer a caller must provide for the largest string framework built here. */
  k_ra8_usb_desc_strings_bytes_max =
    (uint16_t)(k_ra8_usb_desc_string_slots *
               (k_ra8_usb_desc_string_hdr + k_ra8_usb_desc_string_chars_max)),
} ra8_usb_desc_limit_t;

/**
 * @enum ra8_usb_desc_langid_t
 * @brief Language identifiers this header knows by name.
 *
 * @details A zero `langid` in ::ra8_usb_desc_device_t means
 * ::k_ra8_usb_desc_langid_en_us, which is the value all twenty-nine current
 * copies hard-code.
 */
typedef enum : uint16_t {
  k_ra8_usb_desc_langid_en_us = 0x0409U, /**< English (United States). */
} ra8_usb_desc_langid_t;

/**
 * @enum ra8_usb_desc_string_index_t
 * @brief String descriptor indices the synthesised device descriptor refers to.
 *
 * @details Index 0 is the language-id framework and is never a text string,
 * so the three text slots start at 1 and are emitted in this order.
 */
typedef enum : uint8_t {
  k_ra8_usb_desc_str_manufacturer = 1U, /**< iManufacturer. */
  k_ra8_usb_desc_str_product      = 2U, /**< iProduct.      */
  k_ra8_usb_desc_str_serial       = 3U, /**< iSerialNumber. */
} ra8_usb_desc_string_index_t;

/* =============================================================================
 * Configuration
 * =============================================================================
 */

/**
 * @struct ra8_usb_desc_device_t
 * @brief The device-level identity every synthesised framework carries.
 *
 * @details The three strings are borrowed for the duration of the call and are
 * copied into the caller's buffer; nothing here retains a pointer. A NULL or
 * empty string means that slot is not published and its index field in the
 * device descriptor is written as 0.
 *
 * @invariant `max_power_ma` is the real milliamp figure, not the halved
 * bMaxPower encoding: the builder halves it, so a caller writes 100 and the
 * wire carries 0x32. An odd value rounds up, because a device that draws
 * 51 mA may not advertise 50.
 */
typedef struct {
  uint16_t    vid;           /**< idVendor.                                  */
  uint16_t    pid;           /**< idProduct.                                 */
  uint16_t    bcd_device;    /**< bcdDevice, BCD release. Zero means 0x0100. */
  const char* manufacturer;  /**< String index 1, or NULL to omit.           */
  const char* product;       /**< String index 2, or NULL to omit.           */
  const char* serial;        /**< String index 3, or NULL to omit.           */
  uint16_t    langid;        /**< Zero means ::k_ra8_usb_desc_langid_en_us.  */
  uint16_t    max_power_ma;  /**< Bus draw in mA, halved into bMaxPower.     */
  bool        self_powered;  /**< Sets bmAttributes bit 6.                   */
  bool        remote_wakeup; /**< Sets bmAttributes bit 5.                   */
} ra8_usb_desc_device_t;

/**
 * @struct ra8_usb_desc_cdc_acm_t
 * @brief The endpoint layout of a single-function CDC-ACM device.
 *
 * @details Endpoint addresses carry their direction bit exactly as they appear
 * on the wire, so a bulk-IN endpoint 1 is `0x81` and a bulk-OUT endpoint 2 is
 * `0x02`. That matches how every current copy writes them, which keeps a
 * converted app diffable against the array it replaces.
 *
 * @invariant `notify_ep` and `in_ep` have bit 7 set; `out_ep` does not.
 */
typedef struct {
  uint8_t  notify_ep;          /**< Interrupt-IN endpoint address, e.g. 0x83. */
  uint16_t notify_bytes;       /**< Interrupt-IN max packet size.             */
  uint8_t  notify_interval_ms; /**< bInterval for the interrupt endpoint.     */
  uint8_t  out_ep;             /**< Bulk-OUT endpoint address, e.g. 0x02.     */
  uint8_t  in_ep;              /**< Bulk-IN endpoint address, e.g. 0x81.      */
  uint16_t data_bytes;         /**< Bulk max packet size, 64 FS / 512 HS.     */
  bool     high_speed;         /**< Emit the device qualifier for an HS app.  */
} ra8_usb_desc_cdc_acm_t;

/**
 * @struct ra8_usb_desc_msc_t
 * @brief The endpoint layout of a single-interface mass-storage device.
 *
 * @details Bulk-only transport (SCSI transparent command set over BBB) is the
 * only mass-storage protocol every current copy publishes, so the class,
 * subclass and protocol triple is fixed rather than parameterised. Endpoint
 * addresses carry their direction bit exactly as they appear on the wire, the
 * same rule ::ra8_usb_desc_cdc_acm_t follows.
 *
 * @invariant `in_ep` has bit 7 set; `out_ep` does not.
 */
typedef struct {
  uint8_t  in_ep;      /**< Bulk-IN endpoint address, e.g. 0x81.     */
  uint8_t  out_ep;     /**< Bulk-OUT endpoint address, e.g. 0x02.    */
  uint16_t data_bytes; /**< Bulk max packet size, 64 FS / 512 HS.    */
  bool     high_speed; /**< Emit the device qualifier for an HS app. */
} ra8_usb_desc_msc_t;

/**
 * @enum ra8_usb_desc_hid_protocol_t
 * @brief The boot-interface protocols a HID interface can declare.
 *
 * @details bInterfaceProtocol is only meaningful on a boot interface: HID 1.11
 * appendix B defines the two boot report layouts a BIOS can drive without
 * parsing a report descriptor, and reserves the field otherwise. A non-boot
 * interface therefore declares ::k_ra8_usb_desc_hid_protocol_none, and the
 * builder refuses any other pairing rather than putting a field on the wire
 * that no host is allowed to read.
 */
typedef enum : uint8_t {
  k_ra8_usb_desc_hid_protocol_none     = 0U, /**< Not a boot interface.  */
  k_ra8_usb_desc_hid_protocol_keyboard = 1U, /**< Boot keyboard, app. B. */
  k_ra8_usb_desc_hid_protocol_mouse    = 2U, /**< Boot mouse, app. B.    */
} ra8_usb_desc_hid_protocol_t;

/**
 * @struct ra8_usb_desc_hid_t
 * @brief The endpoint layout of a single-interface human-interface device.
 *
 * @details One interrupt-IN endpoint and no OUT endpoint is the shape all
 * three current copies publish, so the endpoint count is fixed rather than
 * parameterised. `report_bytes` is the length of the report descriptor the
 * app hands the class driver separately: it is copied into the HID class
 * descriptor's `wDescriptorLength`, and it is the field a human counting by
 * hand gets wrong, because it has to track an array that lives elsewhere in
 * the file. Passing `sizeof(s_report_descriptor)` keeps the two in step.
 *
 * @invariant `in_ep` has bit 7 set; `poll_interval_ms` is at least 1, because
 * bInterval 0 is not a legal polling period for an interrupt endpoint.
 */
typedef struct {
  uint8_t                     in_ep;            /**< Interrupt-IN address, e.g. 0x81. */
  uint16_t                    data_bytes;       /**< Interrupt max packet size.       */
  uint8_t                     poll_interval_ms; /**< bInterval, frames.               */
  uint16_t                    report_bytes;     /**< Report-descriptor length.        */
  bool                        boot_interface;   /**< Declare the boot subclass.       */
  ra8_usb_desc_hid_protocol_t protocol;         /**< Boot protocol, none if not boot. */
} ra8_usb_desc_hid_t;

/**
 * @struct ra8_usb_desc_dfu_t
 * @brief The function of a Device Firmware Upgrade interface, DFU 1.1 sec 4.

 *
 * @details DFU is the one function in this header with no endpoints of its
 * own: every transfer rides the default control pipe, so the interface
 * declares zero endpoints and the functional descriptor carries everything a
 * host needs to drive it. The capability bits are taken as four booleans
 * rather than a packed bmAttributes byte, because a caller assembling that
 * byte by hand is exactly the arithmetic this header exists to remove.
 *
 * @invariant `transfer_bytes` and `bcd_dfu` are non-zero, and at least one of
 * `can_download` and `can_upload` is set: a DFU function that can neither
 * receive nor send firmware has nothing to offer a host.
 */
typedef struct {
  bool     can_download;           /**< bitCanDnload, accepts DFU_DNLOAD.           */
  bool     can_upload;             /**< bitCanUpload, answers DFU_UPLOAD.           */
  bool     manifestation_tolerant; /**< Survives manifestation without a bus reset. */
  bool     will_detach;            /**< Detaches itself on DFU_DETACH.              */
  bool     dfu_mode;               /**< DFU mode, not the run-time descriptor.      */
  uint16_t detach_timeout_ms;      /**< wDetachTimeOut, milliseconds.               */
  uint16_t transfer_bytes;         /**< wTransferSize, bytes per block.             */
  uint16_t bcd_dfu;                /**< bcdDFUVersion, e.g. 0x0110.                 */
} ra8_usb_desc_dfu_t;

/* =============================================================================
 * Builders
 * =============================================================================
 */

/**
 * @brief Write the two-byte language-id framework.
 *
 * @param[in]  langid   Language id, or 0 for ::k_ra8_usb_desc_langid_en_us.
 * @param[out] out      Caller-owned destination buffer.
 * @param[in]  cap      Capacity of @p out in bytes.
 * @param[out] out_len  Bytes written on success. Untouched on failure.
 *
 * @return ra8_err_t Result of the encode.
 * @retval k_ra8_ok                 Framework written.
 * @retval k_ra8_err_null_ptr       @p out or @p out_len is NULL.
 * @retval k_ra8_err_invalid_size   @p cap is below ::k_ra8_usb_desc_langid_bytes.
 *
 * @pre @p out addresses at least @p cap writable bytes.
 * @post On success @p out holds the framework and @p out_len is 2.
 * @post On failure @p out_len is unchanged and @p out may be partly written.
 *
 * @note Pure; no state is retained between calls.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_usb_desc_build_langid(uint16_t langid, uint8_t* out, uint32_t cap, uint32_t* out_len);

/**
 * @brief Write the string framework for the manufacturer, product and serial.
 *
 * @details Each published slot is emitted as `{langid_lo, langid_hi, index,
 * length}` followed by the ASCII bytes, which is the layout the device stack
 * in this tree parses. A NULL or empty slot is skipped entirely rather than
 * emitted with a zero length, so the indices in the device descriptor and the
 * entries here always agree.
 *
 * @param[in]  dev      Device identity supplying the three strings.
 * @param[out] out      Caller-owned destination buffer.
 * @param[in]  cap      Capacity of @p out in bytes.
 * @param[out] out_len  Bytes written on success. Untouched on failure.
 *
 * @return ra8_err_t Result of the encode.
 * @retval k_ra8_ok                 Framework written; may be zero-length.
 * @retval k_ra8_err_null_ptr       @p dev, @p out or @p out_len is NULL.
 * @retval k_ra8_err_invalid_size   @p cap cannot hold the encoded slots.
 * @retval k_ra8_err_range_check_failed A slot is longer than
 *                                  ::k_ra8_usb_desc_string_chars_max.
 *
 * @pre @p out addresses at least @p cap writable bytes.
 * @post On success @p out holds the framework and @p out_len counts it.
 * @post On failure @p out_len is unchanged and @p out may be partly written.
 *
 * @note Pure; no state is retained between calls.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_usb_desc_build_strings(const ra8_usb_desc_device_t* dev,
                                                   uint8_t*                     out,
                                                   uint32_t                     cap,
                                                   uint32_t*                    out_len);

/**
 * @brief Write the device framework of a single-function CDC-ACM device.
 *
 * @details The result is the 18-byte device descriptor followed by the whole
 * configuration block: configuration, interface association, communications
 * interface, the four CDC functional descriptors, the interrupt-IN endpoint,
 * the data interface and the two bulk endpoints. The `wTotalLength` field is
 * computed from what was actually emitted, which is the field a human counting
 * by hand gets wrong.
 *
 * With `cdc->high_speed` set, a ten-byte device qualifier follows the device
 * descriptor, exactly as ::ra8_usb_desc_build_msc emits one. It sits outside
 * the configuration block, so `wTotalLength` still counts only what the
 * configuration itself spans.
 *
 * @param[in]  dev      Device identity.
 * @param[in]  cdc      Endpoint layout of the CDC function.
 * @param[out] out      Caller-owned destination buffer.
 * @param[in]  cap      Capacity of @p out in bytes.
 * @param[out] out_len  Bytes written on success. Untouched on failure.
 *
 * @return ra8_err_t Result of the encode.
 * @retval k_ra8_ok                 Framework written.
 * @retval k_ra8_err_null_ptr       @p dev, @p cdc, @p out or @p out_len is NULL.
 * @retval k_ra8_err_invalid_size   @p cap cannot hold the framework.
 * @retval k_ra8_err_invalid_arg    An endpoint address carries the wrong
 *                                  direction bit, or a max packet size is zero.
 * @retval k_ra8_err_range_check_failed @p dev->max_power_ma exceeds what
 *                                  bMaxPower can encode (500 mA).
 *
 * @pre @p out addresses at least @p cap writable bytes.
 * @post On success @p out holds the framework and @p out_len counts it.
 * @post On failure @p out_len is unchanged and @p out may be partly written.
 *
 * @note Pure; no state is retained between calls. Synthesising a framework
 *       does not attach a device or touch a controller.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_usb_desc_build_cdc_acm(const ra8_usb_desc_device_t*  dev,
                                                   const ra8_usb_desc_cdc_acm_t* cdc,
                                                   uint8_t*                      out,
                                                   uint32_t                      cap,
                                                   uint32_t*                     out_len);

/**
 * @brief Write the device framework of a single-interface mass-storage device.
 *
 * @details The result is the 18-byte device descriptor, then the device
 * qualifier when @p msc requests one, then the whole configuration block:
 * configuration descriptor, the bulk-only mass-storage interface, and the two
 * bulk endpoints in IN-then-OUT order. As with the CDC builder the
 * `wTotalLength` field is computed from what was actually emitted.
 *
 * The device descriptor advertises class 0 (per-interface) rather than the
 * MISC / common / IAD triple the CDC builder writes, because a single-function
 * mass-storage device has no interface association to declare and every
 * current copy publishes class 0 here.
 *
 * @param[in]  dev      Device identity.
 * @param[in]  msc      Endpoint layout of the mass-storage function.
 * @param[out] out      Caller-owned destination buffer.
 * @param[in]  cap      Capacity of @p out in bytes.
 * @param[out] out_len  Bytes written on success. Untouched on failure.
 *
 * @return ra8_err_t Result of the encode.
 * @retval k_ra8_ok                 Framework written.
 * @retval k_ra8_err_null_ptr       @p dev, @p msc, @p out or @p out_len is NULL.
 * @retval k_ra8_err_invalid_size   @p cap cannot hold the framework.
 * @retval k_ra8_err_invalid_arg    An endpoint address carries the wrong
 *                                  direction bit, or the max packet size is zero.
 * @retval k_ra8_err_range_check_failed @p dev->max_power_ma exceeds what
 *                                  bMaxPower can encode (500 mA).
 *
 * @pre @p out addresses at least @p cap writable bytes.
 * @post On success @p out holds the framework and @p out_len counts it.
 * @post On failure @p out_len is unchanged and @p out may be partly written.
 *
 * @note Pure; no state is retained between calls. Synthesising a framework
 *       does not attach a device or touch a controller.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_usb_desc_build_msc(const ra8_usb_desc_device_t* dev,
                                               const ra8_usb_desc_msc_t*    msc,
                                               uint8_t*                     out,
                                               uint32_t                     cap,
                                               uint32_t*                    out_len);

/**
 * @brief Write the device framework of a single-interface HID device.
 *
 * @details The result is the 18-byte device descriptor followed by the whole
 * configuration block: configuration descriptor, the HID interface, the HID
 * class descriptor naming one report descriptor, and the single interrupt-IN
 * endpoint. As with the other builders `wTotalLength` is computed from what
 * was actually emitted, and the device descriptor advertises class 0 so the
 * class triple is read off the interface.
 *
 * The report descriptor itself is not synthesised. Its bytes are the device's
 * whole personality, they differ completely between a mouse and a keyboard,
 * and an app keeps declaring them; this builder only copies their length into
 * the HID class descriptor so the two cannot drift apart.
 *
 * @param[in]  dev      Device identity.
 * @param[in]  hid      Endpoint layout of the HID function.
 * @param[out] out      Caller-owned destination buffer.
 * @param[in]  cap      Capacity of @p out in bytes.
 * @param[out] out_len  Bytes written on success. Untouched on failure.
 *
 * @return ra8_err_t Result of the encode.
 * @retval k_ra8_ok                 Framework written.
 * @retval k_ra8_err_null_ptr       @p dev, @p hid, @p out or @p out_len is NULL.
 * @retval k_ra8_err_invalid_size   @p cap cannot hold the framework.
 * @retval k_ra8_err_invalid_arg    The endpoint address carries the wrong
 *                                  direction bit, a max packet size or report
 *                                  length is zero, the polling interval is
 *                                  zero, or a boot protocol is declared on a
 *                                  non-boot interface.
 * @retval k_ra8_err_range_check_failed @p dev->max_power_ma exceeds what
 *                                  bMaxPower can encode (500 mA).
 *
 * @pre @p out addresses at least @p cap writable bytes.
 * @post On success @p out holds the framework and @p out_len counts it.
 * @post On failure @p out_len is unchanged and @p out may be partly written.
 *
 * @note Pure; no state is retained between calls. Synthesising a framework
 *       does not attach a device or touch a controller.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_usb_desc_build_hid(const ra8_usb_desc_device_t* dev,
                                               const ra8_usb_desc_hid_t*    hid,
                                               uint8_t*                     out,
                                               uint32_t                     cap,
                                               uint32_t*                    out_len);

/**
 * @brief Synthesise the device framework of a Device Firmware Upgrade device.
 *
 * @details Emits the device descriptor, the configuration, one DFU interface
 * with zero endpoints, and the DFU functional descriptor, in that order. The
 * device class is left at 0 so the interface declares the function, matching
 * the mass-storage builder rather than the CDC one, which only carries the
 * miscellaneous triple to hold an interface association.
 *
 * @param[in]  dev      Device identity and power budget.
 * @param[in]  dfu      DFU capabilities and transfer geometry.
 * @param[out] out      Caller-owned destination buffer.
 * @param[in]  cap      Capacity of @p out in bytes.
 * @param[out] out_len  Bytes written on success. Untouched on failure.
 *
 * @return ra8_err_t Result of the encode.
 * @retval k_ra8_ok                 Framework written.
 * @retval k_ra8_err_null_ptr       @p dev, @p dfu, @p out or @p out_len is NULL.
 * @retval k_ra8_err_invalid_arg    @p dfu->transfer_bytes or @p dfu->bcd_dfu is
 *                                  zero, or neither capability bit is set.
 * @retval k_ra8_err_invalid_size   @p cap cannot hold the framework.
 * @retval k_ra8_err_range_check_failed @p dev->max_power_ma exceeds what
 *                                  bMaxPower can encode (500 mA).
 *
 * @pre @p out addresses at least @p cap writable bytes.
 * @post On success @p out holds the framework and @p out_len counts it.
 * @post On failure @p out_len is unchanged and @p out may be partly written.
 *
 * @note Pure; no state is retained between calls. Synthesising a framework
 *       does not attach a device or touch a controller.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_usb_desc_build_dfu(const ra8_usb_desc_device_t* dev,
                                               const ra8_usb_desc_dfu_t*    dfu,
                                               uint8_t*                     out,
                                               uint32_t                     cap,
                                               uint32_t*                    out_len);

#ifdef __cplusplus
}
#endif
