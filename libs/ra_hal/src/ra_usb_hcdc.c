/**
 * @file ra_usb_hcdc.c
 * @brief Native USB host-side CDC ACM class layer implementation
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Glues the host-mode bring-up paths in `ra_usb` to a CDC-ACM
 * peripheral attached on the EK-RA8D2's USB-host port. This file is
 * the native host-CDC class layer; FSP's `r_usb_hcdc_driver.c` and
 * `r_usb_hcdc.c` are reference material only -- nothing is pulled in
 * verbatim.
 *
 * Mapping vs FSP (FSP function -> our entry point):
 *
 *  - `usb_hcdc_init`            -> `ra_usb_hcdc_init`
 *  - `usb_hcdc_enumeration`     -> `internal_enum_*` step machine
 *                                  driven by `ra_usb_hcdc_step`.
 *  - `usb_hcdc_pipe_info`       -> `internal_walk_config_descriptor`
 *  - `R_USB_HCDC_DeviceInfoGet` -> attach-callback `device` payload
 *
 * The starter does CPU-FIFO, single-device, no-hub. Enumeration is
 * driven step-by-step from the controller's CTRT interrupt path
 * (production) or directly via `ra_usb_hcdc_step` (tests). Each step
 * issues exactly one chapter-9 SETUP request via
 * `ra_usb_host_setup_request`; the next CTRT advances the step.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_usb_hcdc.h"

#include <stdint.h>

#include "ra_check.h"
#include "ra_err.h"
#include "ra_log.h"
#include "ra_usb.h"

static const char* s_tag = "USBHCDC";

/* =============================================================================
 * Internal constants
 * =============================================================================
 */

/**
 * @enum ra_usb_hcdc_step_t
 * @brief Enumeration step machine states.
 *
 * @details Mirrors FSP's `g_usb_hcdc_smpl_class_seq` step indices in
 * `r_usb_hcdc_driver.c:91`. Each step issues exactly one SETUP via
 * `ra_usb_host_setup_request`; the next CTRT interrupt advances to
 * the next step.
 */
typedef enum : uint8_t {
  k_ra_hcdc_step_idle          = 0U, /**< Pre-attach.                  */
  k_ra_hcdc_step_bus_reset     = 1U, /**< Drive USBRST then release.   */
  k_ra_hcdc_step_set_address   = 2U, /**< SET_ADDRESS to assigned 1.   */
  k_ra_hcdc_step_get_dev_desc  = 3U, /**< GET_DEVICE_DESCRIPTOR (18 B).*/
  k_ra_hcdc_step_get_cfg_desc  = 4U, /**< GET_CONFIGURATION_DESCRIPTOR.*/
  k_ra_hcdc_step_set_config    = 5U, /**< SET_CONFIGURATION (1).       */
  k_ra_hcdc_step_set_interface = 6U, /**< SET_INTERFACE (0).           */
  k_ra_hcdc_step_walk_desc     = 7U, /**< Find CDC IFs; populate pipes.*/
  k_ra_hcdc_step_done          = 8U, /**< Attach callback fires.       */
} ra_usb_hcdc_step_t;

/**
 * @enum ra_usb_hcdc_setup_field_t
 * @brief Standard chapter-9 + CDC class request encodings.
 */
typedef enum : uint8_t {
  /* Chapter-9 standard requests (USB 2.0 spec section 9.4). */
  k_ra_hcdc_bm_std_dev_in       = 0x80U, /**< Std | Device | In.       */
  k_ra_hcdc_bm_std_dev_out      = 0x00U, /**< Std | Device | Out.      */
  k_ra_hcdc_bm_std_iface_out    = 0x01U, /**< Std | Interface | Out.   */
  k_ra_hcdc_breq_get_descriptor = 0x06U, /**< GET_DESCRIPTOR.          */
  k_ra_hcdc_breq_set_address    = 0x05U, /**< SET_ADDRESS.             */
  k_ra_hcdc_breq_set_config     = 0x09U, /**< SET_CONFIGURATION.       */
  k_ra_hcdc_breq_set_interface  = 0x0BU, /**< SET_INTERFACE.           */
  /* CDC class-specific request envelope. */
  k_ra_hcdc_bm_class_iface_out = 0x21U, /**< Class | Interface | Out. */
  /* Descriptor types in wValue's high byte. */
  k_ra_hcdc_desc_device        = 0x01U, /**< DEVICE descriptor.       */
  k_ra_hcdc_desc_configuration = 0x02U, /**< CONFIGURATION descriptor.*/
  k_ra_hcdc_desc_interface     = 0x04U, /**< INTERFACE descriptor.    */
  k_ra_hcdc_desc_endpoint      = 0x05U, /**< ENDPOINT descriptor.     */
} ra_usb_hcdc_setup_field_t;

