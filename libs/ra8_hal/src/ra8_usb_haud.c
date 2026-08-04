/**
 * @file ra8_usb_haud.c
 * @brief Native USB host-side Audio class layer implementation
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Glues the host-mode bring-up paths in `ra8_usb` to a USB Audio class
 * peripheral - speaker / headphones / microphone - attached on the
 * EK-RA8D2's USB-host port. This file is the native host-Audio class
 * layer; FSP's `r_usb_haud.c` is reference material only -- nothing is
 * pulled in verbatim.
 *
 * Mapping vs FSP (FSP function -> our entry point):
 *
 *  - `R_USB_HAUD_DeviceInfoGet`              -> attach-callback payload
 *  - `usb_haud_audio10_format_descriptor_get` -> `internal_walk_config_descriptor`
 *  - `usb_haud_audio10_feature_unit_descriptor_get` -> Feature Unit
 *                                              ID extraction in same.
 *  - `usb_haud_set_sampling_frequency`        -> `ra8_usb_haud_set_format`
 *  - Feature-Unit SET_CUR(VOLUME)             -> `ra8_usb_haud_set_volume`
 *  - Feature-Unit SET_CUR(MUTE)               -> `ra8_usb_haud_set_mute`
 *  - iso-IN / iso-OUT                          -> `_recv_samples` / `_send_samples`
 *
 * The starter does CPU-FIFO, single-device, no-hub. Enumeration is
 * driven step-by-step from the controller's CTRT interrupt path
 * (production) or directly via `ra8_usb_haud_step` (tests). Each step
 * issues exactly one chapter-9 SETUP request via
 * `ra8_usb_host_setup_request`; the next CTRT advances the step.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_usb_haud.h"

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_log.h"
#include "ra8_usb.h"

static const char* s_tag = "USBHAUD";

/* =============================================================================
 * Internal constants
 * =============================================================================
 */

/**
 * @enum ra8_usb_haud_step_t
 * @brief Enumeration step machine states.
 *
 * @details Mirrors FSP's host-Audio enumeration sequence in
 * `r_usb_haud.c`. Each step issues exactly one SETUP via
 * `ra8_usb_host_setup_request`; the next CTRT interrupt advances.
 */
typedef enum : uint8_t {
  k_ra8_haud_step_idle          = 0U, /**< Pre-attach.                   */
  k_ra8_haud_step_bus_reset     = 1U, /**< Drive USBRST then release.    */
  k_ra8_haud_step_set_address   = 2U, /**< SET_ADDRESS to assigned 1.    */
  k_ra8_haud_step_get_dev_desc  = 3U, /**< GET_DEVICE_DESCRIPTOR (18).   */
  k_ra8_haud_step_get_cfg_desc  = 4U, /**< GET_CONFIGURATION_DESCRIPTOR. */
  k_ra8_haud_step_set_config    = 5U, /**< SET_CONFIGURATION (1).        */
  k_ra8_haud_step_set_interface = 6U, /**< SET_INTERFACE (alt 1).        */
  k_ra8_haud_step_walk_desc     = 7U, /**< Walk AC + AS interfaces.      */
  k_ra8_haud_step_done          = 8U, /**< Attach callback fires.        */
} ra8_usb_haud_step_t;

/**
 * @enum ra8_usb_haud_setup_field_t
 * @brief Standard chapter-9 + Audio class request encodings.
 */
typedef enum : uint8_t {
  /* Chapter-9 standard requests (USB 2.0 spec section 9.4). */
  k_ra8_haud_bm_std_dev_in       = 0x80U, /**< Std | Device | In.     */
  k_ra8_haud_bm_std_dev_out      = 0x00U, /**< Std | Device | Out.    */
  k_ra8_haud_bm_std_iface_out    = 0x01U, /**< Std | Interface | Out. */
  k_ra8_haud_breq_get_descriptor = 0x06U, /**< GET_DESCRIPTOR.        */
  k_ra8_haud_breq_set_address    = 0x05U, /**< SET_ADDRESS.           */
  k_ra8_haud_breq_set_config     = 0x09U, /**< SET_CONFIGURATION.     */
  k_ra8_haud_breq_set_interface  = 0x0BU, /**< SET_INTERFACE.         */
  /* USB Audio 1.0 sec 5.2 "Class-Specific Requests" envelopes. */
  k_ra8_haud_bm_class_iface_out = 0x21U, /**< Class | Interface | Out. */
  k_ra8_haud_bm_class_iface_in  = 0xA1U, /**< Class | Interface | In.  */
  k_ra8_haud_bm_class_ep_out    = 0x22U, /**< Class | Endpoint | Out.  */
  k_ra8_haud_bm_class_ep_in     = 0xA2U, /**< Class | Endpoint | In.   */
  /* Descriptor types in wValue's high byte (chapter 9). */
  k_ra8_haud_desc_device        = 0x01U, /**< DEVICE descriptor.        */
  k_ra8_haud_desc_configuration = 0x02U, /**< CONFIGURATION descriptor. */
} ra8_usb_haud_setup_field_t;

