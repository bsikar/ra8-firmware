/**
 * @file ra8_usb_haud.h
 * @brief Native USB host-side Audio class layer (USB Audio 1.0)
 * @ingroup grp_hal_usb
 *
 * @details
 * Glues the host-mode bring-up paths in `ra8_usb` to a USB Audio class
 * peripheral - typically a USB speaker, headphones, or microphone -
 * attached on the EK-RA8D2's USB-host port. Mirrors FSP's `r_usb_haud`
 * host-Audio class flow but compiled as part of this tree with no
 * FSP / CherryUSB / TinyUSB binaries pulled in.
 *
 * Lifecycle (mirrors `ra8_usb_hcdc.h` / `ra8_usb_hhid.h` shape):
 *
 *  1. `ra8_usb_haud_init(speed)` flips the controller to host mode,
 *     primes the internal step machine to idle, and leaves the bus in
 *     the "wait for attach" state (UACT cleared).
 *  2. The driver runs the chapter-9 enumeration step machine over the
 *     DCP, walks the configuration descriptor for the Audio-Control
 *     interface (class=0x01 / subclass=0x01), then for at least one
 *     Audio-Streaming interface (class=0x01 / subclass=0x02), and
 *     pulls out the isochronous IN / OUT endpoints. Optionally walks
 *     the class-specific AC interface descriptors (Input Terminal,
 *     Output Terminal, Feature Unit, Selector Unit) to capture the
 *     Feature Unit ID needed for volume / mute control.
 *  3. When enumeration completes the registered attach callback fires
 *     once with the discovered isochronous-IN / OUT endpoints, max
 *     packet sizes, sampling-rate range, channel count, and bits-per-
 *     sample.
 *  4. Audio class control transfers (`_set_format`, `_set_volume`,
 *     `_set_mute`) are layered on top of the DCP per USB Audio 1.0
 *     sec 5.2 "Class-Specific Requests".
 *  5. `ra8_usb_haud_send_samples` / `ra8_usb_haud_recv_samples` push and
 *     drain isochronous packets to / from a connected
 *     speaker / microphone.
 *  6. `ra8_usb_haud_close()` drops bus power and releases the device.
 *
 * The starter only tracks a single attached audio device; hubs and
 * multi-streaming-interface composite audio devices are deferred
 * follow-ups.
 *
 * Reference: USB Device Class Definition for Audio Devices revision
 * 1.0 (USB-IF, 1998-03-18) and USB Audio Data Formats 1.0.
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
 * @enum ra8_usb_haud_pipe_t
 * @brief PIPE numbers used by the host-Audio driver for the attached
 *        peripheral's isochronous endpoints.
 *
 * @details The RA8D2 / FSP PIPE assignment rules constrain isochronous
 * pipes to PIPE1..PIPE2. The host-Audio class uses one isochronous-IN
 * pipe (microphone) and one isochronous-OUT pipe (speaker). See USB
 * Audio 1.0 sec 4.5 "Isochronous Endpoint Descriptor".
 */
typedef enum : uint8_t {
  k_ra8_haud_pipe_iso_in  = 1U, /**< PIPE1 -> attached EP iso IN.  */
  k_ra8_haud_pipe_iso_out = 2U, /**< PIPE2 -> attached EP iso OUT. */
} ra8_usb_haud_pipe_t;

/**
 * @enum ra8_usb_haud_class_t
 * @brief Class / subclass triplet that identifies the Audio function
 *        within an attached USB device's descriptor walk.
 *
 * @details Numbered from USB Audio 1.0 sec A "Audio Device Class Codes".
 */
typedef enum : uint8_t {
  k_ra8_haud_class_audio             = 0x01U, /**< AUDIO interface class. */
  k_ra8_haud_subclass_undefined      = 0x00U, /**< SUBCLASS_UNDEFINED.    */
  k_ra8_haud_subclass_audiocontrol   = 0x01U, /**< AUDIOCONTROL.          */
  k_ra8_haud_subclass_audiostreaming = 0x02U, /**< AUDIOSTREAMING.        */
  k_ra8_haud_subclass_midistreaming  = 0x03U, /**< MIDISTREAMING.         */
} ra8_usb_haud_class_t;

/**
 * @enum ra8_usb_haud_cs_desc_t
 * @brief Class-specific descriptor type values (USB Audio 1.0 sec A.4
 *        "Audio Class-Specific Descriptor Types").
 */
