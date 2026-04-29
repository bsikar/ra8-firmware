/**
 * @file ra_usb_phid.c
 * @brief Native USB device-side HID class layer implementation
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Glues the device-mode `ra_usb` driver to a HID interface so the
 * EK-RA8D2 enumerates as a USB keyboard, mouse, gamepad, or
 * vendor-defined HID gadget. This file is the native peripheral-HID
 * class layer; FSP's `r_usb_phid_driver.c` is reference material only,
 * nothing is pulled in verbatim.
 *
 * Mapping vs FSP (FSP entry point -> our entry point):
 *
 *  - `usb_phid_class_request_init` -> `ra_usb_phid_init`
 *  - `usb_phid_write_complete`     -> `ra_usb_phid_send_report` queue
 *  - `usb_phid_read_complete`      -> `ra_usb_phid_recv_report` drain
 *  - `usb_phid_setup`              -> `ra_usb_phid_handle_setup`
 *  - `R_USB_PeriDeviceInfoGet`     -> `ra_usb_phid_get_*` shadow getters
 *
 * Behaviourally the class layer responds to host requests rather than
 * initiating them: the host enumerates, walks GET_DESCRIPTOR(HID) and
 * GET_DESCRIPTOR(Report) over EP0, may issue SET_IDLE / SET_PROTOCOL,
 * and then keeps polling the interrupt-IN pipe for input reports.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_usb_phid.h"

#include <stdint.h>

#include "ra_check.h"
#include "ra_err.h"
#include "ra_log.h"
#include "ra_usb.h"

static const char* s_tag = "USBPHID";

/* =============================================================================
 * Internal constants
 * =============================================================================
 */

/**
 * @enum ra_usb_phid_setup_field_t
 * @brief Constants used to decode HID class-specific SETUPs.
 *
 * @details Per USB HID 1.11 sec 7.2 "Class-Specific Requests" the
 * standard interface-recipient class envelope uses:
 *   - bmRequestType = 0x21 (H2D | Class | Interface)
 *   - bmRequestType = 0xA1 (D2H | Class | Interface)
 */
typedef enum : uint8_t {
  k_ra_phid_bm_class_iface_in     = 0xA1U, /**< Class | Iface | In.  */
  k_ra_phid_bm_class_iface_out    = 0x21U, /**< Class | Iface | Out. */
  k_ra_phid_default_idle_rate     = 0U,    /**< Spec default idle.   */
  k_ra_phid_default_protocol      = 1U,    /**< Spec default = report. */
  k_ra_phid_report_id_prepend_len = 1U,    /**< Report ID byte.      */
} ra_usb_phid_setup_field_t;

/**
 * @enum ra_usb_phid_byte_shift_t
 * @brief Per-byte shift constants for `wValue` decoding.
 */
typedef enum : uint8_t {
  k_ra_phid_shift_byte0 = 0U,
  k_ra_phid_shift_byte1 = 8U,
} ra_usb_phid_byte_shift_t;

/**
 * @enum ra_usb_phid_byte_mask_t
 * @brief Per-byte mask constants for `wValue` decoding.
 */
typedef enum : uint16_t {
  k_ra_phid_mask_byte = 0xFFU,
} ra_usb_phid_byte_mask_t;

/* =============================================================================
 * Internal state
 * =============================================================================
 */

/**
 * @struct ra_usb_phid_state_t
 * @brief Singleton shadow state for the device-HID function.
 *
 * @details Tracks descriptor pointers, endpoint numbers, idle rate
 * (set by SET_IDLE), protocol (boot vs report, set by SET_PROTOCOL),
 * and the application's class-setup callback. The device-side state
 * machine is intentionally request-driven; there is no enumeration
 * step counter (the host drives that).
 */
typedef struct {
  bool                          initialised;     /**< True after `ra_usb_phid_init`. */
  ra_usb_speed_t                speed;           /**< Underlying controller.        */
  uint8_t                       intr_in_ep;      /**< IN endpoint number.           */
  uint8_t                       intr_out_ep;     /**< OUT endpoint number.          */
  uint16_t                      intr_max_packet; /**< Pipe max-packet size.         */
  const uint8_t*                report_desc;     /**< Cached Report descriptor.     */
  uint16_t                      report_desc_len; /**< Report descriptor byte len.   */
  const uint8_t*                hid_desc;        /**< Cached HID class descriptor.  */
  uint16_t                      hid_desc_len;    /**< HID class descriptor byte len.*/
  uint8_t                       idle_rate;       /**< 4 ms ticks (0 = on-change).   */
  ra_usb_phid_protocol_select_t protocol;        /**< Boot vs report.               */
  ra_usb_phid_setup_fn_t        setup_cb;        /**< Application class handler.    */
  void*                         setup_ctx;       /**< Class handler context.        */
} ra_usb_phid_state_t;

static ra_usb_phid_state_t s_state = {};

/* =============================================================================
 * Internal helpers
 * =============================================================================
 */

/**
 * @brief Pick the interrupt-max-packet ceiling matching the negotiated
 *        speed.
 */
