/**
 * @file ra_usb_pprn.c
 * @brief Native USB device-side Printer class layer implementation
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Glues the device-mode `ra_usb` driver to a USB Printer 1.1 function
 * so the EK-RA8D2 enumerates as a uni-directional or bi-directional
 * printer gadget. This file is the native peripheral-Printer class
 * layer; FSP's `r_usb_pprn_driver.c` is reference material only,
 * nothing is pulled in verbatim.
 *
 * Reference: USB Printer Class 1.1 sec 4.2 "Class Specific Requests"
 * (GET_DEVICE_ID = 0x00, GET_PORT_STATUS = 0x01, SOFT_RESET = 0x02).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_usb_pprn.h"

#include <stdint.h>

#include "ra_check.h"
#include "ra_err.h"
#include "ra_log.h"
#include "ra_usb.h"

static const char* s_tag = "USBPPRN";

/* =============================================================================
 * Internal constants
 * =============================================================================
 */

/**
 * @enum ra_usb_pprn_setup_field_t
 * @brief Constants used to recognise printer class-specific SETUPs.
 *
 * @details Per USB Printer 1.1 sec 4.2 "Class Specific Requests" all
 * printer requests are interface-recipient class envelopes. IN side
 * (0xA1) for GET_DEVICE_ID / GET_PORT_STATUS, OUT side (0x21) for
 * SOFT_RESET (no data).
 */
typedef enum : uint8_t {
  k_ra_pprn_bm_class_iface_in  = 0xA1U, /**< Class | Iface | In.  */
  k_ra_pprn_bm_class_iface_out = 0x21U, /**< Class | Iface | Out. */
} ra_usb_pprn_setup_field_t;

/**
 * @enum ra_usb_pprn_default_status_t
 * @brief Spec defaults the class layer seeds at init.
 *
 * @details "select | not-error" -> printer online, no error,
 * paper-empty bit clear. Per USB Printer 1.1 sec 4.2.2.
 */
typedef enum : uint8_t {
  k_ra_pprn_default_port_status = (uint8_t)((1U << 4U) | (1U << 3U)),
} ra_usb_pprn_default_status_t;

/* =============================================================================
 * Internal state
 * =============================================================================
 */

/**
 * @struct ra_usb_pprn_state_t
 * @brief Singleton shadow state for the device-Printer function.
 */
typedef struct {
  bool                   initialised;     /**< True after init.            */
  ra_usb_speed_t         speed;           /**< Underlying controller.      */
  uint16_t               bulk_max_packet; /**< Pipe max-packet size.       */
  const uint8_t*         desc;            /**< Cached descriptor blob.     */
  uint16_t               desc_len;        /**< Descriptor blob byte len.   */
  const uint8_t*         device_id;       /**< Cached device-ID payload.   */
  uint16_t               device_id_len;   /**< Device-ID byte length.      */
  uint8_t                port_status;     /**< Current port-status byte.   */
  ra_usb_pprn_setup_fn_t setup_cb;        /**< Application class handler.  */
  void*                  setup_ctx;       /**< Class handler context.      */
} ra_usb_pprn_state_t;

static ra_usb_pprn_state_t s_state = {};

/* =============================================================================
 * Internal helpers
 * =============================================================================
 */

/**
 * @brief Pick the bulk-max-packet ceiling matching the negotiated speed.
 */
static uint16_t internal_bulk_max_packet(ra_usb_speed_t speed)
{
  return (speed == k_ra_usb_speed_hs) ? k_ra_pprn_bulk_max_packet_hs : k_ra_pprn_bulk_max_packet_fs;
}

/**
 * @brief Configure the two bulk pipes.
 */