typedef enum : uint8_t {
  k_ra8_haud_cs_desc_undefined = 0x20U, /**< CS_UNDEFINED.     */
  k_ra8_haud_cs_desc_device    = 0x21U, /**< CS_DEVICE.        */
  k_ra8_haud_cs_desc_config    = 0x22U, /**< CS_CONFIGURATION. */
  k_ra8_haud_cs_desc_string    = 0x23U, /**< CS_STRING.        */
  k_ra8_haud_cs_desc_interface = 0x24U, /**< CS_INTERFACE.     */
  k_ra8_haud_cs_desc_endpoint  = 0x25U, /**< CS_ENDPOINT.      */
} ra8_usb_haud_cs_desc_t;

/**
 * @enum ra8_usb_haud_ac_subtype_t
 * @brief Class-specific AC interface descriptor subtypes (USB Audio
 *        1.0 sec A.5 "Audio Class-Specific AC Interface Descriptor
 *        Subtypes").
 */
typedef enum : uint8_t {
  k_ra8_haud_ac_header          = 0x01U, /**< HEADER.          */
  k_ra8_haud_ac_input_terminal  = 0x02U, /**< INPUT_TERMINAL.  */
  k_ra8_haud_ac_output_terminal = 0x03U, /**< OUTPUT_TERMINAL. */
  k_ra8_haud_ac_mixer_unit      = 0x04U, /**< MIXER_UNIT.      */
  k_ra8_haud_ac_selector_unit   = 0x05U, /**< SELECTOR_UNIT.   */
  k_ra8_haud_ac_feature_unit    = 0x06U, /**< FEATURE_UNIT.    */
  k_ra8_haud_ac_processing_unit = 0x07U, /**< PROCESSING_UNIT. */
  k_ra8_haud_ac_extension_unit  = 0x08U, /**< EXTENSION_UNIT.  */
} ra8_usb_haud_ac_subtype_t;

/**
 * @enum ra8_usb_haud_as_subtype_t
 * @brief Class-specific AS interface descriptor subtypes (USB Audio
 *        1.0 sec A.6 "Audio Class-Specific AS Interface Descriptor
 *        Subtypes").
 */
typedef enum : uint8_t {
  k_ra8_haud_as_general         = 0x01U, /**< AS_GENERAL.      */
  k_ra8_haud_as_format_type     = 0x02U, /**< FORMAT_TYPE.     */
  k_ra8_haud_as_format_specific = 0x03U, /**< FORMAT_SPECIFIC. */
} ra8_usb_haud_as_subtype_t;

/**
 * @enum ra8_usb_haud_format_type_t
 * @brief Audio-Streaming format-type codes (USB Audio Data Formats 1.0
 *        sec 2 "Format Type Codes").
 */
typedef enum : uint8_t {
  k_ra8_haud_format_type_undefined = 0x00U, /**< FORMAT_TYPE_UNDEFINED. */
  k_ra8_haud_format_type_i         = 0x01U, /**< FORMAT_TYPE_I (PCM).   */
  k_ra8_haud_format_type_ii        = 0x02U, /**< FORMAT_TYPE_II.        */
  k_ra8_haud_format_type_iii       = 0x03U, /**< FORMAT_TYPE_III.       */
} ra8_usb_haud_format_type_t;

/**
 * @enum ra8_usb_haud_request_t
 * @brief Audio class-specific request codes the host issues to the
 *        attached device (USB Audio 1.0 sec A.8 "Audio Class-Specific
 *        Request Codes").
 */
typedef enum : uint8_t {
  k_ra8_haud_req_undefined = 0x00U, /**< REQUEST_CODE_UNDEFINED. */
  k_ra8_haud_req_set_cur   = 0x01U, /**< SET_CUR.                */
  k_ra8_haud_req_get_cur   = 0x81U, /**< GET_CUR.                */
  k_ra8_haud_req_set_min   = 0x02U, /**< SET_MIN.                */
  k_ra8_haud_req_get_min   = 0x82U, /**< GET_MIN.                */
  k_ra8_haud_req_set_max   = 0x03U, /**< SET_MAX.                */
  k_ra8_haud_req_get_max   = 0x83U, /**< GET_MAX.                */
  k_ra8_haud_req_set_res   = 0x04U, /**< SET_RES.                */
  k_ra8_haud_req_get_res   = 0x84U, /**< GET_RES.                */
} ra8_usb_haud_request_t;

