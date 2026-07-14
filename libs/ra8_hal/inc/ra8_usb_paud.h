/**
 * @file ra8_usb_paud.h
 * @brief Native USB device-side Audio (UAC) class layer
 * @ingroup grp_hal_usb
 *
 * @details
 * Glues the device-mode `ra8_usb` controller driver to a USB Audio
 * Class 1.0 function so the EK-RA8D2 enumerates as a microphone (iso
 * IN) or speaker (iso OUT) gadget. The implementation is from-scratch;
 * FSP `r_usb_paud_driver.c` is reference material only -- nothing is
 * pulled in verbatim. Mirrors the surface that FSP's peripheral-Audio
 * class exposes:
 *
 *  - Caller-supplied audio descriptor blob (interface association,
 *    AC interface header, AC input / output / feature units, AS
 *    interface alt-settings, AS general + format-type CS interface
 *    descriptors, plus iso EP descriptors).
 *  - One iso-IN pipe (PIPE1) for capture (microphone).
 *  - One iso-OUT pipe (PIPE2) for playback (speaker).
 *  - SET_CUR / GET_CUR shadow for current sample-rate (sampling-frequency
 *    control on the iso endpoint) and feature-unit volume / mute.
 *  - Caller-supplied class-setup callback so the application picks the
 *    rate / volume policy (e.g. clamp to USB-IF audio rates 8 / 16 /
 *    44.1 / 48 / 96 kHz, or follow the host without question).
 *
 * Reference: USB Device Class Definition for Audio Devices revision
 * 1.0 (USB-IF, 1998-03-18) -- specifically:
 *   - sec 4.3.2.5 "Class-Specific AS Interface Descriptor"
 *   - sec 5.2.1 "Request Layout" (SET_CUR / GET_CUR / GET_MIN / ...)
 *   - sec A.9 "Audio Class-Specific Request Codes"
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_usb.h"

/* =============================================================================
 * Constants
 * =============================================================================
 */

/**
 * @enum ra8_usb_paud_pipe_t
 * @brief PIPE numbers used by the device-Audio function for the local
 *        isochronous endpoints.
 *
 * @details FSP / RA8D2 PIPE assignment rules constrain isochronous
 * pipes to PIPE1 / PIPE2. PIPE1 carries iso IN (microphone), PIPE2
 * carries iso OUT (speaker). USB Audio 1.0 sec 3.7.1 "Standard AS
 * Isochronous Audio Data Endpoint Descriptor".
 */
typedef enum : uint8_t {
  k_ra8_paud_pipe_iso_in  = 1U, /**< PIPE1 -> EP1 IN  (capture).  */
  k_ra8_paud_pipe_iso_out = 2U, /**< PIPE2 -> EP2 OUT (playback). */
} ra8_usb_paud_pipe_t;

/**
 * @enum ra8_usb_paud_ep_t
 * @brief USB endpoint addresses used by the device-Audio function.
 */
typedef enum : uint8_t {
  k_ra8_paud_ep_iso_in_addr  = 1U, /**< EP1 IN address.  */
  k_ra8_paud_ep_iso_out_addr = 2U, /**< EP2 OUT address. */
} ra8_usb_paud_ep_t;

/**
 * @enum ra8_usb_paud_packet_t
 * @brief Packet sizing for the iso endpoints.
 *
 * @details Per USB 2.0 sec 5.6.3 "Isochronous Transfer Bus Access
 * Constraints", FS iso pipes top out at 1023 bytes / frame and HS at
 * 1024 bytes / microframe. A typical 48 kHz / 16-bit / stereo capture
 * stream is 192 bytes per FS frame (48 samples * 2 ch * 2 bytes).
 */
typedef enum : uint16_t {
  k_ra8_paud_iso_max_packet_fs_default = 192U,  /**< 48 kHz stereo 16-bit. */
  k_ra8_paud_iso_max_packet_fs         = 1023U, /**< FS iso ceiling.       */
  k_ra8_paud_iso_max_packet_hs         = 1024U, /**< HS iso ceiling.       */
} ra8_usb_paud_packet_t;

/**
 * @enum ra8_usb_paud_class_t
 * @brief Class / subclass / protocol triplet that identifies the local
 *        Audio function in the configuration descriptor.
 *
 * @details USB Audio 1.0 sec A.1 "Audio Interface Class Code" and A.2
 * "Audio Interface Subclass Codes".
 */
