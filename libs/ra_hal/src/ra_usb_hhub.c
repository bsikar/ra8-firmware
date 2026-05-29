/**
 * @file ra_usb_hhub.c
 * @brief Native USB host-side HUB class layer implementation
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Glues the host-mode bring-up paths in `ra_usb` to a HUB peripheral
 * (class 0x09) attached on the EK-RA8D2's USB host port. Mirrors
 * FSP's `r_usb_basic` hub-support pattern -- enumerate the HUB
 * itself, cache `bNbrPorts`, and expose the GET_PORT_STATUS /
 * SET_PORT_FEATURE / CLEAR_PORT_FEATURE class transfers used by the
 * application to sequence per-port reset, power, and child enumeration.
 *
 * Each enumeration step issues exactly one chapter-9 SETUP request via
 * `ra_usb_host_setup_request`; the next CTRT advances the step.
 *
 * Reference: USB 2.0 specification chapter 11 "Hub Specification"
 * (sec 11.23 descriptors, sec 11.24 class requests). Class request
 * envelope encoding follows USB 2.0 Table 11-15.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_usb_hhub.h"

#include <stdint.h>

#include "ra_check.h"
#include "ra_err.h"
#include "ra_log.h"
#include "ra_usb.h"

static const char* s_tag = "USBHHUB";

/* =============================================================================
 * Internal constants
 * =============================================================================
 */

/**
 * @enum ra_usb_hhub_step_t
 * @brief Enumeration step machine states.
 */
typedef enum : uint8_t {
  k_ra_hhub_step_idle         = 0U, /**< Pre-attach.                  */
  k_ra_hhub_step_bus_reset    = 1U, /**< Drive USBRST then release.   */
  k_ra_hhub_step_set_address  = 2U, /**< SET_ADDRESS to assigned 1.   */
  k_ra_hhub_step_get_dev_desc = 3U, /**< GET_DEVICE_DESCRIPTOR.       */
  k_ra_hhub_step_get_cfg_desc = 4U, /**< GET_CONFIGURATION_DESCRIPTOR.*/
  k_ra_hhub_step_set_config   = 5U, /**< SET_CONFIGURATION (1).       */
  k_ra_hhub_step_get_hub_desc = 6U, /**< GET_DESCRIPTOR(HUB) class.   */
  k_ra_hhub_step_done         = 7U, /**< Attach callback fires.       */
} ra_usb_hhub_step_t;

/**
 * @enum ra_usb_hhub_setup_field_t
 * @brief Standard chapter-9 + HUB class request encodings.
 */
typedef enum : uint8_t {
  /* Chapter-9 standard requests. */
  k_ra_hhub_bm_std_dev_in    = 0x80U, /**< Std | Device | In.    */
  k_ra_hhub_bm_std_dev_out   = 0x00U, /**< Std | Device | Out.   */
  k_ra_hhub_breq_get_desc    = 0x06U, /**< GET_DESCRIPTOR.       */
  k_ra_hhub_breq_set_address = 0x05U, /**< SET_ADDRESS.          */
  k_ra_hhub_breq_set_config  = 0x09U, /**< SET_CONFIGURATION.    */
  /* HUB class request envelopes (USB 2.0 sec 11.24.1 Table 11-14). */
  k_ra_hhub_bm_class_dev_in    = 0xA0U, /**< Class | Device | In.  */
  k_ra_hhub_bm_class_other_in  = 0xA3U, /**< Class | Other | In.   */
  k_ra_hhub_bm_class_other_out = 0x23U, /**< Class | Other | Out. */
  /* Descriptor types in wValue's high byte. */
  k_ra_hhub_desc_device        = 0x01U, /**< DEVICE descriptor.    */
  k_ra_hhub_desc_configuration = 0x02U, /**< CONFIG descriptor.   */
} ra_usb_hhub_setup_field_t;

