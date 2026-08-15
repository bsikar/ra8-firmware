/**
 * @file ra8_audio_internal.h
 * @brief Private audio-source vtable shared by concrete backends.
 * @ingroup grp_audio
 * @details Defines the minimal capture, optional streaming, and stop rows used
 *          internally by the public transport-neutral audio facade.
 *
 * @par Tag
 * [Ring 4 / Service] {World: NS}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#include "ra8_attributes.h"
#include "ra8_audio.h"

/** @brief Operations supplied by every audio source backend. */
struct ra8_audio_source_iface {
  ra8_err_t (*get_info)(void* ctx, ra8_audio_source_info_t* out_info); /**< Query output. */
  ra8_err_t (*capture)(void*                     ctx,
                       const ra8_audio_buffer_t* buffer,
                       ra8_audio_frame_t*        out_frame); /**< Capture PCM. */
  ra8_err_t (*stream_start)(void*                      ctx,
                            const ra8_audio_buffer_t*  buffer,
                            ra8_audio_frame_callback_t callback,
                            void*                      callback_ctx); /**< Start streaming. */
  ra8_err_t (*stop)(void* ctx);                                       /**< Stop backend.    */
};