/**
 * @enum ra8_usb_haud_fu_control_t
 * @brief Feature Unit control selectors (USB Audio 1.0 sec A.10.2
 *        "Feature Unit Control Selectors"). Encoded in wValue's high
 *        byte of the SET_CUR / GET_CUR request.
 */
typedef enum : uint8_t {
  k_ra8_haud_fu_control_undefined  = 0x00U, /**< FU_CONTROL_UNDEFINED.   */
  k_ra8_haud_fu_control_mute       = 0x01U, /**< MUTE_CONTROL.           */
  k_ra8_haud_fu_control_volume     = 0x02U, /**< VOLUME_CONTROL.         */
  k_ra8_haud_fu_control_bass       = 0x03U, /**< BASS_CONTROL.           */
  k_ra8_haud_fu_control_mid        = 0x04U, /**< MID_CONTROL.            */
  k_ra8_haud_fu_control_treble     = 0x05U, /**< TREBLE_CONTROL.         */
  k_ra8_haud_fu_control_graphic_eq = 0x06U, /**< GRAPHIC_EQ_CONTROL.     */
  k_ra8_haud_fu_control_auto_gain  = 0x07U, /**< AUTOMATIC_GAIN_CONTROL. */
  k_ra8_haud_fu_control_delay      = 0x08U, /**< DELAY_CONTROL.          */
  k_ra8_haud_fu_control_bass_boost = 0x09U, /**< BASS_BOOST_CONTROL.     */
  k_ra8_haud_fu_control_loudness   = 0x0AU, /**< LOUDNESS_CONTROL.       */
} ra8_usb_haud_fu_control_t;

/**
 * @enum ra8_usb_haud_ep_control_t
 * @brief Endpoint Control Selectors (USB Audio 1.0 sec A.10.5).
 *        Encoded in wValue's high byte for SET_CUR on an isochronous
 *        endpoint - this is how the sampling-frequency is changed.
 */
typedef enum : uint8_t {
  k_ra8_haud_ep_control_undefined     = 0x00U, /**< EP_CONTROL_UNDEFINED.  */
  k_ra8_haud_ep_control_sampling_freq = 0x01U, /**< SAMPLING_FREQ_CONTROL. */
  k_ra8_haud_ep_control_pitch         = 0x02U, /**< PITCH_CONTROL.         */
} ra8_usb_haud_ep_control_t;

/**
 * @enum ra8_usb_haud_volume_t
 * @brief Volume-control wire encoding helpers.
 *
 * @details Per USB Audio 1.0 sec 5.2.2.4.3.2 "Volume Control": volume
 * is a signed 16-bit value in 1/256 dB steps. The minimum encoded
 * value (0x8000) is the "silence" sentinel.
 */
typedef enum : int32_t {
  k_ra8_haud_volume_silence = (int32_t)0x8000, /**< Silence sentinel.    */
  k_ra8_haud_volume_min_db  = (int32_t)-32768, /**< -128.0 dB ceiling.   */
  k_ra8_haud_volume_max_db  = (int32_t)32767,  /**< +127.998 dB ceiling. */
} ra8_usb_haud_volume_t;

/**
 * @enum ra8_usb_haud_packet_t
 * @brief Packet sizing for the attached device's isochronous endpoints.
 */
typedef enum : uint16_t {
  k_ra8_haud_iso_max_packet_default = 192U,  /**< Common 48 kHz / 16-bit / stereo. */
  k_ra8_haud_iso_max_packet_fs      = 1023U, /**< FS isochronous ceiling.          */
  k_ra8_haud_iso_max_packet_hs      = 1024U, /**< HS isochronous ceiling.          */
} ra8_usb_haud_packet_t;

/**
 * @enum ra8_usb_haud_default_t
 * @brief Default audio format applied by the descriptor-walk stub when
 *        the attached device follows the canonical USB-headphones
 *        layout.
 */
typedef enum : uint32_t {
  k_ra8_haud_default_sample_rate_hz = 48000U, /**< 48 kHz default. */
  k_ra8_haud_default_channels       = 2U,     /**< Stereo.         */
  k_ra8_haud_default_bits           = 16U,    /**< 16-bit PCM.     */
} ra8_usb_haud_default_t;

/**
 * @enum ra8_usb_haud_format_limit_t
 * @brief Range checks for `ra8_usb_haud_set_format`.
 */
