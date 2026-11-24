/**
 * @file inc/zoom_mem.h
 * @brief In-memory 8-bit grayscale source adapter for the zoom viewport.
 *
 * @details
 * Adapts a caller-owned gray8 pixel plane (for example a raster EPUB figure
 * decoded by ::ra8_img_decode_gray8) into a ::zoom_source_t without heap
 * allocation, flash paging, or JOF tiling. Reads slice caller memory directly
 * into the viewport's single-row scratch buffer.
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

#include "ra8_err.h"
#include "zoom.h"

/**
 * @struct zoom_mem_src_t
 * @brief Caller-owned 8-bit grayscale bitmap descriptor bound to a zoom view.
 *
 * @invariant `pixels != NULL`, `width in [1, k_zoom_dim_max]`,
 *            `height in [1, k_zoom_dim_max]`, and `stride >= width`.
 *
 * @see zoom_mem_src_init
 * @see zoom_mem_src_bind
 * @since 0.1.0
 */
typedef struct {
  const uint8_t* pixels; /**< Gray8 pixel plane, top-left at index 0.   */
  uint32_t       width;  /**< Width in source pixels.                   */
  uint32_t       height; /**< Height in source pixels.                  */
  uint32_t       stride; /**< Byte stride between source rows >= width. */
} zoom_mem_src_t;

/**
 * @brief Validate and populate an in-memory gray8 source descriptor.
 *
 * @param[out] out    Descriptor to populate.
 * @param[in]  pixels Caller-owned gray8 pixel buffer (`>= height * stride` bytes).
 * @param[in]  width  Image width in pixels, in [1, k_zoom_dim_max].
 * @param[in]  height Image height in pixels, in [1, k_zoom_dim_max].
 * @param[in]  stride Source row stride in bytes (`0` defaults to @p width).
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok              Descriptor populated cleanly.
 * @retval k_ra8_err_null_ptr    @p out or @p pixels is NULL.
 * @retval k_ra8_err_invalid_arg Zero/oversized extent, or non-zero @p stride < @p width.
 *
 * @pre  @p pixels remains valid for as long as any view reads from @p out.
 * @post On k_ra8_ok, `out->stride >= out->width`.
 *
 * @note Not thread-safe on @p out during init.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t zoom_mem_src_init(zoom_mem_src_t* out,
                                          const uint8_t*  pixels,
                                          uint32_t        width,
                                          uint32_t        height,
                                          uint32_t        stride);

/**
 * @brief Present a ::zoom_mem_src_t as a ::zoom_source_t for ::zoom_view_open.
 *
 * @param[in]  src Initialised in-memory bitmap descriptor (must outlive bound views).
 * @param[out] out Generic zoom source descriptor populated with ::zoom_mem_read.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok              Bound cleanly.
 * @retval k_ra8_err_null_ptr    @p src, `src->pixels`, or @p out is NULL.
 * @retval k_ra8_err_invalid_arg Invalid dimensions or `src->stride < src->width`.
 *
 * @pre  @p src was initialised via ::zoom_mem_src_init (or holds valid fields).
 * @post On k_ra8_ok, `out->ctx == src` and `out->read == zoom_mem_read`.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t zoom_mem_src_bind(const zoom_mem_src_t* src, zoom_source_t* out);

/**
 * @brief ::zoom_read_fn over a ::zoom_mem_src_t pixel plane.
 *
 * @param[in]  ctx        Pointer to a valid ::zoom_mem_src_t.
 * @param[in]  x          Sub-rectangle left column in source pixels.
 * @param[in]  y          Sub-rectangle top row in source pixels.
 * @param[in]  w          Sub-rectangle width in pixels.
 * @param[in]  h          Sub-rectangle height in pixels.
 * @param[out] out        Destination gray8 row buffer (`>= h * out_stride`).
 * @param[in]  out_stride Byte stride between rows in @p out (`>= w` when `w > 0`).
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok               Requested rectangle copied to @p out.
 * @retval k_ra8_err_null_ptr     @p ctx, `src->pixels`, or @p out is NULL.
 * @retval k_ra8_err_invalid_arg  Bad source metadata or `out_stride < w`.
 * @retval k_ra8_err_out_of_range `[x, x + w) x [y, y + h)` exceeds source extent.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t zoom_mem_read(void*    ctx,
                                      uint32_t x,
                                      uint32_t y,
                                      uint32_t w,
                                      uint32_t h,
                                      uint8_t* out,
                                      uint32_t out_stride);

#ifdef __cplusplus
}
#endif