/**
 * @enum ra8_usb_haud_size_t
 * @brief Standard descriptor sizes and wire payloads.
 */
typedef enum : uint16_t {
  k_ra8_haud_dev_desc_len        = 18U, /**< USB DEVICE descriptor.        */
  k_ra8_haud_cfg_desc_len        = 9U,  /**< CONFIGURATION descriptor hdr. */
  k_ra8_haud_assigned_address    = 1U,  /**< First assigned device addr.   */
  k_ra8_haud_default_config      = 1U,  /**< bConfigurationValue = 1.      */
  k_ra8_haud_default_alt         = 1U,  /**< Streaming alt setting.        */
  k_ra8_haud_volume_payload      = 2U,  /**< 16-bit signed dB payload.     */
  k_ra8_haud_mute_payload        = 1U,  /**< 1-byte boolean payload.       */
  k_ra8_haud_sample_rate_payload = 3U,  /**< 24-bit LE rate payload.       */
} ra8_usb_haud_size_t;

/**
 * @enum ra8_usb_haud_byte_shift_t
 * @brief Per-byte left-shift constants for wValue / wIndex layout.
 */
typedef enum : uint8_t {
  k_ra8_haud_shift_byte0 = 0U, /**< RA8 haud shift byte0. */
  k_ra8_haud_shift_byte1 = 8U, /**< RA8 haud shift byte1. */
} ra8_usb_haud_byte_shift_t;

/**
 * @enum ra8_usb_haud_default_unit_t
 * @brief Default Feature Unit ID applied by the descriptor-walk stub
 *        when the attached device follows the canonical USB-headphones
 *        layout (Input Terminal -> Feature Unit -> Output Terminal).
 */
typedef enum : uint8_t {
  k_ra8_haud_default_feature_unit_id = 2U, /**< Common bUnitID for FU.  */
  k_ra8_haud_default_ac_interface    = 0U, /**< AC bInterfaceNumber.    */
  k_ra8_haud_default_as_interface    = 1U, /**< AS bInterfaceNumber.    */
  k_ra8_haud_default_iso_out_ep      = 1U, /**< Common iso-OUT EP num.  */
  k_ra8_haud_default_iso_in_ep       = 0U, /**< No mic in default stub. */
} ra8_usb_haud_default_unit_t;

/* =============================================================================
 * Internal state
 * =============================================================================
 */

/**
 * @struct ra8_usb_haud_state_t
 * @brief Singleton shadow state for the host-Audio driver.
 */
typedef struct {
  bool                     initialized; /**< True after `ra8_usb_haud_init`. */
  bool                     attached;    /**< True after enumeration done.    */
  ra8_usb_speed_t          speed;       /**< Underlying controller.          */
  ra8_usb_haud_step_t      step;        /**< Current enumeration step.       */
  ra8_usb_haud_attach_fn_t attach_cb;   /**< Attach callback, or NULL.       */
  void*                    attach_ctx;  /**< Attach callback ctx.            */
  ra8_usb_haud_device_t    device;      /**< Snapshot of attached device.    */
} ra8_usb_haud_state_t;

static ra8_usb_haud_state_t s_state = {};

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
  return (speed == k_ra8_usb_speed_hs) ? k_ra8_haud_iso_max_packet_hs
                                       : k_ra8_haud_iso_max_packet_fs;
}