typedef enum : uint32_t {
  k_ra8_haud_min_channels       = 1U,      /**< Mono.                 */
  k_ra8_haud_max_channels       = 8U,      /**< 7.1 surround ceiling. */
  k_ra8_haud_min_bits           = 8U,      /**< 8-bit ceiling.        */
  k_ra8_haud_max_bits           = 32U,     /**< 32-bit ceiling.       */
  k_ra8_haud_min_sample_rate_hz = 8000U,   /**< 8 kHz min.            */
  k_ra8_haud_max_sample_rate_hz = 192000U, /**< 192 kHz max.          */
} ra8_usb_haud_format_limit_t;

/**
 * @struct ra8_usb_haud_device_t
 * @brief Snapshot of the attached audio device, passed to the attach
 *        callback.
 *
 * @details Populated by the host-Audio driver once the descriptor walk
 * completes. The pipe number is the controller-side handle wired up
 * against the attached device's isochronous endpoints. The recipient
 * should hold onto this struct (or copy it) so subsequent format /
 * volume / mute calls land on the right Feature Unit.
 */
typedef struct {
  uint8_t  device_address;     /**< Assigned USB address (1..127).          */
  uint8_t  ac_interface;       /**< AudioControl bInterfaceNumber.          */
  uint8_t  as_interface;       /**< AudioStreaming bInterfaceNumber.        */
  uint8_t  feature_unit_id;    /**< Feature Unit bUnitID (for FU controls). */
  uint8_t  iso_in_ep;          /**< iso-IN EP num (0 if absent).            */
  uint8_t  iso_out_ep;         /**< iso-OUT EP num (0 if absent).           */
  uint8_t  channel_count;      /**< bNrChannels from FORMAT_TYPE_I desc.    */
  uint8_t  bits_per_sample;    /**< bBitResolution from FORMAT_TYPE_I.      */
  uint16_t iso_in_max_packet;  /**< iso-IN wMaxPacketSize.                  */
  uint16_t iso_out_max_packet; /**< iso-OUT wMaxPacketSize.                 */
  uint16_t vendor_id;          /**< idVendor from device descriptor.        */
  uint16_t product_id;         /**< idProduct from device descriptor.       */
  uint32_t sample_rate_min_hz; /**< Minimum tSamFreq from FORMAT_TYPE_I.    */
  uint32_t sample_rate_max_hz; /**< Maximum tSamFreq from FORMAT_TYPE_I.    */
} ra8_usb_haud_device_t;

/**
 * @typedef ra8_usb_haud_attach_fn_t
 * @brief Attach-callback signature.
 *
 * @param[in] ctx Caller-supplied context registered with
 *                `ra8_usb_haud_attach_callback`.
 * @param[in] device Snapshot of the attached audio device. The pointer
 *                   remains valid only for the duration of the call;
 *                   copy out anything you need.
 *
 * @note Invoked from the dispatch site (typically ISR context) after
 * enumeration completes successfully.
 */
typedef void (*ra8_usb_haud_attach_fn_t)(void* ctx, const ra8_usb_haud_device_t* device);

/* =============================================================================
 * Lifecycle
 * =============================================================================
 */

/**
 * @brief Bring up the host-Audio driver on a chosen USB controller.
 *
 * @details
 * Initialises the underlying `ra8_usb` driver in HOST mode for `speed`,
 * leaves the bus in the "wait for attach" state (UACT cleared), and
 * arms the internal enumeration step machine. Delegates to
 * `ra8_usb_host_init`.
 *
 * @param[in] speed Which USB controller (FS or HS).
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok Host-Audio ready, awaiting attach.
 * @retval k_ra8_err_invalid_arg `speed` out of range.
 * @retval k_ra8_err_hw_init_failed Underlying `ra8_usb_host_init` failed.
 *
 * @pre Single-threaded init context.
 * @pre `ra8_mstp_init` and `ra8_pwr_init` already ran.
 *
 * @post `ra8_usb_host_init` succeeded for `speed`.
 * @post Internal step machine armed; attach callback has not fired.
 *
 * @note Not thread-safe.
 * @see ra8_usb_haud_attach_callback
 * @see ra8_usb_haud_close
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_usb_haud_init(ra8_usb_speed_t speed);

/**
 * @brief Tear down the host-Audio driver and release the controller.
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok Released.
 * @retval k_ra8_err_invalid_state Driver was never initialized.
 *
 * @pre Single-threaded shutdown context.
 *
 * @post `ra8_usb_host_deinit` ran; SOF generation halted; bus power
 *       dropped; subsequent host-Audio API calls return
 *       `k_ra8_err_invalid_state`.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_usb_haud_close(void);

/* =============================================================================
 * Attach callback
 * =============================================================================
 */

