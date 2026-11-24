/**
 * @file ra8_drw_mve.h
 * @brief Helium MVE pixel-format conversion and clipped DRW band-blit helpers.
 * @ingroup grp_hal_graphics
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Uncompressed DRW texture reads support RGB565, ARGB8888, ARGB4444, ARGB1555,
 * A8, and indexed CLUT formats (HUM Ch 62.3 Table 62.3 p 3711), not 24-bit
 * packed RGB888. This interface provides Cortex-M85 Helium MVE vectorized pixel
 * converters (``rgb888 -> rgb565``, ``gray8 -> rgb565``, ``rgba8888 -> rgb565``)
 * with exact bit-for-bit scalar fallbacks on host builds (`RA8_OFF_TARGET`),
 * plus a cache-coherent clipped band-blit adapter driving ``ra8_drw``.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

#include "ra8_err.h"

/**
 * @enum ra8_drw_px_bpp_t
 * @brief Supported packed source pixel widths for RGB565 staging conversion.
 */
typedef enum : uint8_t {
  k_ra8_drw_px_bpp_gray8    = 1U, /**< 8-bit grayscale luminance.   */
  k_ra8_drw_px_bpp_rgb565   = 2U, /**< Native little-endian RGB565. */
  k_ra8_drw_px_bpp_rgb888   = 3U, /**< Packed 24-bit R, G, B bytes. */
  k_ra8_drw_px_bpp_rgba8888 = 4U, /**< Packed 32-bit R, G, B, X/A.  */
} ra8_drw_px_bpp_t;

/**
 * @struct ra8_drw_mve_rect_conv_t
 * @brief Parameters for 2D strided sub-rectangle conversion into RGB565.
 *
 * @invariant ``src`` and ``dst`` are non-null and span the declared strides.
 * @invariant ``src_bpp`` is one of ``ra8_drw_px_bpp_t``.
 * @invariant ``width_px`` > 0 and ``height_px`` > 0.
 *
 * @code
 * const ra8_drw_mve_rect_conv_t conv = {
 *   .src           = band_rgb888,
 *   .dst           = stage_rgb565,
 *   .src_stride_px = 1024U,
 *   .dst_stride_px = clip_w,
 *   .src_x         = sx,
 *   .src_y         = sy,
 *   .width_px      = clip_w,
 *   .height_px     = clip_h,
 *   .src_bpp       = (uint8_t)k_ra8_drw_px_bpp_rgb888,
 * };
 * (void)ra8_drw_mve_convert_rect_to_rgb565(&conv);
 * @endcode
 *
 * @see ra8_drw_mve_convert_rect_to_rgb565()
 */
typedef struct {
  const uint8_t* src;           /**< Source pixel buffer base.            */
  uint16_t*      dst;           /**< Destination RGB565 buffer base.      */
  uint16_t       src_stride_px; /**< Source row pitch in pixels.          */
  uint16_t       dst_stride_px; /**< Destination row pitch in pixels.     */
  uint16_t       src_x;         /**< Source top-left X coordinate (px).   */
  uint16_t       src_y;         /**< Source top-left Y coordinate (px).   */
  uint16_t       width_px;      /**< Rectangle width to convert (px).     */
  uint16_t       height_px;     /**< Rectangle height to convert (px).    */
  uint8_t        src_bpp;       /**< Source bytes per pixel (1, 2, 3, 4). */
} ra8_drw_mve_rect_conv_t;

/**
 * @struct ra8_drw_mve_band_blit_t
 * @brief Parameters for viewport-clipped MVE convert + DRW textured blit.
 *
 * @details
 * Clips ``[dst_x, dst_x + src_w) x [dst_y, dst_y + src_h)`` against
 * ``[0, view_w) x [0, view_h)``. When ``src_bpp != 2`` (or when a caller-owned
 * staging buffer is supplied), converts visible rows into ``stage_565`` via
 * Helium MVE (splitting into horizontal strips if ``stage_cap_px`` is smaller
 * than the full clipped rectangle), cleans D-cache lines on the texture, and
 * issues ``ra8_drw_set_texture()`` + ``ra8_drw_blit_textured_subrect()`` +
 * ``ra8_drw_wait_idle()``.
 *
 * @invariant ``src_pixels`` is non-null; ``stage_565`` is non-null if
 *            ``src_bpp != k_ra8_drw_px_bpp_rgb565``.
 * @invariant ``view_w > 0`` and ``view_h > 0``.
 *
 * @code
 * const ra8_drw_mve_band_blit_t job = {
 *   .src_pixels     = pixels,
 *   .src_w          = src_w,
 *   .src_h          = src_h,
 *   .src_bpp        = bpp,
 *   .dst_x          = dst_x,
 *   .dst_y          = dst_y,
 *   .view_w         = 1024U,
 *   .view_h         = 600U,
 *   .stage_565      = s_stage_565,
 *   .stage_cap_px   = k_stage_cap_px,
 *   .stage_tex_addr = 0UL,
 *   .src_tex_addr   = 0UL,
 *   .fb_ptr         = s_framebuffer,
 *   .fb_pitch_px    = 1024U,
 *   .poll_budget    = 1000000UL,
 * };
 * (void)ra8_drw_mve_blit_band(&job);
 * @endcode
 *
 * @see ra8_drw_mve_blit_band()
 */
