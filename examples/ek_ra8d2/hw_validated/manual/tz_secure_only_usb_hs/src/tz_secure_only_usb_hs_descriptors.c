/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
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
 * ``examples/ek_ra8d2/hw_validated/manual/tz_secure_only_usb_hs/main.c``.
 * Holds the four USBX descriptor tables (HS device framework, FS device
 * framework, string framework, language-id framework). These byte arrays
 * were moved here verbatim from ``main.c`` (a pure, behaviour-preserving
 * code move) so that every translation unit stays under the 1000-line
 * ``check_file_size.py`` cap.
 *
 * The arrays are consumed by ``demo_worker_usbx_init`` in the companion
 * ``tz_secure_only_usb_hs_steps.c`` translation unit, so they carry the
 * cross-TU ``s_tz_secure_only_usb_hs_`` prefix and are declared in
 * ``tz_secure_only_usb_hs_steps.h`` with their explicit dimensions (a
 * ``static_assert`` here cross-checks each declared dimension against the
 * actual initializer length).
 *
 * @author Brighton Sikarskie
 * @date 2026-05-03
 * @since 0.1.0
 */

#include <stdint.h>

#include "tz_secure_only_usb_hs_steps.h"

#ifndef RA8_OFF_TARGET
#include "ux_api.h"

/* -------------------------------------------------------------------------- */
/* USB descriptors (DEVICE + CONFIG + IAD + CDC interfaces + endpoints) */
/* -------------------------------------------------------------------------- */

/* CDC-ACM composite descriptor frameworks. Two slots are required so
 * USBX can hand the right one to the host based on the negotiated bus
 * speed. The HS slot (s_tz_secure_only_usb_hs_device_framework_hs)
 * carries 512-byte bulk wMaxPacketSize values, which the USB 2.0 spec
 * MANDATES for HS bulk endpoints; the FS fallback keeps 64-byte bulk MPS
 * for FS-only hosts. All other fields (Device, Configuration, IAD, CDC
 * class descriptors, Interrupt EP) are identical between the two -- only
 * the two bulk EP wMaxPacketSize bytes differ. */
UCHAR s_tz_secure_only_usb_hs_device_framework_fs[] = {
  /* Device descriptor (USB 2.0 sec 9.6.1) -- 18 bytes. */
  0x12U,
  0x01U,
  0x00U,
  0x02U,
  0xEFU,
  0x02U,
  0x01U,
  0x40U,
  0x09U, /* idVendor  lo: VID = 0x1209 (pid.codes open-source VID). */
  0x12U, /* idVendor  hi                                            */
  0x0CU, /* idProduct lo: PID = 0x000C (HS CDC ACM).                */
  0x00U, /* idProduct hi                                            */
  0x00U,
  0x01U,
  0x01U,
  0x02U,
  0x03U,
  0x01U,
  /* Configuration descriptor (75 bytes total). */
  0x09U,
  0x02U,
  0x4BU,
  0x00U,
  0x02U,
  0x01U,
  0x00U,
  0x80U,
  0x32U,
  /* Interface association (CDC). */
  0x08U,
  0x0BU,
  0x00U,
  0x02U,
  0x02U,
  0x02U,
  0x01U,
  0x00U,
  /* Communications interface (CDC ACM). */
  0x09U,
  0x04U,
  0x00U,
  0x00U,
  0x01U,
  0x02U,
  0x02U,
  0x01U,
  0x00U,
  /* CDC header functional descriptor. bcdCDC = 0x0120 (CDC 1.20). */
  0x05U,
  0x24U,
  0x00U,
  0x20U,
  0x01U,
  /* Call-management functional descriptor. */
  0x05U,
  0x24U,
  0x01U,
  0x01U,
  0x01U,
  /* ACM functional descriptor. */
  0x04U,
  0x24U,
  0x02U,
  0x02U,
  /* Union functional descriptor. */
  0x05U,
  0x24U,
  0x06U,
  0x00U,
  0x01U,
  /* Interrupt-IN endpoint (EP3 IN, 8-byte MPS, 255 ms poll). */
  0x07U,
  0x05U,
  0x83U,
  0x03U,
  0x08U,
  0x00U,
  0xFFU,
  /* Data-class interface. */
  0x09U,
  0x04U,
  0x01U,
  0x00U,
  0x02U,
  0x0AU,
  0x00U,
  0x00U,
  0x00U,
  /* Bulk-OUT endpoint (EP2 OUT, 64-byte MPS). */
  0x07U,
  0x05U,
  0x02U,
  0x02U,
  0x40U,
  0x00U,
  0x00U,
  /* Bulk-IN endpoint (EP1 IN, 64-byte MPS). */
  0x07U,
  0x05U,
  0x81U,
  0x02U,
  0x40U,
  0x00U,
  0x00U,
};

