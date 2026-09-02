/**
 * @file test_ra8_usb_hmsc_enum_fixture.h
 * @brief Protocol constants shared by the host-MSC enumeration coverage fixture.
 * @details Holds descriptor constants and construction helpers used by the
 *          white-box enumeration tests without owning production behavior.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdint.h>

#include "ra8_attributes.h"

/** @brief Descriptor offsets and standard requests the enumeration mock answers. */
typedef enum : uint8_t {
  k_t_iface_off_class    = 5U,    /**< bInterfaceClass offset.             */
  k_t_iface_off_protocol = 7U,    /**< bInterfaceProtocol offset.          */
  k_t_req_set_address    = 0x05U, /**< Standard request SET_ADDRESS.       */
  k_t_req_set_config     = 0x09U, /**< Standard request SET_CONFIGURATION. */
  k_t_req_get_max_lun    = 0xFEU, /**< Class request GET_MAX_LUN.          */
  k_t_byte_mask          = 0xFFU, /**< Descriptor-field low-byte mask.     */
} t_enum_desc_t;

/** @brief Blob capacity and the mock clock step that bounds the attach wait. */
typedef enum : uint16_t {
  k_t_cfg_blob_cap = 64U,   /**< Configuration-descriptor scratch, bytes.          */
  k_t_time_step_us = 1500U, /**< Clock step used to cross the 2000-us wait budget. */
} t_enum_budget_t;

/** @brief USB descriptor byte constants used to craft mock responses. */
typedef enum : uint16_t {
  k_tc_dev_desc_len   = 18U,     /**< DEVICE descriptor length.          */
  k_tc_cfg_hdr_len    = 9U,      /**< CONFIGURATION header length.       */
  k_tc_iface_desc_len = 9U,      /**< INTERFACE descriptor length.       */
  k_tc_ep_desc_len    = 7U,      /**< ENDPOINT descriptor length.        */
  k_tc_dtype_device   = 0x01U,   /**< bDescriptorType DEVICE.            */
  k_tc_dtype_config   = 0x02U,   /**< bDescriptorType CONFIGURATION.     */
  k_tc_dtype_iface    = 0x04U,   /**< bDescriptorType INTERFACE.         */
  k_tc_dtype_endpoint = 0x05U,   /**< bDescriptorType ENDPOINT.          */
  k_tc_class_msc      = 0x08U,   /**< bInterfaceClass mass storage.      */
  k_tc_subclass_scsi  = 0x06U,   /**< bInterfaceSubClass SCSI.           */
  k_tc_protocol_bbb   = 0x50U,   /**< bInterfaceProtocol Bulk-Only.      */
  k_tc_class_hid      = 0x03U,   /**< A non-MSC interface class.         */
  k_tc_sub_rbc        = 0x01U,   /**< A non-SCSI subclass (RBC).         */
  k_tc_proto_cbi      = 0x51U,   /**< A non-BOT protocol (CBI).          */
  k_tc_ep_in_addr     = 0x81U,   /**< bEndpointAddress: IN, endpoint 1.  */
  k_tc_ep_out_addr    = 0x02U,   /**< bEndpointAddress: OUT, endpoint 2. */
  k_tc_ep_in2_addr    = 0x83U,   /**< A second IN endpoint (num 3).      */
  k_tc_ep_out2_addr   = 0x04U,   /**< A second OUT endpoint (num 4).     */
  k_tc_attr_bulk      = 0x02U,   /**< bmAttributes bulk transfer.        */
  k_tc_attr_int       = 0x03U,   /**< bmAttributes interrupt transfer.   */
  k_tc_mps_lo         = 0x40U,   /**< wMaxPacketSize LSB (64).           */
  k_tc_mps            = 0x0040U, /**< wMaxPacketSize (64).               */
  k_tc_vid            = 0x1234U, /**< Fabricated idVendor.               */
  k_tc_pid            = 0xABCDU, /**< Fabricated idProduct.              */
  k_tc_off_vid        = 8U,      /**< idVendor LSB offset in DEVICE.     */
  k_tc_off_pid        = 10U,     /**< idProduct LSB offset in DEVICE.    */
  k_tc_off_total      = 2U,      /**< wTotalLength LSB in CONFIGURATION. */
  k_tc_off_cfgval     = 5U,      /**< bConfigurationValue in CONFIG.     */
  k_tc_cfg_value      = 1U,      /**< bConfigurationValue we advertise.  */
  k_tc_iface_number   = 0U,      /**< bInterfaceNumber we advertise.     */
  k_tc_clamp_total    = 200U,    /**< A wTotalLength above the 128 cap.  */
  k_tc_lun_val        = 3U,      /**< A GET_MAX_LUN payload value.       */
  k_tc_byte_bits      = 8U,      /**< Bit width of one byte.             */
} test_enum_cov_desc_t;

