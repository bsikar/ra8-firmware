/**
 * @file ra_usb_pvnd.c
 * @brief Native USB device-side Vendor-defined class layer implementation
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Glues the device-mode `ra_usb` driver to a vendor-defined function
 * (class 0xFF) so the EK-RA8D2 can carry application-specific
 * protocols over raw bulk pipes. This file is the native peripheral-
 * Vendor class layer; FSP's `r_usb_vendor_descriptor.c.template` is
 * reference material only.
 *
 * Reference: USB 2.0 sec 9.3 "USB Device Requests" (vendor request
 * envelope encoding).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_usb_pvnd.h"

#include <stdint.h>

#include "ra_check.h"
#include "ra_err.h"
#include "ra_log.h"
#include "ra_usb.h"

static const char* s_tag = "USBPVND";

/* =============================================================================
 * Internal state
 * =============================================================================
 */

/**
 * @struct ra_usb_pvnd_state_t
 * @brief Singleton shadow state for the device-Vendor function.
 */
typedef struct {
  bool                   initialized;     /**< True after init.           */
  ra_usb_speed_t         speed;           /**< Underlying controller.     */
  uint16_t               bulk_max_packet; /**< Pipe max-packet size.      */
  const uint8_t*         desc;            /**< Cached descriptor blob.    */
  uint16_t               desc_len;        /**< Descriptor blob byte len.  */
  ra_usb_pvnd_setup_fn_t setup_cb;        /**< Application class handler. */
  void*                  setup_ctx;       /**< Class handler context.     */
} ra_usb_pvnd_state_t;

static ra_usb_pvnd_state_t s_state = {};

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
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static uint16_t internal_bulk_max_packet(ra_usb_speed_t speed)
{
  return (speed == k_ra_usb_speed_hs) ? k_ra_pvnd_bulk_max_packet_hs : k_ra_pvnd_bulk_max_packet_fs;
}

/**
 * @brief Configure the two bulk pipes.
 *
 * @details See implementation.
 * @param[in] speed See implementation.
 * @return Result code.
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static ra_err_t internal_configure_pipes(ra_usb_speed_t speed)
{
  const uint16_t mp = internal_bulk_max_packet(speed);

  ra_err_t err = ra_usb_configure_endpoint(speed,
                                           k_ra_pvnd_pipe_bulk_in,
                                           k_ra_pvnd_ep_bulk_in_addr,
                                           k_ra_usb_ep_dir_in,
                                           k_ra_usb_ep_type_bulk,
                                           mp);
  RA_RETURN_ON_ERROR(err, s_tag, "pvnd: bulk-in cfg"); /* GCOVR_EXCL_BR_LINE */

  err = ra_usb_configure_endpoint(speed,
                                  k_ra_pvnd_pipe_bulk_out,
                                  k_ra_pvnd_ep_bulk_out_addr,
                                  k_ra_usb_ep_dir_out,
                                  k_ra_usb_ep_type_bulk,
                                  mp);
  return err;
}

/**
 * @brief Reset shadow state to spec defaults.
 *
 * @details See implementation.
 * @param[in] speed See implementation.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static void internal_reset_shadow(ra_usb_speed_t speed)
{
  s_state.speed           = speed;
  s_state.bulk_max_packet = internal_bulk_max_packet(speed);
  s_state.desc            = nullptr;
  s_state.desc_len        = 0U;
  s_state.setup_cb        = nullptr;
  s_state.setup_ctx       = nullptr;
}

/**
 * @brief Recognise a vendor-recipient SETUP envelope.
 *
 * @details Per USB 2.0 sec 9.3 "USB Device Requests", the type field
 * is bits 6:5 of `bmRequestType`. 0b10 = vendor.
 *
 * @param[in] bm See implementation.
 * @return Result code.
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static bool internal_is_vendor_envelope(uint8_t bm)
{
  return (bm == k_ra_pvnd_bm_vendor_dev_in) || (bm == k_ra_pvnd_bm_vendor_dev_out) ||
         (bm == k_ra_pvnd_bm_vendor_iface_in) || (bm == k_ra_pvnd_bm_vendor_iface_out) ||
         (bm == k_ra_pvnd_bm_vendor_ep_in) || (bm == k_ra_pvnd_bm_vendor_ep_out);
}

/* =============================================================================
 * Lifecycle
 * =============================================================================
 */

