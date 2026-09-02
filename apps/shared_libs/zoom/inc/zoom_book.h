/**
 * @file zoom_book.h
 * @brief Bind a `.rabook` image-pool figure as a zoom source (#478).
 * @ingroup grp_ereader
 *
 * @details
 * The EPUB half of the tap-to-zoom viewer. A reflow page lays a figure out at
 * whatever size the column allows; tapping it should open the *retained* image,
 * not the laid-out thumbnail, and let the reader magnify into it -- which is the
 * whole reason `.rabook` import stopped downscaling (#210-213, #476).
 *
 * This adapter is deliberately thin, because the work is already done:
 * ::book_src_image_rect owns the image-pool addressing contract, serves both
 * retained depths (4 bpp packed and 8 bpp continuous tone) as gray8, and faults a
 * paged book frame-by-frame so a figure larger than RAM still reads. All this
 * file does is present that call as a ::zoom_read_fn and remember the two
 * things the engine needs -- the descriptor and its extent.
 *
 * The TU compiles to nothing when `book_paged.h` is not on the include path,
 * so an app that only magnifies tiled comics does not drag `book` in.
 *
 * @note Not thread-safe; inherits the (non-)reentrancy of the bound book source.
 * @see zoom.h        The viewport engine this feeds.
 * @see zoom_tiles.h  The tiled-atlas source, for comics and manga.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#include <stdint.h>

#include "book.h"
#include "book_paged.h"
#include "ra8_err.h"
#include "zoom.h"

/**
 * @struct zoom_book_src_t
 * @brief One `.rabook` raster figure, ready to be magnified.
 *
 * @details Holds a borrowed pointer to the book source and a *copy* of the image
 *          descriptor. The copy matters: on a paged book the descriptor itself
 *          lives behind the page cache, so re-reading it per row would fault a
 *          frame per row for 24 bytes that never change.
 *
 * @invariant `src` is non-NULL and outlives this binding.
 * @invariant `img.format == k_book_image_gray4` (SVG is rejected at bind).
 *
 * @par Example:
 * @code
 * zoom_book_src_t fig = {};
 * zoom_source_t   src = {};
 * (void)zoom_book_src_init(&fig, &book_src, image_index);
 * (void)zoom_book_src_bind(&fig, &src);
 * @endcode
 *
 * @see zoom_book_src_init
 * @since 0.1.0
 */
typedef struct {
  const book_src_t* src; /**< Borrowed book source (resident or paged).  */
  book_image_t      img; /**< Copy of the image descriptor being zoomed. */
} zoom_book_src_t;

/**
 * @brief Look up one image in a book source and prepare it for magnification.
 *
 * @details Reads the descriptor once, and fails closed on anything the viewer
 *          cannot magnify: a vector (SVG) entry, or a zero extent. A raster is
 *          accepted at either retained depth -- ::book_src_image_rect
 *          normalises both to gray8, which is the only depth the engine reads.
 *
 * @param[out] bs      Binding to populate.
 * @param[in]  src     Book source, already bound resident or paged.
 * @param[in]  img_idx Image index, `< src->hdr.image_count`.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                 @p bs describes a magnifiable raster.
 * @retval k_ra8_err_null_ptr       @p bs or @p src is NULL.
 * @retval k_ra8_err_not_supported  The entry is SVG, not a raster.
 * @retval k_ra8_err_invalid_arg    The entry has a zero width or height.
 * @retval k_ra8_err_*              Propagated from ::book_src_image.
 *
 * @pre  @p src was populated by book_src_resident() / book_src_paged().
 * @pre  @p bs addresses writable storage for one ::zoom_book_src_t.
 * @post On k_ra8_ok `bs->src == src` and `bs->img` is the descriptor copy.
 * @post On any error @p bs is not left partially bound.
 *
 * @note Not thread-safe.
 * @see zoom_book_src_bind
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
zoom_book_src_init(zoom_book_src_t* bs, const book_src_t* src, uint32_t img_idx);

/**
 * @brief Present a bound figure to the zoom engine as a ::zoom_source_t.
 *
 * @param[in]  bs  Binding populated by ::zoom_book_src_init.
 * @param[out] out Source descriptor to fill.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok              @p out is ready to hand to ::zoom_view_open.
 * @retval k_ra8_err_null_ptr    @p bs, @p out, or `bs->src` is NULL.
 * @retval k_ra8_err_invalid_arg The bound extent is unusable (zero or too large).
 *
 * @pre  @p bs was populated by ::zoom_book_src_init.
 * @pre  @p bs outlives every view bound to @p out.
 * @post On k_ra8_ok `out->ctx == bs` and the extent equals the image's.
 * @post @p bs is not modified.
 *
 * @note Not thread-safe.
 * @see zoom_view_open
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t zoom_book_src_bind(zoom_book_src_t* bs, zoom_source_t* out);

/**
 * @brief ::zoom_read_fn over a `.rabook` image pool.
 *
 * @details Bound by ::zoom_book_src_bind; not normally called directly. It
 *          forwards to ::book_src_image_rect, which is where the gray4 nibble
 *          parity, the gray8 passthrough and the bounded paged reads all live.
 *
 * @param[in]  ctx        The ::zoom_book_src_t binding.
 * @param[in]  x          Rectangle left edge, source pixels.
 * @param[in]  y          Rectangle top edge, source pixels.
 * @param[in]  w          Rectangle width, source pixels (`> 0`).
 * @param[in]  h          Rectangle height, source pixels (`> 0`).
 * @param[out] out        gray8 destination of at least `out_stride * h` bytes.
 * @param[in]  out_stride Bytes between successive output rows (`>= w`).
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok            The rectangle was written as gray8.
 * @retval k_ra8_err_null_ptr  @p ctx or @p out is NULL.
 * @retval k_ra8_err_*         Propagated verbatim from ::book_src_image_rect.
 *
 * @pre  @p ctx is a bound ::zoom_book_src_t.
 * @pre  The rectangle lies inside the bound image.
 * @post On k_ra8_ok every output pixel is the gray8 value of its source pixel.
 * @post On failure @p out content is unspecified.
 *
 * @note Not thread-safe.
 * @see book_src_image_rect
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t zoom_book_read(void*    ctx,
                                       uint32_t x,
                                       uint32_t y,
                                       uint32_t w,
                                       uint32_t h,
                                       uint8_t* out,
                                       uint32_t out_stride);
