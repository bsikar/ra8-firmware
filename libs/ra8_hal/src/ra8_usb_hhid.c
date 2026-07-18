/**
 * @file ra8_usb_hhid.c
 * @brief Native USB host-side HID class layer implementation
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Glues the host-mode bring-up paths in `ra8_usb` to a USB HID
 * peripheral - keyboard, mouse, gamepad - attached on the EK-RA8D2's
 * USB-host port. This file is the native host-HID class layer; FSP's
 * `r_usb_hhid_driver.c` and `r_usb_hhid.c` are reference material
 * only -- nothing is pulled in verbatim.
 *
 * Mapping vs FSP (FSP function -> our entry point):
 *
 *  - `usb_hhid_class_request_init` -> `ra8_usb_hhid_init`
 *  - `usb_hhid_enumeration`        -> `internal_enum_*` step machine
 *                                     driven by `ra8_usb_hhid_step`.
 *  - `usb_hhid_pipe_info`          -> `internal_walk_config_descriptor`
 *  - `usb_hhid_set_report`         -> `ra8_usb_hhid_set_report`
 *  - `usb_hhid_get_report`         -> `ra8_usb_hhid_get_report`
 *  - `usb_hhid_set_idle`           -> `ra8_usb_hhid_set_idle`
 *  - `usb_hhid_set_protocol`       -> `ra8_usb_hhid_set_protocol`
 *  - `R_USB_HHID_DeviceInfoGet`    -> attach-callback `device` payload
 *
 * The starter does CPU-FIFO, single-device, no-hub. Enumeration is
 * driven step-by-step from the controller's CTRT interrupt path
 * (production) or directly via `ra8_usb_hhid_step` (tests). Each step
 * issues exactly one chapter-9 SETUP request via
 * `ra8_usb_host_setup_request`; the next CTRT advances the step.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_usb_hhid.h"

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_log.h"
#include "ra8_usb.h"
#include "ra8_usb_regs.h"

static const char* s_tag = "USBHHID";

/* =============================================================================
 * Internal constants
 * =============================================================================
 */

/**
 * @enum ra8_usb_hhid_step_t
 * @brief Enumeration step machine states.
 *
 * @details Mirrors FSP's host-HID enumeration sequence in
 * `r_usb_hhid_driver.c`. Each step issues exactly one SETUP via
 * `ra8_usb_host_setup_request`; the next CTRT interrupt advances.
 */
typedef enum : uint8_t {
  k_ra8_hhid_step_idle            = 0U, /**< Pre-attach.                   */
  k_ra8_hhid_step_bus_reset       = 1U, /**< Drive USBRST then release.    */
  k_ra8_hhid_step_set_address     = 2U, /**< SET_ADDRESS to assigned 1.    */
  k_ra8_hhid_step_get_dev_desc    = 3U, /**< GET_DEVICE_DESCRIPTOR (18 B). */
  k_ra8_hhid_step_get_cfg_desc    = 4U, /**< GET_CONFIGURATION_DESCRIPTOR. */
  k_ra8_hhid_step_set_config      = 5U, /**< SET_CONFIGURATION (1).        */
  k_ra8_hhid_step_set_interface   = 6U, /**< SET_INTERFACE (0).            */
  k_ra8_hhid_step_walk_desc       = 7U, /**< Find HID IF; populate pipes.  */
  k_ra8_hhid_step_get_report_desc = 8U, /**< GET_DESCRIPTOR (Report).      */
  k_ra8_hhid_step_done            = 9U, /**< Attach callback fires.        */
} ra8_usb_hhid_step_t;

/**
 * @enum ra8_usb_hhid_setup_field_t
 * @brief Standard chapter-9 + HID class request encodings.
 */