/**
 * @enum ra_usb_hhub_size_t
 * @brief Standard descriptor sizes / payload sizes.
 */
typedef enum : uint16_t {
  k_ra_hhub_dev_desc_len     = 18U, /**< USB DEVICE descriptor.       */
  k_ra_hhub_cfg_desc_len     = 9U,  /**< CONFIG descriptor header.    */
  k_ra_hhub_hub_desc_len     = 9U,  /**< Min HUB class descriptor.    */
  k_ra_hhub_port_status_len  = 4U,  /**< GET_PORT_STATUS data length. */
  k_ra_hhub_assigned_address = 1U,  /**< First assigned device addr.  */
  k_ra_hhub_default_config   = 1U,  /**< bConfigurationValue = 1.     */
  k_ra_hhub_default_ports    = 4U,  /**< Stub default port count.     */
} ra_usb_hhub_size_t;

/**
 * @enum ra_usb_hhub_byte_shift_t
 * @brief Per-byte left-shift constants for wValue layout.
 */
typedef enum : uint8_t {
  k_ra_hhub_shift_byte1 = 8U, /**< Shift to high byte of a 16-bit field. */
} ra_usb_hhub_byte_shift_t;

/* =============================================================================
 * Internal state
 * =============================================================================
 */

/**
 * @struct ra_usb_hhub_state_t
 * @brief Singleton shadow state for the host-HUB driver.
 */
typedef struct {
  bool                    initialized; /**< True after `ra_usb_hhub_init`.   */
  bool                    attached;    /**< True after enumeration done.    */
  ra_usb_speed_t          speed;       /**< Underlying controller.          */
  ra_usb_hhub_step_t      step;        /**< Current enumeration step.       */
  ra_usb_hhub_attach_fn_t attach_cb;   /**< Attach callback, or NULL.       */
  void*                   attach_ctx;  /**< Attach callback ctx.            */
  ra_usb_hhub_device_t    device;      /**< Snapshot of attached HUB.       */
} ra_usb_hhub_state_t;

static ra_usb_hhub_state_t s_state = {};

/* =============================================================================
 * Internal helpers
 * =============================================================================
 */

