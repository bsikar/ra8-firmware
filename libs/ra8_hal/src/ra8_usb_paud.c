/**
 * @file ra8_usb_paud.c
 * @brief Native USB device-side Audio (UAC1) class layer implementation
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Glues the device-mode `ra8_usb` driver to a USB Audio Class 1.0
 * function so the EK-RA8D2 enumerates as a microphone (iso IN), a
 * speaker (iso OUT), or both. This file is the native peripheral-Audio
 * class layer; FSP's `r_usb_paud_driver.c` is reference material only,
 * nothing is pulled in verbatim.
 *
 * Mapping vs FSP:
 *
 *  - `usb_paud_class_request_init` -> `ra8_usb_paud_init`
 *  - `usb_paud_write_complete`     -> `ra8_usb_paud_send_frame` queue
 *  - `usb_paud_read_complete`      -> `ra8_usb_paud_recv_frame` drain
 *  - `usb_paud_setup`              -> `ra8_usb_paud_handle_setup`
 *
 * Reference: USB Audio 1.0 sec 5.2.1 "Request Layout" and sec A.9
 * "Audio Class-Specific Request Codes".
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_usb_paud.h"

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_log.h"
#include "ra8_usb.h"

static const char* s_tag = "USBPAUD";

/* =============================================================================
 * Internal constants
 * =============================================================================
 */

/**
 * @enum ra8_usb_paud_setup_field_t
 * @brief Constants used to recognise audio class-specific SETUPs.
 *
 * @details Per USB Audio 1.0 sec 5.2 "Class-Specific Request" the
 * standard envelopes are interface-recipient class (0x21 / 0xA1) for
 * AC / AS interface controls and endpoint-recipient class (0x22 /
 * 0xA2) for endpoint-controls (e.g. sampling-frequency).
 */
typedef enum : uint8_t {
  k_ra8_paud_bm_class_iface_in  = 0xA1U, /**< Class | Iface | In.  */
  k_ra8_paud_bm_class_iface_out = 0x21U, /**< Class | Iface | Out. */
  k_ra8_paud_bm_class_ep_in     = 0xA2U, /**< Class | EP    | In.  */
  k_ra8_paud_bm_class_ep_out    = 0x22U, /**< Class | EP    | Out. */
} ra8_usb_paud_setup_field_t;

/**
 * @enum ra8_usb_paud_default_t
 * @brief Spec defaults the class layer seeds at init.
 *
 * @details 48 kHz / stereo / 16-bit is the most common HID-class /
 * UAC1 default for capture and playback gadgets.
 */
typedef enum : uint32_t {
  k_ra8_paud_default_rate_hz  = 48000U, /**< 48 kHz default rate. */
  k_ra8_paud_default_channels = 2U,     /**< Stereo default.      */
  k_ra8_paud_default_bps      = 2U,     /**< 16-bit default.      */
  k_ra8_paud_default_volume   = 0U,     /**< 0 dB volume default. */
} ra8_usb_paud_default_t;

/**
 * @enum ra8_usb_paud_limit_t
 * @brief Format-shadow validation limits.
 */
typedef enum : uint8_t {
  k_ra8_paud_max_channels = 2U, /**< Stereo cap.           */
  k_ra8_paud_max_bps      = 4U, /**< 32-bit max sub-frame. */
  k_ra8_paud_min_bps      = 1U, /**< 8-bit min sub-frame.  */
  k_ra8_paud_min_channels = 1U, /**< Mono min.             */
} ra8_usb_paud_limit_t;

/* =============================================================================
 * Internal state
 * =============================================================================
 */

/**
 * @struct ra8_usb_paud_state_t
 * @brief Singleton shadow state for the device-Audio function.
 */
typedef struct {
  bool                    initialized;    /**< True after init.           */
  ra8_usb_speed_t         speed;          /**< Underlying controller.     */
  uint16_t                iso_max_packet; /**< Pipe max-packet size.      */
  const uint8_t*          desc;           /**< Cached descriptor blob.    */
  uint16_t                desc_len;       /**< Descriptor blob byte len.  */
  ra8_usb_paud_format_t   format;         /**< Current iso format shadow. */
  int16_t                 volume_q8_8;    /**< Current volume in Q8.8 dB. */
  ra8_usb_paud_setup_fn_t setup_cb;       /**< Application class handler. */
  void*                   setup_ctx;      /**< Class handler context.     */
} ra8_usb_paud_state_t;

static ra8_usb_paud_state_t s_state = {};

/* =============================================================================
 * Internal helpers
 * =============================================================================
 */