typedef enum : uint8_t {
  /* Chapter-9 standard requests (USB 2.0 spec section 9.4). */
  k_ra8_hhid_bm_std_dev_in       = 0x80U, /**< Std | Device | In.     */
  k_ra8_hhid_bm_std_dev_out      = 0x00U, /**< Std | Device | Out.    */
  k_ra8_hhid_bm_std_iface_in     = 0x81U, /**< Std | Interface | In.  */
  k_ra8_hhid_bm_std_iface_out    = 0x01U, /**< Std | Interface | Out. */
  k_ra8_hhid_breq_get_descriptor = 0x06U, /**< GET_DESCRIPTOR.        */
  k_ra8_hhid_breq_set_address    = 0x05U, /**< SET_ADDRESS.           */
  k_ra8_hhid_breq_set_config     = 0x09U, /**< SET_CONFIGURATION.     */
  k_ra8_hhid_breq_set_interface  = 0x0BU, /**< SET_INTERFACE.         */
  /* HID class-specific request envelopes (USB HID 1.11 sec 7.2). */
  k_ra8_hhid_bm_class_iface_in  = 0xA1U, /**< Class | Interface | In.  */
  k_ra8_hhid_bm_class_iface_out = 0x21U, /**< Class | Interface | Out. */
  /* Descriptor types in wValue's high byte (chapter 9). */
  k_ra8_hhid_desc_device        = 0x01U, /**< DEVICE descriptor.        */
  k_ra8_hhid_desc_configuration = 0x02U, /**< CONFIGURATION descriptor. */
  k_ra8_hhid_desc_interface     = 0x04U, /**< INTERFACE descriptor.     */
  k_ra8_hhid_desc_endpoint      = 0x05U, /**< ENDPOINT descriptor.      */
} ra8_usb_hhid_setup_field_t;

/**
 * @enum ra8_usb_hhid_size_t
 * @brief Standard descriptor sizes and request payload sizes.
 */
typedef enum : uint16_t {
  k_ra8_hhid_dev_desc_len     = 18U, /**< USB DEVICE descriptor.        */
  k_ra8_hhid_cfg_desc_len     = 9U,  /**< CONFIGURATION descriptor hdr. */
  k_ra8_hhid_iface_desc_len   = 9U,  /**< INTERFACE descriptor.         */
  k_ra8_hhid_ep_desc_len      = 7U,  /**< ENDPOINT descriptor.          */
  k_ra8_hhid_hid_desc_len     = 9U,  /**< HID class descriptor (min).   */
  k_ra8_hhid_assigned_address = 1U,  /**< First assigned device addr.   */
  k_ra8_hhid_default_config   = 1U,  /**< bConfigurationValue = 1.      */
} ra8_usb_hhid_size_t;

/**
 * @enum ra8_usb_hhid_byte_shift_t
 * @brief Per-byte left-shift constants for wValue layout.
 */
typedef enum : uint8_t {
  k_ra8_hhid_shift_byte0 = 0U, /**< RA8 hhid shift byte0. */
  k_ra8_hhid_shift_byte1 = 8U, /**< RA8 hhid shift byte1. */
} ra8_usb_hhid_byte_shift_t;

/* =============================================================================
 * Internal state
 * =============================================================================
 */

/**
 * @struct ra8_usb_hhid_state_t
 * @brief Singleton shadow state for the host-HID driver.
 */
typedef struct {
  bool                     initialized; /**< True after `ra8_usb_hhid_init`. */
  bool                     attached;    /**< True after enumeration done.    */
  ra8_usb_speed_t          speed;       /**< Underlying controller.          */
  ra8_usb_hhid_step_t      step;        /**< Current enumeration step.       */
  ra8_usb_hhid_attach_fn_t attach_cb;   /**< Attach callback, or NULL.       */
  void*                    attach_ctx;  /**< Attach callback ctx.            */
  ra8_usb_hhid_device_t    device;      /**< Snapshot of attached device.    */
  uint8_t                  hid_desc[k_ra8_hhid_hid_desc_len];       /**< HID
                                            class descriptor cache.         */
  uint8_t                  report_desc[k_ra8_hhid_report_desc_max]; /**< HID
                                            Report descriptor cache.        */
} ra8_usb_hhid_state_t;

static ra8_usb_hhid_state_t s_state = {};

/* =============================================================================
 * Internal helpers
 * =============================================================================
 */