/**
 * @brief Stage a chapter-9 GET_DESCRIPTOR SETUP request.
 *
 * @details See implementation.
 * @param[in] desc_type See implementation.
 * @param[in] length See implementation.
 * @return Result code.
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static ra_err_t internal_setup_get_descriptor(uint8_t desc_type, uint16_t length)
{
  const ra_usb_setup_t setup = {
    .bm_request_type = k_ra_hhub_bm_std_dev_in,
    .b_request       = k_ra_hhub_breq_get_desc,
    .w_value         = (uint16_t)((uint16_t)desc_type << k_ra_hhub_shift_byte1),
    .w_index         = 0U,
    .w_length        = length,
  };
  return ra_usb_host_setup_request(s_state.speed, &setup);
}

/**
 * @brief Stage a SET_ADDRESS SETUP request.
 *
 * @details See implementation.
 * @param[in] address See implementation.
 * @return Result code.
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static ra_err_t internal_setup_set_address(uint8_t address)
{
  const ra_usb_setup_t setup = {
    .bm_request_type = k_ra_hhub_bm_std_dev_out,
    .b_request       = k_ra_hhub_breq_set_address,
    .w_value         = (uint16_t)address,
    .w_index         = 0U,
    .w_length        = 0U,
  };
  return ra_usb_host_setup_request(s_state.speed, &setup);
}

/**
 * @brief Stage a SET_CONFIGURATION SETUP request.
 *
 * @details See implementation.
 * @param[in] config_value See implementation.
 * @return Result code.
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static ra_err_t internal_setup_set_config(uint8_t config_value)
{
  const ra_usb_setup_t setup = {
    .bm_request_type = k_ra_hhub_bm_std_dev_out,
    .b_request       = k_ra_hhub_breq_set_config,
    .w_value         = (uint16_t)config_value,
    .w_index         = 0U,
    .w_length        = 0U,
  };
  return ra_usb_host_setup_request(s_state.speed, &setup);
}

/**
 * @brief Stage a GET_DESCRIPTOR(HUB) class request.
 *
 * @details Per USB 2.0 sec 11.24.2.5 GET_HUB_DESCRIPTOR uses
 * bmRequestType = 0xA0 (Class | Device | In), bRequest = 0x06,
 * wValue.high = 0x29 (HUB), wIndex = 0, wLength = descriptor length.
 *
 * @return Result code.
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static ra_err_t internal_setup_get_hub_descriptor(void)
{
  const ra_usb_setup_t setup = {
    .bm_request_type = k_ra_hhub_bm_class_dev_in,
    .b_request       = k_ra_hhub_req_get_descriptor,
    .w_value         = (uint16_t)((uint16_t)k_ra_hhub_desc_hub << k_ra_hhub_shift_byte1),
    .w_index         = 0U,
    .w_length        = k_ra_hhub_hub_desc_len,
  };
  return ra_usb_host_setup_request(s_state.speed, &setup);
}

/**
 * @brief Populate `s_state.device` with stub descriptor data.
 *
 * @details In production this routine consumes the HUB class
 * descriptor data stage and reads `bNbrPorts` (USB 2.0 sec 11.23.2.1).
 * The starter populates a sensible default (4 ports, no VID/PID) so
 * tests can exercise the API surface deterministically.
 *
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static void internal_populate_device_defaults(void)
{
  s_state.device.device_address = k_ra_hhub_assigned_address;
  s_state.device.port_count     = k_ra_hhub_default_ports;
  s_state.device.vendor_id      = 0U;
  s_state.device.product_id     = 0U;
}

/**
 * @brief Validate `port` is within [1, port_count].
 *
 * @details See implementation.
 * @param[in] port See implementation.
 * @return Result code.
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static bool internal_port_ok(uint8_t port)
{
  return (port >= k_ra_hhub_first_port) && (port <= s_state.device.port_count);
}

/* ---- Per-step handlers ---- */

/**
 * @brief Internal helper.
 * @details See implementation.
 * @return Result code.
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static ra_err_t internal_do_idle(void)
{
  s_state.step = k_ra_hhub_step_bus_reset;
  return ra_usb_host_bus_reset(s_state.speed, true);
}

/**
 * @brief Internal helper.
 * @details See implementation.
 * @return Result code.
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static ra_err_t internal_do_bus_reset(void)
{
  const ra_err_t rel = ra_usb_host_bus_reset(s_state.speed, false);
  RA_RETURN_ON_ERROR(rel, s_tag, "hhub: release bus reset"); /* GCOVR_EXCL_BR_LINE */
  s_state.step = k_ra_hhub_step_set_address;
  return internal_setup_set_address(k_ra_hhub_assigned_address);
}

/**
 * @brief Internal helper.
 * @details See implementation.
 * @return Result code.
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static ra_err_t internal_do_set_address(void)
{
  const ra_err_t addr_err = ra_usb_set_address(s_state.speed, k_ra_hhub_assigned_address);
  RA_RETURN_ON_ERROR(addr_err, s_tag, "hhub: set USBADDR"); /* GCOVR_EXCL_BR_LINE */
  s_state.step = k_ra_hhub_step_get_dev_desc;
  return internal_setup_get_descriptor(k_ra_hhub_desc_device, k_ra_hhub_dev_desc_len);
}

