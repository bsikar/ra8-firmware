/**
 * @file ra8_audio.h
 * @brief Transport-neutral, caller-owned audio capture facade.
 * @ingroup grp_audio
 *
 * @par Tag
 * [Ring 4 / Service] {World: NS}
 *
 * @details Audio sources capture fixed-size PCM frames into storage supplied by
 * the caller. Concrete backends bind an opaque vtable through an `_init()`
 * helper. Handles, backend state, and sample buffers are never allocated or
 * freed by this library.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_err.h"

/** @brief Opaque source vtable implemented by concrete audio backends. */
typedef struct ra8_audio_source_iface ra8_audio_source_iface_t;

/**
 * @enum ra8_audio_format_t
 * @brief PCM container layouts supported by the facade.
 * @invariant Each value has a stable one-byte representation.
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_ra8_audio_format_pcm_s16le = 0U, /**< Signed 16-bit little-endian PCM. */
  k_ra8_audio_format_pcm_s32le,      /**< Signed 32-bit little-endian PCM. */
} ra8_audio_format_t;

/** @brief Caller-owned writable byte buffer. */
typedef struct {
  void*    data;     /**< First writable byte.        */
  uint32_t capacity; /**< Writable capacity in bytes. */
} ra8_audio_buffer_t;

/**
 * @struct ra8_audio_frame_t
 * @brief Immutable view of one interleaved PCM frame.
 * @invariant `bytes == sample_count * channels * container_bytes`.
 * @invariant `valid_bits` does not exceed the selected container width.
 * @since 0.1.0
 */
typedef struct {
  const void*        data;           /**< Borrowed interleaved PCM bytes.       */
  uint32_t           bytes;          /**< Valid byte count.                     */
  uint32_t           sample_count;   /**< Sample frames per channel.            */
  uint32_t           sample_rate_hz; /**< Sample frames per second.             */
  uint32_t           timestamp_ms;   /**< Capture-start monotonic milliseconds. */
  uint8_t            channels;       /**< Interleaved channel count.            */
  uint8_t            valid_bits;     /**< Significant bits per sample.          */
  ra8_audio_format_t format;         /**< PCM container layout.                 */
} ra8_audio_frame_t;

/** @brief Fixed output contract and storage requirement for one source. */
typedef struct {
  uint32_t           frame_bytes;       /**< Required capture-buffer capacity. */
  uint32_t           samples_per_frame; /**< Sample frames returned per call.  */
  uint32_t           sample_rate_hz;    /**< Sample frames per second.         */
  uint8_t            channels;          /**< Interleaved channel count.        */
  uint8_t            valid_bits;        /**< Significant bits per sample.      */
  ra8_audio_format_t format;            /**< PCM container layout.             */
} ra8_audio_source_info_t;

/** @brief Caller-owned handle binding an audio source backend and its state. */
typedef struct {
  const ra8_audio_source_iface_t* iface; /**< Bound source vtable (private). */
  void*                           ctx;   /**< Caller-owned backend state.    */
} ra8_audio_source_t;

/**
 * @typedef ra8_audio_frame_callback_t
 * @brief Complete-frame callback for asynchronous audio sources.
 * @param[in] ctx Caller context supplied to stream start.
 * @param[in] frame Borrowed complete PCM frame.
 * @pre `frame` and `frame->data` remain valid only during the callback.
 * @post The callback must copy or consume the frame before returning.
 * @note The backend defines callback context; PDM invokes it from ISR context.
 * @since 0.1.0
 */
typedef void (*ra8_audio_frame_callback_t)(void* ctx, const ra8_audio_frame_t* frame);

/**
 * @brief Validate one PCM frame view.
 * @param[in] frame Candidate frame.
 * @return Error code.
 * @retval k_ra8_ok Frame metadata and byte coverage are valid.
 * @retval k_ra8_err_null_ptr `frame` or `frame->data` is `nullptr`.
 * @retval k_ra8_err_invalid_arg Format or scalar metadata is invalid.
 * @retval k_ra8_err_invalid_size Byte coverage does not match metadata.
 * @pre When non-NULL, `frame->data` is readable for `frame->bytes` bytes.
 * @post No state is modified.
 * @note Thread-safe; reads only its argument.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_audio_frame_validate(const ra8_audio_frame_t* frame);

/**
 * @brief Query a bound source's fixed output contract.
 * @param[in,out] source Bound source handle.
 * @param[out] out_info Receives source metadata.
 * @return Error code.
 * @retval k_ra8_ok Metadata returned.
 * @retval k_ra8_err_null_ptr An argument was `nullptr`.
 * @retval k_ra8_err_not_initialized No source backend is bound.
 * @retval other Propagated backend error.
 * @pre `out_info` points to writable storage.
 * @post On error `out_info` is zeroed.
 * @note Not thread-safe with respect to the same source.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_audio_source_get_info(ra8_audio_source_t*      source,
                                                  ra8_audio_source_info_t* out_info);

/**
 * @brief Capture one fixed-size PCM frame into caller-owned storage.
 * @param[in,out] source Bound source handle.
 * @param[in] buffer Writable PCM storage.
 * @param[out] out_frame Receives a borrowed immutable frame view.
 * @return Error code.
 * @retval k_ra8_ok One complete frame captured.
 * @retval k_ra8_err_null_ptr An argument or buffer pointer was `nullptr`.
 * @retval k_ra8_err_not_initialized No source backend is bound.
 * @retval k_ra8_err_invalid_size Buffer cannot hold the source frame.
 * @retval other Propagated backend error.
 * @pre No other call uses `source` or `buffer`.
 * @post On success `out_frame->data == buffer->data` and validates.
 * @post On error `out_frame` is zeroed.
 * @note Not thread-safe with respect to the same source or buffer.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_audio_source_capture(ra8_audio_source_t*       source,
                                                 const ra8_audio_buffer_t* buffer,
                                                 ra8_audio_frame_t*        out_frame);

/**
 * @brief Start asynchronous fixed-frame delivery into caller-owned scratch.
 * @param[in,out] source Bound source handle.
 * @param[in] buffer Persistent writable frame storage.
 * @param[in] callback Complete-frame callback.
 * @param[in] ctx Opaque callback context; may be `nullptr`.
 * @return Error code.
 * @retval k_ra8_ok Streaming started.
 * @retval k_ra8_err_null_ptr Required argument or buffer pointer was `nullptr`.
 * @retval k_ra8_err_not_initialized Source is unbound.
 * @retval k_ra8_err_not_supported Backend has no streaming operation.
 * @retval k_ra8_err_invalid_size Buffer cannot hold one source frame.
 * @retval other Propagated backend error.
 * @pre `buffer` and its storage out-live the stream.
 * @post Complete frames are delivered until ::ra8_audio_source_stop.
 * @note Callback execution context is backend-specific and must be respected.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_audio_source_stream_start(ra8_audio_source_t*        source,
                                                      const ra8_audio_buffer_t*  buffer,
                                                      ra8_audio_frame_callback_t callback,
                                                      void*                      ctx);

/**
 * @brief Stop and release a bound source backend.
 * @param[in,out] source Bound source handle.
 * @return Error code.
 * @retval k_ra8_ok Backend stopped and handle cleared.
 * @retval k_ra8_err_null_ptr `source` was `nullptr`.
 * @retval k_ra8_err_not_initialized No source backend is bound.
 * @retval other Propagated backend error; the handle remains bound for retry.
 * @pre No capture is in progress.
 * @post On success `source` is reset to an unbound state.
 * @note Not thread-safe with respect to the same source.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_audio_source_stop(ra8_audio_source_t* source);

#ifdef __cplusplus
}
#endif