static ra_err_t internal_configure_pipes(ra_usb_speed_t speed)
{
  const uint16_t mp = internal_bulk_max_packet(speed);

  ra_err_t err = ra_usb_configure_endpoint(speed,
                                           k_ra_pprn_pipe_bulk_out,
                                           k_ra_pprn_ep_bulk_out_addr,
                                           k_ra_usb_ep_dir_out,
                                           k_ra_usb_ep_type_bulk,
                                           mp);
  RA_RETURN_ON_ERROR(err, s_tag, "pprn: bulk-out cfg");

  err = ra_usb_configure_endpoint(speed,
                                  k_ra_pprn_pipe_bulk_in,
                                  k_ra_pprn_ep_bulk_in_addr,
                                  k_ra_usb_ep_dir_in,
                                  k_ra_usb_ep_type_bulk,
                                  mp);
  return err;
}

/**
 * @brief Reset shadow state to spec defaults.
 */
static void internal_reset_shadow(ra_usb_speed_t speed)
{
  s_state.speed           = speed;
  s_state.bulk_max_packet = internal_bulk_max_packet(speed);
  s_state.desc            = nullptr;
  s_state.desc_len        = 0U;
  s_state.device_id       = nullptr;
  s_state.device_id_len   = 0U;
  s_state.port_status     = (uint8_t)k_ra_pprn_default_port_status;
  s_state.setup_cb        = nullptr;
  s_state.setup_ctx       = nullptr;
}

/**
 * @brief Recognise a printer class request code we forward.
 */
static bool internal_is_known_class_request(uint8_t b_request)
{
  return (b_request == k_ra_pprn_req_get_device_id) ||
         (b_request == k_ra_pprn_req_get_port_status) || (b_request == k_ra_pprn_req_soft_reset);
}

/* =============================================================================
 * Lifecycle
 * =============================================================================
 */

ra_err_t ra_usb_pprn_init(ra_usb_speed_t speed)
{
  if ((speed != k_ra_usb_speed_fs) && (speed != k_ra_usb_speed_hs)) {
    return k_ra_err_invalid_arg;
  }
  const ra_err_t usb_err = ra_usb_device_init(speed);
  if (usb_err != k_ra_ok) {
    ra_log_error_val(s_tag, "ra_usb_device_init failed", (uint32_t)usb_err);
    return k_ra_err_hw_init_failed;
  }
  internal_reset_shadow(speed);

  const ra_err_t pipes_err = internal_configure_pipes(speed);
  if (pipes_err != k_ra_ok) {
    (void)ra_usb_device_deinit(speed);
    return pipes_err;
  }
  s_state.initialised = true;
  ra_log_info_val(s_tag, "device-Printer ready", (uint32_t)speed);
  return k_ra_ok;
}

ra_err_t ra_usb_pprn_close(void)
{
  if (!s_state.initialised) {
    return k_ra_err_invalid_state;
  }
  (void)ra_usb_device_attach(s_state.speed, false);
  const ra_err_t err  = ra_usb_device_deinit(s_state.speed);
  s_state.initialised = false;
  s_state.desc        = nullptr;
  s_state.device_id   = nullptr;
  s_state.setup_cb    = nullptr;
  s_state.setup_ctx   = nullptr;
  return err;
}

/* =============================================================================
 * Descriptor + identity handoff
 * =============================================================================
 */

ra_err_t ra_usb_pprn_set_descriptors(const uint8_t* desc,
                                     uint16_t       desc_len,
                                     const uint8_t* device_id,
                                     uint16_t       device_id_len)
{
  if (!s_state.initialised) {
    return k_ra_err_invalid_state;
  }
  RA_CHECK_NULL_PTR(desc, s_tag, "set_descriptors: desc");
  if (desc_len == 0U) {
    return k_ra_err_invalid_arg;
  }
  /* device_id is optional, but the (ptr, len) pair must agree. */
  const bool id_ptr_set = (device_id != nullptr);
  const bool id_len_set = (device_id_len != 0U);
  if (id_ptr_set != id_len_set) {
    return k_ra_err_invalid_arg;
  }
  s_state.desc          = desc;
  s_state.desc_len      = desc_len;
  s_state.device_id     = device_id;
  s_state.device_id_len = device_id_len;
  return k_ra_ok;
}