static uint16_t internal_intr_max_packet(ra_usb_speed_t speed)
{
  return (speed == k_ra_usb_speed_hs) ? k_ra_phid_intr_max_packet_hs
                                      : k_ra_phid_intr_max_packet_default;
}

/**
 * @brief Configure the two HID interrupt pipes.
 */
static ra_err_t internal_configure_pipes(ra_usb_speed_t speed)
{
  const uint16_t mp = internal_intr_max_packet(speed);

  ra_err_t err = ra_usb_configure_endpoint(speed,
                                           k_ra_phid_pipe_intr_in,
                                           k_ra_phid_ep_intr_in_addr,
                                           k_ra_usb_ep_dir_in,
                                           k_ra_usb_ep_type_intr,
                                           mp);
  RA_RETURN_ON_ERROR(err, s_tag, "phid: intr-in cfg");

  err = ra_usb_configure_endpoint(speed,
                                  k_ra_phid_pipe_intr_out,
                                  k_ra_phid_ep_intr_out_addr,
                                  k_ra_usb_ep_dir_out,
                                  k_ra_usb_ep_type_intr,
                                  mp);
  return err;
}

/**
 * @brief Reset shadow state to spec defaults.
 */
static void internal_reset_shadow(ra_usb_speed_t speed)
{
  s_state.speed           = speed;
  s_state.intr_in_ep      = k_ra_phid_ep_intr_in_addr;
  s_state.intr_out_ep     = k_ra_phid_ep_intr_out_addr;
  s_state.intr_max_packet = internal_intr_max_packet(speed);
  s_state.report_desc     = nullptr;
  s_state.report_desc_len = 0U;
  s_state.hid_desc        = nullptr;
  s_state.hid_desc_len    = 0U;
  s_state.idle_rate       = k_ra_phid_default_idle_rate;
  s_state.protocol        = k_ra_phid_proto_report;
  s_state.setup_cb        = nullptr;
  s_state.setup_ctx       = nullptr;
}

/**
 * @brief Recognise a HID class request code we shadow / forward.
 */
static bool internal_is_known_class_request(uint8_t b_request)
{
  return (b_request == k_ra_phid_req_get_report) || (b_request == k_ra_phid_req_set_report) ||
         (b_request == k_ra_phid_req_get_idle) || (b_request == k_ra_phid_req_set_idle) ||
         (b_request == k_ra_phid_req_get_protocol) || (b_request == k_ra_phid_req_set_protocol);
}

/**
 * @brief Apply SET_IDLE / SET_PROTOCOL to the local shadow.
 *
 * @details `wValue.high` carries the duration for SET_IDLE; `wValue`
 * carries 0 (boot) or 1 (report) for SET_PROTOCOL.
 */
static void internal_apply_class_setup(const ra_usb_setup_t* setup)
{
  switch (setup->b_request) {
    case k_ra_phid_req_set_idle: {
      const uint8_t duration =
        (uint8_t)((setup->w_value >> k_ra_phid_shift_byte1) & k_ra_phid_mask_byte);
      s_state.idle_rate = duration;
      break;
    }
    case k_ra_phid_req_set_protocol: {
      if (setup->w_value == (uint16_t)k_ra_phid_proto_boot) {
        s_state.protocol = k_ra_phid_proto_boot;
      } else if (setup->w_value == (uint16_t)k_ra_phid_proto_report) {
        s_state.protocol = k_ra_phid_proto_report;
      } else {
        /* Unknown selector -- leave shadow untouched. */
      }
      break;
    }
    default: {
      /* GET_REPORT / GET_IDLE / GET_PROTOCOL / SET_REPORT have payloads
       * the application owns; defer to the registered callback. */
      break;
    }
  }
}

/* =============================================================================
 * Lifecycle
 * =============================================================================
 */

ra_err_t ra_usb_phid_init(ra_usb_speed_t speed)
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
  ra_log_info_val(s_tag, "device-HID ready", (uint32_t)speed);
  return k_ra_ok;
}

ra_err_t ra_usb_phid_close(void)
{
  if (!s_state.initialised) {
    return k_ra_err_invalid_state;
  }
  (void)ra_usb_device_attach(s_state.speed, false);
  const ra_err_t err  = ra_usb_device_deinit(s_state.speed);
  s_state.initialised = false;
  s_state.report_desc = nullptr;
  s_state.hid_desc    = nullptr;
  s_state.setup_cb    = nullptr;
  s_state.setup_ctx   = nullptr;
  return err;
}

/* =============================================================================
 * Descriptor handoff
 * =============================================================================
 */

ra_err_t ra_usb_phid_set_descriptors(const uint8_t* report_desc,
                                     uint16_t       report_desc_len,
                                     const uint8_t* hid_desc,
                                     uint16_t       hid_desc_len)
{
  if (!s_state.initialised) {
    return k_ra_err_invalid_state;
  }
  RA_CHECK_NULL_PTR(report_desc, s_tag, "set_descriptors: report_desc");
  RA_CHECK_NULL_PTR(hid_desc, s_tag, "set_descriptors: hid_desc");
  if ((report_desc_len == 0U) || (hid_desc_len == 0U)) {
    return k_ra_err_invalid_arg;
  }
  s_state.report_desc     = report_desc;
  s_state.report_desc_len = report_desc_len;
  s_state.hid_desc        = hid_desc;
  s_state.hid_desc_len    = hid_desc_len;
  return k_ra_ok;
}

