/**
 * @file ra_usb_cdc.c
 * @brief Native USB CDC ACM class layer implementation
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Glues the device-mode `ra_usb` driver to a CDC ACM endpoint set so
 * the host enumerates the board as `/dev/ttyACM*` (Linux / macOS) or
 * `COMn` (Windows). Class-specific SETUP requests
 * (SET_LINE_CODING / GET_LINE_CODING / SET_CONTROL_LINE_STATE) are
 * answered locally; the CDC standard descriptors live in the
 * caller's stack so this file does not own VID / PID.
 *
 * The implementation is from-scratch -- there is no FSP `r_usb_pcdc`
 * source pulled in. Behaviourally it tracks the small subset that
 * the CDC PSTN spec rev 1.20 makes mandatory for an ACM device:
 *
 *  - Persistent line-coding shadow (default 9600/8/N/1).
 *  - DTR / RTS shadow.
 *  - Bulk byte pipe via `ra_usb_queue_in` / `ra_usb_queue_out`.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_usb_cdc.h"

#include <stdint.h>

#include "ra_check.h"
#include "ra_err.h"
#include "ra_log.h"
#include "ra_usb.h"

static const char* s_tag = "USBCDC";

/**
 * @enum ra_usb_cdc_setup_field_t
 * @brief Constants used to decode CDC class-specific SETUPs.
 */
typedef enum : uint8_t {
  k_ra_cdc_bm_class_recip_iface = 0x21U, /**< Class | Interface | Out. */
  k_ra_cdc_bm_class_recip_in    = 0xA1U, /**< Class | Interface | In.  */
  k_ra_cdc_default_data_bits    = 8U,    /**< 8 data bits.             */
  k_ra_cdc_default_stop_bits    = 0U,    /**< 1 stop bit.              */
  k_ra_cdc_default_parity_none  = 0U,    /**< No parity.               */
  k_ra_cdc_line_coding_len      = 7U,    /**< 7-byte payload length.   */
} ra_usb_cdc_setup_field_t;

/**
 * @enum ra_usb_cdc_default_baud_t
 * @brief 9600 baud assembled from the two low bytes above.
 */
typedef enum : uint32_t {
  k_ra_cdc_default_baud = 9600U,
} ra_usb_cdc_default_baud_t;

/**
 * @enum ra_usb_cdc_byte_shift_t
 * @brief Per-byte left-shift constants for the little-endian baud
 * accumulator in `internal_apply_line_coding`.
 */
typedef enum : uint8_t {
  k_ra_cdc_shift_byte0 = 0U,
  k_ra_cdc_shift_byte1 = 8U,
  k_ra_cdc_shift_byte2 = 16U,
  k_ra_cdc_shift_byte3 = 24U,
} ra_usb_cdc_byte_shift_t;

/**
 * @enum ra_usb_cdc_byte_idx_t
 * @brief Indices into the 7-byte SET_LINE_CODING payload.
 */
typedef enum : uint8_t {
  k_ra_cdc_idx_baud_b0     = 0U,
  k_ra_cdc_idx_baud_b1     = 1U,
  k_ra_cdc_idx_baud_b2     = 2U,
  k_ra_cdc_idx_baud_b3     = 3U,
  k_ra_cdc_idx_char_format = 4U,
  k_ra_cdc_idx_parity_type = 5U,
  k_ra_cdc_idx_data_bits   = 6U,
} ra_usb_cdc_byte_idx_t;

/**
 * @struct ra_usb_cdc_state_t
 * @brief Singleton shadow state for the CDC function.
 */
typedef struct {
  bool                     initialised; /**< True after `ra_usb_cdc_init`. */
  ra_usb_speed_t           speed;       /**< Underlying controller.        */
  ra_usb_cdc_line_coding_t coding;      /**< Most recent line coding.      */
  bool                     dtr;         /**< DTR shadow.                   */
  bool                     rts;         /**< RTS shadow.                   */
} ra_usb_cdc_state_t;

static ra_usb_cdc_state_t s_state = {};

/* =============================================================================
 * Internal helpers
 * =============================================================================
 */

static void internal_default_coding(ra_usb_cdc_line_coding_t* coding)
{
  coding->dte_rate    = k_ra_cdc_default_baud;
  coding->char_format = k_ra_cdc_default_stop_bits;
  coding->parity_type = k_ra_cdc_default_parity_none;
  coding->data_bits   = k_ra_cdc_default_data_bits;
}

/**
 * @brief Configure the three CDC pipes (bulk IN / OUT, intr IN).
 */