/**
 * @brief Pick the interrupt-max-packet ceiling matching the negotiated
 *        speed.
 *
 * @details See implementation.
 * @param[in] speed See implementation.
 * @return Result code.
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL
static uint16_t internal_intr_max_packet(ra8_usb_speed_t speed)
{
  return (speed == k_ra8_usb_speed_hs) ? k_ra8_hhid_intr_max_packet_hs
                                       : k_ra8_hhid_intr_max_packet_default;
}

/**
 * @brief Configure the host-HID interrupt-IN pipe against the attached
 *        device's endpoint.
 *
 * @details Mirrors FSP's `usb_hhid_pipe_info`. Only PIPE6 is mandatory
 * (interrupt-IN). PIPE7 (interrupt-OUT) is configured only if the
 * attached device advertises one.
 *
 * @return Result code.
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_configure_pipes(void)
{
  ra8_err_t err = ra8_usb_configure_endpoint(s_state.speed,
                                             k_ra8_hhid_pipe_intr_in,
                                             s_state.device.intr_in_ep,
                                             k_ra8_usb_ep_dir_in,
                                             k_ra8_usb_ep_type_intr,
                                             s_state.device.intr_in_max_packet);
  RA8_RETURN_ON_ERROR(err, s_tag, "hhid: intr-in cfg"); /* GCOVR_EXCL_BR_LINE */

  if (s_state.device.intr_out_ep != 0U) {
    err = ra8_usb_configure_endpoint(s_state.speed,
                                     k_ra8_hhid_pipe_intr_out,
                                     s_state.device.intr_out_ep,
                                     k_ra8_usb_ep_dir_out,
                                     k_ra8_usb_ep_type_intr,
                                     s_state.device.intr_out_max_packet);
  }
  return err;
}

/**
 * @brief Stage a chapter-9 GET_DESCRIPTOR SETUP request.
 *
 * @details See implementation.
 * @param[in] desc_type See implementation.
 * @param[in] length See implementation.
 * @return Result code.
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_setup_get_descriptor(uint8_t desc_type, uint16_t length)
{
  const ra8_usb_setup_t setup = {
    .bm_request_type = k_ra8_hhid_bm_std_dev_in,
    .b_request       = k_ra8_hhid_breq_get_descriptor,
    .w_value         = (uint16_t)((uint16_t)desc_type << k_ra8_hhid_shift_byte1),
    .w_index         = 0U,
    .w_length        = length,
  };
  return ra8_usb_host_setup_request(s_state.speed, &setup);
}

/**
 * @brief Stage a SET_ADDRESS SETUP request.
 *
 * @details See implementation.
 * @param[in] address See implementation.
 * @return Result code.
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_setup_set_address(uint8_t address)
{
  const ra8_usb_setup_t setup = {
    .bm_request_type = k_ra8_hhid_bm_std_dev_out,
    .b_request       = k_ra8_hhid_breq_set_address,
    .w_value         = (uint16_t)address,
    .w_index         = 0U,
    .w_length        = 0U,
  };
  return ra8_usb_host_setup_request(s_state.speed, &setup);
}

/**
 * @brief Stage a SET_CONFIGURATION SETUP request.
 *
 * @details See implementation.
 * @param[in] config_value See implementation.
 * @return Result code.
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_setup_set_config(uint8_t config_value)
{
  const ra8_usb_setup_t setup = {
    .bm_request_type = k_ra8_hhid_bm_std_dev_out,
    .b_request       = k_ra8_hhid_breq_set_config,
    .w_value         = (uint16_t)config_value,
    .w_index         = 0U,
    .w_length        = 0U,
  };
  return ra8_usb_host_setup_request(s_state.speed, &setup);
}

/**
 * @brief Stage a SET_INTERFACE (alt 0, iface 0) SETUP request.
 *
 * @details See implementation.
 * @return Result code.
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_setup_set_interface(void)
{
  const ra8_usb_setup_t setup = {
    .bm_request_type = k_ra8_hhid_bm_std_iface_out,
    .b_request       = k_ra8_hhid_breq_set_interface,
    .w_value         = 0U,
    .w_index         = 0U,
    .w_length        = 0U,
  };
  return ra8_usb_host_setup_request(s_state.speed, &setup);
}

/**
 * @brief Stage a GET_DESCRIPTOR (Report descriptor) SETUP request.
 *
 * @details Per USB HID 1.11 sec 7.1.1 the host fetches the HID Report
 * descriptor with bmRequestType = 0x81 (Std | Interface | In),
 * bRequest = 0x06 (GET_DESCRIPTOR), wValue high byte = 0x22 (Report
 * descriptor type).
 *
 * @param[in] length See implementation.
 * @return Result code.
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_setup_get_report_descriptor(uint16_t length)
{
  const ra8_usb_setup_t setup = {
    .bm_request_type = k_ra8_hhid_bm_std_iface_in,
    .b_request       = k_ra8_hhid_breq_get_descriptor,
    .w_value         = (uint16_t)((uint16_t)k_ra8_hhid_desc_report << k_ra8_hhid_shift_byte1),
    .w_index         = (uint16_t)s_state.device.interface_number,
    .w_length        = length,
  };
  return ra8_usb_host_setup_request(s_state.speed, &setup);
}

/**
 * @brief Populate `s_state.device` with stub descriptor data.
 *
 * @details In production this routine walks the configuration
 * descriptor returned in the GET_CONFIG_DESCRIPTOR data stage and
 * picks out the HID interface (class=0x03), its interrupt-IN endpoint,
 * the optional interrupt-OUT endpoint, and the embedded HID class
 * descriptor (descriptor type 0x21). The starter relies on the fact
 * that a single boot-protocol HID device (keyboard or mouse) follows a
 * near-universal layout: interrupt-IN at EP address 1, no interrupt-
 * OUT, single interface 0, subclass 1 (boot), protocol 1 (kb) or 2
 * (mouse). If the attached device deviates, the production path will
 * overwrite these defaults during the descriptor walk.
 *
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_walk_config_descriptor(void)
{
  s_state.device.device_address        = k_ra8_hhid_assigned_address;
  s_state.device.intr_in_ep            = 1U;
  s_state.device.intr_out_ep           = 0U;
  s_state.device.interface_number      = 0U;
  s_state.device.subclass              = k_ra8_hhid_subclass_boot;
  s_state.device.protocol              = k_ra8_hhid_protocol_keyboard;
  s_state.device.intr_in_max_packet    = internal_intr_max_packet(s_state.speed);
  s_state.device.intr_out_max_packet   = 0U;
  s_state.device.report_descriptor_len = 0U;
  s_state.device.hid_descriptor        = s_state.hid_desc;
  s_state.device.report_descriptor     = s_state.report_desc;
  /* VID / PID stay at zero until the production GET_DEVICE_DESCRIPTOR
   * data stage lands; both fields are informational. */
}

