/**
 * @file examples/ek_ra8d2/hw_validated/hil/camera_capture/cam_image.h
 * @brief SDRAM-backed RGB888 views of one packed VGA UYVY camera frame.
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Defines the stable firmware/SWD contract for four RGB888 renderings of the
 * captured VGA frame. The 0- and 180-degree buffers are 640x480; the 90- and
 * 270-degree clockwise buffers are 480x640. Every buffer contains exactly
 * ::k_cam_rgb_frame_bytes bytes in row-major RGB byte order.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#include <stdint.h>

#include "ra8_err.h"

/** @brief Captured VGA geometry and RGB888 buffer contract. */
typedef enum : uint32_t {
  k_cam_image_width_px   = 640U,    /**< Unrotated and 180-degree width.       */
  k_cam_image_height_px  = 480U,    /**< Unrotated and 180-degree height.      */
  k_cam_uyvy_frame_bytes = 614400U, /**< 640 x 480 x 2-byte packed UYVY input. */
  k_cam_rgb_bytes_per_px = 3U,      /**< RGB888 bytes per output pixel.        */
  k_cam_rgb_frame_bytes  = 921600U, /**< 640 x 480 x 3 bytes.                  */
} cam_image_size_t;

/**
 * @brief Unrotated 640x480 RGB888 image in external SDRAM.
 * @since 0.1.0
 */
extern uint8_t g_cam_rgb_0[k_cam_rgb_frame_bytes];

/**
 * @brief Clockwise-90-degree 480x640 RGB888 image in external SDRAM.
 * @since 0.1.0
 */
extern uint8_t g_cam_rgb_90[k_cam_rgb_frame_bytes];

/**
 * @brief Clockwise-180-degree 640x480 RGB888 image in external SDRAM.
 * @since 0.1.0
 */
extern uint8_t g_cam_rgb_180[k_cam_rgb_frame_bytes];

/**
 * @brief Clockwise-270-degree 480x640 RGB888 image in external SDRAM.
 * @since 0.1.0
 */
extern uint8_t g_cam_rgb_270[k_cam_rgb_frame_bytes];

/**
 * @brief Stable byte count of each exported RGB888 buffer.
 * @since 0.1.0
 */
extern const uint32_t g_cam_rgb_frame_bytes;

/**
 * @brief Convert one packed Cb-Y0-Cr-Y1 VGA frame into all four RGB views.
 * @details Performs fixed-point YCbCr conversion and scatters each pixel into
 *          caller-observable SDRAM buffers for the four clockwise orientations.
 *
 * @param[in] uyvy Source VGA frame in packed UYVY byte order.
 * @param[in] source_bytes Valid bytes at `uyvy`; must equal 614400.
 * @return Error code from validation or cache maintenance.
 * @retval k_ra8_ok Four complete RGB888 buffers are visible to SWD.
 * @retval k_ra8_err_null_ptr `uyvy` was NULL.
 * @retval k_ra8_err_invalid_arg `source_bytes` was not one VGA UYVY frame.
 *
 * @pre External SDRAM was initialized by `ra8_sdramc_init`.
 * @pre `uyvy` contains a completed, cache-coherent CEU capture.
 * @post All four exported buffers contain the same frame at 0/90/180/270.
 * @post Dirty D-cache lines covering the four buffers are written to SDRAM.
 * @note Rotation angles are clockwise. Not thread-safe.
 * @since 0.1.0
 */
ra8_err_t cam_image_generate(const uint8_t* uyvy, uint32_t source_bytes);