/**
 * @brief Configure the host-Audio iso pipes against the attached
 *        device's endpoints.
 *
 * @details Mirrors FSP's `usb_haud_pipe_info`. PIPE2 (iso-OUT) is
 * configured if the device exposes a speaker; PIPE1 (iso-IN) is
 * configured if it exposes a microphone. A device with neither
 * endpoint is silently accepted -- the class layer just won't be able
 * to push or drain samples.
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
  ra8_err_t err = k_ra8_ok;
  if (s_state.device.iso_out_ep != 0U) {
    err = ra8_usb_configure_endpoint(s_state.speed,
                                     k_ra8_haud_pipe_iso_out,
                                     s_state.device.iso_out_ep,
                                     k_ra8_usb_ep_dir_out,
                                     k_ra8_usb_ep_type_iso,
                                     s_state.device.iso_out_max_packet);
    RA8_RETURN_ON_ERROR(err, s_tag, "haud: iso-out cfg"); /* GCOVR_EXCL_BR_LINE */
  }
  if (s_state.device.iso_in_ep != 0U) {
    err = ra8_usb_configure_endpoint(s_state.speed,
                                     k_ra8_haud_pipe_iso_in,
                                     s_state.device.iso_in_ep,
                                     k_ra8_usb_ep_dir_in,
                                     k_ra8_usb_ep_type_iso,
                                     s_state.device.iso_in_max_packet);
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
    .bm_request_type = k_ra8_haud_bm_std_dev_in,
    .b_request       = k_ra8_haud_breq_get_descriptor,
    .w_value         = (uint16_t)((uint16_t)desc_type << k_ra8_haud_shift_byte1),
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
    .bm_request_type = k_ra8_haud_bm_std_dev_out,
    .b_request       = k_ra8_haud_breq_set_address,
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
    .bm_request_type = k_ra8_haud_bm_std_dev_out,
    .b_request       = k_ra8_haud_breq_set_config,
    .w_value         = (uint16_t)config_value,
    .w_index         = 0U,
    .w_length        = 0U,
  };
  return ra8_usb_host_setup_request(s_state.speed, &setup);
}

/**
 * @brief Stage a SET_INTERFACE SETUP request to the AS interface.
 *
 * @details Per USB Audio 1.0 sec 4.5 the host must select a non-zero
 * Audio-Streaming alt setting (alt 0 is the "zero-bandwidth" default)
 * for any iso-EP transfers to actually move data.
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
static ra8_err_t internal_setup_set_interface(void)
{
  const ra8_usb_setup_t setup = {
    .bm_request_type = k_ra8_haud_bm_std_iface_out,
    .b_request       = k_ra8_haud_breq_set_interface,
    .w_value         = k_ra8_haud_default_alt,
    .w_index         = (uint16_t)s_state.device.as_interface,
    .w_length        = 0U,
  };
  return ra8_usb_host_setup_request(s_state.speed, &setup);
}

/**
 * @brief Populate `s_state.device` with stub descriptor data.
 *
 * @details In production this routine walks the configuration
 * descriptor returned in the GET_CONFIG_DESCRIPTOR data stage. It
 * locates:
 *
 *  1. The Audio-Control interface (bInterfaceClass = 0x01,
 *     bInterfaceSubClass = 0x01).
 *  2. At least one Audio-Streaming interface (bInterfaceClass = 0x01,
 *     bInterfaceSubClass = 0x02), and within it the FORMAT_TYPE_I
 *     class-specific AS descriptor (bDescriptorSubtype = 0x02,
 *     bFormatType = 0x01) - this is where bNrChannels,
 *     bBitResolution, and the tSamFreq[] range live.
 *  3. The isochronous-IN / OUT endpoints inside that AS interface
 *     (bmAttributes & 0x03 == 0x01).
 *  4. (Optional but useful) Within the AC interface's class-specific
 *     descriptors, the Feature Unit (bDescriptorSubtype = 0x06) - the
 *     bUnitID field is what wIndex's high byte must carry for any
 *     SET_CUR(VOLUME) / SET_CUR(MUTE) request. The walk also visits
 *     Input Terminal (0x02), Output Terminal (0x03), and Selector
 *     Unit (0x05) entries to make sure the Feature Unit picked is the
 *     one chained between the input and output terminals (rather than
 *     a sidechain unit).
 *
 * The starter relies on the fact that a typical USB-headphones device
 * advertises the canonical "Input Terminal -> Feature Unit (id=2) ->
 * Output Terminal" graph, with 48 kHz / 16-bit / stereo PCM and an
 * iso-OUT endpoint at EP address 1. If the attached device deviates,
 * the production path will overwrite these defaults during the
 * descriptor walk.
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
  s_state.device.device_address     = (uint8_t)k_ra8_haud_assigned_address;
  s_state.device.ac_interface       = k_ra8_haud_default_ac_interface;
  s_state.device.as_interface       = k_ra8_haud_default_as_interface;
  s_state.device.feature_unit_id    = k_ra8_haud_default_feature_unit_id;
  s_state.device.iso_out_ep         = k_ra8_haud_default_iso_out_ep;
  s_state.device.iso_in_ep          = k_ra8_haud_default_iso_in_ep;
  s_state.device.channel_count      = (uint8_t)k_ra8_haud_default_channels;
  s_state.device.bits_per_sample    = (uint8_t)k_ra8_haud_default_bits;
  s_state.device.iso_out_max_packet = internal_iso_max_packet(s_state.speed);
  s_state.device.iso_in_max_packet  = 0U;
  s_state.device.sample_rate_min_hz = k_ra8_haud_default_sample_rate_hz;
  s_state.device.sample_rate_max_hz = k_ra8_haud_default_sample_rate_hz;
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
  s_state.step = k_ra8_haud_step_bus_reset;
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
  RA8_RETURN_ON_ERROR(rel, s_tag, "haud: release bus reset"); /* GCOVR_EXCL_BR_LINE */
  s_state.step = k_ra8_haud_step_set_address;
  return internal_setup_set_address((uint8_t)k_ra8_haud_assigned_address);
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
  const ra8_err_t addr_err =
    ra8_usb_set_address(s_state.speed, (uint8_t)k_ra8_haud_assigned_address);
  RA8_RETURN_ON_ERROR(addr_err, s_tag, "haud: set USBADDR"); /* GCOVR_EXCL_BR_LINE */
  s_state.step = k_ra8_haud_step_get_dev_desc;
  return internal_setup_get_descriptor(k_ra8_haud_desc_device, k_ra8_haud_dev_desc_len);
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
  s_state.step = k_ra8_haud_step_get_cfg_desc;
  return internal_setup_get_descriptor(k_ra8_haud_desc_configuration, k_ra8_haud_cfg_desc_len);
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
  /* Walk the class-specific descriptors out of the staged GET_CFG_DESC
   * data buffer. Stub overwrites s_state.device defaults. */
  internal_walk_config_descriptor();
  s_state.step = k_ra8_haud_step_set_config;
  return internal_setup_set_config((uint8_t)k_ra8_haud_default_config);
}