/**
 * @brief Step handler -- bus-reset assert.
 *
 * @details See implementation.
 * @return Result code.
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_do_idle(void)
{
  s_state.step = k_ra8_hhid_step_bus_reset;
  return ra8_usb_host_bus_reset(s_state.speed, true);
}

/**
 * @brief Step handler -- bus-reset release + SETUP for SET_ADDRESS.
 *
 * @details See implementation.
 * @return Result code.
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_do_bus_reset(void)
{
  const ra8_err_t rel = ra8_usb_host_bus_reset(s_state.speed, false);
  RA8_RETURN_ON_ERROR(rel, s_tag, "hhid: release bus reset"); /* GCOVR_EXCL_BR_LINE */
  s_state.step = k_ra8_hhid_step_set_address;
  return internal_setup_set_address(k_ra8_hhid_assigned_address);
}

/**
 * @brief Step handler -- store assigned address + SETUP for
 *        GET_DEVICE_DESCRIPTOR.
 *
 * @details See implementation.
 * @return Result code.
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_do_set_address(void)
{
  const ra8_err_t addr_err = ra8_usb_set_address(s_state.speed, k_ra8_hhid_assigned_address);
  RA8_RETURN_ON_ERROR(addr_err, s_tag, "hhid: set USBADDR"); /* GCOVR_EXCL_BR_LINE */
  s_state.step = k_ra8_hhid_step_get_dev_desc;
  return internal_setup_get_descriptor(k_ra8_hhid_desc_device, k_ra8_hhid_dev_desc_len);
}