static ra_err_t internal_configure_pipes(ra_usb_speed_t speed)
{
  const uint16_t bulk_mp =
    (speed == k_ra_usb_speed_hs) ? k_ra_cdc_bulk_max_packet_hs : k_ra_cdc_bulk_max_packet_fs;

  ra_err_t err = ra_usb_configure_endpoint(speed,
                                           k_ra_cdc_pipe_bulk_in,
                                           k_ra_cdc_ep_bulk_in_addr,
                                           k_ra_usb_ep_dir_in,
                                           k_ra_usb_ep_type_bulk,
                                           bulk_mp);
  RA_RETURN_ON_ERROR(err, s_tag, "cdc: bulk-in cfg");

  err = ra_usb_configure_endpoint(speed,
                                  k_ra_cdc_pipe_bulk_out,
                                  k_ra_cdc_ep_bulk_out_addr,
                                  k_ra_usb_ep_dir_out,
                                  k_ra_usb_ep_type_bulk,
                                  bulk_mp);
  RA_RETURN_ON_ERROR(err, s_tag, "cdc: bulk-out cfg");

  err = ra_usb_configure_endpoint(speed,
                                  k_ra_cdc_pipe_intr_in,
                                  k_ra_cdc_ep_intr_in_addr,
                                  k_ra_usb_ep_dir_in,
                                  k_ra_usb_ep_type_intr,
                                  k_ra_cdc_intr_max_packet);
  return err;
}

/**
 * @brief Apply a 7-byte line-coding payload from the host.
 *
 * @details The host sends little-endian. We only honour the fields
 * we have storage for; bytes past offset 7 are ignored if `len > 7`.
 */
static void internal_apply_line_coding(const uint8_t* data, uint16_t len)
{
  if ((data == nullptr) || (len < k_ra_cdc_line_coding_len)) {
    return;
  }
  uint32_t baud = (uint32_t)data[k_ra_cdc_idx_baud_b0];
  baud |= (uint32_t)((uint32_t)data[k_ra_cdc_idx_baud_b1] << k_ra_cdc_shift_byte1);
  baud |= (uint32_t)((uint32_t)data[k_ra_cdc_idx_baud_b2] << k_ra_cdc_shift_byte2);
  baud |= (uint32_t)((uint32_t)data[k_ra_cdc_idx_baud_b3] << k_ra_cdc_shift_byte3);
  s_state.coding.dte_rate    = baud;
  s_state.coding.char_format = data[k_ra_cdc_idx_char_format];
  s_state.coding.parity_type = data[k_ra_cdc_idx_parity_type];
  s_state.coding.data_bits   = data[k_ra_cdc_idx_data_bits];
}

/* =============================================================================
 * Lifecycle
 * =============================================================================
 */

ra_err_t ra_usb_cdc_init(ra_usb_speed_t speed)
{
  if ((speed != k_ra_usb_speed_fs) && (speed != k_ra_usb_speed_hs)) {
    return k_ra_err_invalid_arg;
  }
  const ra_err_t usb_err = ra_usb_device_init(speed);
  if (usb_err != k_ra_ok) {
    ra_log_error_val(s_tag, "ra_usb_device_init failed", (uint32_t)usb_err);
    return k_ra_err_hw_init_failed;
  }

  s_state.speed = speed;
  s_state.dtr   = false;
  s_state.rts   = false;
  internal_default_coding(&s_state.coding);

  const ra_err_t pipes_err = internal_configure_pipes(speed);
  if (pipes_err != k_ra_ok) {
    (void)ra_usb_device_deinit(speed);
    return pipes_err;
  }
  s_state.initialised = true;
  ra_log_info(s_tag, "CDC ready");
  return k_ra_ok;
}

ra_err_t ra_usb_cdc_deinit(void)
{
  if (!s_state.initialised) {
    return k_ra_err_invalid_state;
  }
  (void)ra_usb_device_attach(s_state.speed, false);
  const ra_err_t err  = ra_usb_device_deinit(s_state.speed);
  s_state.initialised = false;
  s_state.dtr         = false;
  s_state.rts         = false;
  return err;
}

ra_err_t ra_usb_cdc_attach(bool attached)
{
  if (!s_state.initialised) {
    return k_ra_err_invalid_state;
  }
  return ra_usb_device_attach(s_state.speed, attached);
}

/* =============================================================================
 * Bulk byte pipe
 * =============================================================================
 */