/**
 * @brief Step handler -- SETUP for SET_INTERFACE (alt 1).
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
  s_state.step = k_ra8_haud_step_set_interface;
  return internal_setup_set_interface();
}

/**
 * @brief Step handler -- pure software state move into walk_desc.
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
  s_state.step = k_ra8_haud_step_walk_desc;
  return k_ra8_ok;
}

/**
 * @brief Step handler -- configure pipes and finalise.
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
  RA8_RETURN_ON_ERROR(pipes_err, s_tag, "haud: configure pipes"); /* GCOVR_EXCL_BR_LINE */
  s_state.attached = true;
  s_state.step     = k_ra8_haud_step_done;
  if (s_state.attach_cb != nullptr) {
    s_state.attach_cb(s_state.attach_ctx, &s_state.device);
  }
  return k_ra8_ok;
}

/**
 * @brief Drive the enumeration step machine forward by one step.
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
static ra8_err_t internal_step_advance(void)
{
  switch (s_state.step) {
    case k_ra8_haud_step_idle:
      return internal_do_idle();
    case k_ra8_haud_step_bus_reset:
      return internal_do_bus_reset();
    case k_ra8_haud_step_set_address:
      return internal_do_set_address();
    case k_ra8_haud_step_get_dev_desc:
      return internal_do_get_dev_desc();
    case k_ra8_haud_step_get_cfg_desc:
      return internal_do_get_cfg_desc();
    case k_ra8_haud_step_set_config:
      return internal_do_set_config();
    case k_ra8_haud_step_set_interface:
      return internal_do_set_interface();
    case k_ra8_haud_step_walk_desc:
      return internal_do_walk_desc();
    default:
      /* Already done; idempotent. */
      return k_ra8_ok;
  }
}

