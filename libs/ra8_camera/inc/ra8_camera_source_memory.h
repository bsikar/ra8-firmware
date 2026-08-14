/**
 * @file ra8_camera_source_memory.h
 * @brief Fixed in-memory frame source backend for `ra8_camera`.
 * @ingroup grp_camera
 *
 * @par Tag
 * [Ring 4 / Service] {World: NS}
 *
 * @details Binds one immutable frame as a repeatable camera source. Each
 * capture copies the fixed frame into the caller's capture buffer. Besides
 * host tests, this is useful for replaying a recorded RGB, UYVY, or JPEG frame
 * through the same source/codec pipeline as live hardware.
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
 * @struct ra8_camera_source_memory_state_t
 * @brief Caller-owned state for a fixed-frame memory source.
 *
 * @details Treat the field as private after init. The borrowed source bytes
 *          must remain readable until the source is no longer used.
 * @invariant `frame` passes ::ra8_camera_frame_validate after successful init.
 * @since 0.1.0
 */
typedef struct {
  ra8_camera_frame_t frame; /**< Borrowed fixed frame (private). */
} ra8_camera_source_memory_state_t;

/**
 * @brief Bind a repeatable source backed by one immutable frame.
 *
 * @param[out] source Caller-owned source handle to bind.
 * @param[out] state  Caller-owned backend state to populate.
 * @param[in]  frame  Valid immutable frame to replay.
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok           Source bound.
 * @retval k_ra8_err_null_ptr An argument or `frame->data` was NULL.
 * @retval other              Propagated from ::ra8_camera_frame_validate.
 * @pre `source` and `state` out-live every source operation.
 * @pre `frame->data` remains readable for the source lifetime.
 * @post On success captures copy the fixed bytes into caller storage.
 * @post On error `source` and `state` are unchanged.
 * @note Not thread-safe with respect to the same source state.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_camera_source_memory_init(ra8_camera_source_t*              source,
                                                      ra8_camera_source_memory_state_t* state,
                                                      const ra8_camera_frame_t*         frame);

#ifdef __cplusplus
}
#endif
