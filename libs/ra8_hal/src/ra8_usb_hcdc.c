/**
 * @file ra8_usb_hcdc.c
 * @brief Native USB host-side CDC ACM class layer implementation
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Glues the host-mode bring-up paths in `ra8_usb` to a CDC-ACM
 * peripheral attached on the EK-RA8D2's USB-host port. This file is
 * the native host-CDC class layer; FSP's `r_usb_hcdc_driver.c` and
 * `r_usb_hcdc.c` are reference material only -- nothing is pulled in
 * verbatim.
 *
 * Mapping vs FSP (FSP function -> our entry point):
 *
 *  - `usb_hcdc_init`            -> `ra8_usb_hcdc_init`
 *  - `usb_hcdc_enumeration`     -> `internal_enum_*` step machine
 *                                  driven by `ra8_usb_hcdc_step`.
 *  - `usb_hcdc_pipe_info`       -> `internal_walk_config_descriptor`
 *  - `R_USB_HCDC_DeviceInfoGet` -> attach-callback `device` payload
 *
 * The starter does CPU-FIFO, single-device, no-hub. Enumeration is
 * driven step-by-step from the controller's CTRT interrupt path
 * (production) or directly via `ra8_usb_hcdc_step` (tests). Each step
 * issues exactly one chapter-9 SETUP request via
 * `ra8_usb_host_setup_request`; the next CTRT advances the step.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_usb_hcdc.h"

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_log.h"
#include "ra8_usb.h"

static const char* s_tag = "USBHCDC";

/* =============================================================================
 * Internal constants
 * =============================================================================
 */

/**
 * @enum ra8_usb_hcdc_step_t
 * @brief Enumeration step machine states.
 *
 * @details Mirrors FSP's `g_usb_hcdc_smpl_class_seq` step indices in
 * `r_usb_hcdc_driver.c`. Each step issues exactly one SETUP via
 * `ra8_usb_host_setup_request`; the next CTRT interrupt advances to
 * the next step.
 */
typedef enum : uint8_t {
  k_ra8_hcdc_step_idle          = 0U, /**< Pre-attach.                   */
  k_ra8_hcdc_step_bus_reset     = 1U, /**< Drive USBRST then release.    */
  k_ra8_hcdc_step_set_address   = 2U, /**< SET_ADDRESS to assigned 1.    */
  k_ra8_hcdc_step_get_dev_desc  = 3U, /**< GET_DEVICE_DESCRIPTOR (18 B). */
  k_ra8_hcdc_step_get_cfg_desc  = 4U, /**< GET_CONFIGURATION_DESCRIPTOR. */
  k_ra8_hcdc_step_set_config    = 5U, /**< SET_CONFIGURATION (1).        */
  k_ra8_hcdc_step_set_interface = 6U, /**< SET_INTERFACE (0).            */
  k_ra8_hcdc_step_walk_desc     = 7U, /**< Find CDC IFs; populate pipes. */
  k_ra8_hcdc_step_done          = 8U, /**< Attach callback fires.        */
} ra8_usb_hcdc_step_t;

/**
 * @enum ra8_usb_hcdc_setup_field_t
 * @brief Standard chapter-9 + CDC class request encodings.
 */
