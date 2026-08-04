/**
 * @file examples/ek_ra8d2/hw_pending/manual/usb_printer_vendor/inc/usb_printer_vendor_ch9.h
 * @brief Chapter-9 SETUP router + descriptor tables for the printer/vendor demo
 *
 * @par Tag
 * [Ring 6 / APP] {World: NS}
 *
 * @details
 * The pure, hardware-free heart of the ``usb_printer_vendor`` example. The
 * native ``ra8_usb`` device driver plus the ``ra8_usb_pprn`` (Printer) and
 * ``ra8_usb_pvnd`` (vendor-specific) class layers do not carry a USB
 * chapter-9 responder of their own -- they expect the application to answer
 * the standard GET_DESCRIPTOR / SET_ADDRESS / SET_CONFIGURATION requests and
 * to route the class / vendor SETUPs into the class layers. This module
 * factors that decision logic out of ``main.c`` into ``static inline`` pure
 * functions plus the descriptor byte tables, so both the firmware and the
 * host test compile the SAME code and it can be exercised with full MC/DC
 * coverage (the header-only pattern, mirroring ``er_pageturn.h``):
 *
 *  - ::pv_route_setup classifies an incoming 8-byte SETUP envelope into a
 *    ::pv_action_t (which descriptor to stage, whether to ACK a no-data
 *    request, or which class layer owns the request).
 *  - ::pv_descriptor hands back the byte table + length for a descriptor
 *    action.
 *  - ::pv_clamp_len applies the ``wLength`` ceiling the host advertises.
 *  - ::pv_is_printer_class_request is the compound recognition predicate the
 *    Printer interface uses (USB Printer Class 1.1 sec 4.2).
 *
 * The composite configuration advertises two interfaces so a single
 * enumeration exercises both class layers:
 *
 *  - IF0: Printer, class 0x07 / subclass 0x01 / protocol 0x02 (bi-directional).
 *    Bulk OUT EP 0x01 (print data) + bulk IN EP 0x82 (printer status).
 *  - IF1: Vendor specific, class 0xFF. Bulk IN EP 0x81 + bulk OUT EP 0x02.
 *
 * The wire-format descriptor byte tables follow USB 2.0 sec 9.6; the byte
 * values are protocol constants documented inline (the same convention the
 * other USB device examples use for their descriptor blobs).
 *
 * Reference: USB 2.0 sec 9.4 "Standard Device Requests", sec 9.6 "Standard
 * USB Descriptor Definitions"; USB Printer Class 1.1 sec 4.2 "Class Specific
 * Requests".
 *
 * @author Brighton Sikarskie
 * @date 2026-07-16
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_usb.h"

/* =============================================================================
 * SETUP-envelope field constants (USB 2.0 sec 9.3 "USB Device Requests")
 * =============================================================================
 */

/**
 * @enum pv_req_field_t
 * @brief Masks + values used to decode a SETUP ``bmRequestType`` byte.
 *
 * @details Per USB 2.0 sec 9.3 Table 9-2: bit 7 is direction, bits 6:5 are
 * the request type, bits 4:0 are the recipient. The class layer needs the
 * type field (standard / class / vendor) to route the request and the
 * recipient field to recognise the interface-directed Printer requests.
 *
 * @see pv_route_setup
 */
typedef enum : uint8_t {
  k_pv_bm_dir_mask      = 0x80U, /**< Direction bit 7 (1 = device->host). */
  k_pv_bm_dir_in        = 0x80U, /**< Device-to-host control read.        */
  k_pv_bm_type_mask     = 0x60U, /**< Request-type field, bits 6:5.       */
  k_pv_bm_type_standard = 0x00U, /**< Standard request.                   */
  k_pv_bm_type_class    = 0x20U, /**< Class request.                      */
  k_pv_bm_type_vendor   = 0x40U, /**< Vendor request.                     */
  k_pv_bm_recip_mask    = 0x1FU, /**< Recipient field, bits 4:0.          */
  k_pv_bm_recip_iface   = 0x01U, /**< Interface recipient.                */
} pv_req_field_t;

/**
 * @enum pv_std_request_t
 * @brief Standard ``bRequest`` codes this device answers (USB 2.0 Table 9-4).
 *
 * @details The chapter-9 responder only needs the three requests the host
 * issues during enumeration of a bulk-only device: GET_DESCRIPTOR (the bulk
 * of the traffic), SET_ADDRESS, and SET_CONFIGURATION. All other standard
 * requests are stalled.
 */
