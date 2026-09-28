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
 * verbatim into ``tz_secure_only_usb_hs_steps.c``, and the USB identity
 * plus the four framework buffers into
 * ``tz_secure_only_usb_hs_descriptors.c``, to keep every translation unit
 * under the 1000-line ``check_file_size.py`` cap. ``main.c`` retains ``main()``, ``demo_pins_init`` and
 * ``demo_panic_halt``; it reaches the worker side only through ThreadX's
 * ``tx_kernel_enter`` -> ``tx_application_define`` linkage. ThreadX's
 * ``tx_api.h`` declares that kernel hook, so this header exposes only the
 * framework buffers shared between the two sibling implementation units
 * and the one call that fills them.
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
#include "ra8_usb_desc.h"
#include "tx_api.h"
#include "ux_api.h"

/**
 * @brief Synthesise the app's four USBX frameworks into the buffers below.
 *
 * @details
 * Encodes one device identity into a full-speed and a high-speed CDC-ACM
 * device framework, a string table and a language-id table, through
 * ``ra8_usb_desc_build_cdc_acm``, ``ra8_usb_desc_build_strings`` and
 * ``ra8_usb_desc_build_langid``. Defined in the sibling unit
 * ``tz_secure_only_usb_hs_descriptors.c``, which owns the identity and the
 * two endpoint layouts. Nothing here touches a controller: a synthesised
 * framework is bytes, not an attached device.
 *
 * @return ra8_err_t Result of the four encodes.
 * @retval k_ra8_ok               All four frameworks written.
 * @retval k_ra8_err_invalid_size A destination buffer is too small.
 * @retval k_ra8_err_invalid_arg  An endpoint address or packet size is wrong.
 *
 * @pre Called from thread context before ``_ux_device_stack_initialize``.
 * @post On success each buffer holds its framework and the matching length
 *       variable counts it; on failure the lengths of the encodes that did
 *       not run stay 0.
 *
 * @note Single-call; the builders are pure, so a repeat call is harmless.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t tz_secure_only_usb_hs_build_frameworks(void);

/**
 * @var s_tz_secure_only_usb_hs_device_framework_fs
 * @brief Full-Speed CDC-ACM composite device framework (64-byte bulk MPS).
 * @details Written by ::tz_secure_only_usb_hs_build_frameworks, then
 *          read-only. Sized by the library maximum, so
 *          ::s_tz_secure_only_usb_hs_device_framework_fs_len, not
 *          ``sizeof``, is the byte count to hand USBX.
 * @since 0.1.0
 */
extern UCHAR s_tz_secure_only_usb_hs_device_framework_fs[k_ra8_usb_desc_framework_bytes_max];

/**
 * @var s_tz_secure_only_usb_hs_device_framework_hs
 * @brief High-Speed CDC-ACM composite device framework (512-byte bulk MPS).
 * @details Written by ::tz_secure_only_usb_hs_build_frameworks, then
 *          read-only. Ten bytes longer than the full-speed framework
 *          because high speed also publishes the device qualifier.
 * @since 0.1.0
 */
extern UCHAR s_tz_secure_only_usb_hs_device_framework_hs[k_ra8_usb_desc_framework_bytes_max];

/**
 * @var s_tz_secure_only_usb_hs_string_framework
 * @brief USBX string descriptor table (vendor / product / serial).
 * @details Written by ::tz_secure_only_usb_hs_build_frameworks.
 * @since 0.1.0
 */
extern UCHAR s_tz_secure_only_usb_hs_string_framework[k_ra8_usb_desc_strings_bytes_max];

/**
 * @var s_tz_secure_only_usb_hs_language_id_framework
 * @brief USBX language-id table -- US English (LANGID 0x0409).
 * @details Written by ::tz_secure_only_usb_hs_build_frameworks.
 * @since 0.1.0
 */
extern UCHAR s_tz_secure_only_usb_hs_language_id_framework[k_ra8_usb_desc_langid_bytes];

/**
 * @var s_tz_secure_only_usb_hs_device_framework_fs_len
 * @brief Bytes written to ::s_tz_secure_only_usb_hs_device_framework_fs.
 * @since 0.1.0
 */
extern uint32_t s_tz_secure_only_usb_hs_device_framework_fs_len;

/**
 * @var s_tz_secure_only_usb_hs_device_framework_hs_len
 * @brief Bytes written to ::s_tz_secure_only_usb_hs_device_framework_hs.
 * @since 0.1.0
 */
extern uint32_t s_tz_secure_only_usb_hs_device_framework_hs_len;

/**
 * @var s_tz_secure_only_usb_hs_string_framework_len
 * @brief Bytes written to ::s_tz_secure_only_usb_hs_string_framework.
 * @since 0.1.0
 */
extern uint32_t s_tz_secure_only_usb_hs_string_framework_len;

/**
 * @var s_tz_secure_only_usb_hs_language_id_framework_len
 * @brief Bytes written to ::s_tz_secure_only_usb_hs_language_id_framework.
 * @since 0.1.0
 */
extern uint32_t s_tz_secure_only_usb_hs_language_id_framework_len;

#endif /* !RA8_OFF_TARGET */