/**
 * @brief Step handler -- SETUP for GET_CONFIGURATION_DESCRIPTOR.
 *
 * @details See implementation.
 * @return Result code.
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_do_get_dev_desc(void)
{
  s_state.step = k_ra8_hhid_step_get_cfg_desc;
  return internal_setup_get_descriptor(k_ra8_hhid_desc_configuration, k_ra8_hhid_cfg_desc_len);
}

/**
 * @brief Step handler -- SETUP for SET_CONFIGURATION.
 *
 * @details See implementation.
 * @return Result code.
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_do_get_cfg_desc(void)
{
  s_state.step = k_ra8_hhid_step_set_config;
  return internal_setup_set_config(k_ra8_hhid_default_config);
}

/**
 * @brief Step handler -- SETUP for SET_INTERFACE.
 *
 * @details See implementation.
 * @return Result code.
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_do_set_config(void)
{
  s_state.step = k_ra8_hhid_step_set_interface;
  return internal_setup_set_interface();
}

/**
 * @brief Step handler -- pure software descriptor walk.
 *
 * @details See implementation.
 * @return Result code.
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_do_set_interface(void)
{
  internal_walk_config_descriptor();
  s_state.step = k_ra8_hhid_step_walk_desc;
  return k_ra8_ok;
}

/**
 * @brief Step handler -- configure pipes + SETUP GET_DESCRIPTOR(Report).
 *
 * @details See implementation.
 * @return Result code.
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_do_walk_desc(void)
{
  const ra8_err_t pipes_err = internal_configure_pipes();
  RA8_RETURN_ON_ERROR(pipes_err, s_tag, "hhid: configure pipes"); /* GCOVR_EXCL_BR_LINE */
  s_state.step = k_ra8_hhid_step_get_report_desc;
  return internal_setup_get_report_descriptor(k_ra8_hhid_report_desc_max);
}

/**
 * @brief Step handler -- finalise + fire attach callback.
 *
 * @details See implementation.
 * @return Result code.
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_do_get_report_desc(void)
{
  s_state.attached = true;
  s_state.step     = k_ra8_hhid_step_done;
  if (s_state.attach_cb != nullptr) {
    s_state.attach_cb(s_state.attach_ctx, &s_state.device);
  }
  return k_ra8_ok;
}

/**
 * @brief Drive the enumeration step machine forward by one step.
 *
 * @details Invoked from `ra8_usb_hhid_step` (tests) or from the CTRT
 * branch of `ra8_usb_dispatch` (production). Each call advances the
 * step counter by one and dispatches to the appropriate handler.
 *
 * @return Result code.
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_step_advance(void)
{
  switch (s_state.step) {
    case k_ra8_hhid_step_idle:
      return internal_do_idle();
    case k_ra8_hhid_step_bus_reset:
      return internal_do_bus_reset();
    case k_ra8_hhid_step_set_address:
      return internal_do_set_address();
    case k_ra8_hhid_step_get_dev_desc:
      return internal_do_get_dev_desc();
    case k_ra8_hhid_step_get_cfg_desc:
      return internal_do_get_cfg_desc();
    case k_ra8_hhid_step_set_config:
      return internal_do_set_config();
    case k_ra8_hhid_step_set_interface:
      return internal_do_set_interface();
    case k_ra8_hhid_step_walk_desc:
      return internal_do_walk_desc();
    case k_ra8_hhid_step_get_report_desc:
      return internal_do_get_report_desc();
    default:
      /* Already done; idempotent. */
      return k_ra8_ok;
  }
}

/**
 * @brief Validate that a `ra8_usb_hhid_report_type_t` value is in range.
 *
 * @details See implementation.
 * @param[in] t See implementation.
 * @return Result code.
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL
static bool internal_report_type_ok(ra8_usb_hhid_report_type_t t)
{
  return (t == k_ra8_hhid_report_type_input) || (t == k_ra8_hhid_report_type_output) ||
         (t == k_ra8_hhid_report_type_feature);
}

/* =============================================================================
 * Lifecycle
 * =============================================================================
 */

ra8_err_t ra8_usb_hhid_init(ra8_usb_speed_t speed)
{
  if ((speed != k_ra8_usb_speed_fs) && (speed != k_ra8_usb_speed_hs)) {
    return k_ra8_err_invalid_arg;
  }
  const ra8_err_t usb_err = ra8_usb_host_init(speed);
  if (usb_err != k_ra8_ok) {
    ra8_log_error_val(s_tag, "ra8_usb_host_init failed", (uint32_t)usb_err);
    return k_ra8_err_hw_init_failed;
  }

  s_state.speed       = speed;
  s_state.step        = k_ra8_hhid_step_idle;
  s_state.attached    = false;
  s_state.attach_cb   = nullptr;
  s_state.attach_ctx  = nullptr;
  s_state.device      = (ra8_usb_hhid_device_t){};
  s_state.initialized = true;

  ra8_log_info_val(s_tag, "host-HID ready", (uint32_t)speed);
  return k_ra8_ok;
}