/**
 * @brief Pick the iso-max-packet ceiling matching the negotiated speed.
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
static uint16_t internal_iso_max_packet(ra8_usb_speed_t speed)
{
  return (speed == k_ra8_usb_speed_hs) ? k_ra8_paud_iso_max_packet_hs
                                       : k_ra8_paud_iso_max_packet_fs_default;
}

/**
 * @brief Configure the two iso pipes.
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
static ra8_err_t internal_configure_pipes(ra8_usb_speed_t speed)
{
  const uint16_t mp = internal_iso_max_packet(speed);

  ra8_err_t err = ra8_usb_configure_endpoint(speed,
                                             k_ra8_paud_pipe_iso_in,
                                             k_ra8_paud_ep_iso_in_addr,
                                             k_ra8_usb_ep_dir_in,
                                             k_ra8_usb_ep_type_iso,
                                             mp);
  RA8_RETURN_ON_ERROR(err, s_tag, "paud: iso-in cfg"); /* GCOVR_EXCL_BR_LINE */

  err = ra8_usb_configure_endpoint(speed,
                                   k_ra8_paud_pipe_iso_out,
                                   k_ra8_paud_ep_iso_out_addr,
                                   k_ra8_usb_ep_dir_out,
                                   k_ra8_usb_ep_type_iso,
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
RA8_INTERNAL
static void internal_reset_shadow(ra8_usb_speed_t speed)
{
  s_state.speed                   = speed;
  s_state.iso_max_packet          = internal_iso_max_packet(speed);
  s_state.desc                    = nullptr;
  s_state.desc_len                = 0U;
  s_state.format.sample_rate_hz   = (uint32_t)k_ra8_paud_default_rate_hz;
  s_state.format.channels         = (uint8_t)k_ra8_paud_default_channels;
  s_state.format.bytes_per_sample = (uint8_t)k_ra8_paud_default_bps;
  s_state.volume_q8_8             = (int16_t)k_ra8_paud_default_volume;
  s_state.setup_cb                = nullptr;
  s_state.setup_ctx               = nullptr;
}

/**
 * @brief Recognise an audio class request code we forward.
 *
 * @details See implementation.
 * @param[in] b_request See implementation.
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
static bool internal_is_known_class_request(uint8_t b_request)
{
  return (b_request == k_ra8_paud_req_set_cur) || (b_request == k_ra8_paud_req_get_cur) ||
         (b_request == k_ra8_paud_req_set_min) || (b_request == k_ra8_paud_req_get_min) ||
         (b_request == k_ra8_paud_req_set_max) || (b_request == k_ra8_paud_req_get_max) ||
         (b_request == k_ra8_paud_req_set_res) || (b_request == k_ra8_paud_req_get_res) ||
         (b_request == k_ra8_paud_req_get_stat);
}

/**
 * @brief Recognise an audio class request envelope.
 *
 * @details See implementation.
 * @param[in] bm See implementation.
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
static bool internal_is_class_envelope(uint8_t bm)
{
  return (bm == k_ra8_paud_bm_class_iface_in) || (bm == k_ra8_paud_bm_class_iface_out) ||
         (bm == k_ra8_paud_bm_class_ep_in) || (bm == k_ra8_paud_bm_class_ep_out);
}

/* =============================================================================
 * Lifecycle
 * =============================================================================
 */

ra8_err_t ra8_usb_paud_init(ra8_usb_speed_t speed)
{
  if ((speed != k_ra8_usb_speed_fs) && (speed != k_ra8_usb_speed_hs)) {
    return k_ra8_err_invalid_arg;
  }
  const ra8_err_t usb_err = ra8_usb_device_init(speed);
  if (usb_err != k_ra8_ok) {
    ra8_log_error_val(s_tag, "ra8_usb_device_init failed", (uint32_t)usb_err);
    return k_ra8_err_hw_init_failed;
  }
  internal_reset_shadow(speed);

  const ra8_err_t pipes_err = internal_configure_pipes(speed);
  if (pipes_err != k_ra8_ok) {
    (void)ra8_usb_device_deinit(speed);
    return pipes_err;
  }
  s_state.initialized = true;
  ra8_log_info_val(s_tag, "device-Audio ready", (uint32_t)speed);
  return k_ra8_ok;
}

ra8_err_t ra8_usb_paud_close(void)
{
  if (!s_state.initialized) {
    return k_ra8_err_invalid_state;
  }
  (void)ra8_usb_device_attach(s_state.speed, false);
  const ra8_err_t err = ra8_usb_device_deinit(s_state.speed);
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

ra8_err_t ra8_usb_paud_set_descriptors(const uint8_t* desc, uint16_t desc_len)
{
  if (!s_state.initialized) {
    return k_ra8_err_invalid_state;
  }
  RA8_CHECK_NULL_PTR(desc, s_tag, "set_descriptors: desc");
  if (desc_len == 0U) {
    return k_ra8_err_invalid_arg;
  }
  s_state.desc     = desc;
  s_state.desc_len = desc_len;
  return k_ra8_ok;
}

/* =============================================================================
 * Audio data transport
 * =============================================================================
 */

ra8_err_t ra8_usb_paud_send_frame(const uint8_t* frame, uint16_t len)
{
  if (!s_state.initialized) {
    return k_ra8_err_invalid_state;
  }
  if ((frame == nullptr) && (len != 0U)) {
    return k_ra8_err_null_ptr;
  }
  if ((len == 0U) || (len > s_state.iso_max_packet)) {
    return k_ra8_err_invalid_arg;
  }
  return ra8_usb_queue_in(s_state.speed, k_ra8_paud_pipe_iso_in, frame, len);
}

ra8_err_t ra8_usb_paud_recv_frame(uint8_t* buf, uint16_t max_len, uint16_t* got_len)
{
  RA8_CHECK_NULL_PTR(buf, s_tag, "recv_frame: buf");
  RA8_CHECK_NULL_PTR(got_len, s_tag, "recv_frame: got_len");
  if (!s_state.initialized) {
    return k_ra8_err_invalid_state;
  }
  if (max_len == 0U) {
    return k_ra8_err_invalid_arg;
  }
  uint16_t        inout_len = max_len;
  const ra8_err_t err =
    ra8_usb_queue_out(s_state.speed, k_ra8_paud_pipe_iso_out, buf, &inout_len, true);
  if (err == k_ra8_ok) {
    *got_len = inout_len;
  } else {
    *got_len = 0U;
  }
  return err;
}

/* =============================================================================
 * Format / volume helpers
 * =============================================================================
 */

ra8_err_t ra8_usb_paud_set_format(ra8_usb_paud_format_t format)
{
  if (!s_state.initialized) {
    return k_ra8_err_invalid_state;
  }
  if (format.sample_rate_hz == 0U) {
    return k_ra8_err_invalid_arg;
  }
  if ((format.channels < (uint8_t)k_ra8_paud_min_channels) ||
      (format.channels > (uint8_t)k_ra8_paud_max_channels)) {
    return k_ra8_err_invalid_arg;
  }
  if ((format.bytes_per_sample < (uint8_t)k_ra8_paud_min_bps) ||
      (format.bytes_per_sample > (uint8_t)k_ra8_paud_max_bps)) {
    return k_ra8_err_invalid_arg;
  }
  s_state.format = format;
  return k_ra8_ok;
}

ra8_err_t ra8_usb_paud_get_format(ra8_usb_paud_format_t* out_format)
{
  RA8_CHECK_NULL_PTR(out_format, s_tag, "get_format: out_format");
  if (!s_state.initialized) {
    return k_ra8_err_invalid_state;
  }
  *out_format = s_state.format;
  return k_ra8_ok;
}

ra8_err_t ra8_usb_paud_set_volume(int16_t volume_q8_8)
{
  if (!s_state.initialized) {
    return k_ra8_err_invalid_state;
  }
  s_state.volume_q8_8 = volume_q8_8;
  return k_ra8_ok;
}

ra8_err_t ra8_usb_paud_get_volume(int16_t* out_volume)
{
  RA8_CHECK_NULL_PTR(out_volume, s_tag, "get_volume: out_volume");
  if (!s_state.initialized) {
    return k_ra8_err_invalid_state;
  }
  *out_volume = s_state.volume_q8_8;
  return k_ra8_ok;
}

/* =============================================================================
 * Setup-handler attach
 * =============================================================================
 */

ra8_err_t ra8_usb_paud_attach_setup_handler(ra8_usb_paud_setup_fn_t setup_fn, void* ctx)
{
  if (!s_state.initialized) {
    return k_ra8_err_invalid_state;
  }
  s_state.setup_cb  = setup_fn;
  s_state.setup_ctx = ctx;
  return k_ra8_ok;
}

/* =============================================================================
 * Class SETUP dispatch
 * =============================================================================
 */

ra8_err_t ra8_usb_paud_handle_setup(const ra8_usb_setup_t* setup)
{
  RA8_CHECK_NULL_PTR(setup, s_tag, "handle_setup: setup");
  if (!s_state.initialized) {
    return k_ra8_err_invalid_state;
  }
  if (!internal_is_class_envelope(setup->bm_request_type)) {
    return k_ra8_err_not_supported;
  }
  if (!internal_is_known_class_request(setup->b_request)) {
    return k_ra8_err_not_supported;
  }
  if (s_state.setup_cb != nullptr) {
    const ra8_err_t cb_err = s_state.setup_cb(s_state.setup_ctx, setup);
    if (cb_err != k_ra8_ok) {
      return ra8_usb_control_response(s_state.speed, false);
    }
  }
  return ra8_usb_control_response(s_state.speed, true);
}
