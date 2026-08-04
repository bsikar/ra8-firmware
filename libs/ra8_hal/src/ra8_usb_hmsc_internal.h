/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_usb_hmsc_internal.h
 * @brief Cross-TU surface shared between the host-MSC class layer
 * @ingroup grp_hal_usb
 *        (ra8_usb_hmsc.c) and the polled enumeration ladder
 *        (ra8_usb_hmsc_enum.c).
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * The native host-MSC class layer is split across two translation units
 * so each stays under the 1000-line file-size cap. This src/-local header
 * carries the typed-enum constants that both TUs reference: the chapter-9
 * SETUP field encodings (used to assemble GET_DESCRIPTOR / SET_ADDRESS /
 * SET_CONFIGURATION requests during enumeration) and the standard
 * descriptor / BOT wrapper sizes. It is NOT part of the public API: the
 * public contract lives in `ra8_usb_hmsc.h`, and the shadow-state singleton
 * `s_usb_hmsc_state` is shared through `ra8_hal_internal.h`.
 *
 *
 * @since 0.1.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/**
 * @enum ra8_usb_hmsc_setup_field_t
 * @brief Standard chapter-9 + MSC class request encodings.
 */
typedef enum : uint8_t {
  /* Chapter-9 standard requests (USB 2.0 spec section 9.4). */
  k_ra8_hmsc_bm_std_dev_in       = 0x80U, /**< Std | Device | In.     */
  k_ra8_hmsc_bm_std_dev_out      = 0x00U, /**< Std | Device | Out.    */
  k_ra8_hmsc_bm_std_iface_out    = 0x01U, /**< Std | Interface | Out. */
  k_ra8_hmsc_breq_get_descriptor = 0x06U, /**< GET_DESCRIPTOR.        */
  k_ra8_hmsc_breq_set_address    = 0x05U, /**< SET_ADDRESS.           */
  k_ra8_hmsc_breq_set_config     = 0x09U, /**< SET_CONFIGURATION.     */
  k_ra8_hmsc_breq_set_interface  = 0x0BU, /**< SET_INTERFACE.         */
  /* MSC class-specific request envelope: 0xA1 = D2H | Class | Iface. */
  k_ra8_hmsc_bm_class_iface_in = 0xA1U, /**< Class | Interface | In. */
  /* Descriptor types in wValue's high byte. */
  k_ra8_hmsc_desc_device        = 0x01U, /**< DEVICE descriptor.        */
  k_ra8_hmsc_desc_configuration = 0x02U, /**< CONFIGURATION descriptor. */
  k_ra8_hmsc_desc_interface     = 0x04U, /**< INTERFACE descriptor.     */
  k_ra8_hmsc_desc_endpoint      = 0x05U, /**< ENDPOINT descriptor.      */
} ra8_usb_hmsc_setup_field_t;

/**
 * @enum ra8_usb_hmsc_size_t
 * @brief Standard descriptor sizes and BOT wrapper sizes.
 *
 * @details The CBW / CSW lengths are nailed down by USB MSC BBB rev
 * 1.0 sections 5.1 and 5.2 respectively.
 */
typedef enum : uint16_t {
  k_ra8_hmsc_dev_desc_len     = 18U, /**< USB DEVICE descriptor.        */
  k_ra8_hmsc_cfg_desc_len     = 9U,  /**< CONFIGURATION descriptor hdr. */
  k_ra8_hmsc_iface_desc_len   = 9U,  /**< INTERFACE descriptor.         */
  k_ra8_hmsc_ep_desc_len      = 7U,  /**< ENDPOINT descriptor.          */
  k_ra8_hmsc_assigned_address = 1U,  /**< First assigned device addr.   */
  k_ra8_hmsc_default_config   = 1U,  /**< bConfigurationValue = 1.      */
  k_ra8_hmsc_get_max_lun_len  = 1U,  /**< Get-Max-LUN response len.     */
  k_ra8_hmsc_cbw_len          = 31U, /**< CBW length (BBB sec 5.1).     */
  k_ra8_hmsc_csw_len          = 13U, /**< CSW length (BBB sec 5.2).     */
  k_ra8_hmsc_cdb_max_len      = 16U, /**< CDB ceiling.                  */
  k_ra8_hmsc_cdb6_len         = 6U,  /**< 6-byte SCSI CDB.              */
  k_ra8_hmsc_cdb10_len        = 10U, /**< 10-byte SCSI CDB.             */
} ra8_usb_hmsc_size_t;

#ifdef __cplusplus
}
#endif
