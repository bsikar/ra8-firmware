/**
 * @file ra_cec.h
 * @brief HDMI Consumer Electronics Control (CEC) driver -- placeholder
 *
 * @details
 * Mirrors the FSP `r_cec` API shape (open / close / send / receive /
 * status / attach handler). The CEC peripheral is the single-wire
 * HDMI control bus that carries 10-bit framed bytes between
 * consumer-electronics devices over HDMI pin 13. Each CEC frame is
 * a sequence of (initiator, follower, opcode, payload...) bytes.
 *
 * @warning The Renesas RA8D2 silicon does not carry a CEC block;
 *          this file ships a host-testable placeholder so portable
 *          code keeps compiling and unit tests can exercise the
 *          driver state machine without real hardware. CEC service
 *          on EK-RA8D2 is expected to be done through the off-chip
 *          HDMI transmitter or via the GLCDC/MIPI sideband.
 *
 * Reference: FSP `r_cec` driver shape and `r_cec_api.h`.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra_err.h"

/**
 * @enum ra_cec_logical_addr_t
 * @brief Logical CEC device address (HDMI CEC v1.4 Table 7).
 */
typedef enum : uint8_t {
  k_ra_cec_addr_tv                 = 0U,
  k_ra_cec_addr_recording_device_1 = 1U,
  k_ra_cec_addr_recording_device_2 = 2U,
  k_ra_cec_addr_tuner_1            = 3U,
  k_ra_cec_addr_playback_device_1  = 4U,
  k_ra_cec_addr_audio_system       = 5U,
  k_ra_cec_addr_tuner_2            = 6U,
  k_ra_cec_addr_tuner_3            = 7U,
  k_ra_cec_addr_playback_device_2  = 8U,
  k_ra_cec_addr_recording_device_3 = 9U,
  k_ra_cec_addr_tuner_4            = 10U,
  k_ra_cec_addr_playback_device_3  = 11U,
  k_ra_cec_addr_specific_use       = 14U,
  k_ra_cec_addr_unregistered       = 15U,
  k_ra_cec_addr_broadcast          = 15U,
} ra_cec_logical_addr_t;

/**
 * @enum ra_cec_state_t
 * @brief Driver lifecycle state.
 */
typedef enum : uint8_t {
  k_ra_cec_state_uninit    = 0U,
  k_ra_cec_state_ready     = 1U,
  k_ra_cec_state_tx_active = 2U,
  k_ra_cec_state_rx_active = 3U,
} ra_cec_state_t;

/**
 * @enum ra_cec_payload_limit_t
 * @brief Maximum payload bytes carried by a single CEC frame.
 *
 * @details The HDMI CEC standard caps payload at 14 data bytes
 *          plus the 1-byte (initiator, follower) header.
 */
typedef enum : uint8_t {
  k_ra_cec_payload_max = 14U,
} ra_cec_payload_limit_t;

/**
 * @struct ra_cec_message_t
 * @brief One HDMI CEC frame in transit.
 *
 * @code
 * ra_cec_message_t m = {
 *     .source = k_ra_cec_addr_playback_device_1,
 *     .destination = k_ra_cec_addr_tv,
 *     .opcode = 0x36U, // <Standby>
 *     .payload_len = 0U,
 * };
 * @endcode
 */
typedef struct {
  ra_cec_logical_addr_t source;                        /**< 4-bit initiator address.    */
  ra_cec_logical_addr_t destination;                   /**< 4-bit follower address.     */
  uint8_t               opcode;                        /**< CEC opcode byte.            */
  uint8_t               payload_len;                   /**< Bytes valid in `payload`.   */
  uint8_t               payload[k_ra_cec_payload_max]; /**< Data.    */
} ra_cec_message_t;

/**
 * @struct ra_cec_status_t
 * @brief Snapshot of CEC driver / bus state.
 */
typedef struct {
  ra_cec_state_t        state;         /**< Driver state.             */
  ra_cec_logical_addr_t local_address; /**< Currently allocated addr. */
  uint16_t              error_flags;   /**< Sticky error bitfield.    */
  uint32_t              tx_count;      /**< Successful TX frames.     */
  uint32_t              rx_count;      /**< Successful RX frames.     */
} ra_cec_status_t;

/**
 * @brief Receive-handler callback signature.
 *
 * @param[in] msg     Newly-received frame.
 * @param[in] context User-supplied opaque pointer registered alongside.
 */
typedef void (*ra_cec_rx_handler_t)(const ra_cec_message_t* msg, void* context);

/**
 * @struct ra_cec_cfg_t
 * @brief Configuration for `ra_cec_open`.
 */
typedef struct {
  ra_cec_logical_addr_t local_address; /**< Allocated logical addr.   */
  uint16_t              clock_div;     /**< Source-clock divisor.     */
} ra_cec_cfg_t;

