/**
 * @file ra_usb_pal.c
 * @brief USB PAL implementation -- ra_usb wrapper
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details
 * Wave 7.2 scaffold. Implements the ``ra_usb_pal_*`` API by
 * wrapping the Ring-3 ``ra_usb`` driver. Endpoint I/O bottoms
 * out in ``k_ra_err_not_supported`` until ra_usb gains pipe
 * primitives (Wave 7.2b).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_usb_pal.h"

#include <stdint.h>

#include "ra_check.h"
#include "ra_err.h"
#include "ra_log.h"
#include "ra_usb.h"

static const char* s_tag = "USBPAL";

/* =============================================================================
 * Per-PAL state (one instance -- chip has FS + HS but only one is active)
 * =============================================================================
 */

/**
 * @struct ra_usb_pal_state_inner_t
 * @brief Singleton PAL state.
 */
typedef struct {
  ra_usb_speed_t        speed;       /**< FS or HS, set at init.       */
  ra_usb_pal_state_t    state;       /**< Detached/attached/...        */
  ra_usb_pal_event_fn_t event_fn;    /**< Stack-installed callback.    */
  void*                 event_ctx;   /**< Callback context.            */
  bool                  initialised; /**< True after ra_usb_pal_init. */
} ra_usb_pal_state_inner_t;

static ra_usb_pal_state_inner_t s_state = {};

/* =============================================================================
 * Internal helpers
 * =============================================================================
 */

/**
 * @brief Translate ra_usb status mask -> PAL event mask.
 */
static uint16_t internal_translate(uint16_t usb_mask)
{
  /* Wave 7.2: ra_usb only exposes a generic INTSTS0 mask; bit
   * assignments come with the descriptor-ring work in 7.2b. Until
   * then translate "any non-zero" into "error" so the PAL has a
   * defined contract. */
  if (usb_mask != 0U) {
    return (uint16_t)k_ra_usb_pal_event_error;
  }
  return (uint16_t)k_ra_usb_pal_event_none;
}

/**
 * @brief ra_usb event handler -- translate + forward to the PAL callback.
 */
static void internal_usb_event(void* ctx, ra_usb_speed_t speed, uint16_t status_mask)
{
  (void)ctx;
  if (!s_state.initialised) {
    return;
  }
  if (speed != s_state.speed) {
    return;
  }
  const uint16_t pal_mask = internal_translate(status_mask);
  if ((s_state.event_fn != nullptr) && (pal_mask != (uint16_t)k_ra_usb_pal_event_none)) {
    s_state.event_fn(s_state.event_ctx, speed, pal_mask);
  }
}

/* =============================================================================
 * Public API
 * =============================================================================
 */

ra_err_t ra_usb_pal_init(ra_usb_speed_t speed)
{
  if ((speed != k_ra_usb_speed_fs) && (speed != k_ra_usb_speed_hs)) {
    return k_ra_err_invalid_arg;
  }
  const ra_err_t usb_err = ra_usb_device_init(speed);
  if (usb_err != k_ra_ok) {
    ra_log_error_val(s_tag, "ra_usb_device_init failed", (uint32_t)usb_err);
    return k_ra_err_hw_init_failed;
  }

  s_state.speed       = speed;
  s_state.state       = k_ra_usb_pal_state_detached;
  s_state.event_fn    = nullptr;
  s_state.event_ctx   = nullptr;
  s_state.initialised = true;

  /* Wire ra_usb dispatch into the PAL translator. */
  const ra_err_t att_err = ra_usb_attach_handler(internal_usb_event, nullptr);
  if (att_err != k_ra_ok) { /* GCOVR_EXCL_BR_LINE */
    /* GCOVR_EXCL_START */
    ra_log_error_val(s_tag, "ra_usb_attach_handler failed", (uint32_t)att_err);
    s_state.initialised = false;
    (void)ra_usb_device_deinit(speed);
    return k_ra_err_hw_init_failed;
    /* GCOVR_EXCL_STOP */
  }

  ra_log_info(s_tag, "PAL ready");
  return k_ra_ok;
}