typedef enum : uint8_t {
  k_ra8_paud_class_audio        = 0x01U, /**< Audio interface class.   */
  k_ra8_paud_subclass_control   = 0x01U, /**< AudioControl Subclass.   */
  k_ra8_paud_subclass_streaming = 0x02U, /**< AudioStreaming Subclass. */
  k_ra8_paud_protocol_none      = 0x00U, /**< No protocol (UAC1).      */
} ra8_usb_paud_class_t;

/**
 * @enum ra8_usb_paud_request_t
 * @brief Audio class-specific request codes the host issues to the
 *        device.
 *
 * @details Per USB Audio 1.0 sec A.9 "Audio Class-Specific Request
 * Codes". The class layer pre-decodes SET_CUR / GET_CUR / GET_MIN /
 * GET_MAX / GET_RES and forwards them to the application callback.
 */
typedef enum : uint8_t {
  k_ra8_paud_req_set_cur  = 0x01U, /**< SET_CUR.  */
  k_ra8_paud_req_get_cur  = 0x81U, /**< GET_CUR.  */
  k_ra8_paud_req_set_min  = 0x02U, /**< SET_MIN.  */
  k_ra8_paud_req_get_min  = 0x82U, /**< GET_MIN.  */
  k_ra8_paud_req_set_max  = 0x03U, /**< SET_MAX.  */
  k_ra8_paud_req_get_max  = 0x83U, /**< GET_MAX.  */
  k_ra8_paud_req_set_res  = 0x04U, /**< SET_RES.  */
  k_ra8_paud_req_get_res  = 0x84U, /**< GET_RES.  */
  k_ra8_paud_req_get_stat = 0xFFU, /**< GET_STAT. */
} ra8_usb_paud_request_t;

/**
 * @enum ra8_usb_paud_control_sel_t
 * @brief Feature-unit / endpoint control selectors carried in the high
 *        byte of `wValue`.
 *
 * @details Per USB Audio 1.0 sec 5.2.2.4.3 "Feature Unit Control
 * Selectors" and sec 5.2.3.2.3.1 "Sampling Frequency Control".
 */
typedef enum : uint8_t {
  k_ra8_paud_ctl_mute        = 0x01U, /**< Mute Control.               */
  k_ra8_paud_ctl_volume      = 0x02U, /**< Volume Control.             */
  k_ra8_paud_ctl_sample_rate = 0x01U, /**< Sampling Frequency Control. */
} ra8_usb_paud_control_sel_t;

/**
 * @struct ra8_usb_paud_format_t
 * @brief Current iso stream format negotiated with the host.
 *
 * @details Mirrors the AS Format-Type-I descriptor: sample rate,
 * channel count, and sub-frame size. USB Audio 1.0 sec 2.2.5
 * "Format Type Descriptor".
 *
 * @invariant `sample_rate_hz != 0 && channels in {1,2} && bytes_per_sample in {1,2,3,4}`.
 */
typedef struct {
  uint32_t sample_rate_hz;   /**< Current SET_CUR sample rate.   */
  uint8_t  channels;         /**< 1 = mono, 2 = stereo.          */
  uint8_t  bytes_per_sample; /**< 16-bit -> 2, 24-bit -> 3, etc. */
} ra8_usb_paud_format_t;

/**
 * @typedef ra8_usb_paud_setup_fn_t
 * @brief Caller-supplied audio class-setup handler signature.
 *
 * @details The class layer pre-decodes the SETUP envelope (validates
 * it really is an audio class request to the local AC / AS interface
 * or to one of the iso EPs) and hands the SETUP packet to the
 * application. The callback can update `*format` (e.g. when SET_CUR
 * on the sampling-frequency control fires) or queue a data-stage
 * payload for GET_CUR responses.
 *
 * @param[in] ctx Caller-supplied context registered alongside the
 *                handler.
 * @param[in] setup The 8-byte SETUP envelope (class request).
 *
 * @return `ra8_err_t` error code; `k_ra8_ok` lets the class layer ACK the
 *         status stage, anything else stalls EP0.
 *
 * @note Invoked from the dispatch site (typically ISR context) when an
 * audio class SETUP lands.
 */
typedef ra8_err_t (*ra8_usb_paud_setup_fn_t)(void* ctx, const ra8_usb_setup_t* setup);

/* =============================================================================
 * Lifecycle
 * =============================================================================
 */