/**
 * @brief Common pre-flight for every class control transfer.
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
static ra8_err_t internal_class_preflight(void)
{
  if (!s_state.initialized) {
    return k_ra8_err_invalid_state;
  }
  if (!s_state.attached) {
    return k_ra8_err_invalid_state;
  }
  return k_ra8_ok;
}

/**
 * @brief Validate audio format against the supported ranges.
 *
 * @details See implementation.
 * @param[in] channel_count See implementation.
 * @param[in] bits_per_sample See implementation.
 * @param[in] sample_rate See implementation.
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
static bool internal_format_ok(uint8_t channel_count, uint8_t bits_per_sample, uint32_t sample_rate)
{
  if ((channel_count < (uint8_t)k_ra8_haud_min_channels) ||
      (channel_count > (uint8_t)k_ra8_haud_max_channels)) {
    return false;
  }
  if ((bits_per_sample < (uint8_t)k_ra8_haud_min_bits) ||
      (bits_per_sample > (uint8_t)k_ra8_haud_max_bits)) {
    return false;
  }
  if ((sample_rate < k_ra8_haud_min_sample_rate_hz) ||
      (sample_rate > k_ra8_haud_max_sample_rate_hz)) {
    return false;
  }
  return true;
}

/* =============================================================================
 * Lifecycle
 * =============================================================================
 */

ra8_err_t ra8_usb_haud_init(ra8_usb_speed_t speed)
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
  s_state.step        = k_ra8_haud_step_idle;
  s_state.attached    = false;
  s_state.attach_cb   = nullptr;
  s_state.attach_ctx  = nullptr;
  s_state.device      = (ra8_usb_haud_device_t){};
  s_state.initialized = true;

  ra8_log_info_val(s_tag, "host-Audio ready", (uint32_t)speed);
  return k_ra8_ok;
}

ra8_err_t ra8_usb_haud_close(void)
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
  s_state.step        = k_ra8_haud_step_idle;
  return err;
}

/* =============================================================================
 * Attach callback
 * =============================================================================
 */

ra8_err_t ra8_usb_haud_attach_callback(ra8_usb_haud_attach_fn_t on_attach, void* ctx)
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

/* USB Audio 1.0 sec 5.2.3.2.3.1 "Sampling Frequency Control":
 *   bmRequestType = 0x22 (H2D | Class | Endpoint)
 *   bRequest      = 0x01 (SET_CUR)
 *   wValue.high   = 0x01 (SAMPLING_FREQ_CONTROL)
 *   wValue.low    = 0x00
 *   wIndex        = endpoint address
 *   wLength       = 3 (24-bit LE sample rate)
 */
ra8_err_t
ra8_usb_haud_set_format(uint8_t channel_count, uint8_t bits_per_sample, uint32_t sample_rate_hz)
{
  const ra8_err_t pre = internal_class_preflight();
  if (pre != k_ra8_ok) {
    return pre;
  }
  if (!internal_format_ok(channel_count, bits_per_sample, sample_rate_hz)) {
    return k_ra8_err_invalid_arg;
  }
  /* Update the cached snapshot so the recipient's
   * `ra8_usb_haud_device_t` reflects the new format. */
  s_state.device.channel_count      = channel_count;
  s_state.device.bits_per_sample    = bits_per_sample;
  s_state.device.sample_rate_min_hz = sample_rate_hz;
  s_state.device.sample_rate_max_hz = sample_rate_hz;

  const uint8_t target_ep =
    (s_state.device.iso_out_ep != 0U) ? s_state.device.iso_out_ep : s_state.device.iso_in_ep;
  const ra8_usb_setup_t setup = {
    .bm_request_type = k_ra8_haud_bm_class_ep_out,
    .b_request       = k_ra8_haud_req_set_cur,
    .w_value  = (uint16_t)((uint16_t)k_ra8_haud_ep_control_sampling_freq << k_ra8_haud_shift_byte1),
    .w_index  = (uint16_t)target_ep,
    .w_length = k_ra8_haud_sample_rate_payload,
  };
  return ra8_usb_host_setup_request(s_state.speed, &setup);
}

/* USB Audio 1.0 sec 5.2.2.4.3.2 "Volume Control":
 *   bmRequestType = 0x21 (H2D | Class | Interface)
 *   bRequest      = 0x01 (SET_CUR)
 *   wValue.high   = 0x02 (VOLUME_CONTROL)
 *   wValue.low    = channel
 *   wIndex.high   = bUnitID (Feature Unit)
 *   wIndex.low    = bInterfaceNumber (AC interface)
 *   wLength       = 2 (signed 16-bit dB in 1/256 step)
 */