ra_err_t ra_usb_pal_deinit(void)
{
  if (!s_state.initialised) {
    return k_ra_err_invalid_state;
  }
  /* Drop the pull-up first to detach cleanly. */
  (void)ra_usb_device_attach(s_state.speed, false);
  (void)ra_usb_attach_handler(nullptr, nullptr);
  const ra_err_t err  = ra_usb_device_deinit(s_state.speed);
  s_state.initialised = false;
  s_state.event_fn    = nullptr;
  s_state.event_ctx   = nullptr;
  s_state.state       = k_ra_usb_pal_state_detached;
  return err;
}

ra_err_t ra_usb_pal_attach(bool attached)
{
  if (!s_state.initialised) {
    return k_ra_err_invalid_state;
  }
  const ra_err_t err = ra_usb_device_attach(s_state.speed, attached);
  if (err != k_ra_ok) { /* GCOVR_EXCL_BR_LINE */
    /* GCOVR_EXCL_START */
    return err;
    /* GCOVR_EXCL_STOP */
  }
  if (attached) {
    s_state.state = k_ra_usb_pal_state_attached;
  } else {
    s_state.state = k_ra_usb_pal_state_detached;
  }
  return k_ra_ok;
}

ra_err_t ra_usb_pal_get_state(ra_usb_pal_state_t* out_state)
{
  RA_CHECK_NULL_PTR(out_state, s_tag, "get_state: out_state");
  if (!s_state.initialised) {
    return k_ra_err_invalid_state;
  }
  *out_state = s_state.state;
  return k_ra_ok;
}

ra_err_t ra_usb_pal_ep_open(uint8_t              ep_addr,
                            ra_usb_pal_ep_dir_t  dir,
                            ra_usb_pal_ep_type_t type,
                            uint16_t             max_packet)
{
  if (!s_state.initialised) {
    return k_ra_err_invalid_state;
  }
  if ((ep_addr == 0U) || (ep_addr > (uint8_t)k_ra_usb_pal_ep_max)) {
    return k_ra_err_invalid_arg;
  }
  if ((dir != k_ra_usb_pal_ep_dir_out) && (dir != k_ra_usb_pal_ep_dir_in)) {
    return k_ra_err_invalid_arg;
  }
  if ((type > k_ra_usb_pal_ep_type_intr) || (max_packet == 0U) ||
      (max_packet > (uint16_t)k_ra_usb_pal_xfer_max)) {
    return k_ra_err_invalid_arg;
  }
  /* Wave 7.2 stub. ra_usb pipe primitives land in 7.2b. */
  return k_ra_err_not_supported;
}

ra_err_t ra_usb_pal_ep_send(uint8_t ep_addr, const uint8_t* data, uint16_t len)
{
  if (!s_state.initialised) {
    return k_ra_err_invalid_state;
  }
  if ((ep_addr == 0U) || (ep_addr > (uint8_t)k_ra_usb_pal_ep_max)) {
    return k_ra_err_invalid_arg;
  }
  if ((len > (uint16_t)k_ra_usb_pal_xfer_max) || ((data == nullptr) && (len != 0U))) {
    return (data == nullptr) ? k_ra_err_null_ptr : k_ra_err_invalid_arg;
  }
  /* Wave 7.2 stub. */
  return k_ra_err_not_supported;
}

/* out_buf and inout_len are written by the future ra_usb pipe path. */
ra_err_t ra_usb_pal_ep_recv(uint8_t   ep_addr,
                            uint8_t*  out_buf,   // NOLINT(readability-non-const-parameter)
                            uint16_t* inout_len) // NOLINT(readability-non-const-parameter)
{
  RA_CHECK_NULL_PTR(out_buf, s_tag, "ep_recv: out_buf");
  RA_CHECK_NULL_PTR(inout_len, s_tag, "ep_recv: inout_len");
  if (!s_state.initialised) {
    return k_ra_err_invalid_state;
  }
  if ((ep_addr == 0U) || (ep_addr > (uint8_t)k_ra_usb_pal_ep_max)) {
    return k_ra_err_invalid_arg;
  }
  if (*inout_len == 0U) {
    return k_ra_err_invalid_arg;
  }
  /* Wave 7.2 stub. */
  return k_ra_err_not_supported;
}

ra_err_t ra_usb_pal_set_event_handler(ra_usb_pal_event_fn_t fn, void* ctx)
{
  if (!s_state.initialised) {
    return k_ra_err_invalid_state;
  }
  s_state.event_fn  = fn;
  s_state.event_ctx = ctx;
  return k_ra_ok;
}
