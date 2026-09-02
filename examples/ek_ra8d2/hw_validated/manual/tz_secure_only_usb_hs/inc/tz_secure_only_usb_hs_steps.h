/**
 * @file
 * examples/ek_ra8d2/hw_validated/manual/tz_secure_only_usb_hs/inc/tz_secure_only_usb_hs_steps.h
 * @brief ThreadX + USBX worker machinery for the secure-only USB-HS echo app.
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Companion sibling header for
 * ``examples/ek_ra8d2/hw_validated/manual/tz_secure_only_usb_hs/src/main.c``.
 * The CDC-ACM activate/deactivate callbacks, the USBX bring-up step
 * routines, the INTENB0 re-arm watchdog and the ThreadX
 * ``tx_application_define`` kernel hook were factored out of ``main.c``
 * verbatim into ``tz_secure_only_usb_hs_steps.c``, and the four USBX
 * descriptor byte arrays into ``tz_secure_only_usb_hs_descriptors.c``, to
 * keep every translation unit under the 1000-line ``check_file_size.py``
 * cap. ``main.c`` retains ``main()``, ``demo_pins_init`` and
 * ``demo_panic_halt``; it reaches the worker side only through ThreadX's
 * ``tx_kernel_enter`` -> ``tx_application_define`` linkage. ThreadX's
 * ``tx_api.h`` declares that kernel hook, so this header exposes only the
 * descriptor arrays shared between the two sibling implementation units.
 *
 * This split is a pure, behaviour-preserving code move: no logic was
 * altered.
 *
 * @author Brighton Sikarskie
 * @date 2026-05-03
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#include <stdint.h>

#ifndef RA8_OFF_TARGET
#include "tx_api.h"
#include "ux_api.h"

/**
 * @enum tz_secure_only_usb_hs_descriptor_len_t
 * @brief Byte lengths of the four USBX descriptor frameworks.
 *
 * @details
 * The descriptor byte arrays are defined in the sibling translation unit
 * ``tz_secure_only_usb_hs_descriptors.c`` and consumed by
 * ``demo_worker_usbx_init`` in ``tz_secure_only_usb_hs_steps.c``. Because
 * an ``extern UCHAR foo[]`` declaration alone is incomplete in the
 * consuming TU (``sizeof`` would not compile), each array is declared with
 * its explicit dimension below; these enum members name those dimensions
 * so the declarations stay magic-number-free. The defining TU
 * ``static_assert``s each value against the real initializer length, so
 * any drift is caught at compile time.
 *
 * @invariant Each value equals ``sizeof`` of the matching array.
 * @since 0.1.0
 */
typedef enum : uint16_t {
  k_tz_secure_only_usb_hs_device_framework_fs_len   = 93U,  /**< FS framework bytes. */
  k_tz_secure_only_usb_hs_device_framework_hs_len   = 103U, /**< HS framework bytes. */
  k_tz_secure_only_usb_hs_string_framework_len      = 60U,  /**< String table bytes. */
  k_tz_secure_only_usb_hs_language_id_framework_len = 2U,   /**< LANGID table bytes. */
} tz_secure_only_usb_hs_descriptor_len_t;

/**
 * @var s_tz_secure_only_usb_hs_device_framework_fs
 * @brief Full-Speed CDC-ACM composite device framework (64-byte bulk MPS).
 * @details Defined in ``tz_secure_only_usb_hs_descriptors.c``.
 * @since 0.1.0
 */
extern UCHAR
  s_tz_secure_only_usb_hs_device_framework_fs[k_tz_secure_only_usb_hs_device_framework_fs_len];

/**
 * @var s_tz_secure_only_usb_hs_device_framework_hs
 * @brief High-Speed CDC-ACM composite device framework (512-byte bulk MPS).
 * @details Defined in ``tz_secure_only_usb_hs_descriptors.c``.
 * @since 0.1.0
 */
extern UCHAR
  s_tz_secure_only_usb_hs_device_framework_hs[k_tz_secure_only_usb_hs_device_framework_hs_len];

/**
 * @var s_tz_secure_only_usb_hs_string_framework
 * @brief USBX string descriptor table (vendor / product / serial).
 * @details Defined in ``tz_secure_only_usb_hs_descriptors.c``.
 * @since 0.1.0
 */
extern UCHAR s_tz_secure_only_usb_hs_string_framework[k_tz_secure_only_usb_hs_string_framework_len];

/**
 * @var s_tz_secure_only_usb_hs_language_id_framework
 * @brief USBX language-id table -- US English (LANGID 0x0409).
 * @details Defined in ``tz_secure_only_usb_hs_descriptors.c``.
 * @since 0.1.0
 */
extern UCHAR
  s_tz_secure_only_usb_hs_language_id_framework[k_tz_secure_only_usb_hs_language_id_framework_len];

#endif /* !RA8_OFF_TARGET */