ra8_err_t ra8_usb_hhid_close(void)
{
  if (!s_state.initialized) {
    return k_ra8_err_invalid_state;
  }
  /* Bus power down: drop UACT before tearing the controller. */
  (void)ra8_usb_host_set_uact(s_state.speed, false);
  const ra8_err_t err = ra8_usb_host_deinit(s_state.speed);
  s_state.initialized = false;
  s_state.attached    = false;
  s_state.attach_cb   = nullptr;
  s_state.attach_ctx  = nullptr;
  s_state.step        = k_ra8_hhid_step_idle;
  return err;
}

/* =============================================================================
 * Attach callback
 * =============================================================================
 */

ra8_err_t ra8_usb_hhid_attach_callback(ra8_usb_hhid_attach_fn_t on_attach, void* ctx)
{
  if (!s_state.initialized) {
    return k_ra8_err_invalid_state;
  }
  s_state.attach_cb  = on_attach;
  s_state.attach_ctx = ctx;
  return k_ra8_ok;
}

/* =============================================================================
 * Class control transfers
 * =============================================================================
 */

/**
 * @enum ra8_usb_hhid_dcp_t
 * @brief CFIFO programming constants used by the EP0 IN drain helper.
 */
typedef enum : uint16_t {
  k_ra8_hhid_dcp_pipe_dcp  = 0U,    /**< DCP / EP0 select. */
  k_ra8_hhid_fifo_poll_lim = 4096U, /**< FRDY-poll budget. */
} ra8_usb_hhid_dcp_t;

/**
 * @brief Pick the controller register window for the active speed.
 */
RA8_INTERNAL
static volatile r_usb_regs_t* internal_pick_regs(ra8_usb_speed_t speed)
{
  if (speed == k_ra8_usb_speed_hs) {
    return ra8_usb_hs();
  }
  if (speed == k_ra8_usb_speed_fs) {
    return ra8_usb_fs();
  }
  return nullptr;
}

/**
 * @brief Drain the DCP (EP0) IN FIFO after a class GET_REPORT SETUP.
 *
 * @details
 * Mirrors the FIFO-read path inside ``ra8_usb_queue_out`` but targets
 * pipe 0 (the DCP), which the public API rejects.
 *
 * Flow:
 * 1. Select CFIFO -> DCP, IN direction (CFIFOSEL.ISEL=1, MBW=16).
 * 2. Wait for CFIFOCTR.FRDY (bounded poll).
 * 3. Read CFIFOCTR.DTLN to get the available byte count.
 * 4. 16-bit LE drain into @p out, capped at @p max_len.
 * 5. Write CFIFOCTR.BCLR to release the buffer.
 *
 * See HUM Ch 36.2.5 "CFIFO" p 1973 and Ch 36.2.8 "CFIFOCTR" p 1979.
 *
 * @param[in] reg See implementation.
 * @param[in] out See implementation.
 * @param[in] max_len See implementation.
 * @return Result code.
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL
static uint16_t internal_dcp_in_drain(volatile r_usb_regs_t* reg, uint8_t* out, uint16_t max_len)
{
  if (max_len == 0U) {
    return 0U;
  }
  /* HUM Ch 36.2.7 "CFIFOSEL" p 1976 */ /* pipe=0 (DCP), MBW=16, ISEL=1. */
  uint16_t sel  = (uint16_t)k_ra8_hhid_dcp_pipe_dcp & (uint16_t)k_ra8_fifosel_curpipe;
  sel           = (uint16_t)(sel | k_ra8_fifosel_mbw_16);
  sel           = (uint16_t)(sel | k_ra8_fifosel_isel);
  reg->CFIFOSEL = sel;

  /* HUM Ch 36.2.8 "CFIFOCTR" p 1979 */ /* bounded FRDY spin. */
  uint16_t ready = 0U;
  for (uint16_t i = 0U; i < k_ra8_hhid_fifo_poll_lim; ++i) { /* GCOVR_EXCL_BR_LINE */
    if ((reg->CFIFOCTR & k_ra8_fifoctr_frdy) != 0U) {        /* GCOVR_EXCL_BR_LINE */
      ready = 1U;
      break;
    }
  }
  if (ready == 0U) {
    return 0U;
  }

  const uint16_t available = (uint16_t)(reg->CFIFOCTR & k_ra8_fifoctr_dtln);
  uint16_t       take      = (available < max_len) ? available : max_len;

  /* HUM Ch 36.2.5 "CFIFO" p 1973 */ /* 16-bit LE drain. */
  enum : uint8_t {
    k_byte_bits = 8U,    /**< Byte bits. */
    k_byte_mask = 0xFFU, /**< Byte mask. */
  };
  const uint16_t even = (uint16_t)(take >> 1U);
  for (uint16_t i = 0U; i < even; ++i) {
    const uint16_t word = reg->CFIFO;
    out[(2U * i) + 0U]  = (uint8_t)(word & k_byte_mask);
    out[(2U * i) + 1U]  = (uint8_t)((word >> k_byte_bits) & k_byte_mask);
  }
  if ((take & 1U) != 0U) {
    const uint16_t word = reg->CFIFO;
    out[take - 1U]      = (uint8_t)(word & k_byte_mask);
  }
  /* Release the buffer so the controller can ACK the IN data phase. */
  reg->CFIFOCTR = k_ra8_fifoctr_bclr;
  return take;
}