/**
 * @brief Open the CEC peripheral.
 *
 * @details Initialises the placeholder register block and parks the
 *          driver in `k_ra_cec_state_ready`. Mirrors `R_CEC_Open` plus
 *          the equivalent of `R_CEC_MediaInit` (single-call open).
 *
 * @param[in] cfg Configuration descriptor. Must not be `nullptr`.
 *
 * @return `ra_err_t`.
 * @retval k_ra_ok                  Opened successfully.
 * @retval k_ra_err_null_ptr        `cfg` was `nullptr`.
 * @retval k_ra_err_invalid_arg     Address out of the 0..15 range.
 * @retval k_ra_err_exists          Already opened.
 *
 * @pre Single-threaded init context.
 * @pre `cfg->clock_div` is non-zero.
 * @post Driver is in `k_ra_cec_state_ready`.
 * @post `ra_cec_close` will succeed.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_cec_open(const ra_cec_cfg_t* cfg);

/**
 * @brief Close the CEC peripheral.
 *
 * @return `ra_err_t`.
 * @retval k_ra_ok                  Closed.
 * @retval k_ra_err_invalid_state   Driver was never opened.
 *
 * @pre Driver is open.
 * @post Driver is in `k_ra_cec_state_uninit`.
 * @post Subsequent `ra_cec_send_message` returns `k_ra_err_not_initialized`.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_cec_close(void);

/**
 * @brief Send an HDMI CEC message frame.
 *
 * @details Stages the (source, destination) header byte plus opcode
 *          and payload into the placeholder TX buffer. The placeholder
 *          implementation simulates an immediate completion and bumps
 *          the success counter so the host test build can validate
 *          the frame layout without real silicon.
 *
 * @param[in] msg Frame to transmit. Must not be `nullptr`.
 *
 * @return `ra_err_t`.
 * @retval k_ra_ok                   Frame queued / transmitted.
 * @retval k_ra_err_null_ptr         `msg` was `nullptr`.
 * @retval k_ra_err_invalid_arg      Address out of range or payload too long.
 * @retval k_ra_err_not_initialized  Driver not opened.
 *
 * @pre `ra_cec_open` succeeded.
 * @pre `msg->payload_len <= k_ra_cec_payload_max`.
 * @post On success, `tx_count` is incremented.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_cec_send_message(const ra_cec_message_t* msg);

/**
 * @brief Pull the most-recent received frame, if any.
 *
 * @param[out] out Receives the buffered RX frame on success. Must not
 *                 be `nullptr`.
 *
 * @return `ra_err_t`.
 * @retval k_ra_ok                   Frame copied to `*out`.
 * @retval k_ra_err_null_ptr         `out` was `nullptr`.
 * @retval k_ra_err_no_data          No buffered frame.
 * @retval k_ra_err_not_initialized  Driver not opened.
 *
 * @pre `ra_cec_open` succeeded.
 * @post On success, the internal RX slot is consumed.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_cec_receive_message(ra_cec_message_t* out);

/**
 * @brief Register a receive-event callback.
 *
 * @param[in] handler Callback to invoke on RX completion. May be
 *                    `nullptr` to clear an existing handler.
 * @param[in] context Opaque pointer passed back to `handler`.
 *
 * @return `ra_err_t`.
 * @retval k_ra_ok                   Handler installed.
 * @retval k_ra_err_not_initialized  Driver not opened.
 *
 * @pre `ra_cec_open` succeeded.
 * @post Subsequent simulated RX events invoke `handler(msg, context)`.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_cec_attach_handler(ra_cec_rx_handler_t handler, void* context);

/**
 * @brief Snapshot the driver / bus status.
 *
 * @param[out] out Receives the status snapshot. Must not be `nullptr`.
 *
 * @return `ra_err_t`.
 * @retval k_ra_ok                   Status copied.
 * @retval k_ra_err_null_ptr         `out` was `nullptr`.
 * @retval k_ra_err_not_initialized  Driver not opened.
 *
 * @pre Pointer references writable memory.
 * @post `*out` reflects current state.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_cec_status_get(ra_cec_status_t* out);

/**
 * @brief Test-only hook to inject a synthetic RX frame.
 *
 * @details Visible to host unit tests so they can exercise the
 *          handler-dispatch path without poking real hardware. On
 *          target silicon (no CEC peripheral) this is the only way
 *          a frame ever lands in the RX slot.
 *
 * @param[in] msg Frame to inject. Must not be `nullptr`.
 *
 * @return `ra_err_t`.
 * @retval k_ra_ok                   Frame stored.
 * @retval k_ra_err_null_ptr         `msg` was `nullptr`.
 * @retval k_ra_err_not_initialized  Driver not opened.
 * @retval k_ra_err_invalid_arg      Payload too long.
 *
 * @pre `ra_cec_open` succeeded.
 * @post On success, `rx_count` is incremented.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_cec_inject_rx(const ra_cec_message_t* msg);

#ifdef __cplusplus
}
#endif