typedef struct {
  const uint8_t* src_pixels;     /**< Source strip/tile base pointer.                         */
  uint16_t       src_w;          /**< Source width in pixels.                                 */
  uint16_t       src_h;          /**< Source height in pixels.                                */
  uint8_t        src_bpp;        /**< Source bytes per pixel (1, 2, 3, 4).                    */
  int32_t        dst_x;          /**< Destination X on viewport (signed).                     */
  int32_t        dst_y;          /**< Destination Y on viewport (signed).                     */
  uint16_t       view_w;         /**< Viewport / surface width (px).                          */
  uint16_t       view_h;         /**< Viewport / surface height (px).                         */
  uint16_t*      stage_565;      /**< Caller-owned RGB565 texture buffer.                     */
  uint32_t       stage_cap_px;   /**< Capacity of @c stage_565 in pixels.                     */
  uint32_t       stage_tex_addr; /**< Explicit TEXORIGIN for @c stage_565, or 0 to cast ptr.  */
  uint32_t       src_tex_addr;   /**< Explicit TEXORIGIN for @c src_pixels, or 0 to cast ptr. */
  uint16_t*      fb_ptr;         /**< Optional FB CPU pointer for D-cache sync (or nullptr).  */
  uint16_t       fb_pitch_px;    /**< Framebuffer pitch in pixels when @c fb_ptr != nullptr.  */
  uint32_t       poll_budget;    /**< DRW idle wait loop budget (0 = 1000000 default).        */
} ra8_drw_mve_band_blit_t;