ra8_err_t ra8_usb_haud_set_volume(uint8_t channel, int16_t volume)
{
  const ra8_err_t pre = internal_class_preflight();
  if (pre != k_ra8_ok) {
    return pre;
  }
  const uint16_t w_value =
    (uint16_t)(((uint16_t)k_ra8_haud_fu_control_volume << k_ra8_haud_shift_byte1) |
               (uint16_t)channel);
  const uint16_t w_index =
    (uint16_t)(((uint16_t)s_state.device.feature_unit_id << k_ra8_haud_shift_byte1) |
               (uint16_t)s_state.device.ac_interface);
  const ra8_usb_setup_t setup = {
    .bm_request_type = k_ra8_haud_bm_class_iface_out,
    .b_request       = k_ra8_haud_req_set_cur,
    .w_value         = w_value,
    .w_index         = w_index,
    .w_length        = k_ra8_haud_volume_payload,
  };
  /* Production path queues the 2-byte signed dB payload on the DCP
   * data stage. The starter just ensures the SETUP envelope hits the
   * wire. */
  (void)volume;
  return ra8_usb_host_setup_request(s_state.speed, &setup);
}

/* USB Audio 1.0 sec 5.2.2.4.3.1 "Mute Control":
 *   bmRequestType = 0x21 (H2D | Class | Interface)
 *   bRequest      = 0x01 (SET_CUR)
 *   wValue.high   = 0x01 (MUTE_CONTROL)
 *   wValue.low    = channel
 *   wIndex.high   = bUnitID (Feature Unit)
 *   wIndex.low    = bInterfaceNumber (AC interface)
 *   wLength       = 1 (boolean)
 */
ra8_err_t ra8_usb_haud_set_mute(uint8_t channel, bool mute)
{
  const ra8_err_t pre = internal_class_preflight();
  if (pre != k_ra8_ok) {
    return pre;
  }
  const uint16_t w_value =
    (uint16_t)(((uint16_t)k_ra8_haud_fu_control_mute << k_ra8_haud_shift_byte1) |
               (uint16_t)channel);
  const uint16_t w_index =
    (uint16_t)(((uint16_t)s_state.device.feature_unit_id << k_ra8_haud_shift_byte1) |
               (uint16_t)s_state.device.ac_interface);
  const ra8_usb_setup_t setup = {
    .bm_request_type = k_ra8_haud_bm_class_iface_out,
    .b_request       = k_ra8_haud_req_set_cur,
    .w_value         = w_value,
    .w_index         = w_index,
    .w_length        = k_ra8_haud_mute_payload,
  };
  (void)mute;
  return ra8_usb_host_setup_request(s_state.speed, &setup);
}

/* =============================================================================
 * Isochronous data transfer
 * =============================================================================
 */

ra8_err_t ra8_usb_haud_send_samples(const uint8_t* buf, uint16_t len_bytes)
{
  if ((buf == nullptr) && (len_bytes != 0U)) {
    return k_ra8_err_null_ptr;
  }
  if (buf == nullptr) {
    return k_ra8_err_null_ptr;
  }
  const ra8_err_t pre = internal_class_preflight();
  if (pre != k_ra8_ok) {
    return pre;
  }
  if (len_bytes == 0U) {
    return k_ra8_err_invalid_arg;
  }
  if (s_state.device.iso_out_ep == 0U) {
    return k_ra8_err_invalid_state;
  }
  return ra8_usb_queue_in(s_state.speed, k_ra8_haud_pipe_iso_out, buf, len_bytes);
}

ra8_err_t ra8_usb_haud_recv_samples(uint8_t* buf, uint16_t max_len_bytes, uint16_t* got_len_bytes)
{
  RA8_CHECK_NULL_PTR(buf, s_tag, "recv_samples: buf");
  RA8_CHECK_NULL_PTR(got_len_bytes, s_tag, "recv_samples: got_len_bytes");
  const ra8_err_t pre = internal_class_preflight();
  if (pre != k_ra8_ok) {
    return pre;
  }
  if (max_len_bytes == 0U) {
    return k_ra8_err_invalid_arg;
  }
  if (s_state.device.iso_in_ep == 0U) {
    return k_ra8_err_invalid_state;
  }
  uint16_t        inout_len = max_len_bytes;
  const ra8_err_t err =
    ra8_usb_queue_out(s_state.speed, k_ra8_haud_pipe_iso_in, buf, &inout_len, true);
  if (err == k_ra8_ok) {
    *got_len_bytes = inout_len;
  } else {
    *got_len_bytes = 0U;
  }
  return err;
}

/* =============================================================================
 * Test / introspection helpers
 * =============================================================================
 */

ra8_err_t ra8_usb_haud_step(void)
{
  if (!s_state.initialized) {
    return k_ra8_err_invalid_state;
  }
  return internal_step_advance();
}