/* USB HID 1.11 sec 7.2.1 "Get_Report request":
 *   bmRequestType = 0xA1 (D2H | Class | Interface)
 *   bRequest      = 0x01 (GET_REPORT)
 *   wValue.high   = report type (1=Input / 2=Output / 3=Feature)
 *   wValue.low    = report ID (0 if device uses a single unnamed report)
 */
ra8_err_t ra8_usb_hhid_get_report(ra8_usb_hhid_report_type_t target_report_type,
                                  uint8_t                    target_report_id,
                                  uint8_t*                   out_buf,
                                  uint16_t                   max_len,
                                  uint16_t*                  got_len)
{
  RA8_CHECK_NULL_PTR(out_buf, s_tag, "get_report: out_buf");
  RA8_CHECK_NULL_PTR(got_len, s_tag, "get_report: got_len");
  if (!s_state.initialized) {
    return k_ra8_err_invalid_state;
  }
  if (!s_state.attached) {
    return k_ra8_err_invalid_state;
  }
  if (!internal_report_type_ok(target_report_type)) {
    return k_ra8_err_invalid_arg;
  }
  if (max_len == 0U) {
    return k_ra8_err_invalid_arg;
  }

  const uint16_t value = (uint16_t)(((uint16_t)target_report_type << k_ra8_hhid_shift_byte1) |
                                    (uint16_t)target_report_id);
  const ra8_usb_setup_t setup = {
    .bm_request_type = k_ra8_hhid_bm_class_iface_in,
    .b_request       = k_ra8_hhid_req_get_report,
    .w_value         = value,
    .w_index         = (uint16_t)s_state.device.interface_number,
    .w_length        = max_len,
  };
  *got_len                  = 0U;
  const ra8_err_t setup_err = ra8_usb_host_setup_request(s_state.speed, &setup);
  if (setup_err != k_ra8_ok) {
    return setup_err;
  }

  /* Wire the IN data phase: after the SETUP request lands the
   * controller drives an IN token on EP0 and the device's response
   * ends up in the DCP CFIFO.  Drain it into the caller's buffer. */
  volatile r_usb_regs_t* reg = internal_pick_regs(s_state.speed);
  if (reg != nullptr) {
    *got_len = internal_dcp_in_drain(reg, out_buf, max_len);
  }
  return k_ra8_ok;
}

/* USB HID 1.11 sec 7.2.2 "Set_Report request":
 *   bmRequestType = 0x21 (H2D | Class | Interface)
 *   bRequest      = 0x09 (SET_REPORT)
 */