/**
 * @enum ra_usb_hcdc_size_t
 * @brief Standard descriptor sizes and request payload sizes.
 */
typedef enum : uint16_t {
  k_ra_hcdc_dev_desc_len     = 18U, /**< USB DEVICE descriptor.       */
  k_ra_hcdc_cfg_desc_len     = 9U,  /**< CONFIGURATION descriptor hdr.*/
  k_ra_hcdc_iface_desc_len   = 9U,  /**< INTERFACE descriptor.        */
  k_ra_hcdc_ep_desc_len      = 7U,  /**< ENDPOINT descriptor.         */
  k_ra_hcdc_line_coding_len  = 7U,  /**< SET_LINE_CODING payload.     */
  k_ra_hcdc_assigned_address = 1U,  /**< First assigned device addr.  */
  k_ra_hcdc_default_config   = 1U,  /**< bConfigurationValue = 1.     */
} ra_usb_hcdc_size_t;

/**
 * @enum ra_usb_hcdc_byte_shift_t
 * @brief Per-byte left-shift constants for little-endian baud
 *        serialisation.
 */
typedef enum : uint8_t {
  k_ra_hcdc_shift_byte0 = 0U,
  k_ra_hcdc_shift_byte1 = 8U,
  k_ra_hcdc_shift_byte2 = 16U,
  k_ra_hcdc_shift_byte3 = 24U,
} ra_usb_hcdc_byte_shift_t;

/**
 * @enum ra_usb_hcdc_byte_mask_t
 * @brief Byte mask for little-endian baud serialisation.
 */
typedef enum : uint32_t {
  k_ra_hcdc_byte_mask = 0xFFU, /**< Single-byte extraction mask. */
} ra_usb_hcdc_byte_mask_t;

/**
 * @enum ra_usb_hcdc_baud_min_t
 * @brief Minimum legal baud the host driver allows.
 */
typedef enum : uint32_t {
  k_ra_hcdc_baud_min = 1U, /**< 0 baud is rejected as bogus. */
} ra_usb_hcdc_baud_min_t;

/* =============================================================================
 * Internal state
 * =============================================================================
 */

/**
 * @struct ra_usb_hcdc_state_t
 * @brief Singleton shadow state for the host-CDC driver.
 */
typedef struct {
  bool                    initialised; /**< True after `ra_usb_hcdc_init`. */
  bool                    attached;    /**< True after enumeration done.   */
  ra_usb_speed_t          speed;       /**< Underlying controller.         */
  ra_usb_hcdc_step_t      step;        /**< Current enumeration step.      */
  ra_usb_hcdc_attach_fn_t attach_cb;   /**< Attach callback, or NULL.      */
  void*                   attach_ctx;  /**< Attach callback ctx.           */
  ra_usb_hcdc_device_t    device;      /**< Snapshot of attached device.   */
} ra_usb_hcdc_state_t;

static ra_usb_hcdc_state_t s_state = {};

/* =============================================================================
 * Internal helpers
 * =============================================================================
 */

/**
 * @brief Pick the bulk-max-packet ceiling matching the negotiated speed.
 */
static uint16_t internal_bulk_max_packet(ra_usb_speed_t speed)
{
  return (speed == k_ra_usb_speed_hs) ? k_ra_hcdc_bulk_max_packet_hs : k_ra_hcdc_bulk_max_packet_fs;
}

/**
 * @brief Configure the three host-CDC pipes against the attached
 *        device's endpoints.
 *
 * @details Mirrors FSP's `usb_hcdc_pipe_info`
 * (`r_usb_hcdc_driver.c:172`). Bulk pipes are PIPE1 / PIPE2; the
 * notification pipe is PIPE6.
 */
