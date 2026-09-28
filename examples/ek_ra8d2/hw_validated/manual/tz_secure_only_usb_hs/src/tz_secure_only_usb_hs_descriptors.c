/**
 * @file
 * examples/ek_ra8d2/hw_validated/manual/tz_secure_only_usb_hs/src/tz_secure_only_usb_hs_descriptors.c
 * @brief USB CDC-ACM descriptor frameworks for the secure-only USB-HS echo app.
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Second sibling translation unit for
 * ``examples/ek_ra8d2/hw_validated/manual/tz_secure_only_usb_hs/src/main.c``.
 * Owns the app's USB identity and endpoint layout and synthesises the four
 * USBX frameworks from it through ``ra8_usb_device_compose``, once per bus
 * speed. This
 * unit used to hold the same four tables hand-typed as 258 lines of byte
 * literals; the bytes are unchanged, they are now derived.
 *
 * Two device frameworks are still built because USBX wants one per bus
 * speed. They come from one identity and two endpoint layouts that differ
 * only in bulk ``wMaxPacketSize`` and ``bInterval``; the high-speed one
 * also carries the device qualifier the USB 2.0 spec requires, which is
 * the whole ten-byte difference between them.
 *
 * The buffers are consumed by ``demo_worker_usbx_init`` in the companion
 * ``tz_secure_only_usb_hs_steps.c`` translation unit, so they keep the
 * cross-TU ``s_tz_secure_only_usb_hs_`` prefix and are declared in
 * ``tz_secure_only_usb_hs_steps.h``. That unit must call
 * ::tz_secure_only_usb_hs_build_frameworks before it reads them.
 *
 * @author Brighton Sikarskie
 * @date 2026-05-03
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "tz_secure_only_usb_hs_steps.h"

#ifndef RA8_OFF_TARGET
#include "ra8_usb_compose.h"
#include "ra8_usb_desc.h"
#include "ux_api.h"

/* -------------------------------------------------------------------------- */
/* USB identity and endpoint layout */
/* -------------------------------------------------------------------------- */

/**
 * @enum demo_usb_identity_t
 * @brief The device identity this app publishes.
 *
 * @details These are the values the hand-typed device descriptor carried in
 * its idVendor / idProduct / bcdDevice / bMaxPower bytes, named rather than
 * spelled out little-endian.
 * @since 0.1.0
 */
typedef enum : uint16_t {
  k_demo_usb_vid          = 0x1209U, /**< idVendor, pid.codes test range. */
  k_demo_usb_pid          = 0x000CU, /**< idProduct.                      */
  k_demo_usb_bcd_device   = 0x0100U, /**< bcdDevice, release 1.00.        */
  k_demo_usb_max_power_ma = 100U,    /**< Bus draw in mA.                 */
} demo_usb_identity_t;

/**
 * @enum demo_usb_endpoint_t
 * @brief The CDC-ACM endpoint layout, addresses as they appear on the wire.
 *
 * @details An IN endpoint carries bit 7 set, so EP1 IN is 0x81 and EP3 IN is
 * 0x83, while EP2 OUT is 0x02. The two bulk packet sizes and the two notify
 * intervals are the only fields that differ between the speeds: USB 2.0
 * mandates 512-byte bulk endpoints at high speed, and the interrupt endpoint
 * counts its bInterval in frames at full speed but in 2^(n-1) microframes at
 * high speed, so 255 ms becomes an 8.
 * @since 0.1.0
 */
typedef enum : uint16_t {
  k_demo_usb_notify_ep          = 0x83U, /**< Interrupt-IN, notifications.  */
  k_demo_usb_notify_bytes       = 8U,    /**< Interrupt-IN max packet size. */
  k_demo_usb_notify_interval_fs = 255U,  /**< bInterval, 255 frames at FS.  */
  k_demo_usb_notify_interval_hs = 8U,    /**< bInterval, 2^7 microframes.   */
  k_demo_usb_out_ep             = 0x02U, /**< Bulk-OUT data pipe.           */
  k_demo_usb_in_ep              = 0x81U, /**< Bulk-IN data pipe.            */
  k_demo_usb_data_bytes_fs      = 64U,   /**< Bulk max packet size at FS.   */
  k_demo_usb_data_bytes_hs      = 512U,  /**< Bulk max packet size at HS.   */
  k_demo_usb_functions          = 1U,    /**< Functions per config.         */
} demo_usb_endpoint_t;

/**
 * @var k_demo_usb_device
 * @brief Device identity handed to the framework builders.
 * @note The three strings are string-literal storage with static duration;
 *       the builders copy them and retain no pointer.
 * @since 0.1.0
 */
static const ra8_usb_desc_device_t k_demo_usb_device = {
  .vid           = (uint16_t)k_demo_usb_vid,
  .pid           = (uint16_t)k_demo_usb_pid,
  .bcd_device    = (uint16_t)k_demo_usb_bcd_device,
  .manufacturer  = "Brighton Sikarskie",
  .product       = "RA8D2 HS CDC Echo",
  .serial        = "RA8D2-CDC-002",
  .langid        = (uint16_t)k_ra8_usb_desc_langid_en_us,
  .max_power_ma  = (uint16_t)k_demo_usb_max_power_ma,
  .self_powered  = false,
  .remote_wakeup = false,
};