ra8_err_t ra8_usb_hhid_set_report(ra8_usb_hhid_report_type_t target_report_type,
                                  uint8_t                    target_report_id,
                                  const uint8_t*             in_buf,
                                  uint16_t                   len)
{
  if (!s_state.initialized) {
    return k_ra8_err_invalid_state;
  }
  if (!s_state.attached) {
    return k_ra8_err_invalid_state;
  }
  if ((in_buf == nullptr) && (len != 0U)) {
    return k_ra8_err_null_ptr;
  }
  if (!internal_report_type_ok(target_report_type)) {
    return k_ra8_err_invalid_arg;
  }

  const uint16_t value = (uint16_t)(((uint16_t)target_report_type << k_ra8_hhid_shift_byte1) |
                                    (uint16_t)target_report_id);
  const ra8_usb_setup_t setup = {
    .bm_request_type = k_ra8_hhid_bm_class_iface_out,
    .b_request       = k_ra8_hhid_req_set_report,
    .w_value         = value,
    .w_index         = (uint16_t)s_state.device.interface_number,
    .w_length        = len,
  };
  /* Production path queues `in_buf[0..len-1]` on the DCP data stage
   * once the controller advances past the SETUP token. The starter
   * just ensures the SETUP envelope hits the wire. */
  (void)in_buf;
  return ra8_usb_host_setup_request(s_state.speed, &setup);
}

/* USB HID 1.11 sec 7.2.4 "Set_Idle request":
 *   bmRequestType = 0x21
 *   bRequest      = 0x0A (SET_IDLE)
 *   wValue.high   = duration (in 4 ms units)
 *   wValue.low    = report ID (0 = "all reports")
 */
ra8_err_t ra8_usb_hhid_set_idle(uint8_t duration, uint8_t report_id)
{
  if (!s_state.initialized) {
    return k_ra8_err_invalid_state;
  }
  if (!s_state.attached) {
    return k_ra8_err_invalid_state;
  }
  const uint16_t value =
    (uint16_t)(((uint16_t)duration << k_ra8_hhid_shift_byte1) | (uint16_t)report_id);
  const ra8_usb_setup_t setup = {
    .bm_request_type = k_ra8_hhid_bm_class_iface_out,
    .b_request       = k_ra8_hhid_req_set_idle,
    .w_value         = value,
    .w_index         = (uint16_t)s_state.device.interface_number,
    .w_length        = 0U,
  };
  return ra8_usb_host_setup_request(s_state.speed, &setup);
}

/* USB HID 1.11 sec 7.2.6 "Set_Protocol request":
 *   bmRequestType = 0x21
 *   bRequest      = 0x0B (SET_PROTOCOL)
 *   wValue        = 0 (boot protocol) or 1 (report protocol)
 */
ra8_err_t ra8_usb_hhid_set_protocol(ra8_usb_hhid_protocol_select_t boot_or_report)
{
  if (!s_state.initialized) {
    return k_ra8_err_invalid_state;
  }
  if (!s_state.attached) {
    return k_ra8_err_invalid_state;
  }
  if ((boot_or_report != k_ra8_hhid_proto_boot) && (boot_or_report != k_ra8_hhid_proto_report)) {
    return k_ra8_err_invalid_arg;
  }
  const ra8_usb_setup_t setup = {
    .bm_request_type = k_ra8_hhid_bm_class_iface_out,
    .b_request       = k_ra8_hhid_req_set_protocol,
    .w_value         = (uint16_t)boot_or_report,
    .w_index         = (uint16_t)s_state.device.interface_number,
    .w_length        = 0U,
  };
  return ra8_usb_host_setup_request(s_state.speed, &setup);
}

/* =============================================================================
 * Interrupt-IN polling
 * =============================================================================
 */

ra8_err_t ra8_usb_hhid_get_input_report(uint8_t* out_buf, uint16_t max_len, uint16_t* got_len)
{
  RA8_CHECK_NULL_PTR(out_buf, s_tag, "get_input_report: out_buf");
  RA8_CHECK_NULL_PTR(got_len, s_tag, "get_input_report: got_len");
  if (!s_state.initialized) {
    return k_ra8_err_invalid_state;
  }
  if (!s_state.attached) {
    return k_ra8_err_invalid_state;
  }
  if (max_len == 0U) {
    return k_ra8_err_invalid_arg;
  }
  uint16_t        inout_len = max_len;
  const ra8_err_t err =
    ra8_usb_queue_out(s_state.speed, k_ra8_hhid_pipe_intr_in, out_buf, &inout_len, true);
  if (err == k_ra8_ok) {
    *got_len = inout_len;
  } else {
    *got_len = 0U;
  }
  return err;
}

/* =============================================================================
 * Test / introspection helpers
 * =============================================================================
 */

ra8_err_t ra8_usb_hhid_step(void)
{
  if (!s_state.initialized) {
    return k_ra8_err_invalid_state;
  }
  return internal_step_advance();
}