static_assert(sizeof(s_tz_secure_only_usb_hs_device_framework_fs) ==
                k_tz_secure_only_usb_hs_device_framework_fs_len,
              "FS device framework length must match the header declaration");

/* HS variant: identical to s_tz_secure_only_usb_hs_device_framework_fs
 * except the bulk endpoints carry wMaxPacketSize = 512 (0x0200) per USB
 * 2.0 sec 5.8.3 ("HS bulk endpoints must use 512-byte MPS"). Includes a
 * DEVICE_QUALIFIER descriptor (USB 2.0 sec 9.6.2), which is
 * mandatory for HS-capable devices -- the host (e.g. macOS)
 * issues GET_DESCRIPTOR(DEVICE_QUALIFIER) during enumeration to
 * learn the device's other-speed capabilities, and a STALL
 * response causes some hosts to abandon enumeration. */
UCHAR s_tz_secure_only_usb_hs_device_framework_hs[] = {
  /* Device descriptor (USB 2.0 sec 9.6.1) -- 18 bytes. */
  0x12U,
  0x01U,
  0x00U,
  0x02U,
  0xEFU,
  0x02U,
  0x01U,
  0x40U,
  0x09U, /* idVendor  lo: VID = 0x1209 (pid.codes open-source VID). */
  0x12U, /* idVendor  hi                                            */
  0x0CU, /* idProduct lo: PID = 0x000C (HS CDC ACM).                */
  0x00U, /* idProduct hi                                            */
  0x00U,
  0x01U,
  0x01U,
  0x02U,
  0x03U,
  0x01U,
  /* Device Qualifier descriptor (USB 2.0 sec 9.6.2) -- 10 bytes.
   * Reports the device's capabilities at the OTHER speed (FS in this
   * case). bcdUSB=0x0200, class/subclass/protocol mirror the device
   * descriptor, MPS0=64, num-configurations=1, reserved=0. */
  0x0AU,
  0x06U,
  0x00U,
  0x02U,
  0xEFU,
  0x02U,
  0x01U,
  0x40U,
  0x01U,
  0x00U,
  /* Configuration descriptor (75 bytes total). */
  0x09U,
  0x02U,
  0x4BU,
  0x00U,
  0x02U,
  0x01U,
  0x00U,
  0x80U,
  0x32U,
  /* Interface association (CDC). */
  0x08U,
  0x0BU,
  0x00U,
  0x02U,
  0x02U,
  0x02U,
  0x01U,
  0x00U,
  /* Communications interface (CDC ACM). */
  0x09U,
  0x04U,
  0x00U,
  0x00U,
  0x01U,
  0x02U,
  0x02U,
  0x01U,
  0x00U,
  /* CDC header functional descriptor. bcdCDC = 0x0120 (CDC 1.20). */
  0x05U,
  0x24U,
  0x00U,
  0x20U,
  0x01U,
  /* Call-management functional descriptor. */
  0x05U,
  0x24U,
  0x01U,
  0x01U,
  0x01U,
  /* ACM functional descriptor. */
  0x04U,
  0x24U,
  0x02U,
  0x02U,
  /* Union functional descriptor. */
  0x05U,
  0x24U,
  0x06U,
  0x00U,
  0x01U,
  /* Interrupt-IN endpoint (EP3 IN, 8-byte MPS, bInterval=8 microframes
   * = 1 ms poll). HS uses bInterval=2^(n-1)*125us; n=8 -> 16ms. */
  0x07U,
  0x05U,
  0x83U,
  0x03U,
  0x08U,
  0x00U,
  0x08U,
  /* Data-class interface. */
  0x09U,
  0x04U,
  0x01U,
  0x00U,
  0x02U,
  0x0AU,
  0x00U,
  0x00U,
  0x00U,
  /* Bulk-OUT endpoint (EP2 OUT, 512-byte MPS for HS). */
  0x07U,
  0x05U,
  0x02U,
  0x02U,
  0x00U,
  0x02U,
  0x00U,
  /* Bulk-IN endpoint (EP1 IN, 512-byte MPS for HS). */
  0x07U,
  0x05U,
  0x81U,
  0x02U,
  0x00U,
  0x02U,
  0x00U,
};