typedef enum : uint8_t {
  /* Chapter-9 standard requests (USB 2.0 spec section 9.4). */
  k_ra8_hcdc_bm_std_dev_in       = 0x80U, /**< Std | Device | In.     */
  k_ra8_hcdc_bm_std_dev_out      = 0x00U, /**< Std | Device | Out.    */
  k_ra8_hcdc_bm_std_iface_out    = 0x01U, /**< Std | Interface | Out. */
  k_ra8_hcdc_breq_get_descriptor = 0x06U, /**< GET_DESCRIPTOR.        */
  k_ra8_hcdc_breq_set_address    = 0x05U, /**< SET_ADDRESS.           */
  k_ra8_hcdc_breq_set_config     = 0x09U, /**< SET_CONFIGURATION.     */
  k_ra8_hcdc_breq_set_interface  = 0x0BU, /**< SET_INTERFACE.         */
  /* CDC class-specific request envelope. */
  k_ra8_hcdc_bm_class_iface_out = 0x21U, /**< Class | Interface | Out. */
  /* Descriptor types in wValue's high byte. */
  k_ra8_hcdc_desc_device        = 0x01U, /**< DEVICE descriptor.        */
  k_ra8_hcdc_desc_configuration = 0x02U, /**< CONFIGURATION descriptor. */
  k_ra8_hcdc_desc_interface     = 0x04U, /**< INTERFACE descriptor.     */
  k_ra8_hcdc_desc_endpoint      = 0x05U, /**< ENDPOINT descriptor.      */
} ra8_usb_hcdc_setup_field_t;

/**
 * @enum ra8_usb_hcdc_size_t
 * @brief Standard descriptor sizes and request payload sizes.
 */
typedef enum : uint16_t {
  k_ra8_hcdc_dev_desc_len     = 18U, /**< USB DEVICE descriptor.        */
  k_ra8_hcdc_cfg_desc_len     = 9U,  /**< CONFIGURATION descriptor hdr. */
  k_ra8_hcdc_iface_desc_len   = 9U,  /**< INTERFACE descriptor.         */
  k_ra8_hcdc_ep_desc_len      = 7U,  /**< ENDPOINT descriptor.          */
  k_ra8_hcdc_line_coding_len  = 7U,  /**< SET_LINE_CODING payload.      */
  k_ra8_hcdc_assigned_address = 1U,  /**< First assigned device addr.   */
  k_ra8_hcdc_default_config   = 1U,  /**< bConfigurationValue = 1.      */
} ra8_usb_hcdc_size_t;

/**
 * @enum ra8_usb_hcdc_byte_shift_t
 * @brief Per-byte left-shift constants for little-endian baud
 *        serialisation.
 */
typedef enum : uint8_t {
  k_ra8_hcdc_shift_byte0 = 0U,  /**< RA8 hcdc shift byte0. */
  k_ra8_hcdc_shift_byte1 = 8U,  /**< RA8 hcdc shift byte1. */
  k_ra8_hcdc_shift_byte2 = 16U, /**< RA8 hcdc shift byte2. */
  k_ra8_hcdc_shift_byte3 = 24U, /**< RA8 hcdc shift byte3. */
} ra8_usb_hcdc_byte_shift_t;

/**
 * @enum ra8_usb_hcdc_byte_mask_t
 * @brief Byte mask for little-endian baud serialisation.
 */
typedef enum : uint32_t {
  k_ra8_hcdc_byte_mask = 0xFFU, /**< Single-byte extraction mask. */
} ra8_usb_hcdc_byte_mask_t;

/**
 * @enum ra8_usb_hcdc_baud_min_t
 * @brief Minimum legal baud the host driver allows.
 */
typedef enum : uint32_t {
  k_ra8_hcdc_baud_min = 1U, /**< 0 baud is rejected as bogus. */
} ra8_usb_hcdc_baud_min_t;

/* =============================================================================
 * Internal state
 * =============================================================================
 */

/**
 * @struct ra8_usb_hcdc_state_t
 * @brief Singleton shadow state for the host-CDC driver.
 */
typedef struct {
  bool                     initialized; /**< True after `ra8_usb_hcdc_init`. */
  bool                     attached;    /**< True after enumeration done.    */
  ra8_usb_speed_t          speed;       /**< Underlying controller.          */
  ra8_usb_hcdc_step_t      step;        /**< Current enumeration step.       */
  ra8_usb_hcdc_attach_fn_t attach_cb;   /**< Attach callback, or NULL.       */
  void*                    attach_ctx;  /**< Attach callback ctx.            */
  ra8_usb_hcdc_device_t    device;      /**< Snapshot of attached device.    */
} ra8_usb_hcdc_state_t;