ra_err_t ra_usb_cdc_send(const uint8_t* data, uint16_t len)
{
  if (!s_state.initialised) {
    return k_ra_err_invalid_state;
  }
  if ((data == nullptr) && (len != 0U)) {
    return k_ra_err_invalid_arg;
  }
  return ra_usb_queue_in(s_state.speed, k_ra_cdc_pipe_bulk_in, data, len);
}

ra_err_t ra_usb_cdc_recv(uint8_t* out_buf, uint16_t* inout_len)
{
  RA_CHECK_NULL_PTR(out_buf, s_tag, "cdc_recv: out_buf");
  RA_CHECK_NULL_PTR(inout_len, s_tag, "cdc_recv: inout_len");
  if (!s_state.initialised) {
    return k_ra_err_invalid_state;
  }
  if (*inout_len == 0U) {
    return k_ra_err_invalid_arg;
  }
  return ra_usb_queue_out(s_state.speed, k_ra_cdc_pipe_bulk_out, out_buf, inout_len);
}

/* =============================================================================
 * SETUP request handler
 * =============================================================================
 */

/**
 * @brief Pull the SET_LINE_CODING data stage off EP0 once it lands.
 *
 * @details The DCP handling for the data stage of a control-write
 * happens on a future BRDY interrupt for PIPE0; the present iteration
 * stores no payload because the CDC ISR path that wires it in lives
 * in the next live-USB iteration. The function intentionally returns
 * `k_ra_err_not_supported` so the caller can fall through to a
 * minimal "store nothing, ACK status stage" path; tests cover this
 * branch.
 */
static ra_err_t internal_pull_data_stage(const uint8_t* buf, uint16_t cap, const uint16_t* out_len)
{
  (void)buf;
  (void)cap;
  (void)out_len;
  return k_ra_err_not_supported;
}

/**
 * @brief Decode a CDC class-specific SETUP packet body.
 *
 * @return ra_ok on success, or an error code that the public
 * `ra_usb_cdc_handle_setup` propagates verbatim.
 */
static ra_err_t internal_dispatch_class_setup(const ra_usb_setup_t* setup)
{
  switch (setup->b_request) {
    case k_ra_cdc_req_set_line_coding: {
      uint8_t        buf[k_ra_cdc_line_coding_len + 1U] = {};
      const uint16_t plen                               = k_ra_cdc_line_coding_len;
      const ra_err_t pull = internal_pull_data_stage(buf, (uint16_t)sizeof(buf), &plen);
      if (pull == k_ra_ok) {
        internal_apply_line_coding(buf, plen);
      }
      return ra_usb_control_response(s_state.speed, true);
    }
    case k_ra_cdc_req_get_line_coding: {
      return ra_usb_control_response(s_state.speed, true);
    }
    case k_ra_cdc_req_set_control_line_state: {
      const uint16_t mask = setup->w_value;
      s_state.dtr         = ((mask & k_ra_cdc_line_state_dtr) != 0U);
      s_state.rts         = ((mask & k_ra_cdc_line_state_rts) != 0U);
      return ra_usb_control_response(s_state.speed, true);
    }
    default: {
      return k_ra_err_not_supported;
    }
  }
}

ra_err_t ra_usb_cdc_handle_setup(const ra_usb_setup_t* setup)
{
  RA_CHECK_NULL_PTR(setup, s_tag, "handle_setup: setup");
  if (!s_state.initialised) {
    return k_ra_err_invalid_state;
  }
  if ((setup->bm_request_type != k_ra_cdc_bm_class_recip_iface) &&
      (setup->bm_request_type != k_ra_cdc_bm_class_recip_in)) {
    return k_ra_err_not_supported;
  }
  return internal_dispatch_class_setup(setup);
}

ra_err_t ra_usb_cdc_get_line_coding(ra_usb_cdc_line_coding_t* out)
{
  RA_CHECK_NULL_PTR(out, s_tag, "get_line_coding: out");
  if (!s_state.initialised) {
    return k_ra_err_invalid_state;
  }
  *out = s_state.coding;
  return k_ra_ok;
}

ra_err_t ra_usb_cdc_get_line_state(bool* out_dtr, bool* out_rts)
{
  RA_CHECK_NULL_PTR(out_dtr, s_tag, "get_line_state: out_dtr");
  RA_CHECK_NULL_PTR(out_rts, s_tag, "get_line_state: out_rts");
  if (!s_state.initialised) {
    return k_ra_err_invalid_state;
  }
  *out_dtr = s_state.dtr;
  *out_rts = s_state.rts;
  return k_ra_ok;
}