/**
 * @var k_demo_usb_cdc_fs
 * @brief Full-speed endpoint layout: 64-byte bulk, no device qualifier.
 * @since 0.1.0
 */
static const ra8_usb_desc_cdc_acm_t k_demo_usb_cdc_fs = {
  .notify_ep          = (uint8_t)k_demo_usb_notify_ep,
  .notify_bytes       = (uint16_t)k_demo_usb_notify_bytes,
  .notify_interval_ms = (uint8_t)k_demo_usb_notify_interval_fs,
  .out_ep             = (uint8_t)k_demo_usb_out_ep,
  .in_ep              = (uint8_t)k_demo_usb_in_ep,
  .data_bytes         = (uint16_t)k_demo_usb_data_bytes_fs,
  .high_speed         = false,
};

/**
 * @var k_demo_usb_cdc_hs
 * @brief High-speed endpoint layout: 512-byte bulk plus the device qualifier.
 * @since 0.1.0
 */
static const ra8_usb_desc_cdc_acm_t k_demo_usb_cdc_hs = {
  .notify_ep          = (uint8_t)k_demo_usb_notify_ep,
  .notify_bytes       = (uint16_t)k_demo_usb_notify_bytes,
  .notify_interval_ms = (uint8_t)k_demo_usb_notify_interval_hs,
  .out_ep             = (uint8_t)k_demo_usb_out_ep,
  .in_ep              = (uint8_t)k_demo_usb_in_ep,
  .data_bytes         = (uint16_t)k_demo_usb_data_bytes_hs,
  .high_speed         = true,
};

/* -------------------------------------------------------------------------- */
/* Synthesised frameworks */
/* -------------------------------------------------------------------------- */

UCHAR s_tz_secure_only_usb_hs_device_framework_fs[k_ra8_usb_desc_framework_bytes_max];

UCHAR s_tz_secure_only_usb_hs_device_framework_hs[k_ra8_usb_desc_framework_bytes_max];

UCHAR s_tz_secure_only_usb_hs_string_framework[k_ra8_usb_desc_strings_bytes_max];

UCHAR s_tz_secure_only_usb_hs_language_id_framework[k_ra8_usb_desc_langid_bytes];

uint32_t s_tz_secure_only_usb_hs_device_framework_fs_len;

uint32_t s_tz_secure_only_usb_hs_device_framework_hs_len;

uint32_t s_tz_secure_only_usb_hs_string_framework_len;

uint32_t s_tz_secure_only_usb_hs_language_id_framework_len;

ra8_err_t tz_secure_only_usb_hs_build_frameworks(void)
{
  /* USBX wants one device framework per bus speed, so this composes twice off
   * one identity. The string and language-id frameworks are written by both
   * calls, into the same two buffers: the encoders are pure and depend only on
   * the identity, so the second pass rewrites the same bytes. That costs a few
   * dozen stores at bring-up and keeps the call shape identical to every
   * single-speed app in the tree. */
  const ra8_usb_class_t cdc_acm_full_speed = {
    .kind    = k_ra8_usb_class_cdc_acm,
    .cdc_acm = k_demo_usb_cdc_fs,
  };

  const ra8_usb_class_t cdc_acm_high_speed = {
    .kind    = k_ra8_usb_class_cdc_acm,
    .cdc_acm = k_demo_usb_cdc_hs,
  };

  ra8_usb_device_cfg_t cfg = {
    .desc        = &k_demo_usb_device,
    .classes     = &cdc_acm_full_speed,
    .class_count = (uint8_t)k_demo_usb_functions,
  };

  ra8_usb_device_frameworks_t fw = {
    .device      = s_tz_secure_only_usb_hs_device_framework_fs,
    .device_cap  = (uint32_t)sizeof(s_tz_secure_only_usb_hs_device_framework_fs),
    .strings     = s_tz_secure_only_usb_hs_string_framework,
    .strings_cap = (uint32_t)sizeof(s_tz_secure_only_usb_hs_string_framework),
    .langid      = s_tz_secure_only_usb_hs_language_id_framework,
    .langid_cap  = (uint32_t)sizeof(s_tz_secure_only_usb_hs_language_id_framework),
  };

  ra8_err_t err = ra8_usb_device_compose(&cfg, &fw);
  if (err != k_ra8_ok) {
    return err;
  }
  s_tz_secure_only_usb_hs_device_framework_fs_len = fw.device_len;

  cfg.classes   = &cdc_acm_high_speed;
  fw.device     = s_tz_secure_only_usb_hs_device_framework_hs;
  fw.device_cap = (uint32_t)sizeof(s_tz_secure_only_usb_hs_device_framework_hs);

  err = ra8_usb_device_compose(&cfg, &fw);
  if (err != k_ra8_ok) {
    return err;
  }
  s_tz_secure_only_usb_hs_device_framework_hs_len = fw.device_len;

  s_tz_secure_only_usb_hs_string_framework_len      = fw.strings_len;
  s_tz_secure_only_usb_hs_language_id_framework_len = fw.langid_len;

  return k_ra8_ok;
}
#endif /* !RA8_OFF_TARGET */