/**
 * @brief Register (or detach) the attach callback.
 *
 * @details
 * The supplied callback fires exactly once per attach event, after the
 * descriptor walk identifies an Audio-Control interface (class=0x01 /
 * subclass=0x01) and at least one Audio-Streaming interface
 * (class=0x01 / subclass=0x02), and the matching isochronous IN / OUT
 * endpoints and Feature Unit are cached. Pass `NULL` to detach.
 *
 * @param[in] on_attach Callback. NULL detaches.
 * @param[in] ctx Context pointer threaded back into `on_attach`.
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok Callback installed.
 * @retval k_ra8_err_invalid_state Driver was never initialized.
 *
 * @pre `ra8_usb_haud_init` has run.
 *
 * @post On a subsequent attach, `on_attach(ctx, &device)` fires once.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_usb_haud_attach_callback(ra8_usb_haud_attach_fn_t on_attach, void* ctx);

/* =============================================================================
 * Class control transfers
 * =============================================================================
 */

/**
 * @brief Issue SET_CUR on the format-type-I descriptor (USB Audio 1.0
 *        sec 5.2.3 "Class-Specific AS Interface Control Requests").
 *
 * @details
 * Builds an 8-byte SETUP packet with bmRequestType = 0x22 (Class |
 * Endpoint | Host-to-Device), bRequest = 0x01 (SET_CUR), and
 * wValue = (SAMPLING_FREQ_CONTROL << 8). The data stage payload is a
 * 3-byte little-endian sample rate (USB Audio 1.0 sec 5.2.3.2.3.1).
 * Channel count and bits-per-sample are validated and recorded into
 * the cached device snapshot, but the wire transfer for those is
 * already implicit in the chosen Audio-Streaming interface alt
 * setting; this entry point just refreshes the iso-EP sampling-rate.
 *
 * @param[in] channel_count Channels (1..8).
 * @param[in] bits_per_sample 8..32.
 * @param[in] sample_rate_hz 8000..192000.
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok SETUP queued.
 * @retval k_ra8_err_invalid_state Driver not initialized, or no device
 *         attached.
 * @retval k_ra8_err_invalid_arg One of channel_count / bits_per_sample
 *         / sample_rate_hz is out of range.
 * @retval k_ra8_err_busy Controller busy with a prior SETUP.
 *
 * @pre `ra8_usb_haud_init` succeeded.
 * @pre Attach callback already fired.
 *
 * @post On success the SETUP mirror registers hold the SET_CUR
 *       request envelope; the cached format snapshot is updated.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_usb_haud_set_format(uint8_t channel_count, uint8_t bits_per_sample, uint32_t sample_rate_hz);

/**
 * @brief Issue SET_CUR on the Feature Unit Volume control (USB Audio
 *        1.0 sec 5.2.2.4.3 "Volume Control").
 *
 * @details
 * Builds an 8-byte SETUP packet with bmRequestType = 0x21 (Class |
 * Interface | Host-to-Device), bRequest = 0x01 (SET_CUR), and
 * wValue = (VOLUME_CONTROL << 8) | channel. wIndex packs the Feature
 * Unit ID in the high byte and the AudioControl interface in the low
 * byte. Volume is a signed 16-bit value in 1/256 dB steps; 0x8000
 * is the silence sentinel.
 *
 * @param[in] channel Logical channel (0 = main, 1..N per channel).
 * @param[in] volume Signed dB in 1/256 step.
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok SETUP queued.
 * @retval k_ra8_err_invalid_state Driver not initialized, or no device
 *         attached.
 * @retval k_ra8_err_busy Controller busy with a prior SETUP.
 *
 * @pre `ra8_usb_haud_init` succeeded.
 * @pre Attach callback already fired.
 *
 * @post On success the SETUP mirror registers hold the SET_CUR
 *       request envelope.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_usb_haud_set_volume(uint8_t channel, int16_t volume);

/**
 * @brief Issue SET_CUR on the Feature Unit Mute control (USB Audio
 *        1.0 sec 5.2.2.4.3.1 "Mute Control").
 *
 * @details
 * Builds an 8-byte SETUP packet with bmRequestType = 0x21 (Class |
 * Interface | Host-to-Device), bRequest = 0x01 (SET_CUR), and
 * wValue = (MUTE_CONTROL << 8) | channel. wIndex packs the Feature
 * Unit ID in the high byte and the AudioControl interface in the low
 * byte. Mute is a one-byte boolean (0 = unmute, 1 = mute).
 *
 * @param[in] channel Logical channel (0 = main, 1..N per channel).
 * @param[in] mute true = mute, false = unmute.
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok SETUP queued.
 * @retval k_ra8_err_invalid_state Driver not initialized, or no device
 *         attached.
 * @retval k_ra8_err_busy Controller busy with a prior SETUP.
 *
 * @pre `ra8_usb_haud_init` succeeded.
 * @pre Attach callback already fired.
 *
 * @post On success the SETUP mirror registers hold the SET_CUR
 *       request envelope.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_usb_haud_set_mute(uint8_t channel, bool mute);

/* =============================================================================
 * Isochronous data transfer
 * =============================================================================
 */