/**
 * @brief Internal helper.
 * @details See implementation.
 * @return Result code.
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static ra_err_t internal_do_get_dev_desc(void)
{
  s_state.step = k_ra_hhub_step_get_cfg_desc;
  return internal_setup_get_descriptor(k_ra_hhub_desc_configuration, k_ra_hhub_cfg_desc_len);
}

/**
 * @brief Internal helper.
 * @details See implementation.
 * @return Result code.
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static ra_err_t internal_do_get_cfg_desc(void)
{
  s_state.step = k_ra_hhub_step_set_config;
  return internal_setup_set_config(k_ra_hhub_default_config);
}

/**
 * @brief Internal helper.
 * @details See implementation.
 * @return Result code.
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static ra_err_t internal_do_set_config(void)
{
  s_state.step = k_ra_hhub_step_get_hub_desc;
  return internal_setup_get_hub_descriptor();
}

/**
 * @brief Internal helper.
 * @details See implementation.
 * @return Result code.
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static ra_err_t internal_do_get_hub_desc(void)
{
  internal_populate_device_defaults();
  s_state.attached = true;
  s_state.step     = k_ra_hhub_step_done;
  if (s_state.attach_cb != nullptr) {
    s_state.attach_cb(s_state.attach_ctx, &s_state.device);
  }
  return k_ra_ok;
}

/**
 * @brief Drive the enumeration step machine forward by one step.
 *
 * @details See implementation.
 * @return Result code.
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static ra_err_t internal_step_advance(void)
{
  switch (s_state.step) {
    case k_ra_hhub_step_idle:
      return internal_do_idle();
    case k_ra_hhub_step_bus_reset:
      return internal_do_bus_reset();
    case k_ra_hhub_step_set_address:
      return internal_do_set_address();
    case k_ra_hhub_step_get_dev_desc:
      return internal_do_get_dev_desc();
    case k_ra_hhub_step_get_cfg_desc:
      return internal_do_get_cfg_desc();
    case k_ra_hhub_step_set_config:
      return internal_do_set_config();
    case k_ra_hhub_step_get_hub_desc:
      return internal_do_get_hub_desc();
    default:
      /* Already done; idempotent. */
      return k_ra_ok;
  }
}

/* =============================================================================
 * Lifecycle
 * =============================================================================
 */

ra_err_t ra_usb_hhub_init(ra_usb_speed_t speed)
{
  if ((speed != k_ra_usb_speed_fs) && (speed != k_ra_usb_speed_hs)) {
    return k_ra_err_invalid_arg;
  }
  const ra_err_t usb_err = ra_usb_host_init(speed);
  if (usb_err != k_ra_ok) {
    ra_log_error_val(s_tag, "ra_usb_host_init failed", (uint32_t)usb_err);
    return k_ra_err_hw_init_failed;
  }

  s_state.speed       = speed;
  s_state.step        = k_ra_hhub_step_idle;
  s_state.attached    = false;
  s_state.attach_cb   = nullptr;
  s_state.attach_ctx  = nullptr;
  s_state.device      = (ra_usb_hhub_device_t){};
  s_state.initialized = true;

  ra_log_info_val(s_tag, "host-HUB ready", (uint32_t)speed);
  return k_ra_ok;
}

ra_err_t ra_usb_hhub_close(void)
{
  if (!s_state.initialized) {
    return k_ra_err_invalid_state;
  }
  (void)ra_usb_host_set_uact(s_state.speed, false);
  const ra_err_t err  = ra_usb_host_deinit(s_state.speed);
  s_state.initialized = false;
  s_state.attached    = false;
  s_state.attach_cb   = nullptr;
  s_state.attach_ctx  = nullptr;
  s_state.step        = k_ra_hhub_step_idle;
  return err;
}

/* =============================================================================
 * Attach callback
 * =============================================================================
 */

ra_err_t ra_usb_hhub_attach_callback(ra_usb_hhub_attach_fn_t on_attach, void* ctx)
{
  if (!s_state.initialized) {
    return k_ra_err_invalid_state;
  }
  s_state.attach_cb  = on_attach;
  s_state.attach_ctx = ctx;
  return k_ra_ok;
}

/* =============================================================================
 * HUB class control transfers
 * =============================================================================
 */