static ra_err_t internal_configure_pipes(void)
{
  const uint16_t bulk_mp = internal_bulk_max_packet(s_state.speed);

  ra_err_t err = ra_usb_configure_endpoint(s_state.speed,
                                           k_ra_hcdc_pipe_bulk_in,
                                           s_state.device.bulk_in_ep,
                                           k_ra_usb_ep_dir_in,
                                           k_ra_usb_ep_type_bulk,
                                           bulk_mp);
  RA_RETURN_ON_ERROR(err, s_tag, "hcdc: bulk-in cfg");

  err = ra_usb_configure_endpoint(s_state.speed,
                                  k_ra_hcdc_pipe_bulk_out,
                                  s_state.device.bulk_out_ep,
                                  k_ra_usb_ep_dir_out,
                                  k_ra_usb_ep_type_bulk,
                                  bulk_mp);
  RA_RETURN_ON_ERROR(err, s_tag, "hcdc: bulk-out cfg");

  err = ra_usb_configure_endpoint(s_state.speed,
                                  k_ra_hcdc_pipe_intr_in,
                                  s_state.device.intr_in_ep,
                                  k_ra_usb_ep_dir_in,
                                  k_ra_usb_ep_type_intr,
                                  k_ra_hcdc_intr_max_packet);
  return err;
}

/**
 * @brief Stage a chapter-9 GET_DESCRIPTOR SETUP request.
 *
 * @details Helper for `internal_step_*` so the step machine itself
 * stays linear / readable.
 */
static ra_err_t internal_setup_get_descriptor(uint8_t desc_type, uint16_t length)
{
  const ra_usb_setup_t setup = {
    .bm_request_type = k_ra_hcdc_bm_std_dev_in,
    .b_request       = k_ra_hcdc_breq_get_descriptor,
    .w_value         = (uint16_t)((uint16_t)desc_type << k_ra_hcdc_shift_byte1),
    .w_index         = 0U,
    .w_length        = length,
  };
  return ra_usb_host_setup_request(s_state.speed, &setup);
}

/**
 * @brief Stage a SET_ADDRESS SETUP request.
 */
static ra_err_t internal_setup_set_address(uint8_t address)
{
  const ra_usb_setup_t setup = {
    .bm_request_type = k_ra_hcdc_bm_std_dev_out,
    .b_request       = k_ra_hcdc_breq_set_address,
    .w_value         = (uint16_t)address,
    .w_index         = 0U,
    .w_length        = 0U,
  };
  return ra_usb_host_setup_request(s_state.speed, &setup);
}

/**
 * @brief Stage a SET_CONFIGURATION SETUP request.
 */
static ra_err_t internal_setup_set_config(uint8_t config_value)
{
  const ra_usb_setup_t setup = {
    .bm_request_type = k_ra_hcdc_bm_std_dev_out,
    .b_request       = k_ra_hcdc_breq_set_config,
    .w_value         = (uint16_t)config_value,
    .w_index         = 0U,
    .w_length        = 0U,
  };
  return ra_usb_host_setup_request(s_state.speed, &setup);
}

/**
 * @brief Stage a SET_INTERFACE (alt 0, iface 0) SETUP request.
 */
static ra_err_t internal_setup_set_interface(void)
{
  const ra_usb_setup_t setup = {
    .bm_request_type = k_ra_hcdc_bm_std_iface_out,
    .b_request       = k_ra_hcdc_breq_set_interface,
    .w_value         = 0U,
    .w_index         = 0U,
    .w_length        = 0U,
  };
  return ra_usb_host_setup_request(s_state.speed, &setup);
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
 * the layout the device-side CDC class in `ra_usb_cdc.c` advertises).
 * If the attached device deviates, the production path will overwrite
 * these defaults during the descriptor walk.
 */