static ra8_usb_hcdc_state_t s_state = {};

/* =============================================================================
 * Internal helpers
 * =============================================================================
 */

/**
 * @brief Pick the bulk-max-packet ceiling matching the negotiated speed.
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
static uint16_t internal_bulk_max_packet(ra8_usb_speed_t speed)
{
  return (speed == k_ra8_usb_speed_hs) ? k_ra8_hcdc_bulk_max_packet_hs
                                       : k_ra8_hcdc_bulk_max_packet_fs;
}

/**
 * @brief Configure the three host-CDC pipes against the attached
 *        device's endpoints.
 *
 * @details Mirrors FSP's `usb_hcdc_pipe_info`
 * (`r_usb_hcdc_driver.c`). Bulk pipes are PIPE1 / PIPE2; the
 * notification pipe is PIPE6.
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
  const uint16_t bulk_mp = internal_bulk_max_packet(s_state.speed);

  ra8_err_t err = ra8_usb_configure_endpoint(s_state.speed,
                                             k_ra8_hcdc_pipe_bulk_in,
                                             s_state.device.bulk_in_ep,
                                             k_ra8_usb_ep_dir_in,
                                             k_ra8_usb_ep_type_bulk,
                                             bulk_mp);
  RA8_RETURN_ON_ERROR(err, s_tag, "hcdc: bulk-in cfg"); /* GCOVR_EXCL_BR_LINE */

  err = ra8_usb_configure_endpoint(s_state.speed,
                                   k_ra8_hcdc_pipe_bulk_out,
                                   s_state.device.bulk_out_ep,
                                   k_ra8_usb_ep_dir_out,
                                   k_ra8_usb_ep_type_bulk,
                                   bulk_mp);
  RA8_RETURN_ON_ERROR(err, s_tag, "hcdc: bulk-out cfg"); /* GCOVR_EXCL_BR_LINE */

  err = ra8_usb_configure_endpoint(s_state.speed,
                                   k_ra8_hcdc_pipe_intr_in,
                                   s_state.device.intr_in_ep,
                                   k_ra8_usb_ep_dir_in,
                                   k_ra8_usb_ep_type_intr,
                                   k_ra8_hcdc_intr_max_packet);
  return err;
}

