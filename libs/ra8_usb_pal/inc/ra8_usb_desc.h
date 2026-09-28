/**
 * @file ra8_usb_desc.h
 * @brief Synthesise the three USB device frameworks from a config struct (#766).
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
} ra8_usb_desc_cdc_acm_t;

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

#ifdef __cplusplus
}
#endif
