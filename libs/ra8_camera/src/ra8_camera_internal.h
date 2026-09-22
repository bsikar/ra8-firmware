/**
 * @file ra8_camera_internal.h
 * @brief Private source and codec vtables for `ra8_camera` backends.
 * @ingroup grp_camera
 *
 * @par Tag
 * [Ring 4 / Service] {World: NS}
 *
 * @details Applications see only forward declarations in `ra8_camera.h`.
 *          Concrete backends bind one of these tables through their `_init()`
 *          helper, exactly as `ra8_io_stream` sinks bind their private vtable.
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

/** @brief Operations every source backend supplies. */
struct ra8_camera_source_iface {
  ra8_err_t (*get_info)(void* ctx, ra8_camera_source_info_t* out_info); /**< Query geometry. */
  ra8_err_t (*capture)(void*                      ctx,
                       const ra8_camera_buffer_t* buffer,
                       ra8_camera_frame_t*        out_frame); /**< Capture one frame. */
};

/** @brief Operation every codec backend supplies. */
struct ra8_camera_codec_iface {
  ra8_err_t (*encode)(void*                      ctx,
                      const ra8_camera_frame_t*  input,
                      const ra8_camera_buffer_t* output_buffer,
                      ra8_camera_frame_t*        out_frame); /**< Produce one encoded frame. */
};

#ifdef __cplusplus
}
#endif
