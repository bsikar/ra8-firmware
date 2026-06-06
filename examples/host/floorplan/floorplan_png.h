/**
 * @file floorplan_png.h
 * @brief Headless PNG export for the floor-plan demo
 *
 * @details
 * Writes an RGB565 framebuffer to a PNG file using CoreGraphics +
 * AppKit, with no window and no window-server connection required. Used
 * by the demo's ``--png <path>`` mode so the rendered output can be
 * inspected on a CI runner or in a headless shell.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * @since 0.1.0
 */

#pragma once

#include <stdint.h>

#include "ra_err.h"

/**
 * @brief Encode an RGB565 framebuffer as a PNG file (top-down).
 *
 * @param[in] path         Destination file path (ASCII).
 * @param[in] rgb565       Base of the RGB565 pixel buffer.
 * @param[in] width_px     Image width in pixels.
 * @param[in] height_px    Image height in pixels.
 * @param[in] stride_bytes Row stride of @p rgb565 in bytes.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok               PNG written.
 * @retval k_ra_err_null_ptr     @p path or @p rgb565 was NULL.
 * @retval k_ra_err_invalid_arg  Zero width / height / stride.
 * @retval k_ra_err_no_mem       Conversion buffer / image allocation failed.
 * @retval k_ra_err_hw_error     Encoding or file write failed.
 *
 * @pre @p rgb565 holds at least ``stride_bytes * height_px`` bytes.
 * @pre @p path is writable by the process.
 * @post On success the PNG at @p path mirrors the framebuffer, top row first.
 * @post @p rgb565 is read only.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t floorplan_write_png(const char* path,
                                           const void* rgb565,
                                           uint16_t    width_px,
                                           uint16_t    height_px,
                                           uint32_t    stride_bytes);