/**
 * @brief Bring up the device-Audio function on a chosen USB controller.
 *
 * @details
 * Initialises the underlying `ra8_usb` driver in DEVICE mode for
 * `speed`, configures PIPE1 (iso IN) and PIPE2 (iso OUT), seeds the
 * format shadow to 48 kHz / stereo / 16-bit, and leaves the D+ pull-up
 * dropped. The caller raises it via `ra8_usb_device_attach` once
 * descriptors are set.
 *
 * @param[in] speed Which USB controller (FS or HS).
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok Device-Audio ready.
 * @retval k_ra8_err_invalid_arg `speed` out of range.
 * @retval k_ra8_err_hw_init_failed Underlying `ra8_usb_device_init`
 *         failed.
 *
 * @pre Single-threaded init context.
 * @pre `ra8_mstp_init` and `ra8_pwr_init` already ran.
 *
 * @post Internal state machine armed; descriptor pointer cleared.
 * @post Pipe1 / pipe2 configured at the speed's default packet size.
 *
 * @note Not thread-safe.
 * @see ra8_usb_paud_set_descriptors
 * @see ra8_usb_paud_close
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_usb_paud_init(ra8_usb_speed_t speed);

/**
 * @brief Tear down the device-Audio function and release the controller.
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok Released.
 * @retval k_ra8_err_invalid_state Driver was never initialized.
 *
 * @pre Single-threaded shutdown context.
 *
 * @post `ra8_usb_device_deinit` ran; D+ pull-up dropped; subsequent
 *       device-Audio API calls return `k_ra8_err_invalid_state`.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_usb_paud_close(void);

/* =============================================================================
 * Descriptor handoff
 * =============================================================================
 */

/**
 * @brief Install the caller-supplied audio descriptor blob.
 *
 * @details
 * The class layer keeps a pointer + length pair; the application owns
 * the storage. The descriptor blob is the concatenation of:
 * AudioControl interface header, input/output/feature units, an IAD,
 * and AudioStreaming alt-settings + Format-Type-I + iso EP descriptors.
 *
 * @param[in] desc Pointer to the caller-owned descriptor blob.
 * @param[in] desc_len Byte length of `desc`.
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok Pointer + length stored.
 * @retval k_ra8_err_invalid_state Driver not initialized.
 * @retval k_ra8_err_null_ptr `desc` was NULL.
 * @retval k_ra8_err_invalid_arg `desc_len` was 0.
 *
 * @pre `ra8_usb_paud_init` succeeded.
 *
 * @post Subsequent GET_DESCRIPTOR(Configuration) requests can use
 *       this blob.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_usb_paud_set_descriptors(const uint8_t* desc, uint16_t desc_len);

/* =============================================================================
 * Audio data transport
 * =============================================================================
 */

/**
 * @brief Push a captured audio frame on the iso-IN endpoint.
 *
 * @details
 * Queues `len` bytes onto PIPE1. The caller is responsible for sample
 * alignment (e.g. integer multiple of `channels * bytes_per_sample`)
 * and for not exceeding the configured pipe max-packet.
 *
 * @param[in] frame  Audio frame buffer.
 * @param[in] len    Frame byte length.
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok Bytes queued onto iso-IN.
 * @retval k_ra8_err_invalid_state Driver not initialized.
 * @retval k_ra8_err_null_ptr `frame` was NULL with `len > 0`.
 * @retval k_ra8_err_invalid_arg `len == 0` or larger than the pipe max.
 *
 * @pre `ra8_usb_paud_init` succeeded.
 *
 * @post `len` bytes sit on PIPE1.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_usb_paud_send_frame(const uint8_t* frame, uint16_t len);

/**
 * @brief Drain a playback audio frame from the iso-OUT endpoint.
 *
 * @param[out] buf      Receive buffer.
 * @param[in]  max_len  Capacity of `buf`, > 0.
 * @param[out] got_len  Receives the number of bytes actually placed.
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok Bytes drained.
 * @retval k_ra8_err_no_data Pipe was empty.
 * @retval k_ra8_err_null_ptr `buf` or `got_len` was NULL.
 * @retval k_ra8_err_invalid_state Driver not initialized.
 * @retval k_ra8_err_invalid_arg `max_len == 0`.
 *
 * @pre `ra8_usb_paud_init` succeeded.
 *
 * @post On success `*got_len` reflects the actual byte count.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_usb_paud_recv_frame(uint8_t* buf, uint16_t max_len, uint16_t* got_len);

/* =============================================================================
 * Format / volume helpers
 * =============================================================================
 */