typedef enum : uint8_t {
  k_pv_std_get_descriptor    = 0x06U, /**< GET_DESCRIPTOR.    */
  k_pv_std_set_address       = 0x05U, /**< SET_ADDRESS.       */
  k_pv_std_set_configuration = 0x09U, /**< SET_CONFIGURATION. */
} pv_std_request_t;

/**
 * @enum pv_desc_type_t
 * @brief ``bDescriptorType`` values carried in the GET_DESCRIPTOR ``wValue``
 *        high byte (USB 2.0 Table 9-5).
 */
typedef enum : uint8_t {
  k_pv_dt_device = 0x01U, /**< DEVICE descriptor.        */
  k_pv_dt_config = 0x02U, /**< CONFIGURATION descriptor. */
  k_pv_dt_string = 0x03U, /**< STRING descriptor.        */
} pv_desc_type_t;

/**
 * @enum pv_printer_request_t
 * @brief Printer class ``bRequest`` codes (USB Printer Class 1.1 sec 4.2).
 */
typedef enum : uint8_t {
  k_pv_pprn_get_device_id   = 0x00U, /**< GET_DEVICE_ID.   */
  k_pv_pprn_get_port_status = 0x01U, /**< GET_PORT_STATUS. */
  k_pv_pprn_soft_reset      = 0x02U, /**< SOFT_RESET.      */
} pv_printer_request_t;

/* =============================================================================
 * Router action
 * =============================================================================
 */

/**
 * @enum pv_action_t
 * @brief The decision ::pv_route_setup produces for a SETUP envelope.
 *
 * @details Each value tells ``main.c`` exactly which side effect to run on
 * the default control pipe (DCP / EP0): stage a specific descriptor, ACK a
 * no-data request, hand off to a class layer, or STALL an unsupported
 * request.
 *
 * @see pv_route_setup
 * @see pv_descriptor
 */
typedef enum : uint8_t {
  k_pv_action_stall             = 0U, /**< Unsupported: STALL EP0.               */
  k_pv_action_get_device_desc   = 1U, /**< Stage the DEVICE descriptor.          */
  k_pv_action_get_config_desc   = 2U, /**< Stage the CONFIGURATION descriptor.   */
  k_pv_action_get_string_desc   = 3U, /**< Stage the STRING (LANGID) descriptor. */
  k_pv_action_set_address       = 4U, /**< SET_ADDRESS: SIE-owned, ACK.          */
  k_pv_action_set_configuration = 5U, /**< SET_CONFIGURATION: ACK + go live.     */
  k_pv_action_printer_class     = 6U, /**< Route to the Printer class layer.     */
  k_pv_action_vendor_class      = 7U, /**< Route to the Vendor class layer.      */
} pv_action_t;

/* =============================================================================
 * Descriptor tables (USB 2.0 sec 9.6 "Standard USB Descriptor Definitions")
 * =============================================================================
 */

/**
 * @var g_pv_device_desc
 * @brief 18-byte DEVICE descriptor for the composite printer + vendor gadget.
 * @details bDeviceClass = 0x00 (class declared per interface), 64-byte EP0,
 *          VID/PID from the pid.codes test range, one configuration.
 * @note Read-only wire-format constant.
 * @since 0.1.0
 */
static const uint8_t g_pv_device_desc[] = {
  0x12U, /* bLength = 18             */
  0x01U, /* bDescriptorType = DEVICE */
  0x00U,
  0x02U, /* bcdUSB = 2.00                */
  0x00U, /* bDeviceClass = 0 (per-iface) */
  0x00U, /* bDeviceSubClass              */
  0x00U, /* bDeviceProtocol              */
  0x40U, /* bMaxPacketSize0 = 64         */
  0x09U,
  0x12U, /* idVendor  = 0x1209 (pid.codes) */
  0x01U,
  0x00U, /* idProduct = 0x0001 */
  0x00U,
  0x01U, /* bcdDevice = 1.00       */
  0x00U, /* iManufacturer = 0      */
  0x00U, /* iProduct = 0           */
  0x00U, /* iSerialNumber = 0      */
  0x01U, /* bNumConfigurations = 1 */
};