static_assert(sizeof(s_tz_secure_only_usb_hs_device_framework_hs) ==
                k_tz_secure_only_usb_hs_device_framework_hs_len,
              "HS device framework length must match the header declaration");

/**
 * @var s_tz_secure_only_usb_hs_string_framework
 * @brief USBX string descriptor table (vendor / product / serial).
 * @since 0.1.0
 */
UCHAR s_tz_secure_only_usb_hs_string_framework[] = {
  /* idx 1: "Brighton Sikarskie" (18 ASCII bytes). */
  0x09U,
  0x04U,
  0x01U,
  0x12U,
  'B',
  'r',
  'i',
  'g',
  'h',
  't',
  'o',
  'n',
  ' ',
  'S',
  'i',
  'k',
  'a',
  'r',
  's',
  'k',
  'i',
  'e',
  /* idx 2: "RA8D2 HS CDC Echo" (17 ASCII bytes). */
  0x09U,
  0x04U,
  0x02U,
  0x11U,
  'R',
  'A',
  '8',
  'D',
  '2',
  ' ',
  'H',
  'S',
  ' ',
  'C',
  'D',
  'C',
  ' ',
  'E',
  'c',
  'h',
  'o',
  /* idx 3: "RA8D2-CDC-002" (13 ASCII bytes). */
  0x09U,
  0x04U,
  0x03U,
  0x0DU,
  'R',
  'A',
  '8',
  'D',
  '2',
  '-',
  'C',
  'D',
  'C',
  '-',
  '0',
  '0',
  '2',
};

static_assert(sizeof(s_tz_secure_only_usb_hs_string_framework) ==
                k_tz_secure_only_usb_hs_string_framework_len,
              "String framework length must match the header declaration");

/* USBX LANGID descriptor 0x0409 (English-US), little-endian byte pair. */
typedef enum : uint8_t {
  k_usb_langid_en_us_lo = 0x09U, /**< LANGID 0x0409 low byte.  */
  k_usb_langid_en_us_hi = 0x04U, /**< LANGID 0x0409 high byte. */
} usb_langid_byte_t;

/**
 * @var s_tz_secure_only_usb_hs_language_id_framework
 * @brief USBX language-id table -- US English.
 * @since 0.1.0
 */
UCHAR s_tz_secure_only_usb_hs_language_id_framework[] = {k_usb_langid_en_us_lo,
                                                         k_usb_langid_en_us_hi};

static_assert(sizeof(s_tz_secure_only_usb_hs_language_id_framework) ==
                k_tz_secure_only_usb_hs_language_id_framework_len,
              "Language-id framework length must match the header declaration");
#endif /* !RA8_OFF_TARGET */