/**
 * @brief Apply a SET_CUR(sampling-frequency) shadow update.
 *
 * @details
 * Used by the class-setup callback (or directly by the application)
 * to reflect a new sample-rate negotiated by the host. USB Audio 1.0
 * sec 5.2.3.2.3.1 "Sampling Frequency Control".
 *
 * @param[in] format New format triplet.
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok Shadow updated.
 * @retval k_ra8_err_invalid_state Driver not initialized.
 * @retval k_ra8_err_invalid_arg Format fields are zero / out of range.
 *
 * @pre `ra8_usb_paud_init` succeeded.
 *
 * @post `ra8_usb_paud_get_format` reflects the new value.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_usb_paud_set_format(ra8_usb_paud_format_t format);

/**
 * @brief Read the current iso stream format.
 *
 * @param[out] out_format Receives the current format.
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok Value copied.
 * @retval k_ra8_err_invalid_state Driver not initialized.
 * @retval k_ra8_err_null_ptr `out_format` was NULL.
 *
 * @pre `out_format` non-NULL.
 *
 * @post No internal state mutated.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_usb_paud_get_format(ra8_usb_paud_format_t* out_format);

/**
 * @brief Apply a SET_CUR(volume) shadow update on the feature unit.
 *
 * @details Volume on the audio feature unit is a 16-bit signed dB
 * value with 1/256 dB resolution (USB Audio 1.0 sec 5.2.2.4.3.2
 * "Volume Control"). 0x8000 means "silence" / mute floor.
 *
 * @param[in] volume_q8_8 New volume in Q8.8 dB format.
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok Shadow updated.
 * @retval k_ra8_err_invalid_state Driver not initialized.
 *
 * @pre `ra8_usb_paud_init` succeeded.
 *
 * @post `ra8_usb_paud_get_volume` reflects the new value.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_usb_paud_set_volume(int16_t volume_q8_8);

/**
 * @brief Read the current feature-unit volume.
 *
 * @param[out] out_volume Receives the current volume in Q8.8 dB.
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok Value copied.
 * @retval k_ra8_err_invalid_state Driver not initialized.
 * @retval k_ra8_err_null_ptr `out_volume` was NULL.
 *
 * @pre `out_volume` non-NULL.
 *
 * @post No internal state mutated.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_usb_paud_get_volume(int16_t* out_volume);

/* =============================================================================
 * Setup-handler attach
 * =============================================================================
 */

/**
 * @brief Register the application's audio class-setup handler.
 *
 * @details
 * Pass `NULL` for `setup_fn` to detach. The class layer drains the
 * SETUP envelope from the controller and forwards it if its
 * `bmRequestType` indicates a class-recipient-interface or
 * class-recipient-endpoint request and its `bRequest` is one of the
 * audio class request codes (USB Audio 1.0 sec A.9).
 *
 * @param[in] setup_fn Application's handler. NULL detaches.
 * @param[in] ctx Context pointer threaded back into `setup_fn`.
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok Handler installed.
 * @retval k_ra8_err_invalid_state Driver not initialized.
 *
 * @pre `ra8_usb_paud_init` succeeded.
 *
 * @post On the next class SETUP, `setup_fn(ctx, &setup)` fires.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_usb_paud_attach_setup_handler(ra8_usb_paud_setup_fn_t setup_fn,
                                                          void*                   ctx);

/* =============================================================================
 * Class SETUP dispatch
 * =============================================================================
 */

/**
 * @brief Process a class-specific SETUP packet on EP0.
 *
 * @details
 * Switches on `b_request` and forwards the SETUP to the registered
 * application handler if any. Standard (non-class) SETUPs are rejected
 * with `k_ra8_err_not_supported` so the caller can fall back to its
 * own standard-request handler.
 *
 * @param[in] setup The SETUP packet returned by `ra8_usb_read_setup_if_valid`.
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok SETUP handled (status stage queued internally).
 * @retval k_ra8_err_invalid_state Driver not initialized.
 * @retval k_ra8_err_null_ptr `setup` was NULL.
 * @retval k_ra8_err_not_supported `bRequest` is not an audio class
 *         request this layer cares about.
 *
 * @pre `ra8_usb_paud_init` succeeded.
 *
 * @post Internal shadows may have been updated by the callback.
 *
 * @note Call from the `CTRT` ISR path of `ra8_usb`.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_usb_paud_handle_setup(const ra8_usb_setup_t* setup);

#ifdef __cplusplus
}
#endif