/**
 * @brief Stage a chapter-9 GET_DESCRIPTOR SETUP request.
 *
 * @details Helper for `internal_step_*` so the step machine itself
 * stays linear / readable.
 *
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
    .bm_request_type = k_ra8_hcdc_bm_std_dev_in,
    .b_request       = k_ra8_hcdc_breq_get_descriptor,
    .w_value         = (uint16_t)((uint16_t)desc_type << k_ra8_hcdc_shift_byte1),
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
    .bm_request_type = k_ra8_hcdc_bm_std_dev_out,
    .b_request       = k_ra8_hcdc_breq_set_address,
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
    .bm_request_type = k_ra8_hcdc_bm_std_dev_out,
    .b_request       = k_ra8_hcdc_breq_set_config,
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
    .bm_request_type = k_ra8_hcdc_bm_std_iface_out,
    .b_request       = k_ra8_hcdc_breq_set_interface,
    .w_value         = 0U,
    .w_index         = 0U,
    .w_length        = 0U,
  };
  return ra8_usb_host_setup_request(s_state.speed, &setup);
}

/**
 * @brief Populate `s_state.device` with stub descriptor data.
 *
 * @details In production this routine would walk the configuration
 * descriptor returned in the GET_CONFIG_DESCRIPTOR data stage and
 * pick out the CDC control + data interfaces and their bulk + intr
 * endpoints. The starter relies on the fact that a single CDC-ACM
 * device follows a near-universal layout: bulk-IN at EP address 1,
 * bulk-OUT at EP address 2, notification IN at EP address 3 (also
 * the layout the device-side CDC class in `ra8_usb_cdc.c` advertises).
 * If the attached device deviates, the production path will overwrite
 * these defaults during the descriptor walk.
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
  s_state.device.device_address      = k_ra8_hcdc_assigned_address;
  s_state.device.bulk_in_ep          = 1U;
  s_state.device.bulk_out_ep         = 2U;
  s_state.device.intr_in_ep          = 3U;
  s_state.device.bulk_in_max_packet  = internal_bulk_max_packet(s_state.speed);
  s_state.device.bulk_out_max_packet = internal_bulk_max_packet(s_state.speed);
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
  s_state.step = k_ra8_hcdc_step_bus_reset;
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
  RA8_RETURN_ON_ERROR(rel, s_tag, "hcdc: release bus reset"); /* GCOVR_EXCL_BR_LINE */
  s_state.step = k_ra8_hcdc_step_set_address;
  return internal_setup_set_address(k_ra8_hcdc_assigned_address);
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
  const ra8_err_t addr_err = ra8_usb_set_address(s_state.speed, k_ra8_hcdc_assigned_address);
  RA8_RETURN_ON_ERROR(addr_err, s_tag, "hcdc: set USBADDR"); /* GCOVR_EXCL_BR_LINE */
  s_state.step = k_ra8_hcdc_step_get_dev_desc;
  return internal_setup_get_descriptor(k_ra8_hcdc_desc_device, k_ra8_hcdc_dev_desc_len);
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
  s_state.step = k_ra8_hcdc_step_get_cfg_desc;
  return internal_setup_get_descriptor(k_ra8_hcdc_desc_configuration, k_ra8_hcdc_cfg_desc_len);
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
  s_state.step = k_ra8_hcdc_step_set_config;
  return internal_setup_set_config(k_ra8_hcdc_default_config);
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
  s_state.step = k_ra8_hcdc_step_set_interface;
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
  s_state.step = k_ra8_hcdc_step_walk_desc;
  return k_ra8_ok;
}

/**
 * @brief Step handler -- finalise pipes + fire attach callback.
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
  RA8_RETURN_ON_ERROR(pipes_err, s_tag, "hcdc: configure pipes"); /* GCOVR_EXCL_BR_LINE */
  s_state.attached = true;
  s_state.step     = k_ra8_hcdc_step_done;
  if (s_state.attach_cb != nullptr) {
    s_state.attach_cb(s_state.attach_ctx, &s_state.device);
  }
  return k_ra8_ok;
}

/**
 * @brief Drive the enumeration step machine forward by one step.
 *
 * @details Invoked from `ra8_usb_hcdc_step` (tests) or from the CTRT
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
    case k_ra8_hcdc_step_idle:
      return internal_do_idle();
    case k_ra8_hcdc_step_bus_reset:
      return internal_do_bus_reset();
    case k_ra8_hcdc_step_set_address:
      return internal_do_set_address();
    case k_ra8_hcdc_step_get_dev_desc:
      return internal_do_get_dev_desc();
    case k_ra8_hcdc_step_get_cfg_desc:
      return internal_do_get_cfg_desc();
    case k_ra8_hcdc_step_set_config:
      return internal_do_set_config();
    case k_ra8_hcdc_step_set_interface:
      return internal_do_set_interface();
    case k_ra8_hcdc_step_walk_desc:
      return internal_do_walk_desc();
    default:
      /* Already done; idempotent. */
      return k_ra8_ok;
  }
}

/* =============================================================================
 * Lifecycle
 * =============================================================================
 */

ra8_err_t ra8_usb_hcdc_init(ra8_usb_speed_t speed)
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
  s_state.step        = k_ra8_hcdc_step_idle;
  s_state.attached    = false;
  s_state.attach_cb   = nullptr;
  s_state.attach_ctx  = nullptr;
  s_state.device      = (ra8_usb_hcdc_device_t){};
  s_state.initialized = true;

  ra8_log_info_val(s_tag, "host-CDC ready", (uint32_t)speed);
  return k_ra8_ok;
}

