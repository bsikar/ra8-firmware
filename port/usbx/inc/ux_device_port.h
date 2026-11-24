/**
 * @file port/usbx/inc/ux_device_port.h
 * @brief USBX stack-ops seam binding for ``ra8_usb_device_open`` (RA8FW-317).
 *
 * @par Tag
 * [Ring 5 / PORT] {World: S/NS}
 *
 * @details
 * Implements the middleware side of the ``libs/ra8_usb_pal`` stack-agnostic
 * ``ra8_usb_stack_ops_t`` contract so examples invoke ``ra8_usb_device_open``
 * instead of calling ``_ux_*`` entry points directly.
 *
 * Class registration descriptors and runtime class helpers below are gated on
 * the USBX class headers each translation unit already includes, keeping
 * single-class builds (such as ``tz_nsc_cgc_usb`` CDC-ACM) free of unused
 * MSC/HID/DFU symbol references.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_usb_compose.h"
#include "ux_api.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Return the USBX stack ops table without automatic DCD initialisation.
 *
 * @details
 * Executes ``_ux_system_initialize``, optional error-callback registration,
 * ``_ux_device_stack_initialize``, and ``_ux_device_stack_class_register`` inside
 * ``ra8_usb_device_open``, leaving controller bring-up (``ux_dcd_ra8_usb_initialize``
 * and attach) to the caller.
 *
 * @return const ra8_usb_stack_ops_t* Static stack operations descriptor.
 * @since 0.1.0
 */
[[nodiscard]] const ra8_usb_stack_ops_t* ux_device_port_stack_ops(void);

/**
 * @brief Return the USBX stack ops table including ``ux_dcd_ra8_usb_initialize``.
 *
 * @return const ra8_usb_stack_ops_t* Static stack operations descriptor.
 * @since 0.1.0
 */
[[nodiscard]] const ra8_usb_stack_ops_t* ux_device_port_stack_ops_with_dcd(void);

#ifdef UX_DEVICE_CLASS_CDC_ACM_H
/**
 * @brief Adapt ``_ux_device_class_cdc_acm_entry`` to ``ra8_usb_device_class_entry_fn_t``.
 * @param[in,out] command Opaque USBX class command pointer.
 * @return uint32_t USBX completion status.
 */
static inline uint32_t ux_device_port_cdc_acm_entry(void* command)
{
  return (uint32_t)_ux_device_class_cdc_acm_entry((UX_SLAVE_CLASS_COMMAND*)command);
}

/**
 * @brief Return the static CDC-ACM class driver descriptor for ``ra8_usb_device_open``.
 * @return const ra8_usb_device_class_driver_t* Immutable driver descriptor.
 */
[[nodiscard]] static inline const ra8_usb_device_class_driver_t* ux_device_port_class_cdc_acm(void)
{
  static const ra8_usb_device_class_driver_t s_driver = {
    .name  = "ux_slave_class_cdc_acm",
    .entry = ux_device_port_cdc_acm_entry,
  };
  return &s_driver;
}

/**
 * @brief Blocking bulk-OUT read from an activated USBX CDC-ACM instance.
 *
 * @param[in]  cdc_acm       Active CDC-ACM class instance.
 * @param[out] buffer        Caller receive buffer.
 * @param[in]  requested_len Maximum byte count to read.
 * @param[out] actual_len    Actual bytes received.
 * @return ra8_err_t ``k_ra8_ok`` on USBX success, error code otherwise.
 */
[[nodiscard]] static inline ra8_err_t ux_device_port_cdc_read(UX_SLAVE_CLASS_CDC_ACM* cdc_acm,
                                                              uint8_t*                buffer,
                                                              uint32_t                requested_len,
                                                              uint32_t*               actual_len)
{
  if ((cdc_acm == UX_NULL) || (buffer == UX_NULL) || (actual_len == nullptr)) {
    return k_ra8_err_invalid_arg;
  }
  ULONG actual = 0UL;
  UINT  status =
    _ux_device_class_cdc_acm_read(cdc_acm, (UCHAR*)buffer, (ULONG)requested_len, &actual);
  *actual_len = (uint32_t)actual;
  return (status == UX_SUCCESS) ? k_ra8_ok : k_ra8_err_invalid_state;
}

/**
 * @brief Blocking bulk-IN write to an activated USBX CDC-ACM instance.
 *
 * @param[in]  cdc_acm       Active CDC-ACM class instance.
 * @param[in]  buffer        Payload buffer to transmit.
 * @param[in]  requested_len Number of bytes to transmit.
 * @param[out] actual_len    Actual bytes transmitted.
 * @return ra8_err_t ``k_ra8_ok`` on USBX success, error code otherwise.
 */