/**
 * @var g_pv_config_desc
 * @brief 55-byte CONFIGURATION descriptor: IF0 Printer + IF1 Vendor.
 * @details wTotalLength = 0x0037. IF0 is Printer 0x07/0x01/0x02 with bulk OUT
 *          EP 0x01 + bulk IN EP 0x82; IF1 is Vendor 0xFF with bulk IN EP 0x81
 *          + bulk OUT EP 0x02. Endpoint addresses match the pipe assignments
 *          ra8_usb_pprn (PIPE3/PIPE4) and ra8_usb_pvnd (PIPE5/PIPE1) program.
 * @note Read-only wire-format constant.
 * @since 0.1.0
 */
static const uint8_t g_pv_config_desc[] = {
  /* Configuration descriptor (USB 2.0 sec 9.6.3) -- 9 bytes. */
  0x09U, /* bLength                         */
  0x02U, /* bDescriptorType = CONFIGURATION */
  0x37U,
  0x00U, /* wTotalLength = 55          */
  0x02U, /* bNumInterfaces = 2         */
  0x01U, /* bConfigurationValue = 1    */
  0x00U, /* iConfiguration = 0         */
  0x80U, /* bmAttributes = bus-powered */
  0x32U, /* bMaxPower = 100 mA         */
  /* IF0 -- Printer, bi-directional (USB Printer Class 1.1 sec 4.1). */
  0x09U, /* bLength                       */
  0x04U, /* bDescriptorType = INTERFACE   */
  0x00U, /* bInterfaceNumber = 0          */
  0x00U, /* bAlternateSetting = 0         */
  0x02U, /* bNumEndpoints = 2             */
  0x07U, /* bInterfaceClass = Printer     */
  0x01U, /* bInterfaceSubClass = Printers */
  0x02U, /* bInterfaceProtocol = bi-dir   */
  0x00U, /* iInterface = 0                */
  /* Printer bulk OUT endpoint (print data). */
  0x07U, /* bLength                    */
  0x05U, /* bDescriptorType = ENDPOINT */
  0x01U, /* bEndpointAddress = EP1 OUT */
  0x02U, /* bmAttributes = bulk        */
  0x40U,
  0x00U, /* wMaxPacketSize = 64 */
  0x00U, /* bInterval = 0       */
  /* Printer bulk IN endpoint (status). */
  0x07U, /* bLength                    */
  0x05U, /* bDescriptorType = ENDPOINT */
  0x82U, /* bEndpointAddress = EP2 IN  */
  0x02U, /* bmAttributes = bulk        */
  0x40U,
  0x00U, /* wMaxPacketSize = 64 */
  0x00U, /* bInterval = 0       */
  /* IF1 -- Vendor specific (USB-IF class 0xFF). */
  0x09U, /* bLength                     */
  0x04U, /* bDescriptorType = INTERFACE */
  0x01U, /* bInterfaceNumber = 1        */
  0x00U, /* bAlternateSetting = 0       */
  0x02U, /* bNumEndpoints = 2           */
  0xFFU, /* bInterfaceClass = Vendor    */
  0x00U, /* bInterfaceSubClass          */
  0x00U, /* bInterfaceProtocol          */
  0x00U, /* iInterface = 0              */
  /* Vendor bulk IN endpoint. */
  0x07U, /* bLength                    */
  0x05U, /* bDescriptorType = ENDPOINT */
  0x81U, /* bEndpointAddress = EP1 IN  */
  0x02U, /* bmAttributes = bulk        */
  0x40U,
  0x00U, /* wMaxPacketSize = 64 */
  0x00U, /* bInterval = 0       */
  /* Vendor bulk OUT endpoint. */
  0x07U, /* bLength                    */
  0x05U, /* bDescriptorType = ENDPOINT */
  0x02U, /* bEndpointAddress = EP2 OUT */
  0x02U, /* bmAttributes = bulk        */
  0x40U,
  0x00U, /* wMaxPacketSize = 64 */
  0x00U, /* bInterval = 0       */
};

/**
 * @var g_pv_string0_desc
 * @brief STRING descriptor index 0 -- the supported-language ID array.
 * @details Single LANGID 0x0409 (English, US), little-endian, per USB 2.0
 *          sec 9.6.7. The device advertises no string indices, so this is the
 *          only string the host requests.
 * @note Read-only wire-format constant.
 * @since 0.1.0
 */
