/**
 * @file ra8_audio_source_pdm.h
 * @brief RA8 PDM-IF backend for the transport-neutral audio facade.
 * @ingroup grp_audio
 *
 * @par Tag
 * [Ring 4 / Service] {World: NS}
 *
 * @details Binds one PDM-IF channel as a fixed-size mono PCM-S32LE source.
 * Filter coefficients and board pin routing remain caller/BSP policy.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#include <stdint.h>

#include "ra8_audio.h"
#include "ra8_pdm.h"

/** @brief Complete PDM source initialization and capture policy. */
typedef struct {
  ra8_pdm_channel_cfg_t pdm;               /**< HAL filter configuration.      */
  uint32_t              sample_rate_hz;    /**< Decimated PCM sample rate.     */
  uint32_t              samples_per_frame; /**< Samples returned per capture.  */
  uint32_t              settle_ms;         /**< Mic/filter startup delay.      */
  uint32_t              discard_samples;   /**< Startup samples to discard.    */
  uint32_t              poll_attempts;     /**< Empty-FIFO retry bound/frame.  */
  uint8_t               channel;           /**< `ra8_pdm_channel_t` selector.  */
  uint8_t               valid_bits;        /**< Significant bits in PCM-S32LE. */
  uint8_t               irq_priority;      /**< Async-stream NVIC priority.    */
} ra8_audio_source_pdm_cfg_t;

/** @brief Caller-owned mutable state for one PDM source backend. */
typedef struct {
  ra8_audio_source_info_t    info;                /**< Fixed output contract.       */
  uint32_t                   poll_attempts;       /**< Empty-FIFO retry bound.      */
  void*                      stream_data;         /**< Persistent stream scratch.   */
  ra8_audio_frame_callback_t stream_callback;     /**< Complete-frame callback.     */
  void*                      stream_ctx;          /**< Callback context.            */
  uint32_t                   stream_filled;       /**< Samples currently assembled. */
  uint32_t                   stream_timestamp_ms; /**< Current frame timestamp.     */
  uint8_t                    channel;             /**< Bound PDM channel.           */
  uint8_t                    irq_priority;        /**< Async-stream NVIC priority.  */
  bool                       initialized;         /**< Hardware source is running.  */
  bool                       streaming;           /**< Interrupt delivery enabled.  */
} ra8_audio_source_pdm_state_t;

/**
 * @brief Configure, start, settle, and bind one PDM audio source.
 * @param[out] source Generic source handle.
 * @param[out] state Caller-owned backend state.
 * @param[in] cfg PDM filter and fixed-frame policy.
 * @return Error code.
 * @retval k_ra8_ok Source is running and bound.
 * @retval k_ra8_err_null_ptr An argument was `nullptr`.
 * @retval k_ra8_err_invalid_arg Configuration is invalid.
 * @retval other Propagated PDM HAL error.
 * @pre PDM pins are routed and `ra8_time_init` has run.
 * @pre `source` and `state` out-live all later operations.
 * @post On success no heap is used and captures return PCM-S32LE.
 * @post On error hardware is stopped/deinitialized on a best-effort basis.
 * @note Not thread-safe with respect to the PDM-IF block.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_audio_source_pdm_init(ra8_audio_source_t*               source,
                                                  ra8_audio_source_pdm_state_t*     state,
                                                  const ra8_audio_source_pdm_cfg_t* cfg);