[[nodiscard]] static inline ra8_err_t ux_device_port_cdc_write(UX_SLAVE_CLASS_CDC_ACM* cdc_acm,
                                                               uint8_t*                buffer,
                                                               uint32_t  requested_len,
                                                               uint32_t* actual_len)
{
  if ((cdc_acm == UX_NULL) || (buffer == UX_NULL) || (actual_len == nullptr)) {
    return k_ra8_err_invalid_arg;
  }
  ULONG actual = 0UL;
  UINT  status =
    _ux_device_class_cdc_acm_write(cdc_acm, (UCHAR*)buffer, (ULONG)requested_len, &actual);
  *actual_len = (uint32_t)actual;
  return (status == UX_SUCCESS) ? k_ra8_ok : k_ra8_err_invalid_state;
}
#endif /* UX_DEVICE_CLASS_CDC_ACM_H */

#ifdef UX_DEVICE_CLASS_STORAGE_H
/**
 * @brief Adapt ``_ux_device_class_storage_entry`` to ``ra8_usb_device_class_entry_fn_t``.
 * @param[in,out] command Opaque USBX class command pointer.
 * @return uint32_t USBX completion status.
 */
static inline uint32_t ux_device_port_storage_entry(void* command)
{
  return (uint32_t)_ux_device_class_storage_entry((UX_SLAVE_CLASS_COMMAND*)command);
}

/**
 * @brief Return the static Mass-Storage class driver descriptor for ``ra8_usb_device_open``.
 * @return const ra8_usb_device_class_driver_t* Immutable driver descriptor.
 */
[[nodiscard]] static inline const ra8_usb_device_class_driver_t* ux_device_port_class_msc(void)
{
  static const ra8_usb_device_class_driver_t s_driver = {
    .name  = "ux_slave_class_storage",
    .entry = ux_device_port_storage_entry,
  };
  return &s_driver;
}
#endif /* UX_DEVICE_CLASS_STORAGE_H */

#ifdef UX_DEVICE_CLASS_HID_H
/**
 * @brief Adapt ``_ux_device_class_hid_entry`` to ``ra8_usb_device_class_entry_fn_t``.
 * @param[in,out] command Opaque USBX class command pointer.
 * @return uint32_t USBX completion status.
 */
static inline uint32_t ux_device_port_hid_entry(void* command)
{
  return (uint32_t)_ux_device_class_hid_entry((UX_SLAVE_CLASS_COMMAND*)command);
}

/**
 * @brief Return the static HID class driver descriptor for ``ra8_usb_device_open``.
 * @return const ra8_usb_device_class_driver_t* Immutable driver descriptor.
 */
[[nodiscard]] static inline const ra8_usb_device_class_driver_t* ux_device_port_class_hid(void)
{
  static const ra8_usb_device_class_driver_t s_driver = {
    .name  = "ux_slave_class_hid",
    .entry = ux_device_port_hid_entry,
  };
  return &s_driver;
}

/**
 * @brief Queue one HID input report event on an activated USBX HID class instance.
 *
 * @param[in] hid       Active HID class instance.
 * @param[in] hid_event Filled HID report event.
 * @return ra8_err_t ``k_ra8_ok`` on USBX success, error code otherwise.
 */
[[nodiscard]] static inline ra8_err_t
ux_device_port_hid_event_set(UX_SLAVE_CLASS_HID* hid, UX_SLAVE_CLASS_HID_EVENT* hid_event)
{
  if ((hid == UX_NULL) || (hid_event == UX_NULL)) {
    return k_ra8_err_invalid_arg;
  }
  return (_ux_device_class_hid_event_set(hid, hid_event) == UX_SUCCESS) ? k_ra8_ok
                                                                        : k_ra8_err_invalid_state;
}
#endif /* UX_DEVICE_CLASS_HID_H */

#ifdef UX_DEVICE_CLASS_DFU_H
/**
 * @brief Adapt ``_ux_device_class_dfu_entry`` to ``ra8_usb_device_class_entry_fn_t``.
 * @param[in,out] command Opaque USBX class command pointer.
 * @return uint32_t USBX completion status.
 */
static inline uint32_t ux_device_port_dfu_entry(void* command)
{
  return (uint32_t)_ux_device_class_dfu_entry((UX_SLAVE_CLASS_COMMAND*)command);
}

/**
 * @brief Return the static DFU class driver descriptor for ``ra8_usb_device_open``.
 * @return const ra8_usb_device_class_driver_t* Immutable driver descriptor.
 */
[[nodiscard]] static inline const ra8_usb_device_class_driver_t* ux_device_port_class_dfu(void)
{
  static const ra8_usb_device_class_driver_t s_driver = {
    .name  = "ux_slave_class_dfu",
    .entry = ux_device_port_dfu_entry,
  };
  return &s_driver;
}
#endif /* UX_DEVICE_CLASS_DFU_H */

#ifdef __cplusplus
}
#endif