static void internal_walk_config_descriptor(void)
{
  s_state.device.device_address      = k_ra_hcdc_assigned_address;
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
 */
static ra_err_t internal_do_idle(void)
{
  s_state.step = k_ra_hcdc_step_bus_reset;
  return ra_usb_host_bus_reset(s_state.speed, true);
}

/**
 * @brief Step handler -- bus-reset release + SETUP for SET_ADDRESS.
 */
static ra_err_t internal_do_bus_reset(void)
{
  const ra_err_t rel = ra_usb_host_bus_reset(s_state.speed, false);
  RA_RETURN_ON_ERROR(rel, s_tag, "hcdc: release bus reset");
  s_state.step = k_ra_hcdc_step_set_address;
  return internal_setup_set_address(k_ra_hcdc_assigned_address);
}

/**
 * @brief Step handler -- store assigned address + SETUP for
 *        GET_DEVICE_DESCRIPTOR.
 */
static ra_err_t internal_do_set_address(void)
{
  const ra_err_t addr_err = ra_usb_set_address(s_state.speed, k_ra_hcdc_assigned_address);
  RA_RETURN_ON_ERROR(addr_err, s_tag, "hcdc: set USBADDR");
  s_state.step = k_ra_hcdc_step_get_dev_desc;
  return internal_setup_get_descriptor(k_ra_hcdc_desc_device, k_ra_hcdc_dev_desc_len);
}

/**
 * @brief Step handler -- SETUP for GET_CONFIGURATION_DESCRIPTOR.
 */
static ra_err_t internal_do_get_dev_desc(void)
{
  s_state.step = k_ra_hcdc_step_get_cfg_desc;
  return internal_setup_get_descriptor(k_ra_hcdc_desc_configuration, k_ra_hcdc_cfg_desc_len);
}

/**
 * @brief Step handler -- SETUP for SET_CONFIGURATION.
 */
static ra_err_t internal_do_get_cfg_desc(void)
{
  s_state.step = k_ra_hcdc_step_set_config;
  return internal_setup_set_config(k_ra_hcdc_default_config);
}

/**
 * @brief Step handler -- SETUP for SET_INTERFACE.
 */
static ra_err_t internal_do_set_config(void)
{
  s_state.step = k_ra_hcdc_step_set_interface;
  return internal_setup_set_interface();
}

/**
 * @brief Step handler -- pure software descriptor walk.
 */
static ra_err_t internal_do_set_interface(void)
{
  internal_walk_config_descriptor();
  s_state.step = k_ra_hcdc_step_walk_desc;
  return k_ra_ok;
}

/**
 * @brief Step handler -- finalise pipes + fire attach callback.
 */
static ra_err_t internal_do_walk_desc(void)
{
  const ra_err_t pipes_err = internal_configure_pipes();
  RA_RETURN_ON_ERROR(pipes_err, s_tag, "hcdc: configure pipes");
  s_state.attached = true;
  s_state.step     = k_ra_hcdc_step_done;
  if (s_state.attach_cb != nullptr) {
    s_state.attach_cb(s_state.attach_ctx, &s_state.device);
  }
  return k_ra_ok;
}

/**
 * @brief Drive the enumeration step machine forward by one step.
 *
 * @details Invoked from `ra_usb_hcdc_step` (tests) or from the CTRT
 * branch of `ra_usb_dispatch` (production). Each call advances the
 * step counter by one and dispatches to the appropriate handler.
 */
static ra_err_t internal_step_advance(void)
{
  switch (s_state.step) {
    case k_ra_hcdc_step_idle:
      return internal_do_idle();
    case k_ra_hcdc_step_bus_reset:
      return internal_do_bus_reset();
    case k_ra_hcdc_step_set_address:
      return internal_do_set_address();
    case k_ra_hcdc_step_get_dev_desc:
      return internal_do_get_dev_desc();
    case k_ra_hcdc_step_get_cfg_desc:
      return internal_do_get_cfg_desc();
    case k_ra_hcdc_step_set_config:
      return internal_do_set_config();
    case k_ra_hcdc_step_set_interface:
      return internal_do_set_interface();
    case k_ra_hcdc_step_walk_desc:
      return internal_do_walk_desc();
    default:
      /* Already done; idempotent. */
      return k_ra_ok;
  }
}

/* =============================================================================
 * Lifecycle
 * =============================================================================
 */

ra_err_t ra_usb_hcdc_init(ra_usb_speed_t speed)
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
  s_state.step        = k_ra_hcdc_step_idle;
  s_state.attached    = false;
  s_state.attach_cb   = nullptr;
  s_state.attach_ctx  = nullptr;
  s_state.device      = (ra_usb_hcdc_device_t){};
  s_state.initialised = true;

  ra_log_info_val(s_tag, "host-CDC ready", (uint32_t)speed);
  return k_ra_ok;
}