/**
 * @brief Convert packed 24-bit RGB888 pixels to native little-endian RGB565.
 *
 * @details
 * Packs each pixel as ``((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)``.
 * Uses Cortex-M85 Helium MVE predicated gather loads and vector bit shifts
 * when compiled with ``__ARM_FEATURE_MVE``, and an exact scalar loop on host.
 *
 * @param[in]  src         Source RGB888 byte buffer (``pixel_count * 3`` bytes).
 * @param[out] dst         Destination RGB565 pixel buffer (``pixel_count`` entries).
 * @param[in]  pixel_count Number of pixels to convert (0 is a valid no-op).
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok           All pixels converted.
 * @retval k_ra8_err_null_ptr @p src or @p dst was ``nullptr``.
 *
 * @pre @p src and @p dst are non-null and non-overlapping.
 * @pre @p src spans at least ``pixel_count * 3`` bytes; @p dst spans ``pixel_count`` halfwords.
 * @post Every element ``dst[0..pixel_count-1]`` matches the scalar RGB565 formula.
 * @post @p src is unmodified.
 *
 * @note Reentrant and thread-safe on disjoint buffers.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_drw_mve_rgb888_to_rgb565(const uint8_t* src, uint16_t* dst, uint32_t pixel_count);

/**
 * @brief Convert packed 8-bit grayscale pixels to native little-endian RGB565.
 *
 * @details
 * Replicates gray sample ``y`` across RGB channels as
 * ``((y >> 3) << 11) | ((y >> 2) << 5) | (y >> 3)``.
 *
 * @param[in]  src         Source Gray8 byte buffer (``pixel_count`` bytes).
 * @param[out] dst         Destination RGB565 pixel buffer (``pixel_count`` entries).
 * @param[in]  pixel_count Number of pixels to convert (0 is a valid no-op).
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok           All pixels converted.
 * @retval k_ra8_err_null_ptr @p src or @p dst was ``nullptr``.
 *
 * @pre @p src and @p dst are non-null and non-overlapping.
 * @pre @p src spans ``pixel_count`` bytes; @p dst spans ``pixel_count`` halfwords.
 * @post Every element ``dst[0..pixel_count-1]`` holds the expanded RGB565 gray value.
 * @post @p src is unmodified.
 *
 * @note Reentrant and thread-safe on disjoint buffers.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_drw_mve_gray8_to_rgb565(const uint8_t* src, uint16_t* dst, uint32_t pixel_count);

/**
 * @brief Convert packed 32-bit RGBA8888/RGBX8888 pixels to native RGB565.
 *
 * @details
 * Packs bytes ``[R, G, B, A]`` as ``((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)``,
 * discarding alpha/padding byte ``A``.
 *
 * @param[in]  src         Source RGBA8888 byte buffer (``pixel_count * 4`` bytes).
 * @param[out] dst         Destination RGB565 pixel buffer (``pixel_count`` entries).
 * @param[in]  pixel_count Number of pixels to convert (0 is a valid no-op).
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok           All pixels converted.
 * @retval k_ra8_err_null_ptr @p src or @p dst was ``nullptr``.
 *
 * @pre @p src and @p dst are non-null and non-overlapping.
 * @pre @p src spans ``pixel_count * 4`` bytes; @p dst spans ``pixel_count`` halfwords.
 * @post Every element ``dst[0..pixel_count-1]`` matches the scalar RGB565 formula.
 * @post @p src is unmodified.
 *
 * @note Reentrant and thread-safe on disjoint buffers.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_drw_mve_rgba8888_to_rgb565(const uint8_t* src, uint16_t* dst, uint32_t pixel_count);

/**
 * @brief Convert a strided 2D source sub-rectangle into a packed RGB565 surface.
 *
 * @param[in] conv Conversion descriptor specifying source/dest strides and window.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok              Sub-rectangle converted.
 * @retval k_ra8_err_null_ptr    @p conv, ``conv->src``, or ``conv->dst`` was ``nullptr``.
 * @retval k_ra8_err_invalid_arg Zero dimensions, out-of-bounds stride, or unsupported ``src_bpp``.
 *
 * @pre @p conv and both buffer pointers are non-null.
 * @pre ``conv->src_x + conv->width_px <= conv->src_stride_px`` and ``conv->width_px <= conv->dst_stride_px``.
 * @post Rows ``[0..height_px)`` of ``conv->dst`` contain RGB565 pixels matching scalar conversion.
 * @post Source buffer is unmodified.
 *
 * @note Reentrant and thread-safe on disjoint buffers.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_drw_mve_convert_rect_to_rgb565(const ra8_drw_mve_rect_conv_t* conv);

/**
 * @brief Clip a source band/tile against the viewport and composite via DRW + MVE.
 *
 * @param[in] blit Composite descriptor specifying source, destination, staging, and waits.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok                Blitted (or completely outside viewport).
 * @retval k_ra8_err_null_ptr      @p blit, ``src_pixels``, or needed ``stage_565`` was ``nullptr``.
 * @retval k_ra8_err_invalid_arg   Zero dimensions, unsupported ``src_bpp``, or bad FB pitch.
 * @retval k_ra8_err_invalid_size  ``stage_cap_px`` smaller than one clipped row.
 * @retval k_ra8_err_timeout       ``ra8_drw_wait_idle()`` timed out.
 *
 * @pre ``ra8_drw_init()`` has bound an RGB565 framebuffer covering ``[0..view_w) x [0..view_h)``.
 * @pre When @c blit->src_bpp != 2, @c blit->stage_565 spans at least ``clip_w`` halfwords.
 * @post DRW texture state is cleared via ``ra8_drw_clear_texture()`` before return.
 * @post All clipped visible pixels are written to the bound DRW framebuffer and idle-synced.
 *
 * @note Not thread-safe; drives shared DRW hardware registers.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_drw_mve_blit_band(const ra8_drw_mve_band_blit_t* blit);

/**
 * @brief Report whether this binary was compiled with Cortex-M85 Helium MVE support.
 *
 * @return ``true`` when compiled with ``__ARM_FEATURE_MVE``, ``false`` on scalar host fallback.
 *
 * @pre None.
 * @pre Callable before driver initialization.
 * @post No hardware or driver state is modified.
 * @post Return value is compile-time constant for the active TU target.
 *
 * @note Pure and thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] bool ra8_drw_mve_is_hardware_enabled(void);

#ifdef __cplusplus
}
#endif
