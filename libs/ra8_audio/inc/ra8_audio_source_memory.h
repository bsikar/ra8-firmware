/**
 * @file ra8_audio_source_memory.h
 * @brief Fixed in-memory PCM replay backend for `ra8_audio`.
 * @ingroup grp_audio
 * @details Binds an immutable caller-owned PCM frame as a deterministic source
 *          for replay, tests, and transport-independent processing pipelines.
 *
 * @par Tag
 * [Ring 4 / Service] {World: NS}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#include "ra8_audio.h"

/** @brief Caller-owned state for one immutable PCM replay frame. */
typedef struct {
  ra8_audio_frame_t frame; /**< Borrowed fixed frame (private). */
} ra8_audio_source_memory_state_t;

/**
 * @brief Bind one immutable PCM frame as a repeatable audio source.
 * @param[out] source Caller-owned source handle.
 * @param[out] state Caller-owned backend state.
 * @param[in] frame Valid immutable PCM frame retained by the caller.
 * @return Error code.
 * @retval k_ra8_ok Source bound.
 * @retval k_ra8_err_null_ptr An argument was `nullptr`.
 * @retval other Propagated frame-validation error.
 * @pre `frame->data` remains readable until the source is stopped.
 * @post Captures copy the fixed frame into caller storage.
 * @note Not thread-safe with respect to the same state.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_audio_source_memory_init(ra8_audio_source_t*              source,
                                                     ra8_audio_source_memory_state_t* state,
                                                     const ra8_audio_frame_t*         frame);
