/**
 * @file ra8_camera_codec_jpeg_sw.h
 * @brief RGB888-to-JPEG software codec backend for `ra8_camera`.
 * @ingroup grp_camera
 *
 * @par Tag
 * [Ring 4 / Service] {World: NS}
 *
 * @details Wraps ::ra8_jpeg_sw_encode behind the generic camera codec seam.
 * Input may be RGB888 or packed UYVY422. Each input is nearest-neighbour sampled
 * to configured output geometry in a caller-owned packed-RGB workspace; UYVY is
 * converted with BT.601 limited-range coefficients. Encoded bytes are written
 * to the output buffer supplied on each encode call.
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

#include "ra8_camera.h"
#include "ra8_err.h"

/**
 * @struct ra8_camera_codec_jpeg_sw_cfg_t
 * @brief Configuration and caller-owned RGB workspace for software JPEG.
 *
 * @details `rgb_workspace_capacity` must cover
 *          `output_width * output_height * 3` bytes. For VGA UYVY to QVGA,
 *          configure 320x240 and provide 230400 bytes.
 * @invariant Output dimensions are non-zero and their RGB byte count fits
 *            `uint32_t`.
 * @invariant `quality` is in the inclusive range 1..100.
 * @since 0.1.0
 */
typedef struct {
  uint8_t* rgb_workspace;          /**< Caller-owned packed RGB workspace.  */
  uint32_t rgb_workspace_capacity; /**< Writable workspace bytes.           */
  uint16_t output_width;           /**< Encoded image width.                */
  uint16_t output_height;          /**< Encoded image height.               */
  uint8_t  quality;                /**< JPEG quality passed to the encoder. */
} ra8_camera_codec_jpeg_sw_cfg_t;

/**
 * @struct ra8_camera_codec_jpeg_sw_state_t
 * @brief Caller-owned private state for the software JPEG backend.
 *
 * @details Populated by init from the caller's configuration. The workspace
 *          remains caller-owned and must out-live this state.
 * @invariant `cfg` passed validation during successful init.
 * @since 0.1.0
 */
typedef struct {
  ra8_camera_codec_jpeg_sw_cfg_t cfg; /**< Validated configuration (private). */
} ra8_camera_codec_jpeg_sw_state_t;

/**
 * @brief Bind the software baseline-JPEG encoder backend.
 *
 * @param[out] codec   Caller-owned codec handle to bind.
 * @param[out] state   Caller-owned backend state to populate.
 * @param[in]  cfg     Output geometry, quality, and RGB workspace.
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok              Codec bound.
 * @retval k_ra8_err_null_ptr     An argument or `cfg->rgb_workspace` was NULL.
 * @retval k_ra8_err_invalid_arg  Quality or output geometry was invalid.
 * @retval k_ra8_err_invalid_size RGB workspace was too small.
 * @pre `codec` and `state` out-live every codec operation.
 * @pre No other call mutates them while encoding.
 * @post On success RGB888 and UYVY422 inputs can be encoded as JPEG.
 * @post On error `codec` and `state` are unchanged.
 * @note Independent state objects are thread-safe; `ra8_jpeg_sw_encode` is
 *       re-entrant.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_camera_codec_jpeg_sw_init(ra8_camera_codec_t*                   codec,
                                                      ra8_camera_codec_jpeg_sw_state_t*     state,
                                                      const ra8_camera_codec_jpeg_sw_cfg_t* cfg);

#ifdef __cplusplus
}
#endif