static const uint8_t g_pv_string0_desc[] = {
  0x04U, /* bLength = 4              */
  0x03U, /* bDescriptorType = STRING */
  0x09U,
  0x04U, /* wLANGID[0] = 0x0409 (en-US) */
};

/* =============================================================================
 * Router helpers
 * =============================================================================
 */

/**
 * @brief Route a standard GET_DESCRIPTOR by its ``wValue`` high byte.
 *
 * @param[in] setup Decoded SETUP (non-NULL, standard GET_DESCRIPTOR).
 *
 * @return ::pv_action_t The resolved descriptor action, or
 *         ::k_pv_action_stall for an unknown descriptor type.
 *
 * @pre @p setup is non-NULL (caller-guaranteed).
 * @pre The request is a standard GET_DESCRIPTOR.
 * @post No state mutated (pure function).
 *
 * @note Reentrant and thread-safe.
 * @since 0.1.0
 */
static inline pv_action_t pv_route_get_descriptor(const ra8_usb_setup_t* setup)
{
  const uint8_t desc_type = (uint8_t)(setup->w_value >> 8U);
  switch (desc_type) {
    case (uint8_t)k_pv_dt_device:
      return k_pv_action_get_device_desc;
    case (uint8_t)k_pv_dt_config:
      return k_pv_action_get_config_desc;
    case (uint8_t)k_pv_dt_string:
      return k_pv_action_get_string_desc;
    default:
      return k_pv_action_stall;
  }
}

/**
 * @brief Route a standard request by its ``bRequest`` code.
 *
 * @param[in] setup Decoded SETUP (non-NULL, standard request type).
 *
 * @return ::pv_action_t The resolved standard action, or ::k_pv_action_stall.
 *
 * @pre @p setup is non-NULL (caller-guaranteed).
 * @pre The request type is standard.
 * @post No state mutated (pure function).
 *
 * @note Reentrant and thread-safe.
 * @since 0.1.0
 */
static inline pv_action_t pv_route_standard(const ra8_usb_setup_t* setup)
{
  switch (setup->b_request) {
    case (uint8_t)k_pv_std_get_descriptor:
      return pv_route_get_descriptor(setup);
    case (uint8_t)k_pv_std_set_address:
      return k_pv_action_set_address;
    case (uint8_t)k_pv_std_set_configuration:
      return k_pv_action_set_configuration;
    default:
      return k_pv_action_stall;
  }
}

/* =============================================================================
 * Public router + accessors
 * =============================================================================
 */

/**
 * @brief Classify an 8-byte SETUP envelope into a ::pv_action_t.
 *
 * @details
 * Decodes ``bmRequestType`` bits 6:5 (request type). Standard requests are
 * further switched on ``bRequest``: GET_DESCRIPTOR maps to a per-descriptor
 * action keyed on the ``wValue`` high byte, SET_ADDRESS and SET_CONFIGURATION
 * map to their own actions, and everything else STALLs. Class requests route
 * to ::k_pv_action_printer_class and vendor requests to
 * ::k_pv_action_vendor_class; the class layers then validate the specific
 * request. A ``NULL`` envelope STALLs.
 *
 * @param[in] setup Decoded SETUP packet (may be ``NULL``).
 *
 * @return ::pv_action_t The decoded action.
 * @retval k_pv_action_stall             ``setup`` is ``NULL`` or unsupported.
 * @retval k_pv_action_get_device_desc   Standard GET_DESCRIPTOR(DEVICE).
 * @retval k_pv_action_get_config_desc   Standard GET_DESCRIPTOR(CONFIGURATION).
 * @retval k_pv_action_get_string_desc   Standard GET_DESCRIPTOR(STRING).
 * @retval k_pv_action_set_address       Standard SET_ADDRESS.
 * @retval k_pv_action_set_configuration Standard SET_CONFIGURATION.
 * @retval k_pv_action_printer_class     Any class-type request.
 * @retval k_pv_action_vendor_class      Any vendor-type request.
 *
 * @pre None (``NULL`` tolerated).
 * @post No state mutated (pure function).
 *
 * @note Reentrant and thread-safe: reads only the argument.
 * @see pv_descriptor
 * @since 0.1.0
 */