/* =============================================================================
 * Input / output reports
 * =============================================================================
 */

ra_err_t ra_usb_phid_send_report(uint8_t report_id, const uint8_t* payload, uint16_t len)
{
  if (!s_state.initialised) {
    return k_ra_err_invalid_state;
  }
  if ((payload == nullptr) && (len != 0U)) {
    return k_ra_err_null_ptr;
  }
  if ((report_id == 0U) && (len == 0U)) {
    return k_ra_err_invalid_arg;
  }
  /* Per USB HID 1.11 sec 8 "Report Protocol", multi-report devices
   * prepend the report ID. The combined frame must still fit the
   * configured pipe max-packet. */
  const uint16_t framed_len =
    (report_id != 0U) ? (uint16_t)(len + (uint16_t)k_ra_phid_report_id_prepend_len) : len;
  if (framed_len > s_state.intr_max_packet) {
    return k_ra_err_invalid_arg;
  }

  if (report_id != 0U) {
    /* Prepend the report ID byte: queue it on its own first, then the
     * payload. The controller's FIFO write coalesces both into a
     * single interrupt-IN packet on the next IN token. */
    const uint8_t  rid_byte = report_id;
    const ra_err_t rid_err  = ra_usb_queue_in(s_state.speed, k_ra_phid_pipe_intr_in, &rid_byte, 1U);
    RA_RETURN_ON_ERROR(rid_err, s_tag, "send_report: rid byte");
  }
  return ra_usb_queue_in(s_state.speed, k_ra_phid_pipe_intr_in, payload, len);
}

ra_err_t
ra_usb_phid_recv_report(uint8_t report_id, uint8_t* buf, uint16_t max_len, uint16_t* got_len)
{
  RA_CHECK_NULL_PTR(buf, s_tag, "recv_report: buf");
  RA_CHECK_NULL_PTR(got_len, s_tag, "recv_report: got_len");
  if (!s_state.initialised) {
    return k_ra_err_invalid_state;
  }
  if (max_len == 0U) {
    return k_ra_err_invalid_arg;
  }
  /* `report_id` is informational here -- the HID Report descriptor
   * declares which IDs the device recognises; the host transmits the
   * corresponding payload on PIPE7 directly. */
  (void)report_id;

  uint16_t       inout_len = max_len;
  const ra_err_t err = ra_usb_queue_out(s_state.speed, k_ra_phid_pipe_intr_out, buf, &inout_len);
  if (err == k_ra_ok) {
    *got_len = inout_len;
  } else {
    *got_len = 0U;
  }
  return err;
}

/* =============================================================================
 * Setup-handler attach
 * =============================================================================
 */

ra_err_t ra_usb_phid_attach_setup_handler(ra_usb_phid_setup_fn_t setup_fn, void* ctx)
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

ra_err_t ra_usb_phid_handle_setup(const ra_usb_setup_t* setup)
{
  RA_CHECK_NULL_PTR(setup, s_tag, "handle_setup: setup");
  if (!s_state.initialised) {
    return k_ra_err_invalid_state;
  }
  if ((setup->bm_request_type != k_ra_phid_bm_class_iface_in) &&
      (setup->bm_request_type != k_ra_phid_bm_class_iface_out)) {
    return k_ra_err_not_supported;
  }
  if (!internal_is_known_class_request(setup->b_request)) {
    return k_ra_err_not_supported;
  }
  internal_apply_class_setup(setup);

  /* Forward to the application handler if one is registered. The
   * application decides whether to ACK or stall via its return value. */
  if (s_state.setup_cb != nullptr) {
    const ra_err_t cb_err = s_state.setup_cb(s_state.setup_ctx, setup);
    if (cb_err != k_ra_ok) {
      return ra_usb_control_response(s_state.speed, false);
    }
  }
  return ra_usb_control_response(s_state.speed, true);
}

/* =============================================================================
 * Introspection
 * =============================================================================
 */

ra_err_t ra_usb_phid_get_idle(uint8_t* out_idle_rate)
{
  RA_CHECK_NULL_PTR(out_idle_rate, s_tag, "get_idle: out_idle_rate");
  if (!s_state.initialised) {
    return k_ra_err_invalid_state;
  }
  *out_idle_rate = s_state.idle_rate;
  return k_ra_ok;
}

ra_err_t ra_usb_phid_get_protocol(ra_usb_phid_protocol_select_t* out_protocol)
{
  RA_CHECK_NULL_PTR(out_protocol, s_tag, "get_protocol: out_protocol");
  if (!s_state.initialised) {
    return k_ra_err_invalid_state;
  }
  *out_protocol = s_state.protocol;
  return k_ra_ok;
}