ra_err_t ra_usb_pvnd_init(ra_usb_speed_t speed)
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
  s_state.initialized = true;
  ra_log_info_val(s_tag, "device-Vendor ready", (uint32_t)speed);
  return k_ra_ok;
}

ra_err_t ra_usb_pvnd_close(void)
{
  if (!s_state.initialized) {
    return k_ra_err_invalid_state;
  }
  (void)ra_usb_device_attach(s_state.speed, false);
  const ra_err_t err  = ra_usb_device_deinit(s_state.speed);
  s_state.initialized = false;
  s_state.desc        = nullptr;
  s_state.setup_cb    = nullptr;
  s_state.setup_ctx   = nullptr;
  return err;
}

/* =============================================================================
 * Descriptor handoff
 * =============================================================================
 */

ra_err_t ra_usb_pvnd_set_descriptors(const uint8_t* desc, uint16_t desc_len)
{
  if (!s_state.initialized) {
    return k_ra_err_invalid_state;
  }
  RA_CHECK_NULL_PTR(desc, s_tag, "set_descriptors: desc");
  if (desc_len == 0U) {
    return k_ra_err_invalid_arg;
  }
  s_state.desc     = desc;
  s_state.desc_len = desc_len;
  return k_ra_ok;
}

/* =============================================================================
 * Vendor data transport
 * =============================================================================
 */

ra_err_t ra_usb_pvnd_send(const uint8_t* data, uint16_t len)
{
  if (!s_state.initialized) {
    return k_ra_err_invalid_state;
  }
  if ((data == nullptr) && (len != 0U)) {
    return k_ra_err_null_ptr;
  }
  if ((len == 0U) || (len > s_state.bulk_max_packet)) {
    return k_ra_err_invalid_arg;
  }
  return ra_usb_queue_in(s_state.speed, k_ra_pvnd_pipe_bulk_in, data, len);
}

ra_err_t ra_usb_pvnd_recv(uint8_t* buf, uint16_t max_len, uint16_t* got_len)
{
  RA_CHECK_NULL_PTR(buf, s_tag, "recv: buf");
  RA_CHECK_NULL_PTR(got_len, s_tag, "recv: got_len");
  if (!s_state.initialized) {
    return k_ra_err_invalid_state;
  }
  if (max_len == 0U) {
    return k_ra_err_invalid_arg;
  }
  uint16_t       inout_len = max_len;
  const ra_err_t err =
    ra_usb_queue_out(s_state.speed, k_ra_pvnd_pipe_bulk_out, buf, &inout_len, true);
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

ra_err_t ra_usb_pvnd_attach_setup_handler(ra_usb_pvnd_setup_fn_t setup_fn, void* ctx)
{
  if (!s_state.initialized) {
    return k_ra_err_invalid_state;
  }
  s_state.setup_cb  = setup_fn;
  s_state.setup_ctx = ctx;
  return k_ra_ok;
}

/* =============================================================================
 * Vendor SETUP dispatch
 * =============================================================================
 */

ra_err_t ra_usb_pvnd_handle_setup(const ra_usb_setup_t* setup)
{
  RA_CHECK_NULL_PTR(setup, s_tag, "handle_setup: setup");
  if (!s_state.initialized) {
    return k_ra_err_invalid_state;
  }
  if (!internal_is_vendor_envelope(setup->bm_request_type)) {
    return k_ra_err_not_supported;
  }
  if (s_state.setup_cb == nullptr) {
    /* No application handler -> stall the unknown vendor request. */
    return ra_usb_control_response(s_state.speed, false);
  }
  const ra_err_t cb_err = s_state.setup_cb(s_state.setup_ctx, setup);
  if (cb_err != k_ra_ok) {
    return ra_usb_control_response(s_state.speed, false);
  }
  return ra_usb_control_response(s_state.speed, true);
}