/* =============================================================================
 * Print data transport
 * =============================================================================
 */

ra_err_t ra_usb_pprn_recv(uint8_t* buf, uint16_t max_len, uint16_t* got_len)
{
  RA_CHECK_NULL_PTR(buf, s_tag, "recv: buf");
  RA_CHECK_NULL_PTR(got_len, s_tag, "recv: got_len");
  if (!s_state.initialised) {
    return k_ra_err_invalid_state;
  }
  if (max_len == 0U) {
    return k_ra_err_invalid_arg;
  }
  uint16_t       inout_len = max_len;
  const ra_err_t err = ra_usb_queue_out(s_state.speed, k_ra_pprn_pipe_bulk_out, buf, &inout_len);
  if (err == k_ra_ok) {
    *got_len = inout_len;
  } else {
    *got_len = 0U;
  }
  return err;
}

ra_err_t ra_usb_pprn_send(const uint8_t* data, uint16_t len)
{
  if (!s_state.initialised) {
    return k_ra_err_invalid_state;
  }
  if ((data == nullptr) && (len != 0U)) {
    return k_ra_err_null_ptr;
  }
  if ((len == 0U) || (len > s_state.bulk_max_packet)) {
    return k_ra_err_invalid_arg;
  }
  return ra_usb_queue_in(s_state.speed, k_ra_pprn_pipe_bulk_in, data, len);
}

/* =============================================================================
 * Port-status shadow
 * =============================================================================
 */

ra_err_t ra_usb_pprn_set_port_status(uint8_t status_byte)
{
  if (!s_state.initialised) {
    return k_ra_err_invalid_state;
  }
  s_state.port_status = status_byte;
  return k_ra_ok;
}

ra_err_t ra_usb_pprn_get_port_status(uint8_t* out_status)
{
  RA_CHECK_NULL_PTR(out_status, s_tag, "get_port_status: out_status");
  if (!s_state.initialised) {
    return k_ra_err_invalid_state;
  }
  *out_status = s_state.port_status;
  return k_ra_ok;
}

/* =============================================================================
 * Setup-handler attach
 * =============================================================================
 */

ra_err_t ra_usb_pprn_attach_setup_handler(ra_usb_pprn_setup_fn_t setup_fn, void* ctx)
{
  if (!s_state.initialised) {
    return k_ra_err_invalid_state;
  }
  s_state.setup_cb  = setup_fn;
  s_state.setup_ctx = ctx;
  return k_ra_ok;
}

/* =============================================================================
 * Class SETUP dispatch
 * =============================================================================
 */

ra_err_t ra_usb_pprn_handle_setup(const ra_usb_setup_t* setup)
{
  RA_CHECK_NULL_PTR(setup, s_tag, "handle_setup: setup");
  if (!s_state.initialised) {
    return k_ra_err_invalid_state;
  }
  if ((setup->bm_request_type != k_ra_pprn_bm_class_iface_in) &&
      (setup->bm_request_type != k_ra_pprn_bm_class_iface_out)) {
    return k_ra_err_not_supported;
  }
  if (!internal_is_known_class_request(setup->b_request)) {
    return k_ra_err_not_supported;
  }
  /* SOFT_RESET (USB Printer 1.1 sec 4.2.3) flushes both bulk pipes;
   * implemented here as a no-op shadow refresh so the application can
   * re-arm pipes from its callback if it wants to. */
  if (s_state.setup_cb != nullptr) {
    const ra_err_t cb_err = s_state.setup_cb(s_state.setup_ctx, setup);
    if (cb_err != k_ra_ok) {
      return ra_usb_control_response(s_state.speed, false);
    }
  }
  return ra_usb_control_response(s_state.speed, true);
}
