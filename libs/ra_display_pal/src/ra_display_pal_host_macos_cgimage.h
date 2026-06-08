/**
 * @file ra_display_pal_host_macos_cgimage.h
 * @brief RGB565 framebuffer -> CoreGraphics CGImage for the host backend
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details
 * Shared factory used by both macOS presentation paths -- the live
 * window (``ra_display_pal_host_macos_window.m``) and the headless PNG
 * export (the demo's ``floorplan_png.m``). Both need the same step:
 * convert the caller's RGB565 framebuffer to ARGB8888 and wrap it in a
 * ``CGImageRef``. Keeping that in one place removes the duplicated
 * CoreGraphics bitmap dance.
 *
 * macOS-only; the whole header is guarded by ``__APPLE__`` and is only
 * ever included by the Objective-C shims (never by a globbed ``.c``).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * @since 0.1.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#ifdef __APPLE__

#include <CoreGraphics/CoreGraphics.h>
#include <stdint.h>

#include "ra_err.h"

/**
 * @brief Build a ``CGImageRef`` from an RGB565 framebuffer.
 *
 * @details
 * Converts the source to top-down ARGB8888 (via
 * ``ra_macos_rgb565_to_argb8888``) and wraps it in a CoreGraphics image
 * laid out for ``kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little``.
 * The returned image owns its own copy of the pixels, so the caller's
 * framebuffer may change immediately afterwards.
 *
 * @param[in]  rgb565       Source RGB565 pixel buffer.
 * @param[in]  width_px     Image width in pixels (>= 1).
 * @param[in]  height_px    Image height in pixels (>= 1).
 * @param[in]  stride_bytes Source row stride in bytes (>= width_px * 2).
 * @param[out] out_image    Receives the new image on success; the caller
 *                          owns it and must ``CGImageRelease`` it.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok              Image created; ``*out_image`` set.
 * @retval k_ra_err_null_ptr    ``rgb565`` or ``out_image`` was NULL.
 * @retval k_ra_err_invalid_arg Zero width / height / stride.
 * @retval k_ra_err_no_mem      Conversion buffer or image allocation failed.
 *
 * @pre ``rgb565`` holds at least ``stride_bytes * height_px`` bytes.
 * @pre ``out_image`` is writable.
 * @post On success ``*out_image`` is a retained CGImage owned by the caller.
 * @post On failure ``*out_image`` is untouched and no memory leaks.
 *
 * @note Not thread-safe with concurrent writes to ``rgb565``.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_macos_framebuffer_to_cgimage(const void* rgb565,
                                                       uint16_t    width_px,
                                                       uint16_t    height_px,
                                                       uint32_t    stride_bytes,
                                                       CGImageRef* out_image);

#endif /* defined(__APPLE__) */

#ifdef __cplusplus
}
#endif