/**
 * @brief Push isochronous-OUT samples to a speaker / headphones.
 *
 * @details
 * Streams `buf[0..len_bytes-1]` onto the iso-OUT pipe configured for
 * the attached Audio-Streaming interface. PCM byte-order matches the
 * Audio-Streaming format (little-endian per USB Audio Data Formats
 * 1.0 sec 2.2). Non-blocking; bytes that don't fit in the current
 * micro-frame return `k_ra8_err_busy` for the caller to retry.
 *
 * @param[in] buf Sample byte buffer.
 * @param[in] len_bytes Number of bytes in `buf`.
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok Bytes queued for the next iso-OUT slot.
 * @retval k_ra8_err_null_ptr `buf == NULL` and `len_bytes != 0`.
 * @retval k_ra8_err_invalid_state Driver not initialized, no device
 *         attached, or device has no iso-OUT endpoint.
 * @retval k_ra8_err_invalid_arg `len_bytes == 0`.
 * @retval k_ra8_err_busy iso-OUT pipe busy.
 *
 * @pre `ra8_usb_haud_init` succeeded.
 * @pre Attach callback already fired with an iso-OUT endpoint.
 *
 * @post On success bytes queued onto PIPE2.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_usb_haud_send_samples(const uint8_t* buf, uint16_t len_bytes);

/**
 * @brief Drain isochronous-IN samples from a microphone.
 *
 * @details
 * Polling / non-blocking. Pulls bytes from the configured PIPE1
 * (isochronous IN) into `buf`. Returns `k_ra8_err_no_data` if the pipe
 * has no bytes ready in this micro-frame.
 *
 * @param[out] buf Destination buffer.
 * @param[in] max_len_bytes Capacity of `buf`, > 0.
 * @param[out] got_len_bytes Receives the number of bytes actually placed.
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok Bytes drained; `*got_len_bytes` reflects the count.
 * @retval k_ra8_err_no_data Pipe was empty.
 * @retval k_ra8_err_null_ptr `buf` or `got_len_bytes` was NULL.
 * @retval k_ra8_err_invalid_state Driver not initialized, no device
 *         attached, or device has no iso-IN endpoint.
 * @retval k_ra8_err_invalid_arg `max_len_bytes == 0`.
 *
 * @pre `ra8_usb_haud_init` succeeded.
 * @pre Attach callback already fired with an iso-IN endpoint.
 *
 * @post On success `*got_len_bytes` reflects the actual byte count.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_usb_haud_recv_samples(uint8_t* buf, uint16_t max_len_bytes, uint16_t* got_len_bytes);

/* =============================================================================
 * Test / introspection helpers
 * =============================================================================
 */

/**
 * @brief Drive the enumeration step machine forward by one step.
 *
 * @details
 * Test / debug entry point. The production path drives this from the
 * `ra8_usb_dispatch` callback when the controller fires a CTRT or BRDY
 * interrupt; tests call it directly to walk the state machine
 * deterministically.
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok Step advanced.
 * @retval k_ra8_err_invalid_state Driver not initialized.
 *
 * @pre `ra8_usb_haud_init` succeeded.
 *
 * @post Internal enumeration step counter advances by one. When the
 *       step machine reaches the terminal state the registered
 *       attach callback fires.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_usb_haud_step(void);

#ifdef __cplusplus
}
#endif
