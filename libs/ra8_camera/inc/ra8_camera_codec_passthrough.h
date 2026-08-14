/**
 * @file ra8_camera_codec_passthrough.h
 * @brief Zero-copy JPEG passthrough codec backend.
 * @ingroup grp_camera
 *
 * @par Tag
 * [Ring 4 / Service] {World: NS}
 *
 * @details A source which already produces complete JPEG frames should not be
 * decoded and re-encoded. This codec validates that the input format is JPEG
 * and returns an exact view of the same bytes without using output storage.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "ra8_camera.h"
#include "ra8_err.h"

/**
 * @brief Bind the zero-copy JPEG passthrough codec.
 *
 * @param[out] codec Caller-owned codec handle to bind.
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok           Codec bound.
 * @retval k_ra8_err_null_ptr `codec` was NULL.
 * @pre `codec` out-lives every codec operation.
 * @pre The caller does not mutate the handle while encoding.
 * @post On success JPEG input is returned byte-for-byte without copying.
 * @post On error no state is modified.
 * @note Stateless; independent handles are thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_camera_codec_passthrough_init(ra8_camera_codec_t* codec);

#ifdef __cplusplus
}
#endif