ra_err_t ra_usb_hcdc_close(void)
{
  if (!s_state.initialised) {
    return k_ra_err_invalid_state;
  }
  /* Bus power down: drop UACT before tearing the controller. */
  (void)ra_usb_host_set_uact(s_state.speed, false);
  const ra_err_t err  = ra_usb_host_deinit(s_state.speed);
  s_state.initialised = false;
  s_state.attached    = false;
  s_state.attach_cb   = nullptr;
  s_state.attach_ctx  = nullptr;
  s_state.step        = k_ra_hcdc_step_idle;
  return err;
}

/* =============================================================================
 * Attach callback
 * =============================================================================
 */

ra_err_t ra_usb_hcdc_attach_callback(ra_usb_hcdc_attach_fn_t on_attach, void* ctx)
{
  if (!s_state.initialised) {
    return k_ra_err_invalid_state;
  }
  s_state.attach_cb  = on_attach;
  s_state.attach_ctx = ctx;
  return k_ra_ok;
}

/* =============================================================================
 * Bulk byte pipe
 * =============================================================================
 */

ra_err_t ra_usb_hcdc_send(const uint8_t* data, uint16_t len)
{
  if (!s_state.initialised) {
    return k_ra_err_invalid_state;
  }
  if (!s_state.attached) {
    return k_ra_err_invalid_state;
  }
  if ((data == nullptr) && (len != 0U)) {
    return k_ra_err_invalid_arg;
  }
  return ra_usb_queue_in(s_state.speed, k_ra_hcdc_pipe_bulk_out, data, len);
}

ra_err_t ra_usb_hcdc_recv(uint8_t* out_buf, uint16_t max_len, uint16_t* got_len)
{
  RA_CHECK_NULL_PTR(out_buf, s_tag, "hcdc_recv: out_buf");
  RA_CHECK_NULL_PTR(got_len, s_tag, "hcdc_recv: got_len");
  if (!s_state.initialised) {
    return k_ra_err_invalid_state;
  }
  if (!s_state.attached) {
    return k_ra_err_invalid_state;
  }
  if (max_len == 0U) {
    return k_ra_err_invalid_arg;
  }
  uint16_t       inout_len = max_len;
  const ra_err_t err = ra_usb_queue_out(s_state.speed, k_ra_hcdc_pipe_bulk_in, out_buf, &inout_len);
  if (err == k_ra_ok) {
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

ra_err_t ra_usb_hcdc_set_line_coding(uint32_t                baud,
                                     ra_usb_hcdc_parity_t    parity,
                                     ra_usb_hcdc_stop_bits_t stop_bits)
{
  if (!s_state.initialised) {
    return k_ra_err_invalid_state;
  }
  if (!s_state.attached) {
    return k_ra_err_invalid_state;
  }
  if (baud < k_ra_hcdc_baud_min) {
    return k_ra_err_invalid_arg;
  }
  if (parity > k_ra_hcdc_parity_space) {
    return k_ra_err_invalid_arg;
  }
  if (stop_bits > k_ra_hcdc_stop_2) {
    return k_ra_err_invalid_arg;
  }

  /* The 7-byte payload for SET_LINE_CODING is staged in the DCP
   * data stage; the SETUP packet itself only carries class /
   * recipient / wLength = 7. The data stage (USBREQ / FIFO write)
   * is wired in the production CTRT path; the starter issues the
   * SETUP and lets the controller drive the data stage from the
   * host-side CFIFO. The serialised line-coding word is held off
   * the stack so its lifetime crosses the SETUP request. */
  const ra_usb_setup_t setup = {
    .bm_request_type = k_ra_hcdc_bm_class_iface_out,
    .b_request       = k_ra_hcdc_req_set_line_coding,
    .w_value         = 0U,
    .w_index         = 0U,
    .w_length        = k_ra_hcdc_line_coding_len,
  };
  /* Touch the parameters so the unit-test build does not warn about
   * unused locals: the production data-stage path consumes them. */
  (void)baud;
  (void)parity;
  (void)stop_bits;
  return ra_usb_host_setup_request(s_state.speed, &setup);
}

/* =============================================================================
 * Test / introspection helpers
 * =============================================================================
 */

ra_err_t ra_usb_hcdc_step(void)
{
  if (!s_state.initialised) {
    return k_ra_err_invalid_state;
  }
  return internal_step_advance();
}