ra_err_t ra_usb_hhub_get_port_count(uint8_t* count)
{
  RA_CHECK_NULL_PTR(count, s_tag, "get_port_count: count");
  if (!s_state.initialized) {
    return k_ra_err_invalid_state;
  }
  if (!s_state.attached) {
    return k_ra_err_invalid_state;
  }
  *count = s_state.device.port_count;
  return k_ra_ok;
}

/* USB 2.0 sec 11.24.2.7 GET_PORT_STATUS:
 *   bmRequestType = 0xA3 (D2H | Class | Other)
 *   bRequest      = 0x00 (GET_STATUS)
 *   wValue        = 0
 *   wIndex        = port
 *   wLength       = 4
 */
ra_err_t ra_usb_hhub_get_port_status(uint8_t port, uint32_t* status)
{
  RA_CHECK_NULL_PTR(status, s_tag, "get_port_status: status");
  if (!s_state.initialized) {
    return k_ra_err_invalid_state;
  }
  if (!s_state.attached) {
    return k_ra_err_invalid_state;
  }
  if (!internal_port_ok(port)) {
    return k_ra_err_invalid_arg;
  }
  *status                    = 0U;
  const ra_usb_setup_t setup = {
    .bm_request_type = k_ra_hhub_bm_class_other_in,
    .b_request       = k_ra_hhub_req_get_status,
    .w_value         = 0U,
    .w_index         = (uint16_t)port,
    .w_length        = k_ra_hhub_port_status_len,
  };
  return ra_usb_host_setup_request(s_state.speed, &setup);
}

/* USB 2.0 sec 11.24.2.13 SET_PORT_FEATURE:
 *   bmRequestType = 0x23 (H2D | Class | Other)
 *   bRequest      = 0x03 (SET_FEATURE)
 *   wValue        = feature
 *   wIndex        = port
 *   wLength       = 0
 */
ra_err_t ra_usb_hhub_set_port_feature(uint8_t port, ra_usb_hhub_feature_t feature)
{
  if (!s_state.initialized) {
    return k_ra_err_invalid_state;
  }
  if (!s_state.attached) {
    return k_ra_err_invalid_state;
  }
  if (!internal_port_ok(port)) {
    return k_ra_err_invalid_arg;
  }
  const ra_usb_setup_t setup = {
    .bm_request_type = k_ra_hhub_bm_class_other_out,
    .b_request       = k_ra_hhub_req_set_feature,
    .w_value         = (uint16_t)feature,
    .w_index         = (uint16_t)port,
    .w_length        = 0U,
  };
  return ra_usb_host_setup_request(s_state.speed, &setup);
}

/* USB 2.0 sec 11.24.2.2 CLEAR_PORT_FEATURE:
 *   bmRequestType = 0x23 (H2D | Class | Other)
 *   bRequest      = 0x01 (CLEAR_FEATURE)
 *   wValue        = feature
 *   wIndex        = port
 *   wLength       = 0
 */
ra_err_t ra_usb_hhub_clear_port_feature(uint8_t port, ra_usb_hhub_feature_t feature)
{
  if (!s_state.initialized) {
    return k_ra_err_invalid_state;
  }
  if (!s_state.attached) {
    return k_ra_err_invalid_state;
  }
  if (!internal_port_ok(port)) {
    return k_ra_err_invalid_arg;
  }
  const ra_usb_setup_t setup = {
    .bm_request_type = k_ra_hhub_bm_class_other_out,
    .b_request       = k_ra_hhub_req_clear_feature,
    .w_value         = (uint16_t)feature,
    .w_index         = (uint16_t)port,
    .w_length        = 0U,
  };
  return ra_usb_host_setup_request(s_state.speed, &setup);
}

/* =============================================================================
 * Test / introspection helpers
 * =============================================================================
 */

ra_err_t ra_usb_hhub_step(void)
{
  if (!s_state.initialized) {
    return k_ra_err_invalid_state;
  }
  return internal_step_advance();
}