static inline pv_action_t pv_route_setup(const ra8_usb_setup_t* setup)
{
  if (setup == nullptr) {
    return k_pv_action_stall;
  }
  const uint8_t req_type = (uint8_t)(setup->bm_request_type & (uint8_t)k_pv_bm_type_mask);
  switch (req_type) {
    case (uint8_t)k_pv_bm_type_standard:
      return pv_route_standard(setup);
    case (uint8_t)k_pv_bm_type_class:
      return k_pv_action_printer_class;
    case (uint8_t)k_pv_bm_type_vendor:
      return k_pv_action_vendor_class;
    default:
      return k_pv_action_stall;
  }
}

/**
 * @brief Recognise an interface-directed Printer class-specific request.
 *
 * @details
 * The compound predicate the Printer interface applies before it answers a
 * class SETUP: the recipient field must be "interface" AND the ``bRequest``
 * must be one of the three Printer 1.1 requests (GET_DEVICE_ID,
 * GET_PORT_STATUS, SOFT_RESET). USB Printer Class 1.1 sec 4.2.
 *
 * @param[in] setup Decoded SETUP packet (may be ``NULL``).
 *
 * @return ``true`` when the envelope is an interface-recipient Printer
 *         request, ``false`` otherwise (including ``NULL``).
 *
 * @pre None (``NULL`` tolerated).
 * @post No state mutated (pure function).
 *
 * @note Reentrant and thread-safe.
 * @see pv_route_setup
 * @since 0.1.0
 */
static inline bool pv_is_printer_class_request(const ra8_usb_setup_t* setup)
{
  if (setup == nullptr) {
    return false;
  }
  const bool is_iface =
    ((setup->bm_request_type & (uint8_t)k_pv_bm_recip_mask) == (uint8_t)k_pv_bm_recip_iface);
  const bool is_known = ((setup->b_request == (uint8_t)k_pv_pprn_get_device_id) ||
                         (setup->b_request == (uint8_t)k_pv_pprn_get_port_status) ||
                         (setup->b_request == (uint8_t)k_pv_pprn_soft_reset));
  return is_iface && is_known;
}

/**
 * @brief Fetch the descriptor byte table + length for a descriptor action.
 *
 * @param[in]  action   A ::pv_action_t. Only the three GET_DESCRIPTOR actions
 *                       resolve to a table.
 * @param[out] out_desc Receives a pointer to the static descriptor bytes.
 * @param[out] out_len  Receives the descriptor's full byte length.
 *
 * @return ``true`` when @p action names a descriptor and the pointers were
 *         populated; ``false`` for a non-descriptor action or a ``NULL``
 *         output pointer.
 *
 * @pre @p out_desc and @p out_len are non-``NULL``.
 * @post On ``true``, ``*out_desc`` / ``*out_len`` describe a static buffer.
 * @post On ``false``, the outputs are left untouched.
 *
 * @note Reentrant and thread-safe: returns pointers to ``static const`` data.
 * @see pv_route_setup
 * @since 0.1.0
 */
static inline bool pv_descriptor(pv_action_t action, const uint8_t** out_desc, uint16_t* out_len)
{
  if ((out_desc == nullptr) || (out_len == nullptr)) {
    return false;
  }
  switch (action) {
    case k_pv_action_get_device_desc:
      *out_desc = g_pv_device_desc;
      *out_len  = (uint16_t)sizeof(g_pv_device_desc);
      return true;
    case k_pv_action_get_config_desc:
      *out_desc = g_pv_config_desc;
      *out_len  = (uint16_t)sizeof(g_pv_config_desc);
      return true;
    case k_pv_action_get_string_desc:
      *out_desc = g_pv_string0_desc;
      *out_len  = (uint16_t)sizeof(g_pv_string0_desc);
      return true;
    default:
      return false;
  }
}

/**
 * @brief Clamp a descriptor length to the host's advertised ``wLength``.
 *
 * @param[in] available Full descriptor length the device can offer.
 * @param[in] requested The host's ``wLength`` ceiling.
 *
 * @return ``min(available, requested)``.
 *
 * @pre None.
 * @post Return value is <= both arguments.
 *
 * @note Reentrant and thread-safe.
 * @since 0.1.0
 */
static inline uint16_t pv_clamp_len(uint16_t available, uint16_t requested)
{
  return (available < requested) ? available : requested;
}

#ifdef __cplusplus
}
#endif