/**
 * @brief Lay out one canonical MSC configuration descriptor set.
 * @param[out] buf Destination with room for configuration, interface, and endpoints.
 * @return Total descriptor-set byte count.
 * @pre @p buf provides at least 32 bytes.
 * @post The descriptor total-length field matches the returned byte count.
 */
RA8_INTERNAL static uint16_t internal_build_msc_config(uint8_t* buf)
{
  uint16_t o                      = 0U;
  buf[o + 0U]                     = (uint8_t)k_tc_cfg_hdr_len;
  buf[o + 1U]                     = (uint8_t)k_tc_dtype_config;
  buf[o + 2U]                     = 0U;
  buf[o + 3U]                     = 0U;
  buf[o + 4U]                     = 1U;
  buf[o + k_t_iface_off_class]    = (uint8_t)k_tc_cfg_value;
  buf[o + 6U]                     = 0U;
  buf[o + k_t_iface_off_protocol] = 0U;
  buf[o + 8U]                     = 0U;
  o                               = (uint16_t)(o + k_tc_cfg_hdr_len);
  buf[o + 0U]                     = (uint8_t)k_tc_iface_desc_len;
  buf[o + 1U]                     = (uint8_t)k_tc_dtype_iface;
  buf[o + 2U]                     = (uint8_t)k_tc_iface_number;
  buf[o + 3U]                     = 0U;
  buf[o + 4U]                     = 2U;
  buf[o + k_t_iface_off_class]    = (uint8_t)k_tc_class_msc;
  buf[o + 6U]                     = (uint8_t)k_tc_subclass_scsi;
  buf[o + k_t_iface_off_protocol] = (uint8_t)k_tc_protocol_bbb;
  buf[o + 8U]                     = 0U;
  o                               = (uint16_t)(o + k_tc_iface_desc_len);
  buf[o + 0U]                     = (uint8_t)k_tc_ep_desc_len;
  buf[o + 1U]                     = (uint8_t)k_tc_dtype_endpoint;
  buf[o + 2U]                     = (uint8_t)k_tc_ep_in_addr;
  buf[o + 3U]                     = (uint8_t)k_tc_attr_bulk;
  buf[o + 4U]                     = (uint8_t)k_tc_mps_lo;
  buf[o + k_t_iface_off_class]    = 0U;
  buf[o + 6U]                     = 0U;
  o                               = (uint16_t)(o + k_tc_ep_desc_len);
  buf[o + 0U]                     = (uint8_t)k_tc_ep_desc_len;
  buf[o + 1U]                     = (uint8_t)k_tc_dtype_endpoint;
  buf[o + 2U]                     = (uint8_t)k_tc_ep_out_addr;
  buf[o + 3U]                     = (uint8_t)k_tc_attr_bulk;
  buf[o + 4U]                     = (uint8_t)k_tc_mps_lo;
  buf[o + k_t_iface_off_class]    = 0U;
  buf[o + 6U]                     = 0U;
  o                               = (uint16_t)(o + k_tc_ep_desc_len);
  buf[k_tc_off_total]             = (uint8_t)(o & k_t_byte_mask);
  return o;
}