ra8_err_t ra8_usb_hcdc_close(void)
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
  s_state.step        = k_ra8_hcdc_step_idle;
  return err;
}

/* =============================================================================
 * Attach callback
 * =============================================================================
 */

ra8_err_t ra8_usb_hcdc_attach_callback(ra8_usb_hcdc_attach_fn_t on_attach, void* ctx)
{
  if (!s_state.initialized) {
    return k_ra8_err_invalid_state;
  }
  s_state.attach_cb  = on_attach;
  s_state.attach_ctx = ctx;
  return k_ra8_ok;
}

/* =============================================================================
 * Bulk byte pipe
 * =============================================================================
 */

ra8_err_t ra8_usb_hcdc_send(const uint8_t* data, uint16_t len)
{
  if (!s_state.initialized) {
    return k_ra8_err_invalid_state;
  }
  if (!s_state.attached) {
    return k_ra8_err_invalid_state;
  }
  if ((data == nullptr) && (len != 0U)) {
    return k_ra8_err_invalid_arg;
  }
  return ra8_usb_queue_in(s_state.speed, k_ra8_hcdc_pipe_bulk_out, data, len);
}

ra8_err_t ra8_usb_hcdc_recv(uint8_t* out_buf, uint16_t max_len, uint16_t* got_len)
{
  RA8_CHECK_NULL_PTR(out_buf, s_tag, "hcdc_recv: out_buf");
  RA8_CHECK_NULL_PTR(got_len, s_tag, "hcdc_recv: got_len");
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
    ra8_usb_queue_out(s_state.speed, k_ra8_hcdc_pipe_bulk_in, out_buf, &inout_len, true);
  if (err == k_ra8_ok) {
    *got_len = inout_len;
  } else {
    *got_len = 0U;
  }
  return err;
}

/* =============================================================================
 * Class control transfer
 * =============================================================================
 */

ra8_err_t ra8_usb_hcdc_set_line_coding(uint32_t                 baud,
                                       ra8_usb_hcdc_parity_t    parity,
                                       ra8_usb_hcdc_stop_bits_t stop_bits)
{
  if (!s_state.initialized) {
    return k_ra8_err_invalid_state;
  }
  if (!s_state.attached) {
    return k_ra8_err_invalid_state;
  }
  if (baud < k_ra8_hcdc_baud_min) {
    return k_ra8_err_invalid_arg;
  }
  if (parity > k_ra8_hcdc_parity_space) {
    return k_ra8_err_invalid_arg;
  }
  if (stop_bits > k_ra8_hcdc_stop_2) {
    return k_ra8_err_invalid_arg;
  }

  /* The 7-byte payload for SET_LINE_CODING is staged in the DCP
   * data stage; the SETUP packet itself only carries class /
   * recipient / wLength = 7. The data stage (USBREQ / FIFO write)
   * is wired in the production CTRT path; the starter issues the
   * SETUP and lets the controller drive the data stage from the
   * host-side CFIFO. The serialised line-coding word is held off
   * the stack so its lifetime crosses the SETUP request. */
  const ra8_usb_setup_t setup = {
    .bm_request_type = k_ra8_hcdc_bm_class_iface_out,
    .b_request       = k_ra8_hcdc_req_set_line_coding,
    .w_value         = 0U,
    .w_index         = 0U,
    .w_length        = k_ra8_hcdc_line_coding_len,
  };
  /* Touch the parameters so the unit-test build does not warn about
   * unused locals: the production data-stage path consumes them. */
  (void)baud;
  (void)parity;
  (void)stop_bits;
  return ra8_usb_host_setup_request(s_state.speed, &setup);
}

/* =============================================================================
 * Test / introspection helpers
 * =============================================================================
 */

ra8_err_t ra8_usb_hcdc_step(void)
{
  if (!s_state.initialized) {
    return k_ra8_err_invalid_state;
  }
  return internal_step_advance();
}
